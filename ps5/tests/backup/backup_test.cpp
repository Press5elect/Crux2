// Crux2 - the backups of memory cards and saved states (frontend/ps5_backup.cpp), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   backup_test <work folder>                     a place that is a folder of the PC
//   backup_test <work folder> smb <url> <folder>  and a share of a real server, which serves <folder>
//
// What a backup holds is compared file by file with what was backed up; a restore with what the
// backup holds; and what a restore replaces has to be where it says.

#include "ps5_backup.h"
#include "vfs/ps5_vfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static std::mt19937 s_random(5);

static void Put(const std::string& path, size_t size)
{
	std::string data(size, '\0');
	for (char& c : data)
		c = static_cast<char>(s_random());
	FILE* const fp = fopen(path.c_str(), "wb");
	if (!fp)
	{
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		std::exit(2);
	}
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
}

static std::string Get(const std::string& path)
{
	std::string data;
	FILE* const fp = fopen(path.c_str(), "rb");
	if (!fp)
		return "<missing>";
	char block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), fp)) > 0)
		data.append(block, got);
	fclose(fp);
	return data;
}

// Every file under a folder, by its path inside it, with its bytes.
static void Tree(const std::string& root, const std::string& below, std::map<std::string, std::string>* out)
{
	DIR* const dir = opendir((below.empty() ? root : root + "/" + below).c_str());
	if (!dir)
		return;
	while (const struct dirent* const entry = readdir(dir))
	{
		if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
			continue;
		const std::string relative = below.empty() ? entry->d_name : below + "/" + entry->d_name;
		struct stat st;
		if (stat((root + "/" + relative).c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			Tree(root, relative, out);
		else
			(*out)[relative] = Get(root + "/" + relative);
	}
	closedir(dir);
}

static std::map<std::string, std::string> Tree(const std::string& root)
{
	std::map<std::string, std::string> out;
	Tree(root, std::string(), &out);
	return out;
}

static void Run(const std::string& command)
{
	if (std::system(command.c_str()) != 0)
	{
		std::fprintf(stderr, "failed: %s\n", command.c_str());
		std::exit(2);
	}
}

// One round over a place: `place` as the backup code names it, `served` the PC folder behind it.
static void Round(const std::string& work, const std::string& place, const std::string& served)
{
	using namespace PS5Backup;
	const std::string data = work + "/console";
	Run("rm -rf '" + data + "' '" + served + "/" + FOLDER + "' && mkdir -p '" + data + "/memcards/Folder card.ps2/BESLES-00000' '" + data +
		"/sstates'");
	const Folders folders{data + "/memcards", data + "/sstates", data + "/backup-before-restore"};
	const std::string version = "v-test";

	// Nothing to back up yet.
	{
		const Result none = Backup(folders, place, true, false, version);
		CHECK(!none.ok && none.files == 0);
		CHECK(Tree(served + "/" + FOLDER).empty());
	}

	Put(folders.memcards + "/Mcd001.ps2", 3 * 1024 * 1024 + 17);
	Put(folders.memcards + "/Mcd002.ps2", 70000);
	Put(folders.memcards + "/Folder card.ps2/_pcsx2_superblock", 8192);
	Put(folders.memcards + "/Folder card.ps2/BESLES-00000/icon.sys", 964);
	Put(folders.memcards + "/Folder card.ps2/BESLES-00000/save.bin", 0);
	Put(folders.memcards + "/cut short.ps2.part", 100);
	Put(folders.sstates + "/SLES-00000 (0BADF00D).01.p2s", 400000);
	Put(folders.sstates + "/SLES-00000 (0BADF00D).02.p2s", 250000);
	Put(folders.sstates + "/SLES-00000 (0BADF00D).01.p2s.backup", 300000);
	Put(folders.sstates + "/SLUS-11111 (12345678).resume.p2s", 123456);
	// (The game as it was before a state was last loaded, "Undo the last load": of one sitting, and
	// not backed up.)
	Put(folders.sstates + "/SLUS-11111 (12345678).undo.p2s", 222222);
	const auto cards_then = Tree(folders.memcards);
	const auto states_then = Tree(folders.sstates);
	const std::string signature_then = CardsSignature(folders.memcards);
	CHECK(signature_then == CardsSignature(folders.memcards));

	// ---- a backup of everything
	const time_t first_time = 1791288000; // 2026-10-06, some hour of the day wherever this runs
	int steps = 0, last_total = 0;
	Watch watch;
	watch.step = [&](int, int total) {
		steps++;
		last_total = total;
	};
	const Result first = Backup(folders, place, true, false, version, watch, first_time);
	CHECK(first.ok);
	CHECK(first.files == 8);
	CHECK(steps == 9 && last_total == 8);
	CHECK(first.snapshot == NameFor(first_time, false));
	CHECK(first.message.find("5 memory card files") != std::string::npos && first.message.find("3 saved states") != std::string::npos);
	const std::string held = served + "/" + FOLDER + "/" + first.snapshot;
	{
		auto cards = Tree(held + "/memcards");
		auto expected = cards_then;
		expected.erase("cut short.ps2.part");
		CHECK(cards == expected);
		auto states = Tree(held + "/sstates");
		auto expected_states = states_then;
		expected_states.erase("SLES-00000 (0BADF00D).01.p2s.backup");
		expected_states.erase("SLUS-11111 (12345678).undo.p2s");
		CHECK(states == expected_states);
		CHECK(states.count("SLUS-11111 (12345678).resume.p2s") == 1);
		const std::string manifest = Get(held + "/backup.txt");
		CHECK(manifest.compare(0, 13, "Crux2 backup\n") == 0);
		CHECK(manifest.find("\nby: v-test\n") != std::string::npos);
		CHECK(manifest.find("\nkind: manual\n") != std::string::npos);
		CHECK(manifest.find("\nmemory card files: 5\n") != std::string::npos);
		CHECK(manifest.find("\nsaved states: 3\n") != std::string::npos);
		// Nothing a transfer leaves behind.
		for (const auto& [name, bytes] : Tree(served + "/" + FOLDER))
			CHECK(name.size() < 5 || name.compare(name.size() - 5, 5, ".part") != 0);
	}

	// ---- the list
	std::vector<Snapshot> snapshots;
	std::string error;
	CHECK(List(place, &snapshots, &error));
	CHECK(snapshots.size() == 1);
	if (snapshots.size() == 1)
	{
		const Snapshot& s = snapshots[0];
		CHECK(s.complete && !s.automatic && s.card_files == 5 && s.states == 3);
		CHECK(s.name == first.snapshot && s.when == first_time);
		CHECK(s.bytes == first.bytes && s.bytes == 3 * 1024 * 1024 + 17 + 70000 + 8192 + 964 + 400000 + 250000 + 123456);
	}
	// A folder that is no backup, and a backup cut short, among them.
	// (Made through the layer where the place is a share: it keeps a listing for some seconds, and
	// would not see what the server's own folder was given behind its back.)
	if (ps5vfs_is_virtual(place.c_str()))
	{
		CHECK(ps5vfs_make_folders((place + "/" + FOLDER + "/holiday pictures").c_str()) == 0);
		CHECK(ps5vfs_make_folders((place + "/" + FOLDER + "/2026-10-07_10-00-00/memcards").c_str()) == 0);
		CHECK(ps5vfs_store((place + "/" + FOLDER + "/2026-10-07_10-00-00/memcards/Mcd001.ps2").c_str(), "card", 4) == 0);
	}
	else
	{
		Run("mkdir -p '" + served + "/" + FOLDER + "/holiday pictures' '" + served + "/" + FOLDER + "/2026-10-07_10-00-00/memcards'");
		Put(served + "/" + FOLDER + "/2026-10-07_10-00-00/memcards/Mcd001.ps2", 500);
	}
	CHECK(List(place, &snapshots, &error) && snapshots.size() == 2);
	if (snapshots.size() == 2)
	{
		CHECK(snapshots[0].name == "2026-10-07_10-00-00" && !snapshots[0].complete);
		CHECK(snapshots[1].name == first.snapshot && snapshots[1].complete);
		// One that was cut short is not restored, and changes nothing.
		const Result refused = Restore(folders, snapshots[0]);
		CHECK(!refused.ok && refused.files == 0);
		CHECK(Tree(folders.memcards) == cards_then);
	}

	// ---- the console goes on: a card is written to, a state deleted, another made
	Put(folders.memcards + "/Mcd001.ps2", 3 * 1024 * 1024 + 17);
	Put(folders.memcards + "/Folder card.ps2/BESLES-00000/icon.sys", 1000);
	Put(folders.memcards + "/New card.ps2", 4096);
	unlink((folders.sstates + "/SLES-00000 (0BADF00D).02.p2s").c_str());
	Put(folders.sstates + "/SLPM-22222 (00000001).05.p2s", 2000);
	CHECK(CardsSignature(folders.memcards) != signature_then);
	const auto cards_changed = Tree(folders.memcards);
	const auto states_changed = Tree(folders.sstates);

	// ---- and the backup is restored
	if (snapshots.size() == 2)
	{
		const Result restored = Restore(folders, snapshots[1]);
		CHECK(restored.ok && restored.files == 8);
		CHECK(restored.message.find("backup-before-restore") != std::string::npos);
		auto cards_now = Tree(folders.memcards);
		// The backup's files are back as they were; a file the backup does not have stays.
		for (const auto& [name, bytes] : cards_then)
		{
			if (name == "cut short.ps2.part")
				continue;
			CHECK(cards_now[name] == bytes);
		}
		CHECK(cards_now["New card.ps2"] == cards_changed.at("New card.ps2"));
		auto states_now = Tree(folders.sstates);
		CHECK(states_now["SLES-00000 (0BADF00D).02.p2s"] == states_then.at("SLES-00000 (0BADF00D).02.p2s"));
		CHECK(states_now["SLPM-22222 (00000001).05.p2s"] == states_changed.at("SLPM-22222 (00000001).05.p2s"));
		CHECK(states_now["SLES-00000 (0BADF00D).01.p2s.backup"] == states_then.at("SLES-00000 (0BADF00D).01.p2s.backup"));
		// What the restore replaced is kept: the files as they were just before it.
		auto kept = Tree(folders.replaced);
		CHECK(kept["memcards/Mcd001.ps2"] == cards_changed.at("Mcd001.ps2"));
		CHECK(kept["memcards/Folder card.ps2/BESLES-00000/icon.sys"] == cards_changed.at("Folder card.ps2/BESLES-00000/icon.sys"));
		CHECK(kept.count("memcards/New card.ps2") == 0);
		CHECK(kept.count("sstates/SLES-00000 (0BADF00D).02.p2s") == 0); // it was not there to be replaced
		CHECK(kept["sstates/SLES-00000 (0BADF00D).01.p2s"] == states_changed.at("SLES-00000 (0BADF00D).01.p2s"));
		for (const auto& [name, bytes] : Tree(data))
			CHECK(name.find(".part") == std::string::npos || name == "memcards/cut short.ps2.part");

		// A second restore: what the first one kept makes room.
		Put(folders.memcards + "/Mcd002.ps2", 10);
		const std::string small = Get(folders.memcards + "/Mcd002.ps2");
		CHECK(Restore(folders, snapshots[1]).ok);
		kept = Tree(folders.replaced);
		CHECK(kept["memcards/Mcd002.ps2"] == small);
		CHECK(Get(folders.memcards + "/Mcd002.ps2") == cards_then.at("Mcd002.ps2"));
	}

	// ---- a backup that is stopped leaves nothing
	{
		int seen = 0;
		Watch stopping;
		stopping.stop = [&seen] { return ++seen > 2; };
		const Result stopped = Backup(folders, place, true, false, version, stopping, first_time + 100);
		CHECK(!stopped.ok);
		struct stat st;
		CHECK(stat((served + "/" + FOLDER + "/" + NameFor(first_time + 100, false)).c_str(), &st) != 0);
	}

	// ---- automatic backups: the cards only, the newest ten kept, manual ones never touched
	for (int i = 0; i < 13; i++)
	{
		const Result automatic = Backup(folders, place, true, true, version, {}, first_time + 1000 + i * 60);
		CHECK(automatic.ok);
		CHECK(automatic.snapshot == NameFor(first_time + 1000 + i * 60, true));
	}
	CHECK(List(place, &snapshots, &error));
	{
		int automatic = 0, manual = 0;
		for (const Snapshot& s : snapshots)
		{
			(s.automatic ? automatic : manual)++;
			if (s.automatic)
			{
				CHECK(s.complete && s.states == 0 && s.card_files == 6);
				CHECK(s.when >= first_time + 1000 + 3 * 60); // the three oldest went
				struct stat st;
				CHECK(stat((served + "/" + FOLDER + "/" + s.name + "/sstates").c_str(), &st) != 0);
			}
		}
		CHECK(automatic == AUTOMATIC_KEPT);
		CHECK(manual == 2); // the whole one and the one cut short
		// Newest first.
		for (size_t i = 1; i < snapshots.size(); i++)
			CHECK(snapshots[i - 1].name > snapshots[i].name);
	}

	// ---- the job's thread: one at a time, the list read again when a backup is over
	{
		SetStopCheck([] { return false; });
		Result told;
		bool called = false;
		CHECK(StartBackup(folders, place, true, false, version, [&](const Result& result) {
			told = result;
			called = true;
		}));
		WaitUntilIdle();
		// (`done` runs after the state says idle: wait for it.)
		for (int i = 0; i < 2000 && !called; i++)
			usleep(1000);
		CHECK(called && told.ok);
		const State state = CurrentState();
		CHECK(!state.busy && state.doing.empty() && state.listed && state.listed_place == place && state.list_error.empty());
		CHECK(state.snapshots.size() == snapshots.size() + 1);
		CHECK(StartList(place));
		WaitUntilIdle();
		CHECK(CurrentState().generation > state.generation);
	}

	// ---- a backup of the builds before v0.6.0 (the title's name was another then, and so was the
	// folder's and the first line of backup.txt): listed among the others, and restored
	{
		const std::string old = std::string(FOLDER_BEFORE) + "/2025-01-02_03-04-05";
		const std::string text = "PSPCSX2 backup\nmade: 2025-01-02 03:04:05\nkind: manual\nmemory card files: 1\nsaved states: 0\n"
								 "files written: 1\nbytes: 4\n";
		const auto store = [&](const std::string& name, const std::string& bytes) {
			if (ps5vfs_is_virtual(place.c_str()))
				return ps5vfs_store((place + "/" + name).c_str(), bytes.data(), bytes.size()) == 0;
			FILE* const fp = fopen((served + "/" + name).c_str(), "wb");
			const bool ok = fp != nullptr && fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
			if (fp)
				fclose(fp);
			return ok;
		};
		if (ps5vfs_is_virtual(place.c_str()))
			CHECK(ps5vfs_make_folders((place + "/" + old + "/memcards").c_str()) == 0);
		else
			Run("mkdir -p '" + served + "/" + old + "/memcards'");
		CHECK(store(old + "/memcards/Card of before.ps2", "card"));
		CHECK(store(old + "/backup.txt", text));
		const size_t before = snapshots.size() + 1; // (the job above made one more)
		CHECK(List(place, &snapshots, &error) && snapshots.size() == before + 1);
		// The oldest of them all: the last of the list.
		if (!snapshots.empty())
		{
			const Snapshot& last = snapshots.back();
			CHECK(last.name == "2025-01-02_03-04-05" && last.complete && !last.automatic && last.card_files == 1 && last.bytes == 4);
			CHECK(last.path.find(FOLDER_BEFORE) != std::string::npos);
			const Result restored = Restore(folders, last);
			CHECK(restored.ok && restored.files == 1);
			CHECK(Get(folders.memcards + "/Card of before.ps2") == "card");
		}
		// A new backup goes to the folder of this name, never to the old one.
		const Result next = Backup(folders, place, true, false, version, {}, first_time + 5000);
		CHECK(next.ok);
		struct stat st;
		CHECK(stat((served + "/" + FOLDER + "/" + next.snapshot + "/backup.txt").c_str(), &st) == 0);
		CHECK(stat((served + "/" + FOLDER_BEFORE + "/" + next.snapshot).c_str(), &st) != 0);
	}

	// ---- "each backup replaces the one before it": a backup that was made whole takes the older
	// ones of its kind away, and those of the other kind are left; one that was cut short takes
	// nothing away
	{
		const auto count = [&](bool automatic, bool complete_only) {
			int n = 0;
			CHECK(List(place, &snapshots, &error));
			for (const Snapshot& s : snapshots)
				n += (s.automatic == automatic && (s.complete || !complete_only)) ? 1 : 0;
			return n;
		};
		const int automatic_before = count(true, false);
		CHECK(automatic_before == AUTOMATIC_KEPT && count(false, false) > 2);
		SetReplaceOlder(true);
		// Cut short: everything stays.
		const int manual_before = count(false, true);
		Watch cut;
		cut.stop = [] { return true; };
		const Result stopped = Backup(folders, place, true, false, version, cut, first_time + 5900);
		CHECK(!stopped.ok);
		CHECK(count(false, true) == manual_before && count(true, false) == automatic_before);
		// Made by hand: the one by hand, and the automatic ones as they were.
		const Result manual = Backup(folders, place, true, false, version, {}, first_time + 6000);
		CHECK(manual.ok);
		CHECK(count(false, false) == 1 && count(true, false) == automatic_before);
		// (The one just made is the one kept, though one made by the job above carries a later
		// time in its name: a clock that was set back must not cost the newest backup.)
		{
			bool there = false;
			for (const Snapshot& s : snapshots)
				there |= (!s.automatic && s.name == manual.snapshot && s.complete);
			CHECK(there);
		}
		// (Also the one of the builds before v0.6.0, in its folder of the old name.)
		for (const Snapshot& s : snapshots)
			CHECK(s.path.find(FOLDER_BEFORE) == std::string::npos);
		// Made by itself: one of each.
		const Result automatic = Backup(folders, place, true, true, version, {}, first_time + 6060);
		CHECK(automatic.ok);
		CHECK(count(true, false) == 1 && count(false, false) == 1);
		// And the two that are left restore what was backed up.
		CHECK(List(place, &snapshots, &error) && snapshots.size() == 2);
		for (const Snapshot& s : snapshots)
			CHECK(s.complete && Restore(folders, s).ok);
		// Off again: an automatic one more is kept beside it.
		SetReplaceOlder(false);
		CHECK(Backup(folders, place, true, true, version, {}, first_time + 6120).ok);
		CHECK(Backup(folders, place, true, false, version, {}, first_time + 6180).ok);
		CHECK(count(true, false) == 2 && count(false, false) == 2);
	}
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: backup_test <work folder> [smb <url> <served folder>]\n");
		return 2;
	}
	const std::string work = argv[1];
	using namespace PS5Backup;

	// Names.
	{
		bool automatic = true;
		const time_t when = 1791288000;
		CHECK(WhenOf(NameFor(when, false), &automatic) == when && !automatic);
		CHECK(WhenOf(NameFor(when, true), &automatic) == when && automatic);
		CHECK(NameFor(when, true).size() == 24 && NameFor(when, false).size() == 19);
		CHECK(WhenOf("holiday pictures", nullptr) == 0);
		CHECK(WhenOf("2026-10-06_14-32-07_old", nullptr) == 0);
		CHECK(WhenOf("2026-13-06_14-32-07", nullptr) == 0);
		CHECK(WhenOf("2026-10-06", nullptr) == 0);
	}

	Run("rm -rf '" + work + "/usb' && mkdir -p '" + work + "/usb'");
	std::fprintf(stderr, "a folder of the PC\n");
	Round(work, work + "/usb", work + "/usb");

	// A place that is not there, and one that cannot be written to.
	{
		const Folders folders{work + "/console/memcards", work + "/console/sstates", work + "/console/backup-before-restore"};
		std::vector<Snapshot> snapshots;
		std::string error;
		Run("rm -rf '" + work + "/no such drive'");
		CHECK(!List(work + "/no such drive", &snapshots, &error) && !error.empty());
		const Result nowhere = Backup(folders, work + "/no such drive/deeper", false, false, "v-test");
		CHECK(nowhere.ok || !nowhere.message.empty()); // (a PC folder can be made anywhere; a drive that is gone cannot)
		if (geteuid() != 0)
		{
			Run("mkdir -p '" + work + "/locked' && chmod 555 '" + work + "/locked'");
			const Result locked = Backup(folders, work + "/locked", false, false, "v-test");
			CHECK(!locked.ok && locked.message.find("did not let") != std::string::npos);
		}
	}

	if (argc == 5 && std::strcmp(argv[2], "smb") == 0)
	{
		std::fprintf(stderr, "a share\n");
		CHECK(ps5vfs_mount_smb("backups", argv[3]) == 0);
		Round(work, "/smb/backups", argv[4]);
	}
	if (argc == 5 && std::strcmp(argv[2], "smb-readonly") == 0)
	{
		std::fprintf(stderr, "a share nobody may write to\n");
		CHECK(ps5vfs_mount_smb("readonly", argv[3]) == 0);
		const Folders folders{work + "/console/memcards", work + "/console/sstates", work + "/console/backup-before-restore"};
		const Result refused = Backup(folders, "/smb/readonly", true, false, "v-test");
		CHECK(!refused.ok);
		CHECK(refused.message.find("network.cfg") != std::string::npos);
		std::fprintf(stderr, "  said: %s\n", refused.message.c_str());
		std::vector<Snapshot> snapshots;
		std::string error;
		CHECK(List("/smb/readonly", &snapshots, &error) && snapshots.empty());
	}

	std::fprintf(stderr, "backup: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
