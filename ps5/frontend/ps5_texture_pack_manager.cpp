// Crux2 - the HD texture pack manager's runtime (ps5_texture_pack_manager.h says what it does).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// None of this has run on a console when it was written: the file work is
// ps5_texture_pack_store.cpp's, tested on a PC (ps5/tests/texturepacks/store_test.cpp), and what is
// here says what it does in the boot log ("texture packs:" lines).

#include "ps5_texture_pack_manager.h"

#include "ps5_jobs.h"
#include "ps5_priority.h"
#include "ps5_texture_packs_logic.h"
#include "ps5_texture_packs_pieces.h"
#include "vfs/ps5_vfs.h"

#include "Config.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5TexturePackManager
{
namespace
{
using namespace PS5TexturePackStore;
using PS5TexturePacksLogic::SizeText;

// ---- the console's file calls: its own storage and the drives as they are, a share (or a drive
// reached only through the console's FTP server) through the virtual file layer.
class ConsoleFiles final : public PosixFiles
{
public:
	static bool Virtual(const std::string& path) { return ps5vfs_is_virtual(path.c_str()) != 0; }

	bool List(const std::string& folder, std::vector<Entry>* out) override
	{
		if (!Virtual(folder))
			return PosixFiles::List(folder, out);
		out->clear();
		ps5vfs_dir* const dir = ps5vfs_opendir(folder.c_str());
		if (dir == nullptr)
			return false;
		ps5vfs_dirent e;
		while (ps5vfs_readdir(dir, &e) == 1)
		{
			if (e.name == nullptr || std::strcmp(e.name, ".") == 0 || std::strcmp(e.name, "..") == 0)
				continue;
			out->push_back({e.name, e.is_directory != 0, e.is_directory ? 0 : static_cast<uint64_t>(std::max<int64_t>(e.size, 0))});
		}
		ps5vfs_closedir(dir);
		return true;
	}
	bool Exists(const std::string& path, bool* folder, uint64_t* size) override
	{
		if (!Virtual(path))
			return PosixFiles::Exists(path, folder, size);
		struct stat st;
		if (ps5vfs_stat(path.c_str(), &st) != 0)
			return false;
		if (folder)
			*folder = S_ISDIR(st.st_mode);
		if (size)
			*size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
		return true;
	}
	bool MakeFolders(const std::string& path) override
	{
		if (!Virtual(path))
			return PosixFiles::MakeFolders(path);
		return ps5vfs_make_folders(path.c_str()) == 0;
	}
	bool CopyFile(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
		const std::function<void(uint64_t)>& moved, std::string* error) override
	{
		if (!Virtual(from) && !Virtual(to))
			return PosixFiles::CopyFile(from, to, size, stop, moved, error);
		// (ps5vfs_fopen reads a path that is not virtual from libc.)
		FILE* const in = ps5vfs_fopen(from.c_str(), "rb");
		if (in == nullptr)
		{
			*error = "cannot read " + from + ": " + std::strerror(errno);
			return false;
		}
		static thread_local std::vector<char> buffer(1u << 20);
		bool ok = true;
		if (Virtual(to))
		{
			// Written beside its place by the layer and moved there when whole.
			const int rc = ps5vfs_store_from(to.c_str(), size, [&](void* data, uint32_t length, uint64_t offset) -> int64_t {
				if (stop && stop())
				{
					errno = ECANCELED;
					return -1;
				}
				if (ps5vfs_fseeko(in, static_cast<off_t>(offset), SEEK_SET) != 0)
					return -1;
				const size_t got = ps5vfs_fread(data, 1, length, in);
				if (got != length)
				{
					errno = EIO;
					return -1;
				}
				if (moved)
					moved(got);
				return static_cast<int64_t>(got);
			});
			if (rc != 0)
			{
				ok = false;
				*error = (stop && stop()) ? std::string("stopped") : "cannot write " + to + ": " + std::strerror(errno);
			}
		}
		else
		{
			const std::string part = to + PART;
			const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (out < 0)
			{
				*error = "cannot write " + to + ": " + std::strerror(errno);
				ps5vfs_fclose(in);
				return false;
			}
			uint64_t done = 0;
			while (ok)
			{
				if (stop && stop())
				{
					*error = "stopped";
					ok = false;
					break;
				}
				const size_t got = ps5vfs_fread(buffer.data(), 1, buffer.size(), in);
				if (got == 0)
				{
					if (ps5vfs_ferror(in))
					{
						*error = "cannot read " + from;
						ok = false;
					}
					break;
				}
				size_t written = 0;
				while (written < got)
				{
					const ssize_t w = write(out, buffer.data() + written, got - written);
					if (w < 0 && errno == EINTR)
						continue;
					if (w <= 0)
					{
						*error = "cannot write " + to + ": " + (w < 0 ? std::strerror(errno) : "the drive is full");
						ok = false;
						break;
					}
					written += static_cast<size_t>(w);
				}
				done += got;
				if (moved)
					moved(got);
			}
			if (close(out) != 0 && ok)
			{
				*error = "cannot write " + to;
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
		}
		ps5vfs_fclose(in);
		return ok;
	}
	bool Remove(const std::string& path, bool folder) override
	{
		if (!Virtual(path))
			return PosixFiles::Remove(path, folder);
		return ps5vfs_remove(path.c_str(), folder ? 1 : 0) == 0;
	}
	bool CanRename(const std::string& path) override { return !Virtual(path); }
	bool Rename(const std::string& from, const std::string& to) override
	{
		if (Virtual(from) || Virtual(to))
			return false;
		return PosixFiles::Rename(from, to);
	}
	int64_t FreeBytes(const std::string& folder) override
	{
#if defined(__PROSPERO__)
		// No call a title can make reports a drive's free space (statvfs is the platform layer's
		// stand-in and always answers 16 GiB), so nothing is claimed.
		(void)folder;
		return -1;
#else
		return Virtual(folder) ? -1 : PosixFiles::FreeBytes(folder);
#endif
	}
};

ConsoleFiles s_files;

// ---- what the threads found, under the lock
std::mutex s_mutex;
std::atomic<unsigned> s_generation{1};
std::vector<PlaceInfo> s_places;
std::vector<Pack> s_packs;
std::map<std::string, std::pair<uint64_t, uint64_t>> s_sizes; // a pack's path: its files and bytes, measured
int s_collection_state = 0;
std::string s_collection_error;
std::vector<PS5TexturePacks::Pack> s_collection;
std::atomic_bool s_scanning{false}, s_rescan{false};

// ---- the move or the delete
std::atomic_bool s_job{false}, s_job_stop{false};
Progress s_progress;
std::string s_job_title; // (s_mutex)
bool s_job_delete = false;
PS5TexturePacksPieces::SpeedMeter s_meter;

// ---- what is handed to the graphics thread
std::deque<std::function<void()>> s_said; // (s_mutex)
bool s_relook = false;                    // (s_mutex) the places are to be looked into again

// ---- the downloads
struct Waiting
{
	PS5TexturePacks::Pack pack;
	PS5TexturePacks::Place place;
};
PS5TexturePackStore::Queue<Waiting> s_queue; // (graphics thread)
std::vector<std::string> s_downloading_serials;
bool s_was_busy = false;

void Bump()
{
	s_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool RunDetached(std::function<void()> work)
{
	auto* const job = new std::function<void()>(std::move(work));
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1u << 20);
	pthread_t thread;
	const int rc = pthread_create(
		&thread, &attr,
		[](void* argument) -> void* {
			auto* const todo = static_cast<std::function<void()>*>(argument);
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			(*todo)();
			delete todo;
			return nullptr;
		},
		job);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		delete job;
		ps5px_log("texture packs: a thread did not start: %d", rc);
		return false;
	}
	pthread_detach(thread);
	return true;
}

std::vector<PlaceInfo> MakePlaces()
{
	std::vector<PlaceInfo> places;
	{
		PlaceInfo own;
		own.place.name = "This console";
		own.place.top = EmuFolders::Textures;
		own.place.textures = {EmuFolders::Textures};
		own.place.target = EmuFolders::Textures;
		own.kind = 0;
		own.downloads = true;
		own.freeBytes = s_files.FreeBytes(EmuFolders::Textures);
		places.push_back(std::move(own));
	}
	// The drives and the shares, in the order the game looks in them (ps5_extras.cpp).
	for (const PS5Jobs::Place& p : PS5Jobs::Places())
	{
		PlaceInfo info;
		info.place = PlaceAt(p.name, p.path);
		info.kind = (p.path.compare(0, 5, "/smb/") == 0) ? 2 : 1;
		// (The downloader unpacks with the console's own file calls: not onto a share, nor a drive
		// reached only through the console's FTP server.)
		info.downloads = (info.kind == 1) && !ConsoleFiles::Virtual(p.path);
		info.freeBytes = s_files.FreeBytes(p.path);
		places.push_back(std::move(info));
	}
	return places;
}

// Lists the places and measures what is not measured, until nothing more is asked of it.
void ScanLoop()
{
	do
	{
		while (s_rescan.exchange(false))
		{
			std::vector<Place> places;
			{
				const std::lock_guard lock(s_mutex);
				for (const PlaceInfo& p : s_places)
					places.push_back(p.place);
			}
			std::vector<Pack> found = Find(s_files, places);
			size_t known = 0;
			{
				const std::lock_guard lock(s_mutex);
				for (Pack& p : found)
				{
					if (auto it = s_sizes.find(p.path); it != s_sizes.end() && !p.zip)
					{
						p.measured = true;
						p.files = it->second.first;
						p.bytes = it->second.second;
						known++;
					}
				}
				s_packs = found;
			}
			Bump();
			ps5px_log("texture packs: %zu pack(s) on %zu place(s), %zu measured before", found.size(), places.size(), known);
			for (const Pack& p : found)
			{
				if (p.measured)
					continue;
				if (s_rescan.load())
					break;
				const auto began = std::chrono::steady_clock::now();
				const Measured m = Measure(s_files, p.path, [] { return s_rescan.load(); });
				if (!m.ok && s_rescan.load())
					break;
				const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
				if (m.ok)
					ps5px_log("texture packs: %s: %llu files, %s (measured in %.1f s)", p.path.c_str(), static_cast<unsigned long long>(m.files),
						SizeText(m.bytes).c_str(), seconds);
				else
					ps5px_log("texture packs: %s could not be measured", p.path.c_str());
				{
					const std::lock_guard lock(s_mutex);
					if (m.ok)
						s_sizes[p.path] = {m.files, m.bytes};
					for (Pack& held : s_packs)
					{
						if (held.path == p.path)
						{
							held.measured = true;
							held.files = m.files;
							held.bytes = m.bytes;
						}
					}
				}
				Bump();
			}
		}
		s_scanning = false;
	} while (s_rescan.load() && !s_scanning.exchange(true));
}

void Said(std::function<void()> what)
{
	const std::lock_guard lock(s_mutex);
	s_said.push_back(std::move(what));
	s_relook = true;
}

std::string PlaceName(const std::string& textures)
{
	const std::lock_guard lock(s_mutex);
	for (const PlaceInfo& p : s_places)
	{
		for (const std::string& t : p.place.textures)
		{
			if (t == textures)
				return p.place.name;
		}
	}
	return textures;
}

bool BeginJob(const std::string& title, bool erase)
{
	bool expected = false;
	if (!s_job.compare_exchange_strong(expected, true))
		return false;
	s_job_stop = false;
	s_progress.files = 0;
	s_progress.bytes = 0;
	s_progress.totalFiles = 0;
	s_progress.totalBytes = 0;
	s_progress.phase = erase ? PS5TexturePackStore::Removing : PS5TexturePackStore::Measuring;
	const std::lock_guard lock(s_mutex);
	s_job_title = title;
	s_job_delete = erase;
	s_meter.Clear();
	return true;
}
} // namespace

unsigned Generation()
{
	return s_generation.load(std::memory_order_acquire);
}

std::vector<PlaceInfo> Places()
{
	const std::lock_guard lock(s_mutex);
	return s_places;
}

std::vector<Pack> Packs()
{
	const std::lock_guard lock(s_mutex);
	return s_packs;
}

void LookAgain(bool all)
{
	std::vector<PlaceInfo> places = MakePlaces();
	bool fetch = false;
	{
		const std::lock_guard lock(s_mutex);
		s_places = std::move(places);
		if (all)
			s_sizes.clear();
		if (all || s_collection_state == 0)
		{
			fetch = s_collection_state != 1;
			if (fetch)
				s_collection_state = 1;
		}
	}
	ps5px_log("texture packs: looking at %zu place(s)%s", Places().size(), all ? ", every pack measured anew" : "");
	Bump();
	if (fetch)
	{
		PS5TexturePacks::FindAll(all, [](std::vector<PS5TexturePacks::Pack> packs, std::string error) {
			{
				const std::lock_guard lock(s_mutex);
				s_collection_state = (packs.empty() && !error.empty()) ? 3 : 2;
				s_collection_error = error;
				s_collection = std::move(packs);
			}
			Bump();
		});
	}
	s_rescan = true;
	if (!s_scanning.exchange(true))
	{
		if (!RunDetached(ScanLoop))
			s_scanning = false;
	}
}

int Measuring()
{
	const std::lock_guard lock(s_mutex);
	int n = 0;
	for (const Pack& p : s_packs)
		n += p.measured ? 0 : 1;
	return n;
}

int CollectionState()
{
	const std::lock_guard lock(s_mutex);
	return s_collection_state;
}

std::string CollectionError()
{
	const std::lock_guard lock(s_mutex);
	return s_collection_error;
}

std::vector<PS5TexturePacks::Pack> Collection()
{
	const std::lock_guard lock(s_mutex);
	return s_collection;
}

bool Move(const Pack& pack, size_t place, std::function<void(bool ok, std::string sentence)> done)
{
	PlaceInfo to;
	{
		const std::lock_guard lock(s_mutex);
		if (place >= s_places.size())
			return false;
		to = s_places[place];
	}
	const std::string from = PlaceName(pack.textures);
	if (!BeginJob(fmt::format("Moving {} to {}", pack.serial, to.place.name), false))
		return false;
	ps5px_log("texture packs: moving %s from %s to %s", pack.path.c_str(), from.c_str(), to.place.target.c_str());
	const bool started = RunDetached([pack, to, from, done] {
		const auto began = std::chrono::steady_clock::now();
		const Outcome o = PS5TexturePackStore::Move(s_files, pack, to.place.target, [] { return s_job_stop.load(); }, &s_progress);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
		std::string sentence;
		if (o.ok)
		{
			sentence = fmt::format("The pack of {} is on {} now: {} files, {}.", pack.serial, to.place.name, o.files, SizeText(o.bytes));
			if (!o.error.empty())
				sentence += fmt::format(" What is left of it on {} could not all be deleted ({}): delete it there.", from, o.error);
			const std::lock_guard lock(s_mutex);
			s_sizes.erase(pack.path);
			s_sizes[o.path] = {o.files, o.bytes};
		}
		else if (o.stopped)
		{
			sentence = fmt::format("The move of {} was stopped. The pack is on {} as it was; moving it again goes on from what was copied.", pack.serial, from);
		}
		else
		{
			sentence = fmt::format("The pack of {} was not moved ({}). It is on {} as it was.", pack.serial, o.error, from);
		}
		ps5px_log("texture packs: move of %s %s in %.0f s: %s", pack.path.c_str(), o.ok ? "done" : o.stopped ? "stopped" : "failed", seconds,
			o.ok ? o.path.c_str() : o.error.c_str());
		const bool ok = o.ok;
		s_job = false;
		Said([done, ok, sentence] { done(ok, sentence); });
		Bump();
	});
	if (!started)
	{
		s_job = false;
		return false;
	}
	Bump();
	return true;
}

bool Delete(const Pack& pack, std::function<void(bool ok, std::string sentence)> done)
{
	const std::string where = PlaceName(pack.textures);
	if (!BeginJob(fmt::format("Deleting {} on {}", pack.serial, where), true))
		return false;
	if (pack.measured)
	{
		s_progress.totalFiles = pack.files;
		s_progress.totalBytes = pack.bytes;
	}
	ps5px_log("texture packs: deleting %s (%s)", pack.path.c_str(), pack.measured ? SizeText(pack.bytes).c_str() : "not measured");
	const bool started = RunDetached([pack, where, done] {
		const Outcome o = PS5TexturePackStore::Delete(s_files, pack, [] { return s_job_stop.load(); }, &s_progress);
		std::string sentence;
		if (o.ok)
			sentence = fmt::format("The pack of {} on {} is deleted ({} files).", pack.serial, where, o.files);
		else if (o.stopped)
			sentence = fmt::format("Stopped: {} files of the pack of {} were deleted. The game does not read what is left; deleting it again finishes it.",
				o.files, pack.serial);
		else
			sentence = fmt::format("The pack of {} on {} was not all deleted ({}).", pack.serial, where, o.error);
		ps5px_log("texture packs: delete of %s %s: %llu files", pack.path.c_str(), o.ok ? "done" : o.stopped ? "stopped" : "failed",
			static_cast<unsigned long long>(o.files));
		{
			const std::lock_guard lock(s_mutex);
			s_sizes.erase(pack.path);
		}
		const bool ok = o.ok;
		s_job = false;
		Said([done, ok, sentence] { done(ok, sentence); });
		Bump();
	});
	if (!started)
	{
		s_job = false;
		return false;
	}
	Bump();
	return true;
}

bool JobBusy()
{
	return s_job.load();
}

std::string JobLine()
{
	if (!s_job.load())
		return {};
	std::string title;
	{
		const std::lock_guard lock(s_mutex);
		title = s_job_title;
	}
	const uint64_t total = s_progress.totalBytes.load(), done = s_progress.bytes.load();
	static constexpr const char* phases[] = {"measuring", "copying", "checking the copy", "deleting"};
	const int phase = std::clamp(s_progress.phase.load(), 0, 3);
	if (total == 0)
		return fmt::format("{}: {}", title, phases[phase]);
	return fmt::format("{}: {}, {}%", title, phases[phase], std::min<uint64_t>(done * 100 / total, 100));
}

void StopJob()
{
	if (s_job.load())
	{
		s_job_stop = true;
		ps5px_log("texture packs: the move or delete is asked to stop");
	}
}

JobBar JobNow()
{
	JobBar bar;
	if (!s_job.load())
		return bar;
	bar.active = true;
	const std::lock_guard lock(s_mutex);
	static constexpr const char* phases[] = {" (measuring)", "", " (checking the copy)", " (deleting)"};
	const int phase = std::clamp(s_progress.phase.load(), 0, 3);
	bar.title = s_job_title + (s_job_delete ? std::string() : phases[phase]);
	bar.done = s_progress.bytes.load();
	bar.total = s_progress.totalBytes.load();
	if (phase == PS5TexturePackStore::Copying)
	{
		s_meter.Add(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), bar.done);
		bar.speed = s_meter.BytesPerSecond();
		bar.secondsLeft = PS5TexturePacksPieces::SecondsLeft(bar.done, bar.total, bar.speed);
	}
	return bar;
}

void Queue(const PS5TexturePacks::Pack& pack, const PS5TexturePacks::Place& place)
{
	s_queue.Add({pack, place});
	ps5px_log("texture packs: %s waits to be downloaded to %s (%zu in the queue)", pack.file.c_str(), place.root.c_str(), s_queue.Size());
	Bump();
}

std::vector<std::string> Queued()
{
	std::vector<std::string> out;
	for (const Waiting& w : s_queue.Items())
		out.push_back(fmt::format("{} to {}", w.pack.title, w.place.name));
	return out;
}

void Unqueue(size_t index)
{
	if (index < s_queue.Size())
		ps5px_log("texture packs: %s taken off the queue", s_queue.Items()[index].pack.file.c_str());
	s_queue.Remove(index);
	Bump();
}

bool Downloading(const std::string& serial)
{
	const auto has = [&serial](const std::vector<std::string>& serials) {
		return std::find(serials.begin(), serials.end(), serial) != serials.end();
	};
	if (PS5TexturePacks::Busy() && has(s_downloading_serials))
		return true;
	return s_queue.Has([&has](const Waiting& w) { return has(w.pack.serials); });
}

void Frame(const std::function<bool(const PS5TexturePacks::Pack&, const PS5TexturePacks::Place&)>& start)
{
	std::deque<std::function<void()>> said;
	bool relook = false;
	{
		const std::lock_guard lock(s_mutex);
		said.swap(s_said);
		relook = s_relook;
		s_relook = false;
	}
	for (const std::function<void()>& f : said)
		f();
	// A download that ended: its pack is looked for and measured.
	const bool busy = PS5TexturePacks::Busy();
	if (s_was_busy && !busy)
	{
		relook = true;
		s_downloading_serials.clear();
		Bump();
	}
	s_was_busy = busy;
	if (relook)
		LookAgain(false);
	if (std::optional<Waiting> next = s_queue.Next(busy))
	{
		ps5px_log("texture packs: the next in the queue: %s", next->pack.file.c_str());
		if (start(next->pack, next->place))
		{
			s_downloading_serials = next->pack.serials;
			s_was_busy = true;
		}
		Bump();
	}
}
} // namespace PS5TexturePackManager
