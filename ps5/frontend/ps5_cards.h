// Crux2 - the saves on the memory cards: looked at, brought in from a file, copied out to one,
// copied to another card, deleted. (ps5_cards.cpp)
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The engine of the card manager in the settings (Memory cards). It works on card files by their
// paths and knows nothing of the console or the emulator, so the PC tests it
// (ps5/tests/cards/cards_engine_test.cpp); the card's file system itself is ps5_mc.h.
//
// How a card is changed: its whole file is read, the change is made in memory and the result is
// checked to read as a sound card (ps5_mc.h refuses otherwise); the card as it was is written to
// the "undo" folder under its own name, and only then the new file takes the old one's place, in
// one step (written beside it, then renamed). A card that does not read as sound is never written
// to. Whoever calls sees to it that no game runs: a running PlayStation 2 has the card's layout in
// its own memory and would write over the change.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5Cards
{
struct SaveInfo
{
	std::string name;   // its folder on the card: "BESLES-12345SAVE"
	std::string title;  // what it calls itself; empty when it says nothing a Latin font shows
	std::string when;   // "2026-10-07 14:32", the clock of Japan, as the card has it
	uint32_t bytes = 0; // what it takes of the card
	int files = 0;
	bool ps1 = false;   // a PlayStation 1 save kept on a PlayStation 2 card
};

struct CardInfo
{
	enum State
	{
		Listed,   // its saves are in `saves`
		New,      // never formatted (as PCSX2 makes a new card): empty, and made ready by the first save brought in
		Unknown,  // it cannot be looked into; `why` says why
	};
	State state = Unknown;
	std::string why;
	uint32_t free_bytes = 0, total_bytes = 0;
	std::vector<SaveInfo> saves;
};

/// What is on a card.
CardInfo Look(const std::string& card);

/// Whether a file's name is that of a save file this reads: .psu, .max, .cbs, .sps, .xps, .psv.
bool IsSaveFile(const std::string& name);

struct SaveFile
{
	std::string path;
	std::string name; // the file's
	uint64_t size = 0;
};
/// The save files of a folder, by name. An empty list when there is no such folder.
std::vector<SaveFile> SaveFiles(const std::string& folder);

struct Result
{
	bool ok = false;
	std::string message; // a sentence for the user, whatever came of it
};

/// `undo`: the folder the card as it was before is kept in (one copy for each card, the last
/// change's); empty: none is kept.
Result Import(const std::string& card, const std::string& file, const std::string& undo);
/// Writes a save out as <its folder's name>.psu into `folder` (made when it is not there); a
/// file of that name there already is left, and the new one is numbered.
Result Export(const std::string& card, const std::string& save, const std::string& folder);
Result Copy(const std::string& from, const std::string& save, const std::string& to, const std::string& undo);
Result Delete(const std::string& card, const std::string& save, const std::string& undo);

/// Whether the card as it was before its last change is kept, and putting it back (the card as it
/// is now is kept in its place: doing it twice is where one began).
bool HasUndo(const std::string& card, const std::string& undo);
Result Undo(const std::string& card, const std::string& undo);

/// A game's start and a card's change must not cross: the PlayStation 2 would read a card that is
/// replaced under it. `allowed` is asked just before a card's file is replaced, with no other
/// change being written; false leaves the card as it was ("a game was started meanwhile").
/// Whoever starts a game makes `allowed` answer false first, then calls WaitForWrites, and only
/// then lets the emulator open the cards.
void SetWriteGuard(std::function<bool()> allowed);
void WaitForWrites();

/// "7.1 MB", "120 KB".
std::string Size(uint64_t bytes);

// ---- whole cards (v1.5.0)

/// A file that may be a whole card (a card image another program or a PlayStation 2 tool made):
/// .ps2, .mcd, .mc2, .bin, .vmc or .vm2, of a card's size (8, 16, 32 or 64 MB, with the
/// error-correction bytes or without).
bool IsCardImageName(const std::string& name);
bool IsCardImageSize(uint64_t size);

/// Brings a card image in as the new card `to` (a file that must not be there yet): checked to
/// read as a card (or to be one nobody formatted), and given the error-correction bytes PCSX2
/// keeps when it has none (a card of the PlayStation 3, .vm2, and most dumps of a real card).
Result ImportCard(const std::string& from, const std::string& to);
/// Writes a card's file out into `folder` as "<its name>.ps2", numbered when that is taken.
Result ExportCard(const std::string& card, const std::string& folder);
/// A new card of 8, 16, 32 or 64 MB at `path` (which must not be there yet), formatted.
Result Create(const std::string& path, int megabytes);
/// Erases every save on a card: it is made empty, of the same size.
Result Format(const std::string& card, const std::string& undo);

/// A save that travels as a folder of files: copied off a PlayStation 2 to a USB drive as it is on
/// the card (wLaunchELF's copy), or read from a PlayStation 2 over the network.
struct FolderFile
{
	std::string name;
	std::string data;
};
Result ImportFolder(const std::string& card, const std::string& folder_name, const std::vector<FolderFile>& files, const std::string& undo);
/// Whether a folder's files look like a save's: an icon.sys among them.
bool LooksLikeSaveFolder(const std::vector<std::string>& file_names);
} // namespace PS5Cards
