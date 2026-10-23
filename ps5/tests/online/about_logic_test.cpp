// Crux2 - the reading of Wikipedia's and Wikidata's answers about a game (ps5_about_logic.h),
// with answers shaped as theirs are.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+

#include "ps5_about_logic.h"
#include "bigpicture/bp_shown_text.h"

#include <cstdio>

using namespace PS5About;

static int failures = 0, checks = 0;
#define CHECK(x)                                                 \
	do                                                           \
	{                                                            \
		checks++;                                                \
		if (!(x))                                                \
		{                                                        \
			std::printf("FAILED: %s (line %d)\n", #x, __LINE__); \
			failures++;                                          \
		}                                                        \
	} while (0)

int main()
{
	// Titles as the game database has them, as Wikipedia has them.
	CHECK(SearchTitle("Jak and Daxter - The Precursor Legacy") == "Jak and Daxter: The Precursor Legacy");
	CHECK(SearchTitle("Ratchet & Clank (Greatest Hits)") == "Ratchet & Clank");
	CHECK(SearchTitle("Okami [Demo]") == "Okami");
	CHECK(Words("Assassin's Creed: Bloodlines") == (std::vector<std::string>{"assassins", "creed", "bloodlines"}));
	CHECK(Words("Ratchet & Clank") == (std::vector<std::string>{"ratchet", "and", "clank"}));

	CHECK(Fit("Ico", "Ico (video game)") > 0.9);
	CHECK(Fit("Ico", "Icon") == 0);
	CHECK(Fit("Ratchet & Clank", "Ratchet & Clank (2002 video game)") > 0.9);
	CHECK(Fit("Ratchet & Clank", "List of Ratchet & Clank media") == 0);
	CHECK(Fit("Gran Turismo 4", "Gran Turismo 3: A-Spec") == 0);
	CHECK(Fit("Gran Turismo 4", "Gran Turismo 4") == 1);
	CHECK(Fit("Jak and Daxter: The Precursor Legacy", "Jak and Daxter: The Precursor Legacy") == 1);
	CHECK(Fit("Kingdom Hearts", "Kingdom Hearts II: Final Mix") < Fit("Kingdom Hearts", "Kingdom Hearts (video game)"));

	const char* search = R"j({"batchcomplete":"","continue":{"sroffset":3,"continue":"-||"},"query":{"searchinfo":{"totalhits":812},"search":[
		{"ns":0,"title":"List of PlayStation 2 games (A-K)","pageid":1,"size":1,"wordcount":1,"snippet":"..."},
		{"ns":0,"title":"Kingdom Hearts II","pageid":2,"size":1,"wordcount":1,"snippet":"..."},
		{"ns":0,"title":"Kingdom Hearts (video game)","pageid":3,"size":1,"wordcount":1,"snippet":"..."}]}})j";
	CHECK(ChooseArticle(search, "Kingdom Hearts") == "Kingdom Hearts (video game)");
	CHECK(ChooseArticle(search, "Shadow of the Colossus").empty());
	CHECK(ChooseArticle("not json", "Kingdom Hearts").empty());
	CHECK(ChooseArticle(R"j({"query":{"search":[]}})j", "Kingdom Hearts").empty());

	std::string summary, item;
	CHECK(ReadSummary(R"j({"type":"standard","title":"Ico","wikibase_item":"Q1361966","extract":"Ico is a 2001 action-adventure game developed by Team Ico and published by Sony Computer Entertainment for the PlayStation 2."})j",
		&summary, &item));
	CHECK(item == "Q1361966");
	CHECK(summary.rfind("Ico is a 2001", 0) == 0);
	CHECK(!ReadSummary(R"j({"type":"disambiguation","extract":"Ico may refer to:"})j", &summary, &item));
	{
		std::string longText;
		for (int i = 0; i < 40; i++)
			longText += "This is sentence number " + std::to_string(i) + ". ";
		const std::string cut = Sentences(longText, 200);
		CHECK(cut.size() <= 200 && cut.back() == '.');
	}

	const char* claims = R"j({"entities":{"Q1361966":{"type":"item","id":"Q1361966","claims":{
		"P31":[{"mainsnak":{"snaktype":"value","property":"P31","datavalue":{"value":{"entity-type":"item","numeric-id":7889,"id":"Q7889"},"type":"wikibase-entityid"}}}],
		"P178":[{"mainsnak":{"datavalue":{"value":{"id":"Q1047946"},"type":"wikibase-entityid"}}}],
		"P123":[{"mainsnak":{"datavalue":{"value":{"id":"Q1052025"},"type":"wikibase-entityid"}}},{"mainsnak":{"datavalue":{"value":{"id":"Q1052025"}}}}],
		"P136":[{"mainsnak":{"datavalue":{"value":{"id":"Q343568"}}}},{"mainsnak":{"snaktype":"novalue"}}],
		"P404":[{"mainsnak":{"datavalue":{"value":{"id":"Q208850"}}}}],
		"P577":[{"mainsnak":{"datavalue":{"value":{"time":"+2002-03-22T00:00:00Z","precision":11}}}},{"mainsnak":{"datavalue":{"value":{"time":"+2001-09-24T00:00:00Z","precision":11}}}}]
		}}}})j";
	const Claims c = ReadClaims(claims, "Q1361966");
	CHECK(c.videoGame);
	CHECK(c.developers == std::vector<std::string>{"Q1047946"});
	CHECK(c.publishers == std::vector<std::string>{"Q1052025"});
	CHECK(c.genres == std::vector<std::string>{"Q343568"});
	CHECK(c.released == "2001");
	CHECK(!ReadClaims(R"j({"entities":{"Q1":{"claims":{"P31":[{"mainsnak":{"datavalue":{"value":{"id":"Q11424"}}}}]}}}})j", "Q1").videoGame); // a film

	const char* labels = R"j({"entities":{"Q1047946":{"labels":{"en":{"language":"en","value":"Team Ico"}}},"Q343568":{"labels":{"en":{"value":"action-adventure game"}}},"Q9":{"labels":{}}}})j";
	CHECK(ReadLabels(labels, {"Q1047946"}) == "Team Ico");
	CHECK(ReadLabels(labels, {"Q343568"}) == "Action-adventure game");
	CHECK(ReadLabels(labels, {"Q9", "Q1047946"}) == "Team Ico");

	Info info;
	info.found = true;
	info.looked = 1760000000;
	info.article = "Ico";
	info.summary = "Line one.\nLine two with a \\ in it.";
	info.developer = "Team Ico";
	info.released = "2001";
	const Info back = Read(Write(info));
	CHECK(back.found && back.looked == 1760000000 && back.article == "Ico" && back.summary == info.summary && back.developer == "Team Ico" &&
		  back.released == "2001" && back.publisher.empty());
	CHECK(Encode("Ico (video game)") == "Ico%20%28video%20game%29");

	{
		// What the interface's font has: Latin and its punctuation.
		const auto latin = [](char32_t c) { return c < 0x250 || (c >= 0x2000 && c < 0x2070); };
		CHECK(bp::StripUnshown("Tekken 5 (Japanese: \xe9\x89\x84\xe6\x8b\xb3" "5, Hepburn: Tekken Faibu) is a 2004 fighting game.", latin) ==
			  "Tekken 5 is a 2004 fighting game.");
		CHECK(bp::StripUnshown("Ico (video game) is by Team Ico \xe2\x80\x93 a studio.", latin) == "Ico (video game) is by Team Ico \xe2\x80\x93 a studio.");
		CHECK(bp::StripUnshown("Ok\xe9\x89\x84" "ami, a game (\xe5\xa4\xa7 (inner) x).", latin) == "Okami, a game.");
		CHECK(bp::StripUnshown("Caf\xc3\xa9 (unclosed \xe9\x89\x84", latin) == "Caf\xc3\xa9 (unclosed");
	}
	std::printf("about: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
