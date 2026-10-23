// Crux2 - HD texture packs from the Internet Archive: what a pack's name and its files mean.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of PCSX2 or the console here: ps5/tests/texturepacks runs it on a PC.
//
// The collection (archive.org/details/pcsx2-hd-texture-packs) holds one archive a game, named like
// "Black (USA) [SLUS-21376] HD Remaster.rar" (a game of several discs: one archive with each
// disc's serial). Inside, a pack is the folder named by the serial that PCSX2 reads from
// textures/: "SLUS-21376/replacements/...".

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace PS5TexturePacksLogic
{
inline bool IEquals(std::string_view a, std::string_view b)
{
	return a.size() == b.size() &&
		   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y)); });
}

inline bool IContains(std::string_view haystack, std::string_view needle)
{
	if (needle.empty() || needle.size() > haystack.size())
		return needle.empty();
	for (size_t i = 0; i + needle.size() <= haystack.size(); i++)
	{
		if (IEquals(haystack.substr(i, needle.size()), needle))
			return true;
	}
	return false;
}

/// The serials in a name, as "SLUS-21376": four letters, a dash, underscore or space, and five
/// digits (a dot among them, as on a disc's own file name, is let through: "SLUS_213.76").
inline std::vector<std::string> SerialsIn(std::string_view name)
{
	std::vector<std::string> out;
	for (size_t i = 0; i + 10 <= name.size(); i++)
	{
		const auto letter = [&](size_t k) { return std::isalpha(static_cast<unsigned char>(name[k])) != 0; };
		if (!(letter(i) && letter(i + 1) && letter(i + 2) && letter(i + 3)))
			continue;
		if (i > 0 && std::isalnum(static_cast<unsigned char>(name[i - 1])))
			continue;
		const char sep = name[i + 4];
		if (sep != '-' && sep != '_' && sep != ' ')
			continue;
		std::string digits;
		size_t k = i + 5;
		for (; k < name.size() && digits.size() < 5; k++)
		{
			if (std::isdigit(static_cast<unsigned char>(name[k])))
				digits.push_back(name[k]);
			else if (name[k] != '.' || digits.empty())
				break;
		}
		if (digits.size() != 5 || (k < name.size() && std::isdigit(static_cast<unsigned char>(name[k]))))
			continue;
		std::string serial;
		for (size_t j = i; j < i + 4; j++)
			serial.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(name[j]))));
		serial += "-" + digits;
		if (std::find(out.begin(), out.end(), serial) == out.end())
			out.push_back(serial);
		i = k - 1;
	}
	return out;
}

/// A file of the collection that is a game's pack: an archive Crux2 can read, with a serial in
/// its name, and not the collection's BIOS (a console's BIOS is never fetched).
inline bool IsPack(std::string_view name)
{
	const bool archive = (name.size() > 4) && (IEquals(name.substr(name.size() - 4), ".rar") || IEquals(name.substr(name.size() - 4), ".zip") ||
												  IEquals(name.substr(name.size() - 3), ".7z"));
	return archive && !IContains(name, "BIOS") && !SerialsIn(name).empty();
}

/// What a pack is called on the screen: its file's name without the extension.
inline std::string TitleOf(std::string_view name)
{
	const size_t dot = name.rfind('.');
	return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

/// Where a file of the archive goes, under the textures folder: from the folder named by one of
/// the pack's serials on ("SLUS-21376/replacements/a.png"); a file under no such folder goes into
/// the first serial's ("<serial>/<its path>"). Empty for what is not to be written: a folder, a
/// path that climbs out ("..") or starts at the root, a Mac's resource fork.
inline std::string EntryTarget(std::string_view entry, const std::vector<std::string>& serials)
{
	if (serials.empty() || entry.empty())
		return {};
	std::vector<std::string> parts;
	std::string part;
	for (const char c : entry)
	{
		if (c == '/' || c == '\\')
		{
			if (!part.empty())
				parts.push_back(part);
			part.clear();
		}
		else
		{
			part.push_back(c);
		}
	}
	if (part.empty())
		return {}; // a folder
	parts.push_back(part);
	if (entry.front() == '/' || entry.front() == '\\' || (entry.size() > 1 && entry[1] == ':'))
		return {};
	for (const std::string& p : parts)
	{
		if (p == ".." || p == "." || p == "__MACOSX" || (p.size() > 2 && p.compare(0, 2, "._") == 0))
			return {};
	}
	size_t from = parts.size();
	std::string serial;
	for (size_t i = 0; i + 1 < parts.size() && from == parts.size(); i++)
	{
		for (const std::string& s : serials)
		{
			if (IEquals(parts[i], s))
			{
				from = i + 1;
				serial = s;
				break;
			}
		}
	}
	std::string out;
	if (from == parts.size())
	{
		// No serial folder: the whole path under the first serial's.
		out = serials.front();
		for (const std::string& p : parts)
			out += "/" + p;
		return out;
	}
	out = serial;
	for (size_t i = from; i < parts.size(); i++)
		out += "/" + parts[i];
	return out;
}

/// "1.8 GB" and the like.
inline std::string SizeText(unsigned long long bytes)
{
	char text[32];
	if (bytes >= (1ull << 30))
		std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
	else
		std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
	return text;
}

/// A file name for an address: spaces and the rest as %XX (letters, digits and -._~ kept).
inline std::string EncodeForURL(std::string_view name)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	std::string out;
	for (const char ch : name)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
		{
			out.push_back(ch);
		}
		else
		{
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}
} // namespace PS5TexturePacksLogic
