// Crux2 - the console's disc drive, for the interface (ps5_disc.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Two halves. The first is the parsing: a disc's ISO 9660 volume descriptor, its root folder and
// SYSTEM.CNF, read through a callback, with every number from the disc checked before use. The
// second is the watcher: finding the drive's device node, and the thread that looks at it.
//
// The console's own drive is /dev/cd0, FreeBSD's cd(4) node, and a title that has left its
// sandbox reads PlayStation 2 DVDs from it (run on a console with v0.3.0). A USB DVD drive is
// another cd(4) node (cd1 beside the console's own drive), read the same way, and it also reads
// CDs, which the console's own drive does not. Every step is logged once.

#include "ps5_disc.h"
#include "ps5_disc_image.h"

#include "ps5_platform.h"

#ifdef __PROSPERO__
#include <ps5platform/offload.h>
#else
// (The PC test: no console FTP server, every write the title's own.)
struct ps5_offload;
static int ps5_offload_setup(const char*, const char* const*, unsigned) { return -1; }
static void ps5_offload_note_write(size_t, double) {}
static bool ps5_offload_wanted() { return false; }
static struct ps5_offload* ps5_offload_begin(const char*, unsigned long long) { return nullptr; }
static int ps5_offload_write(struct ps5_offload*, const void*, size_t) { return -1; }
static int ps5_offload_end(struct ps5_offload*) { return -1; }
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/cdio.h>
#include <sys/disk.h>
#include <sys/dvdio.h>
#include <sys/ioctl.h>
#endif

namespace PS5Disc
{
	namespace
	{
		constexpr std::uint32_t kSector = 2048;
		constexpr std::uint32_t kVolumeDescriptors = 16; // the first volume descriptor's sector
		constexpr std::uint32_t kMaxRootSectors = 256; // a root folder of 512 KiB holds thousands of names
		constexpr std::uint32_t kMaxSystemCnf = 4096; // a SYSTEM.CNF is a few lines
		[[maybe_unused]] constexpr std::uint64_t kCDMaxSectors = 99 * 60 * 75; // the longest CDs made: 99 minutes

		std::uint32_t LE16(const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
		}

		std::uint32_t LE32(const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
				   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
		}

		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '\0';
		}

		std::string_view Strip(std::string_view text)
		{
			while (!text.empty() && IsSpace(text.front()))
				text.remove_prefix(1);
			while (!text.empty() && IsSpace(text.back()))
				text.remove_suffix(1);
			return text;
		}

		char Upper(char c)
		{
			return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
		}

		// A primary volume descriptor: type 1, "CD001".
		bool IsPrimaryVolume(const std::uint8_t* sector)
		{
			return sector[0] == 1 && std::memcmp(sector + 1, "CD001", 5) == 0;
		}

		// Whether a root folder entry's name is this file: without the ";1" version and a trailing
		// dot, capitals or not.
		bool NameIs(const std::uint8_t* name, std::size_t length, std::string_view wanted)
		{
			std::size_t end = length;
			for (std::size_t i = 0; i < length; i++)
			{
				if (name[i] == ';')
				{
					end = i;
					break;
				}
			}
			if (end > 0 && name[end - 1] == '.')
				end--;
			if (end != wanted.size())
				return false;
			for (std::size_t i = 0; i < end; i++)
			{
				if (Upper(static_cast<char>(name[i])) != wanted[i])
					return false;
			}
			return true;
		}

		struct RootEntry
		{
			bool found = false;
			bool directory = false;
			std::uint32_t lsn = 0;
			std::uint32_t size = 0;
		};

		struct Root
		{
			RootEntry system_cnf;
			RootEntry video_ts;
			RootEntry psx_exe;
		};

		// Walks the root folder's records for the three names that tell the kinds of disc apart.
		// Returns false when the folder cannot be read or is not a folder's data.
		bool ReadRoot(const SectorReader& read, std::uint32_t lsn, std::uint32_t bytes, std::uint64_t limit, Root* root)
		{
			const std::uint32_t sectors = std::min((bytes + kSector - 1) / kSector, kMaxRootSectors);
			if (sectors == 0 || lsn < kVolumeDescriptors || static_cast<std::uint64_t>(lsn) + sectors > limit)
				return false;

			std::vector<std::uint8_t> data(kSector);
			for (std::uint32_t s = 0; s < sectors; s++)
			{
				if (!read(lsn + s, 1, data.data()))
					return false;
				// Records do not cross a sector; a zero length ends the sector's records.
				for (std::uint32_t at = 0; at + 34 <= kSector;)
				{
					const std::uint8_t* const record = data.data() + at;
					const std::uint32_t length = record[0];
					if (length == 0)
						break;
					const std::uint32_t name_length = record[32];
					if (length < 34 || at + length > kSector || 33 + name_length > length)
						return false;

					RootEntry entry;
					entry.found = true;
					entry.directory = (record[25] & 0x02) != 0;
					entry.lsn = LE32(record + 2);
					entry.size = LE32(record + 10);
					const std::uint8_t* const name = record + 33;
					if (!entry.directory && NameIs(name, name_length, "SYSTEM.CNF"))
					{
						root->system_cnf = entry;
						return true; // nothing else is needed once this is known
					}
					if (entry.directory && NameIs(name, name_length, "VIDEO_TS"))
						root->video_ts = entry;
					else if (!entry.directory && NameIs(name, name_length, "PSX.EXE"))
						root->psx_exe = entry;
					at += length;
				}
			}
			return true;
		}

		std::string CleanLabel(const std::uint8_t* text, std::size_t length)
		{
			while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == 0))
				length--;
			std::string label;
			for (std::size_t i = 0; i < length; i++)
				label.push_back((text[i] >= 0x20 && text[i] < 0x7f) ? static_cast<char>(text[i]) : '?');
			return label;
		}
	} // namespace

	std::string SerialFromBootPath(std::string_view path)
	{
		// cdrom0:\SLUS_123.45;1, or cdrom:SLUS_123.45;1
		std::string_view name = path;
		if (const std::size_t slash = path.rfind('\\'); slash != std::string_view::npos)
			name = path.substr(slash + 1);
		else if (const std::size_t colon = path.rfind(':'); colon != std::string_view::npos)
			name = path.substr(colon + 1);
		if (const std::size_t version = name.rfind(';'); version != std::string_view::npos)
			name = name.substr(0, version);

		// ????_???.??* or ????-???.??*
		if (name.size() < 11 || (name[4] != '_' && name[4] != '-') || name[8] != '.')
			return {};

		std::string serial;
		for (const char c : name)
		{
			if (c == '.')
				continue;
			serial.push_back(c == '_' ? '-' : Upper(c));
		}
		return serial;
	}

	BootLine ParseSystemCnf(std::string_view text)
	{
		BootLine out;
		while (!text.empty())
		{
			const std::size_t end = text.find('\n');
			const std::string_view line = Strip(text.substr(0, end));
			text = (end == std::string_view::npos) ? std::string_view() : text.substr(end + 1);

			const std::size_t equals = line.find('=');
			if (equals == std::string_view::npos)
				continue;
			const std::string_view key = Strip(line.substr(0, equals));
			const std::string_view value = Strip(line.substr(equals + 1));
			if (key == "BOOT2")
				out.kind = Kind::PS2;
			else if (key == "BOOT")
				out.kind = Kind::PS1;
			else
				continue;
			out.path = std::string(value);
			out.serial = SerialFromBootPath(value);
		}
		return out;
	}

	Inspection Inspect(const SectorReader& read, std::uint64_t media_sectors)
	{
		Inspection out;
		// No sector at or past this is asked for: the media's size, or anything a 32-bit sector
		// number can name when the drive does not tell its size (the read then fails by itself).
		const std::uint64_t limit = media_sectors ? media_sectors : (std::uint64_t{1} << 32);
		std::vector<std::uint8_t> sector(kSector);

		// The volume descriptors: from sector 16 on, up to the terminator (type 255).
		bool primary = false;
		for (std::uint32_t lsn = kVolumeDescriptors; lsn < kVolumeDescriptors + 16 && lsn < limit; lsn++)
		{
			if (!read(lsn, 1, sector.data()))
			{
				if (lsn == kVolumeDescriptors)
					return out; // Unreadable
				break;
			}
			out.kind = Kind::Unknown;
			if (std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[0] == 255)
				break;
			if (sector[0] == 1)
			{
				primary = true;
				break;
			}
		}
		if (out.kind == Kind::Unreadable)
			return out; // a media smaller than 17 sectors

		out.sectors = media_sectors;
		out.layers = 1;
		if (!primary || LE16(sector.data() + 128) != kSector)
			return out; // not ISO 9660 (a UDF-only disc), or blocks this reader does not read

		out.iso9660 = true;
		out.volume_label = CleanLabel(sector.data() + 40, 32);
		out.volume_sectors = LE32(sector.data() + 80);
		const std::uint32_t root_lsn = LE32(sector.data() + 156 + 2);
		const std::uint32_t root_bytes = LE32(sector.data() + 156 + 10);

		// A dual-layer PlayStation 2 disc is two volumes, one a layer: the second starts where the
		// first one's size says it ends, with a primary volume descriptor of its own. (This is how
		// the emulator's reader of image files finds the layer, CDVDisoReader.cpp.)
		std::uint32_t second_volume_sectors = 0;
		if (out.volume_sectors > kVolumeDescriptors && out.volume_sectors < limit)
		{
			std::vector<std::uint8_t> other(kSector);
			if (read(out.volume_sectors, 1, other.data()) && IsPrimaryVolume(other.data()))
			{
				out.layer1_start = out.volume_sectors;
				out.layers = 2;
				second_volume_sectors = LE32(other.data() + 80);
			}
		}
		if (out.sectors == 0)
			out.sectors = static_cast<std::uint64_t>(out.volume_sectors) + second_volume_sectors;

		Root root;
		if (!ReadRoot(read, root_lsn, root_bytes, limit, &root))
			return out;

		if (root.system_cnf.found)
		{
			const std::uint32_t bytes = std::min(root.system_cnf.size, kMaxSystemCnf);
			const std::uint32_t sectors = (bytes + kSector - 1) / kSector;
			std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSector);
			if (sectors != 0 && root.system_cnf.lsn >= kVolumeDescriptors &&
				static_cast<std::uint64_t>(root.system_cnf.lsn) + sectors <= limit &&
				read(root.system_cnf.lsn, sectors, data.data()))
			{
				const BootLine boot = ParseSystemCnf(std::string_view(reinterpret_cast<const char*>(data.data()), bytes));
				out.system_cnf_lsn = root.system_cnf.lsn;
				out.boot_path = boot.path;
				out.serial = boot.serial;
				if (boot.kind != Kind::Unknown)
				{
					out.kind = boot.kind;
					return out;
				}
			}
		}
		if (root.video_ts.found)
			out.kind = Kind::DVDVideo;
		else if (root.psx_exe.found)
			out.kind = Kind::PS1;
		return out;
	}

	State Describe(const Inspection& inspection)
	{
		State state;
		state.present = inspection.kind != Kind::Unreadable;
		state.ps2 = inspection.kind == Kind::PS2;
		state.volume_label = inspection.volume_label;
		state.sectors = state.present ? inspection.sectors : 0;
		state.layers = state.present ? inspection.layers : 0;
		switch (inspection.kind)
		{
			case Kind::PS2:
				state.serial = inspection.serial;
				break;
			case Kind::PS1:
				state.serial = inspection.serial;
				state.note = "This is a PlayStation (PS1) disc; only PlayStation 2 discs can be played.";
				break;
			case Kind::DVDVideo:
				state.note = "This is a DVD-Video disc, not a game.";
				break;
			case Kind::Unknown:
				state.note = "This disc is not a PlayStation 2 game.";
				break;
			case Kind::Unreadable:
				state.note = "The disc in the drive cannot be read.";
				break;
		}
		return state;
	}

	// ---------------------------------------------------------------------------------------
	// The drive

	namespace
	{
		// Whole sectors by pread; the errno of the first failure is kept for the log (-1: the
		// device ended before the sectors did).
		struct DeviceReader
		{
			int fd = -1;
			int first_errno = 0;

			bool operator()(std::uint32_t lsn, std::uint32_t count, std::uint8_t* out)
			{
				std::size_t left = static_cast<std::size_t>(count) * kSector;
				off_t offset = static_cast<off_t>(lsn) * kSector;
				while (left != 0)
				{
					const ssize_t got = pread(fd, out, left, offset);
					if (got < 0 && errno == EINTR)
						continue;
					if (got <= 0)
					{
						if (first_errno == 0)
							first_errno = (got < 0) ? errno : -1;
						return false;
					}
					out += got;
					left -= static_cast<std::size_t>(got);
					offset += got;
				}
				return true;
			}
		};

		// What the device says its size is, without reading the disc: FreeBSD's media size of a
		// disk device; the length of an image file (the PC tests, and a console whose drive were
		// given as a file). `token` is something that differs when the disc is another one (0:
		// nothing to tell by), `how` names the source for the log.
		std::uint64_t MediaSectors(int fd, std::uint64_t* token, const char** how, int* ioctl_errno)
		{
			*token = 0;
			*how = "not told";
			*ioctl_errno = 0;
#if defined(__FreeBSD__)
			off_t bytes = 0;
			if (ioctl(fd, DIOCGMEDIASIZE, &bytes) == 0)
			{
				*how = "DIOCGMEDIASIZE";
				if (bytes >= static_cast<off_t>(kSector))
				{
					*token = static_cast<std::uint64_t>(bytes) | (std::uint64_t{1} << 63);
					return static_cast<std::uint64_t>(bytes) / kSector;
				}
				return 0;
			}
			*ioctl_errno = errno;
#endif
			struct stat st;
			if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
			{
				*how = "file size";
				// (Another file of the same length is another disc: the PC tests swap images.)
				*token = (static_cast<std::uint64_t>(st.st_size) * 0x9e3779b97f4a7c15ull) ^ (static_cast<std::uint64_t>(st.st_ino) << 20) ^
						 static_cast<std::uint64_t>(st.st_mtim.tv_sec) ^ (static_cast<std::uint64_t>(st.st_mtim.tv_nsec) << 34) ^
						 (std::uint64_t{1} << 63);
				return static_cast<std::uint64_t>(st.st_size) / kSector;
			}
			return 0;
		}

		struct Look
		{
			State state;
			Inspection inspection;
			int open_errno = 0;
			int read_errno = 0;
			int size_errno = 0;
			std::uint64_t media_sectors = 0;
			std::uint64_t token = 0;
			const char* size_source = "";
		};

		// O_NONBLOCK: an optical drive's node opens with it even when the drive is empty.
		int OpenDevice(const std::string& device)
		{
			return open(device.c_str(), O_RDONLY | O_NONBLOCK);
		}

		// Whether the disc is a CD: the drive answers no DVD structure for it, and has a table of
		// contents (a DVD drive makes one up for a DVD too, which is why the DVD question comes
		// first), within what a CD holds. False where the device has neither question (an image
		// file on the PC tests).
		bool MediaIsCD(int fd)
		{
#if defined(__FreeBSD__)
			struct dvd_struct request = {};
			request.format = DVD_STRUCT_PHYSICAL;
			request.layer_num = 0;
			if (ioctl(fd, DVDIOCREADSTRUCTURE, &request) == 0)
				return false;
			struct ioc_toc_header header = {};
			if (ioctl(fd, CDIOREADTOCHEADER, &header) != 0 || header.starting_track == 0 || header.ending_track < header.starting_track)
				return false;
			off_t bytes = 0;
			return ioctl(fd, DIOCGMEDIASIZE, &bytes) != 0 || static_cast<std::uint64_t>(bytes) / kSector <= kCDMaxSectors;
#else
			(void)fd;
			return false;
#endif
		}

		void LookAt(int fd, Look* look)
		{
			look->media_sectors = MediaSectors(fd, &look->token, &look->size_source, &look->size_errno);
			DeviceReader reader{fd, 0};
			look->inspection = Inspect(std::ref(reader), look->media_sectors);
			look->read_errno = reader.first_errno;
			look->state = Describe(look->inspection);
			look->state.cd = look->state.present && MediaIsCD(fd);
			// Reads that fail on a drive that reports no media are an empty drive, not a bad disc.
			if (look->inspection.kind == Kind::Unreadable && look->media_sectors == 0)
				look->state.note.clear();
		}
	} // namespace

	State Probe(const std::string& device, int* open_errno)
	{
		Look look;
		const int fd = OpenDevice(device);
		if (fd < 0)
		{
			look.open_errno = errno;
		}
		else
		{
			LookAt(fd, &look);
			close(fd);
		}
		if (open_errno)
			*open_errno = look.open_errno;
		look.state.device = device;
		return look.state;
	}

	// ---------------------------------------------------------------------------------------
	// The watcher
	//
	// Every disc drive is watched: the console's own (cd0 on a console with a drive) and USB DVD
	// drives (cd1 and on; cd0 on a Digital Edition). /dev is listed again at every look, so a
	// USB drive plugged in while Crux2 runs is found within one look, and one pulled out goes.

	namespace
	{
		constexpr auto kInterval = std::chrono::seconds(2);

		// One drive, as the thread knows it (nothing else reads these).
		struct Watched
		{
			std::string device;
			State state; // what was last published for it
			bool known = false; // the disc of known_token has been read and described
			std::uint64_t known_token = 0;
			std::vector<std::uint8_t> known_volume; // its sector 16, for drives that tell no size
			int logged_open_errno = -2; // what the log last said about open() (-2: nothing yet)
			int logged_read_errno = 0;
			int failed_looks = 0; // looks in a row that found no disc that can be read
			unsigned skipped = 0; // looks passed over while it is looked at less often
		};

		std::mutex s_mutex; // everything below up to s_io_mutex
		std::condition_variable s_wake;
		std::thread s_thread;
		bool s_running = false;
		bool s_stop = false;
		bool s_kick = false; // look now, and read the discs again even if they seem the same ones
		std::vector<State> s_states; // every drive, in the order of their names
		std::string s_override;
		std::string s_dev_folder = "/dev";
		std::atomic<unsigned> s_generation{0};

		// Held while the watcher has a drive open. SetInUse(true) takes it once, which is how it
		// knows the watcher is out of the drive.
		std::mutex s_io_mutex;
		std::atomic<bool> s_in_use{false};
		std::string s_lent; // the drive the emulator has (empty with s_in_use: all of them); under s_mutex

		// The thread's own.
		std::vector<Watched> s_drives;
		std::string s_logged_nodes; // the optical names /dev was last said to have

		bool SameState(const State& a, const State& b)
		{
			return a.drive_found == b.drive_found && a.present == b.present && a.ps2 == b.ps2 && a.cd == b.cd && a.device == b.device &&
				   a.serial == b.serial && a.volume_label == b.volume_label && a.sectors == b.sectors && a.layers == b.layers && a.note == b.note;
		}

		// What the interface sees: every drive's last state, published when one of them changed.
		void Publish()
		{
			std::vector<State> states;
			states.reserve(s_drives.size());
			for (const Watched& drive : s_drives)
				states.push_back(drive.state);
			const std::lock_guard lock(s_mutex);
			bool same = states.size() == s_states.size();
			for (std::size_t i = 0; same && i < states.size(); i++)
				same = SameState(states[i], s_states[i]);
			if (same)
				return;
			s_states = std::move(states);
			s_generation.fetch_add(1, std::memory_order_release);
		}

		// The names an optical drive's node has had: FreeBSD's cd(4) ("cd0", "cd1"...), then what
		// other systems of the family have called theirs. A name followed by anything but digits
		// (a partition, "cdev") is not a drive.
		bool LooksOptical(std::string_view name)
		{
			for (const std::string_view prefix : {std::string_view("cd"), std::string_view("bd"), std::string_view("dvd"), std::string_view("acd")})
			{
				if (!name.starts_with(prefix) || name.size() == prefix.size())
					continue;
				const std::string_view rest = name.substr(prefix.size());
				if (std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; }))
					return true;
			}
			return false;
		}

		// "cd0" before "cd1" before "cd10": the console's own drive is the first.
		bool NodeBefore(const std::string& a, const std::string& b)
		{
			const auto split = [](const std::string& path) {
				std::size_t digits = path.size();
				while (digits > 0 && path[digits - 1] >= '0' && path[digits - 1] <= '9')
					digits--;
				const unsigned long number = (digits < path.size()) ? std::strtoul(path.c_str() + digits, nullptr, 10) : 0;
				return std::make_pair(path.substr(0, digits), number);
			};
			return split(a) < split(b);
		}

		// The drives' nodes in /dev now. The list goes to the log when it differs from the last
		// one, with whether the SCSI pass-through nodes are there (not opened: they are every
		// disk's).
		std::vector<std::string> ListDrives(const std::string& folder)
		{
			std::vector<std::string> nodes;
			std::string optical, passthrough;
			unsigned entries = 0;
			DIR* const dir = opendir(folder.c_str());
			if (dir == nullptr)
			{
				const std::string said = "error " + std::to_string(errno);
				if (said != s_logged_nodes)
				{
					ps5px_log("disc: %s cannot be listed: errno %d (%s)", folder.c_str(), errno, std::strerror(errno));
					s_logged_nodes = said;
				}
				return nodes;
			}
			while (const struct dirent* const entry = readdir(dir))
			{
				const std::string_view name = entry->d_name;
				entries++;
				if (LooksOptical(name))
				{
					nodes.push_back(folder + "/" + std::string(name));
				}
				else if (name.starts_with("pass") || name.starts_with("xpt"))
				{
					passthrough += ' ';
					passthrough += name;
				}
			}
			closedir(dir);
			std::sort(nodes.begin(), nodes.end(), NodeBefore);
			for (const std::string& node : nodes)
				optical += " " + node.substr(folder.size() + 1);
			if (optical != s_logged_nodes)
			{
				ps5px_log("disc: %s has %u names (uid %d); optical:%s; pass-through:%s", folder.c_str(), entries, static_cast<int>(getuid()),
					optical.empty() ? " none" : optical.c_str(), passthrough.empty() ? " none" : passthrough.c_str());
				s_logged_nodes = optical;
			}
			return nodes;
		}

		// Lets the drive's tray open again: the console locks a USB drive's tray when it finds a disc
		// it does not play (as it does at start-up, before a jailbreak), and its eject button then
		// does nothing. CDIOCALLOW sends the drive "allow medium removal"; with `eject`, CDIOCEJECT
		// opens the tray. 0, or the errno of the step that failed.
		int Release(const std::string& device, bool eject, const char** step)
		{
			*step = "open";
#if defined(__FreeBSD__)
			const int fd = OpenDevice(device);
			if (fd < 0)
				return errno;
			int error = 0;
			*step = "unlock";
			if (ioctl(fd, CDIOCALLOW) != 0)
				error = errno;
			if (error == 0 && eject)
			{
				*step = "eject";
				if (ioctl(fd, CDIOCEJECT) != 0)
					error = errno;
			}
			close(fd);
			return error;
#else
			(void)device;
			(void)eject;
			return ENOTTY;
#endif
		}

		// Brings s_drives to the nodes there are now: new ones (a USB drive plugged in) are added,
		// gone ones (pulled out) are dropped. With an override, only that one.
		void UpdateDrives(const std::string& override_device, const std::string& folder)
		{
			std::vector<std::string> nodes;
			if (!override_device.empty())
				nodes.push_back(override_device);
			else
				nodes = ListDrives(folder);

			// (Named before the drives still there are moved over below: a moved-from entry has no
			// name left, and would read as a drive gone.)
			for (const Watched& drive : s_drives)
			{
				if (std::find(nodes.begin(), nodes.end(), drive.device) == nodes.end())
					ps5px_log("disc: %s is gone (a USB drive pulled out)", drive.device.c_str());
			}

			std::vector<Watched> drives;
			drives.reserve(nodes.size());
			for (const std::string& node : nodes)
			{
				const auto old = std::find_if(s_drives.begin(), s_drives.end(), [&](const Watched& d) { return d.device == node; });
				if (old != s_drives.end())
				{
					drives.push_back(std::move(*old));
					continue;
				}
				Watched drive;
				drive.device = node;
				drive.state.drive_found = true;
				drive.state.device = node;
				ps5px_log("disc: a drive: %s%s", node.c_str(), s_drives.empty() && drives.empty() ? "" : " (one more)");
				{
					// Its tray unlocked once, when it is first seen (the console may have locked it).
					const char* step = "";
					const int error = Release(node, false, &step);
					ps5px_log("disc: %s: tray unlocked: %s%s%s", node.c_str(), error == 0 ? "ok" : step, error == 0 ? "" : " failed, ",
						error == 0 ? "" : std::strerror(error));
				}
				drives.push_back(std::move(drive));
			}
			s_drives = std::move(drives);
		}

		State NoDisc(const Watched& drive)
		{
			State state;
			state.drive_found = true;
			state.device = drive.device;
			return state;
		}

		// One look at one drive. A disc that was already read is not read again: its size is asked
		// for (which is no work for the drive), so that a disc lying in the drive is not kept
		// spinning by the interface.
		void LookOnce(Watched& drive, bool again)
		{
			const int fd = OpenDevice(drive.device);
			if (fd < 0)
			{
				const int error = errno;
				if (error != drive.logged_open_errno)
				{
					ps5px_log("disc: %s does not open: errno %d (%s)", drive.device.c_str(), error, std::strerror(error));
					drive.logged_open_errno = error;
				}
				drive.known = false;
				State state = NoDisc(drive);
				// (A drive the console does not let the title open is said, not passed over as empty.)
				if (error == EACCES || error == EPERM)
					state.note = "The console does not let Crux2 read this disc drive.";
				drive.state = std::move(state);
				drive.failed_looks++;
				return;
			}
			if (drive.logged_open_errno > 0)
				ps5px_log("disc: %s opens again", drive.device.c_str());
			drive.logged_open_errno = 0;

			Look look;
			std::vector<std::uint8_t> volume;
			{
				// Is it the disc that is already known?
				int size_errno = 0;
				const char* how = "";
				std::uint64_t token = 0;
				MediaSectors(fd, &token, &how, &size_errno);
				if (token == 0)
				{
					// The drive tells no size: sector 16 is the disc's name tag.
					volume.resize(kSector);
					DeviceReader reader{fd, 0};
					if (!reader(kVolumeDescriptors, 1, volume.data()))
						volume.clear();
				}
				if (drive.known && !again &&
					((token != 0 && token == drive.known_token) || (token == 0 && !volume.empty() && volume == drive.known_volume)))
				{
					close(fd);
					return;
				}
			}

			LookAt(fd, &look);
			close(fd);

			State state = look.state;
			state.drive_found = true;
			state.device = drive.device;

			// A disc that could not be read is looked at again next time (a drive may need a
			// moment after the disc goes in); one that was read is remembered.
			drive.known = state.present;
			drive.known_token = look.token;
			drive.known_volume = std::move(volume);

			// (A disc that cannot be read is not asked for every two seconds for ever.)
			drive.failed_looks = state.present ? 0 : drive.failed_looks + 1;

			const bool changed = !SameState(state, drive.state);
			drive.state = state;
			if (!state.present)
			{
				if (changed || look.read_errno != drive.logged_read_errno)
				{
					if (look.read_errno == -1)
						ps5px_log("disc: %s: no disc that can be read: sector 16 is past the end (size: %llu sectors, %s, ioctl errno %d)",
							drive.device.c_str(), static_cast<unsigned long long>(look.media_sectors), look.size_source, look.size_errno);
					else
						ps5px_log("disc: %s: no disc that can be read: sector 16 read errno %d (%s) (size: %llu sectors, %s, ioctl errno %d)",
							drive.device.c_str(), look.read_errno, std::strerror(look.read_errno), static_cast<unsigned long long>(look.media_sectors),
							look.size_source, look.size_errno);
					drive.logged_read_errno = look.read_errno;
				}
				return;
			}
			drive.logged_read_errno = 0;
			if (!changed && !again)
				return;

			const Inspection& in = look.inspection;
			const char* const kind = (in.kind == Kind::PS2) ? "PlayStation 2" : (in.kind == Kind::PS1) ? "PlayStation 1" :
									 (in.kind == Kind::DVDVideo)                                       ? "DVD-Video" :
																										 "unknown";
			ps5px_log("disc: %s: %s %s%s%s, label '%s', %llu sectors (drive: %llu, %s, ioctl errno %d; volume: %u), %d layer%s%s%s", drive.device.c_str(),
				kind, state.cd ? "CD" : "DVD", state.serial.empty() ? "" : " ", state.serial.c_str(), state.volume_label.c_str(),
				static_cast<unsigned long long>(state.sectors), static_cast<unsigned long long>(look.media_sectors), look.size_source, look.size_errno,
				in.volume_sectors, state.layers, state.layers == 1 ? "" : "s", in.iso9660 ? "" : ", no ISO 9660 volume",
				in.layers == 2 ? ", second layer found by its volume descriptor" : "");
			if (in.layers == 2)
				ps5px_log("disc: second layer starts at sector %u", in.layer1_start);
			if (in.system_cnf_lsn != 0)
				ps5px_log("disc: SYSTEM.CNF at sector %u boots '%s'", in.system_cnf_lsn, in.boot_path.c_str());
		}

		// Every drive once, but the one the emulator has (or all of them while it has an unnamed
		// one). A drive with nothing readable in it for a while is looked at every third time.
		void LookAll(bool again)
		{
			const std::lock_guard io(s_io_mutex);
			std::string lent;
			bool all_lent;
			{
				const std::lock_guard lock(s_mutex);
				const bool in_use = s_in_use.load(std::memory_order_acquire);
				lent = in_use ? s_lent : std::string();
				all_lent = in_use && s_lent.empty();
			}
			if (!all_lent)
			{
				for (Watched& drive : s_drives)
				{
					if (drive.device == lent)
						continue;
					if (!again && drive.failed_looks > 15 && (drive.skipped++ % 3) != 0)
						continue;
					LookOnce(drive, again);
				}
			}
			Publish();
		}

		void ThreadMain()
		{
			std::string override_device, folder;
			{
				const std::lock_guard lock(s_mutex);
				override_device = s_override;
				folder = s_dev_folder;
			}
			if (!override_device.empty())
				ps5px_log("disc: told to use %s", override_device.c_str());
			s_drives.clear();
			s_logged_nodes = "(not listed yet)";

			std::unique_lock lock(s_mutex);
			while (!s_stop)
			{
				const bool again = s_kick;
				s_kick = false;
				lock.unlock();
				UpdateDrives(override_device, folder);
				LookAll(again);
				lock.lock();
				s_wake.wait_for(lock, kInterval, [] { return s_stop || s_kick; });
			}
		}
	} // namespace

	void SetDeviceOverride(std::string device)
	{
		const std::lock_guard lock(s_mutex);
		s_override = std::move(device);
	}

	void SetDeviceFolder(std::string folder)
	{
		const std::lock_guard lock(s_mutex);
		s_dev_folder = std::move(folder);
	}

	void Start()
	{
		const std::lock_guard lock(s_mutex);
		if (s_running)
			return;
		s_running = true;
		s_stop = false;
		s_kick = false;
		s_states.clear();
		s_generation.fetch_add(1, std::memory_order_release);
		s_thread = std::thread(ThreadMain);
	}

	void Stop()
	{
		{
			const std::lock_guard lock(s_mutex);
			if (!s_running)
				return;
			s_stop = true;
		}
		s_wake.notify_all();
		s_thread.join();
		const std::lock_guard lock(s_mutex);
		s_running = false;
	}

	void SetInUse(bool in_use, const std::string& device)
	{
		if (in_use)
		{
			{
				const std::lock_guard lock(s_mutex);
				s_lent = device;
			}
			s_in_use.store(true, std::memory_order_release);
			// A look that had begun finishes first; none of this drive begins after.
			const std::lock_guard io(s_io_mutex);
			return;
		}
		s_in_use.store(false, std::memory_order_release);
		{
			const std::lock_guard lock(s_mutex);
			s_lent.clear();
			s_kick = true;
		}
		s_wake.notify_all();
	}

	std::vector<State> Drives()
	{
		const std::lock_guard lock(s_mutex);
		return s_states;
	}

	State Current()
	{
		const std::lock_guard lock(s_mutex);
		for (const State& state : s_states)
		{
			if (state.present && state.ps2)
				return state;
		}
		for (const State& state : s_states)
		{
			if (state.present)
				return state;
		}
		if (!s_states.empty())
			return s_states.front();
		State none;
		none.note = "No disc drive was found.";
		return none;
	}

	State Drive(const std::string& device)
	{
		const std::lock_guard lock(s_mutex);
		for (const State& state : s_states)
		{
			if (state.device == device)
				return state;
		}
		return State();
	}

	bool Eject(const std::string& device, std::string* error)
	{
		{
			const std::lock_guard lock(s_mutex);
			if (s_in_use.load(std::memory_order_acquire) && (s_lent.empty() || s_lent == device))
			{
				*error = "A game is playing from this drive: close it first.";
				return false;
			}
		}
		const char* step = "";
		int result;
		{
			// (Not in the middle of a look at the drive.)
			const std::lock_guard io(s_io_mutex);
			result = Release(device, true, &step);
		}
		ps5px_log("disc: %s: eject: %s%s%s", device.c_str(), result == 0 ? "ok" : step, result == 0 ? "" : " failed, ",
			result == 0 ? "" : std::strerror(result));
		{
			const std::lock_guard lock(s_mutex);
			s_kick = true;
		}
		s_wake.notify_all();
		if (result != 0)
			*error = std::string("The drive did not open (") + step + ": " + std::strerror(result) + ").";
		return result == 0;
	}

	unsigned Generation()
	{
		return s_generation.load(std::memory_order_acquire);
	}

	// The disc as an image file: every sector, in order, into `path` (made as path.part and put in
	// its place when whole). The watcher keeps away meanwhile. Large files are written through the
	// console's FTP server once the title's own writes are held back (ps5platform/offload.h): a
	// dual-layer disc is more than the 1.3 GiB the console lets a title write at full speed.
	// The disc as an image written by someone else (a network share's file layer): the writer is
	// given the size and a source that reads the disc where it is asked to, so the image never has
	// to fit in memory. The watcher keeps away meanwhile.
	bool SaveImageWith(const std::string& device, const ImageWriter& write,
		const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error)
	{
		const State state = device.empty() ? Current() : Drive(device);
		if (!state.present || state.device.empty() || state.sectors == 0)
		{
			*error = "There is no disc in the drive to read.";
			return false;
		}
		SetInUse(true, state.device);
		const int fd = open(state.device.c_str(), O_RDONLY);
		if (fd < 0)
		{
			*error = std::string("The drive could not be opened: ") + std::strerror(errno);
			SetInUse(false);
			return false;
		}
		const std::uint64_t total = state.sectors * kSector;
		std::string read_error;
		const auto source = [&](void* buffer, std::uint32_t length, std::uint64_t offset) -> std::int64_t {
			std::uint8_t* const out = static_cast<std::uint8_t*>(buffer);
			std::size_t got = 0;
			// A read the drive fails is tried again: a scratch is often read on the second try.
			for (int attempt = 0; attempt < 4 && got < length; attempt++)
			{
				const ssize_t r = pread(fd, out + got, length - got, static_cast<off_t>(offset + got));
				if (r > 0)
				{
					got += static_cast<std::size_t>(r);
					attempt = -1;
				}
				else if (r < 0 && errno == EINTR)
				{
					attempt--;
				}
			}
			if (got < length)
			{
				read_error = "The disc could not be read at " + std::to_string((offset + got) / kSector) + " (of " +
							 std::to_string(state.sectors) + " sectors). It may be scratched or dirty.";
				errno = EIO;
				return -1;
			}
			if (progress && !progress(offset + length, total))
			{
				read_error = "Stopped.";
				errno = ECANCELED;
				return -1;
			}
			return length;
		};
		std::string write_error;
		const bool ok = write(total, source, &write_error);
		close(fd);
		SetInUse(false);
		if (!ok)
			*error = !read_error.empty() ? read_error : !write_error.empty() ? write_error : std::string("The image could not be written.");
		ps5px_log("disc: an image of %s %s (%llu MB, written elsewhere)%s%s", state.serial.c_str(), ok ? "was made" : "was not made",
			static_cast<unsigned long long>(total >> 20), ok ? "" : ": ", ok ? "" : error->c_str());
		return ok;
	}

	bool SaveImage(const std::string& device, const std::string& path, const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress,
		std::string* error)
	{
		return SaveImageAs(device, path, PS5DiscImage::Format::Iso, progress, error);
	}

	bool SaveImageAs(const std::string& device, const std::string& path, PS5DiscImage::Format format,
		const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error)
	{
		const State state = device.empty() ? Current() : Drive(device);
		if (!state.present || state.device.empty() || state.sectors == 0)
		{
			*error = "There is no disc in the drive to read.";
			return false;
		}
		SetInUse(true, state.device);
		const int fd = open(state.device.c_str(), O_RDONLY);
		if (fd < 0)
		{
			*error = std::string("The drive could not be opened: ") + std::strerror(errno);
			SetInUse(false);
			return false;
		}
		const std::string part = path + ".part";
		const int out = open(part.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (out < 0)
		{
			*error = std::string("The image could not be made: ") + std::strerror(errno);
			close(fd);
			SetInUse(false);
			return false;
		}
		const std::uint64_t total = state.sectors * kSector;
		{
			// (Once: where the console's FTP server sees this folder.)
			const std::string folder = path.substr(0, path.find_last_of('/'));
			if (ps5_offload_setup(folder.c_str(), nullptr, 0) != 0)
				ps5px_log("disc: the console's FTP server was not found: the image is written by the title alone (slower after 1.3 GB)");
		}

		// What the encoder gives goes into the file: by the title while its writes go fast, through
		// the FTP server once they do not.
		struct ps5_offload* stream = nullptr;
		std::uint64_t in_file = 0;
		const PS5DiscImage::Append append = [&](const void* data, std::size_t size) {
			if (stream == nullptr && ps5_offload_wanted())
				stream = ps5_offload_begin(part.c_str(), in_file);
			bool written = true;
			if (stream != nullptr)
			{
				written = (ps5_offload_write(stream, data, size) == 0);
			}
			else
			{
				const auto started = std::chrono::steady_clock::now();
				const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
				std::size_t done = 0;
				while (written && done < size)
				{
					const ssize_t w = write(out, p + done, size - done);
					if (w < 0 && errno == EINTR)
						continue;
					written = (w > 0);
					done += (w > 0) ? static_cast<std::size_t>(w) : 0;
				}
				ps5_offload_note_write(size, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
			}
			if (written)
				in_file += size;
			return written;
		};
		// (The compressed formats keep six of the console's cores busy: the disc is read meanwhile.)
		std::unique_ptr<PS5DiscImage::Encoder> encoder = PS5DiscImage::Encoder::Create(format, total, 6, append);

		constexpr std::size_t CHUNK = 1u << 20; // 512 sectors
		std::vector<std::uint8_t> buffer(CHUNK);
		std::uint64_t done = 0;
		bool ok = true;
		const auto started = std::chrono::steady_clock::now();
		while (ok && done < total)
		{
			const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(CHUNK, total - done));
			std::size_t got = 0;
			// A read the drive fails is tried again: a scratch is often read on the second try.
			for (int attempt = 0; attempt < 4 && got < want; attempt++)
			{
				const ssize_t r = pread(fd, buffer.data() + got, want - got, static_cast<off_t>(done + got));
				if (r > 0)
				{
					got += static_cast<std::size_t>(r);
					attempt = -1;
				}
				else if (r < 0 && errno == EINTR)
				{
					attempt--;
				}
			}
			if (got < want)
			{
				*error = "The disc could not be read at " + std::to_string((done + got) / kSector) + " (of " + std::to_string(state.sectors) +
						 " sectors). It may be scratched or dirty.";
				ok = false;
				break;
			}
			if (!encoder->Add(buffer.data(), want))
			{
				*error = encoder->Error();
				ok = false;
				break;
			}
			done += want;
			if (progress && !progress(done, total))
			{
				*error = "Stopped.";
				ok = false;
			}
		}
		close(fd);
		SetInUse(false);
		std::vector<PS5DiscImage::Patch> patches;
		if (ok && !encoder->Finish(&patches))
		{
			*error = encoder->Error();
			ok = false;
		}
		if (stream != nullptr && ps5_offload_end(stream) != 0 && ok)
		{
			*error = "The image could not be written to the end.";
			ok = false;
		}
		// What is only known at the end: the CHD's header, the CSO's index.
		for (const PS5DiscImage::Patch& patch : patches)
		{
			if (!ok)
				break;
			std::size_t put = 0;
			while (ok && put < patch.bytes.size())
			{
				const ssize_t w = pwrite(out, patch.bytes.data() + put, patch.bytes.size() - put, static_cast<off_t>(patch.offset + put));
				if (w < 0 && errno == EINTR)
					continue;
				ok = (w > 0);
				put += (w > 0) ? static_cast<std::size_t>(w) : 0;
			}
			if (!ok)
				*error = "The image could not be finished (is the drive full?).";
		}
		const std::uint64_t size = encoder->Written();
		close(out);
		struct stat st;
		if (ok && (stat(part.c_str(), &st) != 0 || static_cast<std::uint64_t>(st.st_size) != size))
		{
			*error = "The image is not as large as it should be.";
			ok = false;
		}
		if (ok && (unlink(path.c_str()), rename(part.c_str(), path.c_str()) != 0))
		{
			*error = std::string("The image could not be put in place: ") + std::strerror(errno);
			ok = false;
		}
		if (!ok)
			unlink(part.c_str());
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		ps5px_log("disc: an image (%s) of %s %s: %llu of %llu MB read, %llu MB written, %.0f s", PS5DiscImage::Name(format), state.serial.c_str(),
			ok ? "was made" : "was not made", static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(total >> 20),
			static_cast<unsigned long long>(size >> 20), seconds);
		return ok;
	}
} // namespace PS5Disc
