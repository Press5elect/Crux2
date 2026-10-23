// Crux2 - what the interface asks for that takes a while: backups of the memory cards and the
// saved states, the logs' copy, the download of PCSX2's patches. (ps5_jobs.cpp)
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Each is started from the thread that draws and returns at once; the work goes on on a thread
// of its own, and what came of it is said in a toast or a message (PS5UI). The engines know
// nothing of the console (ps5_backup.cpp, ps5_logs.cpp, ps5_online.cpp); this is where they
// meet the emulator's folders and settings.

#pragma once

#include "ps5_backup.h"
#include "ps5_cards.h"
#include "ps5_logs.h"

#include <functional>
#include <string>
#include <vector>

namespace PS5Jobs
{
/// A place backups and logs can go to: a USB drive that is in, or a network share.
struct Place
{
	std::string path; // "/mnt/usb0", "/smb/games"
	std::string name; // "USB drive 1", "Share: games"
};
/// The places there are now. Cheap (a few stats of the console's own folders): the shares are
/// not asked whether they answer.
std::vector<Place> Places();
/// The place backups go to: the one chosen in the settings ([PS5] BackupPlace) while it is
/// there, else the first; an empty path when there is none.
Place BackupPlace();

/// Each returns false, with a toast saying why, when it could not be started.
bool BackupNow();
bool Restore(const PS5Backup::Snapshot& snapshot);
/// Reads the list of backups of BackupPlace() again, quietly.
void ListBackups();
/// A game was closed: with "Back up memory cards when a game is closed" on, and the cards other
/// than at the last automatic backup, makes one; with "Keep the memory cards in sync" on, syncs
/// them (ps5_card_sync.h). From the emulation thread.
void GameClosed();

/// The memory cards both ways with "<backup place>/Crux2/memory cards" (ps5_card_sync.h), on a
/// thread of its own; never while a game runs. `quiet`: a toast only when something was copied
/// or went wrong (the sync when the title starts and after a game). Does nothing when one runs.
void SyncCards(bool quiet);

PS5Logs::Files LogFiles();
void CopyLogs(const Place& place);
/// Once, when the title starts: whether the run before ended in a crash. Empty when it did not.
std::string CrashNotice();

// ---- the saves on the memory cards (ps5_cards.cpp). A card is changed on a thread of its own,
// one change at a time, and only while no game is loaded or being started; what came of it is
// said in a toast or a message, and the pages are made again.

/// A card's file, the folder the cards as they were before their last change are kept in, and the
/// folder of the title's own that save files are brought in from and written out to.
std::string CardPath(const std::string& name);
std::string CardUndoFolder();
std::string SavesFolder();
/// A save file found in a place saves are brought in from.
struct FoundSave
{
	std::string path, name, where;
	uint64_t size = 0;
};
/// Looks into SavesFolder() and into saves/ of every place. Returns at once; `then` is called on
/// the thread that draws, with what was found.
void FindSaveFiles(std::function<void(std::vector<FoundSave>)> then);
/// Where a save can be written out to: SavesFolder() first, then saves/ of every place.
std::vector<Place> ExportPlaces();
void ImportSave(const std::string& card, const std::string& file);
void ExportSave(const std::string& card, const std::string& save, const Place& place);
void CopySave(const std::string& card, const std::string& save, const std::string& to_card);
void DeleteSave(const std::string& card, const std::string& save);
void UndoCard(const std::string& card);

// ---- the card manager of v1.5.0: where saves and whole cards are brought in from

/// A place to look in: a PlayStation 2 on the network (its FTP server: wLaunchELF's), Crux2's own
/// saves folder, a USB drive, a share.
struct CardSource
{
	bool ps2 = false;
	std::string path; // a folder; for a PlayStation 2 its address ("192.168.1.20")
	std::string name; // "USB drive 1", "A PlayStation 2 on the network"
};
std::vector<CardSource> CardSources();

/// What was found there.
struct CardThing
{
	enum Kind
	{
		SaveFile,   // .psu .max .cbs .sps .xps .psv
		SaveFolder, // a save copied off a card as it is (a folder with an icon.sys)
		CardImage,  // a whole card: .ps2 .mcd .mc2 .bin .vmc .vm2
		Ps2Save,    // a save on a real PlayStation 2's card
	};
	Kind kind = SaveFile;
	std::string path, name, where;
	uint64_t size = 0;
	std::string ps2; // the PlayStation 2's address, for Ps2Save
};
/// Looks at a source: in a folder, the folder itself and saves/, Crux2/saves, PS2/SAVES,
/// memcards/, Crux2/memory cards, VMC/ (Open PS2 Loader's cards) and PS2/VMC, one level of
/// folders deep for saves copied as folders. Returns at once; `then` on the thread that draws,
/// with what was found or why nothing could be.
void FindIn(const CardSource& source, std::function<void(std::vector<CardThing>, std::string why)> then);

/// Brings a save (any kind but CardImage) onto a card.
void BringIn(const std::string& card, const CardThing& thing);
/// Brings a card image in as a new card of the given name (without its ending).
void BringInCard(const CardThing& image, const std::string& title);
/// A save of a card image brought onto one of Crux2's cards.
void CopyFromImage(const std::string& image, const std::string& save, const std::string& to_card);
/// A new card: 8, 16, 32 or 64 MB, or 0 for a PlayStation 1 card (made by PCSX2).
void CreateCard(const std::string& title, int megabytes);
void FormatCard(const std::string& card);
/// The card's file copied out whole, as <its name>.ps2, to Crux2/memory cards at a place.
void ExportCard(const std::string& card, const Place& place);
/// A copy of a card's file beside it, named "<its name> copy".
void DuplicateCard(const std::string& card);

void DownloadPatches();
/// The covers both ways with the backup place (a share or a USB drive), in its folder
/// Crux2/covers: what one side has and the other has not is copied over, so that every console
/// (and the PC) that uses the place has all of them. On a thread of its own; a toast says what came of it.
void ShareCovers();
/// A game's title screen and picture of play, from libretro's collection, whatever was found for
/// it before. (Those of every game are fetched after a scan: ps5_host.cpp.)
void FetchPictures(const std::string& serial);
} // namespace PS5Jobs
