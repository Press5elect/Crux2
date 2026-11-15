// Crux2 - the settings inventory: what PCSX2 is asked while the pages are made, on a PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The functions the pages call while they are made, of the files the inventory cannot compile on a PC
// (the console's own, and PCSX2's that want SDL, libzip or YAML): each answers as a console would in the
// state Crux2InventorySetUp gives it. Where a fake copies a few lines of the real file, it says so. A
// function that is not here and not in a file build-inventory.sh compiles is a stub that stops the
// program with its name (make-stubs.py): add it here then, answering as a new console would.

#include "common/MemorySettingsInterface.h"
#include "Host.h"
#include "Input/InputManager.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "common/StringUtil.h"

#include "ps5_online.h"
#include "ps5_extras.h"
#include "ps5_recommend.h"
#include "GameDatabase.h"
#include "Achievements.h"
#include "Patch.h"
#include "VMManager.h"
#include "GameList.h"
#include "ps5_update.h"
#include "ps5_platform.h"
#include "ps5_backup.h"
#include "ps5_jobs.h"
#include "ps5_shares.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

namespace
{
MemorySettingsInterface s_base, s_secrets;
int s_state = 0;
}

// ps5_host.cpp's list (it has none of its own: BEGIN_HOTKEY_LIST(g_host_hotkeys) END_HOTKEY_LIST()), and
// InputManager's walk over the three lists, as InputManager.cpp has it (that file wants SDL or the console).
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

std::vector<const HotkeyInfo*> InputManager::GetHotkeyList()
{
	std::vector<const HotkeyInfo*> ret;
	for (const HotkeyInfo* hotkey_list : {g_common_hotkeys, g_gs_hotkeys, g_host_hotkeys})
	{
		for (const HotkeyInfo* hotkey = hotkey_list; hotkey->name != nullptr; hotkey++)
			ret.push_back(hotkey);
	}
	return ret;
}

// The memory cards (MemoryCardFile.cpp, which wants the folder cards' YAML): none is there yet, and the
// default names are PCSX2's.
uint FileMcd_GetMtapPort(uint slot)
{
	return (slot == 1 || slot >= 5) ? 1 : 0;
}

uint FileMcd_GetMtapSlot(uint slot)
{
	return (slot >= 5) ? slot - 4 : (slot >= 2) ? slot - 1 : 0;
}

bool FileMcd_IsMultitapSlot(uint slot)
{
	return (slot > 1);
}

std::string FileMcd_GetDefaultName(uint slot)
{
	if (FileMcd_IsMultitapSlot(slot))
		return StringUtil::StdStringFromFormat("Mcd-Multitap%u-Slot%02u.ps2", FileMcd_GetMtapPort(slot) + 1, FileMcd_GetMtapSlot(slot) + 1);
	return StringUtil::StdStringFromFormat("Mcd%03u.ps2", slot + 1);
}

std::vector<AvailableMcdInfo> FileMcd_GetAvailableCards(bool include_in_use_cards)
{
	return {};
}

// ---- the console's own (ps5/frontend): what a new console with nothing downloaded or set up says.

std::string PS5Patches::Describe()
{
	return "Not downloaded";
}

PS5Shares::ShareFile& PS5Shares::Current()
{
	static ShareFile file;
	file.shares.clear();
	if (s_state == 1)
		file.shares.push_back({"games", false, "192.168.1.2", "games", "", "", "/smb/games"});
	return file;
}

// One USB drive is in: the rows that need somewhere to put things are shown.
std::vector<PS5Jobs::Place> PS5Jobs::Places()
{
	return {{"/mnt/usb0", "USB drive 1"}};
}

PS5Jobs::Place PS5Jobs::BackupPlace()
{
	return Places().front();
}

// The drive's backups were looked for, and it has none.
PS5Backup::State PS5Backup::CurrentState()
{
	PS5Backup::State state;
	state.listed = true;
	state.listed_place = PS5Jobs::BackupPlace().path;
	return state;
}

bool PS5Platform::IsElevated()
{
	return true;
}

bool PS5Platform::WantsUSB()
{
	return true;
}

// ---- the host (ps5_host.cpp, which is the console's main and wants its SDK)

// English only, as ps5_host.cpp has it.
s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	if (msg.empty())
		return 0;
	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

PS5Logs::Files PS5Jobs::LogFiles()
{
	return {"/title/crux2-boot.log", "/title/crux2-boot.previous.log", "/data/logs/emulog.txt", {}};
}

extern "C" const char* ps5px_title_dir(void)
{
	return "/title/";
}

// A developer build (no version), with nothing outside the title folder.
extern "C" const char* ps5px_build_version(void)
{
	return "";
}

extern "C" const char* ps5px_build_stamp(void)
{
	return "inventory";
}

extern "C" const char* ps5px_build_identity(void)
{
	return "inventory";
}

std::string PS5Update::RunningVersionLabel()
{
	return "inventory";
}

const std::string& PS5Platform::OutsideFolder()
{
	static const std::string none;
	return none;
}

// ---- the emulator: an empty game list (the pages are of settings; Statistics counts nothing).

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
	static std::recursive_mutex lock;
	return std::unique_lock<std::recursive_mutex>(lock);
}

u32 GameList::GetEntryCount()
{
	return 0;
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
	return nullptr;
}

// A game with no settings of its own yet (the file is not there).
std::string VMManager::GetGameSettingsPath(std::string_view game_serial, u32 game_crc)
{
	return "/nonexistent/gamesettings/" + std::string(game_serial) + ".ini";
}

bool Achievements::IsHardcoreModeActive()
{
	return false;
}

// PCSX2's patches (Patch.cpp, which wants libzip): its names for where they are kept, and for the open
// game one patch and one cheat, so that the rows made for each are in the inventory.
namespace Patch
{
const char* PATCHES_CONFIG_SECTION = "Patches";
const char* CHEATS_CONFIG_SECTION = "Cheats";
const char* PATCH_ENABLE_CONFIG_KEY = "Enable";
const char* PATCH_DISABLE_CONFIG_KEY = "Disable";
} // namespace Patch

std::vector<Patch::PatchInfo> Patch::GetPatchInfo(const std::string_view serial, u32 crc, bool cheats, bool showAllCRCS, u32* num_unlabelled_patches)
{
	if (num_unlabelled_patches != nullptr)
		*num_unlabelled_patches = 0;
	Patch::PatchInfo info;
	info.name = cheats ? "A cheat" : "A patch";
	info.description = cheats ? "What the cheat does." : "What the patch does.";
	return {info};
}

// PCSX2's game database does not know the game.
void GameDatabase::ensureLoaded()
{
}

const GameDatabaseSchema::GameEntry* GameDatabase::findGame(const std::string_view serial)
{
	return nullptr;
}

// The repository's recommended.ini has one setting for the game: the rows that show it are there.
bool PS5Recommend::ForGame(const std::string& serial, Pick* pick)
{
	pick->note.clear();
	pick->values = {{"EmuCore/GS", "upscale_multiplier", "2"}};
	return true;
}

std::string PS5Extras::TexturePackPlace(const std::string& serial, bool shares)
{
	return {};
}

std::string PS5Extras::CardNameFor(const std::string& serial)
{
	return serial + ".ps2";
}

// (As ps5_shares.cpp has them; that file wants the virtual file layer.)
std::string PS5Shares::shareAddress(const Share& s)
{
	if (s.nfs)
		return "nfs://" + s.host + "/" + s.path;
	return "smb://" + s.host + "/" + s.path;
}

std::string PS5Shares::shareDefaultName(const Share& s)
{
	const size_t slash = s.path.find_last_of('/');
	const std::string last = slash == std::string::npos ? s.path : s.path.substr(slash + 1);
	return last.empty() ? s.host : last;
}

void Crux2InventorySetUp(int state)
{
	s_state = state;
	if (state == 0)
	{
		// Nothing written: every value is its default.
		if (Host::Internal::GetBaseSettingsLayer() == nullptr)
		{
			Host::Internal::SetBaseSettingsLayer(&s_base);
			Host::Internal::SetSecretsSettingsLayer(&s_secrets);
		}
		return;
	}
	s_base.SetStringValue("Achievements", "Username", "someone");
	s_secrets.SetStringValue("Achievements", "Token", "token");
	s_base.SetStringList("GameList", "RecursivePaths", {"/mnt/usb0/games"});
	s_base.SetBoolValue("PS5", "USBPads", true);
}

// Every symbol make-stubs.py stubs jumps here with its name.
extern "C" [[noreturn]] void crux2_inventory_missing(const char* name)
{
	int status = 0;
	char* const shown = abi::__cxa_demangle(name, nullptr, nullptr, &status);
	std::fprintf(stderr, "inventory: the pages called %s, which the inventory does not have (add it to "
						 "ps5/tests/bigpicture/inventory/host_fakes.cpp, or link the PCSX2 file that has it)\n",
		status == 0 && shown ? shown : name);
	std::fflush(stderr);
	std::_Exit(3);
}
