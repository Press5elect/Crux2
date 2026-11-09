// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - the virtual file layer (see ps5_vfs.h): mounts, path
 * resolution, and stdio streams over a backend's positioned reads.
 *
 * A virtual stream is identified by a real FILE (opened on a small file that
 * always exists), so code that reaches into the FILE through a libc macro, or
 * hands it to a libc function this layer does not bind, touches a valid object
 * instead of this layer's own memory.
 */
#include "ps5_vfs.h"
#include "ps5_vfs_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <strings.h>
#include <string>
#include <utility>
#include <vector>

#ifndef PS5VFS_SHADOW_PATH
#ifdef __PROSPERO__
/* Shipped with every title, and readable by it. */
#define PS5VFS_SHADOW_PATH "/app0/sce_sys/param.json"
#else
#define PS5VFS_SHADOW_PATH "/dev/null"
#endif
#endif

namespace ps5vfs {
namespace {

void (*s_log)(const char* line) = nullptr;

std::atomic<uint64_t> s_bytes_read{0};
std::atomic<uint64_t> s_backend_reads{0};

struct MountPoint
{
  std::string point; /* "/smb/nas": absolute, normalised, no trailing slash */
  std::unique_ptr<Backend> backend;
};

/* Mounts are added at start-up and never removed, so a Backend pointer taken under the lock stays
 * valid for the process's lifetime. */
std::mutex s_mounts_mutex;
std::vector<MountPoint> s_mounts;
std::atomic<unsigned> s_mount_count{0};
std::atomic<const char*> s_shadow_path{PS5VFS_SHADOW_PATH};

/* "/a//b/./c/../d/" -> "/a/b/d". Relative paths are not virtual and come back empty. */
std::string Normalise(const char* path)
{
  std::string out;
  if (!path || path[0] != '/')
    return out;

  std::vector<std::pair<size_t, size_t>> parts;
  const size_t length = std::strlen(path);
  size_t at = 0;
  while (at < length)
  {
    while (at < length && path[at] == '/')
      at++;
    const size_t start = at;
    while (at < length && path[at] != '/')
      at++;
    const size_t size = at - start;
    if (size == 0 || (size == 1 && path[start] == '.'))
      continue;
    if (size == 2 && path[start] == '.' && path[start + 1] == '.')
    {
      if (!parts.empty())
        parts.pop_back();
      continue;
    }
    parts.emplace_back(start, size);
  }

  for (const auto& [start, size] : parts)
  {
    out.push_back('/');
    out.append(path + start, size);
  }
  if (out.empty())
    out = "/";
  return out;
}

bool IsUnder(const std::string& path, const std::string& parent)
{
  return path.size() > parent.size() && path.compare(0, parent.size(), parent) == 0 && path[parent.size()] == '/';
}

/* The backend serving a normalised path and the path inside it, or nullptr. */
Backend* Resolve(const std::string& path, std::string* relative)
{
  if (s_mount_count.load(std::memory_order_acquire) == 0 || path.empty())
    return nullptr;

  std::lock_guard lock(s_mounts_mutex);
  for (const MountPoint& mount : s_mounts)
  {
    if (path == mount.point)
    {
      relative->clear();
      return mount.backend.get();
    }
    if (IsUnder(path, mount.point))
    {
      relative->assign(path, mount.point.size() + 1, std::string::npos);
      return mount.backend.get();
    }
  }
  return nullptr;
}

/* The mount points below a folder that only exists because mounts do ("/smb"): their next path
 * component each. Empty when the path is no such folder. */
std::vector<std::string> SyntheticChildren(const std::string& path)
{
  std::vector<std::string> children;
  if (s_mount_count.load(std::memory_order_acquire) == 0 || path.empty() || path == "/")
    return children;

  std::lock_guard lock(s_mounts_mutex);
  for (const MountPoint& mount : s_mounts)
  {
    if (!IsUnder(mount.point, path))
      continue;
    const size_t start = path.size() + 1;
    const size_t end = mount.point.find('/', start);
    std::string name = mount.point.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (std::find(children.begin(), children.end(), name) == children.end())
      children.push_back(std::move(name));
  }
  return children;
}

void FillStat(struct stat* st, const Entry& entry)
{
  std::memset(st, 0, sizeof(*st));
  st->st_mode = entry.is_directory ? (S_IFDIR | 0555) : (S_IFREG | 0444);
  st->st_nlink = 1;
  st->st_size = entry.is_directory ? 0 : static_cast<off_t>(entry.size);
  st->st_mtime = static_cast<time_t>(entry.mtime);
  st->st_ctime = static_cast<time_t>(entry.mtime);
  st->st_atime = static_cast<time_t>(entry.mtime);
}

/* ------------------------------------------------------------------ streams */

/* Reads go through whole blocks kept for reuse: a disc image is read a sector or a CHD hunk at a
 * time, mostly forwards, and a backend read is a network round trip. */
constexpr uint32_t BLOCK_SIZE = 256 * 1024;
constexpr unsigned BLOCK_COUNT = 8;

struct Block
{
  int64_t offset = -1;
  uint32_t valid = 0;
  uint64_t used = 0;
  std::unique_ptr<uint8_t[]> data;
};

struct VFile
{
  FILE* shadow = nullptr;
  std::unique_ptr<RemoteFile> file;
  std::mutex mutex;
  Entry info;
  int64_t position = 0;
  bool error = false;
  int ungot = -1;
  uint64_t tick = 0;
  Block blocks[BLOCK_COUNT];
};

constexpr unsigned MAX_OPEN = 64;
std::atomic<FILE*> s_keys[MAX_OPEN];
VFile* s_values[MAX_OPEN];
std::atomic<unsigned> s_open_count{0};
std::mutex s_open_mutex;

VFile* Find(FILE* fp)
{
  if (s_open_count.load(std::memory_order_acquire) == 0 || !fp)
    return nullptr;
  for (unsigned i = 0; i < MAX_OPEN; i++)
  {
    if (s_keys[i].load(std::memory_order_acquire) == fp)
      return s_values[i];
  }
  return nullptr;
}

/* The block holding offset, read if needed. nullptr with errno set on failure. */
Block* GetBlock(VFile* file, int64_t offset)
{
  const int64_t block_offset = offset - (offset % BLOCK_SIZE);
  Block* victim = &file->blocks[0];
  for (Block& block : file->blocks)
  {
    if (block.offset == block_offset)
    {
      block.used = ++file->tick;
      return &block;
    }
    if (block.used < victim->used)
      victim = &block;
  }

  if (!victim->data)
    victim->data = std::make_unique<uint8_t[]>(BLOCK_SIZE);
  victim->offset = -1;
  victim->valid = 0;

  const int64_t remaining = file->info.size - block_offset;
  const uint32_t wanted = static_cast<uint32_t>(std::min<int64_t>(remaining, BLOCK_SIZE));
  uint32_t have = 0;
  while (have < wanted)
  {
    const int64_t got = file->file->Read(victim->data.get() + have, wanted - have, block_offset + have);
    s_backend_reads.fetch_add(1, std::memory_order_relaxed);
    if (got < 0)
      return nullptr;
    if (got == 0)
      break;
    have += static_cast<uint32_t>(got);
  }
  s_bytes_read.fetch_add(have, std::memory_order_relaxed);

  victim->offset = block_offset;
  victim->valid = have;
  victim->used = ++file->tick;
  return victim;
}

/* feof() and clearerr() cannot be bound to this layer (compat/ps5_stdio_redirect.cmake), so the
 * real FILE that stands for a virtual stream is the one holder of the end-of-file indicator: it is
 * read to its own end when the virtual stream reaches its end, and cleared when the virtual stream
 * leaves it. A caller's own clearerr() on that FILE is then seen here as well. */
void SetEOF(VFile* file, bool eof)
{
  if ((feof(file->shadow) != 0) == eof)
    return;

  if (eof)
  {
    fseek(file->shadow, 0, SEEK_END);
    (void)fgetc(file->shadow);
  }
  else
  {
    clearerr(file->shadow);
  }
}

/* Reads at the stream's position, with the stream's mutex held. */
size_t ReadLocked(VFile* file, void* buffer, size_t length)
{
  uint8_t* out = static_cast<uint8_t*>(buffer);
  size_t done = 0;

  if (file->ungot >= 0 && length > 0)
  {
    out[done++] = static_cast<uint8_t>(file->ungot);
    file->ungot = -1;
    file->position++;
  }

  while (done < length)
  {
    if (file->position >= file->info.size)
    {
      SetEOF(file, true);
      break;
    }

    Block* const block = GetBlock(file, file->position);
    if (!block)
    {
      // ferror() cannot be bound to this layer, and the real FILE's error indicator cannot be set
      // from outside: end-of-file is what a caller looping on feof() gets to see.
      file->error = true;
      SetEOF(file, true);
      break;
    }

    const uint32_t in_block = static_cast<uint32_t>(file->position - block->offset);
    if (in_block >= block->valid)
    {
      /* The file is shorter than its listing said. */
      SetEOF(file, true);
      break;
    }

    const size_t count = std::min<size_t>(length - done, block->valid - in_block);
    std::memcpy(out + done, block->data.get() + in_block, count);
    done += count;
    file->position += static_cast<int64_t>(count);
  }

  return done;
}

int SeekLocked(VFile* file, int64_t offset, int whence)
{
  int64_t target;
  switch (whence)
  {
    case SEEK_SET:
      target = offset;
      break;
    case SEEK_CUR:
      target = file->position + offset;
      break;
    case SEEK_END:
      target = file->info.size + offset;
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  if (target < 0)
  {
    errno = EINVAL;
    return -1;
  }
  file->position = target;
  SetEOF(file, false);
  file->ungot = -1;
  return 0;
}

bool ValidMountName(const char* name)
{
  if (!name || !name[0] || std::strlen(name) > 32)
    return false;
  for (const char* c = name; *c; c++)
  {
    const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
                    *c == '-' || *c == '.';
    if (!ok)
      return false;
  }
  return std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0;
}

std::string Trim(const std::string& text)
{
  const char* const space = " \t\r\n";
  const size_t first = text.find_first_not_of(space);
  if (first == std::string::npos)
    return {};
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

} // namespace

void Log(const char* format, ...)
{
  if (!s_log)
    return;
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  s_log(line);
}

double MonotonicSeconds()
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

bool Backend::MakeFolder(const std::string&)
{
  errno = EROFS;
  return false;
}

bool Backend::Store(const std::string& path, const void* data, uint64_t size)
{
  const uint8_t* const bytes = static_cast<const uint8_t*>(data);
  return StoreFrom(path, size, [bytes, size](void* buffer, uint32_t length, uint64_t offset) -> int64_t {
    if (offset > size || length > size - offset)
    {
      errno = EINVAL;
      return -1;
    }
    std::memcpy(buffer, bytes + offset, length);
    return length;
  });
}

bool Backend::StoreFrom(const std::string&, uint64_t, const Source&)
{
  errno = EROFS;
  return false;
}

bool Backend::Remove(const std::string&, bool)
{
  errno = EROFS;
  return false;
}

void Mount(const std::string& mount_point, std::unique_ptr<Backend> backend)
{
  std::lock_guard lock(s_mounts_mutex);
  /* Longest mount points first, so a mount inside another is found before its parent. */
  MountPoint mount{mount_point, std::move(backend)};
  const auto at = std::find_if(s_mounts.begin(), s_mounts.end(),
                               [&](const MountPoint& other) { return other.point.size() < mount.point.size(); });
  s_mounts.insert(at, std::move(mount));
  s_mount_count.store(static_cast<unsigned>(s_mounts.size()), std::memory_order_release);
}

/* ------------------------------------------------------------ listing cache */

bool ListingCache::Find(const std::string& directory, std::vector<Entry>* entries)
{
  const double now = MonotonicSeconds();
  for (const Listing& listing : m_listings)
  {
    if (listing.directory == directory && (now - listing.time) < LIFETIME_SECONDS)
    {
      if (entries)
        *entries = listing.entries;
      return true;
    }
  }
  return false;
}

bool ListingCache::FindEntry(const std::string& path, Entry* entry, bool* known_missing)
{
  *known_missing = false;
  const size_t slash = path.rfind('/');
  const std::string directory = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const double now = MonotonicSeconds();
  for (const Listing& listing : m_listings)
  {
    if (listing.directory != directory || (now - listing.time) >= LIFETIME_SECONDS)
      continue;
    for (const Entry& candidate : listing.entries)
    {
      if (candidate.name == name)
      {
        *entry = candidate;
        return true;
      }
    }
    /* A name differing only in case is left to the server to decide: shares usually ignore case. */
    for (const Entry& candidate : listing.entries)
    {
      if (strcasecmp(candidate.name.c_str(), name.c_str()) == 0)
        return false;
    }
    *known_missing = true;
    return false;
  }
  return false;
}

void ListingCache::Store(const std::string& directory, std::vector<Entry> entries)
{
  const double now = MonotonicSeconds();
  for (Listing& listing : m_listings)
  {
    if (listing.directory == directory)
    {
      listing.entries = std::move(entries);
      listing.time = now;
      return;
    }
  }
  if (m_listings.size() >= MAX_LISTINGS)
  {
    const auto oldest = std::min_element(m_listings.begin(), m_listings.end(),
                                         [](const Listing& a, const Listing& b) { return a.time < b.time; });
    m_listings.erase(oldest);
  }
  m_listings.push_back(Listing{directory, std::move(entries), now});
}

void ListingCache::Clear()
{
  m_listings.clear();
}

/* ------------------------------------------------------------ memory backend */

namespace {

class MemoryFile final : public RemoteFile
{
public:
  explicit MemoryFile(const std::string* data) : m_data(data) {}
  int64_t Size() const override { return static_cast<int64_t>(m_data->size()); }
  int64_t Read(void* buffer, uint32_t length, int64_t offset) override
  {
    if (offset >= static_cast<int64_t>(m_data->size()))
      return 0;
    const size_t count = std::min<size_t>(length, m_data->size() - static_cast<size_t>(offset));
    std::memcpy(buffer, m_data->data() + offset, count);
    return static_cast<int64_t>(count);
  }

private:
  const std::string* m_data;
};

class MemoryBackend final : public Backend
{
public:
  explicit MemoryBackend(std::vector<std::pair<std::string, std::string>> files) : m_files(std::move(files)) {}

  bool Stat(const std::string& path, Entry* entry) override
  {
    if (path.empty())
    {
      *entry = Entry{std::string(), true, 0, 0};
      return true;
    }
    for (const auto& [name, data] : m_files)
    {
      if (name == path)
      {
        *entry = Entry{Leaf(name), false, static_cast<int64_t>(data.size()), 1000};
        return true;
      }
      if (IsUnder(name, path))
      {
        *entry = Entry{Leaf(path), true, 0, 1000};
        return true;
      }
    }
    errno = ENOENT;
    return false;
  }

  bool List(const std::string& path, std::vector<Entry>* entries) override
  {
    entries->clear();
    bool found = path.empty();
    for (const auto& [name, data] : m_files)
    {
      std::string rest;
      if (path.empty())
        rest = name;
      else if (IsUnder(name, path))
        rest = name.substr(path.size() + 1);
      else
        continue;
      found = true;
      const size_t slash = rest.find('/');
      Entry entry;
      entry.name = rest.substr(0, slash);
      entry.is_directory = (slash != std::string::npos);
      entry.size = entry.is_directory ? 0 : static_cast<int64_t>(data.size());
      entry.mtime = 1000;
      if (std::none_of(entries->begin(), entries->end(), [&](const Entry& e) { return e.name == entry.name; }))
        entries->push_back(std::move(entry));
    }
    if (!found)
      errno = ENOENT;
    return found;
  }

  std::unique_ptr<RemoteFile> Open(const std::string& path) override
  {
    for (const auto& [name, data] : m_files)
    {
      if (name == path)
        return std::make_unique<MemoryFile>(&data);
    }
    errno = ENOENT;
    return nullptr;
  }

  std::string Describe() const override { return "memory"; }

private:
  static std::string Leaf(const std::string& path)
  {
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
  }

  std::vector<std::pair<std::string, std::string>> m_files;
};

} // namespace

std::unique_ptr<Backend> CreateMemoryBackend(std::vector<std::pair<std::string, std::string>> files)
{
  return std::make_unique<MemoryBackend>(std::move(files));
}

} // namespace ps5vfs

using namespace ps5vfs;

/* ===================================================================== mounts */

extern "C" void ps5vfs_set_log(void (*log)(const char* line))
{
  s_log = log;
}

extern "C" void ps5vfs_set_shadow_path(const char* path)
{
  s_shadow_path.store(path ? path : PS5VFS_SHADOW_PATH, std::memory_order_release);
}

extern "C" int ps5vfs_mount_smb(const char* name, const char* url)
{
  if (!ValidMountName(name) || !url)
  {
    Log("vfs: \"%s\" is not a usable share name (letters, digits, '.', '_' and '-', up to 32)", name ? name : "");
    return -1;
  }

  std::string error;
  std::unique_ptr<Backend> backend = CreateSMBBackend(url, &error);
  if (!backend)
  {
    Log("vfs: share %s: %s", name, error.c_str());
    return -1;
  }

  const std::string point = std::string("/smb/") + name;
  Log("vfs: %s is %s", point.c_str(), backend->Describe().c_str());
  Mount(point, std::move(backend));
  return 0;
}

extern "C" int ps5vfs_mount_nfs(const char* name, const char* url)
{
  if (!ValidMountName(name) || !url)
  {
    Log("vfs: \"%s\" is not a usable share name (letters, digits, '.', '_' and '-', up to 32)", name ? name : "");
    return -1;
  }

  std::string error;
  std::unique_ptr<Backend> backend = CreateNFSBackend(url, &error);
  if (!backend)
  {
    Log("vfs: share %s: %s", name, error.c_str());
    return -1;
  }

  /* Under /smb with the SMB shares: the interface takes every mount there for a network share. */
  const std::string point = std::string("/smb/") + name;
  Log("vfs: %s is %s", point.c_str(), backend->Describe().c_str());
  Mount(point, std::move(backend));
  return 0;
}

extern "C" int ps5vfs_mount_ftp(const char* mount_point, unsigned port, const char* remote_root)
{
  const std::string point = Normalise(mount_point);
  if (point.empty() || point == "/" || !remote_root)
    return -1;

  std::string error;
  std::unique_ptr<Backend> backend = CreateFTPBackend(port, remote_root, &error);
  if (!backend)
  {
    Log("vfs: %s through FTP port %u: %s", point.c_str(), port, error.c_str());
    return -1;
  }

  Log("vfs: %s is %s", point.c_str(), backend->Describe().c_str());
  Mount(point, std::move(backend));
  return 0;
}

namespace {

/* Every byte that is not a letter or a digit as %XX: an account name or a password as part of an
 * smb:// address. */
std::string PercentEncode(const std::string& text)
{
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text)
  {
    const unsigned char byte = static_cast<unsigned char>(c);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9'))
    {
      out += c;
    }
    else
    {
      out += '%';
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
  }
  return out;
}

/* "192.168.1.10/Games/PS2" -> "PS2": the folder a share is shown as under /smb. */
std::string MountNameFor(const std::string& path)
{
  std::string name = path;
  while (!name.empty() && name.back() == '/')
    name.pop_back();
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos)
    name.erase(0, slash + 1);
  for (char& c : name)
  {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.';
    if (!ok)
      c = '_';
  }
  if (name.size() > 28)
    name.resize(28);
  if (name.empty() || name == "." || name == "..")
    name = "share";

  /* Two shares whose last folder has the same name: "PS2", "PS2-2". */
  std::string unique = name;
  for (unsigned n = 2;; n++)
  {
    bool taken = false;
    {
      std::lock_guard lock(s_mounts_mutex);
      for (const MountPoint& mount : s_mounts)
        taken = taken || mount.point == "/smb/" + unique;
    }
    if (!taken)
      return unique;
    unique = name + "-" + std::to_string(n);
  }
}

/* Whether text begins with nfs:// (in any case). */
bool IsNFSAddress(const std::string& text)
{
  return text.size() >= 6 && strncasecmp(text.c_str(), "nfs://", 6) == 0;
}

/* Whether the query of an nfs:// address ("?uid=1&gid=2") names the option key. */
bool QueryNames(const std::string& url, const char* key)
{
  const size_t question = url.find('?');
  if (question == std::string::npos)
    return false;
  const size_t length = std::strlen(key);
  for (size_t at = question + 1; at < url.size();)
  {
    size_t end = url.find('&', at);
    if (end == std::string::npos)
      end = url.size();
    if (end - at >= length && strncasecmp(url.c_str() + at, key, length) == 0 &&
        (end - at == length || url[at + length] == '='))
      return true;
    at = end + 1;
  }
  return false;
}

} // namespace

extern "C" int ps5vfs_load_network_config(const char* config_path)
{
  FILE* const fp = fopen(config_path, "r");
  if (!fp)
    return -1;

  /* The path lines in their order (each with whether it is NFS), so two shares with the same last
   * folder are named in the order the file gives them. */
  std::vector<std::pair<bool, std::string>> paths;
  std::string user = "guest";
  std::string password;
  std::string uid, gid; /* for NFS; empty: the backend's default */
  char line[1024];
  while (fgets(line, sizeof(line), fp))
  {
    std::string text = Trim(line);
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
      text = Trim(text.substr(3));
    if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
      continue;
    const size_t equals = text.find('=');
    if (equals == std::string::npos)
    {
      Log("vfs: %s: no '=' in \"%s\"", config_path, text.c_str());
      continue;
    }
    std::string key = Trim(text.substr(0, equals));
    for (char& c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string value = Trim(text.substr(equals + 1));
    if (key == "path" && IsNFSAddress(value))
    {
      /* An NFS address is taken as written: its own options follow a '?'. */
      paths.emplace_back(true, std::move(value));
    }
    else if (key == "path")
    {
      /* \\server\share\folder and smb://server/share/folder are the same address. */
      for (char& c : value)
        c = (c == '\\') ? '/' : c;
      if (value.compare(0, 6, "smb://") == 0)
        value.erase(0, 6);
      while (!value.empty() && value[0] == '/')
        value.erase(0, 1);
      if (!value.empty())
        paths.emplace_back(false, std::move(value));
    }
    else if (key == "user")
    {
      user = value.empty() ? "guest" : value;
    }
    else if (key == "password")
    {
      password = std::move(value);
    }
    else if (key == "uid" || key == "gid")
    {
      /* A user or group number for the NFS servers; an empty one keeps the default. */
      const bool number = !value.empty() && value.size() <= 10 &&
                          std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                          std::strtoul(value.c_str(), nullptr, 10) <= 0x7fffffffUL;
      if (number || value.empty())
        (key == "uid" ? uid : gid) = std::move(value);
      else
        Log("vfs: %s: %s = \"%s\" is not a number", config_path, key.c_str(), value.c_str());
    }
    else
    {
      Log("vfs: %s: \"%s\" is not a setting (path, user, password, uid, gid)", config_path, key.c_str());
    }
  }
  fclose(fp);

  int mounted = 0;
  for (const auto& [nfs, path] : paths)
  {
    if (nfs)
    {
      /* The file's uid and gid go into the address unless it names its own. */
      std::string url = path;
      for (const auto& [key, number] : {std::pair<const char*, const std::string*>{"uid", &uid}, {"gid", &gid}})
      {
        if (number->empty() || QueryNames(url, key))
          continue;
        url += (url.find('?') == std::string::npos) ? "?" : "&";
        url += std::string(key) + "=" + *number;
      }
      /* Shown as its last folder, as an SMB share is: the address without nfs:// and its options. */
      const size_t question = path.find('?');
      const std::string where = path.substr(6, question == std::string::npos ? std::string::npos : question - 6);
      const std::string name = MountNameFor(where);
      if (ps5vfs_mount_nfs(name.c_str(), url.c_str()) == 0)
        mounted++;
      continue;
    }

    /* The account goes into the address unless the path line carries one of its own. */
    const size_t slash = path.find('/');
    const bool own_account = path.substr(0, slash).find('@') != std::string::npos;
    std::string url = "smb://";
    if (!own_account)
    {
      url += PercentEncode(user);
      if (!password.empty())
        url += ":" + PercentEncode(password);
      url += "@";
    }
    url += path;

    const std::string name = MountNameFor(path);
    if (ps5vfs_mount_smb(name.c_str(), url.c_str()) == 0)
      mounted++;
  }
  return mounted;
}

extern "C" const char* ps5vfs_mount_point(unsigned index)
{
  std::lock_guard lock(s_mounts_mutex);
  return (index < s_mounts.size()) ? s_mounts[index].point.c_str() : nullptr;
}

extern "C" int ps5vfs_describe(char* buffer, size_t size)
{
  std::string text;
  {
    std::lock_guard lock(s_mounts_mutex);
    for (const MountPoint& mount : s_mounts)
    {
      text += mount.point;
      text += ": ";
      text += mount.backend->Describe();
      text += "\n";
    }
  }
  if (size == 0)
    return 0;
  const size_t length = std::min(text.size(), size - 1);
  std::memcpy(buffer, text.data(), length);
  buffer[length] = '\0';
  return static_cast<int>(length);
}

/* ==================================================================== writing */

extern "C" int ps5vfs_make_folders(const char* path)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);
  if (!backend)
  {
    errno = ENOENT;
    return -1;
  }

  /* Each folder of the path below the mount, from the top: one that is there is left alone. */
  for (size_t end = 0; end < relative.size();)
  {
    end = relative.find('/', end + 1);
    if (end == std::string::npos)
      end = relative.size();
    if (!backend->MakeFolder(relative.substr(0, end)))
    {
      if (errno == 0)
        errno = EIO;
      return -1;
    }
  }
  return 0;
}

extern "C" int ps5vfs_store(const char* path, const void* data, size_t size)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);
  if (!backend || relative.empty())
  {
    errno = backend ? EISDIR : ENOENT;
    return -1;
  }
  if (!backend->Store(relative, data, size))
  {
    if (errno == 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

int ps5vfs_store_from(const char* path, uint64_t size, const std::function<int64_t(void* buffer, uint32_t length, uint64_t offset)>& source)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);
  if (!backend || relative.empty())
  {
    errno = backend ? EISDIR : ENOENT;
    return -1;
  }
  if (!backend->StoreFrom(relative, size, source))
  {
    if (errno == 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

extern "C" int ps5vfs_remove(const char* path, int folder)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);
  if (!backend || relative.empty())
  {
    errno = backend ? EBUSY : ENOENT;
    return -1;
  }
  if (!backend->Remove(relative, folder != 0))
  {
    if (errno == 0)
      errno = EIO;
    return -1;
  }
  return 0;
}

/* ====================================================================== paths */

extern "C" int ps5vfs_is_virtual(const char* path)
{
  if (s_mount_count.load(std::memory_order_acquire) == 0)
    return 0;

  const std::string normal = Normalise(path);
  if (normal.empty())
    return 0;
  std::string relative;
  if (Resolve(normal, &relative))
    return 1;
  return SyntheticChildren(normal).empty() ? 0 : 1;
}

extern "C" int ps5vfs_stat(const char* path, struct stat* st)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);
  if (!backend)
  {
    if (!normal.empty() && !SyntheticChildren(normal).empty())
    {
      FillStat(st, Entry{std::string(), true, 0, 0});
      return 0;
    }
    errno = ENOENT;
    return -1;
  }

  Entry entry;
  if (relative.empty())
  {
    /* A mount point is a folder whether or not its server answers: the reason it does not shows
     * when the folder is listed. */
    entry.is_directory = true;
  }
  else if (!backend->Stat(relative, &entry))
  {
    if (errno == 0)
      errno = ENOENT;
    return -1;
  }

  FillStat(st, entry);
  return 0;
}

struct ps5vfs_dir
{
  std::vector<Entry> entries;
  size_t next = 0;
};

extern "C" ps5vfs_dir* ps5vfs_opendir(const char* path)
{
  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = Resolve(normal, &relative);

  auto dir = std::make_unique<ps5vfs_dir>();
  if (backend)
  {
    if (!backend->List(relative, &dir->entries))
    {
      if (errno == 0)
        errno = ENOENT;
      return nullptr;
    }
  }
  else
  {
    const std::vector<std::string> children = normal.empty() ? std::vector<std::string>() : SyntheticChildren(normal);
    if (children.empty())
    {
      errno = ENOENT;
      return nullptr;
    }
    for (const std::string& name : children)
      dir->entries.push_back(Entry{name, true, 0, 0});
  }

  return dir.release();
}

extern "C" int ps5vfs_readdir(ps5vfs_dir* dir, struct ps5vfs_dirent* entry)
{
  if (!dir || dir->next >= dir->entries.size())
    return 0;
  const Entry& current = dir->entries[dir->next++];
  entry->name = current.name.c_str();
  entry->is_directory = current.is_directory ? 1 : 0;
  entry->size = current.size;
  entry->mtime = current.mtime;
  return 1;
}

extern "C" void ps5vfs_closedir(ps5vfs_dir* dir)
{
  delete dir;
}

/* ====================================================================== stdio */

extern "C" FILE* ps5vfs_fopen(const char* path, const char* mode)
{
  if (s_mount_count.load(std::memory_order_acquire) == 0)
    return fopen(path, mode);

  const std::string normal = Normalise(path);
  std::string relative;
  Backend* const backend = normal.empty() ? nullptr : Resolve(normal, &relative);
  if (!backend)
  {
    if (!normal.empty() && !SyntheticChildren(normal).empty())
    {
      errno = EISDIR;
      return nullptr;
    }
    return fopen(path, mode);
  }

  if (!mode || mode[0] != 'r' || std::strchr(mode, '+'))
  {
    errno = EROFS;
    return nullptr;
  }
  if (relative.empty())
  {
    errno = EISDIR;
    return nullptr;
  }

  Entry info;
  if (!backend->Stat(relative, &info))
  {
    if (errno == 0)
      errno = ENOENT;
    return nullptr;
  }
  if (info.is_directory)
  {
    errno = EISDIR;
    return nullptr;
  }

  std::unique_ptr<RemoteFile> remote = backend->Open(relative);
  if (!remote)
  {
    if (errno == 0)
      errno = EIO;
    return nullptr;
  }

  auto file = std::make_unique<VFile>();
  file->file = std::move(remote);
  file->info = std::move(info);
  file->info.size = file->file->Size();
  file->shadow = fopen(s_shadow_path.load(std::memory_order_acquire), "rb");
  if (!file->shadow)
    return nullptr;

  std::lock_guard lock(s_open_mutex);
  for (unsigned i = 0; i < MAX_OPEN; i++)
  {
    if (s_keys[i].load(std::memory_order_relaxed) == nullptr)
    {
      FILE* const shadow = file->shadow;
      s_values[i] = file.release();
      s_keys[i].store(shadow, std::memory_order_release);
      s_open_count.fetch_add(1, std::memory_order_release);
      return shadow;
    }
  }

  fclose(file->shadow);
  errno = EMFILE;
  return nullptr;
}

extern "C" int ps5vfs_fclose(FILE* fp)
{
  VFile* file = nullptr;
  if (s_open_count.load(std::memory_order_acquire) != 0 && fp)
  {
    std::lock_guard lock(s_open_mutex);
    for (unsigned i = 0; i < MAX_OPEN; i++)
    {
      if (s_keys[i].load(std::memory_order_relaxed) == fp)
      {
        file = s_values[i];
        s_keys[i].store(nullptr, std::memory_order_release);
        s_values[i] = nullptr;
        s_open_count.fetch_sub(1, std::memory_order_release);
        break;
      }
    }
  }
  if (!file)
    return fclose(fp);

  {
    /* Whoever is still inside a read on this stream finishes first. */
    std::lock_guard lock(file->mutex);
    file->file.reset();
  }
  delete file;
  return fclose(fp);
}

extern "C" size_t ps5vfs_fread(void* buffer, size_t size, size_t count, FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return fread(buffer, size, count, fp);
  if (size == 0 || count == 0)
    return 0;

  std::lock_guard lock(file->mutex);
  const size_t done = ReadLocked(file, buffer, size * count);
  return done / size;
}

extern "C" size_t ps5vfs_fwrite(const void* buffer, size_t size, size_t count, FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return fwrite(buffer, size, count, fp);

  std::lock_guard lock(file->mutex);
  file->error = true;
  errno = EROFS;
  return 0;
}

extern "C" int ps5vfs_fseeko(FILE* fp, off_t offset, int whence)
{
  VFile* const file = Find(fp);
  if (!file)
    return fseeko(fp, offset, whence);

  std::lock_guard lock(file->mutex);
  return SeekLocked(file, static_cast<int64_t>(offset), whence);
}

extern "C" int ps5vfs_fseek(FILE* fp, long offset, int whence)
{
  VFile* const file = Find(fp);
  if (!file)
    return fseek(fp, offset, whence);

  std::lock_guard lock(file->mutex);
  return SeekLocked(file, static_cast<int64_t>(offset), whence);
}

extern "C" off_t ps5vfs_ftello(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return ftello(fp);

  std::lock_guard lock(file->mutex);
  return static_cast<off_t>(file->position);
}

extern "C" long ps5vfs_ftell(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return ftell(fp);

  std::lock_guard lock(file->mutex);
  return static_cast<long>(file->position);
}

extern "C" void ps5vfs_rewind(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
  {
    rewind(fp);
    return;
  }

  std::lock_guard lock(file->mutex);
  SeekLocked(file, 0, SEEK_SET);
  file->error = false;
}

extern "C" int ps5vfs_feof(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return feof(fp);

  std::lock_guard lock(file->mutex);
  return feof(file->shadow) ? 1 : 0;
}

extern "C" int ps5vfs_ferror(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return ferror(fp);

  std::lock_guard lock(file->mutex);
  return file->error ? 1 : 0;
}

extern "C" void ps5vfs_clearerr(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
  {
    clearerr(fp);
    return;
  }

  std::lock_guard lock(file->mutex);
  SetEOF(file, false);
  file->error = false;
}

extern "C" int ps5vfs_fgetc(FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return fgetc(fp);

  std::lock_guard lock(file->mutex);
  uint8_t c;
  return (ReadLocked(file, &c, 1) == 1) ? static_cast<int>(c) : EOF;
}

extern "C" int ps5vfs_ungetc(int c, FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return ungetc(c, fp);

  std::lock_guard lock(file->mutex);
  if (c == EOF || file->ungot >= 0 || file->position == 0)
    return EOF;
  file->ungot = c & 0xff;
  file->position--;
  SetEOF(file, false);
  return c & 0xff;
}

extern "C" char* ps5vfs_fgets(char* buffer, int size, FILE* fp)
{
  VFile* const file = Find(fp);
  if (!file)
    return fgets(buffer, size, fp);
  if (size <= 0)
    return nullptr;

  std::lock_guard lock(file->mutex);
  int length = 0;
  while (length < size - 1)
  {
    uint8_t c;
    if (ReadLocked(file, &c, 1) != 1)
      break;
    buffer[length++] = static_cast<char>(c);
    if (c == '\n')
      break;
  }
  if (length == 0)
    return nullptr;
  buffer[length] = '\0';
  return buffer;
}

extern "C" int ps5vfs_fflush(FILE* fp)
{
  if (fp && Find(fp))
    return 0;
  return fflush(fp);
}

extern "C" int ps5vfs_fileno(FILE* fp)
{
  if (Find(fp))
  {
    /* No descriptor stands for a virtual stream; ps5vfs_fstat answers what fstat would. */
    errno = EBADF;
    return -1;
  }
  return fileno(fp);
}

extern "C" int ps5vfs_fstat(FILE* fp, struct stat* st)
{
  VFile* const file = Find(fp);
  if (!file)
    return 0;

  std::lock_guard lock(file->mutex);
  FillStat(st, file->info);
  return 1;
}

extern "C" void ps5vfs_stats(unsigned* open_files, uint64_t* bytes_read, uint64_t* backend_reads)
{
  if (open_files)
    *open_files = s_open_count.load(std::memory_order_relaxed);
  if (bytes_read)
    *bytes_read = s_bytes_read.load(std::memory_order_relaxed);
  if (backend_reads)
    *backend_reads = s_backend_reads.load(std::memory_order_relaxed);
}
