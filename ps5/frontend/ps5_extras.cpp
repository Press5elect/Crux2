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
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>
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

// v1.0.19: the safety save in memory. Up to v1.0.18 Sleep-safe saving wrote a whole state (about
// 15 MB) to the console's drive every five minutes and at every opening of the console's menu: the
// most Crux2 ever wrote. Now the five-minute state is zipped into memory only, and written to the
// drive just when it can be needed: by the crash report, when Crux2 stops in a crash (the bytes and
// the note are written by the signal handler as they are: open, write, close). The console's menu is
// still a fresh state on the drive (the console can be put to rest or the title closed from there),
// but not again within a minute of the last, and automatic states are zipped harder (zstd 10, on a
// thread of their own). The states of "Auto-save every N minutes", a choice of the player's, stay on
// the drive.
namespace
{
	constexpr int AUTOMATIC_ZSTD = 10;
	constexpr double MENU_SAVE_GAP = 60.0;

	// Two slots, filled in turn: the crash report reads the one published (s_safety), never the one
	// being filled. Fixed storage for the paths and the note: a signal handler reads them.
	struct SafetySlot
	{
		std::vector<u8> data;
		char state_path[512] = "";
		char note_path[512] = "";
		char note[2048] = "";
		size_t note_length = 0;
	};
	SafetySlot s_slots[2];
	std::atomic<int> s_safety{-1};      // the slot the crash report writes, -1: none
	std::mutex s_safety_fill;           // one filling at a time
	std::atomic<unsigned> s_safety_generation{0}; // a game closed or started: a fill under way is let go

	std::string SafetyNote(const std::string& state_path)
	{
		return "game=" + VMManager::GetDiscPath() + "\nserial=" + VMManager::GetDiscSerial() + "\nstate=" + state_path + "\n";
	}

	void SafetyForget()
	{
		s_safety_generation.fetch_add(1, std::memory_order_acq_rel);
		s_safety.store(-1, std::memory_order_release);
	}
}

// The crash report's part (ps5_crash.cpp, in its signal handler): the safety state and its note onto
// the drive, so that the library offers to go on from it. 1 when written, 0 when there was none.
extern "C" int ps5px_safety_state_write(void)
{
	const int at = s_safety.load(std::memory_order_acquire);
	if (at < 0 || at > 1)
		return 0;
	const SafetySlot& slot = s_slots[at];
	if (slot.data.empty() || slot.state_path[0] == '\0')
		return 0;
	const int fd = open(slot.state_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return 0;
	size_t done = 0;
	while (done < slot.data.size())
	{
		const ssize_t wrote = write(fd, slot.data.data() + done, slot.data.size() - done);
		if (wrote <= 0)
			break;
		done += static_cast<size_t>(wrote);
	}
	fsync(fd);
	close(fd);
	if (done != slot.data.size())
		return 0;
	const int note = open(slot.note_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (note >= 0)
	{
		(void)!write(note, slot.note, slot.note_length);
		fsync(note);
		close(note);
	}
	return 1;
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
	static double last_menu_save = -1e9;

	if (s_clock_reset.exchange(false, std::memory_order_acq_rel))
	{
		clock.Reset();
		menu_before = 0;
		last_menu_save = -1e9;
		SafetyForget();
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
	const double now = timer.GetTimeSeconds();
	if (why != nullptr && now - last_menu_save < MENU_SAVE_GAP)
	{
		// The menu again within a minute (a notification looked at): the state on the drive is
		// that recent, and a crash still has the one in memory.
		ps5px_log("auto-save: the console's menu opened again within a minute of the last save: not saved again");
		why = nullptr;
	}
	const double every = (minutes > 0) ? minutes * 60.0 : (sleep_safe ? 300.0 : 0.0);
	if (why == nullptr && !clock.Tick(now, every))
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
	const std::string note_path = InterruptedNotePath();
	const std::string note = SafetyNote(path);
	// Where it goes: the drive for the console's menu and for the player's own auto-save; memory
	// only for the five-minute safety save.
	const bool to_drive = (why != nullptr) || minutes > 0;
	if (why != nullptr)
		last_menu_save = now;
	Common::Timer took;
	const unsigned generation = s_safety_generation.load(std::memory_order_acquire);
	VMManager::SaveStateAutomatic(to_drive ? path : std::string(), AUTOMATIC_ZSTD,
		[to_drive, sleep_safe_now = sleep_safe, path, note_path, note, generation](std::vector<u8> data, std::string error) {
			if (!error.empty())
			{
				ps5px_log("auto-save: the state could not be saved: %s", error.c_str());
				return;
			}
			if (to_drive)
			{
				if (sleep_safe_now)
					PS5Platform::WriteFileSafely(note_path.c_str(), note.data(), note.size());
				PS5UI::StatesChanged();
			}
			// Either way the newest state is the one a crash writes out (the one on the drive may be
			// older by then).
			if (!sleep_safe_now || data.empty() || note.size() >= sizeof(SafetySlot::note) || path.size() >= sizeof(SafetySlot::state_path) ||
				note_path.size() >= sizeof(SafetySlot::note_path))
				return;
			const std::lock_guard lock(s_safety_fill);
			if (s_safety_generation.load(std::memory_order_acquire) != generation)
				return; // (the game closed while it was zipped)
			const int current = s_safety.load(std::memory_order_acquire);
			const int next = (current == 0) ? 1 : 0;
			SafetySlot& slot = s_slots[next];
			slot.data = std::move(data);
			std::memcpy(slot.state_path, path.c_str(), path.size() + 1);
			std::memcpy(slot.note_path, note_path.c_str(), note_path.size() + 1);
			std::memcpy(slot.note, note.data(), note.size());
			slot.note_length = note.size();
			s_safety.store(next, std::memory_order_release);
			if (!to_drive)
				ps5px_log("auto-save: the safety state kept in memory (%zu KB): written to the drive only if Crux2 stops in a crash",
					slot.data.size() / 1024);
		});
	if (why != nullptr)
		ps5px_log("auto-save: saved because %s; the game stood still for %.0f ms", why, took.GetTimeMilliseconds());
	else if (minutes > 0)
		ps5px_log("auto-save: saved after %d minute%s of play; the game stood still for %.0f ms", minutes, (minutes == 1) ? "" : "s",
			took.GetTimeMilliseconds());
	else
		ps5px_log("auto-save: the five-minute safety state taken (into memory); the game stood still for %.0f ms", took.GetTimeMilliseconds());
}

std::string PS5Extras::InterruptedNotePath()
{
	return Path::Combine(EmuFolders::Settings, "interrupted.txt");
}

void PS5Extras::GameClosed()
{
	// The game was closed in the title: nothing to go on with at the next start, and no safety
	// state for a crash to write.
	SafetyForget();
	const std::string note = InterruptedNotePath();
	if (FileSystem::FileExists(note.c_str()))
		FileSystem::DeleteFilePath(note.c_str());
}
