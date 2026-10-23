// Crux2 - the memory cards kept the same on the console and in a folder on a share or a USB drive,
// both ways ([PS5] CardSync). The engine: it knows nothing of the console (ps5_jobs.cpp gives it
// the folders and the file access), so the tests run it on a PC (ps5/tests/cardsync).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The folder on the other side is "<place>/Crux2/memory cards". The same .ps2 files can be used
// there by PCSX2 on a PC, or by ARMSX2 and the like on Android (through Syncthing or a share).
//
// How it decides, for each card file (".ps2") on either side. What each side's file was (its size
// and modification time) after the last sync is remembered, in a list of the console's own:
//   - a card on one side only: copied to the other (a card is never taken away: one deleted on
//     one side comes back from the other);
//   - changed on one side since the last sync: that side's copy goes to the other;
//   - changed on both sides: the newer one is kept on both, and the other is not lost: it is put in
//     the "older" folder of its side first ("<data>/older memory cards" on the console,
//     "<place>/Crux2/memory cards/.older" there), named with the moment it was put away;
//   - on both sides and never synced before: the same bytes are simply remembered; different ones
//     are handled as changed on both.
// Save states are not synced: they belong to one emulator's build.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace PS5CardSync
{
struct FileInfo
{
	bool exists = false;
	uint64_t size = 0;
	int64_t mtime = 0;
};

/// The file access, for both sides (a path says which: the console's own, or a share's).
struct Io
{
	std::function<bool(const std::string& folder, std::vector<std::string>* names)> list; // files only
	std::function<FileInfo(const std::string& path)> stat;
	std::function<bool(const std::string& path, std::string* data)> read;
	/// The file holds exactly these bytes afterwards, or is as it was.
	std::function<bool(const std::string& path, const std::string& data)> write;
	std::function<bool(const std::string& folder)> make_folder;
	/// Asked before each change to the console's side: true stops the sync there (a game started).
	std::function<bool()> stop;
};

/// What each side's file was after the last sync.
struct Record
{
	uint64_t here_size = 0;
	int64_t here_mtime = 0;
	uint64_t there_size = 0;
	int64_t there_mtime = 0;
};
using Records = std::map<std::string, Record>;

std::string FormatRecords(const Records& records);
Records ParseRecords(const std::string& text);

struct Folders
{
	std::string here;        // the console's memory cards
	std::string there;       // "<place>/Crux2/memory cards"
	std::string older_here;  // "<data>/older memory cards"
	std::string older_there; // "<there>/.older"
};

struct Result
{
	bool listed = false; // the other side could be listed (false: nothing was done)
	bool stopped = false;
	int sent = 0, brought = 0, both_changed = 0, failed = 0, unchanged = 0;
	std::vector<std::string> lines; // one for each card that was copied or could not be, for the log
};

/// A memory card file this syncs: ".ps2", not a folder card.
bool IsCard(const std::string& name);

/// One sync. `records` is read and brought up to date. `now` names what is put away.
Result Sync(const Io& io, const Folders& folders, Records* records, time_t now);

/// A sentence for a toast: "Memory cards synced with Share: games: 2 sent, 1 brought here."
std::string Summary(const Result& result, const std::string& place_name);
} // namespace PS5CardSync
