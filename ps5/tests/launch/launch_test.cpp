// Crux2 - the launch arguments (frontend/ps5_launch.h): --rom and --exit-after-game read, and the
// game's path made from them.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_launch.h"

#include <cstdio>

static int s_failures = 0;
#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static PS5Launch::Args Parse(std::initializer_list<const char*> list)
{
	static char storage[16][128];
	static char* argv[17];
	int argc = 0;
	for (const char* arg : list)
	{
		std::snprintf(storage[argc], sizeof(storage[argc]), "%s", arg);
		argv[argc] = storage[argc];
		argc++;
	}
	argv[argc] = nullptr;
	return PS5Launch::Parse(argc, argv);
}

int main()
{
	using PS5Launch::Resolve;

	PS5Launch::Args a = Parse({"eboot.bin"});
	CHECK(a.rom.empty() && !a.exit_after_game);
	a = Parse({"eboot.bin", "--rom", "/mnt/usb0/games/Game.iso"});
	CHECK(a.rom == "/mnt/usb0/games/Game.iso" && !a.exit_after_game);
	a = Parse({"eboot.bin", "--rom=PS2/Game.chd", "--exit-after-game"});
	CHECK(a.rom == "PS2/Game.chd" && a.exit_after_game);
	a = Parse({"eboot.bin", "--exit-after-game", "--something", "--rom"}); // --rom with nothing after it
	CHECK(a.rom.empty() && a.exit_after_game);
	a = Parse({"eboot.bin", "--rom", "First.iso", "--rom=Second.iso"}); // the last one counts
	CHECK(a.rom == "Second.iso");
	CHECK(PS5Launch::Parse(0, nullptr).rom.empty());

	CHECK(Resolve("/mnt/usb0/games/Game.iso", "/app0/games") == "/mnt/usb0/games/Game.iso");
	CHECK(Resolve("/smb/nas/Game.chd", "/app0/games") == "/smb/nas/Game.chd");
	CHECK(Resolve("Game.iso", "/app0/games") == "/app0/games/Game.iso");
	CHECK(Resolve("Game.iso", "/app0/games/") == "/app0/games/Game.iso");
	CHECK(Resolve("PS2/Game.iso \r\n", "/app0/games") == "/app0/games/PS2/Game.iso");
	CHECK(Resolve("../eboot.bin", "/app0/games").empty());
	CHECK(Resolve("PS2/../../x", "/app0/games").empty());
	CHECK(Resolve("PS2\\..\\x", "/app0/games").empty());
	CHECK(Resolve("..hidden/x.iso", "/app0/games") == "/app0/games/..hidden/x.iso");
	CHECK(Resolve("   ", "/app0/games").empty());

	if (s_failures == 0)
		std::printf("launch arguments: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
