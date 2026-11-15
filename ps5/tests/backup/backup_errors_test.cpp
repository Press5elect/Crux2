// Crux2 - backups that meet a folder they cannot read (frontend/ps5_backup.cpp), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   backup_errors_test <work folder>
//
// A memory card folder whose listing breaks off with a read error, one whose entry cannot be looked
// at, and (when the test does not run as root, which reads everything) one the user may not read:
// the backup fails, says which folder, writes nothing, and with "each backup replaces the one before
// it" on, the good backup made before is still there, whole. A file that went while the folder was
// read is still no failure. The read errors are made by wrapping opendir, readdir and stat at the
// link (-Wl,--wrap), for the folders named BROKEN-*.

#include "ps5_backup.h"
#include "vfs/ps5_vfs_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C"
{
DIR* __real_opendir(const char* path);
struct dirent* __real_readdir(DIR* dir);
int __real_stat(const char* path, struct stat* st);

// The directory whose listing breaks off after its first entry, and the entry stat refuses.
static DIR* s_broken_dir = nullptr;
static int s_broken_reads = 0;
static bool s_vanished_seen = false;

DIR* __wrap_opendir(const char* path)
{
	DIR* const dir = __real_opendir(path);
	if (dir && std::strstr(path, "Card.ps2/BROKEN-LIST") != nullptr)
	{
		s_broken_dir = dir;
		s_broken_reads = 0;
	}
	return dir;
}

struct dirent* __wrap_readdir(DIR* dir)
{
	if (dir == s_broken_dir && dir != nullptr)
	{
		// ".", "..", one entry, and then a read error.
		if (++s_broken_reads > 3)
		{
			errno = EIO;
			return nullptr;
		}
	}
	return __real_readdir(dir);
}

int __wrap_stat(const char* path, struct stat* st)
{
	if (std::strstr(path, "Card.ps2/BROKEN-STAT/") != nullptr)
	{
		errno = EIO;
		return -1;
	}
	if (std::strstr(path, "vanished.ps2") != nullptr)
	{
		s_vanished_seen = true;
		errno = ENOENT;
		return -1;
	}
	return __real_stat(path, st);
}
}

// The virtual file layer's network backends are not in this test.
namespace ps5vfs
{
std::unique_ptr<Backend> CreateSMBBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateNFSBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateFTPBackend(unsigned, const char*, std::string* error) { *error = "not in this test"; return nullptr; }
} // namespace ps5vfs

static int s_failures = 0;
#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static void Put(const std::string& path, const std::string& data)
{
	FILE* const fp = std::fopen(path.c_str(), "wb");
	if (!fp)
	{
		std::fprintf(stderr, "cannot write %s\n", path.c_str());
		std::exit(2);
	}
	std::fwrite(data.data(), 1, data.size(), fp);
	std::fclose(fp);
}

static void Folder(const std::string& path)
{
	mkdir(path.c_str(), 0777);
}

static void Clear(const std::string& path)
{
	const std::string command = "chmod -R u+rwx '" + path + "' 2>/dev/null; rm -rf '" + path + "'";
	if (std::system(command.c_str()) != 0)
		std::exit(2);
}

static std::vector<PS5Backup::Snapshot> Listed(const std::string& place)
{
	std::vector<PS5Backup::Snapshot> snapshots;
	std::string error;
	CHECK(PS5Backup::List(place, &snapshots, &error));
	return snapshots;
}

// A good backup, then one whose memory card folder has `broken` in it: the second fails, the first stays.
static void Case(const std::string& work, const char* broken, bool make_unreadable)
{
	const std::string root = work + "/case-" + std::to_string(std::strlen(broken)) + "-" + std::string(broken).substr(7);
	Clear(root);
	Folder(root);
	PS5Backup::Folders folders;
	folders.memcards = root + "/memcards";
	folders.sstates = root + "/sstates";
	folders.replaced = root + "/replaced";
	Folder(folders.memcards);
	Folder(folders.sstates);
	const std::string place = root + "/place";
	Folder(place);
	Put(folders.memcards + "/Mcd001.ps2", std::string(8192, 'a'));
	Put(folders.sstates + "/SLUS-00000.01.p2s", std::string(4096, 's'));

	PS5Backup::SetReplaceOlder(true);
	const PS5Backup::Result good = PS5Backup::Backup(folders, place, true, false, "v-test", {}, 1700000000);
	CHECK(good.ok);
	CHECK(good.files == 2);

	// A folder card with a folder that cannot be read whole.
	const std::string card = folders.memcards + "/Card.ps2";
	Folder(card);
	const std::string bad = card + "/" + broken;
	Folder(bad);
	Put(bad + "/a.bin", "1");
	Put(bad + "/b.bin", "2");
	Put(bad + "/c.bin", "3");
	if (make_unreadable)
		chmod(bad.c_str(), 0);

	const PS5Backup::Result failed = PS5Backup::Backup(folders, place, true, false, "v-test", {}, 1700000600);
	CHECK(!failed.ok);
	CHECK(failed.message.find("could not be read") != std::string::npos);
	CHECK(failed.message.find(broken) != std::string::npos);

	// Only the good one is there, whole, with what it held.
	const std::vector<PS5Backup::Snapshot> snapshots = Listed(place);
	CHECK(snapshots.size() == 1);
	if (!snapshots.empty())
	{
		CHECK(snapshots[0].name == good.snapshot);
		CHECK(snapshots[0].complete);
		CHECK(snapshots[0].card_files == 1);
		CHECK(snapshots[0].states == 1);
	}
	if (make_unreadable)
		chmod(bad.c_str(), 0777);
	std::fprintf(stderr, "  %s: %s\n", broken, failed.message.c_str());
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: backup_errors_test <work folder>\n");
		return 2;
	}
	const std::string work = argv[1];
	Folder(work);

	Case(work, "BROKEN-LIST", false);
	Case(work, "BROKEN-STAT", false);
	if (geteuid() != 0)
		Case(work, "BROKEN-DENIED", true);
	else
		std::fprintf(stderr, "  (running as root: a folder without permission is read anyway, that case is skipped)\n");

	// A file that goes while the folder is read is no failure; a memory card folder not there yet is
	// an empty one (and a backup of the states alone).
	{
		const std::string root = work + "/vanished";
		Clear(root);
		Folder(root);
		PS5Backup::Folders folders;
		folders.memcards = root + "/memcards";
		folders.sstates = root + "/sstates";
		folders.replaced = root + "/replaced";
		Folder(folders.memcards);
		Folder(folders.sstates);
		Folder(root + "/place");
		Put(folders.memcards + "/Mcd001.ps2", std::string(100, 'a'));
		Put(folders.memcards + "/vanished.ps2", std::string(100, 'v'));
		const PS5Backup::Result result = PS5Backup::Backup(folders, root + "/place", false, false, "v-test", {}, 1700001200);
		CHECK(result.ok);
		CHECK(result.files == 1);
		CHECK(s_vanished_seen);

		folders.memcards = root + "/no-cards-yet";
		Put(folders.sstates + "/SLUS-00000.01.p2s", std::string(64, 's'));
		const PS5Backup::Result states = PS5Backup::Backup(folders, root + "/place", true, false, "v-test", {}, 1700001800);
		CHECK(states.ok);
		CHECK(states.files == 1);
	}

	if (s_failures == 0)
		std::printf("backup read errors: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
