// Crux2 - recommended settings per game (frontend/ps5_recommend.cpp) on the PC: the file read and
// what is refused from it, the download at most once a day over a stand-in for the network, the copy
// kept, and the repository's own ps5/recommended.ini.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   recommend_test <work folder> <ps5/recommended.ini>

#include "ps5_recommend.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

static std::string ReadText(const std::string& path)
{
	std::string out;
	if (FILE* const fp = std::fopen(path.c_str(), "rb"))
	{
		char block[4096];
		size_t got;
		while ((got = std::fread(block, 1, sizeof(block), fp)) > 0)
			out.append(block, got);
		std::fclose(fp);
	}
	return out;
}

int main(int argc, char** argv)
{
	if (argc < 3)
		return 2;
	const std::string work = argv[1];
	mkdir(work.c_str(), 0777);
	using namespace PS5Recommend;

	const std::string feed = std::string(HEADER) + "\r\n"
		"# a comment\n"
		"[slus-00001]\n"
		"note = Steady at 3x.\n"
		"EmuCore/GS/upscale_multiplier = 3\n"
		"EmuCore/GS/upscale_multiplier = 4\n" // the last counts
		"EmuCore/Speedhacks/vuThread = true\n"
		"Framerate/NominalScalar = 1.0\n"
		"EmuCore/GS/UserHacks_HalfPixelOffset=1\n"
		"Achievements/Token = stolen\n"     // refused: not an emulation setting
		"Folders/Bios = /somewhere\n"       // refused
		"PS5/UpdateRepository = x/y\n"      // refused
		"EmuCore/../GS/x = 1\n"             // refused
		"EmuCore/GS/bad key = 1\n"          // refused
		"no equals sign\n"                  // refused
		"[SCES-00002]\n"
		"note = only a note\n"              // no entry
		"[SLES-00003]\n"
		"EmuCore/CPU/Recompiler/EnableEE = false\n";
	std::map<std::string, Pick> picks;
	unsigned refused = 0;
	CHECK(Parse(feed, &picks, &refused));
	CHECK(picks.size() == 2);
	CHECK(refused == 6);
	const auto first = picks.find("SLUS-00001");
	CHECK(first != picks.end());
	if (first != picks.end())
	{
		CHECK(first->second.note == "Steady at 3x.");
		CHECK(first->second.values.size() == 4);
		CHECK(first->second.values[0].section == "EmuCore/GS" && first->second.values[0].key == "upscale_multiplier" &&
			  first->second.values[0].value == "4");
		CHECK(first->second.values[2].section == "Framerate" && first->second.values[2].key == "NominalScalar");
		CHECK(first->second.values[3].key == "UserHacks_HalfPixelOffset" && first->second.values[3].value == "1");
	}
	CHECK(picks.count("SCES-00002") == 0);
	CHECK(picks.count("SLES-00003") == 1);
	CHECK(!Parse("[SLUS-00001]\nEmuCore/GS/x = 1\n", &picks)); // no header: not such a file
	CHECK(SafeSetting("EmuCore", "EnableCheats"));
	CHECK(!SafeSetting("EmuCore/", "x"));
	CHECK(!SafeSetting("EmuCoreX", "x"));
	CHECK(!SafeSetting("InputSources", "SDL"));

	// The repository's own file is such a file, with no entry yet.
	{
		std::map<std::string, Pick> shipped;
		CHECK(Parse(ReadText(argv[2]), &shipped));
		CHECK(shipped.empty());
	}

	// The download: once, then not again within a day; again after one; refused when it is not the
	// file; the copy kept as it was then.
	const std::string cache = work + "/recommended.ini", checked = work + "/recommended.checked";
	unlink(cache.c_str());
	unlink(checked.c_str());
	int asked = 0;
	std::string serve = feed;
	int status = 200;
	const PS5Fixes::Getter get = [&](const std::string& url, std::vector<uint8_t>* body) {
		asked++;
		CHECK(url == "https://raw.example/ps5/recommended.ini");
		body->assign(serve.begin(), serve.end());
		return status;
	};
	const time_t day = 24 * 60 * 60, t0 = 1800000000;
	std::string why;
	CHECK(Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0, get, &why));
	CHECK(asked == 1);
	CHECK(ReadText(cache) == feed);
	Pick pick;
	CHECK(For(cache, "slus-00001", &pick) && pick.values.size() == 4);
	CHECK(!For(cache, "SLUS-99999", &pick));

	CHECK(!Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0 + day - 1, get, &why));
	CHECK(asked == 1);
	CHECK(!Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0 + day, get, &why)); // the same file
	CHECK(asked == 2);
	CHECK(why == "the same as the copy kept");

	serve = "<html>not found</html>";
	CHECK(!Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0 + 2 * day, get, &why));
	CHECK(asked == 3);
	CHECK(ReadText(cache) == feed);
	status = 404;
	CHECK(!Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0 + 3 * day, get, &why));
	CHECK(why == "the answer was 404");
	// A clock set back is no reason to wait.
	status = 200;
	serve = std::string(HEADER) + "\n[SLUS-00004]\nEmuCore/GS/upscale_multiplier = 2\n";
	CHECK(Refresh("https://raw.example/ps5/recommended.ini", cache, checked, t0, get, &why));
	CHECK(asked == 5);
	sleep(1); // (the cache is told apart by its size and time)
	CHECK(For(cache, "SLUS-00004", &pick) && pick.values.size() == 1 && pick.values[0].value == "2");
	CHECK(!For(cache, "SLUS-00001", &pick));

	if (s_failures == 0)
		std::printf("recommended settings: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
