// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 - host test of the virtual file layer's stall count (ps5vfs_stalled_seconds) and of the
// shares woken at start (ps5vfs_wake_file / ps5vfs_wake_one), with no network: a backend in
// memory whose reads can be held, and the layer alone (the network backends stubbed).
#include "vfs/ps5_vfs.h"
#include "vfs/ps5_vfs_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

static int s_failures = 0;
#define CHECK(x)                                                         \
  do                                                                     \
  {                                                                      \
    if (!(x))                                                            \
    {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
      s_failures++;                                                      \
    }                                                                    \
  } while (0)

namespace ps5vfs {
// The network backends are not part of this test.
std::unique_ptr<Backend> CreateSMBBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateNFSBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateFTPBackend(unsigned, const char*, std::string* error) { *error = "not in this test"; return nullptr; }
} // namespace ps5vfs

namespace {
// One file whose reads wait while `hold` is set: a share that stopped answering.
std::mutex s_mutex;
std::condition_variable s_cv;
bool s_hold = false;
std::atomic<int> s_reads{0};

class HeldFile final : public ps5vfs::RemoteFile
{
public:
  explicit HeldFile(int64_t size) : m_size(size) {}
  int64_t Size() const override { return m_size; }
  int64_t Read(void* buffer, uint32_t length, int64_t offset) override
  {
    {
      std::unique_lock lock(s_mutex);
      s_cv.wait(lock, [] { return !s_hold; });
    }
    s_reads++;
    const int64_t n = std::min<int64_t>(length, m_size - offset);
    std::memset(buffer, 0x5A, static_cast<size_t>(std::max<int64_t>(n, 0)));
    return std::max<int64_t>(n, 0);
  }

private:
  int64_t m_size;
};

class HeldBackend final : public ps5vfs::Backend
{
public:
  static constexpr int64_t SIZE = 20 << 20;
  bool Stat(const std::string& path, ps5vfs::Entry* entry) override
  {
    if (path.empty() || path == "games")
    {
      entry->name = path;
      entry->is_directory = true;
      return true;
    }
    if (path == "games/big.iso" || path == "small.txt")
    {
      entry->name = path;
      entry->is_directory = false;
      entry->size = (path == "small.txt") ? 5 : SIZE;
      return true;
    }
    errno = ENOENT;
    return false;
  }
  bool List(const std::string& path, std::vector<ps5vfs::Entry>* entries) override
  {
    if (path.empty())
    {
      entries->push_back({"small.txt", false, 5, 0});
      entries->push_back({"games", true, 0, 0});
      return true;
    }
    if (path == "games")
    {
      entries->push_back({"big.iso", false, SIZE, 0});
      return true;
    }
    errno = ENOENT;
    return false;
  }
  std::unique_ptr<ps5vfs::RemoteFile> Open(const std::string& path) override
  {
    ps5vfs::Entry e;
    if (!Stat(path, &e) || e.is_directory)
      return nullptr;
    return std::make_unique<HeldFile>(e.size);
  }
  std::string Describe() const override { return "held"; }
};
} // namespace

int main()
{
  ps5vfs::Mount("/smb/held", std::make_unique<HeldBackend>());
  ps5vfs::Mount("/smb/empty", ps5vfs::CreateMemoryBackend({{"readme.txt", "hello"}}));

  // Nothing under way: no stall.
  CHECK(ps5vfs_stalled_seconds() == 0.0);

  // The file woken: the one played last when it is on the share, else the first large one.
  CHECK(ps5vfs_wake_file("/smb/held", {}) == "/smb/held/games/big.iso");
  CHECK(ps5vfs_wake_file("/smb/held", {"/smb/other/x.iso", "/smb/held/games/big.iso"}) == "/smb/held/games/big.iso");
  CHECK(ps5vfs_wake_file("/smb/held", {"/smb/heldover/x.iso"}) == "/smb/held/games/big.iso");
  CHECK(ps5vfs_wake_file("/smb/empty", {}).empty());
  {
    std::string file;
    const size_t got = ps5vfs_wake_one("/smb/held", {}, &file);
    CHECK(file == "/smb/held/games/big.iso");
    CHECK(got == 2 * 64 * 1024);
    CHECK(ps5vfs_wake_one("/smb/empty", {}, &file) == 0 && file.empty());
  }
  CHECK(ps5vfs_stalled_seconds() == 0.0);

  // A read held: the seconds count up while it waits, and are 0 once it is done.
  FILE* const fp = ps5vfs_fopen("/smb/held/games/big.iso", "rb");
  CHECK(fp != nullptr);
  if (fp)
  {
    {
      std::lock_guard lock(s_mutex);
      s_hold = true;
    }
    std::atomic<bool> done{false};
    std::thread reader([&] {
      char buffer[4096];
      ps5vfs_fseeko(fp, 10 << 20, SEEK_SET);
      CHECK(ps5vfs_fread(buffer, 1, sizeof(buffer), fp) == sizeof(buffer));
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const double stalled = ps5vfs_stalled_seconds();
    CHECK(stalled >= 1.1 && stalled < 5.0);
    CHECK(!done);
    {
      std::lock_guard lock(s_mutex);
      s_hold = false;
    }
    s_cv.notify_all();
    reader.join();
    CHECK(done);
    CHECK(ps5vfs_stalled_seconds() == 0.0);
    ps5vfs_fclose(fp);
  }

  // The mounts woken on a thread: returns at once.
  ps5vfs_wake_mounts({});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  if (s_failures)
  {
    std::fprintf(stderr, "vfs stall test: %d failed\n", s_failures);
    return 1;
  }
  std::printf("vfs stall test: ok\n");
  return 0;
}
