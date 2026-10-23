// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the matching of games to libretro's box-art names, measured: every game of PCSX2's
// own database is looked up in the list of names.
//
//   covers_match_test <boxarts.txt> <GameIndex.yaml> [show]
//
// Prints how many games found a picture, by the way they were matched and by region; with
// "show", also a sample of the matches that were not exact, for reading (on one's own screen:
// it names games).
#include "ps5_covers_match.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static std::string unquote(std::string v)
{
	while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
		v.pop_back();
	if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
	{
		v = v.substr(1, v.size() - 2);
		std::string out;
		for (size_t i = 0; i < v.size(); i++)
		{
			if (v[i] == '\\' && i + 1 < v.size())
				i++;
			out += v[i];
		}
		return out;
	}
	return v;
}

int main(int argc, char** argv)
{
	if (argc < 3)
		return 2;
	std::ifstream list(argv[1]);
	std::stringstream text;
	text << list.rdbuf();
	PS5Covers::Index index;
	index.Build(PS5Covers::ParseNameList(text.str()));
	std::printf("box-art names: %zu\n", index.Size());

	struct Game
	{
		std::string serial, name, name_en, region;
	};
	std::vector<Game> games;
	std::ifstream db(argv[2]);
	std::string line;
	while (std::getline(db, line))
	{
		if (!line.empty() && line[0] != ' ' && line[0] != '#' && line.find(':') != std::string::npos)
			games.push_back({line.substr(0, line.find(':')), "", "", ""});
		else if (!games.empty() && line.rfind("  name: ", 0) == 0)
			games.back().name = unquote(line.substr(8));
		else if (!games.empty() && line.rfind("  name-en: ", 0) == 0)
			games.back().name_en = unquote(line.substr(11));
		else if (!games.empty() && line.rfind("  region: ", 0) == 0)
			games.back().region = unquote(line.substr(10));
	}
	std::printf("games in the database: %zu\n", games.size());

	std::map<std::string, std::pair<int, int>> by_region; // found, all
	int kinds[5] = {}, own = 0, shown[5] = {};
	const bool show = argc > 3;
	for (size_t i = 0; i < games.size(); i++)
	{
		const Game& g = games[i];
		PS5Covers::Game query;
		query.title = g.name;
		query.title_en = g.name_en;
		query.region = g.region;
		const PS5Covers::Match m = index.Find(query);
		kinds[static_cast<int>(m.kind)]++;
		own += m.own_region ? 1 : 0;
		const std::string market = g.region.substr(0, g.region.find('-') == std::string::npos ? g.region.size() : g.region.find('-') + 2);
		auto& count = by_region[market];
		count.second++;
		count.first += (m.index >= 0) ? 1 : 0;
		const int k = static_cast<int>(m.kind);
		if (show && (k >= 2 ? (i % 7 == 3 && shown[k] < 30) : (k == 0 && i % 211 == 5 && shown[0] < 25)))
		{
			shown[k]++;
			std::printf("  [%s] %s | %s | %s -> %s\n", PS5Covers::MatchKindName(m.kind), g.serial.c_str(), g.region.c_str(), g.name.c_str(),
				m.index >= 0 ? index.Name(m.index).c_str() : "-");
		}
	}
	const int found = static_cast<int>(games.size()) - kinds[0];
	std::printf("found a picture: %d of %zu (%.1f%%); of its own region: %d\n", found, games.size(), 100.0 * found / games.size(), own);
	std::printf("  by the same title: %d, but for a subtitle: %d, but for a letter or two: %d\n", kinds[2], kinds[3], kinds[4]);
	for (const auto& [market, count] : by_region)
		if (count.second >= 50)
			std::printf("  %-8s %5d of %5d (%.0f%%)\n", market.c_str(), count.first, count.second, 100.0 * count.first / count.second);
	return found * 2 > static_cast<int>(games.size()) ? 0 : 1;
}
