// Crux2 - the PC test of the game fixes fetched at start (frontend/ps5_fixes_fetch.cpp), with the
// network stood in for by a map of addresses.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_fixes_fetch.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>

using namespace PS5Fixes;

static int g_failed = 0;
#define CHECK(x) \
	do \
	{ \
		if (!(x)) \
		{ \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			g_failed++; \
		} \
	} while (0)

static std::map<std::string, std::string> s_server; // address -> body
static bool s_offline = false;
static int s_requests = 0;

static int Get(const std::string& url, std::vector<uint8_t>* body)
{
	s_requests++;
	if (s_offline)
		return -1;
	const auto it = s_server.find(url);
	if (it == s_server.end())
		return 404;
	body->assign(it->second.begin(), it->second.end());
	return 200;
}

static std::string Line(const std::string& path, const std::string& body)
{
	return Sha256Hex(body.data(), body.size()) + " " + std::to_string(body.size()) + " " + path + "\n";
}

static std::string Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return "<none>";
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

static bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

static bool Names(const Report& r, const char* serial)
{
	for (const std::string& game : r.games)
		if (game == serial)
			return true;
	return false;
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	const std::string dir = argv[1], shipped = dir + "/title/resources/game-fixes", fixes = dir + "/data/game-fixes";
	const std::string LIST = "https://example/main/ps5/game-fixes.txt", FILES = "https://example/main/ps5/package/resources/game-fixes/";
	if (std::system(("rm -rf '" + dir + "' && mkdir -p '" + shipped + "/gamedb' '" + shipped + "/patches' '" + dir + "/data'").c_str()) != 0)
		return 2;

	// SHA-256 itself: the standard's examples.
	CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(Sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	const std::string million(1000000, 'a');
	CHECK(Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	// The paths that may be fetched, and those that may not.
	CHECK(SafePath("gamedb/crux2.yaml") && SafePath("patches/SLUS-20587_ABCDEF01.pnach"));
	CHECK(!SafePath("gamedb/../../eboot.bin") && !SafePath("../gamedb/a.yaml") && !SafePath("eboot.bin"));
	CHECK(!SafePath("gamedb/.hidden.yaml") && !SafePath("gamedb/a.pnach") && !SafePath("patches/a.yaml"));
	CHECK(!SafePath("gamedb/sub/a.yaml") && !SafePath("/gamedb/a.yaml") && !SafePath("gamedb/a b.yaml") && !SafePath("gamedb/"));
	CHECK(!SafePath("fetched.txt") && !SafePath("inis/PCSX2.ini"));

	// The title ships one game database file; GitHub has it changed, and two new files.
	const std::string db_old = "SLUS-20587:\n  name: \"A\"\n";
	const std::string db_new = "SLUS-20587:\n  name: \"A\"\n  gameFixes:\n    - SoftwareRendererFMVHack\nSLES-50876:\n  name: \"A\"\n";
	const std::string kept = "SCUS-97101:\n  name: \"B\"\n";
	const std::string patch = "[60 fps]\npatch=1,EE,00100000,word,00000000\n";
	{
		std::ofstream(shipped + "/gamedb/crux2.yaml") << db_old;
		std::ofstream(shipped + "/gamedb/kept.yaml") << kept;
	}
	s_server[LIST] = std::string(LIST_HEADER) + "\n# a remark\n" + Line("gamedb/crux2.yaml", db_new) + Line("gamedb/kept.yaml", kept) +
		Line("patches/SLUS-20587_ABCDEF01.pnach", patch) + Line("gamedb/../../escape.yaml", "x") + Line("eboot.bin", "x") +
		Line("gamedb/.hidden.yaml", "x") + "not a line\n" + "zz 1 gamedb/z.yaml\n";
	s_server[FILES + "gamedb/crux2.yaml"] = db_new;
	s_server[FILES + "gamedb/kept.yaml"] = kept;
	s_server[FILES + "patches/SLUS-20587_ABCDEF01.pnach"] = patch;
	s_server[FILES + "eboot.bin"] = "x";
	s_server[FILES + "gamedb/../../escape.yaml"] = "x";

	// 1: what differs is fetched into the data folder; the title's own files are not touched; the
	//    file the title has as listed is not asked for; the unsafe lines are refused.
	Report r;
	s_requests = 0;
	CHECK(Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.listed == 3 && r.fetched == 2 && r.removed == 0 && r.failed == 0);
	CHECK(s_requests == 3);
	CHECK(Read(fixes + "/gamedb/crux2.yaml") == db_new);
	CHECK(Read(fixes + "/patches/SLUS-20587_ABCDEF01.pnach") == patch);
	CHECK(!Exists(fixes + "/gamedb/kept.yaml"));
	CHECK(Read(shipped + "/gamedb/crux2.yaml") == db_old);
	CHECK(!Exists(dir + "/data/eboot.bin") && !Exists(fixes + "/eboot.bin") && !Exists(dir + "/escape.yaml") && !Exists(fixes + "/gamedb/.hidden.yaml"));
	CHECK(Names(r, "SLUS-20587") && Names(r, "SLES-50876") && !Names(r, "SCUS-97101"));
	CHECK(Read(fixes + "/fetched.txt") == "gamedb/crux2.yaml\npatches/SLUS-20587_ABCDEF01.pnach\n");

	// 2: nothing changed: only the list is asked for, and nothing is written.
	s_requests = 0;
	CHECK(Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.fetched == 0 && r.removed == 0 && r.games.empty() && s_requests == 1);

	// 3: tampered: a file whose bytes are not the list's is not put in place (the one there stays,
	//    nothing is left beside it), and it is asked for again the next time.
	const std::string patch2 = "[60 fps]\npatch=1,EE,00100000,word,00000001\n";
	s_server[LIST] = std::string(LIST_HEADER) + "\n" + Line("gamedb/crux2.yaml", db_new) + Line("gamedb/kept.yaml", kept) +
		Line("patches/SLUS-20587_ABCDEF01.pnach", patch2);
	s_server[FILES + "patches/SLUS-20587_ABCDEF01.pnach"] = patch2 + "tampered";
	CHECK(!Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.failed == 1 && r.fetched == 0 && !r.error.empty());
	CHECK(Read(fixes + "/patches/SLUS-20587_ABCDEF01.pnach") == patch);
	CHECK(!Exists(fixes + "/patches/SLUS-20587_ABCDEF01.pnach.part") && !Exists(fixes + "/patches/SLUS-20587_ABCDEF01.pnach.old"));
	s_server[FILES + "patches/SLUS-20587_ABCDEF01.pnach"] = patch2;
	CHECK(Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.fetched == 1 && Read(fixes + "/patches/SLUS-20587_ABCDEF01.pnach") == patch2);

	// 4: withdrawn: the patch is no longer listed: the fetched file goes; a file of the fixes folder
	//    that was never fetched (put there by hand) stays; so do the title's own.
	{
		std::ofstream(fixes + "/patches/SCUS-97101_00000001.pnach") << "[own]\n";
	}
	s_server[LIST] = std::string(LIST_HEADER) + "\n" + Line("gamedb/crux2.yaml", db_new) + Line("gamedb/kept.yaml", kept);
	CHECK(Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.removed == 1 && r.fetched == 0 && Names(r, "SLUS-20587"));
	CHECK(!Exists(fixes + "/patches/SLUS-20587_ABCDEF01.pnach"));
	CHECK(Read(fixes + "/patches/SCUS-97101_00000001.pnach") == "[own]\n");
	CHECK(Read(shipped + "/gamedb/kept.yaml") == kept);
	CHECK(Read(fixes + "/fetched.txt") == "gamedb/crux2.yaml\n");

	// 5: no network, a list of another kind, or none at all: nothing changes.
	s_offline = true;
	CHECK(!Sync(LIST, FILES, shipped, fixes, Get, &r) && r.fetched == 0 && r.removed == 0 && !r.error.empty());
	s_offline = false;
	s_server[LIST] = "# Crux2 game fixes 2\n";
	CHECK(!Sync(LIST, FILES, shipped, fixes, Get, &r) && r.fetched == 0 && r.removed == 0);
	s_server.erase(LIST);
	CHECK(!Sync(LIST, FILES, shipped, fixes, Get, &r) && r.removed == 0);
	CHECK(Read(fixes + "/gamedb/crux2.yaml") == db_new);

	// 6: the network lost after the list: the rest is left for the next start, nothing is removed.
	const std::string db_newer = db_new + "SCES-50000:\n  name: \"C\"\n";
	s_server[LIST] = std::string(LIST_HEADER) + "\n" + Line("gamedb/crux2.yaml", db_newer) + Line("gamedb/kept.yaml", kept);
	int answered = 0;
	const Getter lost = [&answered](const std::string& url, std::vector<uint8_t>* body) { return answered++ == 0 ? Get(url, body) : -1; };
	CHECK(!Sync(LIST, FILES, shipped, fixes, lost, &r) && r.fetched == 0 && r.removed == 0 && r.failed >= 1);
	CHECK(Read(fixes + "/gamedb/crux2.yaml") == db_new);

	// 7: an update of Crux2 ships the listed file: the fetched copy is not needed any more and goes.
	s_server[LIST] = std::string(LIST_HEADER) + "\n" + Line("gamedb/crux2.yaml", db_new) + Line("gamedb/kept.yaml", kept);
	{
		std::ofstream(shipped + "/gamedb/crux2.yaml", std::ios::trunc) << db_new;
	}
	s_requests = 0;
	CHECK(Sync(LIST, FILES, shipped, fixes, Get, &r));
	CHECK(r.fetched == 0 && s_requests == 1 && !Exists(fixes + "/gamedb/crux2.yaml"));
	CHECK(Read(fixes + "/fetched.txt") == "");

	std::printf(g_failed ? "game fixes: %d check(s) FAILED\n" : "game fixes: all checks passed\n", g_failed);
	return g_failed ? 1 : 0;
}
