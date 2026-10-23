// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the plain part of the cheat downloads, on a PC, with invented games.
//   cheats_logic_test [the real cheat-index.txt]
#include "ps5_cheats_logic.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static int g_failed = 0, g_checks = 0;
#define EXPECT(c, what) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAILED: %s (line %d)\n", what, __LINE__); } } while (0)

int main(int argc, char** argv)
{
	using namespace PS5Cheats;
	const Index index = ParseIndex("# a note\n@aa some/collection main 0123 2026-01-01\n@bb other/one dev\n@broken\n"
								   "aa\tcheats/SLUS-29001_0123ABCD.pnach\r\n"
								   "aa\tcheats/0123abcd.pnach\n"
								   "aa\tcheats/A0123ABCD.pnach\n"
								   "aa\tcheats/0123ABCDE.pnach\n"
								   "bb\tA Game (USA)/SLUS-29001_0123ABCD_Infinite Health & More.pnach\n"
								   "bb\tA Game (USA)/SLUS-29001_0123ABCD_Second.pnach\n"
								   "zz\tnot/a/source.pnach\n"
								   "aa\tcheats/SLES-59003_89ABCDEF.pnach\n");
	EXPECT(index.sources.size() == 2 && index.files.size() == 7, "the list is read; a file of no collection is left out");
	const std::vector<Wanted> w = FilesFor(index, "SLUS-29001", 0x0123ABCD);
	EXPECT(w.size() == 4, "a game's files are those with its CRC as a word of its own");
	if (w.size() == 4)
	{
		EXPECT(w[0].url == "https://raw.githubusercontent.com/some/collection/main/cheats/SLUS-29001_0123ABCD.pnach", "the address of a file");
		EXPECT(w[0].save_name == "SLUS-29001_0123ABCD_github-aa.pnach" && w[1].save_name == "SLUS-29001_0123ABCD_github-aa-2.pnach", "the names they are saved as");
		EXPECT(w[2].url == "https://raw.githubusercontent.com/other/one/dev/A%20Game%20%28USA%29/SLUS-29001_0123ABCD_Infinite%20Health%20%26%20More.pnach",
			"an address with spaces and signs in it");
		EXPECT(w[3].save_name == "SLUS-29001_0123ABCD_github-bb-2.pnach", "two files of one collection");
	}
	EXPECT(FilesFor(index, "SLUS-29001", 0).empty() && FilesFor(index, "X", 0x11111111).empty(), "no CRC, or one nobody has: nothing");
	EXPECT(FilesFor(index, "", 0x89ABCDEF).size() == 1 && FilesFor(index, "", 0x89ABCDEF)[0].save_name == "89ABCDEF_github-aa.pnach", "a game without a serial");

	std::string why;
	EXPECT(LooksLikePnach("gametitle=A Game\n\n[Infinite Health]\nauthor=someone\npatch=1,EE,00100000,extended,00000001\n", &why), "a cheat file is one");
	EXPECT(!LooksLikePnach("<!DOCTYPE html><html><body>404: Not Found</body></html>", &why) && !why.empty(), "a page is not");
	EXPECT(!LooksLikePnach(std::string_view("patch=1\0,EE,0", 13), &why), "nor is something with a zero byte in it");
	EXPECT(!LooksLikePnach("404: Not Found", &why), "nor GitHub's answer for a file that is not there");
	EXPECT(!LooksLikePnach(std::string(600 * 1024, 'p'), &why), "nor something far too large");

	const std::vector<std::string> patterns = ParsePatterns(std::string(DefaultPatternsFile()) +
		"https://raw.githubusercontent.com/me/mine/main/{serial}_{CRC}.pnach\nhttp://plain.example/{crc}.pnach\nhttps://no-game-in-it.example/x.pnach\n");
	EXPECT(patterns.size() == 1, "the user's addresses: https, with the game in them; the file's own notes are not addresses");
	EXPECT(ExpandPattern("https://x.example/{serial}/{CRC}-{crc}.pnach", "SLUS-29001", 0x0123ABCD) == "https://x.example/SLUS-29001/0123ABCD-0123abcd.pnach",
		"a pattern with the game put in");

	// ---- a cheat typed in on the console
	EXPECT(TypedCodeLine("2034A5B0 00000001") == "patch=1,EE,2034A5B0,extended,00000001", "a code line as the lists print it");
	EXPECT(TypedCodeLine("2034a5b0:0000ffff") == "patch=1,EE,2034A5B0,extended,0000FFFF", "small letters and a colon");
	EXPECT(TypedCodeLine("  2034A5B000000001 ") == "patch=1,EE,2034A5B0,extended,00000001", "nothing between the groups");
	EXPECT(TypedCodeLine("2034A5B0 0000001").empty() && TypedCodeLine("2034A5B0 000000011").empty(), "fifteen or seventeen digits are no code");
	EXPECT(TypedCodeLine("2034A5G0 00000001").empty() && TypedCodeLine("").empty() && TypedCodeLine("patch=1,EE,0,extended,0").empty(),
		"nor is anything that is not hexadecimal");
	EXPECT(TypedCheatName("  Infinite [health]  ") == "Infinite  health", "a name loses what a section cannot carry");
	EXPECT(TypedCheatName("a\\b=c\n") == "a b c" && TypedCheatName(" [] ").empty(), "a backslash, an equals sign and a line end too");
	EXPECT(TypedCheatName(std::string(200, 'x')).size() == 60, "and is sixty characters at most");
	{
		const std::vector<std::string> one = {TypedCodeLine("2034A5B0 00000001"), TypedCodeLine("1034A5B4 0000FFFF")};
		std::string file = WithTypedCheat("", "Infinite health", one);
		std::string why;
		EXPECT(LooksLikePnach(file, &why), "a file with a typed cheat is a cheat file");
		EXPECT(file.find("[Infinite health]\ndescription=Typed in on the console\npatch=1,EE,2034A5B0,extended,00000001\npatch=1,EE,1034A5B4,extended,0000FFFF\n") !=
				   std::string::npos,
			"its name, and its lines in their order");
		file = WithTypedCheat(file, "All weapons", {TypedCodeLine("0034A5B8 000000FF")});
		EXPECT((CheatNames(file) == std::vector<std::string>{"Infinite health", "All weapons"}), "a second one follows the first");
		file = WithTypedCheat(file, "Infinite health", {TypedCodeLine("2034A5B0 00000063")});
		EXPECT((CheatNames(file) == std::vector<std::string>{"All weapons", "Infinite health"}), "one of the same name is replaced, not doubled");
		EXPECT(file.find("00000001") == std::string::npos && file.find("00000063") != std::string::npos, "by its new lines");
		file = WithoutCheat(file, "All weapons");
		EXPECT((CheatNames(file) == std::vector<std::string>{"Infinite health"}) && file.find("0034A5B8") == std::string::npos, "one is taken out with its lines");
		EXPECT(file.find("\n\n\n") == std::string::npos, "and leaves no run of empty lines");
		EXPECT(WithoutCheat(file, "Not there") == file || WithoutCheat(file, "Not there") + "\n" == file || WithoutCheat(file, "Not there") == file + "\n",
			"taking out one that is not there changes nothing");
	}
	EXPECT(TypedFileName("SLUS-29001", 0x0123ABCD) == "SLUS-29001_0123ABCD_typed.pnach", "the file typed cheats are kept in");
	EXPECT(NameHasCRC(TypedFileName("SLUS-29001", 0x0123ABCD), 0x0123ABCD), "which a game finds by its CRC");

	// ---- names that lost their CRC, a collection's patches, files repaired on the way in
	{
		const Index lost = ParseIndex("@xs a/b main\n@hj c/d main\n"
									  "xs\tcheats/SCUS-97203_6441001.pnach\n"
									  "xs\tcheats/SLUS-21042_3.27E+13.pnach\n"
									  "xs\tcheats/SLUS-21042_11111111.pnach\n"
									  "hj\tpatches/SLUS-20487_55AC9791.pnach\n"
									  "hj\tcheats/SLUS-20487_55AC9791.pnach\n");
		EXPECT(FilesFor(lost, "SCUS-97203", 0x06441001).size() == 1, "a name whose CRC lost its first digit is the game's by its serial");
		EXPECT(FilesFor(lost, "SCUS-97203", 0x06441001)[0].save_name == "SCUS-97203_06441001_github-xs.pnach", "saved under the game's own CRC");
		const std::vector<Wanted> dark = FilesFor(lost, "SLUS-21042", 0x327053E8);
		EXPECT(dark.size() == 1 && dark[0].url.find("3.27E%2B13") != std::string::npos, "a CRC a spreadsheet wrote as a number: the serial; a file of another CRC is not");
		EXPECT(FilesFor(lost, "SLUS-21042", 0x11111111).size() == 1, "a collection with a file named for the CRC gives nothing by the serial");
		EXPECT(FilesFor(lost, "SCUS-97204", 0x06441001).empty() && FilesFor(lost, "SCUS-9720", 0x1).empty(), "another serial, or a part of one, is not");
		const std::vector<Wanted> both = FilesFor(lost, "SLUS-20487", 0x55AC9791);
		EXPECT(both.size() == 2 && both[0].patch && !both[1].patch, "a collection's patches/ are patches, its cheats/ cheats");
		EXPECT(IsPatchPath("Patches/x.pnach") && !IsPatchPath("patches/") && !IsPatchPath("cheats/patches/x.pnach"), "the patches folder is the first part");

		int joined = 0;
		const std::string broken = "// Mercenaries\n[Freeze Mission Timer]\npatch=1,EE,202A0B94,extended,AC40002C\n\n"
								   "[Infinite Ammo (Press Select\nup To Activate\nselect\ndown To Stop)]\npatch=1,EE,D0000000,extended,0\n";
		const std::string fixed = RepairPnach(broken, &joined);
		EXPECT(joined == 1, "one name broken over lines");
		EXPECT(fixed.find("\n[Infinite Ammo (Press Select up To Activate select down To Stop)]\npatch=1,EE,D0000000") != std::string::npos,
			"joined into one name, the cheat's lines after it");
		EXPECT(CheatNames(fixed).size() == 2, "so it is a cheat of its own, not part of the one before");
		EXPECT(RepairPnach("\xEF\xBB\xBF[A]\npatch=1,EE,0,word,1", &joined) == "[A]\npatch=1,EE,0,word,1" && joined == 0, "a byte order mark goes");
		const std::string unclosed = "[Never closed\npatch=1,EE,0,word,1\n[B]\n";
		EXPECT(RepairPnach(unclosed, &joined) == "[Never closed\npatch=1,EE,0,word,1\n[B]" && joined == 0, "a name never closed is left as it was");
		EXPECT(RepairPnach("[A]\r\npatch=1,EE,0,word,1\r\n", &joined) == "[A]\r\npatch=1,EE,0,word,1\r", "lines otherwise as they came");
	}

	if (argc > 1)
	{
		std::ifstream f(argv[1]);
		std::stringstream text;
		text << f.rdbuf();
		const Index real = ParseIndex(text.str());
		size_t with_crc = 0;
		for (const Index::File& file : real.files)
		{
			// (Every file of the list should be one a game can find: a CRC in its name.)
			const size_t slash = file.path.rfind('/');
			const std::string name = file.path.substr(slash == std::string::npos ? 0 : slash + 1);
			bool found = false;
			for (size_t at = 0; at + 8 <= name.size() && !found; at++)
			{
				bool hex = true;
				for (size_t i = 0; i < 8; i++)
					hex = hex && IsHexDigit(name[at + i]);
				found = hex && (at == 0 || !IsHexDigit(name[at - 1])) && (at + 8 == name.size() || !IsHexDigit(name[at + 8]));
			}
			with_crc += found ? 1 : 0;
			// (The few without one are found by their serial, when the name has it.)
		}
		std::printf("the title's list: %zu collections, %zu files, %zu of them findable by a CRC\n", real.sources.size(), real.files.size(), with_crc);
		EXPECT(real.sources.size() >= 2 && real.files.size() > 500 && with_crc * 100 >= real.files.size() * 95, "the title's own list is usable");
	}
	std::printf("cheats logic: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
