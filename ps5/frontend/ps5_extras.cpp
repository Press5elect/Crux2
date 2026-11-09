// Crux2 - comforts of v0.5.0 that reach into the emulator (ps5_extras.h says what each is).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// None of this has run on a console when it was written: the deciding parts are in
// ps5_extras_logic.h and tested on the PC, and what is here says what it did in crux2-boot.log
// ("card:", "textures:", "auto-save:" lines).

#include "ps5_extras.h"

#include "ps5_extras_logic.h"
#include "ps5_jobs.h"
#include "ps5_platform.h"
#include "ps5_ui.h"

#include "Achievements.h"
#include "CDVD/CDVD.h"
#include "Config.h"
#include "GameDatabase.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Sio.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include <atomic>
#include <mutex>

namespace
{
// ---- cards
std::mutex s_groups_mutex;
bool s_groups_made = false;
PS5Extras::CardGroups s_groups;
std::string s_card_said; // the card the log last named, so that it names each once

// ---- texture packs
std::mutex s_pack_mutex;
std::string s_pack_serial, s_pack_folder; // where the running game's pack was found

// ---- the state saved by itself
std::atomic_bool s_clock_reset{true};

// Whether a folder is there and holds anything at all.
bool HoldsAnything(const std::string& folder)
{
	if (!FileSystem::DirectoryExists(folder.c_str()))
		return false;
	FileSystem::FindResultsArray found;
	FileSystem::FindFiles(folder.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &found);
	return !found.empty();
}

// The pack of a game on the drives (and the shares): empty when there is none.
std::string PackElsewhere(const std::string& serial, bool shares)
{
	std::vector<std::string> places;
	for (const PS5Jobs::Place& place : PS5Jobs::Places())
	{
		if (shares || place.path.compare(0, 5, "/smb/") != 0)
			places.push_back(place.path);
	}
	for (const std::string& folder : PS5Extras::TexturePackCandidates(serial, places))
	{
		if (HoldsAnything(folder))
			return folder;
	}
	return std::string();
}
} // namespace

// ------------------------------------------------------------- a memory card for each game

std::string PS5Extras::CardNameFor(const std::string& serial)
{
	if (serial.empty() || GameDatabase::findGame(serial) == nullptr)
		return std::string();
	const std::lock_guard lock(s_groups_mutex);
	if (!s_groups_made)
	{
		s_groups_made = true;
		GameDatabase::forEachMemcardFilters(
			[](const std::string& game, const std::vector<std::string>& filters) { s_groups.Add(game, filters); });
	}
	const std::string key = s_groups.KeyOf(serial);
	return key.empty() ? std::string() : key + ".ps2";
}

void PS5Extras::CardForGame(std::unique_ptr<INISettingsInterface>* layer, const std::string& serial)
{
	if (layer == nullptr || serial.empty() || !Host::GetBaseBoolSettingValue("PS5", "CardPerGame", true))
		return;
	// What the game's own settings say of slot 1 stands, and so does a slot 1 left empty for
	// every game.
	if (*layer && ((*layer)->ContainsValue("MemoryCards", "Slot1_Filename") || (*layer)->ContainsValue("MemoryCards", "Slot1_Enable")))
		return;
	if (!Host::GetBaseBoolSettingValue("MemoryCards", "Slot1_Enable", true))
		return;

	const std::string name = CardNameFor(serial);
	if (name.empty())
		return;
	// (A PlayStation 2 card is for a PlayStation 2 game: a PlayStation 1 disc keeps the slots as
	// they are set.)
	CDVDDiscType type = CDVDDiscType::Other;
	cdvdGetDiscInfo(nullptr, nullptr, nullptr, nullptr, nullptr, &type);
	if (type != CDVDDiscType::PS2Disc)
		return;

	if (!FileMcd_GetCardInfo(name).has_value())
	{
		if (!FileMcd_CreateNewCard(name, MemoryCardType::File, MemoryCardFileType::PS2_8MB))
		{
			ps5px_log("card: %s could not be made; the game keeps the card of the settings", name.c_str());
			return;
		}
		PS5Platform::MakePublic(Path::Combine(EmuFolders::MemoryCards, name).c_str(), false);
		ps5px_log("card: made %s, the card of %s", name.c_str(), serial.c_str());
	}

	if (!*layer)
		*layer = std::make_unique<INISettingsInterface>(std::string());
	(*layer)->SetStringValue("MemoryCards", "Slot1_Filename", name.c_str());

	const std::lock_guard lock(s_groups_mutex);
	if (s_card_said != name)
	{
		s_card_said = name;
		ps5px_log("card: slot 1 holds %s for %s", name.c_str(), serial.c_str());
	}
}

// -------------------------------------------------------------------------- texture packs

std::string PS5Extras::TexturePackFolder(const std::string& serial, const std::string& own)
{
	std::string found;
	if (!HoldsAnything(own))
		found = PackElsewhere(serial, true);
	{
		const std::lock_guard lock(s_pack_mutex);
		s_pack_serial = serial;
		s_pack_folder = found.empty() ? (HoldsAnything(own) ? own : std::string()) : found;
	}
	if (!found.empty())
	{
		ps5px_log("textures: the pack of %s is read from %s", serial.c_str(), found.c_str());
		return found;
	}
	return own;
}

std::string PS5Extras::TexturePackZip(const std::string& serial, const std::string& own)
{
	if (FileSystem::FileExists(own.c_str()))
		return own;
	std::vector<std::string> places;
	for (const PS5Jobs::Place& place : PS5Jobs::Places())
		places.push_back(place.path);
	for (const std::string& zip : PS5Extras::TexturePackZipCandidates(serial, places))
	{
		if (FileSystem::FileExists(zip.c_str()))
		{
			ps5px_log("textures: the pack of %s is read from %s", serial.c_str(), zip.c_str());
			return zip;
		}
	}
	return own;
}

std::string PS5Extras::TexturePackPlace(const std::string& serial, bool shares)
{
	if (serial.empty())
		return std::string();
	{
		// (What the running game was given, a share's pack too, is known without asking again.)
		const std::lock_guard lock(s_pack_mutex);
		if (s_pack_serial == serial && !s_pack_folder.empty())
			return s_pack_folder;
	}
	const std::string own = Path::Combine(Path::Combine(EmuFolders::Textures, serial), "replacements");
	if (HoldsAnything(own))
		return own;
	return PackElsewhere(serial, shares);
}

// ------------------------------------------------------------- a state saved by itself

void PS5Extras::GameStarting()
{
	s_clock_reset.store(true, std::memory_order_release);
	const std::lock_guard lock(s_pack_mutex);
	s_pack_serial.clear();
	s_pack_folder.clear();
}

void PS5Extras::FrameTick()
{
	// (Only the emulation thread comes here.)
	static PS5Extras::PlayClock clock;
	static Common::Timer timer;
	static unsigned frames = 0;
	static int minutes = 0;
	static bool sleep_safe = true;
	static int menu_before = 0;

	if (s_clock_reset.exchange(false, std::memory_order_acq_rel))
	{
		clock.Reset();
		menu_before = 0;
	}
	// The settings are asked every other second: they are behind a lock.
	if ((frames % 120) == 0)
	{
		minutes = std::clamp(Host::GetBaseIntSettingValue("PS5", "AutoSaveMinutes", 0), 0, 120);
		sleep_safe = Host::GetBaseBoolSettingValue("PS5", "SleepSafe", true);
	}

	// Sleep-safe saving: when the console's menu comes over the game (the PS button: the console
	// may be put to rest or the title closed from there, and the game is gone without a word), and
	// every five minutes when no other time is set, the state is saved and a note left that says
	// which game it was. Closing the game takes the note away; one that is there at the next start
	// was left by a title the console closed, and the library offers to go on (ps5_jobs.cpp).
	const char* why = nullptr;
	if (sleep_safe && (frames % 30) == 0)
	{
		const int menu = PS5Platform::SystemMenuOver();
		if (menu == 1 && menu_before != 1)
			why = "the console's menu opened";
		menu_before = menu;
	}
	frames++;
	const double every = (minutes > 0) ? minutes * 60.0 : (sleep_safe ? 300.0 : 0.0);
	if (why == nullptr && !clock.Tick(timer.GetTimeSeconds(), every))
		return;
	if (VMManager::GetState() != VMState::Running)
		return;
	// Hardcore mode has no saved states; and a state made while a memory card is written to
	// would hold half a save.
	if (Achievements::IsHardcoreModeActive())
	{
		clock.Saved();
		return;
	}
	if (MemcardBusy::IsBusy())
	{
		clock.Later(5.0, every);
		return;
	}
	clock.Saved();
	const std::string path = VMManager::GetCurrentSpecialSaveStateFileName("auto");
	if (path.empty())
		return;
	Common::Timer took;
	VMManager::SaveState(path.c_str(), true, false,
		[](const std::string& failure) { ps5px_log("auto-save: the state could not be saved: %s", failure.c_str()); });
	if (why != nullptr)
		ps5px_log("auto-save: saved because %s; the game stood still for %.0f ms", why, took.GetTimeMilliseconds());
	else
		ps5px_log("auto-save: saved after %d minute%s of play; the game stood still for %.0f ms", minutes > 0 ? minutes : 5,
			(minutes == 1) ? "" : "s", took.GetTimeMilliseconds());
	if (sleep_safe)
	{
		// The note: the game's file and the state's.
		const std::string note = "game=" + VMManager::GetDiscPath() + "\nserial=" + VMManager::GetDiscSerial() + "\nstate=" + path + "\n";
		PS5Platform::WriteFileSafely(InterruptedNotePath().c_str(), note.data(), note.size());
	}
	PS5UI::StatesChanged();
}

std::string PS5Extras::InterruptedNotePath()
{
	return Path::Combine(EmuFolders::Settings, "interrupted.txt");
}

void PS5Extras::GameClosed()
{
	// The game was closed in the title: nothing to go on with at the next start.
	const std::string note = InterruptedNotePath();
	if (FileSystem::FileExists(note.c_str()))
		FileSystem::DeleteFilePath(note.c_str());
}
