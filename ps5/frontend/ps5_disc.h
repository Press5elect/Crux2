// Crux2 - the disc drives (the console's own and USB DVD drives), for the interface: is there a
// disc in each, and is it a PlayStation 2 game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A thread looks at every drive every two seconds and keeps a State of each the interface reads. It has its
// own small reader of the disc's ISO 9660 file system (the volume descriptor, the root folder and
// SYSTEM.CNF: a handful of sectors), over pread on the drive's device node; nothing of the
// emulator is used, so it runs with or without a game. While the emulator plays from the drive
// (SetInUse) the thread leaves the drive alone: a second reader would send the head elsewhere
// in the middle of the game's reads.
//
// The parsing below the watcher is plain C++ with no console in it; ps5/tests/disc runs it on a PC
// against image files.

#pragma once

#include "ps5_disc_image.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Disc
{
	struct State
	{
		bool drive_found = false; ///< a device node for the drive exists
		bool present = false; ///< a disc is in the drive and its sectors can be read
		bool ps2 = false; ///< it is a PlayStation 2 game (SYSTEM.CNF has a BOOT2 line)
		bool cd = false; ///< the disc is a CD (blue underside; read only by USB drives), not a DVD
		std::string device; ///< what to give the emulator as the disc ("/dev/cd0")
		std::string serial; ///< "SLUS-12345"; empty when the boot line has no serial in it
		std::string volume_label;
		std::uint64_t sectors = 0; ///< 2048-byte sectors on the disc
		int layers = 0; ///< 1 or 2; 0 without a disc
		std::string note; ///< one sentence for the interface when there is no playable disc
	};

	/// Uses this device node (or image file) instead of looking for the drive. Before Start().
	void SetDeviceOverride(std::string device);

	/// Looks for the drives' nodes in this folder instead of /dev (the PC tests). Before Start().
	void SetDeviceFolder(std::string folder);

	/// Starts the watching thread (which first looks for the drive, once). Returns at once.
	void Start();
	void Stop();

	/// true: the emulator is about to open `device` (empty: every drive); returns when the watcher
	/// is not reading it, and it stays away until false. false: the drives are looked at again at
	/// once (the disc may have been changed during the game).
	void SetInUse(bool in_use, const std::string& device = {});

	/// Every disc drive there is now, the console's own first: USB DVD drives come and go with
	/// their plug.
	std::vector<State> Drives();

	/// One drive: the first with a PlayStation 2 game in it, else the first with any disc, else
	/// the first; with no drive, a State whose note says so.
	State Current();

	/// The drive of this device node (a State with drive_found false when it is not there).
	State Drive(const std::string& device);

	/// Makes an image file of the disc in `device`'s drive (empty: Current()'s) (a backup of your own disc, to play from without
	/// the drive). progress(done, total) in bytes; false from it stops. Blocks: call it on a thread
	/// of its own. False with `error` a sentence when it could not be made.
	bool SaveImage(const std::string& device, const std::string& path, const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error);
	/// The same in one of the formats PCSX2 plays from (ISO, CHD, CSO): see ps5_disc_image.h.
	bool SaveImageAs(const std::string& device, const std::string& path, PS5DiscImage::Format format,
		const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error);

	/// Writes an image of the disc in `device`'s drive through `write` (a network share's file
	/// layer): it gets the image's size and a source to ask for its bytes at any offset (which
	/// returns -1 with errno when the disc cannot be read or progress said stop). Blocks.
	using ImageSource = std::function<std::int64_t(void* buffer, std::uint32_t length, std::uint64_t offset)>;
	using ImageWriter = std::function<bool(std::uint64_t size, const ImageSource& source, std::string* error)>;
	bool SaveImageWith(const std::string& device, const ImageWriter& write,
		const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error);

	/// Unlocks the drive's tray and opens it (the console locks a USB drive's tray at times, and its
	/// button then does nothing). Every drive's tray is also unlocked once when the watcher first
	/// sees it. Refused while a game plays from that drive. False with `error` a sentence.
	bool Eject(const std::string& device, std::string* error);

	/// Goes up by one each time Current() would answer differently.
	unsigned Generation();

	// ---- the parsing (no console in it)

	enum class Kind
	{
		Unreadable, ///< sector 16 could not be read
		Unknown, ///< readable, but none of the below
		PS2,
		PS1, ///< SYSTEM.CNF with "BOOT =", or a PSX.EXE
		DVDVideo, ///< a VIDEO_TS folder
	};

	/// What a SYSTEM.CNF says: PS2 ("BOOT2 = cdrom0:\SLUS_123.45;1"), PS1 ("BOOT = cdrom:\..."),
	/// or Unknown. The last BOOT2/BOOT line decides, as in the emulator's own reading.
	struct BootLine
	{
		Kind kind = Kind::Unknown;
		std::string path; ///< "cdrom0:\SLUS_123.45;1"
		std::string serial; ///< "SLUS-12345"
	};
	BootLine ParseSystemCnf(std::string_view text);

	/// "cdrom0:\SLUS_123.45;1" -> "SLUS-12345"; empty when the name is not of that shape (the
	/// emulator's own rule: four characters, '_' or '-', three, '.', two).
	std::string SerialFromBootPath(std::string_view path);

	/// Reads `count` whole 2048-byte sectors from sector `lsn` on; false when it cannot.
	using SectorReader = std::function<bool(std::uint32_t lsn, std::uint32_t count, std::uint8_t* out)>;

	struct Inspection
	{
		Kind kind = Kind::Unreadable;
		bool iso9660 = false; ///< a primary volume descriptor was found
		std::string volume_label;
		std::string boot_path;
		std::string serial;
		std::uint32_t volume_sectors = 0; ///< the first volume's size, from its descriptor
		std::uint32_t layer1_start = 0; ///< first sector of the second layer; 0: one layer
		std::uint64_t sectors = 0; ///< the media size given, else the volumes' sizes added up
		int layers = 0;
		std::uint32_t system_cnf_lsn = 0;
	};

	/// Reads what the watcher needs from a disc. media_sectors: the size the drive reports, 0 when
	/// it does not say. Every number read from the disc is checked before it is used; a disc that
	/// lies gets Kind::Unknown, not a wild read.
	Inspection Inspect(const SectorReader& read, std::uint64_t media_sectors);

	/// The State a disc of this inspection gets (present, ps2, serial, note...), without `device`
	/// and `drive_found`.
	State Describe(const Inspection& inspection);

	/// One look at a device node or an image file: open, read, close. `open_errno` gets the errno
	/// of a failed open (0 otherwise).
	State Probe(const std::string& device, int* open_errno = nullptr);
} // namespace PS5Disc
