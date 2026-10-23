// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/* Crux2 - the virtual file layer's backends (see ps5_vfs.h). */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps5vfs {

struct Entry
{
  std::string name;
  bool is_directory = false;
  int64_t size = 0;
  int64_t mtime = 0;
};

/* An open remote file: positioned reads only. */
class RemoteFile
{
public:
  virtual ~RemoteFile() = default;
  virtual int64_t Size() const = 0;
  /* Reads up to length bytes at offset. Returns the count (0 at the end), or -1 with errno. */
  virtual int64_t Read(void* buffer, uint32_t length, int64_t offset) = 0;
};

/* One mounted tree. Paths given to it are relative to the mount, with no leading slash ("" is the root).
 * Every method may be called from any thread. */
class Backend
{
public:
  virtual ~Backend() = default;
  virtual bool Stat(const std::string& path, Entry* entry) = 0;
  virtual bool List(const std::string& path, std::vector<Entry>* entries) = 0;
  virtual std::unique_ptr<RemoteFile> Open(const std::string& path) = 0;
  virtual std::string Describe() const = 0;

  /* Writing: whole files, for the backups of memory cards and saved states (nothing else writes).
   * A backend that cannot write leaves these as they are: a read-only file system. Each returns
   * false with errno. */
  /* The folder is there afterwards (it was made, or was there already). Its parent must be. */
  virtual bool MakeFolder(const std::string& path);
  /* The file holds exactly these bytes afterwards. It is written beside its place and moved
   * there, so a transfer cut short leaves no half file under the name. (StoreFrom with the bytes
   * as its source.) */
  bool Store(const std::string& path, const void* data, uint64_t size);
  /* The same for a file too large to hold in memory (a disc image): `size` bytes asked of `source`
   * piece by piece. source(buffer, length, offset) fills the buffer with the file's `length` bytes
   * from `offset` and returns length, or returns -1 (with errno) to stop: the half-written file is
   * then removed and StoreFrom returns false with that errno. When a lost connection makes the
   * backend begin again, the source is asked again from offset 0. */
  using Source = std::function<int64_t(void* buffer, uint32_t length, uint64_t offset)>;
  virtual bool StoreFrom(const std::string& path, uint64_t size, const Source& source);
  /* The file, or the empty folder, is gone afterwards (true also when it was not there). */
  virtual bool Remove(const std::string& path, bool folder);
};

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

/* smb://[domain;][user[:password]@]host[:port]/share[/folder] */
std::unique_ptr<Backend> CreateSMBBackend(const char* url, std::string* error);
/* nfs://server[:port]/path[?uid=N&gid=N&version=3|4|4.2&nfsport=N&mountport=N] (ps5_vfs_nfs.cpp) */
std::unique_ptr<Backend> CreateNFSBackend(const char* url, std::string* error);
/* An FTP server on the loopback. */
std::unique_ptr<Backend> CreateFTPBackend(unsigned port, const char* remote_root, std::string* error);

/* A fixed listing, for tests of the layer above the backends. */
std::unique_ptr<Backend> CreateMemoryBackend(std::vector<std::pair<std::string, std::string>> files);
void Mount(const std::string& mount_point, std::unique_ptr<Backend> backend);

/* A directory listing kept for a short time, so the stat() that follows every readdir() entry
 * costs nothing. */
class ListingCache
{
public:
  bool Find(const std::string& directory, std::vector<Entry>* entries);
  bool FindEntry(const std::string& path, Entry* entry, bool* known_missing);
  void Store(const std::string& directory, std::vector<Entry> entries);
  void Clear();

private:
  struct Listing
  {
    std::string directory;
    std::vector<Entry> entries;
    double time = 0.0;
  };
  static constexpr double LIFETIME_SECONDS = 20.0;
  static constexpr size_t MAX_LISTINGS = 64;
  std::vector<Listing> m_listings;
};

double MonotonicSeconds();

} // namespace ps5vfs
