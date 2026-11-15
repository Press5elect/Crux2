// Crux2 - game fixes from GitHub at start: the game database entries and game patches that the
// repository's main branch has, fetched when they differ from the title folder's, so a fix for a
// game reaches the console without a new version of Crux2.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// What a fix is. Crux2 ships its own game fixes in the title folder's resources/game-fixes/ (from
// ps5/package/resources/game-fixes/ of the repository), beside PCSX2's own game database:
//
//     gamedb/<name>.yaml     entries in the form of PCSX2's GameIndex.yaml; a game named here is
//                            taken from here, not from GameIndex.yaml (pcsx2/GameDatabase.cpp)
//     patches/<name>.pnach   game patches named as PCSX2 names them (<serial>_<crc>.pnach), read
//                            with those of the patches folder (pcsx2/Patch.cpp); a file of the
//                            same name in the patches folder is the user's and wins
//
// The repository's ps5/game-fixes.txt (written by ps5/tools/game-fixes-index.py) lists them all:
//
//     # Crux2 game fixes 1
//     <sha256> <bytes> <path below game-fixes/>
//
// Sync() reads it and, for each file whose copy here (the fetched one, else the shipped one) has
// another size or SHA-256, fetches it from the same branch, checks it against the list and puts it
// in the fixes folder of the data folder (<data folder>/game-fixes/, beside the settings: the
// title's resources are left as the package has them). A file it fetched before that the list no
// longer has (a fix withdrawn) is removed; nothing else is. What was fetched is recorded in
// game-fixes/fetched.txt, one path a line. Nothing fetched is run: these are data the emulator
// reads when a game starts. Paths other than the two forms above are refused.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5Fixes
{
/// GET an address: the HTTP status (less than 0 when there was no answer at all) and the body.
using Getter = std::function<int(const std::string& url, std::vector<uint8_t>* body)>;

struct Report
{
	bool ok = false;         // the list was read and every file it names is in place
	std::string error;       // why not, a sentence for the user (the log has the rest)
	unsigned listed = 0, fetched = 0, removed = 0, failed = 0;
	std::vector<std::string> games; // the serials whose fixes came or went, for the notice
};

/// The list's first line.
inline constexpr const char* LIST_HEADER = "# Crux2 game fixes 1";

/// The work, on any thread that may block. `list_url` is the list's address, `files_url` the
/// address its paths are below ("https://.../main/ps5/package/resources/game-fixes/"); `shipped`
/// is the title folder's resources/game-fixes, `fixes` the folder fetched files go in.
bool Sync(const std::string& list_url, const std::string& files_url, const std::string& shipped, const std::string& fixes,
	const Getter& get, Report* report);

/// A path of the list that may be fetched: gamedb/<name>.yaml or patches/<name>.pnach, the name of
/// letters, digits, '.', '_' and '-' and not starting with '.'.
bool SafePath(const std::string& path);

/// The SHA-256 of some bytes, as 64 lower-case hex digits.
std::string Sha256Hex(const void* data, size_t size);

/// A whole file written beside its place and then put there, never renamed onto the old one (which
/// is moved aside first and put back if the new one does not go in): the file holds exactly these
/// bytes afterwards, or the one that was there is as it was. (Also the recommended settings',
/// ps5_recommend.cpp.)
bool WriteOverSafely(const std::string& path, const void* data, size_t size);

/// At start, when Settings > PlayStation 5 > "Game fixes from GitHub at start" ([PS5]
/// FixesAtStart, on by default) says so: Sync() on a thread of its own, once a run, from
/// Press5elect/Crux2's main branch ([PS5] UpdateRepository names another). Fixes that came apply to
/// games started from then on; the user is told which games got some.
void Start();
} // namespace PS5Fixes
