// Crux2 - a PlayStation 2 memory card as a file, and the files saves are passed around in.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// What the memory card manager of the settings works with (ps5_cards.cpp), with nothing of the
// console or the emulator in it, so that the PC can test it (ps5/tests/cards/mc_test.cpp, against
// mymc+ as a second opinion):
//
//   * Card: the file system of a card image (the .ps2 files PCSX2 keeps, with or without the
//     error-correction bytes after each page): the saves on it, one of them read out, one written
//     in, one removed, and an empty card made ready. A card is changed in memory and
//     checked (Check) before its bytes are handed back; a card that does not read as sound is
//     never written to.
//   * ReadSaveFile / WritePSU: the files a save travels in. Read: EMS (.psu), Max Drive (.max),
//     CodeBreaker (.cbs), SharkPort and X-Port (.sps, .xps) and the PlayStation 3's (.psv, of a
//     PlayStation 2 save). Written: .psu, the plainest of them.
//
// The layout of the card and of those files is as mymc+ documents it by its source (Florian
// Märkl's continuation of Ross Ridge's mymc, GPL-3.0-or-later; https://git.sr.ht/~thestr4ng3r/mymcplus):
// this is written after it, in another language and another shape. The decompression of .max
// files is Haruhiko Okumura's LZARI (1989, his to use freely), and the key of .cbs files is the
// one every tool that reads them carries.

#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Mc
{
using Bytes = std::vector<uint8_t>;

/// A time as a card keeps it: a byte of nothing, the second, minute, hour, day and month, and the
/// year in two bytes; by convention the clock of Japan.
struct Stamp
{
	uint8_t raw[8] = {};

	static Stamp Of(int year, int month, int day, int hour, int minute, int second)
	{
		Stamp s;
		s.raw[1] = static_cast<uint8_t>(second);
		s.raw[2] = static_cast<uint8_t>(minute);
		s.raw[3] = static_cast<uint8_t>(hour);
		s.raw[4] = static_cast<uint8_t>(day);
		s.raw[5] = static_cast<uint8_t>(month);
		s.raw[6] = static_cast<uint8_t>(year & 0xff);
		s.raw[7] = static_cast<uint8_t>(year >> 8);
		return s;
	}
	int Year() const { return raw[6] | (raw[7] << 8); }
	bool Set() const { return Year() != 0 && raw[5] >= 1 && raw[5] <= 12 && raw[4] >= 1 && raw[4] <= 31; }
	/// "2026-10-07 14:32"; empty when it is no date.
	std::string Text() const
	{
		if (!Set())
			return std::string();
		char text[32];
		std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", Year(), raw[5], raw[4], raw[3] % 100, raw[2] % 100);
		return text;
	}
};

/// A file of a save, and a save: a folder of the card's top with files in it.
struct File
{
	std::string name;
	uint16_t mode = 0;
	Stamp created, modified;
	uint32_t attr = 0;
	/// Where the file began on the card it was read from. It means nothing anywhere else, and
	/// nothing reads it; it is kept because the save files of other programs carry it along.
	uint32_t cluster = 0;
	Bytes data;
};
struct Save
{
	std::string name; // the folder's: "BESLES-12345SAVE"
	uint16_t mode = 0;
	Stamp created, modified;
	uint32_t attr = 0;
	uint32_t cluster = 0; // (as a file's)
	std::vector<File> files;
};

// The bits of an entry's mode.
enum : uint16_t
{
	MODE_READ = 0x0001,
	MODE_WRITE = 0x0002,
	MODE_EXECUTE = 0x0004,
	MODE_RWX = 0x0007,
	MODE_FILE = 0x0010,
	MODE_DIR = 0x0020,
	MODE_0400 = 0x0400,
	MODE_PSX = 0x1000,
	MODE_HIDDEN = 0x2000,
	MODE_EXISTS = 0x8000,
};

namespace detail
{
inline uint16_t Get16(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t Get32(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void Put16(uint8_t* p, uint16_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}
inline void Put32(uint8_t* p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16);
	p[3] = static_cast<uint8_t>(v >> 24);
}
/// A name of at most `most` bytes, up to its first zero.
inline std::string Name(const uint8_t* p, size_t most)
{
	size_t n = 0;
	while (n < most && p[n] != 0)
		n++;
	return std::string(reinterpret_cast<const char*>(p), n);
}
/// Whether a name can be a folder's or a file's on a card: 1 to 32 bytes, printable, no slash.
inline bool GoodName(const std::string& name)
{
	if (name.empty() || name.size() > 32 || name == "." || name == "..")
		return false;
	for (const char c : name)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || u == 0x7f || c == '/' || c == '\\')
			return false;
	}
	return true;
}

constexpr size_t ENTRY = 512;

/// An entry of a directory, as its 512 bytes.
struct Entry
{
	uint16_t mode = 0;
	uint32_t length = 0; // a file's bytes; a directory's entries
	Stamp created, modified;
	uint32_t cluster = 0xffffffffu;
	uint32_t dir_entry = 0;
	uint32_t attr = 0;
	std::string name;

	static Entry From(const uint8_t* p)
	{
		Entry e;
		e.mode = Get16(p);
		e.length = Get32(p + 4);
		std::memcpy(e.created.raw, p + 8, 8);
		e.cluster = Get32(p + 0x10);
		e.dir_entry = Get32(p + 0x14);
		std::memcpy(e.modified.raw, p + 0x18, 8);
		e.attr = Get32(p + 0x20);
		e.name = Name(p + 0x40, 32);
		return e;
	}
	void To(uint8_t* p) const
	{
		std::memset(p, 0, ENTRY);
		Put16(p, mode);
		Put32(p + 4, length);
		std::memcpy(p + 8, created.raw, 8);
		Put32(p + 0x10, cluster);
		Put32(p + 0x14, dir_entry);
		std::memcpy(p + 0x18, modified.raw, 8);
		Put32(p + 0x20, attr);
		std::memcpy(p + 0x40, name.data(), std::min<size_t>(name.size(), 32));
	}
	bool IsFile() const { return (mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) == (MODE_FILE | MODE_EXISTS); }
	bool IsDir() const { return (mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) == (MODE_DIR | MODE_EXISTS); }
};

/// The three bytes of error correction for 128 bytes of a page.
inline void Ecc(const uint8_t* data, uint8_t out[3])
{
	static const struct Tables
	{
		uint8_t parity[256], column[256];
		Tables()
		{
			static constexpr uint8_t masks[7] = {0x55, 0x33, 0x0f, 0x00, 0xaa, 0xcc, 0xf0};
			for (int b = 0; b < 256; b++)
			{
				int x = b;
				x ^= x >> 1;
				x ^= x >> 2;
				x ^= x >> 4;
				parity[b] = static_cast<uint8_t>(x & 1);
			}
			for (int b = 0; b < 256; b++)
			{
				uint8_t mask = 0;
				for (int i = 0; i < 7; i++)
					mask = static_cast<uint8_t>(mask | (parity[b & masks[i]] << i));
				column[b] = mask;
			}
		}
	} tables;
	unsigned column = 0x77, line0 = 0x7f, line1 = 0x7f;
	for (unsigned i = 0; i < 128; i++)
	{
		const uint8_t b = data[i];
		column ^= tables.column[b];
		if (tables.parity[b])
		{
			line0 ^= ~i;
			line1 ^= i;
		}
	}
	out[0] = static_cast<uint8_t>(column);
	out[1] = static_cast<uint8_t>(line0 & 0x7f);
	out[2] = static_cast<uint8_t>(line1);
}
} // namespace detail

/// What a save calls itself: the title of its icon.sys (two lines of Shift-JIS, mostly the wide
/// Latin letters), as plain text; empty when the file is none, or says nothing a Latin font shows.
inline std::string TitleOf(const Bytes& icon_sys)
{
	if (icon_sys.size() < 964 || std::memcmp(icon_sys.data(), "PS2D", 4) != 0)
		return std::string();
	const uint8_t* const title = icon_sys.data() + 0xc0;
	// (The second line begins this many bytes in.)
	const size_t second = detail::Get16(icon_sys.data() + 6);
	std::string out;
	int shown = 0, unknown = 0;
	for (size_t i = 0; i + 1 < 68 && title[i] != 0; i += 2)
	{
		if (i == second && !out.empty() && out.back() != ' ')
			out.push_back(' ');
		const unsigned code = (title[i] << 8) | title[i + 1];
		char c = 0;
		if (title[i] < 0x80)
		{
			// (A single-byte letter, which a title should not hold: taken as it is.)
			c = (title[i] >= 0x20 && title[i] < 0x7f) ? static_cast<char>(title[i]) : 0;
			i--;
		}
		else if (code >= 0x8260 && code <= 0x8279)
			c = static_cast<char>('A' + (code - 0x8260));
		else if (code >= 0x8281 && code <= 0x829a)
			c = static_cast<char>('a' + (code - 0x8281));
		else if (code >= 0x824f && code <= 0x8258)
			c = static_cast<char>('0' + (code - 0x824f));
		else
		{
			static constexpr struct
			{
				unsigned code;
				char c;
			} signs[] = {{0x8140, ' '}, {0x8143, ','}, {0x8144, '.'}, {0x8146, ':'}, {0x8147, ';'}, {0x8148, '?'}, {0x8149, '!'}, {0x8151, '_'},
				{0x815b, '-'}, {0x815c, '-'}, {0x815d, '-'}, {0x815e, '/'}, {0x8160, '~'}, {0x8165, '\''}, {0x8166, '\''}, {0x8167, '"'},
				{0x8168, '"'}, {0x8169, '('}, {0x816a, ')'}, {0x816d, '['}, {0x816e, ']'}, {0x817b, '+'}, {0x817c, '-'}, {0x8181, '='},
				{0x8183, '<'}, {0x8184, '>'}, {0x8190, '$'}, {0x8193, '%'}, {0x8194, '#'}, {0x8195, '&'}, {0x8196, '*'}, {0x8197, '@'}};
			for (const auto& sign : signs)
			{
				if (sign.code == code)
					c = sign.c;
			}
		}
		if (c != 0)
		{
			out.push_back(c);
			shown += (c != ' ') ? 1 : 0;
		}
		else
		{
			unknown++;
		}
	}
	// (One space where there are several: the break between the lines often has one of its own.)
	{
		std::string single;
		for (const char c : out)
		{
			if (c != ' ' || (!single.empty() && single.back() != ' '))
				single.push_back(c);
		}
		while (!single.empty() && single.back() == ' ')
			single.pop_back();
		out = std::move(single);
	}
	// A title mostly in letters this cannot show says less than the folder's name does.
	return (shown > 0 && unknown <= shown) ? out : std::string();
}

// ------------------------------------------------------------------------------- a card

class Card
{
public:
	/// The sizes of an 8 MB card's file: with the error-correction bytes, and without.
	static constexpr size_t SIZE_8MB_ECC = 16384u * 528u, SIZE_8MB_PLAIN = 16384u * 512u;

	/// A card nobody has formatted yet: PCSX2 makes a new one as a file of 0xFF bytes, and the
	/// PlayStation 2 formats it the first time it is asked to.
	static bool Unformatted(const Bytes& image)
	{
		if (image.size() < 0x154)
			return false;
		for (size_t i = 0; i < 0x154; i++)
		{
			if (image[i] != 0xff)
				return false;
		}
		return true;
	}
	/// Whether bytes of this length can be made a card of (Format).
	static bool Formattable(size_t size)
	{
		for (const size_t k : {1u, 2u, 4u, 8u})
		{
			if (size == k * SIZE_8MB_ECC || size == k * SIZE_8MB_PLAIN)
				return true;
		}
		return false;
	}

	/// Takes a card's bytes. False, with the reason, when they are not a card this can work with.
	bool Open(Bytes image, std::string* error)
	{
		using namespace detail;
		m_image = std::move(image);
		m_changed = false;
		const auto fail = [&](const char* why) {
			if (error)
				*error = why;
			m_image.clear();
			return false;
		};
		if (m_image.size() < 0x154 || std::memcmp(m_image.data(), "Sony PS2 Memory Card Format ", 28) != 0)
			return fail(Unformatted(m_image) ? "The card was never formatted." : "This is not a PlayStation 2 memory card.");
		const uint8_t* const sb = m_image.data();
		m_page = Get16(sb + 0x28);
		m_pages_per_cluster = Get16(sb + 0x2a);
		m_pages_per_block = Get16(sb + 0x2c);
		m_clusters = Get32(sb + 0x30);
		m_alloc_offset = Get32(sb + 0x34);
		m_alloc_end = Get32(sb + 0x38);
		m_root = Get32(sb + 0x3c);
		const uint32_t good1 = Get32(sb + 0x40), good2 = Get32(sb + 0x44);
		for (int i = 0; i < 32; i++)
			m_ifc[i] = Get32(sb + 0x50 + i * 4);
		if (m_page != 512 || m_pages_per_cluster != 2 || m_pages_per_block == 0 || m_clusters < 64 || m_clusters > 0x40000)
			return fail("The card's layout is not one this can work with.");
		m_cluster = m_page * m_pages_per_cluster;
		const uint64_t pages = static_cast<uint64_t>(m_clusters) * m_pages_per_cluster;
		if (m_image.size() == pages * 528)
			m_spare = 16;
		else if (m_image.size() == pages * 512)
			m_spare = 0;
		else
			return fail("The card's file is not as long as the card says it is.");
		// What may be given to files: up to the first of the two blocks kept for the card itself.
		const uint64_t limit = static_cast<uint64_t>(std::min(good1, good2)) * m_pages_per_block / m_pages_per_cluster;
		if (m_alloc_offset >= m_clusters || limit <= m_alloc_offset || limit > m_clusters)
			return fail("The card's layout is damaged.");
		m_alloc_limit = std::min<uint32_t>(m_alloc_end, static_cast<uint32_t>(limit - m_alloc_offset));
		if (m_alloc_limit == 0 || m_root >= m_alloc_limit)
			return fail("The card's layout is damaged.");
		const std::string sound = Check();
		if (!sound.empty())
		{
			if (error)
				*error = "The card does not read as sound (" + sound + "): it is left alone.";
			m_image.clear();
			return false;
		}
		return true;
	}

	bool IsOpen() const { return !m_image.empty(); }
	const Bytes& Image() const { return m_image; }
	bool Changed() const { return m_changed; }
	uint32_t ClusterSize() const { return m_cluster; }

	/// Makes an empty card of bytes of the right length (what PCSX2 makes as a new card of 8, 16,
	/// 32 or 64 MB; with the error-correction bytes, or without): formatted as mymc+ formats one.
	/// False when the bytes are of another length.
	static bool Format(Bytes* image, const Stamp& now)
	{
		using namespace detail;
		uint32_t times = 0;
		bool ecc = false;
		for (const uint32_t k : {1u, 2u, 4u, 8u})
		{
			if (image->size() == k * SIZE_8MB_ECC || image->size() == k * SIZE_8MB_PLAIN)
			{
				times = k;
				ecc = (image->size() == k * SIZE_8MB_ECC);
			}
		}
		if (times == 0)
			return false;
		const size_t raw = ecc ? 528 : 512;
		const uint32_t pages = 16384 * times, clusters = pages / 2, blocks = pages / 16;
		// Every page: zeros, and the error correction of zeros.
		uint8_t spare[16] = {};
		{
			const uint8_t zeros[128] = {};
			for (int i = 0; i < 4; i++)
				Ecc(zeros, spare + i * 3);
		}
		for (size_t page = 0; page < pages; page++)
		{
			std::memset(image->data() + page * raw, 0, 512);
			if (ecc)
				std::memcpy(image->data() + page * raw + 512, spare, 16);
		}
		const auto write_cluster = [&](uint32_t cluster, const uint8_t* data) {
			for (uint32_t i = 0; i < 2; i++)
			{
				uint8_t* const page = image->data() + (static_cast<size_t>(cluster) * 2 + i) * raw;
				std::memcpy(page, data + i * 512, 512);
				if (ecc)
				{
					for (int chunk = 0; chunk < 4; chunk++)
						Ecc(page + chunk * 128, page + 512 + chunk * 3);
					std::memset(page + 512 + 12, 0, 4);
				}
			}
		};
		// The table of the tables begins after the superblock's 8 clusters; then the tables (an
		// entry for every cluster that could be a file's), then what files are given.
		constexpr uint32_t first_ifc = 8, per = 256;
		const uint32_t fat_clusters = (clusters - (first_ifc + 2) + per - 1) / per, ifc_clusters = (fat_clusters + per - 1) / per;
		const uint32_t first_fat = first_ifc + ifc_clusters, alloc_offset = first_fat + fat_clusters;
		const uint32_t good1 = blocks - 1, good2 = blocks - 2, alloc_end = good2 * 8 - alloc_offset;
		uint8_t buffer[1024];
		// The superblock.
		std::memset(buffer, 0, sizeof(buffer));
		std::memcpy(buffer, "Sony PS2 Memory Card Format ", 28);
		std::memcpy(buffer + 0x1c, "1.2.0.0", 7);
		Put16(buffer + 0x28, 512);
		Put16(buffer + 0x2a, 2);
		Put16(buffer + 0x2c, 16);
		Put16(buffer + 0x2e, 0xff00);
		Put32(buffer + 0x30, clusters);
		Put32(buffer + 0x34, alloc_offset);
		Put32(buffer + 0x38, alloc_end);
		Put32(buffer + 0x3c, 0);
		Put32(buffer + 0x40, good1);
		Put32(buffer + 0x44, good2);
		for (uint32_t i = 0; i < ifc_clusters; i++)
			Put32(buffer + 0x50 + i * 4, first_ifc + i);
		std::memset(buffer + 0xd0, 0xff, 128);
		buffer[0x150] = 2;
		buffer[0x151] = 0x2b;
		{
			// (The superblock is one page: the cluster's second page stays as it is.)
			uint8_t* const page = image->data();
			std::memcpy(page, buffer, 512);
			if (ecc)
			{
				for (int chunk = 0; chunk < 4; chunk++)
					Ecc(page + chunk * 128, page + 512 + chunk * 3);
			}
		}
		// The block the card keeps for itself: erased.
		std::memset(image->data() + static_cast<size_t>(good2) * 16 * raw, 0xff, 16 * raw);
		// The table of the tables, the tables, and the top directory.
		for (uint32_t list = 0; list < ifc_clusters; list++)
		{
			std::memset(buffer, 0xff, sizeof(buffer));
			for (uint32_t i = 0; i < per && list * per + i < fat_clusters; i++)
				Put32(buffer + i * 4, first_fat + list * per + i);
			write_cluster(first_ifc + list, buffer);
		}
		for (uint32_t fat = 0; fat < fat_clusters; fat++)
		{
			for (uint32_t i = 0; i < per; i++)
			{
				const uint32_t n = fat * per + i;
				Put32(buffer + i * 4, (n == 0 || n >= alloc_end) ? 0xffffffffu : 0x7fffffffu);
			}
			write_cluster(first_fat + fat, buffer);
		}
		std::memset(buffer, 0, sizeof(buffer));
		Entry dot;
		dot.mode = MODE_RWX | MODE_DIR | MODE_0400 | MODE_EXISTS;
		dot.length = 2;
		dot.created = dot.modified = now;
		dot.cluster = 0;
		dot.name = ".";
		dot.To(buffer);
		Entry up = dot;
		up.mode = MODE_WRITE | MODE_EXECUTE | MODE_DIR | MODE_0400 | MODE_HIDDEN | MODE_EXISTS;
		up.length = 0;
		up.name = "..";
		up.To(buffer + ENTRY);
		write_cluster(alloc_offset, buffer);
		return true;
	}

	/// A save as the lists show it.
	struct Listed
	{
		std::string name;  // its folder
		std::string title; // what it calls itself; may be empty
		uint32_t bytes = 0; // what it takes of the card, in whole clusters
		int files = 0;
		Stamp modified;
		bool ps1 = false; // a PlayStation 1 save kept on a PlayStation 2 card
	};

	std::vector<Listed> List() const
	{
		std::vector<Listed> out;
		std::vector<detail::Entry> root;
		if (!ReadDir(m_root, RootLength(), &root))
			return out;
		for (size_t i = 2; i < root.size(); i++)
		{
			const detail::Entry& dir = root[i];
			if (!dir.IsDir())
				continue;
			Listed one;
			one.name = dir.name;
			one.modified = dir.modified;
			one.ps1 = (dir.mode & MODE_PSX) != 0;
			std::vector<detail::Entry> entries;
			if (ReadDir(dir.cluster, dir.length, &entries))
			{
				one.bytes = RoundUp(static_cast<uint32_t>(entries.size()) * detail::ENTRY);
				for (size_t f = 2; f < entries.size(); f++)
				{
					if (!entries[f].IsFile())
						continue;
					one.files++;
					one.bytes += RoundUp(entries[f].length);
					if (entries[f].name == "icon.sys")
					{
						Bytes icon;
						if (ReadFile(entries[f].cluster, std::min<uint32_t>(entries[f].length, 964), &icon))
							one.title = TitleOf(icon);
					}
				}
			}
			out.push_back(std::move(one));
		}
		return out;
	}

	/// The bytes that are free, and the bytes there are for saves in all.
	uint32_t FreeBytes() const
	{
		uint32_t free = 0;
		for (uint32_t n = 0; n < m_alloc_limit; n++)
		{
			uint32_t value;
			if (Fat(n, &value) && (value & ALLOCATED) == 0)
				free++;
		}
		return free * m_cluster;
	}
	uint32_t TotalBytes() const { return m_alloc_limit * m_cluster; }

	/// Reads a save out. False, with the reason, when it is not there or does not read.
	bool Read(const std::string& name, Save* save, std::string* error) const
	{
		using namespace detail;
		std::vector<Entry> root;
		if (!ReadDir(m_root, RootLength(), &root))
			return Fail(error, "The card's top directory does not read.");
		for (size_t i = 2; i < root.size(); i++)
		{
			const Entry& dir = root[i];
			if (!dir.IsDir() || dir.name != name)
				continue;
			std::vector<Entry> entries;
			if (!ReadDir(dir.cluster, dir.length, &entries))
				return Fail(error, "The save's directory does not read.");
			save->name = dir.name;
			save->mode = dir.mode;
			save->created = dir.created;
			save->modified = dir.modified;
			save->attr = dir.attr;
			save->cluster = dir.cluster;
			save->files.clear();
			for (size_t f = 2; f < entries.size(); f++)
			{
				const Entry& entry = entries[f];
				if (!(entry.mode & MODE_EXISTS))
					continue;
				if (!entry.IsFile())
					return Fail(error, "The save has a folder inside it, which a save file cannot carry.");
				File file;
				file.name = entry.name;
				file.mode = entry.mode;
				file.created = entry.created;
				file.modified = entry.modified;
				file.attr = entry.attr;
				file.cluster = entry.cluster;
				if (!ReadFile(entry.cluster, entry.length, &file.data))
					return Fail(error, "A file of the save does not read.");
				save->files.push_back(std::move(file));
			}
			return true;
		}
		return Fail(error, "That save is not on the card.");
	}

	/// How many clusters a save needs (its directory and its files; one more when the card's top
	/// directory has to grow for it).
	uint32_t ClustersFor(const Save& save) const
	{
		uint32_t clusters = (static_cast<uint32_t>(save.files.size()) + 2 + 1) / 2;
		for (const File& file : save.files)
			clusters += RoundUp(static_cast<uint32_t>(file.data.size())) / m_cluster;
		return clusters + 1;
	}

	/// Writes a save in. False, with the reason, when a save of that name is there, when it does
	/// not fit, or when it is nothing a card can hold; the card is as it was then.
	bool Write(const Save& save, const Stamp& now, std::string* error)
	{
		using namespace detail;
		if (!GoodName(save.name))
			return Fail(error, "The save's name is nothing a card can hold.");
		if (save.files.empty() || save.files.size() > 500)
			return Fail(error, "The save has no files, or more than a save has.");
		{
			std::set<std::string> names;
			for (const File& file : save.files)
			{
				if (!GoodName(file.name) || !names.insert(file.name).second)
					return Fail(error, "A file of the save has a name a card cannot hold, or two have the same.");
				if (file.data.size() > 0x4000000)
					return Fail(error, "A file of the save is larger than a card.");
			}
		}
		std::vector<Entry> root;
		if (!ReadDir(m_root, RootLength(), &root) || root.size() < 2)
			return Fail(error, "The card's top directory does not read.");
		for (size_t i = 2; i < root.size(); i++)
		{
			if ((root[i].mode & MODE_EXISTS) && root[i].name == save.name)
				return Fail(error, "A save of that name is on the card already.");
		}
		// The place in the top directory: one that was given up, else a new one at its end.
		size_t slot = root.size();
		for (size_t i = 2; i < root.size(); i++)
		{
			if (!(root[i].mode & MODE_EXISTS))
			{
				slot = i;
				break;
			}
		}
		const bool grow = (slot == root.size()) && (root.size() % 2 == 0);
		const uint32_t dir_clusters = (static_cast<uint32_t>(save.files.size()) + 2 + 1) / 2;
		uint32_t needed = dir_clusters + (grow ? 1 : 0);
		for (const File& file : save.files)
			needed += RoundUp(static_cast<uint32_t>(file.data.size())) / m_cluster;
		std::vector<uint32_t> free;
		for (uint32_t n = 0; n < m_alloc_limit && free.size() < needed; n++)
		{
			uint32_t value;
			if (Fat(n, &value) && (value & ALLOCATED) == 0)
				free.push_back(n);
		}
		if (free.size() < needed)
			return Fail(error, "There is not enough room on the card.");

		// Everything from here is done to a copy, which becomes the card when it reads as sound.
		Card work = *this;
		size_t next = 0;
		const auto chain = [&](uint32_t count) {
			std::vector<uint32_t> clusters(free.begin() + static_cast<std::ptrdiff_t>(next), free.begin() + static_cast<std::ptrdiff_t>(next + count));
			next += count;
			for (size_t i = 0; i < clusters.size(); i++)
				work.SetFat(clusters[i], (i + 1 < clusters.size()) ? (clusters[i + 1] | ALLOCATED) : CHAIN_END);
			return clusters;
		};
		Bytes buffer(m_cluster);
		const std::vector<uint32_t> dir = chain(dir_clusters);
		// The save's directory: itself, its parent, its files.
		std::vector<Entry> entries;
		{
			Entry dot;
			dot.mode = MODE_RWX | MODE_DIR | MODE_0400 | MODE_EXISTS;
			dot.length = 0;
			dot.created = dot.modified = save.modified.Set() ? save.modified : now;
			dot.cluster = m_root;
			dot.dir_entry = static_cast<uint32_t>(slot);
			dot.name = ".";
			entries.push_back(dot);
			dot.cluster = 0;
			dot.dir_entry = 0;
			dot.name = "..";
			entries.push_back(dot);
		}
		for (const File& file : save.files)
		{
			Entry entry;
			entry.mode = static_cast<uint16_t>((file.mode & ~MODE_DIR) | MODE_FILE | MODE_EXISTS);
			entry.length = static_cast<uint32_t>(file.data.size());
			entry.created = file.created.Set() ? file.created : now;
			entry.modified = file.modified.Set() ? file.modified : now;
			entry.attr = file.attr;
			entry.dir_entry = 0;
			entry.name = file.name;
			const std::vector<uint32_t> clusters = chain(RoundUp(entry.length) / m_cluster);
			entry.cluster = clusters.empty() ? CHAIN_END : clusters.front();
			for (size_t i = 0; i < clusters.size(); i++)
			{
				const size_t from = i * m_cluster, count = std::min<size_t>(m_cluster, file.data.size() - from);
				std::fill(buffer.begin(), buffer.end(), 0);
				std::memcpy(buffer.data(), file.data.data() + from, count);
				work.WriteCluster(m_alloc_offset + clusters[i], buffer.data());
			}
			entries.push_back(std::move(entry));
		}
		for (size_t i = 0; i < dir.size(); i++)
		{
			std::fill(buffer.begin(), buffer.end(), 0);
			for (size_t e = 0; e < 2 && i * 2 + e < entries.size(); e++)
				entries[i * 2 + e].To(buffer.data() + e * ENTRY);
			work.WriteCluster(m_alloc_offset + dir[i], buffer.data());
		}
		// The top directory: a cluster more when its last one is full, the save's entry, and its
		// own count and time.
		if (grow)
		{
			const uint32_t added = chain(1).front();
			uint32_t last = m_root;
			for (uint32_t guard = 0; guard < m_alloc_limit; guard++)
			{
				uint32_t value;
				if (!work.Fat(last, &value) || value == CHAIN_END || !(value & ALLOCATED))
					break;
				last = value & ~ALLOCATED;
			}
			work.SetFat(last, added | ALLOCATED);
			std::fill(buffer.begin(), buffer.end(), 0);
			work.WriteCluster(m_alloc_offset + added, buffer.data());
		}
		Entry top;
		top.mode = static_cast<uint16_t>((save.mode & ~MODE_FILE) | MODE_DIR | MODE_EXISTS);
		top.length = static_cast<uint32_t>(entries.size());
		top.created = save.created.Set() ? save.created : now;
		top.modified = save.modified.Set() ? save.modified : now;
		top.attr = save.attr;
		top.cluster = dir.front();
		top.dir_entry = 0;
		top.name = save.name;
		if (!work.WriteEntry(m_root, slot, top))
			return Fail(error, "The card's top directory could not be written to.");
		{
			Entry self = root[0];
			if (slot == root.size())
				self.length = static_cast<uint32_t>(root.size() + 1);
			self.modified = now;
			if (!work.WriteEntry(m_root, 0, self))
				return Fail(error, "The card's top directory could not be written to.");
		}
		const std::string sound = work.Check();
		if (!sound.empty())
			return Fail(error, "The card would not read as sound with the save on it (" + sound + "): nothing was changed.");
		m_image = std::move(work.m_image);
		m_changed = true;
		return true;
	}

	/// Takes a save off the card: its files, then its folder.
	bool Remove(const std::string& name, const Stamp& now, std::string* error)
	{
		using namespace detail;
		std::vector<Entry> root;
		if (!ReadDir(m_root, RootLength(), &root))
			return Fail(error, "The card's top directory does not read.");
		for (size_t i = 2; i < root.size(); i++)
		{
			Entry dir = root[i];
			if (!dir.IsDir() || dir.name != name)
				continue;
			Card work = *this;
			std::vector<Entry> entries;
			if (!ReadDir(dir.cluster, dir.length, &entries))
				return Fail(error, "The save's directory does not read.");
			for (size_t f = 2; f < entries.size(); f++)
			{
				Entry entry = entries[f];
				if (!(entry.mode & MODE_EXISTS))
					continue;
				if (!entry.IsFile())
					return Fail(error, "The save has a folder inside it: it is left alone.");
				work.FreeChain(entry.cluster);
				entry.mode = static_cast<uint16_t>(entry.mode & ~MODE_EXISTS);
				if (!work.WriteEntry(dir.cluster, f, entry))
					return Fail(error, "The save's directory could not be written to.");
			}
			work.FreeChain(dir.cluster);
			dir.mode = static_cast<uint16_t>(dir.mode & ~MODE_EXISTS);
			Entry self = root[0];
			self.modified = now;
			if (!work.WriteEntry(m_root, i, dir) || !work.WriteEntry(m_root, 0, self))
				return Fail(error, "The card's top directory could not be written to.");
			const std::string sound = work.Check();
			if (!sound.empty())
				return Fail(error, "The card would not read as sound without the save (" + sound + "): nothing was changed.");
			m_image = std::move(work.m_image);
			m_changed = true;
			return true;
		}
		return Fail(error, "That save is not on the card.");
	}

	/// Whether the card reads as sound: every folder and file of it a chain of its own, as long
	/// as its length says. Empty when it does; else what is wrong.
	std::string Check() const
	{
		using namespace detail;
		std::vector<uint8_t> used(m_alloc_limit, 0);
		const auto walk = [&](uint32_t first, uint32_t clusters_needed, const char* what) -> std::string {
			uint32_t cluster = first, count = 0;
			while (cluster != CHAIN_END)
			{
				if (cluster >= m_alloc_limit)
					return std::string(what) + ": a cluster outside the card";
				if (used[cluster])
					return std::string(what) + ": a cluster used twice";
				used[cluster] = 1;
				count++;
				uint32_t value;
				if (!Fat(cluster, &value))
					return std::string(what) + ": the table does not read";
				if (value == CHAIN_END)
					break;
				if (!(value & ALLOCATED))
					return std::string(what) + ": a cluster that is marked free";
				cluster = value & ~ALLOCATED;
			}
			if (count < clusters_needed)
				return std::string(what) + ": shorter than its length";
			return std::string();
		};
		std::vector<Entry> root;
		{
			// (The top directory's first entry says how long it is: read that one first.)
			Bytes first(m_cluster);
			if (!ReadCluster(m_alloc_offset + m_root, first.data()))
				return "the top directory does not read";
			const Entry dot = Entry::From(first.data()), up = Entry::From(first.data() + ENTRY);
			if (!dot.IsDir() || !up.IsDir() || dot.name != "." || up.name != ".." || dot.length < 2 || dot.length > 2048)
				return "the top directory is damaged";
			const std::string wrong = walk(m_root, (dot.length + 1) / 2, "the top directory");
			if (!wrong.empty())
				return wrong;
			if (!ReadDir(m_root, dot.length, &root))
				return "the top directory does not read";
		}
		for (size_t i = 2; i < root.size(); i++)
		{
			const Entry& dir = root[i];
			if (!(dir.mode & MODE_EXISTS))
				continue;
			if (dir.IsFile())
			{
				const std::string wrong = walk(dir.cluster, RoundUp(dir.length) / m_cluster, "a file");
				if (!wrong.empty())
					return wrong;
				continue;
			}
			if (!dir.IsDir() || dir.length < 2 || dir.length > 2048)
				return "a folder's entry is damaged";
			std::string wrong = walk(dir.cluster, (dir.length + 1) / 2, "a folder");
			if (!wrong.empty())
				return wrong;
			std::vector<Entry> entries;
			if (!ReadDir(dir.cluster, dir.length, &entries))
				return "a folder does not read";
			for (size_t f = 2; f < entries.size(); f++)
			{
				const Entry& entry = entries[f];
				if (!(entry.mode & MODE_EXISTS))
					continue;
				// (A folder inside a save is rare, and nothing here goes into one: its own
				// chain is all that is looked at.)
				const uint32_t needed = entry.IsFile() ? RoundUp(entry.length) / m_cluster : 1;
				wrong = walk(entry.cluster, needed, entry.IsFile() ? "a file" : "a folder inside a save");
				if (!wrong.empty())
					return wrong;
			}
		}
		return std::string();
	}

	/// How many pages carry error-correction bytes that are not those of their data (for the
	/// tests: after a change every page written has to be right).
	size_t EccMismatches() const
	{
		if (m_spare == 0)
			return 0;
		size_t wrong = 0;
		const size_t raw = m_page + m_spare, pages = m_image.size() / raw;
		for (size_t page = 0; page < pages; page++)
		{
			const uint8_t* const at = m_image.data() + page * raw;
			// (An erased page is all 0xFF, its spare bytes too: nothing was ever written there.)
			bool erased = true;
			for (size_t i = 0; i < 12 && erased; i++)
				erased = (at[m_page + i] == 0xff);
			if (erased)
				continue;
			for (int chunk = 0; chunk < 4; chunk++)
			{
				uint8_t ecc[3];
				detail::Ecc(at + chunk * 128, ecc);
				if (std::memcmp(ecc, at + m_page + chunk * 3, 3) != 0)
				{
					wrong++;
					break;
				}
			}
		}
		return wrong;
	}

private:
	static constexpr uint32_t ALLOCATED = 0x80000000u, CHAIN_END = 0xffffffffu;

	static bool Fail(std::string* error, std::string why)
	{
		if (error)
			*error = std::move(why);
		return false;
	}
	uint32_t RoundUp(uint32_t bytes) const { return (bytes + m_cluster - 1) / m_cluster * m_cluster; }

	// A cluster by its place on the card (not among those given to files).
	bool ReadCluster(uint32_t cluster, uint8_t* out) const
	{
		if (cluster >= m_clusters)
			return false;
		const size_t raw = m_page + m_spare;
		for (uint32_t i = 0; i < m_pages_per_cluster; i++)
			std::memcpy(out + i * m_page, m_image.data() + (static_cast<size_t>(cluster) * m_pages_per_cluster + i) * raw, m_page);
		return true;
	}
	bool WriteCluster(uint32_t cluster, const uint8_t* data)
	{
		if (cluster >= m_clusters)
			return false;
		const size_t raw = m_page + m_spare;
		for (uint32_t i = 0; i < m_pages_per_cluster; i++)
		{
			uint8_t* const page = m_image.data() + (static_cast<size_t>(cluster) * m_pages_per_cluster + i) * raw;
			std::memcpy(page, data + i * m_page, m_page);
			if (m_spare != 0)
			{
				for (int chunk = 0; chunk < 4; chunk++)
					detail::Ecc(page + chunk * 128, page + m_page + chunk * 3);
				std::memset(page + m_page + 12, 0, m_spare - 12);
			}
		}
		return true;
	}

	// Where the table's entry for a cluster is: the cluster that holds it, and the place in it.
	bool FatPlace(uint32_t n, uint32_t* cluster, uint32_t* offset) const
	{
		const uint32_t per = m_cluster / 4;
		const uint32_t table = n / per, list = table / per;
		if (list >= 32 || m_ifc[list] >= m_clusters)
			return false;
		// (Read where it is, a page of the cluster, without taking the cluster out.)
		const size_t raw = m_page + m_spare, at = (table % per) * 4;
		const size_t page = static_cast<size_t>(m_ifc[list]) * m_pages_per_cluster + at / m_page;
		*cluster = detail::Get32(m_image.data() + page * raw + at % m_page);
		*offset = (n % per) * 4;
		return *cluster < m_clusters;
	}
	bool Fat(uint32_t n, uint32_t* value) const
	{
		uint32_t cluster, offset;
		if (n >= m_alloc_end || !FatPlace(n, &cluster, &offset))
			return false;
		const size_t raw = m_page + m_spare;
		// (An entry is inside one page: read where it is, without the whole cluster.)
		const size_t page = static_cast<size_t>(cluster) * m_pages_per_cluster + offset / m_page;
		*value = detail::Get32(m_image.data() + page * raw + offset % m_page);
		return true;
	}
	bool SetFat(uint32_t n, uint32_t value)
	{
		uint32_t cluster, offset;
		if (n >= m_alloc_end || !FatPlace(n, &cluster, &offset))
			return false;
		Bytes table(m_cluster);
		if (!ReadCluster(cluster, table.data()))
			return false;
		detail::Put32(table.data() + offset, value);
		return WriteCluster(cluster, table.data());
	}
	// Gives a chain's clusters back: each stays linked to the next, as the PlayStation 2 leaves
	// them, and is marked free.
	void FreeChain(uint32_t first)
	{
		uint32_t cluster = first;
		for (uint32_t guard = 0; guard < m_alloc_limit && cluster != CHAIN_END && cluster < m_alloc_limit; guard++)
		{
			uint32_t value;
			if (!Fat(cluster, &value) || !(value & ALLOCATED))
				break;
			const uint32_t next = value & ~ALLOCATED;
			SetFat(cluster, next);
			if (next == 0x7fffffffu)
				break;
			cluster = next;
		}
	}

	// The clusters of a chain, at most `most` of them.
	bool Chain(uint32_t first, uint32_t most, std::vector<uint32_t>* out) const
	{
		out->clear();
		uint32_t cluster = first;
		while (out->size() < most)
		{
			if (cluster == CHAIN_END || cluster >= m_alloc_limit)
				return false;
			out->push_back(cluster);
			uint32_t value;
			if (!Fat(cluster, &value))
				return false;
			if (value == CHAIN_END)
				break;
			if (!(value & ALLOCATED))
				return false;
			cluster = value & ~ALLOCATED;
		}
		return out->size() == most || most == 0;
	}
	bool ReadFile(uint32_t first, uint32_t length, Bytes* out) const
	{
		out->clear();
		if (length == 0)
			return true;
		if (length > m_alloc_limit * m_cluster)
			return false;
		std::vector<uint32_t> clusters;
		if (!Chain(first, RoundUp(length) / m_cluster, &clusters))
			return false;
		out->resize(clusters.size() * m_cluster);
		for (size_t i = 0; i < clusters.size(); i++)
		{
			if (!ReadCluster(m_alloc_offset + clusters[i], out->data() + i * m_cluster))
				return false;
		}
		out->resize(length);
		return true;
	}
	bool ReadDir(uint32_t first, uint32_t length, std::vector<detail::Entry>* out) const
	{
		out->clear();
		if (length > 2048)
			return false;
		Bytes data;
		if (!ReadFile(first, length * static_cast<uint32_t>(detail::ENTRY), &data))
			return false;
		for (uint32_t i = 0; i < length; i++)
			out->push_back(detail::Entry::From(data.data() + i * detail::ENTRY));
		return true;
	}
	bool WriteEntry(uint32_t first, size_t index, const detail::Entry& entry)
	{
		std::vector<uint32_t> clusters;
		const uint32_t which = static_cast<uint32_t>(index / 2);
		if (!Chain(first, which + 1, &clusters))
			return false;
		Bytes cluster(m_cluster);
		if (!ReadCluster(m_alloc_offset + clusters[which], cluster.data()))
			return false;
		entry.To(cluster.data() + (index % 2) * detail::ENTRY);
		return WriteCluster(m_alloc_offset + clusters[which], cluster.data());
	}
	uint32_t RootLength() const
	{
		Bytes first(m_cluster);
		if (!ReadCluster(m_alloc_offset + m_root, first.data()))
			return 0;
		return std::min<uint32_t>(detail::Entry::From(first.data()).length, 2048);
	}

	Bytes m_image;
	bool m_changed = false;
	uint32_t m_page = 512, m_pages_per_cluster = 2, m_pages_per_block = 16, m_cluster = 1024, m_spare = 16;
	uint32_t m_clusters = 0, m_alloc_offset = 0, m_alloc_end = 0, m_alloc_limit = 0, m_root = 0;
	uint32_t m_ifc[32] = {};
};

// ------------------------------------------------------------------- the files saves travel in

enum class Format
{
	None,
	PSU, // EMS
	MAX, // Action Replay Max / Max Drive
	CBS, // CodeBreaker
	SPS, // SharkPort, X-Port
	PSV, // the PlayStation 3's
};

inline Format Identify(const Bytes& file)
{
	using namespace detail;
	if (file.size() >= 12 && std::memcmp(file.data(), "Ps2PowerSave", 12) == 0)
		return Format::MAX;
	if (file.size() >= 4 && std::memcmp(file.data(), "CFU\0", 4) == 0)
		return Format::CBS;
	if (file.size() >= 17 && std::memcmp(file.data(), "\x0d\0\0\0SharkPortSave", 17) == 0)
		return Format::SPS;
	if (file.size() >= 4 && std::memcmp(file.data(), "\0VSP", 4) == 0)
		return Format::PSV;
	// A .psu has no mark of its own: three directory entries that look like a save's.
	if (file.size() >= ENTRY * 3)
	{
		const Entry dir = Entry::From(file.data()), dot = Entry::From(file.data() + ENTRY), up = Entry::From(file.data() + 2 * ENTRY);
		if (dir.IsDir() && dot.IsDir() && up.IsDir() && dir.length >= 2 && dot.name == "." && up.name == "..")
			return Format::PSU;
	}
	return Format::None;
}

inline const char* FormatName(Format format)
{
	switch (format)
	{
		case Format::PSU: return "EMS (.psu)";
		case Format::MAX: return "Max Drive (.max)";
		case Format::CBS: return "CodeBreaker (.cbs)";
		case Format::SPS: return "SharkPort / X-Port (.sps, .xps)";
		case Format::PSV: return "PlayStation 3 (.psv)";
		default: return "not a save file";
	}
}

namespace detail
{
/// Haruhiko Okumura's LZARI, the decoding half: `length` bytes out of `in`. False when the input
/// ends early by more than the few bits the coder may read past its end.
inline bool Lzari(const uint8_t* in, size_t in_size, size_t length, Bytes* out)
{
	constexpr int N = 4096, F = 60, THRESHOLD = 2, N_CHAR = 256 - THRESHOLD + F, M = 15;
	constexpr uint64_t Q1 = 1u << M, Q2 = 2 * Q1, Q3 = 3 * Q1, Q4 = 4 * Q1, MAX_CUM = Q1 - 1;
	std::vector<int> sym_to_char(N_CHAR + 1), sym_freq(N_CHAR + 1), sym_cum(N_CHAR + 1);
	std::vector<uint32_t> position_cum(N + 1);
	sym_cum[N_CHAR] = 0;
	for (int sym = N_CHAR; sym >= 1; sym--)
	{
		sym_to_char[sym] = sym - 1;
		sym_freq[sym] = 1;
		sym_cum[sym - 1] = sym_cum[sym] + sym_freq[sym];
	}
	sym_freq[0] = 0;
	position_cum[N] = 0;
	for (int i = N; i >= 1; i--)
		position_cum[i - 1] = position_cum[i] + 10000 / (i + 200);

	size_t bit = 0;
	size_t past = 0; // bits asked for after the input's end
	const auto get = [&]() -> uint64_t {
		const size_t byte = bit / 8;
		if (byte >= in_size)
		{
			past++;
			bit++;
			return 0;
		}
		const uint64_t value = (in[byte] >> (7 - bit % 8)) & 1;
		bit++;
		return value;
	};
	uint64_t low = 0, high = Q4, value = 0;
	for (int i = 0; i < M + 2; i++)
		value = 2 * value + get();
	const auto narrow = [&]() {
		for (;;)
		{
			if (low >= Q2)
			{
				value -= Q2;
				low -= Q2;
				high -= Q2;
			}
			else if (low >= Q1 && high <= Q3)
			{
				value -= Q1;
				low -= Q1;
				high -= Q1;
			}
			else if (high > Q2)
			{
				break;
			}
			low += low;
			high += high;
			value = 2 * value + get();
		}
	};
	const auto update = [&](int sym) {
		if (static_cast<uint64_t>(sym_cum[0]) >= MAX_CUM)
		{
			int c = 0;
			for (int i = N_CHAR; i > 0; i--)
			{
				sym_cum[i] = c;
				c += (sym_freq[i] = (sym_freq[i] + 1) >> 1);
			}
			sym_cum[0] = c;
		}
		int i = sym;
		while (sym_freq[i] == sym_freq[i - 1])
			i--;
		if (i < sym)
			std::swap(sym_to_char[i], sym_to_char[sym]);
		sym_freq[i]++;
		while (--i >= 0)
			sym_cum[i]++;
	};
	const auto decode_char = [&]() -> int {
		const uint64_t range = high - low;
		if (range == 0 || value < low)
			return -1;
		const uint64_t x = ((value - low + 1) * static_cast<uint64_t>(sym_cum[0]) - 1) / range;
		int i = 1, j = N_CHAR;
		while (i < j)
		{
			const int k = (i + j) / 2;
			if (static_cast<uint64_t>(sym_cum[k]) > x)
				i = k + 1;
			else
				j = k;
		}
		const int sym = i;
		high = low + (range * static_cast<uint64_t>(sym_cum[sym - 1])) / static_cast<uint64_t>(sym_cum[0]);
		low += (range * static_cast<uint64_t>(sym_cum[sym])) / static_cast<uint64_t>(sym_cum[0]);
		narrow();
		const int ch = sym_to_char[sym];
		update(sym);
		return ch;
	};
	const auto decode_position = [&]() -> int {
		const uint64_t range = high - low;
		if (range == 0 || value < low)
			return -1;
		const uint64_t x = ((value - low + 1) * position_cum[0] - 1) / range;
		int i = 1, j = N;
		while (i < j)
		{
			const int k = (i + j) / 2;
			if (position_cum[k] > x)
				i = k + 1;
			else
				j = k;
		}
		const int position = i - 1;
		high = low + (range * position_cum[position]) / position_cum[0];
		low += (range * position_cum[position + 1]) / position_cum[0];
		narrow();
		return position;
	};

	std::vector<uint8_t> text(N, 0x20);
	int r = N - F;
	out->clear();
	out->reserve(length);
	while (out->size() < length)
	{
		// (The coder reads some bits past what it needs: a few dozen are no error; more are.)
		if (past > 64)
			return false;
		const int c = decode_char();
		if (c < 0)
			return false;
		if (c < 256)
		{
			out->push_back(static_cast<uint8_t>(c));
			text[static_cast<size_t>(r)] = static_cast<uint8_t>(c);
			r = (r + 1) & (N - 1);
			continue;
		}
		const int position = decode_position();
		if (position < 0)
			return false;
		const int from = (r - position - 1) & (N - 1), count = c - 255 + THRESHOLD;
		for (int k = 0; k < count && out->size() < length; k++)
		{
			const uint8_t b = text[static_cast<size_t>((from + k) & (N - 1))];
			out->push_back(b);
			text[static_cast<size_t>(r)] = b;
			r = (r + 1) & (N - 1);
		}
	}
	return true;
}

/// The key every CodeBreaker save is scrambled with (the starting order of an RC4 cipher).
inline const uint8_t* CbsKey()
{
	static const uint8_t key[256] = {0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4, 0x81, 0xb8,
		0xef, 0x98, 0x59, 0xae, 0xf9, 0x26, 0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62, 0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba,
		0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16, 0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b,
		0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52, 0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0, 0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c,
		0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1, 0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70,
		0x05, 0x7d, 0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d, 0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5, 0x54, 0xcf, 0xe7, 0x41, 0xe4,
		0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06, 0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0,
		0x11, 0xc2, 0xea, 0x74, 0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38, 0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e, 0x9b, 0x7f, 0xd8,
		0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed, 0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a,
		0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5, 0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d, 0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3, 0x58,
		0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b, 0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8,
		0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5};
	return key;
}

inline void CbsCrypt(uint8_t* data, size_t size)
{
	uint8_t s[256];
	std::memcpy(s, CbsKey(), 256);
	unsigned j = 0;
	for (size_t n = 0; n < size; n++)
	{
		const unsigned i = (n + 1) & 0xff;
		j = (j + s[i]) & 0xff;
		std::swap(s[i], s[j]);
		data[n] ^= s[(s[i] + s[j]) & 0xff];
	}
}

/// Reads through a file's bytes, never past their end.
struct Reader
{
	const Bytes& bytes;
	size_t at = 0;
	bool ok = true;

	const uint8_t* Take(size_t count)
	{
		if (!ok || count > bytes.size() - at)
		{
			ok = false;
			return nullptr;
		}
		const uint8_t* const p = bytes.data() + at;
		at += count;
		return p;
	}
	uint32_t U32()
	{
		const uint8_t* const p = Take(4);
		return p ? Get32(p) : 0;
	}
	/// A text with its length before it.
	bool Skip32Text()
	{
		const uint32_t length = U32();
		return ok && Take(length) != nullptr;
	}
};
} // namespace detail

/// Reads a save out of a file of any of the kinds above. `now` stands in where the file carries
/// no time. False, with the reason, when the file is none of them or is damaged.
inline bool ReadSaveFile(const Bytes& file, Save* save, const Stamp& now, std::string* error)
{
	using namespace detail;
	const auto fail = [&](const char* why) {
		if (error)
			*error = why;
		return false;
	};
	*save = Save();
	const uint16_t dir_mode = MODE_RWX | MODE_DIR | MODE_0400 | MODE_EXISTS, file_mode = MODE_RWX | MODE_FILE | MODE_0400 | MODE_EXISTS;
	switch (Identify(file))
	{
		case Format::PSU:
		{
			Reader in{file};
			const Entry dir = Entry::From(in.Take(ENTRY));
			in.Take(2 * ENTRY);
			save->name = dir.name;
			save->mode = dir.mode;
			save->created = dir.created;
			save->modified = dir.modified;
			save->attr = dir.attr;
			for (uint32_t i = 2; i < dir.length; i++)
			{
				const uint8_t* const raw = in.Take(ENTRY);
				if (raw == nullptr)
					return fail("The file ends before its save does.");
				const Entry entry = Entry::From(raw);
				if (!entry.IsFile())
					return fail("The save has a folder inside it, which a card's save cannot be given here.");
				const uint8_t* const data = in.Take(entry.length);
				if (data == nullptr)
					return fail("The file ends before its save does.");
				File one;
				one.name = entry.name;
				one.mode = entry.mode;
				one.created = entry.created;
				one.modified = entry.modified;
				one.attr = entry.attr;
				one.data.assign(data, data + entry.length);
				save->files.push_back(std::move(one));
				// (Each file is filled up to a whole cluster; the last one's filling may be missing.)
				const size_t fill = (1024 - entry.length % 1024) % 1024;
				in.at = std::min(file.size(), in.at + fill);
			}
			break;
		}

		case Format::MAX:
		{
			if (file.size() < 0x5c)
				return fail("The file ends before its save does.");
			const uint8_t* const header = file.data();
			save->name = Name(header + 16, 32);
			const uint32_t packed = Get32(header + 80), count = Get32(header + 84), length = Get32(header + 88);
			if (count == 0 || count > 500 || length > 0x4000000)
				return fail("The file's header is damaged.");
			// (Some files give the unpacked length where the packed one belongs: all that
			// follows the header is the data then.)
			size_t data_size = file.size() - 0x5c;
			if (packed != length && packed >= 4)
				data_size = std::min<size_t>(data_size, packed - 4);
			Bytes plain;
			if (!Lzari(file.data() + 0x5c, data_size, length, &plain))
				return fail("The file's data does not unpack.");
			save->mode = dir_mode;
			save->created = save->modified = now;
			size_t at = 0;
			for (uint32_t i = 0; i < count; i++)
			{
				if (plain.size() - at < 36)
					return fail("The save inside the file is cut short.");
				const uint32_t size = Get32(plain.data() + at);
				File one;
				one.name = Name(plain.data() + at + 4, 32);
				at += 36;
				if (size > plain.size() - at)
					return fail("The save inside the file is cut short.");
				one.mode = file_mode;
				one.created = one.modified = now;
				one.data.assign(plain.begin() + static_cast<std::ptrdiff_t>(at), plain.begin() + static_cast<std::ptrdiff_t>(at + size));
				at += size;
				at = (at + 8 + 15) / 16 * 16 - 8;
				at = std::min(at, plain.size());
				save->files.push_back(std::move(one));
			}
			break;
		}

		case Format::CBS:
		{
			Reader in{file};
			in.Take(8);
			const uint32_t header_length = in.U32();
			if (!in.ok || header_length < 92 + 32 || header_length > 4096)
				return fail("The file's header is damaged.");
			const uint8_t* const header = in.Take(header_length - 12);
			if (header == nullptr)
				return fail("The file ends before its save does.");
			const uint32_t plain_length = Get32(header), body_length = Get32(header + 4);
			save->name = Name(header + 8, 32);
			std::memcpy(save->created.raw, header + 40, 8);
			std::memcpy(save->modified.raw, header + 48, 8);
			save->mode = static_cast<uint16_t>(Get32(header + 64));
			if ((save->mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) != (MODE_DIR | MODE_EXISTS))
				save->mode = dir_mode;
			if (!save->created.Set())
				save->created = now;
			if (!save->modified.Set())
				save->modified = now;
			if (plain_length > 0x4000000)
				return fail("The file's header is damaged.");
			// (The length is of the whole file in some, of the packed body in others.)
			size_t have = file.size() - in.at;
			if (have != body_length && have != body_length - header_length)
			{
				if (have < body_length - std::min<uint32_t>(body_length, header_length))
					return fail("The file ends before its save does.");
				have = std::min<size_t>(have, body_length);
			}
			Bytes body(file.begin() + static_cast<std::ptrdiff_t>(in.at), file.begin() + static_cast<std::ptrdiff_t>(in.at + have));
			CbsCrypt(body.data(), body.size());
			Bytes plain(plain_length);
			uLongf unpacked = plain_length;
			const int rc = uncompress(plain.data(), &unpacked, body.data(), static_cast<uLong>(body.size()));
			if (rc != Z_OK && rc != Z_BUF_ERROR)
				return fail("The file's data does not unpack.");
			plain.resize(unpacked);
			size_t at = 0;
			while (at < plain.size())
			{
				if (plain.size() - at < 64)
					return fail("The save inside the file is cut short.");
				const uint8_t* const h = plain.data() + at;
				File one;
				std::memcpy(one.created.raw, h, 8);
				std::memcpy(one.modified.raw, h + 8, 8);
				const uint32_t size = Get32(h + 16);
				one.mode = Get16(h + 20);
				one.name = Name(h + 32, 32);
				at += 64;
				if (size > plain.size() - at)
					return fail("The save inside the file is cut short.");
				if ((one.mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) != (MODE_FILE | MODE_EXISTS))
					return fail("The save has a folder inside it, which a card's save cannot be given here.");
				if (!one.created.Set())
					one.created = now;
				if (!one.modified.Set())
					one.modified = now;
				one.data.assign(plain.begin() + static_cast<std::ptrdiff_t>(at), plain.begin() + static_cast<std::ptrdiff_t>(at + size));
				at += size;
				save->files.push_back(std::move(one));
			}
			break;
		}

		case Format::SPS:
		{
			Reader in{file};
			in.Take(17);
			in.U32(); // the kind of save
			in.Skip32Text();
			in.Skip32Text();
			in.Skip32Text();
			in.U32(); // the length of what follows
			const auto entry = [&](std::string* name, uint32_t* length, uint16_t* mode, Stamp* created, Stamp* modified) {
				const uint8_t* const h = in.Take(98);
				if (h == nullptr)
					return false;
				const uint32_t header = Get16(h);
				*name = Name(h + 2, 64);
				*length = Get32(h + 66);
				// (The mode's two bytes are the other way round in these files.)
				*mode = static_cast<uint16_t>((h[78] << 8) | h[79]);
				std::memcpy(created->raw, h + 82, 8);
				std::memcpy(modified->raw, h + 90, 8);
				return header >= 98 && in.Take(header - 98) != nullptr;
			};
			uint32_t entries = 0;
			if (!entry(&save->name, &entries, &save->mode, &save->created, &save->modified))
				return fail("The file ends before its save does.");
			if ((save->mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) != (MODE_DIR | MODE_EXISTS) || entries < 2 || entries > 502)
				return fail("The file's header is damaged.");
			for (uint32_t i = 2; i < entries; i++)
			{
				File one;
				uint32_t size = 0;
				if (!entry(&one.name, &size, &one.mode, &one.created, &one.modified))
					return fail("The file ends before its save does.");
				if ((one.mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) != (MODE_FILE | MODE_EXISTS))
					return fail("The save has a folder inside it, which a card's save cannot be given here.");
				const uint8_t* const data = in.Take(size);
				if (data == nullptr)
					return fail("The file ends before its save does.");
				one.data.assign(data, data + size);
				save->files.push_back(std::move(one));
			}
			break;
		}

		case Format::PSV:
		{
			Reader in{file};
			const uint8_t* const header = in.Take(0x40);
			if (header == nullptr || Get32(header + 4) != 0)
				return fail("The file's header is damaged.");
			const uint32_t kind = Get32(header + 0x3c);
			if (kind != 2)
				return fail("This is a PlayStation 1 save: a PlayStation 2 card's save is what can be brought in here.");
			const uint8_t* const ps2 = in.Take(40);
			if (ps2 == nullptr)
				return fail("The file ends before its save does.");
			const uint32_t count = Get32(ps2 + 36);
			if (count == 0 || count > 500)
				return fail("The file's header is damaged.");
			const auto info = [&](std::string* name, uint32_t* size, uint16_t* mode, Stamp* created, Stamp* modified) {
				const uint8_t* const h = in.Take(56);
				if (h == nullptr)
					return false;
				std::memcpy(created->raw, h, 8);
				std::memcpy(modified->raw, h + 8, 8);
				*size = Get32(h + 16);
				*mode = static_cast<uint16_t>(Get32(h + 20));
				*name = Name(h + 24, 32);
				return true;
			};
			uint32_t unused = 0;
			if (!info(&save->name, &unused, &save->mode, &save->created, &save->modified))
				return fail("The file ends before its save does.");
			if ((save->mode & (MODE_FILE | MODE_DIR | MODE_EXISTS)) != (MODE_DIR | MODE_EXISTS))
				return fail("The file's header is damaged.");
			std::vector<std::pair<uint32_t, uint32_t>> places; // where each file's bytes are, and how many
			for (uint32_t i = 0; i < count; i++)
			{
				File one;
				uint32_t size = 0;
				if (!info(&one.name, &size, &one.mode, &one.created, &one.modified))
					return fail("The file ends before its save does.");
				const uint32_t offset = in.U32();
				if (!in.ok)
					return fail("The file ends before its save does.");
				if (one.mode & MODE_DIR)
					return fail("The save has a folder inside it, which a card's save cannot be given here.");
				one.mode = static_cast<uint16_t>(one.mode | MODE_FILE | MODE_EXISTS);
				places.emplace_back(offset, size);
				save->files.push_back(std::move(one));
			}
			for (size_t i = 0; i < places.size(); i++)
			{
				const auto [offset, size] = places[i];
				if (offset > file.size() || size > file.size() - offset)
					return fail("The file ends before its save does.");
				save->files[i].data.assign(file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(offset + size));
			}
			break;
		}

		default:
			return fail("This is none of the save files it knows: .psu, .max, .cbs, .sps, .xps or .psv.");
	}
	if (!GoodName(save->name) || save->files.empty())
		return fail("The file holds no save a card can be given.");
	for (const File& one : save->files)
	{
		if (!GoodName(one.name))
			return fail("A file of the save has a name a card cannot hold.");
	}
	return true;
}

/// A save as an EMS file (.psu): its folder's entry, the two every folder begins with, then each
/// file's entry and its bytes filled up to a whole cluster.
inline Bytes WritePSU(const Save& save)
{
	using namespace detail;
	Bytes out;
	const auto put = [&](const Entry& entry) {
		const size_t at = out.size();
		out.resize(at + ENTRY);
		entry.To(out.data() + at);
	};
	Entry dir;
	dir.mode = static_cast<uint16_t>((save.mode & ~MODE_FILE) | MODE_DIR | MODE_EXISTS);
	dir.length = static_cast<uint32_t>(save.files.size() + 2);
	dir.created = save.created;
	dir.modified = save.modified;
	dir.attr = save.attr;
	dir.cluster = save.cluster;
	dir.name = save.name;
	put(dir);
	Entry dot;
	dot.mode = MODE_RWX | MODE_DIR | MODE_0400 | MODE_EXISTS;
	dot.length = 0;
	dot.created = dot.modified = save.created;
	dot.cluster = 0;
	dot.name = ".";
	put(dot);
	dot.name = "..";
	put(dot);
	for (const File& file : save.files)
	{
		Entry entry;
		entry.mode = static_cast<uint16_t>((file.mode & ~MODE_DIR) | MODE_FILE | MODE_EXISTS);
		entry.length = static_cast<uint32_t>(file.data.size());
		entry.created = file.created;
		entry.modified = file.modified;
		entry.attr = file.attr;
		entry.cluster = file.cluster;
		entry.name = file.name;
		put(entry);
		out.insert(out.end(), file.data.begin(), file.data.end());
		out.resize(out.size() + (1024 - file.data.size() % 1024) % 1024);
	}
	return out;
}

} // namespace PS5Mc
