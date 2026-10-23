// Crux2 - backups of the memory cards and the saved states (ps5_backup.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_backup.h"
#include "ps5_priority.h"

#include "vfs/ps5_vfs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PS5Backup
{
namespace
{
// ------------------------------------------------------------------------------------ files
//
// One set of calls for both kinds of place: a path under a mount of the virtual file layer goes
// through it, any other to the console's own file system (with descriptors, not stdio: the build
// binds stdio to the layer for PCSX2's sake, and these must not depend on which file is which).

bool IsVirtual(const std::string& path)
{
	return ps5vfs_is_virtual(path.c_str()) != 0;
}

std::string Join(const std::string& folder, const std::string& name)
{
	if (folder.empty())
		return name;
	return (folder.back() == '/') ? folder + name : folder + "/" + name;
}

struct Item
{
	std::string name;
	bool folder = false;
	uint64_t size = 0;
	int64_t mtime = 0;
};

bool ListFolder(const std::string& path, std::vector<Item>* items)
{
	items->clear();
	if (IsVirtual(path))
	{
		ps5vfs_dir* const dir = ps5vfs_opendir(path.c_str());
		if (!dir)
			return false;
		struct ps5vfs_dirent entry;
		while (ps5vfs_readdir(dir, &entry) == 1)
			items->push_back({entry.name, entry.is_directory != 0, static_cast<uint64_t>(std::max<int64_t>(entry.size, 0)), entry.mtime});
		ps5vfs_closedir(dir);
	}
	else
	{
		DIR* const dir = opendir(path.c_str());
		if (!dir)
			return false;
		while (const struct dirent* const entry = readdir(dir))
		{
			if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
				continue;
			struct stat st;
			if (stat(Join(path, entry->d_name).c_str(), &st) != 0)
				continue;
			items->push_back({entry->d_name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size),
				static_cast<int64_t>(st.st_mtime)});
		}
		closedir(dir);
	}
	std::sort(items->begin(), items->end(), [](const Item& a, const Item& b) { return a.name < b.name; });
	return true;
}

bool Exists(const std::string& path, bool* folder = nullptr)
{
	struct stat st;
	const int result = IsVirtual(path) ? ps5vfs_stat(path.c_str(), &st) : stat(path.c_str(), &st);
	if (result != 0)
		return false;
	if (folder)
		*folder = S_ISDIR(st.st_mode);
	return true;
}

bool MakeFolders(const std::string& path)
{
	if (IsVirtual(path))
		return ps5vfs_make_folders(path.c_str()) == 0;

	for (size_t end = 1; end <= path.size(); end++)
	{
		if (end != path.size() && path[end] != '/')
			continue;
		const std::string part = path.substr(0, end);
		if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
		{
			struct stat st;
			if (stat(part.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
				return false;
		}
	}
	return true;
}

bool ReadFile(const std::string& path, std::string* data)
{
	data->clear();
	if (IsVirtual(path))
	{
		FILE* const fp = ps5vfs_fopen(path.c_str(), "rb");
		if (!fp)
			return false;
		struct stat st;
		if (ps5vfs_fstat(fp, &st) == 1 && st.st_size > 0)
			data->reserve(static_cast<size_t>(st.st_size));
		std::vector<char> block(1 << 20);
		size_t got;
		while ((got = ps5vfs_fread(block.data(), 1, block.size(), fp)) > 0)
			data->append(block.data(), got);
		const bool failed = ps5vfs_ferror(fp) != 0;
		ps5vfs_fclose(fp);
		if (failed)
			errno = EIO;
		return !failed;
	}

	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	struct stat st;
	if (fstat(fd, &st) == 0 && st.st_size > 0)
		data->reserve(static_cast<size_t>(st.st_size));
	std::vector<char> block(1 << 20);
	for (;;)
	{
		const ssize_t got = read(fd, block.data(), block.size());
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
		{
			const int error = errno;
			close(fd);
			errno = error;
			return false;
		}
		if (got == 0)
			break;
		data->append(block.data(), static_cast<size_t>(got));
	}
	close(fd);
	return true;
}

// The file holds exactly these bytes afterwards, or the one that was there is as it was.
bool WriteFile(const std::string& path, const std::string& data)
{
	if (IsVirtual(path))
		return ps5vfs_store(path.c_str(), data.data(), data.size()) == 0;

	const std::string part = path + ".part";
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	size_t written = 0;
	while (written < data.size())
	{
		const ssize_t result = write(fd, data.data() + written, std::min<size_t>(data.size() - written, 1 << 20));
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
		{
			const int error = (result < 0) ? errno : EIO;
			close(fd);
			unlink(part.c_str());
			errno = error;
			return false;
		}
		written += static_cast<size_t>(result);
	}
	// (The old file goes first: on an exFAT drive a rename onto a file can report success and
	// keep the old one.)
	if (close(fd) != 0 || (unlink(path.c_str()) != 0 && errno != ENOENT) || rename(part.c_str(), path.c_str()) != 0)
	{
		const int error = errno;
		unlink(part.c_str());
		errno = error;
		return false;
	}
	return true;
}

bool RemoveOne(const std::string& path, bool folder)
{
	if (IsVirtual(path))
		return ps5vfs_remove(path.c_str(), folder ? 1 : 0) == 0;
	const int result = folder ? rmdir(path.c_str()) : unlink(path.c_str());
	return result == 0 || errno == ENOENT;
}

// A folder and all that is in it.
bool RemoveTree(const std::string& path)
{
	std::vector<Item> items;
	if (!ListFolder(path, &items))
		return !Exists(path);
	bool ok = true;
	for (const Item& item : items)
		ok &= item.folder ? RemoveTree(Join(path, item.name)) : RemoveOne(Join(path, item.name), false);
	return RemoveOne(path, true) && ok;
}

struct Found
{
	std::string relative; // "Mcd001.ps2", "Card (folder).ps2/BESLES-00000/icon.sys"
	uint64_t size = 0;
	int64_t mtime = 0;
};

// Every file under a folder, by its path inside it. What a transfer leaves when it is cut short
// (".part") is no file of anyone's.
void Collect(const std::string& root, const std::string& below, bool deep, std::vector<Found>* out)
{
	std::vector<Item> items;
	if (!ListFolder(below.empty() ? root : Join(root, below), &items))
		return;
	for (const Item& item : items)
	{
		const std::string relative = below.empty() ? item.name : below + "/" + item.name;
		if (item.folder)
		{
			if (deep)
				Collect(root, relative, deep, out);
			continue;
		}
		if (item.name.size() > 5 && item.name.compare(item.name.size() - 5, 5, ".part") == 0)
			continue;
		out->push_back({relative, item.size, item.mtime});
	}
}

bool EndsWith(const std::string& text, const char* end)
{
	const size_t length = std::strlen(end);
	return text.size() >= length && text.compare(text.size() - length, length, end) == 0;
}

std::string Parent(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

// Why a place did not take or give a file, as the user can act on it.
std::string Why(int error, const std::string& place)
{
	const bool share = IsVirtual(place) || place.compare(0, 5, "/smb/") == 0;
	switch (error)
	{
		case EACCES:
		case EPERM:
		case EROFS:
			return share ? "The share did not let Crux2 write there: the account of network.cfg needs permission to write to it." :
			               "The drive did not let Crux2 write to it.";
		case ENOSPC:
#ifdef EDQUOT
		case EDQUOT:
#endif
			return "There is no room left there.";
		case ENOENT:
			return share ? "The share's folder is not there any more." : "The drive is not there any more.";
		default:
			return share ? "The share could not be reached, or it stopped answering." : "The drive could not be written to or read.";
	}
}

std::string Size(uint64_t bytes)
{
	char out[32];
	if (bytes >= (1ull << 30))
		std::snprintf(out, sizeof(out), "%.2f GB", static_cast<double>(bytes) / (1ull << 30));
	else if (bytes >= (1ull << 20))
		std::snprintf(out, sizeof(out), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
	else
		std::snprintf(out, sizeof(out), "%.0f KB", static_cast<double>(bytes) / 1024.0);
	return out;
}

std::string Count(int n, const char* one, const char* many)
{
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

// "memory card files: 3" in backup.txt.
bool ReadNumber(const std::string& text, const char* key, uint64_t* value)
{
	const std::string label = std::string(key) + ": ";
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		if (text.compare(at, label.size(), label) == 0)
		{
			*value = std::strtoull(text.c_str() + at + label.size(), nullptr, 10);
			return true;
		}
		at = end + 1;
	}
	return false;
}

std::atomic_bool s_replace_older{false};

// After a backup was made whole: the automatic backups beyond the newest few, and the ones that
// were cut short, go. With "each backup replaces the one before it", so do all of its kind but
// the one just made. (That one is kept by its name, not for being the newest: a console whose
// clock was set back makes backups that sort before the ones it has.)
void Prune(const std::string& place, bool automatic, const std::string& made)
{
	std::vector<Snapshot> snapshots;
	std::string error;
	if (!List(place, &snapshots, &error))
		return;
	const bool replace = s_replace_older.load(std::memory_order_acquire);
	if (!automatic && !replace)
		return;
	const int keep = replace ? 1 : AUTOMATIC_KEPT;
	int kept = 1; // the one just made
	// (The newest first.)
	for (const Snapshot& snapshot : snapshots)
	{
		if (snapshot.automatic != automatic || snapshot.name == made)
			continue;
		if (snapshot.complete && kept < keep)
		{
			kept++;
			continue;
		}
		RemoveTree(snapshot.path);
	}
}
} // namespace

void SetReplaceOlder(bool on)
{
	s_replace_older.store(on, std::memory_order_release);
}

bool ReadWholeFile(const std::string& path, std::string* data)
{
	return ReadFile(path, data);
}

bool WriteWholeFile(const std::string& path, const std::string& data)
{
	return WriteFile(path, data);
}

bool MakeAllFolders(const std::string& path)
{
	return MakeFolders(path);
}

bool ListFiles(const std::string& folder, std::vector<std::pair<std::string, uint64_t>>* files)
{
	files->clear();
	std::vector<Item> items;
	if (!ListFolder(folder, &items))
		return false;
	for (const Item& item : items)
	{
		if (!item.folder)
			files->emplace_back(item.name, item.size);
	}
	return true;
}

bool PathExists(const std::string& path)
{
	return Exists(path);
}

std::string WhyNot(int error, const std::string& place)
{
	return Why(error, place);
}

// ------------------------------------------------------------------------------------ names

std::string NameFor(time_t when, bool automatic)
{
	char out[48] = "";
	struct tm local;
	if (localtime_r(&when, &local) != nullptr)
		strftime(out, sizeof(out), "%Y-%m-%d_%H-%M-%S", &local);
	return automatic ? std::string(out) + "_auto" : std::string(out);
}

time_t WhenOf(const std::string& name, bool* automatic)
{
	int y, mo, d, h, mi, s;
	char rest[8] = "";
	const int fields = std::sscanf(name.c_str(), "%4d-%2d-%2d_%2d-%2d-%2d%7s", &y, &mo, &d, &h, &mi, &s, rest);
	if (fields < 6 || name.size() < 19 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
		return 0;
	const bool is_automatic = (std::strcmp(rest, "_auto") == 0);
	if (rest[0] != '\0' && !is_automatic)
		return 0;
	if (automatic)
		*automatic = is_automatic;
	struct tm local = {};
	local.tm_year = y - 1900;
	local.tm_mon = mo - 1;
	local.tm_mday = d;
	local.tm_hour = h;
	local.tm_min = mi;
	local.tm_sec = s;
	local.tm_isdst = -1;
	const time_t when = mktime(&local);
	return (when > 0) ? when : 1;
}

std::string CardsSignature(const std::string& memcards)
{
	std::vector<Found> files;
	Collect(memcards, std::string(), true, &files);
	uint64_t hash = 1469598103934665603ull;
	const auto mix = [&hash](const void* data, size_t size) {
		const unsigned char* const bytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; i++)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
	};
	for (const Found& file : files)
	{
		mix(file.relative.data(), file.relative.size() + 1);
		mix(&file.size, sizeof(file.size));
		mix(&file.mtime, sizeof(file.mtime));
	}
	char out[40];
	std::snprintf(out, sizeof(out), "%zu-%016llx", files.size(), static_cast<unsigned long long>(hash));
	return out;
}

// ------------------------------------------------------------------------------------ backup

Result Backup(const Folders& folders, const std::string& place, bool states, bool automatic, const std::string& version, const Watch& watch,
	time_t when)
{
	Result result;
	struct Source
	{
		const char* into;
		std::string local;
		Found file;
	};
	std::vector<Source> sources;
	{
		std::vector<Found> cards;
		Collect(folders.memcards, std::string(), true, &cards);
		for (Found& file : cards)
			sources.push_back({"memcards", Join(folders.memcards, file.relative), std::move(file)});
	}
	const int card_files = static_cast<int>(sources.size());
	int state_files = 0;
	if (states && !automatic)
	{
		std::vector<Found> found;
		Collect(folders.sstates, std::string(), false, &found);
		for (Found& file : found)
		{
			// (PCSX2 keeps what a slot held before as "<state>.backup": the slot's past, not saved again.)
			// (And the game as it was before a state was last loaded, "Undo the last load", is of
			// one sitting only.)
			if (EndsWith(file.relative, ".backup") || EndsWith(file.relative, ".undo.p2s"))
				continue;
			sources.push_back({"sstates", Join(folders.sstates, file.relative), std::move(file)});
			state_files++;
		}
	}
	if (sources.empty())
	{
		result.message = "There is nothing to back up yet: no memory card and no saved state.";
		return result;
	}

	const time_t now = (when != 0) ? when : time(nullptr);
	result.snapshot = NameFor(now, automatic);
	const std::string base = Join(Join(place, FOLDER), result.snapshot);
	const auto fail = [&](const std::string& what) {
		const int error = errno;
		RemoveTree(base);
		result.ok = false;
		result.message = what + " " + Why(error, place);
		return result;
	};

	if (!MakeFolders(Join(base, "memcards")) || (state_files > 0 && !MakeFolders(Join(base, "sstates"))))
		return fail("The backup's folder could not be made.");

	const int total = static_cast<int>(sources.size());
	std::string data;
	std::string made = base;
	for (int i = 0; i < total; i++)
	{
		if (watch.stop && watch.stop())
		{
			RemoveTree(base);
			result.message = "The backup was stopped: a game was started. Nothing of it was kept.";
			return result;
		}
		if (watch.step)
			watch.step(i, total);

		const Source& source = sources[i];
		if (!ReadFile(source.local, &data))
		{
			// A file that went while the list was walked (a state deleted meanwhile) is no failure.
			if (errno == ENOENT)
				continue;
			RemoveTree(base);
			result.message = "A file on the console could not be read: " + source.file.relative + ". Nothing was kept of this backup.";
			return result;
		}
		const std::string target = Join(Join(base, source.into), source.file.relative);
		// (A folder card's files lie in folders of their own.)
		const std::string folder = Parent(target);
		if (folder != made && source.file.relative.find('/') != std::string::npos)
		{
			if (!MakeFolders(folder))
				return fail("A folder of the backup could not be made.");
			made = folder;
		}
		if (!WriteFile(target, data))
			return fail("The backup stopped at " + source.file.relative + ".");
		result.files++;
		result.bytes += data.size();
	}
	if (watch.step)
		watch.step(total, total);

	char stamp[48] = "";
	struct tm local;
	if (localtime_r(&now, &local) != nullptr)
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
	std::string manifest = "Crux2 backup\n";
	manifest += std::string("made: ") + stamp + "\n";
	manifest += "by: " + version + "\n";
	manifest += std::string("kind: ") + (automatic ? "automatic" : "manual") + "\n";
	manifest += "memory card files: " + std::to_string(card_files) + "\n";
	manifest += "saved states: " + std::to_string(state_files) + "\n";
	manifest += "files written: " + std::to_string(result.files) + "\n";
	manifest += "bytes: " + std::to_string(result.bytes) + "\n";
	if (!WriteFile(Join(base, "backup.txt"), manifest))
		return fail("The backup could not be finished.");

	Prune(place, automatic, result.snapshot);

	result.ok = true;
	result.message = "Backed up " + Count(card_files, "memory card file", "memory card files");
	if (state_files > 0)
		result.message += " and " + Count(state_files, "saved state", "saved states");
	result.message += " (" + Size(result.bytes) + ").";
	return result;
}

// -------------------------------------------------------------------------------------- list

// Whether the text of a backup.txt is a finished backup's: this title's, under either of the names
// it has had.
static bool IsManifest(const std::string& manifest)
{
	return manifest.compare(0, 13, "Crux2 backup\n") == 0 || manifest.compare(0, 15, "PSPCSX2 backup\n") == 0;
}

bool List(const std::string& place, std::vector<Snapshot>* snapshots, std::string* error)
{
	snapshots->clear();
	bool any = false;
	int why = 0;
	for (const char* folder : {FOLDER, FOLDER_BEFORE})
	{
		const std::string root = Join(place, folder);
		std::vector<Item> items;
		if (!ListFolder(root, &items))
		{
			if (folder == FOLDER)
				why = errno;
			continue;
		}
		any = true;

		for (const Item& item : items)
		{
			if (!item.folder)
				continue;
			Snapshot snapshot;
			snapshot.when = WhenOf(item.name, &snapshot.automatic);
			if (snapshot.when == 0)
				continue;
			snapshot.name = item.name;
			snapshot.path = Join(root, item.name);
			std::string manifest;
			if (ReadFile(Join(snapshot.path, "backup.txt"), &manifest) && IsManifest(manifest))
			{
				uint64_t cards = 0, states = 0, bytes = 0;
				snapshot.complete = ReadNumber(manifest, "memory card files", &cards) && ReadNumber(manifest, "bytes", &bytes);
				ReadNumber(manifest, "saved states", &states);
				snapshot.card_files = static_cast<int>(cards);
				snapshot.states = static_cast<int>(states);
				snapshot.bytes = bytes;
			}
			snapshots->push_back(std::move(snapshot));
		}
	}
	if (!any)
	{
		// No backup was made there yet: the place itself has to answer, though.
		std::vector<Item> top;
		if (ListFolder(place, &top))
			return true;
		if (error)
			*error = Why(why == ENOENT ? EHOSTUNREACH : why, place);
		return false;
	}
	std::sort(snapshots->begin(), snapshots->end(), [](const Snapshot& a, const Snapshot& b) { return a.name > b.name; });
	return true;
}

// ----------------------------------------------------------------------------------- restore

Result Restore(const Folders& folders, const Snapshot& snapshot, const Watch& watch)
{
	Result result;
	result.snapshot = snapshot.name;

	std::string manifest;
	if (!ReadFile(Join(snapshot.path, "backup.txt"), &manifest) || !IsManifest(manifest))
	{
		result.message = "This backup was never finished (or cannot be read now): nothing was changed on the console.";
		return result;
	}

	struct Target
	{
		std::string remote, local, kept;
	};
	std::vector<Target> targets;
	int card_files = 0, state_files = 0;
	{
		std::vector<Found> found;
		Collect(Join(snapshot.path, "memcards"), std::string(), true, &found);
		for (const Found& file : found)
			targets.push_back({Join(Join(snapshot.path, "memcards"), file.relative), Join(folders.memcards, file.relative),
				Join(Join(folders.replaced, "memcards"), file.relative)});
		card_files = static_cast<int>(found.size());
		found.clear();
		Collect(Join(snapshot.path, "sstates"), std::string(), false, &found);
		for (const Found& file : found)
			targets.push_back({Join(Join(snapshot.path, "sstates"), file.relative), Join(folders.sstates, file.relative),
				Join(Join(folders.replaced, "sstates"), file.relative)});
		state_files = static_cast<int>(found.size());
	}
	if (targets.empty())
	{
		result.message = "This backup has no files in it: nothing was changed on the console.";
		return result;
	}

	// What the restore before this one replaced makes room for what this one replaces.
	RemoveTree(folders.replaced);

	const int total = static_cast<int>(targets.size());
	std::string data;
	for (int i = 0; i < total; i++)
	{
		if (watch.stop && watch.stop())
		{
			result.message = "The restore was stopped after " + Count(result.files, "file", "files") + ": a game was started.";
			return result;
		}
		if (watch.step)
			watch.step(i, total);

		const Target& target = targets[i];
		if (!ReadFile(target.remote, &data))
		{
			const int error = errno;
			result.message = "The restore stopped after " + Count(result.files, "file", "files") + ". " + Why(error, snapshot.path) +
			                 " What was replaced until then is in the folder backup-before-restore.";
			return result;
		}
		if (!MakeFolders(Parent(target.local)))
		{
			result.message = "A folder on the console could not be made; the restore stopped after " + Count(result.files, "file", "files") + ".";
			return result;
		}
		// The file it replaces is kept, not lost: moved aside first.
		struct stat st;
		if (stat(target.local.c_str(), &st) == 0)
		{
			if (!MakeFolders(Parent(target.kept)) || rename(target.local.c_str(), target.kept.c_str()) != 0)
			{
				result.message = "A file on the console could not be put aside before being replaced; the restore stopped after " +
				                 Count(result.files, "file", "files") + ".";
				return result;
			}
		}
		if (!WriteFile(target.local, data))
		{
			// Back where it was.
			rename(target.kept.c_str(), target.local.c_str());
			result.message = "A file could not be written on the console; the restore stopped after " + Count(result.files, "file", "files") + ".";
			return result;
		}
		result.files++;
		result.bytes += data.size();
	}
	if (watch.step)
		watch.step(total, total);

	result.ok = true;
	result.message = "Restored " + Count(card_files, "memory card file", "memory card files");
	if (state_files > 0)
		result.message += " and " + Count(state_files, "saved state", "saved states");
	result.message += ". What they replaced is in the folder backup-before-restore.";
	return result;
}

// --------------------------------------------------------------------------- one job at a time

namespace
{
std::mutex s_mutex;
State s_state;
unsigned s_lists_read = 0;
std::function<bool()> s_stop;

struct Job
{
	enum Kind { DoBackup, DoRestore, DoList } kind = DoList;
	Folders folders;
	std::string place;
	bool states = false, automatic = false;
	std::string version;
	Snapshot snapshot;
	std::function<void(const Result&)> done;
};

void Relist(const std::string& place)
{
	std::vector<Snapshot> snapshots;
	std::string error;
	const bool ok = List(place, &snapshots, &error);
	const std::lock_guard lock(s_mutex);
	s_state.listed = true;
	s_state.listed_place = place;
	s_state.list_error = ok ? std::string() : (error.empty() ? std::string("The place could not be read.") : error);
	s_state.snapshots = std::move(snapshots);
	s_state.generation++;
	s_lists_read++;
}

void* JobMain(void* argument)
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	Job* const job = static_cast<Job*>(argument);
	Watch watch;
	const char* const verb = (job->kind == Job::DoBackup) ? "Backing up" : "Restoring";
	watch.step = [verb](int done, int total) {
		const std::lock_guard lock(s_mutex);
		s_state.doing = std::string(verb) + ": " + std::to_string(std::min(done + 1, total)) + " of " + std::to_string(total) + " files";
		s_state.generation++;
	};
	watch.stop = [] {
		std::function<bool()> stop;
		{
			const std::lock_guard lock(s_mutex);
			stop = s_stop;
		}
		return stop && stop();
	};

	Result result;
	switch (job->kind)
	{
		case Job::DoBackup:
			result = Backup(job->folders, job->place, job->states, job->automatic, job->version, watch);
			Relist(job->place);
			break;
		case Job::DoRestore:
			result = Restore(job->folders, job->snapshot, watch);
			break;
		case Job::DoList:
			Relist(job->place);
			break;
	}

	{
		const std::lock_guard lock(s_mutex);
		s_state.busy = false;
		s_state.doing.clear();
		s_state.generation++;
	}
	if (job->done)
		job->done(result);
	delete job;
	return nullptr;
}

bool Start(Job* job, const char* doing)
{
	{
		const std::lock_guard lock(s_mutex);
		if (s_state.busy)
		{
			delete job;
			return false;
		}
		s_state.busy = true;
		s_state.doing = doing;
		s_state.generation++;
	}
	// (No stack size is asked for: the console gives a default thread 2 MiB of direct memory, and
	// one that asks for a size gets it from the small flexible pool.)
	pthread_t thread;
	if (pthread_create(&thread, nullptr, &JobMain, job) != 0)
	{
		delete job;
		const std::lock_guard lock(s_mutex);
		s_state.busy = false;
		s_state.doing.clear();
		s_state.generation++;
		return false;
	}
	pthread_detach(thread);
	return true;
}
} // namespace

State CurrentState()
{
	const std::lock_guard lock(s_mutex);
	return s_state;
}

std::string Doing()
{
	const std::lock_guard lock(s_mutex);
	return s_state.doing;
}

unsigned ListsRead()
{
	const std::lock_guard lock(s_mutex);
	return s_lists_read;
}

bool StartBackup(const Folders& folders, const std::string& place, bool states, bool automatic, const std::string& version,
	std::function<void(const Result&)> done)
{
	Job* const job = new Job();
	job->kind = Job::DoBackup;
	job->folders = folders;
	job->place = place;
	job->states = states;
	job->automatic = automatic;
	job->version = version;
	job->done = std::move(done);
	return Start(job, "Backing up");
}

bool StartRestore(const Folders& folders, const Snapshot& snapshot, std::function<void(const Result&)> done)
{
	Job* const job = new Job();
	job->kind = Job::DoRestore;
	job->folders = folders;
	job->snapshot = snapshot;
	job->done = std::move(done);
	return Start(job, "Restoring");
}

bool StartList(const std::string& place)
{
	Job* const job = new Job();
	job->kind = Job::DoList;
	job->place = place;
	return Start(job, "Looking for backups");
}

void SetStopCheck(std::function<bool()> stop)
{
	const std::lock_guard lock(s_mutex);
	s_stop = std::move(stop);
}

void WaitUntilIdle()
{
	for (;;)
	{
		{
			const std::lock_guard lock(s_mutex);
			if (!s_state.busy)
				return;
		}
		usleep(2000);
	}
}

} // namespace PS5Backup
