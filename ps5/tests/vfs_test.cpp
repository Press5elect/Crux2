// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - host tests of the virtual file layer (ps5/frontend/vfs).
 *
 *   vfs_test memory                       stdio semantics over a fixed tree
 *   vfs_test ftp <port> <local-root>      an FTP server on the loopback serving <local-root>
 *   vfs_test smb <url> <local-root>       a share whose content is <local-root>
 *   vfs_test nfs <url> <local-root>       an NFS export (or a folder in one) whose content is <local-root>
 *   vfs_test nfs-parse <work folder>      NFS addresses and network.cfg's NFS lines, with no server
 *   (and -write, -readonly, -drop, -write-drop variants of smb and nfs: see main)
 *
 * The remote tests read every file both ways and compare.
 */
#include "../frontend/vfs/ps5_vfs.h"
#include "../frontend/vfs/ps5_vfs_internal.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    s_checks++;                                                                                                        \
    if (!(condition))                                                                                                  \
    {                                                                                                                  \
      s_failures++;                                                                                                    \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                      \
    }                                                                                                                  \
  } while (0)

static void LogLine(const char* line)
{
  std::fprintf(stderr, "  %s\n", line);
}

static std::string ReadLocal(const std::string& path)
{
  std::string data;
  FILE* const fp = fopen(path.c_str(), "rb");
  if (!fp)
    return data;
  char block[65536];
  size_t got;
  while ((got = fread(block, 1, sizeof(block), fp)) > 0)
    data.append(block, got);
  fclose(fp);
  return data;
}

static void TestMemory()
{
  std::string big(700000, '\0');
  std::mt19937 rng(1);
  for (char& c : big)
    c = static_cast<char>(rng());

  const std::string cue = "FILE \"one.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
  ps5vfs::Mount("/mem/a", ps5vfs::CreateMemoryBackend({
                            {"games/one.cue", cue},
                            {"games/one.bin", big},
                            {"games/sub/two.chd", "chd-data"},
                            {"readme.txt", "hello"},
                          }));
  ps5vfs::Mount("/mem/b", ps5vfs::CreateMemoryBackend({{"x", "y"}}));

  CHECK(ps5vfs_is_virtual("/mem"));
  CHECK(ps5vfs_is_virtual("/mem/a"));
  CHECK(ps5vfs_is_virtual("/mem/a/games/one.bin"));
  CHECK(ps5vfs_is_virtual("//mem//a/./games/../games"));
  CHECK(!ps5vfs_is_virtual("/"));
  CHECK(!ps5vfs_is_virtual("/memory"));
  CHECK(!ps5vfs_is_virtual("/tmp"));
  CHECK(!ps5vfs_is_virtual("relative/mem/a"));

  struct stat st;
  CHECK(ps5vfs_stat("/mem", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(ps5vfs_stat("/mem/a", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(ps5vfs_stat("/mem/a/games", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(ps5vfs_stat("/mem/a/games/one.bin", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 700000);
  CHECK(ps5vfs_stat("/mem/a/games/missing.bin", &st) != 0 && errno == ENOENT);
  CHECK(ps5vfs_stat("/mem/c", &st) != 0);

  /* The folder made of mount points lists them. */
  std::vector<std::string> names;
  ps5vfs_dir* dir = ps5vfs_opendir("/mem");
  CHECK(dir != nullptr);
  struct ps5vfs_dirent entry;
  while (dir && ps5vfs_readdir(dir, &entry))
    names.push_back(entry.name);
  ps5vfs_closedir(dir);
  std::sort(names.begin(), names.end());
  CHECK(names.size() == 2 && names[0] == "a" && names[1] == "b");

  names.clear();
  dir = ps5vfs_opendir("/mem/a/games");
  CHECK(dir != nullptr);
  int folders = 0;
  while (dir && ps5vfs_readdir(dir, &entry))
  {
    names.push_back(entry.name);
    folders += entry.is_directory;
  }
  ps5vfs_closedir(dir);
  CHECK(names.size() == 3 && folders == 1);
  CHECK(ps5vfs_opendir("/mem/a/nothing") == nullptr);

  /* Text: fgets, fgetc, ungetc, feof. */
  FILE* fp = ps5vfs_fopen("/mem/a/games/one.cue", "r");
  CHECK(fp != nullptr);
  if (fp)
  {
    char line[128];
    CHECK(ps5vfs_fgets(line, sizeof(line), fp) != nullptr && std::strcmp(line, "FILE \"one.bin\" BINARY\n") == 0);
    CHECK(ps5vfs_fgetc(fp) == ' ');
    CHECK(ps5vfs_ungetc(' ', fp) == ' ');
    CHECK(ps5vfs_ftell(fp) == 22);
    CHECK(ps5vfs_fgets(line, sizeof(line), fp) != nullptr && std::strcmp(line, "  TRACK 01 MODE2/2352\n") == 0);
    CHECK(ps5vfs_fgets(line, 5, fp) != nullptr && std::strcmp(line, "    ") == 0);
    CHECK(ps5vfs_fgets(line, sizeof(line), fp) != nullptr);
    CHECK(!ps5vfs_feof(fp));
    CHECK(!feof(fp));
    CHECK(ps5vfs_fgets(line, sizeof(line), fp) == nullptr);
    CHECK(ps5vfs_feof(fp));
    /* libc's own feof() and clearerr() see the same, since PCSX2 calls those directly. */
    CHECK(feof(fp));
    CHECK(ps5vfs_fgetc(fp) == EOF);
    clearerr(fp);
    CHECK(!feof(fp) && !ps5vfs_feof(fp));
    CHECK(ps5vfs_fgetc(fp) == EOF && feof(fp));
    CHECK(ps5vfs_fseek(fp, 0, SEEK_SET) == 0 && !feof(fp));
    CHECK(ps5vfs_fseek(fp, 0, SEEK_END) == 0 && ps5vfs_fgetc(fp) == EOF && feof(fp));
    ps5vfs_clearerr(fp);
    CHECK(!ps5vfs_feof(fp) && !feof(fp));
    CHECK(ps5vfs_fileno(fp) == -1);
    CHECK(ps5vfs_fstat(fp, &st) == 1 && static_cast<size_t>(st.st_size) == cue.size());
    CHECK(ps5vfs_fwrite("x", 1, 1, fp) == 0 && ps5vfs_ferror(fp));
    CHECK(ps5vfs_fclose(fp) == 0);
  }

  /* Binary: sizes, seeks across blocks, item counts. */
  fp = ps5vfs_fopen("/mem/a/games/one.bin", "rb");
  CHECK(fp != nullptr);
  if (fp)
  {
    CHECK(ps5vfs_fseek(fp, 0, SEEK_END) == 0 && ps5vfs_ftello(fp) == 700000);
    ps5vfs_rewind(fp);
    CHECK(ps5vfs_ftell(fp) == 0);
    std::string all(700000, '\0');
    CHECK(ps5vfs_fread(all.data(), 1, all.size(), fp) == all.size() && all == big);
    CHECK(!ps5vfs_feof(fp));
    char one;
    CHECK(ps5vfs_fread(&one, 1, 1, fp) == 0 && ps5vfs_feof(fp));

    for (int i = 0; i < 2000; i++)
    {
      const size_t offset = rng() % 700000;
      const size_t length = 1 + rng() % 5000;
      std::string buffer(length, '\0');
      CHECK(ps5vfs_fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0);
      const size_t got = ps5vfs_fread(buffer.data(), 1, length, fp);
      const size_t expected = std::min(length, big.size() - offset);
      if (got != expected || std::memcmp(buffer.data(), big.data() + offset, got) != 0)
      {
        CHECK(!"random read matches");
        break;
      }
    }

    /* fread counts whole items. */
    CHECK(ps5vfs_fseek(fp, -5, SEEK_END) == 0);
    char items[16];
    CHECK(ps5vfs_fread(items, 2, 8, fp) == 2);
    CHECK(ps5vfs_fseek(fp, -1, SEEK_SET) != 0);
    CHECK(ps5vfs_fclose(fp) == 0);
  }

  /* Writing and folders are refused; a path outside the mounts is libc's. */
  CHECK(ps5vfs_fopen("/mem/a/new.txt", "wb") == nullptr && errno == EROFS);
  CHECK(ps5vfs_fopen("/mem/a/readme.txt", "r+b") == nullptr);
  CHECK(ps5vfs_fopen("/mem/a/games", "rb") == nullptr && errno == EISDIR);
  CHECK(ps5vfs_fopen("/mem", "rb") == nullptr);
  CHECK(ps5vfs_fopen("/mem/a/none", "rb") == nullptr && errno == ENOENT);
  fp = ps5vfs_fopen("/proc/self/status", "r");
  CHECK(fp != nullptr);
  if (fp)
  {
    char line[64];
    CHECK(ps5vfs_fgets(line, sizeof(line), fp) != nullptr);
    CHECK(ps5vfs_fstat(fp, &st) == 0);
    CHECK(ps5vfs_fileno(fp) >= 0);
    CHECK(ps5vfs_fclose(fp) == 0);
  }

  /* Many streams at once, from several threads. */
  std::vector<std::thread> threads;
  std::atomic<int> thread_failures{0};
  for (int t = 0; t < 8; t++)
  {
    threads.emplace_back([&, t]() {
      std::mt19937 local(100 + t);
      for (int round = 0; round < 50; round++)
      {
        FILE* const file = ps5vfs_fopen("/mem/a/games/one.bin", "rb");
        if (!file)
        {
          thread_failures++;
          return;
        }
        for (int i = 0; i < 40; i++)
        {
          const size_t offset = local() % 699000;
          char buffer[1000];
          if (ps5vfs_fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
              ps5vfs_fread(buffer, 1, sizeof(buffer), file) != sizeof(buffer) ||
              std::memcmp(buffer, big.data() + offset, sizeof(buffer)) != 0)
          {
            thread_failures++;
          }
        }
        ps5vfs_fclose(file);
      }
    });
  }
  for (std::thread& thread : threads)
    thread.join();
  CHECK(thread_failures == 0);

  unsigned open_files = 99;
  ps5vfs_stats(&open_files, nullptr, nullptr);
  CHECK(open_files == 0);
}

/* Walks the local tree and checks that the mounted copy lists and reads the same. */
static void CompareTree(const std::string& local, const std::string& remote, int* files, int* folders)
{
  std::vector<std::pair<std::string, bool>> local_names;
  DIR* const dir = opendir(local.c_str());
  CHECK(dir != nullptr);
  if (!dir)
    return;
  while (struct dirent* const item = readdir(dir))
  {
    if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, ".."))
      continue;
    struct stat st;
    if (stat((local + "/" + item->d_name).c_str(), &st) == 0)
      local_names.emplace_back(item->d_name, S_ISDIR(st.st_mode));
  }
  closedir(dir);

  std::vector<std::pair<std::string, bool>> remote_names;
  ps5vfs_dir* const vdir = ps5vfs_opendir(remote.c_str());
  CHECK(vdir != nullptr);
  struct ps5vfs_dirent entry;
  while (vdir && ps5vfs_readdir(vdir, &entry))
    remote_names.emplace_back(entry.name, entry.is_directory != 0);
  ps5vfs_closedir(vdir);

  std::sort(local_names.begin(), local_names.end());
  std::sort(remote_names.begin(), remote_names.end());
  CHECK(local_names == remote_names);
  if (local_names != remote_names)
  {
    std::fprintf(stderr, "  %s: %zu local entries, %zu remote\n", remote.c_str(), local_names.size(), remote_names.size());
    return;
  }

  std::mt19937 rng(7);
  for (const auto& [name, is_directory] : local_names)
  {
    const std::string local_path = local + "/" + name;
    const std::string remote_path = remote + "/" + name;
    struct stat st;
    CHECK(ps5vfs_stat(remote_path.c_str(), &st) == 0);
    if (is_directory)
    {
      CHECK(S_ISDIR(st.st_mode));
      (*folders)++;
      CompareTree(local_path, remote_path, files, folders);
      continue;
    }

    (*files)++;
    const std::string expected = ReadLocal(local_path);
    CHECK(S_ISREG(st.st_mode) && static_cast<size_t>(st.st_size) == expected.size());
    CHECK(st.st_mtime > 0);

    FILE* const fp = ps5vfs_fopen(remote_path.c_str(), "rb");
    CHECK(fp != nullptr);
    if (!fp)
      continue;

    /* Front to back, in the sizes a disc reader uses. */
    std::string all;
    char block[2352 * 8];
    size_t got;
    while ((got = ps5vfs_fread(block, 1, sizeof(block), fp)) > 0)
      all.append(block, got);
    CHECK(all == expected);
    CHECK(ps5vfs_feof(fp) && !ps5vfs_ferror(fp));

    /* Then anywhere. */
    for (int i = 0; i < 60 && !expected.empty(); i++)
    {
      const size_t offset = rng() % expected.size();
      const size_t length = 1 + rng() % 70000;
      std::string buffer(length, '\0');
      CHECK(ps5vfs_fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0);
      const size_t count = ps5vfs_fread(buffer.data(), 1, length, fp);
      const size_t want = std::min(length, expected.size() - offset);
      if (count != want || std::memcmp(buffer.data(), expected.data() + offset, count) != 0)
      {
        CHECK(!"random read matches");
        break;
      }
    }
    CHECK(ps5vfs_fclose(fp) == 0);
  }
}

static void TestRemote(const std::string& mount_point, const std::string& local_root)
{
  int files = 0, folders = 0;
  CompareTree(local_root, mount_point, &files, &folders);
  std::fprintf(stderr, "  compared %d files in %d folders\n", files, folders + 1);
  CHECK(files > 0);

  struct stat st;
  CHECK(ps5vfs_stat((mount_point + "/no-such-file.bin").c_str(), &st) != 0);
  CHECK(ps5vfs_fopen((mount_point + "/no-such-file.bin").c_str(), "rb") == nullptr);
  CHECK(ps5vfs_opendir((mount_point + "/no-such-folder").c_str()) == nullptr);

  /* Two streams on one file, interleaved, and two threads reading different files. */
  std::vector<std::string> paths;
  ps5vfs_dir* const dir = ps5vfs_opendir(mount_point.c_str());
  struct ps5vfs_dirent entry;
  while (dir && ps5vfs_readdir(dir, &entry))
  {
    if (!entry.is_directory && entry.size > 100000)
      paths.push_back(entry.name);
  }
  ps5vfs_closedir(dir);
  CHECK(!paths.empty());
  if (paths.empty())
    return;

  const std::string expected = ReadLocal(local_root + "/" + paths[0]);
  FILE* const a = ps5vfs_fopen((mount_point + "/" + paths[0]).c_str(), "rb");
  FILE* const b = ps5vfs_fopen((mount_point + "/" + paths[0]).c_str(), "rb");
  CHECK(a && b);
  if (a && b)
  {
    char buffer[5000];
    for (int i = 0; i < 20; i++)
    {
      CHECK(ps5vfs_fread(buffer, 1, sizeof(buffer), a) == sizeof(buffer));
      CHECK(std::memcmp(buffer, expected.data() + i * sizeof(buffer), sizeof(buffer)) == 0);
      CHECK(ps5vfs_fseek(b, static_cast<long>(expected.size() - (i + 1) * sizeof(buffer)), SEEK_SET) == 0);
      CHECK(ps5vfs_fread(buffer, 1, sizeof(buffer), b) == sizeof(buffer));
      CHECK(std::memcmp(buffer, expected.data() + expected.size() - (i + 1) * sizeof(buffer), sizeof(buffer)) == 0);
    }
  }
  if (a)
    ps5vfs_fclose(a);
  if (b)
    ps5vfs_fclose(b);

  std::atomic<int> thread_failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++)
  {
    threads.emplace_back([&, t]() {
      const std::string& name = paths[t % paths.size()];
      const std::string data = ReadLocal(local_root + "/" + name);
      FILE* const fp = ps5vfs_fopen((mount_point + "/" + name).c_str(), "rb");
      if (!fp)
      {
        thread_failures++;
        return;
      }
      std::mt19937 rng(t);
      for (int i = 0; i < 40; i++)
      {
        const size_t offset = rng() % (data.size() - 3000);
        char buffer[3000];
        if (ps5vfs_fseek(fp, static_cast<long>(offset), SEEK_SET) != 0 ||
            ps5vfs_fread(buffer, 1, sizeof(buffer), fp) != sizeof(buffer) ||
            std::memcmp(buffer, data.data() + offset, sizeof(buffer)) != 0)
        {
          thread_failures++;
        }
      }
      ps5vfs_fclose(fp);
    });
  }
  for (std::thread& thread : threads)
    thread.join();
  CHECK(thread_failures == 0);
}

/* Writing whole files (the backups): what is stored is what the server's folder then holds, a
 * file takes the place of the one under its name, folders are made as deep as asked, and what is
 * removed is gone. `local_root` is the folder the mount serves; `drop`, when given, cuts the
 * connection between steps. */
static void TestWriting(const std::string& mount_point, const std::string& local_root, const std::function<void()>& drop = {})
{
  std::mt19937 random(11);
  const auto bytes = [&random](size_t size) {
    std::string data(size, '\0');
    for (char& c : data)
      c = static_cast<char>(random());
    return data;
  };
  struct stat st;

  CHECK(ps5vfs_make_folders((mount_point + "/Crux2-backup/2026-10-06_12-00-00/memcards").c_str()) == 0);
  CHECK(stat((local_root + "/Crux2-backup/2026-10-06_12-00-00/memcards").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  /* Again: a folder that is there is no error. */
  CHECK(ps5vfs_make_folders((mount_point + "/Crux2-backup/2026-10-06_12-00-00/memcards").c_str()) == 0);
  CHECK(ps5vfs_make_folders((mount_point + "/Crux2-backup/2026-10-06_12-00-00/sstates").c_str()) == 0);
  if (drop)
    drop();

  const std::string folder = "/Crux2-backup/2026-10-06_12-00-00";
  const std::string card = bytes(5 * 1024 * 1024 + 321), small = bytes(700), other = bytes(90000);
  CHECK(ps5vfs_store((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), card.data(), card.size()) == 0);
  CHECK(ReadLocal(local_root + folder + "/memcards/Mcd001.ps2") == card);
  if (drop)
    drop();
  CHECK(ps5vfs_store((mount_point + folder + "/sstates/SLUS-00000 (0BADF00D).01.p2s").c_str(), small.data(), small.size()) == 0);
  CHECK(ReadLocal(local_root + folder + "/sstates/SLUS-00000 (0BADF00D).01.p2s") == small);
  CHECK(ps5vfs_store((mount_point + folder + "/empty.txt").c_str(), "", 0) == 0);
  CHECK(stat((local_root + folder + "/empty.txt").c_str(), &st) == 0 && st.st_size == 0);

  /* A file takes the place of the one under its name, and no ".part" stays behind. */
  CHECK(ps5vfs_store((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), other.data(), other.size()) == 0);
  CHECK(ReadLocal(local_root + folder + "/memcards/Mcd001.ps2") == other);
  CHECK(stat((local_root + folder + "/memcards/Mcd001.ps2.part").c_str(), &st) != 0);

  /* What was written is listed and read back through the layer. */
  CHECK(ps5vfs_stat((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), &st) == 0 &&
        static_cast<size_t>(st.st_size) == other.size());
  if (FILE* const fp = ps5vfs_fopen((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), "rb"))
  {
    std::string back(other.size(), '\0');
    CHECK(ps5vfs_fread(back.data(), 1, back.size(), fp) == back.size());
    CHECK(back == other);
    ps5vfs_fclose(fp);
  }
  else
  {
    CHECK(!"the stored file opens");
  }
  int listed = 0;
  if (ps5vfs_dir* const dir = ps5vfs_opendir((mount_point + folder).c_str()))
  {
    struct ps5vfs_dirent entry;
    while (ps5vfs_readdir(dir, &entry) == 1)
      listed++;
    ps5vfs_closedir(dir);
  }
  CHECK(listed == 3);

  /* A file too large to hold, written from a source piece by piece (a disc image): the bytes
   * arrive whole and in order; a source that stops leaves no file and says why. */
  {
    const uint64_t image_size = 9 * 1024 * 1024 + 4096;
    const auto pattern = [](uint64_t at) { return static_cast<char>((at * 2654435761u) >> 13); };
    uint64_t asked = 0;
    const auto source = [&](void* buffer, uint32_t length, uint64_t offset) -> int64_t {
      char* const out = static_cast<char*>(buffer);
      for (uint32_t i = 0; i < length; i++)
        out[i] = pattern(offset + i);
      asked += length;
      return length;
    };
    CHECK(ps5vfs_store_from((mount_point + folder + "/image.iso").c_str(), image_size, source) == 0);
    const std::string back = ReadLocal(local_root + folder + "/image.iso");
    bool same = back.size() == image_size;
    for (uint64_t i = 0; same && i < image_size; i++)
      same = back[i] == pattern(i);
    CHECK(same);
    CHECK(asked >= image_size);
    CHECK(stat((local_root + folder + "/image.iso.part").c_str(), &st) != 0);
    CHECK(ps5vfs_remove((mount_point + folder + "/image.iso").c_str(), 0) == 0);

    uint64_t given = 0;
    const auto stopping = [&](void* buffer, uint32_t length, uint64_t offset) -> int64_t {
      if (offset >= 3 * 1024 * 1024)
      {
        errno = EIO;
        return -1;
      }
      std::memset(buffer, 7, length);
      given += length;
      return length;
    };
    CHECK(ps5vfs_store_from((mount_point + folder + "/stopped.iso").c_str(), image_size, stopping) != 0 && errno == EIO);
    CHECK(given > 0);
    CHECK(stat((local_root + folder + "/stopped.iso").c_str(), &st) != 0);
    CHECK(stat((local_root + folder + "/stopped.iso.part").c_str(), &st) != 0);
  }

  /* Into a folder that is not there: refused, not made up. */
  CHECK(ps5vfs_store((mount_point + "/no such folder/file.bin").c_str(), small.data(), small.size()) != 0);
  /* The mount point itself is no file. */
  CHECK(ps5vfs_store(mount_point.c_str(), small.data(), small.size()) != 0);
  /* Outside every mount. */
  CHECK(ps5vfs_store("/nowhere/file.bin", small.data(), small.size()) != 0);
  CHECK(ps5vfs_make_folders("/nowhere/folder") != 0);

  if (drop)
    drop();
  /* Removing: a folder with something in it stays; files go, then their folders. */
  CHECK(ps5vfs_remove((mount_point + folder + "/memcards").c_str(), 1) != 0);
  CHECK(ps5vfs_remove((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), 0) == 0);
  CHECK(ps5vfs_remove((mount_point + folder + "/memcards/Mcd001.ps2").c_str(), 0) == 0); /* not there: no error */
  CHECK(ps5vfs_remove((mount_point + folder + "/memcards").c_str(), 1) == 0);
  CHECK(stat((local_root + folder + "/memcards").c_str(), &st) != 0);
  CHECK(ps5vfs_stat((mount_point + folder + "/memcards").c_str(), &st) != 0);
  CHECK(ps5vfs_remove((mount_point + folder + "/sstates/SLUS-00000 (0BADF00D).01.p2s").c_str(), 0) == 0);
  CHECK(ps5vfs_remove((mount_point + folder + "/sstates").c_str(), 1) == 0);
  CHECK(ps5vfs_remove((mount_point + folder + "/empty.txt").c_str(), 0) == 0);
  CHECK(ps5vfs_remove((mount_point + folder).c_str(), 1) == 0);
  CHECK(ps5vfs_remove((mount_point + "/Crux2-backup").c_str(), 1) == 0);
  CHECK(stat((local_root + "/Crux2-backup").c_str(), &st) != 0);
}

/* A server that closes the connection while the share is in use (a NAS drops one that was idle for
 * some minutes, which a game read into memory leaves it): the next thing asked of the share is
 * answered over a new connection, whatever it is. `relay` is the relay's process: SIGUSR1 makes it
 * close every connection it holds. */
static void TestDrop(const std::string& point, const std::string& root, pid_t relay)
{
  const auto drop = [relay]() {
    CHECK(kill(relay, SIGUSR1) == 0);
    usleep(300000);
  };

  /* A file opened and half read before the connection goes... */
  const std::string local = ReadLocal(root + "/Game One (USA).bin");
  CHECK(local.size() > 2 * 1024 * 1024);
  FILE* const fp = ps5vfs_fopen((point + "/Game One (USA).bin").c_str(), "rb");
  CHECK(fp != nullptr);
  std::string remote(local.size(), '\0');
  const size_t half = local.size() / 2;
  if (fp)
    CHECK(ps5vfs_fread(remote.data(), 1, half, fp) == half);

  /* ...a folder never listed before is listed right after it went... */
  drop();
  int files = 0, folders = 0;
  CompareTree(root + "/games", point + "/games", &files, &folders);
  CHECK(files >= 4 && folders >= 1);

  /* ...the open file reads on to its end... */
  drop();
  if (fp)
  {
    CHECK(ps5vfs_fread(remote.data() + half, 1, local.size() - half, fp) == local.size() - half);
    CHECK(remote == local);
    ps5vfs_fclose(fp);
  }

  /* ...a file is opened... */
  drop();
  FILE* const second = ps5vfs_fopen((point + "/games/sub folder/tiny.bin").c_str(), "rb");
  CHECK(second != nullptr);
  if (second)
  {
    char tiny[4096];
    CHECK(ps5vfs_fread(tiny, 1, sizeof(tiny), second) == 2352);
    CHECK(std::memcmp(tiny, ReadLocal(root + "/games/sub folder/tiny.bin").data(), 2352) == 0);
    ps5vfs_fclose(second);
  }

  /* ...and a path is asked about. And a folder that is not there is still not there. */
  drop();
  struct stat st;
  CHECK(ps5vfs_stat((point + "/Game One (USA).cue").c_str(), &st) == 0 && S_ISREG(st.st_mode));
  drop();
  CHECK(ps5vfs_opendir((point + "/no such folder").c_str()) == nullptr);
  CHECK(ps5vfs_stat((point + "/games/exact.bin").c_str(), &st) == 0 && st.st_size == 256 * 1024 * 2);
}

/* What ps5vfs_describe says of one mount point, or "". */
static std::string Describe(const std::string& point)
{
  char text[4096];
  ps5vfs_describe(text, sizeof(text));
  const std::string all = text;
  const std::string start = point + ": ";
  for (size_t at = 0; at < all.size();)
  {
    size_t end = all.find('\n', at);
    if (end == std::string::npos)
      end = all.size();
    if (all.compare(at, start.size(), start) == 0)
      return all.substr(at + start.size(), end - at - start.size());
    at = end + 1;
  }
  return {};
}

static bool Contains(const std::string& text, const char* part)
{
  return text.find(part) != std::string::npos;
}

/* NFS addresses and network.cfg's NFS lines, with no server: what is refused, the mount points and
 * names, the user and group numbers, and a server that is not there. */
static void TestNFSParsing(const std::string& work)
{
  /* Refused at once: a bad name, not an NFS address, no server, an account, a bad port, an option
   * that is not one, a version that is not one. */
  CHECK(ps5vfs_mount_nfs("bad name", "nfs://127.0.0.1/x") != 0);
  CHECK(ps5vfs_mount_nfs("x", "smb://127.0.0.1/x") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs:///export") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://user@127.0.0.1/export") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1:0/export") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1:70000/export") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1/export?readonly") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1/export?version=5") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1/export?uid=-1") != 0);
  CHECK(ps5vfs_mount_nfs("x", "nfs://127.0.0.1/export?mountport=abc") != 0);
  CHECK(!ps5vfs_is_virtual("/smb/x"));

  /* Taken: the options in any case and order, a port, a root export, slashes made single. */
  CHECK(ps5vfs_mount_nfs("a", "NFS://127.0.0.1:9//volume1//games/?UID=1026&gid=100&version=4.2&mountport=9") == 0);
  CHECK(ps5vfs_mount_nfs("b", "nfs://192.0.2.1") == 0);
  CHECK(ps5vfs_mount_nfs("c", "nfs://127.0.0.1:9/My%20Games?version=3") == 0);
  const std::string a = Describe("/smb/a"), b = Describe("/smb/b"), c = Describe("/smb/c");
  std::fprintf(stderr, "  /smb/a: %s\n  /smb/b: %s\n  /smb/c: %s\n", a.c_str(), b.c_str(), c.c_str());
  CHECK(Contains(a, "nfs://127.0.0.1:9/volume1/games (NFS 4.2, uid 1026, gid 100)"));
  CHECK(Contains(b, "nfs://192.0.2.1/ (NFS 3, or 4, uid 1000, gid 1000)"));
  CHECK(Contains(c, "nfs://127.0.0.1:9/My Games (NFS 3, uid 1000, gid 1000)"));

  /* A server that is not there fails at once, then is not asked again for a while; the mount
   * point is still a folder. */
  struct stat st;
  const double start = ps5vfs::MonotonicSeconds();
  CHECK(ps5vfs_stat("/smb/a", &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(ps5vfs_opendir("/smb/a") == nullptr);
  CHECK(ps5vfs_stat("/smb/a/game.chd", &st) != 0);
  CHECK(ps5vfs_fopen("/smb/a/game.chd", "rb") == nullptr);
  CHECK(ps5vfs_store("/smb/a/new.bin", "x", 1) != 0);
  CHECK(ps5vfs::MonotonicSeconds() - start < 2.0);
  CHECK(Contains(Describe("/smb/a"), "not reachable"));

  /* network.cfg: SMB and NFS lines mixed, in their order, with the file's uid and gid where an NFS
   * line names none, and the SMB account left out of the NFS addresses. */
  const std::string config = work + "/network-nfs-parse.cfg";
  FILE* const fp = fopen(config.c_str(), "w");
  CHECK(fp != nullptr);
  if (!fp)
    return;
  std::fprintf(fp, "# NFS and SMB\n"
                   "path = nfs://127.0.0.1:9/export/PS2\n"
                   "path = 127.0.0.1:9/share/PS2\n"
                   "Path = nfs://127.0.0.1:9/other/PS2/?gid=7\n"
                   "path = nfs://127.0.0.1:9/x/Games?uid=5\n"
                   "path = nfs://127.0.0.1:9/bad?version=9\n"
                   "user = someone\n"
                   "password = secret\n"
                   "uid = 1026\n"
                   "gid = 100\n"
                   "gid = minus\n");
  fclose(fp);
  CHECK(ps5vfs_load_network_config(config.c_str()) == 4);
  const std::string first = Describe("/smb/PS2"), second = Describe("/smb/PS2-2"), third = Describe("/smb/PS2-3"),
                    fourth = Describe("/smb/Games");
  std::fprintf(stderr, "  /smb/PS2: %s\n  /smb/PS2-2: %s\n  /smb/PS2-3: %s\n  /smb/Games: %s\n", first.c_str(),
               second.c_str(), third.c_str(), fourth.c_str());
  CHECK(Contains(first, "nfs://127.0.0.1:9/export/PS2 (NFS 3, or 4, uid 1026, gid 100)"));
  CHECK(Contains(second, "smb://127.0.0.1:9/share/PS2 (user someone)"));
  CHECK(Contains(third, "nfs://127.0.0.1:9/other/PS2 (NFS 3, or 4, uid 1026, gid 7)"));
  CHECK(Contains(fourth, "nfs://127.0.0.1:9/x/Games (NFS 3, or 4, uid 5, gid 100)"));
  CHECK(!Contains(first + third + fourth, "secret") && !Contains(first + third + fourth, "someone"));
  CHECK(ps5vfs_is_virtual("/smb/PS2-3/folder/file.bin"));
}

int main(int argc, char* argv[])
{
  ps5vfs_set_log(LogLine);
  const std::string mode = (argc > 1) ? argv[1] : "";
  if (mode == "memory")
  {
    TestMemory();
    /* A mount that cannot write says so. */
    errno = 0;
    CHECK(ps5vfs_store("/mem/b/new.bin", "x", 1) != 0 && errno == EROFS);
    CHECK(ps5vfs_make_folders("/mem/b/new folder") != 0 && errno == EROFS);
  }
  else if (mode == "ftp-write" && argc == 4)
  {
    CHECK(ps5vfs_mount_ftp("/mnt-write", static_cast<unsigned>(std::atoi(argv[2])), "/") == 0);
    TestWriting("/mnt-write", argv[3]);
  }
  else if ((mode == "smb-write" || mode == "nfs-write") && argc == 4)
  {
    CHECK((mode == "smb-write" ? ps5vfs_mount_smb : ps5vfs_mount_nfs)("write", argv[2]) == 0);
    TestWriting("/smb/write", argv[3]);
  }
  else if ((mode == "smb-write-drop" || mode == "nfs-write-drop") && argc == 5)
  {
    const pid_t relay = static_cast<pid_t>(std::atoi(argv[4]));
    CHECK((mode == "smb-write-drop" ? ps5vfs_mount_smb : ps5vfs_mount_nfs)("write", argv[2]) == 0);
    TestWriting("/smb/write", argv[3], [relay]() {
      CHECK(kill(relay, SIGUSR1) == 0);
      usleep(300000);
    });
  }
  else if ((mode == "smb-readonly" || mode == "nfs-readonly") && argc == 3)
  {
    /* A share the server lets nobody write to: refused, and said so. */
    CHECK((mode == "smb-readonly" ? ps5vfs_mount_smb : ps5vfs_mount_nfs)("ro", argv[2]) == 0);
    errno = 0;
    CHECK(ps5vfs_store("/smb/ro/new.bin", "x", 1) != 0);
    CHECK(errno == EACCES || errno == EROFS || errno == EIO || errno == EPERM);
    CHECK(ps5vfs_make_folders("/smb/ro/new folder") != 0);
  }
  else if (mode == "ftp" && argc == 4)
  {
    CHECK(ps5vfs_mount_ftp("/mnt-test", static_cast<unsigned>(std::atoi(argv[2])), "/") == 0);
    CHECK(ps5vfs_mount_ftp("/mnt-none", 1, "/") != 0);
    TestRemote("/mnt-test", argv[3]);
  }
  else if (mode == "smb" && argc == 4)
  {
    CHECK(ps5vfs_mount_smb("bad name", argv[2]) != 0);
    CHECK(ps5vfs_mount_smb("nourl", "ftp://x/y") != 0);
    CHECK(ps5vfs_mount_smb("noshare", "smb://127.0.0.1") != 0);
    CHECK(ps5vfs_mount_smb("test", argv[2]) == 0);
    TestRemote("/smb/test", argv[3]);

    /* A server that is not there fails within its timeout, and at once the second time. */
    CHECK(ps5vfs_mount_smb("down", "smb://127.0.0.1:9/none") == 0);
    struct stat st;
    const double start = ps5vfs::MonotonicSeconds();
    CHECK(ps5vfs_stat("/smb/down", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(ps5vfs_opendir("/smb/down") == nullptr);
    CHECK(ps5vfs_stat("/smb/down/game.chd", &st) != 0);
    CHECK(ps5vfs_fopen("/smb/down/game.chd", "rb") == nullptr);
    CHECK(ps5vfs::MonotonicSeconds() - start < 12.0);
  }
  else if ((mode == "smb-drop" || mode == "nfs-drop") && argc == 5)
  {
    CHECK((mode == "smb-drop" ? ps5vfs_mount_smb : ps5vfs_mount_nfs)("drop", argv[2]) == 0);
    TestDrop("/smb/drop", argv[3], static_cast<pid_t>(std::atoi(argv[4])));
  }
  else if (mode == "nfs" && argc == 4)
  {
    CHECK(ps5vfs_mount_nfs("test", argv[2]) == 0);
    TestRemote("/smb/test", argv[3]);
    const std::string described = Describe("/smb/test");
    std::fprintf(stderr, "  /smb/test: %s\n", described.c_str());
    CHECK(Contains(described, "connected (NFS "));

    /* A server that is not there fails within its time limit, and at once the second time. */
    CHECK(ps5vfs_mount_nfs("down", "nfs://127.0.0.1:9/none") == 0);
    struct stat st;
    const double start = ps5vfs::MonotonicSeconds();
    CHECK(ps5vfs_stat("/smb/down", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(ps5vfs_opendir("/smb/down") == nullptr);
    CHECK(ps5vfs_stat("/smb/down/game.chd", &st) != 0);
    CHECK(ps5vfs_fopen("/smb/down/game.chd", "rb") == nullptr);
    CHECK(ps5vfs::MonotonicSeconds() - start < 12.0);

    /* An export that is not there: refused, not taken for a server that is down. */
    CHECK(ps5vfs_mount_nfs("missing", (std::string(argv[2]).substr(0, std::string(argv[2]).find('?')) +
                                       "/no such folder").c_str()) == 0);
    CHECK(ps5vfs_opendir("/smb/missing") == nullptr);
  }
  else if (mode == "nfs-parse" && argc == 3)
  {
    TestNFSParsing(argv[2]);
  }
  else if (mode == "config" && argc == 5)
  {
    /* network.cfg: one share that is there, as /smb/<argv[3]>, and its account from the file. */
    CHECK(ps5vfs_load_network_config("/nonexistent/network.cfg") == -1);
    CHECK(ps5vfs_load_network_config(argv[2]) == 2);
    const std::string point = std::string("/smb/") + argv[3];
    CHECK(ps5vfs_is_virtual(point.c_str()));
    CHECK(ps5vfs_is_virtual((point + "-2").c_str()));
    TestRemote(point.c_str(), argv[4]);
  }
  else
  {
    std::fprintf(stderr, "usage: vfs_test memory | ftp <port> <local-root> | smb <url> <local-root> | "
                         "nfs <url> <local-root> | nfs-parse <work folder> | "
                         "config <network.cfg> <mount name> <local-root>\n");
    return 2;
  }

  char description[1024];
  ps5vfs_describe(description, sizeof(description));
  std::fprintf(stderr, "%s", description);
  uint64_t bytes = 0, reads = 0;
  ps5vfs_stats(nullptr, &bytes, &reads);
  std::fprintf(stderr, "%s: %d checks, %d failed (%llu bytes in %llu backend reads)\n", mode.c_str(), s_checks,
               s_failures, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(reads));
  return s_failures ? 1 : 0;
}
