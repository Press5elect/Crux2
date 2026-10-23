// Crux2 - the virtual file layer's NFS backend, on libnfs.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * An NFS export (versions 3 and 4, AUTH_SYS) mounted like an SMB share: shown at /smb/<name>
 * beside the SMB shares, so the game list, the file browser and the backups find it the same way.
 * libnfs (github.com/sahlberg/libnfs, LGPL-2.1-or-later) is linked into the title; build-deps.sh
 * builds it for the console.
 *
 *     nfs://server[:nfsport]/path[?uid=N&gid=N&version=3|4|4.2&nfsport=N&mountport=N]
 *
 * The server is told a user and a group number (AUTH_SYS) and trusts them: 1000 and 1000 unless the
 * address names others, the first user of most Linux systems and of many NAS. A server that
 * "squashes" them (all_squash, or root_squash for 0) treats the console as its anonymous user,
 * which can read what everyone may read; to write the backups, give the numbers of a user who may
 * write in the folder.
 *
 * Where the export ends and the folder inside it begins is the server's to say. With NFS 3 (the
 * default) the server's MOUNT service lists its exports, and the longest one the path begins with
 * is mounted; the rest of the path is the folder the share shows. A server that lists none (an NFS 4
 * server without the MOUNT service) is asked for the whole path, and libnfs, when no version was
 * named, tries NFS 4 after NFS 3: NFS 4 reaches every folder of every export by its path from the
 * server's root, so there is nothing to split. version=4 (or 4.2) asks for NFS 4 at once.
 *
 * One connection per share, made on first use and remade when it breaks, with libnfs's own
 * reconnection turned off: it would retry for ever under a call that holds the share's mutex. A
 * libnfs context is not safe to share between threads, so a mutex serialises the calls, as the
 * SMB backend does with libsmb2. Before connecting, the NFS port is asked with a short time limit,
 * so a server that is switched off costs a few seconds rather than libnfs's own connection time
 * limits (which do not bound the TCP handshake). A request then has a longer one, for a NAS whose
 * disks are asleep.
 *
 * libnfs binds its source port to a reserved one (below 1024) where the system allows it. Where it
 * does not, a server that only accepts those (Linux's default "secure" exports) needs the export's
 * "insecure" option.
 */
#include "ps5_vfs_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>

namespace ps5vfs {
namespace {

/* The port is asked for this long before any connection is made. */
constexpr int PROBE_MILLISECONDS = 5000;
/* Listing the exports and mounting one. */
constexpr int CONNECT_MILLISECONDS = 8000;
/* Every request once mounted (libnfs takes whole seconds). */
constexpr int REQUEST_MILLISECONDS = 15000;
/* After a failed connection, further attempts are refused for this long (as for SMB shares). */
constexpr double RETRY_AFTER_SECONDS = 15.0;
/* The most one read or write asks for: libnfs shortens it further to what the server allows. */
constexpr uint32_t MOST_PER_REQUEST = 1024 * 1024;

constexpr int DEFAULT_UID = 1000;
constexpr int DEFAULT_GID = 1000;
constexpr int DEFAULT_NFS_PORT = 2049;
constexpr int PORTMAPPER_PORT = 111;

/* libnfs's dialects: NFS_V3 and NFS_V4 are in headers this file does not need whole. */
constexpr int VERSION_ANY = 0; /* 3, then 4 when 3 fails */
constexpr int VERSION_3 = 3;
constexpr int VERSION_4 = 4;
constexpr int VERSION_4_2 = 42;

struct ExportURL
{
  std::string server;  /* the host, without the port */
  int nfs_port = 0;    /* 0: the portmapper's answer (NFS 3), 2049 (NFS 4) */
  int mount_port = 0;  /* 0: the portmapper's answer */
  std::string path;    /* "/volume1/games/ps2": absolute, no trailing slash ("/" for the root) */
  int uid = DEFAULT_UID;
  int gid = DEFAULT_GID;
  int version = VERSION_ANY;
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

/* A whole decimal number from 0 to most, or -1. */
long ParseNumber(const std::string& text, long most)
{
  if (text.empty() || text.size() > 10)
    return -1;
  long value = 0;
  for (const char c : text)
  {
    if (c < '0' || c > '9')
      return -1;
    value = value * 10 + (c - '0');
  }
  return value <= most ? value : -1;
}

bool ParseURL(const char* url, ExportURL* out, std::string* error)
{
  std::string text(url);
  if (text.size() < 6 || strncasecmp(text.c_str(), "nfs://", 6) != 0)
  {
    *error = "the address must start with nfs://";
    return false;
  }
  text.erase(0, 6);

  std::string query;
  const size_t question = text.find('?');
  if (question != std::string::npos)
  {
    query = text.substr(question + 1);
    text.erase(question);
  }

  const size_t first_slash = text.find('/');
  std::string host = text.substr(0, first_slash);
  const std::string path = (first_slash == std::string::npos) ? std::string() : text.substr(first_slash);
  if (host.find('@') != std::string::npos)
  {
    *error = "an NFS address carries no account: the server is told a user number (uid=) and a group number (gid=)";
    return false;
  }
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos)
  {
    const long port = ParseNumber(host.substr(colon + 1), 65535);
    if (port <= 0)
    {
      *error = "the port after the server is not a number from 1 to 65535";
      return false;
    }
    out->nfs_port = static_cast<int>(port);
    host.erase(colon);
  }
  if (host.empty())
  {
    *error = "the address names no server";
    return false;
  }
  out->server = host;

  /* The path, decoded, with its slashes made single and none at the end. */
  const std::string decoded = PercentDecode(path);
  out->path.clear();
  for (const char c : decoded)
  {
    if (c == '/' && !out->path.empty() && out->path.back() == '/')
      continue;
    out->path.push_back(c);
  }
  while (out->path.size() > 1 && out->path.back() == '/')
    out->path.pop_back();
  if (out->path.empty())
    out->path = "/";

  size_t at = 0;
  while (at < query.size())
  {
    size_t end = query.find('&', at);
    if (end == std::string::npos)
      end = query.size();
    const std::string pair = query.substr(at, end - at);
    at = end + 1;
    if (pair.empty())
      continue;
    const size_t equals = pair.find('=');
    std::string key = pair.substr(0, equals);
    for (char& c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string value = (equals == std::string::npos) ? std::string() : PercentDecode(pair.substr(equals + 1));

    if (key == "uid" || key == "gid")
    {
      const long number = ParseNumber(value, 0x7fffffff);
      if (number < 0)
      {
        *error = key + "= is not a number";
        return false;
      }
      (key == "uid" ? out->uid : out->gid) = static_cast<int>(number);
    }
    else if (key == "nfsport" || key == "mountport")
    {
      const long port = ParseNumber(value, 65535);
      if (port <= 0)
      {
        *error = key + "= is not a port from 1 to 65535";
        return false;
      }
      (key == "nfsport" ? out->nfs_port : out->mount_port) = static_cast<int>(port);
    }
    else if (key == "version")
    {
      if (value == "3")
        out->version = VERSION_3;
      else if (value == "4" || value == "4.0")
        out->version = VERSION_4;
#ifdef LIBNFS_FEATURE_NFS4_2
      else if (value == "4.2")
        out->version = VERSION_4_2;
#endif
      else
      {
        *error = "version= is 3, 4 or 4.2";
        return false;
      }
    }
    else
    {
      *error = "\"" + key + "\" is not an option of an NFS address (uid, gid, version, nfsport, mountport)";
      return false;
    }
  }
  return true;
}

/* Whether a failure means the connection is gone, as opposed to the server's answer about the
 * file. libnfs's synchronous calls return the server's NFS status as an error number, and a failed
 * transport as EFAULT (the connection broke), EINTR (the request timed out) or EIO; a stale file
 * handle (the server was restarted, or the file replaced) is mended the same way, by a new mount
 * and the path looked up again. NFS's own I/O error is EIO as well: it costs one new connection. */
bool ConnectionBroken(int result)
{
  switch (-result)
  {
    case ENOENT:
    case EACCES:
    case EPERM:
    case ENOTDIR:
    case EISDIR:
    case EEXIST:
    case EINVAL:
    case ENAMETOOLONG:
    case ENOTEMPTY:
    case EROFS:
    case ENOSPC:
    case EDQUOT:
    case EFBIG:
    case EXDEV:
    case EOPNOTSUPP:
      return false;
    default:
      return true;
  }
}

/* libnfs's text for the last failure, without its line ends. */
std::string Trimmed(const char* text)
{
  std::string out = text ? text : "";
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    out.pop_back();
  return out;
}

/* Whether something accepts connections at host:port within the time limit. */
bool Probe(const std::string& host, int port, std::string* why)
{
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* found = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || !found)
  {
    *why = "the address cannot be looked up (give the server's IP address)";
    errno = EHOSTUNREACH;
    return false;
  }

  bool open = false;
  int failure = 0;
  const int fd = socket(found->ai_family, SOCK_STREAM, 0);
  if (fd >= 0)
  {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
      (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result = connect(fd, found->ai_addr, found->ai_addrlen);
    if (result != 0 && errno == EINPROGRESS)
    {
      struct pollfd ready;
      ready.fd = fd;
      ready.events = POLLOUT;
      ready.revents = 0;
      const double deadline = MonotonicSeconds() + PROBE_MILLISECONDS / 1000.0;
      for (;;)
      {
        const int left = static_cast<int>((deadline - MonotonicSeconds()) * 1000.0);
        if (left <= 0)
        {
          errno = ETIMEDOUT;
          break;
        }
        result = poll(&ready, 1, left);
        if (result < 0 && errno == EINTR)
          continue;
        if (result > 0)
        {
          int error = 0;
          socklen_t size = sizeof(error);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)
            open = true;
          else
            errno = error ? error : ECONNREFUSED;
        }
        else if (result == 0)
        {
          errno = ETIMEDOUT;
        }
        break;
      }
    }
    else if (result == 0)
    {
      open = true;
    }
    if (!open)
    {
      failure = errno;
      *why = std::string("nothing answers on port ") + service + " (" + std::strerror(failure) + ")";
    }
    close(fd);
  }
  else
  {
    failure = errno;
    *why = std::string("no socket: ") + std::strerror(failure);
  }
  freeaddrinfo(found);
  /* (ETIMEDOUT when the time ran out: the host may be off, or the address wrong.) */
  errno = failure;
  return open;
}

class NFSBackend;

class NFSFile final : public RemoteFile
{
public:
  NFSFile(NFSBackend* backend, std::string path, int64_t size)
    : m_backend(backend), m_path(std::move(path)), m_size(size)
  {
  }
  ~NFSFile() override;

  int64_t Size() const override { return m_size; }
  int64_t Read(void* buffer, uint32_t length, int64_t offset) override;

private:
  friend class NFSBackend;

  NFSBackend* m_backend;
  std::string m_path; /* relative to the mount, as the layer names it */
  int64_t m_size;
  struct nfsfh* m_handle = nullptr;
  uint64_t m_generation = 0;
};

class NFSBackend final : public Backend
{
public:
  explicit NFSBackend(ExportURL url) : m_url(std::move(url)) {}

  ~NFSBackend() override
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

    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      struct nfs_stat_64 st;
      const std::string full = FullPath(path);
      const int result = nfs_stat64(m_context, full.c_str(), &st);
      if (result == 0)
      {
        const size_t slash = path.rfind('/');
        entry->name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        entry->is_directory = S_ISDIR(st.nfs_mode);
        entry->size = static_cast<int64_t>(st.nfs_size);
        entry->mtime = static_cast<int64_t>(st.nfs_mtime);
        return true;
      }
      if (!ConnectionBroken(result))
      {
        errno = -result;
        return false;
      }
      LostConnection("a look at", full, attempt);
    }
    errno = EIO;
    return false;
  }

  bool List(const std::string& path, std::vector<Entry>* entries) override
  {
    std::lock_guard lock(m_mutex);

    if (m_cache.Find(path, entries))
      return true;

    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      const std::string full = FullPath(path);
      struct nfsdir* dir = nullptr;
      const int result = nfs_opendir(m_context, full.c_str(), &dir);
      if (result != 0)
      {
        if (ConnectionBroken(result))
        {
          LostConnection("the listing of", full, attempt);
          continue;
        }
        Log("vfs: %s: cannot list \"%s\": %s", m_url.server.c_str(), full.c_str(), ErrorText().c_str());
        errno = -result;
        return false;
      }

      entries->clear();
      while (struct nfsdirent* const item = nfs_readdir(m_context, dir))
      {
        if (!item->name || std::strcmp(item->name, ".") == 0 || std::strcmp(item->name, "..") == 0)
          continue;
        Entry entry;
        entry.name = item->name;
        entry.is_directory = (item->type == NF3DIR) || S_ISDIR(item->mode);
        entry.size = entry.is_directory ? 0 : static_cast<int64_t>(item->size);
        entry.mtime = static_cast<int64_t>(item->mtime.tv_sec);
        entries->push_back(std::move(entry));
      }
      nfs_closedir(m_context, dir);

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

    auto file = std::make_unique<NFSFile>(this, path, entry.size);
    std::lock_guard lock(m_mutex);
    if (!OpenHandle(file.get()))
      return nullptr;
    m_files.push_back(file.get());
    return file;
  }

  bool MakeFolder(const std::string& path) override
  {
    std::lock_guard lock(m_mutex);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      const std::string full = FullPath(path);
      const int result = nfs_mkdir(m_context, full.c_str());
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
      LostConnection("making", full, attempt);
    }
    errno = EIO;
    return false;
  }

  bool StoreFrom(const std::string& path, uint64_t size, const Source& source) override
  {
    std::lock_guard lock(m_mutex);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;

      const std::string full = FullPath(path);
      const std::string part = full + ".part";
      struct nfsfh* handle = nullptr;
      int result = nfs_open2(m_context, part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644, &handle);
      if (result != 0)
      {
        if (ConnectionBroken(result))
        {
          LostConnection("the writing of", part, attempt);
          continue;
        }
        Log("vfs: %s: cannot write \"%s\": %s", m_url.server.c_str(), part.c_str(), ErrorText().c_str());
        errno = -result;
        return false;
      }

      const uint32_t most = std::clamp<uint32_t>(static_cast<uint32_t>(nfs_get_writemax(m_context)), 4096,
                                                 MOST_PER_REQUEST);
      std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(size, 1), most)));
      uint64_t written = 0;
      result = 0;
      while (written < size)
      {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size - written, most));
        if (source(buffer.data(), chunk, written) != static_cast<int64_t>(chunk))
        {
          /* The source stopped: what was written goes. */
          const int why = errno ? errno : ECANCELED;
          nfs_close(m_context, handle);
          (void)nfs_unlink(m_context, part.c_str());
          m_cache.Clear();
          errno = why;
          return false;
        }
        uint32_t done = 0;
        while (done < chunk)
        {
          result = nfs_pwrite(m_context, handle, buffer.data() + done, chunk - done, written + done);
          if (result <= 0)
          {
            if (result == 0)
              result = -EIO;
            break;
          }
          done += static_cast<uint32_t>(result);
          result = 0;
        }
        written += done;
        if (done < chunk)
          break;
      }

      /* What the server holds only in its memory (an UNSTABLE write) is made to reach its disk
       * before the file takes the place of the old one. */
      if (written == size)
        result = nfs_fsync(m_context, handle);
      const int closed = nfs_close(m_context, handle);
      if (result == 0)
        result = closed;
      if (result == 0)
      {
        /* An NFS rename takes the place of a file that is there, in one step. */
        result = nfs_rename(m_context, part.c_str(), full.c_str());
        m_cache.Clear();
        if (result == 0)
          return true;
      }

      if (ConnectionBroken(result))
      {
        LostConnection("the writing of", part, attempt);
        continue;
      }
      Log("vfs: %s: writing \"%s\" failed after %llu of %llu bytes: %s", m_url.server.c_str(), part.c_str(),
          static_cast<unsigned long long>(written), static_cast<unsigned long long>(size), ErrorText().c_str());
      (void)nfs_unlink(m_context, part.c_str());
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
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      const std::string full = FullPath(path);
      const int result = folder ? nfs_rmdir(m_context, full.c_str()) : nfs_unlink(m_context, full.c_str());
      if (result == 0 || result == -ENOENT)
      {
        m_cache.Clear();
        return true;
      }
      if (!ConnectionBroken(result))
      {
        errno = -result;
        return false;
      }
      LostConnection("removing", full, attempt);
    }
    errno = EIO;
    return false;
  }

  std::string Describe() const override
  {
    std::string text = "nfs://" + m_url.server;
    if (m_url.nfs_port)
      text += ":" + std::to_string(m_url.nfs_port);
    text += m_url.path;
    switch (m_url.version)
    {
      case VERSION_3:
        text += " (NFS 3";
        break;
      case VERSION_4:
        text += " (NFS 4";
        break;
      case VERSION_4_2:
        text += " (NFS 4.2";
        break;
      default:
        text += " (NFS 3, or 4";
        break;
    }
    text += ", uid " + std::to_string(m_url.uid) + ", gid " + std::to_string(m_url.gid) + ")";
    std::lock_guard lock(m_state_mutex);
    if (!m_state.empty())
      text += ", " + m_state;
    return text;
  }

  int64_t ReadFile(NFSFile* file, void* buffer, uint32_t length, int64_t offset)
  {
    std::lock_guard lock(m_mutex);
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!OpenHandle(file))
        return -1;

      const uint32_t chunk = std::min<uint32_t>(length, m_max_read);
      const int result = nfs_pread(m_context, file->m_handle, buffer, chunk, static_cast<uint64_t>(offset));
      if (result >= 0)
        return result;

      if (!ConnectionBroken(result))
      {
        Log("vfs: %s: read of \"%s\" at %lld failed: %s", m_url.server.c_str(), file->m_path.c_str(),
            static_cast<long long>(offset), ErrorText().c_str());
        errno = -result;
        return -1;
      }
      LostConnection("a read of", file->m_path, attempt);
    }
    errno = EIO;
    return -1;
  }

  void CloseFile(NFSFile* file)
  {
    std::lock_guard lock(m_mutex);
    if (file->m_handle && m_context && file->m_generation == m_generation)
      nfs_close(m_context, file->m_handle);
    file->m_handle = nullptr;
    m_files.erase(std::remove(m_files.begin(), m_files.end(), file), m_files.end());
  }

private:
  /* The path on the server, inside the mounted export, of a path relative to the share. With
   * m_mutex held and connected (the folder inside the export is known once mounted). */
  std::string FullPath(const std::string& path) const
  {
    std::string full = "/" + m_folder;
    if (!path.empty())
    {
      if (full.size() > 1)
        full += "/";
      full += path;
    }
    return full;
  }

  std::string ErrorText() const { return m_context ? Trimmed(nfs_get_error(m_context)) : "not connected"; }

  void SetState(std::string state)
  {
    std::lock_guard lock(m_state_mutex);
    m_state = std::move(state);
  }

  /* With m_mutex held: a request found the connection gone. It is dropped, and the caller tries
   * once more over a new one. */
  void LostConnection(const char* what, const std::string& path, int attempt)
  {
    Log("vfs: %s: connection lost in %s \"%s\" (%s)%s", m_url.server.c_str(), what, path.c_str(),
        ErrorText().c_str(), attempt == 0 ? ", connecting again" : "");
    Disconnect();
  }

  /* The server's exports, as its MOUNT service lists them (NFS 3). False when it does not answer
   * within the time limit; the list may then be empty. */
  bool ListExports(std::vector<std::string>* list, std::string* why)
  {
    struct nfs_context* const lister = nfs_init_context();
    if (!lister)
    {
      *why = "out of memory";
      return false;
    }
    if (m_url.mount_port)
      nfs_set_mountport(lister, m_url.mount_port);
    struct rpc_context* const rpc = nfs_get_rpc_context(lister);
    rpc_set_timeout(rpc, CONNECT_MILLISECONDS);

    struct Answer
    {
      bool finished = false;
      int status = 0;
      std::vector<std::string>* list;
    } answer;
    answer.list = list;
    const rpc_cb done = [](struct rpc_context*, int status, void* data, void* private_data) {
      Answer* const answer = static_cast<Answer*>(private_data);
      answer->finished = true;
      answer->status = status;
      if (status == 0 && data)
      {
        for (exports node = *static_cast<exports*>(data); node; node = node->ex_next)
        {
          if (node->ex_dir)
            answer->list->emplace_back(node->ex_dir);
        }
      }
    };

    bool answered = false;
    if (mount_getexports_async(rpc, m_url.server.c_str(), done, &answer) == 0)
    {
      const double deadline = MonotonicSeconds() + CONNECT_MILLISECONDS / 1000.0;
      while (!answer.finished && MonotonicSeconds() < deadline)
      {
        struct pollfd ready;
        ready.fd = rpc_get_fd(rpc);
        ready.events = static_cast<short>(rpc_which_events(rpc));
        ready.revents = 0;
        const int result = poll(&ready, 1, 100);
        if (result < 0 && errno == EINTR)
          continue;
        if (rpc_service(rpc, result < 0 ? -1 : ready.revents) < 0)
          break;
      }
      answered = answer.finished && answer.status == 0;
      if (!answered)
        *why = answer.finished ? Trimmed(rpc_get_error(rpc)) : "no answer in time";
    }
    else
    {
      *why = Trimmed(rpc_get_error(rpc));
    }
    /* (A request still waiting is called back with its cancellation here, while `answer` lives.) */
    nfs_destroy_context(lister);
    return answered;
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

    /* The NFS port the address names, else NFS's own; where nothing answers there and NFS 3 may be
     * spoken, the portmapper, which may name another (a server that is switched off is not asked
     * twice: its first time limit ran out). */
    const std::string name = "nfs://" + m_url.server + m_url.path;
    std::string why;
    const int port = m_url.nfs_port ? m_url.nfs_port : DEFAULT_NFS_PORT;
    if (!Probe(m_url.server, port, &why))
    {
      const bool portmapper = !m_url.nfs_port && errno != ETIMEDOUT &&
                              (m_url.version == VERSION_ANY || m_url.version == VERSION_3);
      std::string portmapper_why;
      if (!portmapper || !Probe(m_url.server, PORTMAPPER_PORT, &portmapper_why))
        return ConnectFailed(name, portmapper ? why + "; " + portmapper_why : why);
    }

    /* The export: the longest one the path begins with (NFS 3). */
    std::string export_path;
    if (m_url.version == VERSION_ANY || m_url.version == VERSION_3)
    {
      std::vector<std::string> listed;
      std::string list_why;
      if (ListExports(&listed, &list_why))
      {
        for (std::string candidate : listed)
        {
          while (candidate.size() > 1 && candidate.back() == '/')
            candidate.pop_back();
          const bool contains =
            candidate == "/" || m_url.path == candidate ||
            (m_url.path.compare(0, candidate.size(), candidate) == 0 && m_url.path[candidate.size()] == '/');
          if (contains && candidate.size() > export_path.size())
            export_path = candidate;
        }
        if (export_path.empty())
          Log("vfs: %s lists %zu exports, none of them holding %s: asking for the path itself", m_url.server.c_str(),
              listed.size(), m_url.path.c_str());
      }
      else
      {
        Log("vfs: %s lists no exports (%s): asking for the path itself", m_url.server.c_str(), list_why.c_str());
      }
    }
    if (export_path.empty())
      export_path = m_url.path;

    struct nfs_context* const context = nfs_init_context();
    if (!context)
    {
      errno = ENOMEM;
      return false;
    }
    nfs_set_timeout(context, CONNECT_MILLISECONDS);
    nfs_set_autoreconnect(context, 0);
    nfs_set_dircache(context, 0); /* the layer keeps its own listings, for a short time */
    nfs_set_uid(context, m_url.uid);
    nfs_set_gid(context, m_url.gid);
    if (m_url.nfs_port)
      nfs_set_nfsport(context, m_url.nfs_port);
    if (m_url.mount_port)
      nfs_set_mountport(context, m_url.mount_port);
    if (m_url.version != VERSION_ANY && nfs_set_version(context, m_url.version) != 0)
    {
      nfs_destroy_context(context);
      return ConnectFailed(name, "this libnfs cannot speak the version asked for");
    }

    if (nfs_mount(context, m_url.server.c_str(), export_path.c_str()) != 0)
    {
      why = Trimmed(nfs_get_error(context));
      nfs_destroy_context(context);
      return ConnectFailed(name + " (export " + export_path + ")",
                           why + "; is the console's address allowed by the export, and, where the export is "
                                 "\"secure\", is \"insecure\" set?");
    }

    nfs_set_timeout(context, REQUEST_MILLISECONDS);
    m_context = context;
    m_generation++;
    m_last_failure = 0.0;
    /* The folder inside the export: what the path has beyond it. */
    m_folder = (m_url.path.size() > export_path.size()) ? m_url.path.substr(export_path.size()) : std::string();
    while (!m_folder.empty() && m_folder[0] == '/')
      m_folder.erase(0, 1);
    m_max_read = std::clamp<uint32_t>(static_cast<uint32_t>(nfs_get_readmax(context)), 4096, MOST_PER_REQUEST);
    m_cache.Clear();
    const int version = nfs_get_version(context);
    Log("vfs: connected to nfs://%s%s (NFS %d, export %s, uid %d, gid %d, reads of up to %u bytes)",
        m_url.server.c_str(), m_url.path.c_str(), version, export_path.c_str(), m_url.uid, m_url.gid, m_max_read);
    SetState("connected (NFS " + std::to_string(version) + ", export " + export_path + ")");
    return true;
  }

  bool ConnectFailed(const std::string& name, const std::string& why)
  {
    Log("vfs: connecting to %s failed: %s", name.c_str(), why.c_str());
    SetState("not reachable: " + why);
    m_last_failure = MonotonicSeconds();
    errno = EHOSTUNREACH;
    return false;
  }

  /* With m_mutex held. Handles of the old connection die with it; files reopen theirs. libnfs
   * leaves a handle's memory to nfs_close, not to the context: with NFS 3 that asks nothing of the
   * server for a file only read, so the open files' handles are let go first. (NFS 4 would send a
   * CLOSE over the connection that is gone; those few bytes a file are left behind.) */
  void Disconnect()
  {
    if (!m_context)
      return;
    const bool local_close = (nfs_get_version(m_context) == VERSION_3);
    for (NFSFile* const file : m_files)
    {
      if (file->m_handle && file->m_generation == m_generation && local_close)
        nfs_close(m_context, file->m_handle);
      file->m_handle = nullptr;
    }
    nfs_destroy_context(m_context);
    m_context = nullptr;
    SetState("disconnected");
  }

  /* With m_mutex held. */
  bool OpenHandle(NFSFile* file)
  {
    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Connect())
        return false;
      if (file->m_handle && file->m_generation == m_generation)
        return true;

      const std::string full = FullPath(file->m_path);
      struct nfsfh* handle = nullptr;
      const int result = nfs_open(m_context, full.c_str(), O_RDONLY, &handle);
      file->m_handle = (result == 0) ? handle : nullptr;
      file->m_generation = m_generation;
      if (result == 0)
        return true;

      if (ConnectionBroken(result))
      {
        LostConnection("opening", full, attempt);
        continue;
      }
      Log("vfs: %s: cannot open \"%s\": %s", m_url.server.c_str(), full.c_str(), ErrorText().c_str());
      errno = -result;
      return false;
    }
    errno = EIO;
    return false;
  }

  const ExportURL m_url;
  std::mutex m_mutex;
  struct nfs_context* m_context = nullptr;
  uint64_t m_generation = 0;
  std::string m_folder; /* inside the mounted export, no leading or trailing slash */
  uint32_t m_max_read = 65536;
  double m_last_failure = 0.0;
  ListingCache m_cache;
  std::vector<NFSFile*> m_files; /* open for reading, each with its handle of the connection */
  /* For Describe(), which must not wait for a request in progress. */
  mutable std::mutex m_state_mutex;
  std::string m_state;
};

NFSFile::~NFSFile()
{
  m_backend->CloseFile(this);
}

int64_t NFSFile::Read(void* buffer, uint32_t length, int64_t offset)
{
  return m_backend->ReadFile(this, buffer, length, offset);
}

} // namespace

std::unique_ptr<Backend> CreateNFSBackend(const char* url, std::string* error)
{
  ExportURL parsed;
  if (!ParseURL(url, &parsed, error))
    return nullptr;
  return std::make_unique<NFSBackend>(std::move(parsed));
}

} // namespace ps5vfs

/* The console's build of libnfs calls these in place of libc's (ps5/tools/build-deps.sh):
 *   getservbyport  no system module exports it. libnfs asks it whether a reserved port is a
 *                  well-known service's before it binds its source port there; there is no service
 *                  database to ask, and no port is taken for one.
 *   getuid, getgid a title has no account of its own to give. They are libnfs's default AUTH_SYS
 *                  numbers only, which every context of this layer replaces (nfs_set_uid/gid).
 * The PC's libnfs (ps5/tests) calls libc's, and these go unused. */
extern "C" struct servent* ps5vfs_nfs_getservbyport(int, const char*)
{
  return nullptr;
}

extern "C" uid_t ps5vfs_nfs_getuid(void)
{
  return static_cast<uid_t>(ps5vfs::DEFAULT_UID);
}

extern "C" gid_t ps5vfs_nfs_getgid(void)
{
  return static_cast<gid_t>(ps5vfs::DEFAULT_GID);
}
