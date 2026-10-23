// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - the virtual file layer's FTP backend: a tree served by an
 * FTP server on the loopback.
 *
 * A title's sandbox hides /mnt (USB drives, extended storage). The console's
 * FTP server is another process, outside that sandbox, and a title may open
 * sockets to 127.0.0.1, so the server reads those files for it. Listings come
 * from LIST; a file is read as one RETR stream, restarted with REST when a read
 * lands somewhere the stream is not.
 */
#include "ps5_vfs_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace ps5vfs {
namespace {

constexpr int REPLY_TIMEOUT_SECONDS = 10;
/* A read this far ahead of the stream is reached by reading on, not by restarting the transfer. */
constexpr int64_t SKIP_LIMIT = 512 * 1024;

int ConnectLoopback(unsigned port)
{
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<unsigned short>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  const struct timeval timeout = {REPLY_TIMEOUT_SECONDS, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  if (connect(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) != 0)
  {
    close(fd);
    return -1;
  }
  return fd;
}

class FTPConnection
{
public:
  ~FTPConnection() { Close(); }

  bool IsOpen() const { return m_control >= 0; }
  const std::string& Reply() const { return m_reply; }

  bool Open(unsigned port)
  {
    Close();
    m_control = ConnectLoopback(port);
    if (m_control < 0)
      return false;

    int code = ReadReply();
    if (code == 220)
      code = Command("USER anonymous");
    if (code == 331)
      code = Command("PASS anonymous");
    if (code == 230 || code == 202)
      code = Command("TYPE I");
    if (code != 200)
    {
      Close();
      return false;
    }
    return true;
  }

  void Close()
  {
    if (m_control >= 0)
      close(m_control);
    m_control = -1;
  }

  /* Sends one command and returns the reply's code, or -1 when the connection failed (it is then
   * closed). */
  int Command(const char* format, ...) __attribute__((format(printf, 2, 3)))
  {
    if (m_control < 0)
      return -1;

    char command[1200];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(command, sizeof(command) - 2, format, args);
    va_end(args);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(command) - 2)
      return -1;
    std::memcpy(command + length, "\r\n", 2);

    size_t sent = 0;
    while (sent < static_cast<size_t>(length) + 2)
    {
      const ssize_t result = send(m_control, command + sent, static_cast<size_t>(length) + 2 - sent, 0);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0)
      {
        Close();
        return -1;
      }
      sent += static_cast<size_t>(result);
    }
    return ReadReply();
  }

  /* One reply, of one line or several ("123-" up to "123 "). */
  int ReadReply()
  {
    std::string line;
    if (!ReadLine(&line) || line.size() < 3)
    {
      Close();
      return -1;
    }
    const int code = std::atoi(line.c_str());
    if (line.size() > 3 && line[3] == '-')
    {
      const std::string end = line.substr(0, 3) + " ";
      do
      {
        if (!ReadLine(&line))
        {
          Close();
          return -1;
        }
      } while (line.compare(0, 4, end) != 0);
    }
    m_reply = line;
    return code;
  }

  /* A passive data connection with `command path` started on it, or -1. */
  int OpenData(const char* command, const std::string& path)
  {
    if (Command("PASV") != 227)
      return -1;

    const char* const numbers = std::strchr(m_reply.c_str(), '(');
    unsigned h[4], p[2];
    if (!numbers || std::sscanf(numbers, "(%u,%u,%u,%u,%u,%u)", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6)
      return -1;

    /* The server names any of the console's addresses; the loopback reaches it as the control
     * connection did. */
    const int data = ConnectLoopback(p[0] * 256 + p[1]);
    if (data < 0)
      return -1;

    const int code = Command("%s %s", command, path.c_str());
    if (code != 150 && code != 125)
    {
      close(data);
      return -1;
    }
    return data;
  }

private:
  bool ReadLine(std::string* line)
  {
    line->clear();
    for (;;)
    {
      char c;
      const ssize_t got = recv(m_control, &c, 1, 0);
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0)
        return false;
      if (c == '\n')
        return true;
      if (c != '\r' && line->size() < 1024)
        line->push_back(c);
    }
  }

  int m_control = -1;
  std::string m_reply;
};

/* Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm): the console's libc has no
 * timegm a title can rely on. */
int64_t DaysFromCivil(int year, unsigned month, unsigned day)
{
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/* "Jan 23 11:44" or "Jan 23  2025": seconds since the epoch, 0 when it does not parse. The value
 * only has to be the same every time the same listing is read: the game list compares it. */
int64_t ParseListTime(const char* month_text, const char* day_text, const char* time_text)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* const found = (std::strlen(month_text) == 3) ? std::strstr(months, month_text) : nullptr;
  if (!found || ((found - months) % 3) != 0)
    return 0;
  const unsigned month = static_cast<unsigned>((found - months) / 3) + 1;
  const unsigned day = static_cast<unsigned>(std::atoi(day_text));
  if (day < 1 || day > 31)
    return 0;

  const time_t now = time(nullptr);
  const int64_t today = static_cast<int64_t>(now) / 86400;
  if (const char* const colon = std::strchr(time_text, ':'))
  {
    /* No year: the most recent such date. */
    const int hour = std::atoi(time_text);
    const int minute = std::atoi(colon + 1);
    int year = 1970 + static_cast<int>(today / 365.2425);
    int64_t days = DaysFromCivil(year, month, day);
    if (days > today + 1)
      days = DaysFromCivil(--year, month, day);
    return days * 86400 + hour * 3600 + minute * 60;
  }

  const int year = std::atoi(time_text);
  if (year < 1970 || year > 2200)
    return 0;
  return DaysFromCivil(year, month, day) * 86400;
}

/* "drwxrwxrwx 1 0 0 65536 Jan 23 11:44 name". */
bool ParseListLine(const std::string& line, Entry* entry)
{
  if (line.empty() || (line[0] != '-' && line[0] != 'd' && line[0] != 'l'))
    return false;

  std::string fields[8];
  size_t at = 0;
  for (int field = 0; field < 8; field++)
  {
    while (at < line.size() && line[at] == ' ')
      at++;
    const size_t start = at;
    while (at < line.size() && line[at] != ' ')
      at++;
    if (at == start)
      return false;
    fields[field] = line.substr(start, at - start);
  }
  while (at < line.size() && line[at] == ' ')
    at++;
  if (at >= line.size())
    return false;

  std::string name = line.substr(at);
  if (line[0] == 'l')
  {
    const size_t arrow = name.find(" -> ");
    if (arrow != std::string::npos)
      name.erase(arrow);
  }
  if (name == "." || name == "..")
    return false;

  entry->name = std::move(name);
  /* A link is offered as a folder: on the console these are mount points more often than files. */
  entry->is_directory = (line[0] != '-');
  entry->size = entry->is_directory ? 0 : std::strtoll(fields[4].c_str(), nullptr, 10);
  entry->mtime = ParseListTime(fields[5].c_str(), fields[6].c_str(), fields[7].c_str());
  return true;
}

class FTPBackend;

class FTPFile final : public RemoteFile
{
public:
  FTPFile(unsigned port, std::string path, int64_t size) : m_port(port), m_path(std::move(path)), m_size(size) {}

  ~FTPFile() override
  {
    std::lock_guard lock(m_mutex);
    StopTransfer();
  }

  int64_t Size() const override { return m_size; }

  int64_t Read(void* buffer, uint32_t length, int64_t offset) override
  {
    std::lock_guard lock(m_mutex);
    if (offset >= m_size || length == 0)
      return 0;

    for (int attempt = 0; attempt < 2; attempt++)
    {
      if (!Position(offset))
      {
        StopTransfer();
        m_connection.Close();
        continue;
      }

      const ssize_t got = ReceiveSome(buffer, length);
      if (got > 0)
      {
        m_stream_offset += got;
        return got;
      }

      /* The stream ended early or broke: start again from here. */
      StopTransfer();
      m_connection.Close();
    }

    Log("vfs: ftp: reading \"%s\" at %lld failed", m_path.c_str(), static_cast<long long>(offset));
    errno = EIO;
    return -1;
  }

private:
  ssize_t ReceiveSome(void* buffer, size_t length)
  {
    for (;;)
    {
      const ssize_t got = recv(m_data, buffer, length, 0);
      if (got < 0 && errno == EINTR)
        continue;
      return got;
    }
  }

  /* Brings the stream to offset. */
  bool Position(int64_t offset)
  {
    if (m_data >= 0 && offset >= m_stream_offset && (offset - m_stream_offset) <= SKIP_LIMIT)
    {
      char discard[16384];
      while (m_stream_offset < offset)
      {
        const size_t want = static_cast<size_t>(std::min<int64_t>(offset - m_stream_offset, sizeof(discard)));
        const ssize_t got = ReceiveSome(discard, want);
        if (got <= 0)
          return false;
        m_stream_offset += got;
      }
      return true;
    }

    StopTransfer();
    if (!m_connection.IsOpen() && !m_connection.Open(m_port))
      return false;
    if (offset != 0 && m_connection.Command("REST %lld", static_cast<long long>(offset)) != 350)
      return false;
    m_data = m_connection.OpenData("RETR", m_path);
    if (m_data < 0)
      return false;

    const int size = 1 << 20;
    setsockopt(m_data, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    m_stream_offset = offset;
    return true;
  }

  /* Ends the transfer in progress and takes the server's word on it (226, or 426 for one cut
   * short), so the next command's reply is its own. */
  void StopTransfer()
  {
    if (m_data < 0)
      return;
    close(m_data);
    m_data = -1;
    if (m_connection.IsOpen())
    {
      const int code = m_connection.ReadReply();
      if (code != 226 && code != 250 && code != 426 && code != 451)
        m_connection.Close();
    }
  }

  const unsigned m_port;
  const std::string m_path;
  const int64_t m_size;
  std::mutex m_mutex;
  FTPConnection m_connection;
  int m_data = -1;
  int64_t m_stream_offset = 0;
};

class FTPBackend final : public Backend
{
public:
  FTPBackend(unsigned port, std::string root) : m_port(port), m_root(std::move(root))
  {
    while (m_root.size() > 1 && m_root.back() == '/')
      m_root.pop_back();
  }

  bool Probe()
  {
    std::lock_guard lock(m_mutex);
    return Connect();
  }

  bool Stat(const std::string& path, Entry* entry) override
  {
    if (path.empty())
    {
      *entry = Entry{std::string(), true, 0, 0};
      return true;
    }

    const size_t slash = path.rfind('/');
    const std::string directory = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    std::vector<Entry> entries;
    if (!List(directory, &entries))
      return false;
    for (Entry& candidate : entries)
    {
      if (candidate.name == name)
      {
        *entry = std::move(candidate);
        return true;
      }
    }
    errno = ENOENT;
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
        break;

      const int data = m_connection.OpenData("LIST", full);
      if (data < 0)
      {
        if (m_connection.IsOpen())
        {
          /* The server answered: the folder is not there, or is not a folder. */
          errno = ENOENT;
          return false;
        }
        continue;
      }

      entries->clear();
      std::string line;
      char block[8192];
      ssize_t got;
      while ((got = recv(data, block, sizeof(block), 0)) > 0)
      {
        for (ssize_t i = 0; i < got; i++)
        {
          const char c = block[i];
          if (c != '\n')
          {
            if (c != '\r' && line.size() < 2048)
              line.push_back(c);
            continue;
          }
          Entry entry;
          if (ParseListLine(line, &entry))
            entries->push_back(std::move(entry));
          line.clear();
        }
      }
      close(data);

      const int code = m_connection.ReadReply();
      if (code != 226 && code != 250)
      {
        m_connection.Close();
        continue;
      }

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

    /* The listing's size is what the server printed; SIZE is exact. */
    int64_t size = entry.size;
    {
      std::lock_guard lock(m_mutex);
      if (Connect() && m_connection.Command("SIZE %s", FullPath(path).c_str()) == 213 && m_connection.Reply().size() > 4)
        size = std::strtoll(m_connection.Reply().c_str() + 4, nullptr, 10);
    }
    return std::make_unique<FTPFile>(m_port, FullPath(path), size);
  }

  bool MakeFolder(const std::string& path) override
  {
    {
      std::lock_guard lock(m_mutex);
      const std::string full = FullPath(path);
      for (int attempt = 0; attempt < 2; attempt++)
      {
        if (!Connect())
          break;
        const int code = m_connection.Command("MKD %s", full.c_str());
        if (code == 257 || code == 250)
        {
          m_cache.Clear();
          return true;
        }
        if (code > 0)
          break; /* the server's answer: there already, or refused; the listing below says which */
      }
    }
    Entry entry;
    if (Stat(path, &entry) && entry.is_directory)
      return true;
    errno = EACCES;
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
        break;

      const int stream = m_connection.OpenData("STOR", part);
      if (stream < 0)
      {
        if (m_connection.IsOpen())
        {
          Log("vfs: ftp: the server refused to write \"%s\": %s", part.c_str(), m_connection.Reply().c_str());
          errno = EACCES;
          return false;
        }
        continue;
      }

      constexpr uint32_t PIECE = 256 * 1024;
      std::vector<char> buffer(static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(size, 1), PIECE)));
      uint64_t sent = 0;
      bool stopped = false;
      int why = 0;
      while (sent < size && !stopped)
      {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size - sent, PIECE));
        if (source(buffer.data(), chunk, sent) != static_cast<int64_t>(chunk))
        {
          why = errno ? errno : ECANCELED;
          stopped = true;
          break;
        }
        uint32_t done = 0;
        while (done < chunk)
        {
          const ssize_t result = send(stream, buffer.data() + done, chunk - done, 0);
          if (result < 0 && errno == EINTR)
            continue;
          if (result <= 0)
            break;
          done += static_cast<uint32_t>(result);
        }
        sent += done;
        if (done < chunk)
          break;
      }
      close(stream);
      if (stopped)
      {
        /* The source stopped: the server's reply to the cut transfer, then what was written goes. */
        (void)m_connection.ReadReply();
        if (m_connection.IsOpen())
          (void)m_connection.Command("DELE %s", part.c_str());
        m_cache.Clear();
        errno = why;
        return false;
      }

      const int code = m_connection.ReadReply();
      if (sent != size || (code != 226 && code != 250))
      {
        Log("vfs: ftp: writing \"%s\" stopped after %llu of %llu bytes (%s)", part.c_str(), static_cast<unsigned long long>(sent),
            static_cast<unsigned long long>(size), code > 0 ? m_connection.Reply().c_str() : "the connection closed");
        if (m_connection.IsOpen())
          (void)m_connection.Command("DELE %s", part.c_str());
        else
          continue;
        m_cache.Clear();
        errno = EIO;
        return false;
      }

      (void)m_connection.Command("DELE %s", full.c_str());
      const bool named = m_connection.Command("RNFR %s", part.c_str()) == 350 && m_connection.Command("RNTO %s", full.c_str()) == 250;
      m_cache.Clear();
      if (named)
        return true;
      Log("vfs: ftp: cannot name \"%s\": %s", full.c_str(), m_connection.Reply().c_str());
      errno = EACCES;
      return false;
    }
    errno = EIO;
    return false;
  }

  bool Remove(const std::string& path, bool folder) override
  {
    {
      std::lock_guard lock(m_mutex);
      if (!Connect())
      {
        errno = EIO;
        return false;
      }
      const int code = m_connection.Command("%s %s", folder ? "RMD" : "DELE", FullPath(path).c_str());
      m_cache.Clear();
      if (code == 250)
        return true;
    }
    Entry entry;
    if (!Stat(path, &entry))
      return true; /* not there */
    errno = EACCES;
    return false;
  }

  std::string Describe() const override
  {
    return "ftp://127.0.0.1:" + std::to_string(m_port) + m_root + " (the console's FTP server)";
  }

private:
  std::string FullPath(const std::string& path) const
  {
    if (path.empty())
      return m_root;
    return (m_root == "/") ? ("/" + path) : (m_root + "/" + path);
  }

  bool Connect()
  {
    if (m_connection.IsOpen())
      return true;
    const double now = MonotonicSeconds();
    if (m_last_failure > 0.0 && (now - m_last_failure) < 5.0)
      return false;
    if (!m_connection.Open(m_port))
    {
      m_last_failure = now;
      return false;
    }
    m_last_failure = 0.0;
    return true;
  }

  const unsigned m_port;
  std::string m_root;
  std::mutex m_mutex;
  FTPConnection m_connection;
  ListingCache m_cache;
  double m_last_failure = 0.0;
};

} // namespace

std::unique_ptr<Backend> CreateFTPBackend(unsigned port, const char* remote_root, std::string* error)
{
  auto backend = std::make_unique<FTPBackend>(port, remote_root);
  if (!backend->Probe())
  {
    *error = "no FTP server answered";
    return nullptr;
  }
  return backend;
}

} // namespace ps5vfs
