// Crux2 - the thinking behind the card for each game, the texture packs' places and the state
// saved by itself (frontend/ps5_extras_logic.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// With made-up serials: none of them is a game's.

#include "ps5_extras_logic.h"

#include <cstdio>
#include <random>

using namespace PS5Extras;

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static void Keys()
{
	CHECK(CardKey("tsts-10001") == "TSTS-10001");
	CHECK(CardKey("TSTS-100.01") == "TSTS-100.01");
	CHECK(CardKey("").empty());
	CHECK(CardKey("../TSTS-10001").empty());
	CHECK(CardKey("TSTS 10001").empty());
	CHECK(CardKey("TSTS/10001").empty());
	CHECK(CardKey(".hidden").empty());
	CHECK(CardKey("ABCDEFGHIJKLMNOPQRSTUVWXYZ").empty()); // longer than a serial is
	CHECK(CardKey("TSTS-10001\n").empty());
}

static void Groups()
{
	CardGroups groups;
	// A sequel that reads the first part's saves: the first part's list does not say so.
	groups.Add("tsts-20002", {"TSTS-20002", "TSTS-20001"});
	// The two discs of one game, each naming both.
	groups.Add("tsts-30001", {"TSTS-30001", "TSTS-30002"});
	groups.Add("tsts-30002", {"TSTS-30001", "TSTS-30002"});
	// A chain: C reads B, B reads A; and D reads C.
	groups.Add("tsts-40003", {"TSTS-40002"});
	groups.Add("tsts-40002", {"TSTS-40001"});
	groups.Add("tsts-40004", {"TSTS-40003", "not a serial", "../x"});
	// A game that names only itself.
	groups.Add("tsts-50001", {"TSTS-50001"});

	// Both directions give the same card, named by the smallest of the group.
	CHECK(groups.KeyOf("TSTS-20001") == "TSTS-20001");
	CHECK(groups.KeyOf("TSTS-20002") == "TSTS-20001");
	CHECK(groups.KeyOf("tsts-20002") == "TSTS-20001");
	CHECK(groups.KeyOf("TSTS-30001") == "TSTS-30001");
	CHECK(groups.KeyOf("TSTS-30002") == "TSTS-30001");
	for (const char* serial : {"TSTS-40001", "TSTS-40002", "TSTS-40003", "TSTS-40004"})
		CHECK(groups.KeyOf(serial) == "TSTS-40001");
	CHECK(groups.KeyOf("TSTS-50001") == "TSTS-50001");
	// A game no list names has a card of its own.
	CHECK(groups.KeyOf("TSTS-60001") == "TSTS-60001");
	CHECK(groups.KeyOf("bad name").empty());
	// Asking changes nothing.
	CHECK(groups.KeyOf("TSTS-40004") == "TSTS-40001");
	CHECK(groups.Size() == 2 + 2 + 4 + 1);

	// Whatever order the lists come in (the database is a hash map), the groups are the same.
	std::mt19937 random(7);
	for (int round = 0; round < 200; round++)
	{
		// 40 games in groups of 1 to 6, each naming some of its group.
		std::vector<std::pair<std::string, std::vector<std::string>>> lists;
		std::vector<std::string> expected(40);
		int at = 0;
		while (at < 40)
		{
			const int size = std::min(40 - at, 1 + static_cast<int>(random() % 6));
			char first[16];
			std::snprintf(first, sizeof(first), "TSTS-%05d", at);
			for (int i = 0; i < size; i++)
			{
				char name[16];
				std::snprintf(name, sizeof(name), "tsts-%05d", at + i);
				expected[at + i] = first;
				// Each game after the first names one before it: the group hangs together.
				std::vector<std::string> others;
				if (i > 0)
				{
					char other[16];
					std::snprintf(other, sizeof(other), "TSTS-%05d", at + static_cast<int>(random() % i));
					others.emplace_back(other);
				}
				lists.emplace_back(name, std::move(others));
			}
			at += size;
		}
		std::shuffle(lists.begin(), lists.end(), random);
		CardGroups shuffled;
		for (const auto& [serial, others] : lists)
			shuffled.Add(serial, others);
		for (int i = 0; i < 40; i++)
		{
			char name[16];
			std::snprintf(name, sizeof(name), "TSTS-%05d", i);
			CHECK(shuffled.KeyOf(name) == expected[i]);
		}
	}
}

static void Packs()
{
	const std::vector<std::string> found = TexturePackCandidates("TSTS-10001", {"/mnt/usb0", "/smb/games/"});
	CHECK(found.size() == 8);
	if (found.size() == 8)
	{
		// (PSPCSX2/ is the folder of the builds before v0.6.0: a pack put there is still found.)
		CHECK(found[0] == "/mnt/usb0/Crux2/textures/TSTS-10001/replacements");
		CHECK(found[1] == "/mnt/usb0/PSPCSX2/textures/TSTS-10001/replacements");
		CHECK(found[2] == "/mnt/usb0/PS2/textures/TSTS-10001/replacements");
		CHECK(found[3] == "/mnt/usb0/textures/TSTS-10001/replacements");
		CHECK(found[4] == "/smb/games/Crux2/textures/TSTS-10001/replacements");
		CHECK(found[5] == "/smb/games/PSPCSX2/textures/TSTS-10001/replacements");
		CHECK(found[7] == "/smb/games/textures/TSTS-10001/replacements");
	}
	CHECK(TexturePackCandidates("TSTS-10001", {}).empty());
	CHECK(TexturePackCandidates("../../etc", {"/mnt/usb0"}).empty());
	CHECK(TexturePackCandidates("", {"/mnt/usb0"}).empty());
	CHECK(TexturePackCandidates("TSTS-10001", {""}).empty());
}

static void Clock()
{
	PlayClock clock;
	const double every = 120.0;
	// Two minutes of frames at 60 a second: due at the end and not before.
	double now = 1000.0;
	int due_at = -1;
	for (int frame = 0; frame < 60 * 121; frame++)
	{
		now += 1.0 / 60.0;
		if (clock.Tick(now, every) && due_at < 0)
			due_at = frame;
	}
	CHECK(due_at >= 60 * 120 - 2 && due_at <= 60 * 120 + 2);
	// Until it is saved it stays due; afterwards it counts from nothing.
	CHECK(clock.Tick(now += 1.0 / 60.0, every));
	clock.Saved();
	CHECK(!clock.Tick(now += 1.0 / 60.0, every));
	CHECK(clock.Played() < 0.1);

	// A pause of ten minutes in the middle adds nothing.
	clock.Reset();
	for (int frame = 0; frame < 60 * 60; frame++)
		CHECK(!clock.Tick(now += 1.0 / 60.0, every));
	now += 600.0;
	CHECK(!clock.Tick(now, every));
	CHECK(clock.Played() > 59.0 && clock.Played() < 61.0);
	for (int frame = 0; frame < 60 * 59; frame++)
		CHECK(!clock.Tick(now += 1.0 / 60.0, every));
	bool due = false;
	for (int frame = 0; frame < 60 * 2; frame++)
		due |= clock.Tick(now += 1.0 / 60.0, every);
	CHECK(due);

	// A card was busy: asked again five seconds of play later, not at once and not in two minutes.
	clock.Later(5.0, every);
	CHECK(!clock.Tick(now += 1.0 / 60.0, every));
	due = false;
	int frames = 0;
	while (!due && frames < 60 * 10)
	{
		due = clock.Tick(now += 1.0 / 60.0, every);
		frames++;
	}
	CHECK(due && frames >= 60 * 5 - 3 && frames <= 60 * 5 + 3);

	// Off is never; and a clock that runs backwards (it does not) adds nothing.
	clock.Reset();
	for (int frame = 0; frame < 60 * 600; frame++)
		CHECK(!clock.Tick(now += 1.0 / 60.0, 0.0));
	const double before = clock.Played();
	clock.Tick(now - 50.0, 0.0);
	CHECK(clock.Played() == before);
	// A slow game (20 frames a second) is played as long as a quick one.
	clock.Reset();
	due = false;
	frames = 0;
	while (!due && frames < 20 * 200)
	{
		due = clock.Tick(now += 1.0 / 20.0, every);
		frames++;
	}
	CHECK(due && frames >= 20 * 120 - 2 && frames <= 20 * 120 + 2);
}

int main()
{
	Keys();
	Groups();
	Packs();
	Clock();
	std::printf("extras logic: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
