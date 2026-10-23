// Crux2 - what the interface asks for that takes a while (ps5_jobs.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_card_sync.h"
#include "ps5_priority.h"
#include "ps5_ps2_ftp.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "ps5_jobs.h"

#include "ps5_online.h"
#include "ps5_platform.h"
#include "ps5_ui.h"
#include "vfs/ps5_vfs.h"

#include "Config.h"
#include "Host.h"
#include "MTGS.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsInterface.h"

#include "fmt/format.h"

#include "ps5_pgo.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <pthread.h>
#include <sys/stat.h>

namespace PS5Jobs
{
namespace
{
PS5Backup::Folders BackupFolders()
{
	return {EmuFolders::MemoryCards, EmuFolders::Savestates, Path::Combine(EmuFolders::DataRoot, "backup-before-restore")};
}

std::string Version()
{
	return (*ps5px_build_version() != '\0') ? ps5px_build_version() : "a build without a version";
}

bool GameLoadedOrStarting()
{
	return VMManager::GetState() != VMState::Shutdown;
}

// A small job on a thread of its own (the console gives a default thread 2 MiB of direct memory).
bool RunDetached(void* (*entry)(void*), void* argument)
{
	pthread_t thread;
	const int rc = pthread_create(&thread, nullptr, entry, argument);
	if (rc != 0)
	{
		ps5px_log("jobs: a thread did not start: %d", rc);
		return false;
	}
	pthread_detach(thread);
	return true;
}

std::atomic_bool s_copying{false};
std::atomic_bool s_downloading{false};
std::atomic_bool s_changing_card{false};

// Whatever has to happen on a thread of its own, as a function.
bool RunDetached(std::function<void()> work)
{
	auto* const job = new std::function<void()>(std::move(work));
	const bool started = RunDetached(
		[](void* argument) -> void* {
			auto* const todo = static_cast<std::function<void()>*>(argument);
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			(*todo)();
			delete todo;
			return nullptr;
		},
		job);
	if (!started)
		delete job;
	return started;
}

// A change to a memory card: one at a time, on its thread.
void ChangeCard(const char* what, const char* failed, std::function<PS5Cards::Result()> change)
{
	if (GameLoadedOrStarting())
	{
		PS5UI::Toast("Close the game first: it has the memory cards, and would write over the change.");
		return;
	}
	if (s_changing_card.exchange(true, std::memory_order_acq_rel))
	{
		PS5UI::Toast("A card is being changed already: wait for it.");
		return;
	}
	const bool started = RunDetached([what = std::string(what), failed = std::string(failed), change = std::move(change)] {
		const PS5Cards::Result result = change();
		ps5px_log("cards: %s: %s: %s", what.c_str(), result.ok ? "done" : "NOT done", result.message.c_str());
		if (result.ok)
			PS5UI::Toast(result.message);
		else
			PS5UI::Message(failed, result.message);
		PS5UI::PagesChanged();
		s_changing_card.store(false, std::memory_order_release);
	});
	if (!started)
	{
		s_changing_card.store(false, std::memory_order_release);
		PS5UI::Toast("The change could not be started (crux2-boot.log says why).");
	}
}

// What was written is everybody's to read and to replace, as the title's other files are (the
// console's FTP server reaches them as another user).
void MadeCardFiles(const std::string& card)
{
	PS5Platform::MakePublic(CardPath(card).c_str(), false);
	PS5Platform::MakePublic(CardUndoFolder().c_str(), true);
	PS5Platform::MakePublic(Path::Combine(CardUndoFolder(), card).c_str(), false);
}
} // namespace

std::string CardPath(const std::string& name)
{
	return Path::Combine(EmuFolders::MemoryCards, name);
}

std::string CardUndoFolder()
{
	return Path::Combine(EmuFolders::DataRoot, "card-undo");
}

std::string SavesFolder()
{
	return Path::Combine(EmuFolders::DataRoot, "saves");
}

std::vector<Place> ExportPlaces()
{
	std::vector<Place> places;
	places.push_back({SavesFolder(), "The saves folder of Crux2"});
	for (const Place& place : Places())
		places.push_back({place.path + "/saves", place.name});
	return places;
}

void FindSaveFiles(std::function<void(std::vector<FoundSave>)> then)
{
	// (The places are asked on a thread of their own: a share that does not answer is waited for.)
	const std::vector<Place> places = ExportPlaces();
	const bool started = RunDetached([places, then] {
		std::vector<FoundSave> found;
		for (const Place& place : places)
		{
			for (PS5Cards::SaveFile& file : PS5Cards::SaveFiles(place.path))
				found.push_back({std::move(file.path), std::move(file.name), (&place == &places.front()) ? std::string("saves folder") : place.name, file.size});
		}
		ps5px_log("cards: %zu save file(s) found in %zu place(s)", found.size(), places.size());
		if (MTGS::IsOpen())
			MTGS::RunOnGSThread([then, found = std::move(found)]() { then(found); });
	});
	if (!started)
		then({});
}

void ImportSave(const std::string& card, const std::string& file)
{
	ChangeCard("a save brought in", "The save was not brought in", [card, file] {
		const PS5Cards::Result result = PS5Cards::Import(CardPath(card), file, CardUndoFolder());
		MadeCardFiles(card);
		return result;
	});
}

void ExportSave(const std::string& card, const std::string& save, const Place& place)
{
	ChangeCard("a save written out", "The save was not written out", [card, save, place] {
		PS5Cards::Result result = PS5Cards::Export(CardPath(card), save, place.path);
		if (result.ok)
		{
			// (Its name is the end of the sentence: "... was written as NAME.psu.")
			const size_t as = result.message.rfind(" as ");
			if (place.path == SavesFolder() && as != std::string::npos && result.message.size() > as + 5)
			{
				PS5Platform::MakePublic(place.path.c_str(), true);
				PS5Platform::MakePublic(Path::Combine(place.path, result.message.substr(as + 4, result.message.size() - as - 5)).c_str(), false);
			}
			result.message += fmt::format(" In saves/ {}.", (place.path == SavesFolder()) ? std::string("of Crux2's folder") : "on " + place.name);
		}
		return result;
	});
}

void CopySave(const std::string& card, const std::string& save, const std::string& to_card)
{
	ChangeCard("a save copied to another card", "The save was not copied", [card, save, to_card] {
		const PS5Cards::Result result = PS5Cards::Copy(CardPath(card), save, CardPath(to_card), CardUndoFolder());
		MadeCardFiles(to_card);
		return result;
	});
}

void DeleteSave(const std::string& card, const std::string& save)
{
	ChangeCard("a save deleted", "The save was not deleted", [card, save] {
		const PS5Cards::Result result = PS5Cards::Delete(CardPath(card), save, CardUndoFolder());
		MadeCardFiles(card);
		return result;
	});
}

// ---- the card manager of v1.5.0

namespace
{
/// A folder's files (name, size) and folders, on the console or under a mount of the virtual file
/// layer. False when it cannot be listed.
bool ListFolder(const std::string& folder, std::vector<std::pair<std::string, uint64_t>>* files, std::vector<std::string>* folders)
{
	if (ps5vfs_is_virtual(folder.c_str()))
	{
		ps5vfs_dir* const dir = ps5vfs_opendir(folder.c_str());
		if (!dir)
			return false;
		struct ps5vfs_dirent entry;
		while (ps5vfs_readdir(dir, &entry) == 1)
		{
			if (!entry.name || entry.name[0] == '.')
				continue;
			if (entry.is_directory)
				folders->push_back(entry.name);
			else
				files->push_back({entry.name, static_cast<uint64_t>(entry.size)});
		}
		ps5vfs_closedir(dir);
		return true;
	}
	FileSystem::FindResultsArray found;
	if (!FileSystem::DirectoryExists(folder.c_str()) ||
		!FileSystem::FindFiles(folder.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &found))
		return false;
	for (const FILESYSTEM_FIND_DATA& f : found)
	{
		const std::string name(Path::GetFileName(f.FileName));
		if (name.empty() || name[0] == '.')
			continue;
		if (f.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			folders->push_back(name);
		else
			files->push_back({name, static_cast<uint64_t>(f.Size)});
	}
	return true;
}

/// A card's name that is free: "<title>.ps2", else "<title> (2).ps2"...
std::string FreeCardName(const std::string& title, const char* ending)
{
	std::string base = title;
	for (char& c : base)
	{
		if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
			c = '_';
	}
	if (base.empty())
		base = "Card";
	for (int number = 1; number < 1000; number++)
	{
		const std::string name = (number == 1) ? base + ending : fmt::format("{} ({}){}", base, number, ending);
		if (!FileSystem::FileExists(CardPath(name).c_str()) && !FileSystem::DirectoryExists(CardPath(name).c_str()))
			return name;
	}
	return std::string();
}
} // namespace

std::vector<CardSource> CardSources()
{
	std::vector<CardSource> sources;
	sources.push_back({true, Host::GetBaseStringSettingValue("PS5", "Ps2Address", ""), "A PlayStation 2 on the network"});
	sources.push_back({false, SavesFolder(), "The saves folder of Crux2"});
	for (const Place& place : Places())
		sources.push_back({false, place.path, place.name});
	return sources;
}

void FindIn(const CardSource& source, std::function<void(std::vector<CardThing>, std::string why)> then)
{
	const auto hand_back = [then](std::vector<CardThing> things, std::string why) {
		if (MTGS::IsOpen())
			MTGS::RunOnGSThread([then, things = std::move(things), why = std::move(why)]() { then(things, why); });
	};
	const bool started = RunDetached([source, hand_back] {
		std::vector<CardThing> things;
		std::string why;
		if (source.ps2)
		{
			Host::SetBaseStringSettingValue("PS5", "Ps2Address", source.path.c_str());
			Host::CommitBaseSettingChanges();
			PS5Ps2Ftp::Client client;
			std::vector<PS5Ps2Ftp::Save> saves;
			if (client.Connect(source.path, 21, &why) && PS5Ps2Ftp::ListSaves(client, &saves, &why))
			{
				why.clear();
				for (const PS5Ps2Ftp::Save& s : saves)
					things.push_back({CardThing::Ps2Save, s.path, s.name, s.card, s.bytes, source.path});
			}
			ps5px_log("cards: PlayStation 2 at %s: %zu saves%s%s", source.path.c_str(), things.size(), why.empty() ? "" : ": ", why.c_str());
			hand_back(std::move(things), why);
			return;
		}
		static constexpr const char* subfolders[] = {"", "saves", "Crux2/saves", "PS2/SAVES", "PS2/saves", "SAVES", "memcards", "Crux2/memory cards",
			"VMC", "PS2/VMC"};
		bool any = false;
		for (const char* sub : subfolders)
		{
			const std::string folder = *sub ? source.path + "/" + sub : source.path;
			std::vector<std::pair<std::string, uint64_t>> files;
			std::vector<std::string> folders;
			if (!ListFolder(folder, &files, &folders))
				continue;
			any = true;
			const std::string where = *sub ? source.name + ", " + sub : source.name;
			for (const auto& [name, size] : files)
			{
				if (PS5Cards::IsSaveFile(name) && size > 0)
					things.push_back({CardThing::SaveFile, folder + "/" + name, name, where, size, {}});
				else if (PS5Cards::IsCardImageName(name) && PS5Cards::IsCardImageSize(size))
					things.push_back({CardThing::CardImage, folder + "/" + name, name, where, size, {}});
			}
			// Saves copied off a card as folders (wLaunchELF's copy to a USB drive).
			for (const std::string& name : folders)
			{
				std::vector<std::pair<std::string, uint64_t>> inner;
				std::vector<std::string> unused;
				if (name.size() > 31 || !ListFolder(folder + "/" + name, &inner, &unused) || inner.empty() || inner.size() > 100)
					continue;
				std::vector<std::string> names;
				uint64_t total = 0;
				for (const auto& [n, sz] : inner)
				{
					names.push_back(n);
					total += sz;
				}
				if (PS5Cards::LooksLikeSaveFolder(names))
					things.push_back({CardThing::SaveFolder, folder + "/" + name, name, where, total, {}});
			}
		}
		if (!any)
			why = source.name + " could not be looked into";
		ps5px_log("cards: %s: %zu things to bring in", source.path.c_str(), things.size());
		hand_back(std::move(things), why);
	});
	if (!started)
		then({}, "a thread could not be started");
}

void BringIn(const std::string& card, const CardThing& thing)
{
	if (thing.kind == CardThing::SaveFile)
	{
		ImportSave(card, thing.path);
		return;
	}
	if (thing.kind == CardThing::CardImage)
		return;
	ChangeCard("a save brought in", "The save was not brought in", [card, thing] {
		std::vector<PS5Cards::FolderFile> files;
		std::string why;
		if (thing.kind == CardThing::Ps2Save)
		{
			PS5UI::SetBackgroundNote("Reading the save from the PlayStation 2");
			PS5Ps2Ftp::Client client;
			std::vector<PS5Ps2Ftp::SaveFile> read;
			const bool ok = client.Connect(thing.ps2, 21, &why) && PS5Ps2Ftp::ReadSave(client, thing.path, &read, &why);
			PS5UI::SetBackgroundNote(std::string());
			if (!ok)
				return PS5Cards::Result{false, "The PlayStation 2's save could not be read: " + why + "."};
			for (PS5Ps2Ftp::SaveFile& f : read)
				files.push_back({std::move(f.name), std::move(f.data)});
		}
		else
		{
			std::vector<std::pair<std::string, uint64_t>> list;
			std::vector<std::string> unused;
			if (!ListFolder(thing.path, &list, &unused))
				return PS5Cards::Result{false, "The save's folder could not be read."};
			for (const auto& [name, size] : list)
			{
				std::string data;
				if (!PS5Backup::ReadWholeFile(thing.path + "/" + name, &data))
					return PS5Cards::Result{false, "A file of the save could not be read: " + name + "."};
				files.push_back({name, std::move(data)});
			}
		}
		PS5Cards::Result result = PS5Cards::ImportFolder(CardPath(card), thing.name, files, CardUndoFolder());
		if (result.ok)
			MadeCardFiles(card);
		return result;
	});
}

void BringInCard(const CardThing& image, const std::string& title)
{
	const std::string name = FreeCardName(title, ".ps2");
	if (name.empty())
	{
		PS5UI::Toast("There are too many cards of that name.");
		return;
	}
	ChangeCard("a card brought in", "The card was not brought in", [image, name] {
		PS5Cards::Result result = PS5Cards::ImportCard(image.path, CardPath(name));
		if (result.ok)
			PS5Platform::MakePublic(CardPath(name).c_str(), false);
		return result;
	});
}

void CopyFromImage(const std::string& image, const std::string& save, const std::string& to_card)
{
	ChangeCard("a save copied from a card image", "The save was not copied", [image, save, to_card] {
		PS5Cards::Result result = PS5Cards::Copy(image, save, CardPath(to_card), CardUndoFolder());
		if (result.ok)
			MadeCardFiles(to_card);
		return result;
	});
}

void CreateCard(const std::string& title, int megabytes)
{
	if (megabytes == 0)
	{
		// A PlayStation 1 card: PCSX2 makes it (its format is not this manager's).
		const std::string name = FreeCardName(title, ".mcr");
		if (name.empty() || !FileMcd_CreateNewCard(name, MemoryCardType::File, MemoryCardFileType::PS1))
		{
			PS5UI::Toast("The card could not be made (logs/emulog.txt has the reason).");
			return;
		}
		PS5Platform::MakePublic(CardPath(name).c_str(), false);
		PS5UI::PagesChanged();
		PS5UI::Toast(fmt::format("\"{}\" was made: a PlayStation 1 card.", Path::GetFileTitle(name)));
		return;
	}
	const std::string name = FreeCardName(title, ".ps2");
	if (name.empty())
	{
		PS5UI::Toast("There are too many cards of that name.");
		return;
	}
	ChangeCard("a card made", "The card was not made", [name, megabytes] {
		PS5Cards::Result result = PS5Cards::Create(CardPath(name), megabytes);
		if (result.ok)
			PS5Platform::MakePublic(CardPath(name).c_str(), false);
		return result;
	});
}

void FormatCard(const std::string& card)
{
	ChangeCard("a card formatted", "The card was not formatted", [card] {
		PS5Cards::Result result = PS5Cards::Format(CardPath(card), CardUndoFolder());
		if (result.ok)
			MadeCardFiles(card);
		return result;
	});
}

void ExportCard(const std::string& card, const Place& place)
{
	const std::string folder = place.path + "/Crux2/memory cards";
	const bool started = RunDetached([card, folder, name = place.name] {
		const PS5Cards::Result result = PS5Cards::ExportCard(CardPath(card), folder);
		ps5px_log("cards: card copied out to %s: %s", folder.c_str(), result.message.c_str());
		if (result.ok)
			PS5UI::Toast(result.message + " (" + name + ", Crux2/memory cards)");
		else
			PS5UI::Message("The card was not copied out", result.message);
	});
	if (!started)
		PS5UI::Toast("The card could not be copied out now.");
}

void UndoCard(const std::string& card)
{
	ChangeCard("a card put back as it was", "The card was not put back", [card] {
		const PS5Cards::Result result = PS5Cards::Undo(CardPath(card), CardUndoFolder());
		MadeCardFiles(card);
		return result;
	});
}

void DuplicateCard(const std::string& card)
{
	ChangeCard("a card copied", "The card was not copied", [card]() -> PS5Cards::Result {
		const std::string_view title = Path::GetFileTitle(card), extension = Path::GetExtension(card);
		std::string name;
		for (int number = 1; number < 100; number++)
		{
			name = (number == 1) ? fmt::format("{} copy.{}", title, extension) : fmt::format("{} copy {}.{}", title, number, extension);
			if (!PS5Backup::PathExists(CardPath(name)))
				break;
			name.clear();
		}
		std::string data;
		if (name.empty())
			return {false, "There are too many copies of that card already."};
		if (!PS5Backup::ReadWholeFile(CardPath(card), &data))
			return {false, "The card's file could not be read."};
		if (!PS5Backup::WriteWholeFile(CardPath(name), data))
			return {false, PS5Backup::WhyNot(errno, EmuFolders::MemoryCards)};
		PS5Platform::MakePublic(CardPath(name).c_str(), false);
		return {true, fmt::format("\"{}\" is a copy of {} now: choose it as a slot's card.", Path::GetFileTitle(name), title)};
	});
}

std::vector<Place> Places()
{
	std::vector<Place> places;
	if (PS5Platform::IsElevated())
	{
		for (unsigned drive = 0; drive < 8; drive++)
		{
			const std::string root = fmt::format("/mnt/usb{}", drive);
			if (FileSystem::DirectoryExists(root.c_str()))
				places.push_back({root, fmt::format("USB drive {}", drive + 1)});
		}
	}
	for (unsigned i = 0;; i++)
	{
		const char* const point = ps5vfs_mount_point(i);
		if (!point)
			break;
		if (std::strncmp(point, "/smb/", 5) == 0)
			places.push_back({point, fmt::format("Share: {}", point + 5)});
	}
	return places;
}

Place BackupPlace()
{
	const std::vector<Place> places = Places();
	if (places.empty())
		return {};
	const std::string chosen = Host::GetBaseStringSettingValue("PS5", "BackupPlace", "");
	for (const Place& place : places)
	{
		if (place.path == chosen)
			return place;
	}
	return places.front();
}

bool BackupNow()
{
	const Place place = BackupPlace();
	if (place.path.empty())
	{
		PS5UI::Toast("There is nowhere to back up to: no USB drive is in, and network.cfg names no share.");
		return false;
	}
	if (GameLoadedOrStarting())
	{
		PS5UI::Toast("Close the game first: its memory card may be written to while it runs.");
		return false;
	}
	const bool states = Host::GetBaseBoolSettingValue("PS5", "BackupStates", true);
	PS5Backup::SetStopCheck([] { return GameLoadedOrStarting(); });
	ps5px_log("backup: to %s%s", place.path.c_str(), states ? ", with the saved states" : "");
	PS5Backup::SetReplaceOlder(Host::GetBaseBoolSettingValue("PS5", "BackupReplace", false));
	const bool started = PS5Backup::StartBackup(BackupFolders(), place.path, states, false, Version(), [place](const PS5Backup::Result& result) {
		ps5px_log("backup: %s (%d files, %llu bytes, %s)", result.ok ? "done" : "not made", result.files,
			static_cast<unsigned long long>(result.bytes), result.message.c_str());
		PS5UI::SetBackgroundNote(std::string());
		PS5UI::PagesChanged();
		if (result.ok)
			PS5UI::Toast(fmt::format("{} On {}.", result.message, place.name));
		else
			PS5UI::Message("The backup was not made", result.message);
	});
	if (!started)
	{
		PS5UI::Toast("Another backup or restore is running: wait for it.");
		return false;
	}
	PS5UI::SetBackgroundNote("Backing up to " + place.name);
	PS5UI::PagesChanged();
	return true;
}

bool Restore(const PS5Backup::Snapshot& snapshot)
{
	if (GameLoadedOrStarting())
	{
		PS5UI::Toast("Close the game first: a restore replaces the memory cards it uses.");
		return false;
	}
	PS5Backup::SetStopCheck([] { return GameLoadedOrStarting(); });
	ps5px_log("backup: restoring %s", snapshot.path.c_str());
	const bool started = PS5Backup::StartRestore(BackupFolders(), snapshot, [](const PS5Backup::Result& result) {
		ps5px_log("backup: restore %s (%d files, %s)", result.ok ? "done" : "not completed", result.files, result.message.c_str());
		PS5UI::SetBackgroundNote(std::string());
		// The cards and the states on the console are other ones now.
		PS5UI::PagesChanged();
		PS5UI::StatesChanged();
		PS5UI::Message(result.ok ? "The backup was restored" : "The restore did not complete", result.message);
	});
	if (!started)
	{
		PS5UI::Toast("Another backup or restore is running: wait for it.");
		return false;
	}
	PS5UI::SetBackgroundNote("Restoring a backup");
	PS5UI::PagesChanged();
	return true;
}

void ListBackups()
{
	const Place place = BackupPlace();
	if (place.path.empty())
		return;
	PS5Backup::StartList(place.path);
}

std::atomic_bool s_syncing_cards{false};

void SyncCards(bool quiet)
{
	if (GameLoadedOrStarting())
	{
		if (!quiet)
			PS5UI::Toast("Close the game first: it has the memory cards.");
		return;
	}
	const Place place = BackupPlace();
	if (place.path.empty())
	{
		if (!quiet)
			PS5UI::Toast("No USB drive or network share to sync the memory cards with.");
		return;
	}
	bool expected = false;
	if (!s_syncing_cards.compare_exchange_strong(expected, true))
		return;
	const bool started = RunDetached([place, quiet] {
		PS5UI::SetBackgroundNote("Syncing the memory cards with " + place.name);
		PS5CardSync::Folders folders;
		folders.here = EmuFolders::MemoryCards;
		folders.there = place.path + "/Crux2/memory cards";
		folders.older_here = Path::Combine(EmuFolders::DataRoot, "older memory cards");
		folders.older_there = folders.there + "/.older";

		PS5CardSync::Io io;
		io.list = [](const std::string& folder, std::vector<std::string>* names) {
			std::vector<std::pair<std::string, uint64_t>> files;
			if (!PS5Backup::ListFiles(folder, &files))
				return false;
			for (auto& f : files)
				names->push_back(std::move(f.first));
			return true;
		};
		io.stat = [](const std::string& path) {
			PS5CardSync::FileInfo info;
			struct stat st;
			const int rc = ps5vfs_is_virtual(path.c_str()) ? ps5vfs_stat(path.c_str(), &st) : stat(path.c_str(), &st);
			if (rc == 0 && S_ISREG(st.st_mode))
			{
				info.exists = true;
				info.size = static_cast<uint64_t>(st.st_size);
				info.mtime = static_cast<int64_t>(st.st_mtime);
			}
			return info;
		};
		io.read = [](const std::string& path, std::string* data) { return PS5Backup::ReadWholeFile(path, data); };
		io.write = [](const std::string& path, const std::string& data) { return PS5Backup::WriteWholeFile(path, data); };
		io.make_folder = [](const std::string& folder) { return PS5Backup::MakeAllFolders(folder); };
		io.stop = [] { return GameLoadedOrStarting(); };

		// What each side was after the last sync with this place, in a list of the console's own.
		uint32_t hash = 2166136261u;
		for (const unsigned char c : place.path)
			hash = (hash ^ c) * 16777619u;
		const std::string list = Path::Combine(EmuFolders::Cache, fmt::format("card-sync-{:08x}.txt", hash));
		std::string text;
		PS5Backup::ReadWholeFile(list, &text);
		PS5CardSync::Records records = PS5CardSync::ParseRecords(text);

		const PS5CardSync::Result result = PS5CardSync::Sync(io, folders, &records, time(nullptr));
		if (result.listed)
			PS5Backup::WriteWholeFile(list, PS5CardSync::FormatRecords(records));
		for (const std::string& line : result.lines)
			ps5px_log("card sync: %s", line.c_str());
		const std::string summary = PS5CardSync::Summary(result, place.name);
		ps5px_log("card sync: %s", summary.c_str());
		PS5UI::SetBackgroundNote(std::string());
		if (result.brought > 0)
			PS5UI::PagesChanged();
		if (!quiet || result.sent > 0 || result.brought > 0 || result.failed > 0 || !result.listed)
			PS5UI::Toast(summary);
		s_syncing_cards = false;
	});
	if (!started)
	{
		s_syncing_cards = false;
		if (!quiet)
			PS5UI::Toast("The memory cards could not be synced now.");
	}
}

void GameClosed()
{
	if (Host::GetBaseBoolSettingValue("PS5", "CardSync", false))
		SyncCards(true);
	if (!Host::GetBaseBoolSettingValue("PS5", "AutoBackup", false))
		return;
	const Place place = BackupPlace();
	if (place.path.empty())
		return;
	// (Signed on the thread the cards are closed on: they are whole on the disk by now.)
	const std::string signature = PS5Backup::CardsSignature(EmuFolders::MemoryCards);
	if (signature == Host::GetBaseStringSettingValue("PS5", "AutoBackupSignature", ""))
		return;
	PS5Backup::SetStopCheck([] { return false; }); // (the cards are read in a moment; a game started meanwhile reads them too)
	ps5px_log("backup: the memory cards changed, an automatic backup to %s", place.path.c_str());
	PS5Backup::SetReplaceOlder(Host::GetBaseBoolSettingValue("PS5", "BackupReplace", false));
	const bool started = PS5Backup::StartBackup(BackupFolders(), place.path, false, true, Version(), [place, signature](const PS5Backup::Result& result) {
		ps5px_log("backup: automatic: %s (%s)", result.ok ? "done" : "not made", result.message.c_str());
		PS5UI::SetBackgroundNote(std::string());
		PS5UI::PagesChanged();
		if (result.ok)
		{
			Host::RunOnCPUThread([signature] {
				Host::SetBaseStringSettingValue("PS5", "AutoBackupSignature", signature.c_str());
				Host::CommitBaseSettingChanges();
			});
			PS5UI::Toast(fmt::format("Memory cards backed up to {}.", place.name));
		}
		else
		{
			PS5UI::Toast(fmt::format("The memory cards were not backed up: {}", result.message));
		}
	});
	if (started)
		PS5UI::SetBackgroundNote("Backing up the memory cards to " + place.name);
}

PS5Logs::Files LogFiles()
{
	PS5Logs::Files files{Path::Combine(ps5px_title_dir(), "crux2-boot.log"), Path::Combine(ps5px_title_dir(), "crux2-boot.previous.log"),
		Path::Combine(EmuFolders::Logs, "emulog.txt"), {}};
	// A profiling build's counts go with the logs (ps5_pgo.h).
	if (const std::string pgo = PS5Pgo::Folder(); !pgo.empty())
	{
		FileSystem::FindResultsArray found;
		FileSystem::FindFiles(pgo.c_str(), "*.profraw", FILESYSTEM_FIND_FILES, &found);
		for (const FILESYSTEM_FIND_DATA& file : found)
			files.extra.push_back(file.FileName);
	}
	return files;
}

void CopyLogs(const Place& place)
{
	if (s_copying.exchange(true, std::memory_order_acq_rel))
	{
		PS5UI::Toast("The logs are being copied already.");
		return;
	}
	Place* const job = new Place(place);
	const bool started = RunDetached(
		[](void* argument) -> void* {
			const Place* const to = static_cast<Place*>(argument);
			std::string message;
			const int written = PS5Logs::CopyTo(LogFiles(), to->path, &message);
			ps5px_log("logs: copy to %s: %d file(s), %s", to->path.c_str(), written, message.c_str());
			if (written > 0)
				PS5UI::Toast(fmt::format("{} On {}.", message, to->name));
			else
				PS5UI::Message("The logs were not copied", message);
			delete to;
			s_copying.store(false, std::memory_order_release);
			return nullptr;
		},
		job);
	if (!started)
	{
		delete job;
		s_copying.store(false, std::memory_order_release);
	}
}

std::string CrashNotice()
{
	const PS5Logs::Ending ending = PS5Logs::PreviousRun(LogFiles().previous);
	if (ending.crashed)
		ps5px_log("the run before this one stopped unexpectedly: %s%s%s", ending.what.c_str(), ending.game.empty() ? "" : ", game ",
			ending.game.c_str());
	return PS5Logs::Notice(ending);
}

void DownloadPatches()
{
	if (s_downloading.exchange(true, std::memory_order_acq_rel))
	{
		PS5UI::Toast("The patches are being downloaded already.");
		return;
	}
	const bool started = RunDetached(
		[](void*) -> void* {
			PS5UI::SetBackgroundNote("Downloading PCSX2's patches");
			std::string summary;
			const bool ok = PS5Patches::Download(&summary);
			PS5UI::SetBackgroundNote(std::string());
			PS5UI::PagesChanged();
			if (ok)
				PS5UI::Toast(summary);
			else
				PS5UI::Message("The patches were not downloaded", summary);
			s_downloading.store(false, std::memory_order_release);
			return nullptr;
		},
		nullptr);
	if (!started)
		s_downloading.store(false, std::memory_order_release);
}

namespace
{
// What a download on a thread of its own is told by: it stops when a game is started.
class QuietProgress final : public BaseProgressCallback
{
public:
	bool IsCancelled() const override { return GameLoadedOrStarting(); }
	void SetTitle(const char*) override {}
	void DisplayError(const char* message) override { ps5px_log("jobs: error: %s", message); }
	void DisplayWarning(const char* message) override { ps5px_log("jobs: warning: %s", message); }
	void DisplayInformation(const char*) override {}
	void DisplayDebugMessage(const char*) override {}
	void ModalError(const char* message) override { ps5px_log("jobs: error: %s", message); }
	bool ModalConfirmation(const char*) override { return false; }
	void ModalInformation(const char*) override {}
};
} // namespace

void FetchPictures(const std::string& serial)
{
	if (serial.empty())
	{
		PS5UI::Toast("This game has no serial to look its pictures up by.");
		return;
	}
	if (s_downloading.exchange(true, std::memory_order_acq_rel))
	{
		PS5UI::Toast("Something is being downloaded already: try again when it is done.");
		return;
	}
	std::string* const which = new std::string(serial);
	const bool started = RunDetached(
		[](void* argument) -> void* {
			std::string* const of = static_cast<std::string*>(argument);
			PS5UI::SetBackgroundNote("Fetching this game's pictures");
			QuietProgress progress;
			std::string summary;
			const int saved = PS5Covers::DownloadPictures(&progress, *of, true, &summary);
			PS5UI::SetBackgroundNote(std::string());
			if (saved > 0)
				PS5UI::PicturesChanged();
			PS5UI::Toast(summary);
			delete of;
			s_downloading.store(false, std::memory_order_release);
			return nullptr;
		},
		which);
	if (!started)
	{
		delete which;
		s_downloading.store(false, std::memory_order_release);
	}
}


void ShareCovers()
{
	const Place place = BackupPlace();
	if (place.path.empty())
	{
		PS5UI::Toast("No USB drive or network share to share the covers with.");
		return;
	}
	const std::string local = EmuFolders::Covers;
	const std::string remote = place.path + "/Crux2/covers";
	const bool started = RunDetached([local, remote, name = place.name] {
		PS5UI::SetBackgroundNote("Sharing the covers");
		const auto names = [](const std::string& folder) {
			FileSystem::FindResultsArray found;
			FileSystem::FindFiles(folder.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &found);
			std::vector<std::string> out;
			for (const FILESYSTEM_FIND_DATA& f : found)
				out.push_back(std::string(Path::GetFileName(f.FileName)));
			std::sort(out.begin(), out.end());
			return out;
		};
		FileSystem::CreateDirectoryPath(local.c_str(), false);
		const bool virtual_place = ps5vfs_is_virtual(remote.c_str()) != 0;
		if (!virtual_place)
			FileSystem::CreateDirectoryPath(remote.c_str(), true);
		const std::vector<std::string> here = names(local), there = names(remote);
		unsigned brought = 0, sent = 0, failed = 0;
		for (const std::string& file : there)
		{
			if (std::binary_search(here.begin(), here.end(), file))
				continue;
			std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(Path::Combine(remote, file).c_str());
			if (data.has_value() && PS5Platform::WriteFileSafely(Path::Combine(local, file).c_str(), data->data(), data->size()))
				brought++;
			else
				failed++;
		}
		for (const std::string& file : here)
		{
			if (std::binary_search(there.begin(), there.end(), file))
				continue;
			std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(Path::Combine(local, file).c_str());
			const std::string to = Path::Combine(remote, file);
			const bool ok = data.has_value() &&
							(virtual_place ? ps5vfs_store(to.c_str(), data->data(), data->size()) == 0 :
											 PS5Platform::WriteFileSafely(to.c_str(), data->data(), data->size()));
			ok ? sent++ : failed++;
		}
		PS5UI::SetBackgroundNote(std::string());
		ps5px_log("covers: shared with %s: %u brought here, %u sent there, %u failed", name.c_str(), brought, sent, failed);
		PS5UI::Toast(fmt::format("Covers shared with {}: {} brought here, {} sent there{}.", name, brought, sent,
			failed ? fmt::format(", {} could not be copied", failed) : std::string()));
		if (brought > 0)
			Host::RefreshGameListAsync(false);
	});
	if (!started)
		PS5UI::Toast("The covers could not be shared now.");
}
} // namespace PS5Jobs
