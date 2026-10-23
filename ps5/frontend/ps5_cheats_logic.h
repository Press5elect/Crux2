// Crux2 - cheats from collections on GitHub: the part that is plain C++ (ps5/tests/online
// builds it for the PC). ps5_online.cpp does the fetching.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The title carries a list of where the .pnach files of a few collections are
// (resources/ps5/cheat-index.txt, made by tools/make-online-lists.sh): names only. A game's
// files are those whose name has the game's CRC in it, which is how every collection names them
// ("SLUS-12345_0123ABCD.pnach", "0123ABCD.pnach", "SLUS-12345_0123ABCD_Something.pnach"). A file
// whose name has the serial but no CRC that can be read (a few of a collection's names lost theirs
// to a spreadsheet: "SLUS-12345_6441001", "SLUS-12345_3.27E+13") is the game's by its serial,
// unless that collection has a file named for the game's CRC.
// A collection's files under "patches/" are patches (60 fps, no interlacing...): they go to the
// patches folder, where PCSX2 shows them on a game's Patches page, not among its cheats.
// Besides the list, the user may name addresses of his own in cheat-sources.txt, as patterns
// with {serial} and {crc} in them.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Cheats
{
struct Source
{
	std::string id, repo, branch;
};

struct Index
{
	std::vector<Source> sources;
	struct File
	{
		int source;
		std::string path;
	};
	std::vector<File> files;
};

/// What is to be fetched for a game, and what it is saved as.
struct Wanted
{
	std::string url;
	std::string save_name; // in the cheats folder: a name PCSX2 loads for the game
	std::string from;      // "owner/repo: path", for the file's first line and the log
	bool patch = false;    // a patch (the patches folder), not a cheat (the cheats folder)
};

inline std::string_view TrimLine(std::string_view line)
{
	while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
		line.remove_suffix(1);
	while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
		line.remove_prefix(1);
	return line;
}

template <typename F>
inline void ForEachLine(std::string_view text, F&& each)
{
	while (!text.empty())
	{
		const size_t end = text.find('\n');
		each(text.substr(0, end));
		if (end == std::string_view::npos)
			break;
		text.remove_prefix(end + 1);
	}
}

inline Index ParseIndex(std::string_view text)
{
	Index index;
	ForEachLine(text, [&](std::string_view raw) {
		const std::string_view line = TrimLine(raw);
		if (line.empty() || line[0] == '#')
			return;
		if (line[0] == '@')
		{
			// "@id owner/repo branch ..."
			std::string_view rest = line.substr(1);
			std::string_view parts[3];
			for (std::string_view& part : parts)
			{
				const size_t end = rest.find(' ');
				part = rest.substr(0, end);
				rest = (end == std::string_view::npos) ? std::string_view() : TrimLine(rest.substr(end + 1));
			}
			if (!parts[0].empty() && parts[1].find('/') != std::string_view::npos && !parts[2].empty())
				index.sources.push_back({std::string(parts[0]), std::string(parts[1]), std::string(parts[2])});
			return;
		}
		const size_t tab = line.find('\t');
		if (tab == std::string_view::npos)
			return;
		for (size_t i = 0; i < index.sources.size(); i++)
		{
			if (index.sources[i].id == line.substr(0, tab))
			{
				index.files.push_back({static_cast<int>(i), std::string(line.substr(tab + 1))});
				break;
			}
		}
	});
	return index;
}

/// A path as an address has it: each part between the slashes percent-encoded.
inline std::string EncodePath(std::string_view path)
{
	std::string out;
	for (const char c : path)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~' || u == '/')
		{
			out += c;
		}
		else
		{
			char hex[4];
			std::snprintf(hex, sizeof(hex), "%%%02X", u);
			out += hex;
		}
	}
	return out;
}

inline bool IsHexDigit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Whether a file's name (its last part) has this CRC in it as a word of its own.
inline bool NameHasCRC(std::string_view path, uint32_t crc)
{
	const size_t slash = path.rfind('/');
	const std::string_view name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
	char wanted[9];
	std::snprintf(wanted, sizeof(wanted), "%08X", crc);
	for (size_t at = 0; at + 8 <= name.size(); at++)
	{
		bool same = true;
		for (size_t i = 0; i < 8 && same; i++)
		{
			const char c = name[at + i];
			same = ((c >= 'a' && c <= 'f') ? static_cast<char>(c - 32) : c) == wanted[i];
		}
		if (same && (at == 0 || !IsHexDigit(name[at - 1])) && (at + 8 == name.size() || !IsHexDigit(name[at + 8])))
			return true;
	}
	return false;
}

/// The file's name (its last part).
inline std::string_view LastPart(std::string_view path)
{
	const size_t slash = path.rfind('/');
	return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

/// Whether a file's name has any eight hexadecimal digits as a word of their own (a CRC).
inline bool NameHasSomeCRC(std::string_view path)
{
	const std::string_view name = LastPart(path);
	for (size_t at = 0; at + 8 <= name.size(); at++)
	{
		bool hex = true;
		for (size_t i = 0; i < 8 && hex; i++)
			hex = IsHexDigit(name[at + i]);
		if (hex && (at == 0 || !IsHexDigit(name[at - 1])) && (at + 8 == name.size() || !IsHexDigit(name[at + 8])))
			return true;
	}
	return false;
}

/// Whether a file's name begins with this serial ("SLUS-12345_..."), in capitals or not.
inline bool NameHasSerial(std::string_view path, std::string_view serial)
{
	const std::string_view name = LastPart(path);
	if (serial.size() < 5 || name.size() <= serial.size())
		return false;
	for (size_t i = 0; i < serial.size(); i++)
	{
		const char a = name[i], b = serial[i];
		if (((a >= 'a' && a <= 'z') ? a - 32 : a) != ((b >= 'a' && b <= 'z') ? b - 32 : b))
			return false;
	}
	const char after = name[serial.size()];
	return after == '_' || after == '.' || after == ' ' || after == '-';
}

/// Whether a path is in a collection's "patches" folder (its first part).
inline bool IsPatchPath(std::string_view path)
{
	constexpr std::string_view folder = "patches/";
	if (path.size() <= folder.size())
		return false;
	for (size_t i = 0; i < folder.size(); i++)
	{
		const char c = path[i];
		if (((c >= 'A' && c <= 'Z') ? c + 32 : c) != folder[i])
			return false;
	}
	return true;
}

/// The name a fetched file is kept under: "SERIAL_CRC_<tag>.pnach" is among those PCSX2 reads
/// for the game ("CRC_<tag>.pnach" when the game has no serial), and the tag tells it from a
/// file the user put there himself.
inline std::string SaveName(std::string_view serial, uint32_t crc, std::string_view tag)
{
	char hex[9];
	std::snprintf(hex, sizeof(hex), "%08X", crc);
	std::string name;
	if (!serial.empty())
	{
		name += serial;
		name += '_';
	}
	name += hex;
	name += '_';
	name += tag;
	name += ".pnach";
	return name;
}

/// The files of the list for a game, in the list's order (one collection after another).
inline std::vector<Wanted> FilesFor(const Index& index, std::string_view serial, uint32_t crc)
{
	std::vector<Wanted> wanted;
	if (crc == 0)
		return wanted;
	// A file named for the game's CRC wins: a collection that has one gives nothing by the serial.
	std::vector<char> exact(index.sources.size(), 0);
	for (const Index::File& file : index.files)
	{
		if (NameHasCRC(file.path, crc))
			exact[static_cast<size_t>(file.source)] = 1;
	}
	std::vector<int> counts(index.sources.size(), 0);
	for (const Index::File& file : index.files)
	{
		const bool by_serial = !exact[static_cast<size_t>(file.source)] && NameHasSerial(file.path, serial) && !NameHasSomeCRC(file.path);
		if (!NameHasCRC(file.path, crc) && !by_serial)
			continue;
		const Source& source = index.sources[static_cast<size_t>(file.source)];
		const int n = ++counts[static_cast<size_t>(file.source)];
		Wanted w;
		w.url = "https://raw.githubusercontent.com/" + source.repo + "/" + EncodePath(source.branch) + "/" + EncodePath(file.path);
		w.save_name = SaveName(serial, crc, "github-" + source.id + (n > 1 ? "-" + std::to_string(n) : std::string()));
		w.from = source.repo + ": " + file.path;
		w.patch = IsPatchPath(file.path);
		wanted.push_back(std::move(w));
	}
	return wanted;
}

/// The user's own addresses (cheat-sources.txt): one a line, "#" for notes. Only https.
inline std::vector<std::string> ParsePatterns(std::string_view text)
{
	std::vector<std::string> patterns;
	ForEachLine(text, [&](std::string_view raw) {
		const std::string_view line = TrimLine(raw);
		if (line.rfind("https://", 0) == 0 && line.find("{crc}") + line.find("{CRC}") != 2 * std::string_view::npos)
			patterns.emplace_back(line);
	});
	return patterns;
}

/// A pattern with the game put in: {serial} "SLUS-12345", {crc} "0123abcd", {CRC} "0123ABCD".
inline std::string ExpandPattern(std::string_view pattern, std::string_view serial, uint32_t crc)
{
	char upper[9], lower[9];
	std::snprintf(upper, sizeof(upper), "%08X", crc);
	std::snprintf(lower, sizeof(lower), "%08x", crc);
	std::string out(pattern);
	const auto put = [&](std::string_view what, std::string_view with) {
		for (size_t at = out.find(what); at != std::string::npos; at = out.find(what, at + with.size()))
			out.replace(at, what.size(), with);
	};
	put("{serial}", serial);
	put("{SERIAL}", serial);
	put("{crc}", lower);
	put("{CRC}", upper);
	return out;
}

/// Whether what came is a .pnach file: text, of a sane size, with at least one patch line.
inline bool LooksLikePnach(std::string_view data, std::string* why)
{
	if (data.size() < 12 || data.size() > 512 * 1024)
	{
		*why = "its size is not a cheat file's";
		return false;
	}
	if (data.find('\0') != std::string_view::npos)
	{
		*why = "it is not text";
		return false;
	}
	bool patch = false;
	ForEachLine(data, [&](std::string_view raw) {
		const std::string_view line = TrimLine(raw);
		patch = patch || line.rfind("patch=", 0) == 0 || line.rfind("patch =", 0) == 0;
	});
	if (!patch)
		*why = "it has no patch lines (a page, or an empty file)";
	return patch;
}

/// A fetched file as PCSX2 reads it best, line for line otherwise as it came:
/// - no UTF-8 byte order mark (PCSX2 would read it as part of the first line, and a first
///   "[name]" would not be one);
/// - a cheat's name broken over lines ("[Infinite Ammo (Press Select" / "up To Activate" /
///   "down To Stop)]") joined into one, with spaces: PCSX2 refuses the broken line, and the cheat's
///   lines would then belong to the cheat before it. A name not closed within four lines is left.
/// *repaired counts the names joined.
inline std::string RepairPnach(std::string_view text, int* repaired)
{
	*repaired = 0;
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF)
		text.remove_prefix(3);
	std::vector<std::string_view> lines;
	ForEachLine(text, [&](std::string_view line) { lines.push_back(line); });
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < lines.size(); i++)
	{
		const std::string_view t = TrimLine(lines[i]);
		if (t.size() >= 1 && t.front() == '[' && t.back() != ']' && t.find(']') == std::string_view::npos)
		{
			size_t end = i + 1;
			while (end < lines.size() && end <= i + 4)
			{
				const std::string_view next = TrimLine(lines[end]);
				if (next.empty() || next.front() == '[' || next.find('=') != std::string_view::npos)
				{
					end = lines.size();
					break;
				}
				if (next.back() == ']')
					break;
				end++;
			}
			if (end < lines.size() && end <= i + 4)
			{
				std::string joined(t);
				for (size_t j = i + 1; j <= end; j++)
				{
					joined += ' ';
					joined += TrimLine(lines[j]);
				}
				out += joined;
				out += '\n';
				(*repaired)++;
				i = end;
				continue;
			}
		}
		out.append(lines[i]);
		if (i + 1 < lines.size())
			out += '\n';
	}
	return out;
}

/// The default cheat-sources.txt.
inline const char* DefaultPatternsFile()
{
	return "# Crux2 - more places to look for a game's cheats (Settings > Interface > Download cheats,\n"
		   "# and \"Download cheats for this game\" on a game's Cheats page).\n"
		   "#\n"
		   "# The collections the title knows are asked first. Here you can add addresses of your own:\n"
		   "# one a line, https only, with the game put in where the address has\n"
		   "#   {serial}   SLUS-12345\n"
		   "#   {CRC}      0123ABCD        {crc}   0123abcd\n"
		   "# For a GitHub repository that is the \"raw\" address of a file, for example\n"
		   "#   https://raw.githubusercontent.com/someone/their-cheats/main/cheats/{serial}_{CRC}.pnach\n"
		   "#\n"
		   "# What is found is saved in cheats/ as <serial>_<CRC>_github-....pnach; your own files there are\n"
		   "# never changed. Cheats are off until you switch them on in the game's options.\n";
}
// ---- a cheat typed in on the console
//
// A code as the lists of them print it: lines of two groups of eight hexadecimal digits, "2034A5B0
// 00000001". They are raw codes, the kind PCSX2 applies as they are; a code made for a cheat
// device that scrambles its codes looks the same and does nothing, and nothing here can tell.
// Typed cheats are kept in a file of their own beside the downloaded ones
// (<serial>_<CRC>_typed.pnach), so that a download never replaces them.

/// A typed line as a line of a cheat file: "patch=1,EE,2034A5B0,extended,00000001". Between the
/// two groups there may be spaces, a colon, a dash or nothing; small letters are taken as capitals.
/// Empty when it is not sixteen hexadecimal digits.
inline std::string TypedCodeLine(std::string_view typed)
{
	std::string digits;
	for (const char c : typed)
	{
		if (IsHexDigit(c))
			digits.push_back(static_cast<char>((c >= 'a' && c <= 'f') ? c - 'a' + 'A' : c));
		else if (c != ' ' && c != ':' && c != '-' && c != '\t')
			return std::string();
	}
	if (digits.size() != 16)
		return std::string();
	return "patch=1,EE," + digits.substr(0, 8) + ",extended," + digits.substr(8);
}

/// A name as a cheat file's section can carry it: no brackets, no backslash (PCSX2 reads one as
/// a group), no line ends, nothing at its ends; at most 60 characters. Empty when nothing is left.
inline std::string TypedCheatName(std::string_view typed)
{
	std::string name;
	for (const char c : typed)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		name.push_back((c == '[' || c == ']' || c == '\\' || c == '=' || u < 0x20) ? ' ' : c);
	}
	while (!name.empty() && name.back() == ' ')
		name.pop_back();
	size_t first = 0;
	while (first < name.size() && name[first] == ' ')
		first++;
	name.erase(0, first);
	if (name.size() > 60)
		name.resize(60);
	while (!name.empty() && name.back() == ' ')
		name.pop_back();
	return name;
}

/// The names of the cheats a file holds, in its order.
inline std::vector<std::string> CheatNames(std::string_view file)
{
	std::vector<std::string> names;
	ForEachLine(file, [&](std::string_view line) {
		line = TrimLine(line);
		if (line.size() > 2 && line.front() == '[' && line.back() == ']')
			names.emplace_back(line.substr(1, line.size() - 2));
	});
	return names;
}

/// A file's text without the cheat of that name (its section, up to the next one).
inline std::string WithoutCheat(std::string_view file, std::string_view name)
{
	std::string out;
	bool skipping = false;
	ForEachLine(file, [&](std::string_view line) {
		const std::string_view trimmed = TrimLine(line);
		if (trimmed.size() > 2 && trimmed.front() == '[' && trimmed.back() == ']')
			skipping = (trimmed.substr(1, trimmed.size() - 2) == name);
		if (!skipping)
		{
			out.append(line);
			out.push_back('\n');
		}
	});
	// (No run of empty lines where a cheat was.)
	while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
		out.pop_back();
	return out;
}

/// A file's text with a cheat added at its end: its name and its lines (as TypedCodeLine made
/// them). A cheat of the same name is replaced. An empty file gets a first line saying what it is.
inline std::string WithTypedCheat(std::string_view file, std::string_view name, const std::vector<std::string>& lines)
{
	std::string out = WithoutCheat(file, name);
	if (out.empty())
		out = "// Cheats typed in on the console (Crux2). A game's options > Cheats adds to this file and takes from it.\n";
	if (out.back() != '\n')
		out.push_back('\n');
	out += "\n[" + std::string(name) + "]\ndescription=Typed in on the console\n";
	for (const std::string& line : lines)
		out += line + "\n";
	return out;
}

/// The file the typed cheats of a game are kept in: "<serial>_<CRC>_typed.pnach".
inline std::string TypedFileName(std::string_view serial, uint32_t crc)
{
	return SaveName(serial, crc, "typed");
}
} // namespace PS5Cheats
