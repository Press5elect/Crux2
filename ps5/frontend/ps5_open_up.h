// Crux2 - opening up the files the title keeps, for an FTP client and for the title's other self
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Outside its sandbox the title runs as another user than inside it, and an FTP server as yet
// another: a memory card or a settings file written by one must be writable by the others. So
// everything the title keeps is made readable and writable for all; what the build shipped and
// the user's games are left as they are.
//
// A file is looked at first and changed only when it has to be: a change is a write to the disk,
// and a library's covers are thousands of files. v0.8.0 changed every one of them before the
// first picture and again while closing, which was 8 s each time on a console (2026-10-07).
//
// Header-only and without anything of the console's, so that ps5/tests/openup runs it on a PC.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace PS5OpenUp
{
struct Opened
{
  unsigned seen = 0;    ///< files and folders looked at
  unsigned changed = 0; ///< of those, opened up now
  unsigned failed = 0;  ///< of those, could not be
};

inline constexpr const char* SKIPPED_FOLDERS[] = {"resources", "games", "sce_sys", "sce_module", "licenses"};
inline constexpr const char* SKIPPED_FILES[] = {"eboot.bin", "sandbox-elevator.elf", "README.md", "LEGAL.txt", "symbols.txt"};
/// What a run writes to: the folders that are gone through when the title closes.
inline constexpr const char* SESSION_FOLDERS[] = {"inis", "gamesettings", "memcards", "sstates", "logs", "card-undo",
                                                  "cheats", "patches", "inputprofiles"};

inline bool IsOneOf(const char* name, const char* const* list, size_t count)
{
  for (size_t i = 0; i < count; i++)
  {
    if (std::strcmp(name, list[i]) == 0)
      return true;
  }
  return false;
}

/// One file or folder, already looked at: readable and writable for all (a folder also entered).
inline void One(const char* path, const struct stat& st, Opened* count)
{
  const mode_t wanted = S_ISDIR(st.st_mode) ? 0777 : 0666;
  count->seen++;
  if ((st.st_mode & wanted) == wanted)
    return;
  if (chmod(path, wanted) == 0)
    count->changed++;
  else
    count->failed++;
}

/// Everything in a folder and below it. A link is neither changed nor followed. stop, when it
/// is set by another thread, ends the walk at the next file.
inline void Below(const std::string& folder, int depth, const std::atomic_bool* stop, Opened* count)
{
  DIR* const dir = opendir(folder.c_str());
  if (!dir)
    return;
  std::vector<std::string> below;
  while (const struct dirent* const entry = readdir(dir))
  {
    if (stop && stop->load(std::memory_order_relaxed))
      break;
    const char* const name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
      continue;
    std::string path = folder;
    path += '/';
    path += name;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
      continue;
    One(path.c_str(), st, count);
    if (S_ISDIR(st.st_mode) && depth < 16)
      below.push_back(std::move(path));
  }
  closedir(dir);
  for (const std::string& one : below)
  {
    if (stop && stop->load(std::memory_order_relaxed))
      break;
    Below(one, depth + 1, stop, count);
  }
}

/// A title folder (or the folder outside it). session_only: the files at its top and the folders
/// a run writes to, for the end of a run; otherwise everything but what the build shipped and the
/// games, the folders a run writes to first.
inline void In(const char* root, bool session_only, const std::atomic_bool* stop, Opened* count)
{
  DIR* const dir = opendir(root);
  if (!dir)
    return;
  std::vector<std::string> first, later;
  while (const struct dirent* const entry = readdir(dir))
  {
    const char* const name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
      continue;
    const std::string path = std::string(root) + "/" + name;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
      continue;
    const bool directory = S_ISDIR(st.st_mode);
    const bool session = directory && IsOneOf(name, SESSION_FOLDERS, std::size(SESSION_FOLDERS));
    if (directory ? (IsOneOf(name, SKIPPED_FOLDERS, std::size(SKIPPED_FOLDERS)) || (session_only && !session)) :
                    IsOneOf(name, SKIPPED_FILES, std::size(SKIPPED_FILES)))
      continue;
    One(path.c_str(), st, count);
    if (directory)
      (session ? first : later).push_back(path);
  }
  closedir(dir);
  for (const std::vector<std::string>* const list : {&first, &later})
  {
    for (const std::string& one : *list)
    {
      if (stop && stop->load(std::memory_order_relaxed))
        return;
      Below(one, 1, stop, count);
    }
  }
}
} // namespace PS5OpenUp
