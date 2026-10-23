// Crux2 - the PC test of the memory card file system and the save files (ps5/frontend/ps5_mc.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Two things in one program:
//
//   mc_test self <work folder>     the checks that need nothing else: a card made, saves written,
//                                  read, removed, the card filled to its end, every save format
//                                  read back from bytes made here, and damaged input of every kind.
//   mc_test <command> ...          the card's operations one at a time, for cross_check.py, which
//                                  holds each of them against mymc+ (another program for the same
//                                  cards, written by other people).
//
// Built with the address and undefined-behaviour sanitizers (run-cards-tests.sh).

#include "ps5_mc.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

using namespace PS5Mc;

static int s_checks = 0, s_failed = 0;
#define CHECK(what, ...) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); \
			__VA_OPT__(std::printf("       " __VA_ARGS__); std::printf("\n");) \
		} \
	} while (0)

static bool ReadAll(const std::string& path, Bytes* out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}
static bool WriteAll(const std::string& path, const Bytes& bytes)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}
static Stamp StampOf(const char* hex)
{
	Stamp s;
	for (int i = 0; i < 8 && hex[i * 2] && hex[i * 2 + 1]; i++)
		s.raw[i] = static_cast<uint8_t>(std::strtoul(std::string(hex + i * 2, 2).c_str(), nullptr, 16));
	return s;
}
static uint32_t Hash(const Bytes& bytes)
{
	uint32_t h = 2166136261u;
	for (const uint8_t b : bytes)
		h = (h ^ b) * 16777619u;
	return h;
}

// ------------------------------------------------------------------------------ the commands

static int Usage()
{
	std::fprintf(stderr, "mc_test self <work> | format <card> ecc|plain <stamp> | ls <card> | check <card> |\n"
						 "        import <card> <file> <stamp> | export <card> <save> <out.psu> | remove <card> <save> <stamp> |\n"
						 "        dump <file> <stamp> | title <icon.sys>\n");
	return 2;
}

static bool OpenCard(const std::string& path, Card* card)
{
	Bytes image;
	std::string error;
	if (!ReadAll(path, &image))
	{
		std::printf("error\tthe card's file does not open\n");
		return false;
	}
	if (!card->Open(std::move(image), &error))
	{
		std::printf("error\t%s\n", error.c_str());
		return false;
	}
	return true;
}

static void PrintSave(const Save& save)
{
	std::printf("save\t%s\t%04x\t%zu\n", save.name.c_str(), save.mode, save.files.size());
	for (const File& file : save.files)
		std::printf("file\t%s\t%04x\t%zu\t%08x\n", file.name.c_str(), file.mode, file.data.size(), Hash(file.data));
}

static int Command(int argc, char** argv)
{
	const std::string command = argv[1];
	if (command == "format" && argc == 5)
	{
		// ("ecc" or "plain", and after it the megabytes when they are not 8: "ecc32".)
		const std::string kind = argv[3];
		const bool ecc = kind.compare(0, 3, "ecc") == 0;
		const size_t megabytes = std::max(8, std::atoi(kind.c_str() + (ecc ? 3 : 5)));
		Bytes image((ecc ? Card::SIZE_8MB_ECC : Card::SIZE_8MB_PLAIN) * (megabytes / 8), 0xff);
		if (!Card::Unformatted(image) || !Card::Format(&image, StampOf(argv[4])))
			return 1;
		return WriteAll(argv[2], image) ? 0 : 1;
	}
	if (command == "ls" && argc == 3)
	{
		Card card;
		if (!OpenCard(argv[2], &card))
			return 1;
		for (const Card::Listed& one : card.List())
			std::printf("%s\t%s\t%u\t%d\t%s\t%d\n", one.name.c_str(), one.title.c_str(), one.bytes, one.files, one.modified.Text().c_str(), one.ps1 ? 1 : 0);
		std::printf("free\t%u\ttotal\t%u\n", card.FreeBytes(), card.TotalBytes());
		return 0;
	}
	if (command == "check" && argc == 3)
	{
		Card card;
		if (!OpenCard(argv[2], &card))
			return 1;
		std::printf("sound\tecc-mismatches\t%zu\n", card.EccMismatches());
		return card.EccMismatches() == 0 ? 0 : 1;
	}
	if (command == "import" && argc == 5)
	{
		Card card;
		Bytes file;
		Save save;
		std::string error;
		const Stamp now = StampOf(argv[4]);
		if (!OpenCard(argv[2], &card))
			return 1;
		if (!ReadAll(argv[3], &file) || !ReadSaveFile(file, &save, now, &error))
		{
			std::printf("error\t%s\n", error.c_str());
			return 1;
		}
		std::printf("format\t%s\n", FormatName(Identify(file)));
		const Bytes before = card.Image();
		if (!card.Write(save, now, &error))
		{
			std::printf("error\t%s\n", error.c_str());
			return (card.Image() == before && !card.Changed()) ? 3 : 4;
		}
		return WriteAll(argv[2], card.Image()) ? 0 : 1;
	}
	if (command == "export" && argc == 5)
	{
		Card card;
		Save save;
		std::string error;
		if (!OpenCard(argv[2], &card))
			return 1;
		if (!card.Read(argv[3], &save, &error))
		{
			std::printf("error\t%s\n", error.c_str());
			return 1;
		}
		PrintSave(save);
		return WriteAll(argv[4], WritePSU(save)) ? 0 : 1;
	}
	if (command == "remove" && argc == 5)
	{
		Card card;
		std::string error;
		if (!OpenCard(argv[2], &card))
			return 1;
		if (!card.Remove(argv[3], StampOf(argv[4]), &error))
		{
			std::printf("error\t%s\n", error.c_str());
			return 3;
		}
		return WriteAll(argv[2], card.Image()) ? 0 : 1;
	}
	if (command == "dump" && argc == 4)
	{
		Bytes file;
		Save save;
		std::string error;
		if (!ReadAll(argv[2], &file) || !ReadSaveFile(file, &save, StampOf(argv[3]), &error))
		{
			std::printf("error\t%s\n", error.c_str());
			return 1;
		}
		std::printf("format\t%s\n", FormatName(Identify(file)));
		PrintSave(save);
		return 0;
	}
	if (command == "title" && argc == 3)
	{
		Bytes file;
		if (!ReadAll(argv[2], &file))
			return 1;
		std::printf("%s\n", TitleOf(file).c_str());
		return 0;
	}
	return Usage();
}

// ------------------------------------------------------------------------ the checks by itself

static const Stamp NOW = Stamp::Of(2026, 10, 7, 12, 34, 56);

/// An icon.sys with a title in the wide Latin letters of Shift-JIS, the second line after `first`
/// letters.
static Bytes IconSys(const std::string& title, size_t first)
{
	Bytes out(964, 0);
	std::memcpy(out.data(), "PS2D", 4);
	detail::Put16(out.data() + 6, static_cast<uint16_t>(first * 2));
	size_t at = 0xc0;
	for (const char c : title)
	{
		unsigned code = 0x8140;
		if (c >= 'A' && c <= 'Z')
			code = 0x8260 + static_cast<unsigned>(c - 'A');
		else if (c >= 'a' && c <= 'z')
			code = 0x8281 + static_cast<unsigned>(c - 'a');
		else if (c >= '0' && c <= '9')
			code = 0x824f + static_cast<unsigned>(c - '0');
		else if (c == '-')
			code = 0x817c;
		else if (c == ':')
			code = 0x8146;
		out[at++] = static_cast<uint8_t>(code >> 8);
		out[at++] = static_cast<uint8_t>(code);
	}
	return out;
}

static Save MakeSave(std::mt19937& rng, const std::string& name, const std::string& title, const std::vector<size_t>& sizes)
{
	Save save;
	save.name = name;
	save.mode = MODE_RWX | MODE_DIR | MODE_0400 | MODE_EXISTS;
	save.created = Stamp::Of(2004, 3, 2, 1, 2, 3);
	save.modified = Stamp::Of(2005, 6, 7, 8, 9, 10);
	File icon;
	icon.name = "icon.sys";
	icon.mode = MODE_RWX | MODE_FILE | MODE_0400 | MODE_EXISTS;
	icon.created = save.created;
	icon.modified = save.modified;
	icon.data = IconSys(title, std::min(title.find(' '), title.size()));
	save.files.push_back(icon);
	int n = 0;
	for (const size_t size : sizes)
	{
		File file;
		file.name = "DATA" + std::to_string(n++) + ".BIN";
		file.mode = MODE_READ | MODE_WRITE | MODE_FILE | MODE_0400 | MODE_EXISTS;
		file.created = Stamp::Of(2004, 3, 2, 1, 2, 4 + n);
		file.modified = Stamp::Of(2005, 6, 7, 8, 9, 11 + n);
		file.data.resize(size);
		// (Half of it noise, half of it something that packs.)
		for (size_t i = 0; i < size; i++)
			file.data[i] = (i < size / 2) ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>(i / 7);
		save.files.push_back(std::move(file));
	}
	return save;
}

static bool SameSave(const Save& a, const Save& b, bool times)
{
	if (a.name != b.name || a.files.size() != b.files.size())
		return false;
	if (times && (a.mode != b.mode || std::memcmp(a.created.raw, b.created.raw, 8) != 0 || std::memcmp(a.modified.raw, b.modified.raw, 8) != 0))
		return false;
	for (size_t i = 0; i < a.files.size(); i++)
	{
		const File &x = a.files[i], &y = b.files[i];
		if (x.name != y.name || x.data != y.data)
			return false;
		if (times && (x.mode != y.mode || std::memcmp(x.created.raw, y.created.raw, 8) != 0 || std::memcmp(x.modified.raw, y.modified.raw, 8) != 0))
			return false;
	}
	return true;
}

// The other save files, made here as their readers expect them (cross_check.py holds the same
// bytes against mymc+'s readers, and mymc+'s writers against the readers here).
static Bytes MakeSps(const Save& save)
{
	Bytes out;
	const auto put = [&](const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
	const auto u32 = [&](uint32_t v) {
		uint8_t b[4];
		detail::Put32(b, v);
		put(b, 4);
	};
	const auto text = [&](const std::string& s) {
		u32(static_cast<uint32_t>(s.size()));
		put(s.data(), s.size());
	};
	const auto entry = [&](const std::string& name, uint32_t length, uint16_t mode, const Stamp& created, const Stamp& modified) {
		uint8_t h[98] = {};
		detail::Put16(h, 98);
		std::memcpy(h + 2, name.data(), std::min<size_t>(name.size(), 64));
		detail::Put32(h + 66, length);
		h[78] = static_cast<uint8_t>(mode >> 8);
		h[79] = static_cast<uint8_t>(mode);
		std::memcpy(h + 82, created.raw, 8);
		std::memcpy(h + 90, modified.raw, 8);
		put(h, 98);
	};
	put("\x0d\0\0\0SharkPortSave", 17);
	u32(0);
	text(save.name);
	text("07/10/2026");
	text("a comment");
	u32(0);
	entry(save.name, static_cast<uint32_t>(save.files.size() + 2), save.mode, save.created, save.modified);
	for (const File& file : save.files)
	{
		entry(file.name, static_cast<uint32_t>(file.data.size()), file.mode, file.created, file.modified);
		put(file.data.data(), file.data.size());
	}
	u32(0x12345678); // the sum at the end, which nobody reads
	return out;
}

static Bytes MakeCbs(const Save& save, bool whole_length)
{
	Bytes plain;
	for (const File& file : save.files)
	{
		uint8_t h[64] = {};
		std::memcpy(h, file.created.raw, 8);
		std::memcpy(h + 8, file.modified.raw, 8);
		detail::Put32(h + 16, static_cast<uint32_t>(file.data.size()));
		detail::Put16(h + 20, file.mode);
		std::memcpy(h + 32, file.name.data(), std::min<size_t>(file.name.size(), 32));
		plain.insert(plain.end(), h, h + 64);
		plain.insert(plain.end(), file.data.begin(), file.data.end());
	}
	Bytes body(compressBound(static_cast<uLong>(plain.size())));
	uLongf packed = static_cast<uLongf>(body.size());
	compress2(body.data(), &packed, plain.data(), static_cast<uLong>(plain.size()), 9);
	body.resize(packed);
	detail::CbsCrypt(body.data(), body.size());
	const uint32_t header_length = 92 + 32;
	Bytes out(header_length, 0);
	std::memcpy(out.data(), "CFU\0", 4);
	detail::Put32(out.data() + 4, 0x1f40);
	detail::Put32(out.data() + 8, header_length);
	detail::Put32(out.data() + 12, static_cast<uint32_t>(plain.size()));
	detail::Put32(out.data() + 16, static_cast<uint32_t>(body.size()) + (whole_length ? header_length : 0));
	std::memcpy(out.data() + 20, save.name.data(), std::min<size_t>(save.name.size(), 32));
	std::memcpy(out.data() + 52, save.created.raw, 8);
	std::memcpy(out.data() + 60, save.modified.raw, 8);
	detail::Put32(out.data() + 76, save.mode);
	std::memcpy(out.data() + 92, "A title", 7);
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

static Bytes MakePsv(const Save& save)
{
	Bytes out(0x40 + 40, 0);
	std::memcpy(out.data(), "\0VSP", 4);
	detail::Put32(out.data() + 0x38, 0x2c);
	detail::Put32(out.data() + 0x3c, 2);
	detail::Put32(out.data() + 0x40 + 36, static_cast<uint32_t>(save.files.size()));
	const auto info = [&](const std::string& name, uint32_t size, uint16_t mode, const Stamp& created, const Stamp& modified) {
		uint8_t h[56] = {};
		std::memcpy(h, created.raw, 8);
		std::memcpy(h + 8, modified.raw, 8);
		detail::Put32(h + 16, size);
		detail::Put32(h + 20, mode);
		std::memcpy(h + 24, name.data(), std::min<size_t>(name.size(), 32));
		out.insert(out.end(), h, h + 56);
	};
	info(save.name, static_cast<uint32_t>(save.files.size() + 2), save.mode, save.created, save.modified);
	uint32_t offset = static_cast<uint32_t>(out.size() + save.files.size() * 60);
	for (const File& file : save.files)
	{
		info(file.name, static_cast<uint32_t>(file.data.size()), file.mode, file.created, file.modified);
		uint8_t b[4];
		detail::Put32(b, offset);
		out.insert(out.end(), b, b + 4);
		offset += static_cast<uint32_t>(file.data.size());
	}
	for (const File& file : save.files)
		out.insert(out.end(), file.data.begin(), file.data.end());
	return out;
}

static void SelfCard(bool ecc)
{
	std::mt19937 rng(ecc ? 11 : 12);
	Bytes image(ecc ? Card::SIZE_8MB_ECC : Card::SIZE_8MB_PLAIN, 0xff);
	std::string error;
	Card card;
	CHECK(Card::Unformatted(image));
	CHECK(!card.Open(image, &error) && error == "The card was never formatted.", "%s", error.c_str());
	CHECK(Card::Format(&image, NOW));
	CHECK(!Card::Unformatted(image));
	CHECK(card.Open(image, &error), "%s", error.c_str());
	CHECK(card.List().empty());
	CHECK(card.EccMismatches() == 0);
	const uint32_t total = card.TotalBytes(), empty_free = card.FreeBytes();
	CHECK(total == 8135u * 1024u, "%u", total);
	CHECK(empty_free == total - 1024u, "%u", empty_free);

	// Saves of every awkward size, written, listed and read back the same.
	const std::vector<std::vector<size_t>> shapes = {{}, {0}, {1}, {1023}, {1024}, {1025}, {2048, 0, 5}, {100000, 3, 70000}, {4096, 4096, 4096, 4096}};
	std::vector<Save> saves;
	for (size_t i = 0; i < shapes.size(); i++)
	{
		const Save save = MakeSave(rng, "BESLES-5000" + std::to_string(i) + "SAVE", "Save Number " + std::to_string(i), shapes[i]);
		const uint32_t before = card.FreeBytes();
		CHECK(card.Write(save, NOW, &error), "%s", error.c_str());
		CHECK(card.Changed());
		CHECK(card.Check().empty(), "%s", card.Check().c_str());
		CHECK(card.EccMismatches() == 0, "%zu", card.EccMismatches());
		const uint32_t used = before - card.FreeBytes();
		CHECK(used <= card.ClustersFor(save) * 1024u && used + 1024u >= card.ClustersFor(save) * 1024u, "%u against %u", used, card.ClustersFor(save) * 1024u);
		saves.push_back(save);
	}
	const std::vector<Card::Listed> listed = card.List();
	CHECK(listed.size() == saves.size());
	for (size_t i = 0; i < listed.size() && i < saves.size(); i++)
	{
		CHECK(listed[i].name == saves[i].name);
		CHECK(listed[i].title == "Save Number " + std::to_string(i), "%s", listed[i].title.c_str());
		CHECK(listed[i].files == static_cast<int>(saves[i].files.size()));
		CHECK(listed[i].modified.Text() == "2005-06-07 08:09", "%s", listed[i].modified.Text().c_str());
		Save back;
		CHECK(card.Read(saves[i].name, &back, &error), "%s", error.c_str());
		CHECK(SameSave(saves[i], back, true));
		// (And through a .psu and back.)
		Save again;
		const Bytes psu = WritePSU(back);
		CHECK(Identify(psu) == Format::PSU);
		CHECK(ReadSaveFile(psu, &again, NOW, &error), "%s", error.c_str());
		CHECK(SameSave(saves[i], again, true));
	}
	// A card that was written and read as bytes again is the same card.
	{
		Card again;
		CHECK(again.Open(card.Image(), &error), "%s", error.c_str());
		CHECK(again.List().size() == saves.size());
		CHECK(again.FreeBytes() == card.FreeBytes());
	}

	// What must be refused, with the card as it was.
	{
		const Bytes before = card.Image();
		CHECK(!card.Write(saves[0], NOW, &error) && error == "A save of that name is on the card already.", "%s", error.c_str());
		Save bad = MakeSave(rng, "A/B", "x", {1});
		CHECK(!card.Write(bad, NOW, &error));
		bad = MakeSave(rng, std::string(33, 'A'), "x", {1});
		CHECK(!card.Write(bad, NOW, &error));
		bad = MakeSave(rng, "TWICE", "x", {1, 1});
		bad.files[2].name = bad.files[1].name;
		CHECK(!card.Write(bad, NOW, &error));
		bad = MakeSave(rng, "EMPTY", "x", {});
		bad.files.clear();
		CHECK(!card.Write(bad, NOW, &error));
		bad = MakeSave(rng, "TOOLARGE", "x", {9u * 1024u * 1024u});
		CHECK(!card.Write(bad, NOW, &error) && error == "There is not enough room on the card.", "%s", error.c_str());
		CHECK(!card.Remove("NOTTHERE", NOW, &error) && error == "That save is not on the card.", "%s", error.c_str());
		Save none;
		CHECK(!card.Read("NOTTHERE", &none, &error));
		CHECK(card.Image() == before);
	}

	// Taken off: the room comes back, the others stay, the place is used again.
	{
		const uint32_t before = card.FreeBytes();
		CHECK(card.Remove(saves[7].name, NOW, &error), "%s", error.c_str());
		CHECK(card.Check().empty());
		CHECK(card.EccMismatches() == 0);
		CHECK(card.FreeBytes() > before + 170000u, "%u after %u", card.FreeBytes(), before);
		CHECK(card.List().size() == saves.size() - 1);
		for (size_t i = 0; i < saves.size(); i++)
		{
			Save back;
			CHECK(card.Read(saves[i].name, &back, &error) == (i != 7));
			if (i != 7)
				CHECK(SameSave(saves[i], back, true));
		}
		CHECK(card.Write(saves[7], NOW, &error), "%s", error.c_str());
		CHECK(card.FreeBytes() == before, "%u against %u", card.FreeBytes(), before);
		const std::vector<Card::Listed> now = card.List();
		CHECK(now.size() == saves.size() && now[7].name == saves[7].name);
	}

	// Filled to its end, then one too many; emptied, and as free as it began.
	{
		std::vector<std::string> names;
		for (int i = 0; i < 200; i++)
		{
			const Save save = MakeSave(rng, "FILL" + std::to_string(i), "Fill", {60000 + static_cast<size_t>(rng() % 90000)});
			const Bytes before = card.Image();
			if (!card.Write(save, NOW, &error))
			{
				CHECK(error == "There is not enough room on the card.", "%s", error.c_str());
				CHECK(card.Image() == before);
				break;
			}
			names.push_back(save.name);
			Save back;
			CHECK(card.Read(save.name, &back, &error) && SameSave(save, back, true));
		}
		CHECK(names.size() > 40 && names.size() < 200, "%zu", names.size());
		CHECK(card.Check().empty());
		CHECK(card.FreeBytes() < 160000u, "%u", card.FreeBytes());
		// (The first saves are still what they were.)
		for (size_t i = 0; i < saves.size(); i++)
		{
			Save back;
			CHECK(card.Read(saves[i].name, &back, &error) && SameSave(saves[i], back, true));
		}
		// (Out in an order of its own.)
		std::shuffle(names.begin(), names.end(), rng);
		for (const std::string& name : names)
			CHECK(card.Remove(name, NOW, &error), "%s", error.c_str());
		for (const Save& save : saves)
			CHECK(card.Remove(save.name, NOW, &error), "%s", error.c_str());
		CHECK(card.List().empty());
		CHECK(card.Check().empty());
		CHECK(card.EccMismatches() == 0);
		// (All that stays taken is the top directory, which grew and does not shrink.)
		CHECK(card.FreeBytes() + 1024u * 128u > empty_free && card.FreeBytes() <= empty_free, "%u against %u", card.FreeBytes(), empty_free);
		const Save save = MakeSave(rng, "AFTERWARDS", "After", {5000});
		CHECK(card.Write(save, NOW, &error), "%s", error.c_str());
		Save back;
		CHECK(card.Read(save.name, &back, &error) && SameSave(save, back, true));
		CHECK(card.List().size() == 1);
	}
}

static void SelfFormats()
{
	std::mt19937 rng(21);
	std::string error;
	const Save save = MakeSave(rng, "BASLUS-20001DATA", "Formats: A-Z 0-9", {0, 1, 1024, 33333});
	{
		Save back;
		const Bytes file = WritePSU(save);
		CHECK(std::string(FormatName(Identify(file))) == "EMS (.psu)", "%s", FormatName(Identify(file)));
		CHECK(ReadSaveFile(file, &back, NOW, &error) && SameSave(save, back, true), "%s", error.c_str());
		// (Without the filling after the last file, as some tools leave it.)
		Bytes cut = file;
		cut.resize(cut.size() - (1024 - 33333 % 1024));
		CHECK(ReadSaveFile(cut, &back, NOW, &error) && SameSave(save, back, true), "%s", error.c_str());
	}
	{
		Save back;
		const Bytes file = MakeSps(save);
		CHECK(Identify(file) == Format::SPS);
		CHECK(ReadSaveFile(file, &back, NOW, &error) && SameSave(save, back, true), "%s", error.c_str());
	}
	for (const bool whole : {false, true})
	{
		Save back;
		const Bytes file = MakeCbs(save, whole);
		CHECK(Identify(file) == Format::CBS);
		CHECK(ReadSaveFile(file, &back, NOW, &error) && SameSave(save, back, true), "%s", error.c_str());
	}
	{
		Save back;
		const Bytes file = MakePsv(save);
		CHECK(Identify(file) == Format::PSV);
		CHECK(ReadSaveFile(file, &back, NOW, &error) && SameSave(save, back, true), "%s", error.c_str());
		Bytes ps1 = file;
		detail::Put32(ps1.data() + 0x3c, 1);
		CHECK(!ReadSaveFile(ps1, &back, NOW, &error) && error.find("PlayStation 1") != std::string::npos, "%s", error.c_str());
	}
	{
		Save back;
		CHECK(Identify(Bytes()) == Format::None);
		CHECK(Identify(Bytes(4096, 0)) == Format::None);
		CHECK(!ReadSaveFile(Bytes(4096, 0x41), &back, NOW, &error) && error.find("none of the save files") != std::string::npos, "%s", error.c_str());
	}
	// The titles of saves.
	CHECK(TitleOf(IconSys("Formats: A-Z 0-9", 8)) == "Formats: A-Z 0-9", "%s", TitleOf(IconSys("Formats: A-Z 0-9", 8)).c_str());
	CHECK(TitleOf(IconSys("FirstSecond", 5)) == "First Second", "%s", TitleOf(IconSys("FirstSecond", 5)).c_str());
	CHECK(TitleOf(Bytes(964, 0)).empty());
	CHECK(TitleOf(Bytes(10, 'P')).empty());
	{
		// Mostly letters a Latin font does not show: nothing, so that the folder's name is shown.
		Bytes icon = IconSys("AB", 0);
		for (size_t i = 0; i < 20; i += 2)
		{
			icon[0xc0 + 4 + i] = 0x88;
			icon[0xc0 + 5 + i] = 0x9f;
		}
		CHECK(TitleOf(icon).empty());
	}
}

/// Damaged input: none of it may read past what it was given, hang, or leave a card changed.
static void SelfDamage()
{
	std::mt19937 rng(31);
	std::string error;
	const Save save = MakeSave(rng, "BESLES-55555DMG", "Damage", {700, 5000});
	const std::vector<Bytes> files = {WritePSU(save), MakeSps(save), MakeCbs(save, false), MakeCbs(save, true), MakePsv(save)};
	int read = 0, refused = 0;
	for (const Bytes& file : files)
	{
		// Cut short at every length (in steps where it is long), and with bytes changed.
		for (size_t length = 0; length < file.size(); length += (length < 700 ? 1 : 37))
		{
			Save back;
			const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
			(ReadSaveFile(cut, &back, NOW, &error) ? read : refused)++;
		}
		for (int round = 0; round < 3000; round++)
		{
			Bytes changed = file;
			const int count = 1 + static_cast<int>(rng() % 4);
			for (int i = 0; i < count; i++)
			{
				// (The header more often than the rest: that is where the lengths are.)
				const size_t at = (rng() % 3 == 0) ? rng() % changed.size() : rng() % std::min<size_t>(changed.size(), 300);
				changed[at] = static_cast<uint8_t>(rng());
			}
			Save back;
			(ReadSaveFile(changed, &back, NOW, &error) ? read : refused)++;
		}
	}
	CHECK(read > 100 && refused > 100, "%d read, %d refused", read, refused);

	// A .max file needs mymc+ to be made (cross_check.py); noise after a true header is here.
	for (int round = 0; round < 300; round++)
	{
		Bytes file(0x5c + 2000);
		for (uint8_t& b : file)
			b = static_cast<uint8_t>(rng());
		std::memcpy(file.data(), "Ps2PowerSave", 12);
		std::memset(file.data() + 16, 0, 64);
		std::memcpy(file.data() + 16, "NOISE", 5);
		detail::Put32(file.data() + 80, 2004);
		detail::Put32(file.data() + 84, 1 + rng() % 3);
		detail::Put32(file.data() + 88, 1000 + rng() % 200000);
		Save back;
		(ReadSaveFile(file, &back, NOW, &error) ? read : refused)++;
	}

	// Cards: bytes changed where the layout is kept. Opened or refused, never trusted blindly;
	// and one that opens is not made worse by being written to.
	Bytes image(Card::SIZE_8MB_ECC, 0xff);
	Card::Format(&image, NOW);
	Card good;
	CHECK(good.Open(image, &error));
	CHECK(good.Write(save, NOW, &error));
	CHECK(good.Write(MakeSave(rng, "SECOND", "Second", {3000, 1}), NOW, &error));
	const Bytes sound = good.Image();
	int opened = 0, closed = 0, written = 0;
	// (Where the layout is: the superblock's numbers, the table of tables, the start of the first
	// table, and the entries of the top directory and of the saves' directories.)
	static constexpr struct
	{
		size_t at, length;
	} places[] = {{0x28, 0x2c}, {0x50, 8}, {8 * 2 * 528, 12}, {9 * 2 * 528, 96}, {41 * 2 * 528, 0x24}, {41 * 2 * 528 + 528, 0x24},
		{42 * 2 * 528, 0x24}, {42 * 2 * 528 + 528, 0x24}, {43 * 2 * 528, 0x24}, {44 * 2 * 528, 0x24}, {44 * 2 * 528 + 528, 0x24}};
	for (int round = 0; round < 1200; round++)
	{
		Bytes changed = sound;
		const int count = 1 + static_cast<int>(rng() % 2);
		for (int i = 0; i < count; i++)
		{
			const auto& place = places[rng() % (sizeof(places) / sizeof(places[0]))];
			const size_t at = place.at + rng() % place.length;
			// (Often a value that is nearly right: a length or a cluster one off, a bit flipped.)
			switch (rng() % 3)
			{
				case 0:
					changed[at] = static_cast<uint8_t>(rng());
					break;
				case 1:
					changed[at] ^= static_cast<uint8_t>(1u << (rng() % 8));
					break;
				default:
					changed[at] = static_cast<uint8_t>(changed[at] + 1);
					break;
			}
		}
		Card card;
		if (!card.Open(changed, &error))
		{
			closed++;
			CHECK(!card.IsOpen());
			continue;
		}
		opened++;
		card.List();
		card.FreeBytes();
		Save back;
		card.Read(save.name, &back, &error);
		card.Read("SECOND", &back, &error);
		const Bytes before = card.Image();
		if (card.Write(MakeSave(rng, "THIRD", "Third", {2000}), NOW, &error))
		{
			written++;
			CHECK(card.Check().empty());
		}
		else
		{
			CHECK(card.Image() == before);
		}
		const Bytes then = card.Image();
		if (!card.Remove("SECOND", NOW, &error))
			CHECK(card.Image() == then);
		else
			CHECK(card.Check().empty());
	}
	CHECK(opened > 100 && closed > 100 && written > 50, "%d opened, %d refused, %d written to", opened, closed, written);
	std::printf("  damaged save files: %d read, %d refused; damaged cards: %d opened (%d then written to), %d refused\n", read, refused, opened, written, closed);
	// Files of the wrong length, and nothing at all.
	{
		Card card;
		CHECK(!card.Open(Bytes(), &error));
		CHECK(!card.Open(Bytes(1000, 0), &error));
		Bytes shorter = sound;
		shorter.resize(shorter.size() - 528);
		CHECK(!card.Open(shorter, &error) && error == "The card's file is not as long as the card says it is.", "%s", error.c_str());
		Bytes wrong(Card::SIZE_8MB_ECC + 1, 0xff);
		CHECK(!Card::Format(&wrong, NOW));
		CHECK(!Card::Formattable(wrong.size()) && Card::Formattable(Card::SIZE_8MB_ECC) && Card::Formattable(8 * Card::SIZE_8MB_ECC));
		// The larger cards: made, written to past where an 8 MB card ends, read back.
		for (const size_t times : {2u, 8u})
		{
			Bytes large(Card::SIZE_8MB_ECC * times, 0xff);
			CHECK(Card::Format(&large, NOW));
			CHECK(card.Open(std::move(large), &error), "%s", error.c_str());
			CHECK(card.TotalBytes() > 8100u * 1024u * times, "%u", card.TotalBytes());
			std::vector<Save> kept;
			for (int i = 0; i < 12; i++)
			{
				kept.push_back(MakeSave(rng, "LARGE" + std::to_string(i), "Large", {900000}));
				CHECK(card.Write(kept.back(), NOW, &error), "%s", error.c_str());
			}
			CHECK(card.Check().empty() && card.EccMismatches() == 0);
			for (const Save& one : kept)
			{
				Save back;
				CHECK(card.Read(one.name, &back, &error) && SameSave(one, back, true));
			}
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return Usage();
	if (std::string(argv[1]) != "self")
		return Command(argc, argv);
	SelfCard(true);
	SelfCard(false);
	SelfFormats();
	SelfDamage();
	std::printf("memory cards: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
