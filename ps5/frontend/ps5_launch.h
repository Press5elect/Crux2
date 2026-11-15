// Crux2 - launch arguments: a game started straight away, and Crux2 closed after it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 839dfda and fcb4fd0 (src/porpoise_forward.hpp, docs/FORWARDER.md; GPL-3.0-or-later).
// Whatever starts the title with arguments (the console's launcher, or a payload through the
// loader) can give:
//
//   --rom <file>  or  --rom=<file>   the game to start, instead of the library. An absolute path
//                                    (/mnt/usb0/games/Game.iso, /smb/nas/Game.chd), or a path
//                                    inside the title folder's games/ ("Game.iso", "PS2/Game.chd");
//                                    a relative path may not have a ".." in it.
//   --exit-after-game                when that game has run and is closed, Crux2 closes too, back to
//                                    the home screen. A game that does not start leaves the library
//                                    open, with the reason.
//
// Other arguments are left alone. Arguments only reach a new process: a title already running is
// brought to the front and its main() does not run again.
//
// main() (ps5_host.cpp) reads them once, before any other thread starts; the interface
// (bigpicture/bp_pcsx2.cpp) starts the game once its first frames are drawn.

#pragma once

#include <string>
#include <string_view>

namespace PS5Launch
{
struct Args
{
	std::string rom;              // --rom; empty when not given
	bool exit_after_game = false; // --exit-after-game
};

inline Args Parse(int argc, char** argv)
{
	Args result;
	if (argc <= 0 || argv == nullptr)
		return result;
	constexpr std::string_view rom_flag = "--rom";
	constexpr std::string_view rom_prefix = "--rom=";
	for (int i = 0; i < argc; ++i)
	{
		if (argv[i] == nullptr)
			break;
		const std::string_view arg{argv[i]};
		if (arg == "--exit-after-game")
			result.exit_after_game = true;
		else if (arg == rom_flag)
		{
			if (i + 1 < argc && argv[i + 1] != nullptr)
				result.rom = argv[++i];
		}
		else if (arg.substr(0, rom_prefix.size()) == rom_prefix)
			result.rom = std::string{arg.substr(rom_prefix.size())};
	}
	return result;
}

/// An absolute path as it is; a relative one inside `games_dir`, never above it: empty for a path
/// with a ".." part, or none.
inline std::string Resolve(std::string_view rom, std::string_view games_dir)
{
	while (!rom.empty() && (rom.back() == ' ' || rom.back() == '\r' || rom.back() == '\n'))
		rom.remove_suffix(1);
	while (!rom.empty() && rom.front() == ' ')
		rom.remove_prefix(1);
	if (rom.empty())
		return {};
	if (rom.front() == '/')
		return std::string{rom};
	for (std::string_view rest = rom; !rest.empty();)
	{
		const size_t slash = rest.find_first_of("/\\");
		if (rest.substr(0, slash) == "..")
			return {};
		if (slash == std::string_view::npos)
			break;
		rest.remove_prefix(slash + 1);
	}
	std::string path{games_dir};
	if (!path.empty() && path.back() != '/')
		path += '/';
	path += rom;
	return path;
}

/// What main() was given (read on the interface's thread after main() has started it).
inline Args& Given()
{
	static Args args;
	return args;
}
} // namespace PS5Launch
