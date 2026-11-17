// Crux2 - HD texture packs found, measured, moved and deleted (ps5_texture_pack_store.h says how).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of PCSX2 or of the console here (ps5/tests/texturepacks/store_test.cpp runs it on a PC).

#include "ps5_texture_pack_store.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace PS5TexturePackStore
{
namespace
{
std::string Join(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;
	if (b.empty())
		return a;
	return (a.back() == '/') ? a + b : a + "/" + b;
}

std::string Parent(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return (slash == std::string::npos || slash == 0) ? std::string() : path.substr(0, slash);
}

bool IEquals(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

bool EndsWith(const std::string& s, const std::string& end)
{
	return s.size() >= end.size() && IEquals(s.substr(s.size() - end.size()), end);
}

bool Never()
{
	return false;
}

// Takes the serial's folder away when nothing is left in it.
void DropEmptyFolder(Files& files, const std::string& folder)
{
	std::vector<Entry> left;
	if (!folder.empty() && files.List(folder, &left) && left.empty())
		files.Remove(folder, true);
}
} // namespace

// ------------------------------------------------------------------------------------- POSIX

bool PosixFiles::List(const std::string& folder, std::vector<Entry>* out)
{
	out->clear();
	DIR* const dir = opendir(folder.c_str());
	if (dir == nullptr)
		return false;
	while (const dirent* d = readdir(dir))
	{
		if (std::strcmp(d->d_name, ".") == 0 || std::strcmp(d->d_name, "..") == 0)
			continue;
		Entry e;
		e.name = d->d_name;
		struct stat st;
		if (stat(Join(folder, e.name).c_str(), &st) != 0)
			continue;
		e.folder = S_ISDIR(st.st_mode);
		e.size = e.folder ? 0 : static_cast<uint64_t>(st.st_size);
		out->push_back(std::move(e));
	}
	closedir(dir);
	return true;
}

bool PosixFiles::Exists(const std::string& path, bool* folder, uint64_t* size)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return false;
	if (folder)
		*folder = S_ISDIR(st.st_mode);
	if (size)
		*size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
	return true;
}

bool PosixFiles::MakeFolders(const std::string& path)
{
	std::string so_far;
	size_t at = 0;
	while (at <= path.size())
	{
		const size_t slash = path.find('/', at);
		const size_t end = (slash == std::string::npos) ? path.size() : slash;
		so_far = path.substr(0, end);
		if (!so_far.empty() && mkdir(so_far.c_str(), 0777) != 0 && errno != EEXIST)
			return false;
		if (slash == std::string::npos)
			break;
		at = slash + 1;
	}
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool PosixFiles::CopyFile(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
	const std::function<void(uint64_t)>& moved, std::string* error)
{
	const int in = open(from.c_str(), O_RDONLY);
	if (in < 0)
	{
		*error = "cannot read " + from + ": " + std::strerror(errno);
		return false;
	}
	const std::string part = to + PART;
	const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0)
	{
		*error = "cannot write " + to + ": " + std::strerror(errno);
		close(in);
		return false;
	}
	static thread_local std::vector<char> buffer(1u << 20);
	uint64_t done = 0;
	bool ok = true;
	for (;;)
	{
		if (stop && stop())
		{
			*error = "stopped";
			ok = false;
			break;
		}
		const ssize_t got = read(in, buffer.data(), buffer.size());
		if (got < 0)
		{
			if (errno == EINTR)
				continue;
			*error = "cannot read " + from + ": " + std::strerror(errno);
			ok = false;
			break;
		}
		if (got == 0)
			break;
		ssize_t written = 0;
		while (written < got)
		{
			const ssize_t w = write(out, buffer.data() + written, static_cast<size_t>(got - written));
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
			{
				*error = "cannot write " + to + ": " + (w < 0 ? std::strerror(errno) : "the drive is full");
				ok = false;
				break;
			}
			written += w;
		}
		if (!ok)
			break;
		done += static_cast<uint64_t>(got);
		if (moved)
			moved(static_cast<uint64_t>(got));
	}
	close(in);
	if (close(out) != 0 && ok)
	{
		*error = "cannot write " + to + ": " + std::strerror(errno);
		ok = false;
	}
	if (ok && done != size)
	{
		*error = from + " changed while it was copied";
		ok = false;
	}
	if (ok && rename(part.c_str(), to.c_str()) != 0)
	{
		*error = "cannot write " + to + ": " + std::strerror(errno);
		ok = false;
	}
	if (!ok)
		unlink(part.c_str());
	return ok;
}

bool PosixFiles::Remove(const std::string& path, bool folder)
{
	const int rc = folder ? rmdir(path.c_str()) : unlink(path.c_str());
	return rc == 0 || errno == ENOENT;
}

bool PosixFiles::Rename(const std::string& from, const std::string& to)
{
	return rename(from.c_str(), to.c_str()) == 0;
}

int64_t PosixFiles::FreeBytes(const std::string& folder)
{
	struct statvfs vfs;
	if (statvfs(folder.c_str(), &vfs) != 0)
		return -1;
	const unsigned long long bytes = static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
	return bytes > static_cast<unsigned long long>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(bytes);
}

// ------------------------------------------------------------------------------- finding them

Place PlaceAt(const std::string& name, const std::string& top)
{
	Place place;
	place.name = name;
	std::string base = top;
	while (base.size() > 1 && base.back() == '/')
		base.pop_back();
	place.top = base;
	// (The order ps5_extras_logic.h's TexturePackCandidates looks in.)
	for (const char* folder : {"Crux2/", "PSPCSX2/", "PS2/", ""})
		place.textures.push_back(base + "/" + folder + "textures");
	place.target = place.textures.front();
	return place;
}

std::optional<std::string> SerialOf(const std::string& name)
{
	if (name.size() != 10 || name[4] != '-')
		return std::nullopt;
	std::string serial;
	for (size_t i = 0; i < 10; i++)
	{
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if (i < 4 ? !std::isalpha(c) : (i > 4 && !std::isdigit(c)))
			return std::nullopt;
		serial.push_back(static_cast<char>(std::toupper(c)));
	}
	return serial;
}

std::vector<Pack> Find(Files& files, const std::vector<Place>& places)
{
	std::vector<Pack> out;
	std::vector<std::string> seen; // folders looked into (two places may share one)
	for (size_t p = 0; p < places.size(); p++)
	{
		for (const std::string& textures : places[p].textures)
		{
			if (std::find(seen.begin(), seen.end(), textures) != seen.end())
				continue;
			seen.push_back(textures);
			std::vector<Entry> entries;
			if (!files.List(textures, &entries))
				continue;
			std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
			for (const Entry& e : entries)
			{
				if (!e.folder)
				{
					// <serial>.zip
					if (!EndsWith(e.name, ".zip"))
						continue;
					const std::optional<std::string> serial = SerialOf(e.name.substr(0, e.name.size() - 4));
					if (!serial)
						continue;
					Pack pack;
					pack.serial = *serial;
					pack.path = Join(textures, e.name);
					pack.textures = textures;
					pack.place = p;
					pack.zip = true;
					pack.measured = true;
					pack.files = 1;
					pack.bytes = e.size;
					out.push_back(std::move(pack));
					continue;
				}
				const std::optional<std::string> serial = SerialOf(e.name);
				if (!serial)
					continue;
				std::vector<Entry> inside;
				if (!files.List(Join(textures, e.name), &inside))
					continue;
				std::sort(inside.begin(), inside.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
				// A copy made in its place on a share is marked while it is not whole (Move).
				const bool copying = std::any_of(inside.begin(), inside.end(), [](const Entry& sub) {
					return sub.folder && IEquals(sub.name, std::string("replacements") + COPYING);
				});
				for (const Entry& sub : inside)
				{
					if (!sub.folder)
						continue;
					Pack::State state;
					if (IEquals(sub.name, "replacements"))
						state = copying ? Pack::Moving : Pack::Whole;
					else if (IEquals(sub.name, std::string("replacements") + MOVING))
						state = Pack::Moving;
					else if (IEquals(sub.name, std::string("replacements") + DELETING))
						state = Pack::Deleting;
					else
						continue;
					Pack pack;
					pack.serial = *serial;
					pack.path = Join(Join(textures, e.name), sub.name);
					pack.textures = textures;
					pack.place = p;
					pack.state = state;
					out.push_back(std::move(pack));
				}
			}
		}
	}
	// The one the game reads: the first folder, and the first ZIP, of each serial.
	std::vector<std::string> folderRead, zipRead;
	for (Pack& pack : out)
	{
		if (pack.state != Pack::Whole)
			continue;
		std::vector<std::string>& had = pack.zip ? zipRead : folderRead;
		if (std::find(had.begin(), had.end(), pack.serial) == had.end())
		{
			had.push_back(pack.serial);
			pack.read = true;
		}
	}
	return out;
}

Measured Measure(Files& files, const std::string& path, const std::function<bool()>& stop)
{
	Measured m;
	bool folder = false;
	uint64_t size = 0;
	if (!files.Exists(path, &folder, &size))
		return m;
	if (!folder)
	{
		m.ok = true;
		m.files = 1;
		m.bytes = size;
		return m;
	}
	std::vector<std::string> todo{path};
	std::vector<Entry> entries;
	while (!todo.empty())
	{
		if (stop && stop())
			return m;
		const std::string at = std::move(todo.back());
		todo.pop_back();
		if (!files.List(at, &entries))
			return m;
		for (const Entry& e : entries)
		{
			if (e.folder)
			{
				todo.push_back(Join(at, e.name));
			}
			else
			{
				m.files++;
				m.bytes += e.size;
			}
		}
	}
	m.ok = true;
	return m;
}

// ------------------------------------------------------------------------------- changing them

std::string PathUnder(const Pack& pack, const std::string& textures)
{
	if (pack.zip)
		return Join(textures, pack.serial + ".zip");
	return Join(Join(textures, pack.serial), "replacements");
}

Outcome CopyTree(Files& files, const std::string& from, const std::string& to, const std::function<bool()>& stop, Progress* progress)
{
	Outcome o;
	if (!files.MakeFolders(to))
	{
		o.error = "cannot make the folder " + to;
		return o;
	}
	std::vector<std::string> todo{std::string()}; // relative to `from`
	std::vector<Entry> entries;
	while (!todo.empty())
	{
		const std::string rel = std::move(todo.back());
		todo.pop_back();
		const std::string src = Join(from, rel), dst = Join(to, rel);
		if (!files.List(src, &entries))
		{
			o.error = "cannot read the folder " + src;
			return o;
		}
		for (const Entry& e : entries)
		{
			if (stop && stop())
			{
				o.stopped = true;
				o.error = "stopped";
				return o;
			}
			const std::string relName = Join(rel, e.name);
			if (e.folder)
			{
				if (!files.MakeFolders(Join(to, relName)))
				{
					o.error = "cannot make the folder " + Join(to, relName);
					return o;
				}
				todo.push_back(relName);
				continue;
			}
			// A partial file of an earlier copy is not a file of the pack.
			if (EndsWith(e.name, PART))
				continue;
			const std::string target = Join(dst, e.name);
			bool isFolder = false;
			uint64_t there = 0;
			const bool had = files.Exists(target, &isFolder, &there) && !isFolder && there == e.size;
			if (!had)
			{
				std::string why;
				if (!files.CopyFile(Join(src, e.name), target, e.size, stop,
						[progress](uint64_t n) {
							if (progress)
								progress->bytes += n;
						},
						&why))
				{
					o.stopped = (stop && stop());
					o.error = o.stopped ? std::string("stopped") : why;
					return o;
				}
				if (!files.Exists(target, &isFolder, &there) || isFolder || there != e.size)
				{
					o.error = "the copy of " + relName + " is not the size of the file";
					return o;
				}
			}
			else if (progress)
			{
				progress->bytes += e.size;
			}
			o.files++;
			o.bytes += e.size;
			if (progress)
				progress->files++;
		}
	}
	o.ok = true;
	o.path = to;
	return o;
}

bool SameTree(Files& files, const std::string& from, const std::string& to, const std::function<bool()>& stop, std::string* why)
{
	std::vector<std::string> todo{std::string()};
	std::vector<Entry> entries;
	while (!todo.empty())
	{
		const std::string rel = std::move(todo.back());
		todo.pop_back();
		if (!files.List(Join(from, rel), &entries))
		{
			*why = "cannot read the folder " + Join(from, rel);
			return false;
		}
		for (const Entry& e : entries)
		{
			if (stop && stop())
			{
				*why = "stopped";
				return false;
			}
			const std::string relName = Join(rel, e.name);
			if (e.folder)
			{
				todo.push_back(relName);
				continue;
			}
			if (EndsWith(e.name, PART))
				continue;
			bool folder = false;
			uint64_t size = 0;
			if (!files.Exists(Join(to, relName), &folder, &size) || folder || size != e.size)
			{
				*why = relName + " is not in the copy as it is in the pack";
				return false;
			}
		}
	}
	return true;
}

Outcome RemoveTree(Files& files, const std::string& path, const std::function<bool()>& stop, Progress* progress)
{
	Outcome o;
	bool folder = false;
	uint64_t size = 0;
	if (!files.Exists(path, &folder, &size))
	{
		o.ok = true;
		return o;
	}
	if (!folder)
	{
		o.ok = files.Remove(path, false);
		if (!o.ok)
			o.error = "cannot delete " + path;
		else
		{
			o.files = 1;
			o.bytes = size;
			if (progress)
			{
				progress->files++;
				progress->bytes += size;
			}
		}
		return o;
	}
	// Depth first: a folder is taken away once what is in it is.
	struct Level
	{
		std::string path;
		bool listed = false;
	};
	std::vector<Level> todo{{path, false}};
	std::vector<Entry> entries;
	while (!todo.empty())
	{
		if (stop && stop())
		{
			o.stopped = true;
			o.error = "stopped";
			return o;
		}
		Level& top = todo.back();
		if (top.listed)
		{
			if (!files.Remove(top.path, true))
			{
				o.error = "cannot delete the folder " + top.path;
				return o;
			}
			todo.pop_back();
			continue;
		}
		top.listed = true;
		const std::string at = top.path; // (todo may grow below)
		if (!files.List(at, &entries))
		{
			o.error = "cannot read the folder " + at;
			return o;
		}
		for (const Entry& e : entries)
		{
			if (e.folder)
			{
				todo.push_back({Join(at, e.name), false});
				continue;
			}
			if (stop && stop())
			{
				o.stopped = true;
				o.error = "stopped";
				return o;
			}
			if (!files.Remove(Join(at, e.name), false))
			{
				o.error = "cannot delete " + Join(at, e.name);
				return o;
			}
			o.files++;
			o.bytes += e.size;
			if (progress)
			{
				progress->files++;
				progress->bytes += e.size;
			}
		}
	}
	o.ok = true;
	return o;
}

Outcome Move(Files& files, const Pack& pack, const std::string& textures, const std::function<bool()>& stop, Progress* progress)
{
	Outcome o;
	const std::string dest = PathUnder(pack, textures);
	if (pack.state != Pack::Whole)
	{
		o.error = "this is what an unfinished move or delete left: delete it";
		return o;
	}
	if (dest == pack.path || Join(textures, "") == Join(pack.textures, ""))
	{
		o.error = "the pack is there already";
		return o;
	}
	bool folder = false;
	uint64_t size = 0;

	if (pack.zip)
	{
		if (files.Exists(dest, &folder, &size))
		{
			o.error = "a pack of " + pack.serial + " is there already: delete one of them first";
			return o;
		}
		if (!files.Exists(pack.path, &folder, &size) || folder)
		{
			o.error = "the pack is not there any more";
			return o;
		}
		if (progress)
		{
			progress->totalFiles = 1;
			progress->totalBytes = size;
			progress->phase = Copying;
		}
		if (!files.MakeFolders(textures))
		{
			o.error = "cannot make the folder " + textures;
			return o;
		}
		std::string why;
		if (!files.CopyFile(pack.path, dest, size, stop, [progress](uint64_t n) { if (progress) progress->bytes += n; }, &why))
		{
			o.stopped = (stop && stop());
			o.error = o.stopped ? std::string("stopped") : why;
			return o;
		}
		uint64_t there = 0;
		if (progress)
			progress->phase = Checking;
		if (!files.Exists(dest, &folder, &there) || folder || there != size)
		{
			o.error = "the copy is not the size of the pack: the pack was left where it was";
			files.Remove(dest, false);
			return o;
		}
		if (progress)
		{
			progress->files = 1;
			progress->phase = Removing;
		}
		if (!files.Remove(pack.path, false))
		{
			o.error = "the pack was copied, but the old one could not be deleted";
			o.path = dest;
			return o;
		}
		o.ok = true;
		o.path = dest;
		o.files = 1;
		o.bytes = size;
		return o;
	}

	// ---- a folder
	if (progress)
		progress->phase = Measuring;
	const Measured source = Measure(files, pack.path, stop);
	if (!source.ok)
	{
		o.stopped = (stop && stop());
		o.error = o.stopped ? std::string("stopped") : std::string("the pack cannot be read");
		return o;
	}
	if (progress)
	{
		progress->totalFiles = source.files;
		progress->totalBytes = source.bytes;
		progress->phase = Copying;
	}
	const bool rename = files.CanRename(textures);
	const std::string staging = rename ? dest + MOVING : dest;
	// Where the copy is made in its place (a share), it is marked until it is whole: a copy that was
	// stopped is found as unfinished (Find), and a move begun again goes on with it.
	const std::string marker = dest + COPYING;
	bool resuming = false;
	if (!rename)
	{
		bool markerFolder = false;
		resuming = files.Exists(marker, &markerFolder, &size) && markerFolder;
	}
	if (!resuming && files.Exists(dest, &folder, &size))
	{
		std::vector<Entry> inside;
		if (folder && files.List(dest, &inside) && inside.empty())
		{
			files.Remove(dest, true);
		}
		else
		{
			o.error = "a pack of " + pack.serial + " is there already: delete one of them first";
			return o;
		}
	}
	if (!files.MakeFolders(Parent(staging)) || (!rename && !files.MakeFolders(marker)))
	{
		o.error = "cannot make the folder " + Parent(staging);
		return o;
	}
	const Outcome copied = CopyTree(files, pack.path, staging, stop, progress);
	if (!copied.ok)
	{
		o.stopped = copied.stopped;
		o.error = copied.stopped ? std::string("stopped") : copied.error;
		return o;
	}
	if (progress)
		progress->phase = Checking;
	std::string why;
	const Measured made = Measure(files, staging, stop);
	if (!SameTree(files, pack.path, staging, stop, &why) || !made.ok || made.files < source.files || made.bytes < source.bytes)
	{
		o.stopped = (stop && stop());
		o.error = o.stopped ? std::string("stopped") : (why.empty() ? std::string("the copy is not the whole pack") : why);
		return o;
	}
	if (rename && !files.Rename(staging, dest))
	{
		o.error = "the copy could not be put in its place";
		return o;
	}
	if (!rename && !files.Remove(marker, true))
	{
		o.error = "the copy could not be marked whole: the pack was left where it was";
		return o;
	}
	o.path = dest;

	// The copy is whole and checked: the old one goes. Renamed away first where it can be, so the
	// game no longer reads it; then that may be stopped (it is found as a delete to finish).
	if (progress)
	{
		progress->phase = Removing;
		progress->files = 0;
		progress->bytes = 0;
	}
	std::string gone = pack.path;
	bool renamedAway = false;
	if (files.CanRename(pack.path) && files.Rename(pack.path, pack.path + DELETING))
	{
		gone = pack.path + DELETING;
		renamedAway = true;
	}
	const Outcome removed = RemoveTree(files, gone, renamedAway ? stop : std::function<bool()>(Never), progress);
	DropEmptyFolder(files, Parent(pack.path));
	o.files = source.files;
	o.bytes = source.bytes;
	if (!removed.ok)
	{
		// (The pack is moved: what is left of the old one is said, not hidden.)
		o.ok = true;
		o.error = removed.stopped ? std::string("the old copy was not all deleted (stopped)") : removed.error;
		return o;
	}
	o.ok = true;
	return o;
}

Outcome Delete(Files& files, const Pack& pack, const std::function<bool()>& stop, Progress* progress)
{
	Outcome o;
	if (progress)
		progress->phase = Removing;
	if (pack.zip)
	{
		o = RemoveTree(files, pack.path, stop, progress);
		return o;
	}
	std::string target = pack.path;
	if (pack.state == Pack::Whole && files.CanRename(pack.path))
	{
		if (files.Rename(pack.path, pack.path + DELETING))
			target = pack.path + DELETING;
	}
	o = RemoveTree(files, target, stop, progress);
	// (An unfinished copy's mark goes with the last of it.)
	if (o.ok)
		files.Remove(pack.path + COPYING, true);
	DropEmptyFolder(files, Parent(pack.path));
	return o;
}

// ------------------------------------------------------------------------------------- room

Room Fits(int64_t free, uint64_t need)
{
	if (free < 0)
		return Room::NotKnown;
	const uint64_t margin = need / 100 + (64ull << 20);
	return (need + margin <= static_cast<uint64_t>(free)) ? Room::Fits : Room::TooLittle;
}
} // namespace PS5TexturePackStore
