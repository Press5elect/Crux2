// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the disc drive's watcher on a PC: image files stand in for the drive. The images are
// made here, by hand (a volume descriptor, a root folder, one file): small, and with invented
// serials.
//   disc_test <work folder>
#include "ps5_disc.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" void ps5px_log(const char* format, ...)
{
	if (std::getenv("DISC_TEST_LOG") == nullptr)
		return;
	va_list ap;
	va_start(ap, format);
	std::vfprintf(stderr, format, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}

static int g_failed = 0, g_checks = 0;
#define EXPECT(c, what) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAILED: %s (line %d)\n", what, __LINE__); } } while (0)

namespace
{
constexpr size_t SECTOR = 2048;

void both32(std::uint8_t* p, std::uint32_t v)
{
	for (int i = 0; i < 4; i++)
	{
		p[i] = static_cast<std::uint8_t>(v >> (8 * i));
		p[7 - i] = static_cast<std::uint8_t>(v >> (8 * i));
	}
}

// One directory record at p; returns its length.
size_t record(std::uint8_t* p, const std::string& name, std::uint32_t lsn, std::uint32_t size, bool folder)
{
	const size_t length = (33 + name.size() + 1) & ~static_cast<size_t>(1);
	std::memset(p, 0, length);
	p[0] = static_cast<std::uint8_t>(length);
	both32(p + 2, lsn);
	both32(p + 10, size);
	p[25] = folder ? 2 : 0;
	p[28] = 1;
	p[31] = 1;
	p[32] = static_cast<std::uint8_t>(name.size());
	std::memcpy(p + 33, name.data(), name.size());
	return length;
}

struct File
{
	std::string name; // "SYSTEM.CNF;1", or a folder's name
	std::string content;
	bool folder = false;
};

// An ISO 9660 image of `sectors` sectors with these files in its root folder.
std::vector<std::uint8_t> image(const std::vector<File>& files, std::uint32_t sectors, const char* label = "INVENTED")
{
	std::vector<std::uint8_t> out(static_cast<size_t>(sectors) * SECTOR, 0);
	std::uint8_t* const pvd = out.data() + 16 * SECTOR;
	pvd[0] = 1;
	std::memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	std::memset(pvd + 8, ' ', 32);
	std::memset(pvd + 40, ' ', 32);
	std::memcpy(pvd + 40, label, std::strlen(label));
	both32(pvd + 80, sectors);
	pvd[128] = 0;
	pvd[129] = 8; // logical block size 2048, little endian
	pvd[130] = 8;
	pvd[131] = 0;
	record(pvd + 156, std::string(1, '\0'), 20, SECTOR, true);
	std::uint8_t* const term = out.data() + 17 * SECTOR;
	term[0] = 255;
	std::memcpy(term + 1, "CD001", 5);
	std::uint8_t* dir = out.data() + 20 * SECTOR;
	dir += record(dir, std::string(1, '\0'), 20, SECTOR, true);
	dir += record(dir, std::string(1, '\1'), 20, SECTOR, true);
	std::uint32_t next = 21;
	for (const File& file : files)
	{
		dir += record(dir, file.name, next, file.folder ? SECTOR : static_cast<std::uint32_t>(file.content.size()), file.folder);
		std::memcpy(out.data() + next * SECTOR, file.content.data(), std::min(file.content.size(), SECTOR));
		next++;
	}
	return out;
}

std::string save(const std::string& path, const std::vector<std::uint8_t>& bytes)
{
	std::FILE* f = std::fopen(path.c_str(), "wb");
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	return path;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	const std::string work = argv[1];
	using namespace PS5Disc;

	// ---- the parsing
	EXPECT(SerialFromBootPath("cdrom0:\\SLUS_297.77;1") == "SLUS-29777", "a boot path's serial");
	EXPECT(SerialFromBootPath("cdrom0:\\DIR\\SLES_590.03;1") == "SLES-59003" || SerialFromBootPath("cdrom0:\\DIR\\SLES_590.03;1").empty(), "a boot file in a folder");
	EXPECT(SerialFromBootPath("cdrom0:\\GAME.ELF;1").empty(), "a boot file that is no serial");
	BootLine line = ParseSystemCnf("BOOT2 = cdrom0:\\SLUS_297.77;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n");
	EXPECT(line.kind == Kind::PS2 && line.serial == "SLUS-29777", "a PlayStation 2 game's SYSTEM.CNF");
	line = ParseSystemCnf("BOOT=cdrom:\\SLUS_010.01;1\nTCB=4\n");
	EXPECT(line.kind == Kind::PS1, "a PlayStation 1 game's");
	line = ParseSystemCnf("nothing of use\n");
	EXPECT(line.kind == Kind::Unknown, "one that says nothing");

	// ---- image files as the drive
	const std::string ps2 = save(work + "/ps2.iso", image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.77;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n"}, {"SLUS_297.77;1", "x"}}, 400));
	int open_errno = -1;
	State state = Probe(ps2, &open_errno);
	EXPECT(open_errno == 0 && state.present && state.ps2 && state.serial == "SLUS-29777" && state.layers == 1 && state.sectors == 400 && state.volume_label == "INVENTED",
		"a PlayStation 2 disc is known for what it is");
	state = Probe(save(work + "/ps1.iso", image({{"SYSTEM.CNF;1", "BOOT = cdrom:\\SLUS_010.01;1\r\n"}}, 300)));
	EXPECT(state.present && !state.ps2 && !state.note.empty(), "a PlayStation 1 disc is there and is not played");
	state = Probe(save(work + "/film.iso", image({{"VIDEO_TS", "", true}}, 300)));
	EXPECT(state.present && !state.ps2 && !state.note.empty(), "a film is there and is not played");
	std::printf("  a film's note: %s\n", state.note.c_str());
	state = Probe(save(work + "/data.iso", image({{"README.TXT;1", "hello"}}, 300)));
	EXPECT(state.present && !state.ps2 && !state.note.empty(), "a disc of files is there and is not played");
	// A dual-layer-shaped image: the second layer's volume descriptor is where the first volume
	// says it ends (as the emulator's own reader of image files expects it).
	{
		std::vector<std::uint8_t> two = image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLES_590.03;1\r\n"}}, 600);
		const std::vector<std::uint8_t> second = image({}, 500);
		two.insert(two.end(), second.begin() + 16 * SECTOR, second.end());
		state = Probe(save(work + "/two-layers.iso", two));
		EXPECT(state.present && state.ps2 && state.serial == "SLES-59003" && state.sectors == 1084 && state.layers == 2, "a disc of two layers is read as one");
		std::printf("  a disc of two volumes: %d layer(s), %llu sectors\n", state.layers, static_cast<unsigned long long>(state.sectors));
	}
	// Garbage, a disc cut short, an empty drive.
	{
		std::vector<std::uint8_t> garbage(400 * SECTOR);
		for (size_t i = 0; i < garbage.size(); i++)
			garbage[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 11);
		state = Probe(save(work + "/garbage.iso", garbage));
		EXPECT(!state.ps2, "garbage is no game");
		std::vector<std::uint8_t> lying = image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.77;1\r\n"}}, 400);
		both32(lying.data() + 20 * SECTOR + 68 + 2, 0x7fffff00u); // SYSTEM.CNF's place: far past the end
		state = Probe(save(work + "/lying.iso", lying));
		EXPECT(!state.ps2, "a folder that points past the disc's end is not followed");
		std::vector<std::uint8_t> cut = image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.77;1\r\n"}}, 400);
		cut.resize(17 * SECTOR + 100);
		state = Probe(save(work + "/cut.iso", cut));
		EXPECT(!state.ps2, "a disc that ends after its first descriptor is no game");
		state = Probe(save(work + "/empty.iso", {}));
		EXPECT(!state.present, "an empty drive has no disc");
		state = Probe(work + "/no-such-node", &open_errno);
		EXPECT(!state.present && open_errno != 0, "a node that is not there: no disc, and the reason");
	}

	// ---- the watcher: a disc comes, the emulator takes the drive, the disc goes
	const std::string drive = work + "/drive";
	save(drive, {});
	SetDeviceOverride(drive);
	Start();
	const auto wait_for = [&](bool present) {
		for (int i = 0; i < 400 && !(Current().drive_found && Current().present == present); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return Current().drive_found && Current().present == present;
	};
	EXPECT(wait_for(false) && Current().drive_found, "the watcher finds the drive, empty");
	const unsigned before = Generation();
	std::rename(ps2.c_str(), drive.c_str());
	EXPECT(wait_for(true) && Current().serial == "SLUS-29777" && Generation() != before && Current().device == drive, "a disc put in is seen");
	SetInUse(true);
	save(drive, {});
	std::this_thread::sleep_for(std::chrono::milliseconds(2600));
	EXPECT(Current().present, "while the emulator has the drive the watcher leaves it alone");
	SetInUse(false);
	EXPECT(wait_for(false), "and looks again at once when it is given back");
	Stop();
	Stop();

	// ---- several drives: the console's own and USB ones, which come and go with their plug
	{
		const std::string dev = work + "/dev";
		mkdir(dev.c_str(), 0777);
		for (const char* name : {"cd0", "cd1", "cd2", "cdev", "cd0a", "da0"})
			unlink((dev + "/" + name).c_str());
		save(dev + "/cd0", {});
		save(dev + "/cdev", {}); // not a drive: no digits after "cd"
		save(dev + "/cd0a", {}); // not a drive: a partition's name
		save(dev + "/da0", {});  // a USB stick
		SetDeviceOverride(std::string());
		SetDeviceFolder(dev);
		Start();
		const auto until = [](auto test) {
			for (int i = 0; i < 400 && !test(); i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			return test();
		};
		EXPECT(until([] { return Drives().size() == 1; }) && Drives()[0].device == dev + "/cd0" && !Drives()[0].present,
			"one drive, empty; names that only begin like a drive's are not drives");
		EXPECT(!Current().present && Current().drive_found, "Current() is the empty drive");

		// A USB DVD drive plugged in, with a game in it.
		save(dev + "/cd1", image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.78;1\r\nVER = 1.00\r\n"}}, 400));
		EXPECT(until([] { return Drives().size() == 2 && Drives()[1].ps2; }), "a USB drive plugged in later is found, with its game");
		EXPECT(Drives()[0].device == dev + "/cd0" && Drives()[1].device == dev + "/cd1", "the console's drive stays first");
		EXPECT(Current().device == dev + "/cd1" && Current().serial == "SLUS-29778", "Current() is the drive with a game in it");
		EXPECT(Drive(dev + "/cd1").ps2 && !Drive(dev + "/no-such").drive_found, "a drive by its node");

		// The emulator takes the USB drive: the console's drive is still watched.
		SetInUse(true, dev + "/cd1");
		{
			std::string why;
			EXPECT(!Eject(dev + "/cd1", &why) && why.find("close it first") != std::string::npos, "no eject of the drive a game plays from");
			why.clear();
			// (On the PC an image file has no tray: the drive's own question fails, and says so.)
			EXPECT(!Eject(dev + "/cd0", &why) && why.find("did not open") != std::string::npos, "an eject that fails says why");
		}
		save(dev + "/cd1", {});
		save(dev + "/cd0", image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.79;1\r\n"}}, 400));
		EXPECT(until([] { return Drives().size() == 2 && Drives()[0].ps2; }), "the other drive is looked at while one is lent");
		EXPECT(Drives()[1].present, "the lent drive is left alone");
		SetInUse(false);
		EXPECT(until([] { return Drives().size() == 2 && !Drives()[1].present; }), "and looked at again when it is given back");

		// An image of the game in the second drive, chosen by its node.
		save(dev + "/cd1", image({{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_297.78;1\r\n"}}, 400));
		EXPECT(until([] { return Drives().size() == 2 && Drives()[1].ps2; }), "a disc in the USB drive again");
		std::string error;
		const std::string copy = work + "/copy-of-cd1.iso";
		EXPECT(SaveImage(dev + "/cd1", copy, nullptr, &error) && Probe(copy).serial == "SLUS-29778", "an image of the second drive's disc");
		{
			// A compressed one: the CHD's header, and nothing left beside it.
			const std::string chd = work + "/copy.chd";
			EXPECT(SaveImageAs(dev + "/cd1", chd, PS5DiscImage::Format::ChdFast, nullptr, &error), "a CHD of the second drive's disc");
			FILE* f = std::fopen(chd.c_str(), "rb");
			char magic[8] = {};
			EXPECT(f != nullptr && std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "MComprHD", 8) == 0, "the CHD starts as one");
			if (f)
				std::fclose(f);
			EXPECT(access((chd + ".part").c_str(), F_OK) != 0, "no .part left");
		}
		// The same disc through a writer of someone else's (a network share), piece by piece.
		std::vector<std::uint8_t> remote;
		const bool streamed = SaveImageWith(dev + "/cd1",
			[&](std::uint64_t size, const ImageSource& source, std::string*) {
				remote.resize(size);
				for (std::uint64_t at = 0; at < size; at += 300 * 1024)
				{
					const std::uint32_t piece = static_cast<std::uint32_t>(std::min<std::uint64_t>(size - at, 300 * 1024));
					if (source(remote.data() + at, piece, at) != piece)
						return false;
				}
				return true;
			},
			nullptr, &error);
		std::vector<std::uint8_t> local;
		if (FILE* const f = std::fopen(copy.c_str(), "rb"))
		{
			local.resize(remote.size());
			local.resize(std::fread(local.data(), 1, local.size(), f));
			std::fclose(f);
		}
		EXPECT(streamed && !remote.empty() && remote == local, "an image through a writer of someone else's is the same disc");
		const bool stopped = SaveImageWith(dev + "/cd1",
			[&](std::uint64_t, const ImageSource& source, std::string*) {
				std::vector<std::uint8_t> piece(4096);
				return source(piece.data(), 4096, 0) == 4096 && source(piece.data(), 4096, 4096) == 4096;
			},
			[](std::uint64_t done, std::uint64_t) { return done < 4096; }, &error);
		EXPECT(!stopped && error == "Stopped.", "a stop from the progress is a stop");

		// The USB drive pulled out.
		unlink((dev + "/cd1").c_str());
		EXPECT(until([] { return Drives().size() == 1; }) && Current().serial == "SLUS-29779", "a drive pulled out goes; the other stays");
		Stop();
	}

	std::printf("disc: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
