// Crux2 - HD texture packs on the console's storage, the USB drives and the shares: found,
// measured, moved and deleted (the texture pack manager's file work).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of PCSX2 or of the console here: every file call goes through a Files, which is plain
// POSIX on a PC (ps5/tests/texturepacks/store_test.cpp runs all of it on temporary folders) and on
// the console the same calls for its own storage and the drives, the virtual file layer's for a
// share (ps5_texture_pack_manager.cpp).
//
// A pack is what PCSX2 reads (GSTextureReplacements.cpp, with ps5_extras.cpp's places):
//   <textures>/<serial>/replacements/...   a folder of pictures, or
//   <textures>/<serial>.zip                 one file in place of thousands,
// where <textures> is the console's textures folder, or on a drive or a share Crux2/textures,
// PSPCSX2/textures, PS2/textures or textures at its top. The console's own is read first, then
// the drives and the shares in their order; a folder and a ZIP are both read.
//
// A move is a copy and then a delete, and the source is never touched before the copy is whole
// and checked file by file (each file there, of the same size): a move stopped or failed half-way
// leaves the source as it was. Where folders can be renamed (the console, a drive) the copy is
// made beside its place ("replacements.crux2-moving") and renamed when checked, so a half copy is
// never read by the game; a share cannot rename a folder, so there the copy is made in its place
// (the source, on the console or a drive, is read before it until the end), with an empty folder
// "replacements.crux2-copying" beside it until it is whole: listed as unfinished till then, and a
// pack already there is never merged into. A move begun again
// goes on from what was copied (a file already there with the source's size is not copied again).
// A delete renames the pack away first where it can ("replacements.crux2-deleting"), so the game
// no longer reads it from the first moment; what a stopped delete leaves is found as such and
// deleted by the next one.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace PS5TexturePackStore
{
inline constexpr const char* MOVING = ".crux2-moving";
inline constexpr const char* DELETING = ".crux2-deleting";
inline constexpr const char* PART = ".crux2-part";
inline constexpr const char* COPYING = ".crux2-copying"; // "replacements.crux2-copying": the copy beside it is not whole

struct Entry
{
	std::string name;
	bool folder = false;
	uint64_t size = 0; // of a file
};

/// The file calls the store makes.
class Files
{
public:
	virtual ~Files() = default;
	/// What a folder holds (no "." or ".."). False when it is not a folder that can be read.
	virtual bool List(const std::string& folder, std::vector<Entry>* out) = 0;
	/// Whether something is there, and what.
	virtual bool Exists(const std::string& path, bool* folder, uint64_t* size) = 0;
	/// The folder and those above it.
	virtual bool MakeFolders(const std::string& path) = 0;
	/// A whole file copied to `to`: it is there under its name only when whole. `stop` is asked
	/// between blocks; `moved` is told of the bytes as they go. False with a reason otherwise
	/// (stopped: "stopped").
	virtual bool CopyFile(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
		const std::function<void(uint64_t)>& moved, std::string* error) = 0;
	/// A file, or an empty folder, taken away. One that is not there is no error.
	virtual bool Remove(const std::string& path, bool folder) = 0;
	/// Whether a folder in that place can be renamed (not on a share).
	virtual bool CanRename(const std::string& path) = 0;
	virtual bool Rename(const std::string& from, const std::string& to) = 0;
	/// The bytes free where the folder is; -1 when that cannot be known (on the console: no call
	/// a title can make reports it).
	virtual int64_t FreeBytes(const std::string& folder) = 0;
};

/// The plain POSIX calls (a PC; the console's own storage and its drives).
class PosixFiles : public Files
{
public:
	bool List(const std::string& folder, std::vector<Entry>* out) override;
	bool Exists(const std::string& path, bool* folder, uint64_t* size) override;
	bool MakeFolders(const std::string& path) override;
	bool CopyFile(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
		const std::function<void(uint64_t)>& moved, std::string* error) override;
	bool Remove(const std::string& path, bool folder) override;
	bool CanRename(const std::string&) override { return true; }
	bool Rename(const std::string& from, const std::string& to) override;
	int64_t FreeBytes(const std::string& folder) override; // statvfs
};

// ------------------------------------------------------------------------------- finding them

/// A place packs can be on: the console, a USB drive, a share. `textures` are its textures
/// folders in the order they are read (the console has one; a drive or a share four).
struct Place
{
	std::string name;   // "This console", "USB drive 1", "Share: games"
	std::string top;    // where it is: the console's textures folder, "/mnt/usb0", "/smb/games"
	std::vector<std::string> textures;
	std::string target; // the textures folder a pack moved or downloaded here goes into
	bool writable = true;
};
/// The textures folders of a drive's or a share's top ("/mnt/usb0"): Crux2/textures,
/// PSPCSX2/textures, PS2/textures, textures. A pack put there goes into the first.
Place PlaceAt(const std::string& name, const std::string& top);

struct Pack
{
	enum State
	{
		Whole,    // a pack as the game reads it
		Moving,   // a move's copy that was not finished (beside its place: not read)
		Deleting, // what a delete that was stopped left (not read)
	};
	std::string serial;   // "SLUS-21376"
	std::string path;     // the folder (".../SLUS-21376/replacements") or the ZIP
	std::string textures; // the textures folder it is in
	size_t place = 0;     // an index into the places it was found in
	bool zip = false;
	State state = Whole;
	bool read = false;    // the one the game reads (the first folder, and the first ZIP, in the order of the places)
	// Measured (Measure): the files in it and their bytes.
	bool measured = false;
	uint64_t files = 0, bytes = 0;
};

/// Whether a name is a serial as packs are named ("SLUS-21376", any case), and that serial.
std::optional<std::string> SerialOf(const std::string& name);

/// The packs on the places, in the order the game reads them (unmeasured), with `read` set.
std::vector<Pack> Find(Files& files, const std::vector<Place>& places);

struct Measured
{
	bool ok = false; // false: stopped, or the folder could not be read
	uint64_t files = 0, bytes = 0;
};
/// Every file under a folder (or the one file), counted. `stop` between folders.
Measured Measure(Files& files, const std::string& path, const std::function<bool()>& stop);

// ------------------------------------------------------------------------------- changing them

/// How a move or a delete goes, for a bar: read from other threads while it runs.
struct Progress
{
	std::atomic<uint64_t> files{0}, bytes{0};             // done
	std::atomic<uint64_t> totalFiles{0}, totalBytes{0};   // of the whole (0 until measured)
	std::atomic<int> phase{0};                            // Phase
};
enum Phase { Measuring = 0, Copying = 1, Checking = 2, Removing = 3 };

struct Outcome
{
	bool ok = false;
	bool stopped = false;
	std::string error;   // why not, when not
	std::string path;    // a move: where the pack is now
	uint64_t files = 0, bytes = 0;
};

/// The pack's folder (or ZIP) under another textures folder: the same "<serial>/replacements" or
/// "<serial>.zip" below it.
std::string PathUnder(const Pack& pack, const std::string& textures);

/// Copies the tree `from` into `to` (made if missing): each file not already there with the same
/// size. `stop` between files and blocks.
Outcome CopyTree(Files& files, const std::string& from, const std::string& to, const std::function<bool()>& stop, Progress* progress);
/// Whether every file under `from` is under `to` with the same size (what more is under `to`
/// does not matter). The first that is not, in `why`.
bool SameTree(Files& files, const std::string& from, const std::string& to, const std::function<bool()>& stop, std::string* why);
/// Every file and folder under `path`, and `path` itself, taken away. `stop` between files.
Outcome RemoveTree(Files& files, const std::string& path, const std::function<bool()>& stop, Progress* progress);

/// Moves a pack into another textures folder (see the top of this file). The source is taken away
/// only when the copy is whole and checked.
Outcome Move(Files& files, const Pack& pack, const std::string& textures, const std::function<bool()>& stop, Progress* progress);
/// Deletes a pack (and its serial's folder when that is left empty).
Outcome Delete(Files& files, const Pack& pack, const std::function<bool()>& stop, Progress* progress);

// ------------------------------------------------------------------------------------- room

enum class Room
{
	Fits,
	TooLittle,
	NotKnown, // the place does not say what is free
};
/// Whether `need` bytes fit in `free` (-1: not known), with a margin of 1% and 64 MB.
Room Fits(int64_t free, uint64_t need);
/// What a pack takes once unpacked, from its archive's size (pictures do not compress: about the
/// same, a little more for the file system).
inline uint64_t UnpackedSize(uint64_t archive) { return archive + archive / 20; }

// ------------------------------------------------------------------------------- the downloads

/// Downloads waiting their turn: the downloader brings one at a time, so the next is started when
/// it is not busy (Next, asked once a frame).
template <typename T>
class Queue
{
public:
	void Add(T item) { m_items.push_back(std::move(item)); }
	size_t Size() const { return m_items.size(); }
	bool Empty() const { return m_items.empty(); }
	const std::deque<T>& Items() const { return m_items; }
	bool Remove(size_t index)
	{
		if (index >= m_items.size())
			return false;
		m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}
	void Clear() { m_items.clear(); }
	template <typename Pred>
	bool Has(Pred pred) const
	{
		for (const T& item : m_items)
		{
			if (pred(item))
				return true;
		}
		return false;
	}
	/// The one to start now: the first, taken off the queue, when the downloader is not busy.
	std::optional<T> Next(bool busy)
	{
		if (busy || m_items.empty())
			return std::nullopt;
		T item = std::move(m_items.front());
		m_items.pop_front();
		return item;
	}

private:
	std::deque<T> m_items;
};
} // namespace PS5TexturePackStore
