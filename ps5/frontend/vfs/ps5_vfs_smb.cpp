// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - the virtual file layer's SMB2/3 backend, on libsmb2.
 *
 * One connection per share, made on first use and remade when it breaks. libsmb2's
 * synchronous calls are not safe to mix on one context, so a mutex serialises them:
 * the game list scan and the disc reader then take turns, which costs nothing
 * while a game runs (only the reader is active).
 */
#include "ps5_vfs_internal.h"

#include <cctype>
#include <smb2/smb2.h>

#include <smb2/libsmb2.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>

namespace ps5vfs {
namespace {

constexpr int TIMEOUT_SECONDS = 8;
/* After a failed connection, further attempts are refused for this long: a share that is down
 * must not hold every file operation of a game list scan for its whole timeout. */
constexpr double RETRY_AFTER_SECONDS = 15.0;

struct ShareURL
{
  std::string domain;
  std::string user;
  std::string password;
  std::string server; /* host[:port] */
  std::string share;
  std::string folder; /* inside the share, no leading or trailing slash */
};

int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string PercentDecode(const std::string& text)
{
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++)
  {
    if (text[i] == '%' && i + 2 < text.size() && HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0)
    {
      out.push_back(static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2])));
      i += 2;
    }
    else
    {
      out.push_back(text[i]);
    }
  }
  return out;
}

bool ParseURL(const char* url, ShareURL* out, std::string* error)
{
  std::string text(url);
  if (text.compare(0, 6, "smb://") != 0)
  {
    *error = "the address must start with smb://";
    return false;
  }
  text.erase(0, 6);

  /* The last '@' before the first '/' ends the credentials, so a password may hold '@' only when
   * written %40. */
  const size_t first_slash = text.find('/');
  const std::string authority = text.substr(0, first_slash);
  std::string path = (first_slash == std::string::npos) ? std::string() : text.substr(first_slash + 1);

  std::string host = authority;
  const size_t at = authority.rfind('@');
  if (at != std::string::npos)
  {
    std::string credentials = authority.substr(0, at);
    host = authority.substr(at + 1);
    const size_t semicolon = credentials.find(';');
    if (semicolon != std::string::npos)
    {
      out->domain = PercentDecode(credentials.substr(0, semicolon));
      credentials.erase(0, semicolon + 1);
    }
    const size_t colon = credentials.find(':');
    out->user = PercentDecode(credentials.substr(0, colon));
    if (colon != std::string::npos)
      out->password = PercentDecode(credentials.substr(colon + 1));
  }

  if (host.empty())
  {
    *error = "the address names no server";
    return false;
  }
  out->server = host;

  while (!path.empty() && path.back() == '/')
    path.pop_back();
  const size_t share_end = path.find('/');
  out->share = PercentDecode(path.substr(0, share_end));
  if (out->share.empty())
  {
    *error = "the address names no share (smb://server/share/folder)";
    return false;
  }
  if (share_end != std::string::npos)
    out->folder = PercentDecode(path.substr(share_end + 1));
  return true;
}

class SMBBackend;

class SMBFile final : public RemoteFile
{
public:
  SMBFile(SMBBackend* backend, std::string path, int64_t size)
    : m_backend(backend), m_path(std::move(path)), m_size(size)
  {
  }
  ~SMBFile() override;

  int64_t Size() const override { return m_size; }
  int64_t Read(void* buffer, uint32_t length, int64_t offset) override;

private:
  friend class SMBBackend;

  SMBBackend* m_backend;
  std::string m_path;
  int64_t m_size;
  struct smb2fh* m_handle = nullptr;
  uint64_t m_generation = 0;
};

class SMBBackend final : public Backend
{
public:
  explicit SMBBackend(ShareURL url) : m_url(std::move(url)) {}

  ~SMBBackend() override
  {
    std::lock_guard lock(m_mutex);
    Disconnect();
  }

  bool Stat(const std::string& path, Entry* entry) override
  {
    std::lock_guard lock(m_mutex);

    bool known_missing = false;
    if (m_cache.FindEntry(path, entry, &known_missing))
      return true;
    if (known_missing)
    {
      errno = ENOENT;
      return false;
    }

    const std::string full = FullPath(path);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      struct smb2_stat_64 st;
      const int result = smb2_stat(m_context, full.c_str(), &st);
      if (result == 0)
      {
        const size_t slash = path.rfind('/');
        entry->name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        entry->is_directory = (st.smb2_type == SMB2_TYPE_DIRECTORY);
        entry->size = static_cast<int64_t>(st.smb2_size);
        entry->mtime = static_cast<int64_t>(st.smb2_mtime);
        return true;
      }
      if (!ConnectionBroken(result))
      {
        errno = -result;
        return false;
      }
      Disconnect();
    }
    errno = EIO;
    return false;
  }

  bool List(const std::string& path, std::vector<Entry>* entries) override
  {
    std::lock_guard lock(m_mutex);

    if (m_cache.Find(path, entries))
      return true;

    const std::string full = FullPath(path);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      struct smb2dir* const dir = smb2_opendir(m_context, full.c_str());
      if (!dir)
      {
        /* libsmb2 gives no code here, only its text: a failure that is not the server's answer
         * about the folder is the connection's (a NAS closes one that was idle for some
         * minutes, and the socket still looks open then), and is tried again over a new one. A
         * second failure is taken as the folder's own. */
        if (attempt == 0 && !ServerAnswered())
        {
          Log("vfs: %s: connection lost (%s), connecting again", m_url.server.c_str(), ErrorText().c_str());
          Disconnect();
          continue;
        }
        Log("vfs: %s: cannot list \"%s\": %s", m_url.server.c_str(), full.c_str(), ErrorText().c_str());
        errno = ENOENT;
        return false;
      }

      entries->clear();
      while (struct smb2dirent* const item = smb2_readdir(m_context, dir))
      {
        if (std::strcmp(item->name, ".") == 0 || std::strcmp(item->name, "..") == 0)
          continue;
        Entry entry;
        entry.name = item->name;
        entry.is_directory = (item->st.smb2_type == SMB2_TYPE_DIRECTORY);
        entry.size = static_cast<int64_t>(item->st.smb2_size);
        entry.mtime = static_cast<int64_t>(item->st.smb2_mtime);
        entries->push_back(std::move(entry));
      }
      smb2_closedir(m_context, dir);

      m_cache.Store(path, *entries);
      return true;
    }
    errno = EIO;
    return false;
  }

  std::unique_ptr<RemoteFile> Open(const std::string& path) override
  {
    Entry entry;
    if (!Stat(path, &entry))
      return nullptr;
    if (entry.is_directory)
    {
      errno = EISDIR;
      return nullptr;
    }

    auto file = std::make_unique<SMBFile>(this, FullPath(path), entry.size);
    std::lock_guard lock(m_mutex);
    if (!OpenHandle(file.get()))
      return nullptr;
    return file;
  }

  bool MakeFolder(const std::string& path) override
  {
    std::lock_guard lock(m_mutex);
    const std::string full = FullPath(path);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      const int result = smb2_mkdir(m_context, full.c_str());
      if (result == 0 || result == -EEXIST)
      {
        if (result == 0)
          m_cache.Clear();
        return true;
      }
      if (!ConnectionBroken(result))
      {
        Log("vfs: %s: cannot make the folder \"%s\": %s", m_url.server.c_str(), full.c_str(), ErrorText().c_str());
        errno = -result;
        return false;
      }
      Disconnect();
    }
    errno = EIO;
    return false;
  }

  bool StoreFrom(const std::string& path, uint64_t size, const Source& source) override
  {
    std::lock_guard lock(m_mutex);
    const std::string full = FullPath(path);
    const std::string part = full + ".part";
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      struct smb2fh* const handle = smb2_open(m_context, part.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!handle)
      {
        if (attempt == 0 && !ServerAnswered())
        {
          Log("vfs: %s: connection lost (%s), connecting again", m_url.server.c_str(), ErrorText().c_str());
          Disconnect();
          continue;
        }
        Log("vfs: %s: cannot write \"%s\": %s", m_url.server.c_str(), part.c_str(), ErrorText().c_str());
        /* The server's refusal of the account is the answer that matters to whoever asked. */
        errno = std::strstr(smb2_get_error(m_context), "ACCESS_DENIED") ? EACCES : EIO;
        return false;
      }

      const uint32_t most = std::clamp<uint32_t>(smb2_get_max_write_size(m_context), 4096, 1024 * 1024);
      std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(size, 1), most)));
      uint64_t written = 0;
      int result = 0;
      while (written < size)
      {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size - written, most));
        if (source(buffer.data(), chunk, written) != static_cast<int64_t>(chunk))
        {
          /* The source stopped: what was written goes. */
          const int why = errno ? errno : ECANCELED;
          smb2_close(m_context, handle);
          (void)smb2_unlink(m_context, part.c_str());
          m_cache.Clear();
          errno = why;
          return false;
        }
        uint32_t done = 0;
        while (done < chunk)
        {
          result = smb2_pwrite(m_context, handle, buffer.data() + done, chunk - done, written + done);
          if (result <= 0)
            break;
          done += static_cast<uint32_t>(result);
        }
        written += done;
        if (done < chunk)
          break;
      }

      if (written == size)
      {
        const int closed = smb2_close(m_context, handle);
        if (closed == 0)
        {
          /* A rename does not take the place of a file that is there: that one goes first. */
          (void)smb2_unlink(m_context, full.c_str());
          const int renamed = smb2_rename(m_context, part.c_str(), full.c_str());
          m_cache.Clear();
          if (renamed == 0)
            return true;
          Log("vfs: %s: cannot name \"%s\": %s", m_url.server.c_str(), full.c_str(), ErrorText().c_str());
          (void)smb2_unlink(m_context, part.c_str());
          errno = ConnectionBroken(renamed) ? EIO : -renamed;
          return false;
        }
        result = closed;
      }
      else
      {
        /* (The handle belongs to a connection that may be gone: closed with it, below.) */
        if (result != -1 && !ConnectionBroken(result == 0 ? -EIO : result))
          smb2_close(m_context, handle);
      }

      const std::string why = ErrorText();
      if (result == -1 || ConnectionBroken(result == 0 ? -EIO : result))
      {
        Log("vfs: %s: connection lost in the writing of \"%s\" (%s)%s", m_url.server.c_str(), part.c_str(), why.c_str(),
            attempt == 0 ? ", connecting again" : "");
        Disconnect();
        continue;
      }
      Log("vfs: %s: writing \"%s\" failed after %llu of %llu bytes: %s", m_url.server.c_str(), part.c_str(),
          static_cast<unsigned long long>(written), static_cast<unsigned long long>(size), why.c_str());
      (void)smb2_unlink(m_context, part.c_str());
      m_cache.Clear();
      errno = -result;
      return false;
    }
    errno = EIO;
    return false;
  }

  bool Remove(const std::string& path, bool folder) override
  {
    std::lock_guard lock(m_mutex);
    const std::string full = FullPath(path);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      const int result = folder ? smb2_rmdir(m_context, full.c_str()) : smb2_unlink(m_context, full.c_str());
      if (result == 0 || result == -ENOENT)
      {
        m_cache.Clear();
        /* A folder is removed by marking it and closing it, and a server keeps one that still
         * has something in it while answering the mark with success: asked whether it is gone. */
        struct smb2_stat_64 st;
        if (folder && result == 0 && smb2_stat(m_context, full.c_str(), &st) == 0)
        {
          errno = ENOTEMPTY;
          return false;
        }
        return true;
      }
      if (!ConnectionBroken(result))
      {
        errno = -result;
        return false;
      }
      Disconnect();
    }
    errno = EIO;
    return false;
  }

  std::string Describe() const override
  {
    std::string text = "smb://" + m_url.server + "/" + m_url.share;
    if (!m_url.folder.empty())
      text += "/" + m_url.folder;
    text += m_url.user.empty() ? " (guest)" : (" (user " + m_url.user + ")");
    return text;
  }

  int64_t ReadFile(SMBFile* file, void* buffer, uint32_t length, int64_t offset)
  {
    std::lock_guard lock(m_mutex);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!OpenHandle(file))
        return -1;

      const uint32_t chunk = std::min<uint32_t>(length, m_max_read);
      const int result =
        smb2_pread(m_context, file->m_handle, static_cast<uint8_t*>(buffer), chunk, static_cast<uint64_t>(offset));
      if (result >= 0)
        return result;

      if (!ConnectionBroken(result))
      {
        Log("vfs: %s: read of \"%s\" at %lld failed: %s", m_url.server.c_str(), file->m_path.c_str(),
            static_cast<long long>(offset), ErrorText().c_str());
        errno = -result;
        return -1;
      }
      Log("vfs: %s: connection lost in a read of \"%s\" (%s)%s", m_url.server.c_str(), file->m_path.c_str(),
          ErrorText().c_str(), attempt == 0 ? ", connecting again" : "");
      Disconnect();
    }
    errno = EIO;
    return -1;
  }

  void CloseFile(SMBFile* file)
  {
    std::lock_guard lock(m_mutex);
    if (file->m_handle && m_context && file->m_generation == m_generation)
      smb2_close(m_context, file->m_handle);
    file->m_handle = nullptr;
  }

private:
  std::string FullPath(const std::string& path) const
  {
    if (m_url.folder.empty())
      return path;
    return path.empty() ? m_url.folder : (m_url.folder + "/" + path);
  }

  /* Whether a failure means the connection is gone, as opposed to the file's own answer.
   * libsmb2's blocking calls return -1 when the connection failed under them (a closed socket, a
   * timeout), and the server's answers as other error numbers ("access denied" is EACCES): -1 is
   * therefore not taken for EPERM, which is the number it happens to be. (The first console runs
   * took it for the file's answer, and after a NAS had closed an idle connection every listing,
   * read and open on the share failed until the title was started again.) */
  static bool ConnectionBroken(int result)
  {
    if (result == -1)
      return true;

    switch (-result)
    {
      case ENOENT:
      case EACCES:
      case ENOTDIR:
      case EISDIR:
      case EEXIST:
      case EINVAL:
      case ENAMETOOLONG:
        return false;
      default:
        return true;
    }
  }

  /* libsmb2's text for the last failure, without its line ends. */
  std::string ErrorText() const
  {
    std::string text = m_context ? smb2_get_error(m_context) : "not connected";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    return text;
  }

  /* For the calls that give no code (opening a folder or a file): whether the last failure is
   * the server's answer, which libsmb2's text names by its status ("STATUS_OBJECT_NAME_NOT_FOUND"),
   * as opposed to a failure of the connection, which has no status to name. */
  bool ServerAnswered() const { return m_context && std::strstr(smb2_get_error(m_context), "STATUS_") != nullptr; }

  /* No account, or the "guest" that network.cfg gives when no user is set (with no password): the
   * share is asked for as a guest. (The first console runs took the written-out "guest" for an
   * account, never tried SMB 2.1, and a NAS answered "access denied".) */
  bool IsGuestAccount() const
  {
    if (m_url.user.empty())
      return true;
    if (!m_url.password.empty() || m_url.user.size() != 5)
      return false;
    static const char name[] = "guest";
    for (size_t i = 0; i < 5; i++)
    {
      if (std::tolower(static_cast<unsigned char>(m_url.user[i])) != name[i])
        return false;
    }
    return true;
  }

  /* With m_mutex held. */
  bool Connect()
  {
    if (m_context)
      return true;

    const double now = MonotonicSeconds();
    if (m_last_failure > 0.0 && (now - m_last_failure) < RETRY_AFTER_SECONDS)
    {
      errno = EHOSTUNREACH;
      return false;
    }

    /* A share that asks for no account is opened as "guest" with no password. Where the server
     * has no such account and maps the session to its guest, the session has no key to sign
     * with, and SMB 3 wants the tree connect signed (Samba then answers "access denied"): the
     * second attempt speaks SMB 2.1, which does not. */
    const bool guest = IsGuestAccount();
    const std::string user = guest ? std::string("guest") : m_url.user;
    std::string first_error;
    for (int attempt = 0; attempt < (guest ? 2 : 1); attempt++)
    {
      struct smb2_context* const context = smb2_init_context();
      if (!context)
      {
        errno = ENOMEM;
        return false;
      }

      smb2_set_timeout(context, TIMEOUT_SECONDS);
      smb2_set_security_mode(context, guest ? 0 : SMB2_NEGOTIATE_SIGNING_ENABLED);
      if (attempt == 1)
        smb2_set_version(context, SMB2_VERSION_0210);
      smb2_set_user(context, user.c_str());
      smb2_set_password(context, m_url.password.c_str());
      if (!m_url.domain.empty())
        smb2_set_domain(context, m_url.domain.c_str());

      const int result = smb2_connect_share(context, m_url.server.c_str(), m_url.share.c_str(), user.c_str());
      if (result == 0)
      {
        m_context = context;
        m_generation++;
        m_last_failure = 0.0;
        m_max_read = std::clamp<uint32_t>(smb2_get_max_read_size(context), 4096, 1024 * 1024);
        m_cache.Clear();
        Log("vfs: connected to smb://%s/%s as %s%s (reads of up to %u bytes)", m_url.server.c_str(),
            m_url.share.c_str(), user.c_str(), attempt == 1 ? ", SMB 2.1" : "", m_max_read);
        return true;
      }

      if (attempt == 0)
        first_error = smb2_get_error(context);
      else
        first_error += std::string("; with SMB 2.1: ") + smb2_get_error(context);
      const bool refused = (result == -EACCES || result == -EPERM);
      smb2_destroy_context(context);
      if (!refused)
        break;
    }

    Log("vfs: connecting to smb://%s/%s as %s failed: %s", m_url.server.c_str(), m_url.share.c_str(), user.c_str(),
        first_error.c_str());
    m_last_failure = MonotonicSeconds();
    errno = EHOSTUNREACH;
    return false;
  }

  /* With m_mutex held. Handles of the old connection die with it; files reopen theirs. */
  void Disconnect()
  {
    if (!m_context)
      return;
    smb2_destroy_context(m_context);
    m_context = nullptr;
  }

  /* With m_mutex held. */
  bool OpenHandle(SMBFile* file)
  {
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      if (file->m_handle && file->m_generation == m_generation)
        return true;

      file->m_handle = smb2_open(m_context, file->m_path.c_str(), O_RDONLY);
      file->m_generation = m_generation;
      if (file->m_handle)
        return true;

      /* As for a folder: a failure that is not the server's answer is tried over a new connection. */
      if (attempt == 0 && !ServerAnswered())
      {
        Log("vfs: %s: connection lost (%s), connecting again", m_url.server.c_str(), ErrorText().c_str());
        Disconnect();
        continue;
      }
      Log("vfs: %s: cannot open \"%s\": %s", m_url.server.c_str(), file->m_path.c_str(), ErrorText().c_str());
      break;
    }
    errno = ENOENT;
    return false;
  }

  const ShareURL m_url;
  std::mutex m_mutex;
  struct smb2_context* m_context = nullptr;
  uint64_t m_generation = 0;
  uint32_t m_max_read = 65536;
  double m_last_failure = 0.0;
  ListingCache m_cache;
};

SMBFile::~SMBFile()
{
  m_backend->CloseFile(this);
}

int64_t SMBFile::Read(void* buffer, uint32_t length, int64_t offset)
{
  return m_backend->ReadFile(this, buffer, length, offset);
}

} // namespace

std::unique_ptr<Backend> CreateSMBBackend(const char* url, std::string* error)
{
  ShareURL parsed;
  if (!ParseURL(url, &parsed, error))
    return nullptr;
  return std::make_unique<SMBBackend>(std::move(parsed));
}

} // namespace ps5vfs
