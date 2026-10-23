// Crux2 - what a game is, from Wikipedia and Wikidata: the reading of their answers and the
// choice of the article, with no network in it (the PC tests check it; ps5_about.cpp asks).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The text shown is Wikipedia's (CC BY-SA 4.0): it is fetched on the console when a game is looked
// at, kept in the title's own data, never shipped, and the screen says where it is from.

#pragma once

#include "rapidjson/document.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PS5About
{
	struct Info
	{
		bool found = false; ///< an article of the game was found
		std::string article; ///< its title on Wikipedia
		std::string summary; ///< its first paragraph or two
		std::string developer, publisher, released, genre, modes;
		std::int64_t looked = 0; ///< when it was looked up (seconds since 1970)
	};

	// ---- titles

	/// The words of a title, lower case, without punctuation: "Jak & Daxter - The Precursor
	/// Legacy" -> {"jak", "and", "daxter", "the", "precursor", "legacy"}. Roman numerals and
	/// numbers stay words of their own.
	inline std::vector<std::string> Words(std::string_view title)
	{
		std::vector<std::string> words;
		std::string word;
		const auto flush = [&] {
			if (!word.empty())
				words.push_back(word == "&" ? std::string("and") : word);
			word.clear();
		};
		for (char ch : title)
		{
			const unsigned char c = static_cast<unsigned char>(ch);
			if (std::isalnum(c) || c >= 0x80)
				word += static_cast<char>(std::tolower(c));
			else if (c == '&')
			{
				flush();
				word = "&";
				flush();
			}
			else if (c == '\'')
				continue; // "Assassin's" is one word
			else
				flush();
		}
		flush();
		return words;
	}

	/// The title as Wikipedia would have it: " - " is ": ", and what the game database adds in
	/// brackets ("(Greatest Hits)", "[Demo]") goes.
	inline std::string SearchTitle(std::string title)
	{
		for (const char* const open : {"(", "["})
		{
			const size_t at = title.find(open);
			if (at != std::string::npos && at > 0)
				title.resize(at);
		}
		for (size_t at = title.find(" - "); at != std::string::npos; at = title.find(" - "))
			title.replace(at, 3, ": ");
		while (!title.empty() && (title.back() == ' ' || title.back() == ':'))
			title.pop_back();
		return title;
	}

	/// How well an article's title fits the game's: 0 (not at all) to 1. The words of the game's
	/// title must all be in the article's (a few small ones aside), and the article's few more.
	inline double Fit(std::string_view game, std::string_view article)
	{
		std::string a(article);
		// "Ico (video game)": what is in brackets says what it is, not its name.
		const size_t bracket = a.find(" (");
		const bool says_game = bracket != std::string::npos && a.find("game", bracket) != std::string::npos;
		if (bracket != std::string::npos)
			a.resize(bracket);
		if (a.rfind("List of", 0) == 0)
			return 0;
		const std::vector<std::string> g = Words(game), w = Words(a);
		if (g.empty() || w.empty())
			return 0;
		static const char* const small[] = {"the", "a", "an", "of", "and", "in", "on", "to", "for"};
		const auto isSmall = [](const std::string& s) { return std::find(std::begin(small), std::end(small), s) != std::end(small); };
		int missing = 0, matched = 0;
		for (const std::string& word : g)
		{
			if (std::find(w.begin(), w.end(), word) != w.end())
				matched++;
			else if (!isSmall(word))
				missing++;
		}
		if (missing > 0 || matched == 0)
			return 0;
		int extra = 0;
		for (const std::string& word : w)
		{
			if (std::find(g.begin(), g.end(), word) == g.end() && !isSmall(word))
				extra++;
		}
		double fit = 1.0 - 0.15 * extra;
		if (says_game)
			fit += 0.05;
		return std::clamp(fit, 0.0, 1.0);
	}

	// ---- the answers

	/// Wikipedia's search (action=query&list=search): the article that fits the game best, or
	/// empty when none fits well enough.
	inline std::string ChooseArticle(std::string_view json, std::string_view game)
	{
		rapidjson::Document doc;
		doc.Parse(json.data(), json.size());
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("query") || !doc["query"].IsObject())
			return {};
		const rapidjson::Value& query = doc["query"];
		if (!query.HasMember("search") || !query["search"].IsArray())
			return {};
		std::string best;
		double bestFit = 0.55;
		for (const rapidjson::Value& hit : query["search"].GetArray())
		{
			if (!hit.IsObject() || !hit.HasMember("title") || !hit["title"].IsString())
				continue;
			const std::string title = hit["title"].GetString();
			const double fit = Fit(game, title);
			if (fit > bestFit)
			{
				bestFit = fit;
				best = title;
			}
		}
		return best;
	}

	/// The first sentences of a text, up to about `most` characters, ending at a sentence's end.
	inline std::string Sentences(std::string text, size_t most)
	{
		for (char& c : text)
		{
			if (c == '\n')
				c = ' ';
		}
		if (text.size() <= most)
			return text;
		size_t cut = std::string::npos;
		for (size_t at = text.find(". "); at != std::string::npos && at < most; at = text.find(". ", at + 1))
			cut = at + 1;
		if (cut == std::string::npos)
		{
			cut = text.rfind(' ', most);
			if (cut == std::string::npos)
				cut = most;
			return text.substr(0, cut) + "...";
		}
		return text.substr(0, cut);
	}

	/// The summary of an article (/api/rest_v1/page/summary): its text and its Wikidata item.
	/// False for a disambiguation page or an answer that is not one.
	inline bool ReadSummary(std::string_view json, std::string* summary, std::string* item)
	{
		rapidjson::Document doc;
		doc.Parse(json.data(), json.size());
		if (doc.HasParseError() || !doc.IsObject())
			return false;
		if (doc.HasMember("type") && doc["type"].IsString() && std::string_view(doc["type"].GetString()) == "disambiguation")
			return false;
		if (!doc.HasMember("extract") || !doc["extract"].IsString())
			return false;
		*summary = Sentences(doc["extract"].GetString(), 620);
		if (doc.HasMember("wikibase_item") && doc["wikibase_item"].IsString())
			*item = doc["wikibase_item"].GetString();
		return !summary->empty();
	}

	struct Claims
	{
		bool videoGame = false; ///< it is an instance of a video game (or there was no "instance of")
		std::vector<std::string> developers, publishers, genres, modes; ///< item ids ("Q123")
		std::string released; ///< the earliest year of publication
	};

	/// An item's claims (wbgetentities&props=claims): what is shown of a game.
	inline Claims ReadClaims(std::string_view json, const std::string& item)
	{
		Claims out;
		rapidjson::Document doc;
		doc.Parse(json.data(), json.size());
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("entities") || !doc["entities"].IsObject())
			return out;
		const rapidjson::Value& entities = doc["entities"];
		if (!entities.HasMember(item.c_str()) || !entities[item.c_str()].IsObject())
			return out;
		const rapidjson::Value& entity = entities[item.c_str()];
		if (!entity.HasMember("claims") || !entity["claims"].IsObject())
			return out;
		const rapidjson::Value& claims = entity["claims"];
		const auto values = [&](const char* property, std::vector<std::string>* ids, size_t most) {
			if (!claims.HasMember(property) || !claims[property].IsArray())
				return;
			for (const rapidjson::Value& claim : claims[property].GetArray())
			{
				if (!claim.IsObject() || !claim.HasMember("mainsnak") || !claim["mainsnak"].IsObject())
					continue;
				const rapidjson::Value& snak = claim["mainsnak"];
				if (!snak.HasMember("datavalue") || !snak["datavalue"].IsObject())
					continue;
				const rapidjson::Value& dv = snak["datavalue"];
				if (!dv.HasMember("value") || !dv["value"].IsObject() || !dv["value"].HasMember("id") || !dv["value"]["id"].IsString())
					continue;
				const std::string id = dv["value"]["id"].GetString();
				if (std::find(ids->begin(), ids->end(), id) == ids->end() && ids->size() < most)
					ids->push_back(id);
			}
		};
		std::vector<std::string> kinds;
		values("P31", &kinds, 16);
		// Video game (Q7889), and the kinds a game's item is sometimes said to be instead.
		static const char* const games[] = {"Q7889", "Q4393107", "Q21125433", "Q865493", "Q1755420", "Q16070115"};
		out.videoGame = kinds.empty();
		for (const std::string& k : kinds)
		{
			if (std::find(std::begin(games), std::end(games), k) != std::end(games))
				out.videoGame = true;
		}
		values("P178", &out.developers, 2);
		values("P123", &out.publishers, 2);
		values("P136", &out.genres, 3);
		values("P404", &out.modes, 2);
		if (claims.HasMember("P577") && claims["P577"].IsArray())
		{
			for (const rapidjson::Value& claim : claims["P577"].GetArray())
			{
				if (!claim.IsObject() || !claim.HasMember("mainsnak") || !claim["mainsnak"].IsObject())
					continue;
				const rapidjson::Value& snak = claim["mainsnak"];
				if (!snak.HasMember("datavalue") || !snak["datavalue"].IsObject() || !snak["datavalue"].HasMember("value") ||
					!snak["datavalue"]["value"].IsObject() || !snak["datavalue"]["value"].HasMember("time") ||
					!snak["datavalue"]["value"]["time"].IsString())
					continue;
				// "+2001-12-03T00:00:00Z"
				const std::string time = snak["datavalue"]["value"]["time"].GetString();
				if (time.size() >= 5 && std::isdigit(static_cast<unsigned char>(time[1])))
				{
					const std::string year = time.substr(1, 4);
					if (out.released.empty() || year < out.released)
						out.released = year;
				}
			}
		}
		return out;
	}

	/// The English names of items (wbgetentities&props=labels&languages=en), joined: "Naughty
	/// Dog, Sony". Items without one are left out.
	inline std::string ReadLabels(std::string_view json, const std::vector<std::string>& ids)
	{
		rapidjson::Document doc;
		doc.Parse(json.data(), json.size());
		std::string out;
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("entities") || !doc["entities"].IsObject())
			return out;
		const rapidjson::Value& entities = doc["entities"];
		for (const std::string& id : ids)
		{
			if (!entities.HasMember(id.c_str()))
				continue;
			const rapidjson::Value& e = entities[id.c_str()];
			if (!e.IsObject() || !e.HasMember("labels") || !e["labels"].IsObject() || !e["labels"].HasMember("en") ||
				!e["labels"]["en"].IsObject() || !e["labels"]["en"].HasMember("value") || !e["labels"]["en"]["value"].IsString())
				continue;
			std::string label = e["labels"]["en"]["value"].GetString();
			// "action game" -> "Action game": at the start of a list it reads as a name.
			if (!label.empty() && out.empty())
				label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
			out += (out.empty() ? "" : ", ") + label;
		}
		return out;
	}

	// ---- what is kept: one line a fact, "key=value", the text's line breaks as "\n"

	inline std::string Escape(const std::string& s)
	{
		std::string out;
		for (char c : s)
		{
			if (c == '\\')
				out += "\\\\";
			else if (c == '\n')
				out += "\\n";
			else if (c != '\r')
				out += c;
		}
		return out;
	}

	inline std::string Unescape(std::string_view s)
	{
		std::string out;
		for (size_t i = 0; i < s.size(); i++)
		{
			if (s[i] == '\\' && i + 1 < s.size())
			{
				out += (s[i + 1] == 'n') ? '\n' : s[i + 1];
				i++;
			}
			else
				out += s[i];
		}
		return out;
	}

	inline std::string Write(const Info& info)
	{
		std::string out = "found=" + std::string(info.found ? "1" : "0") + "\n";
		out += "looked=" + std::to_string(info.looked) + "\n";
		const auto put = [&](const char* key, const std::string& value) {
			if (!value.empty())
				out += std::string(key) + "=" + Escape(value) + "\n";
		};
		put("article", info.article);
		put("summary", info.summary);
		put("developer", info.developer);
		put("publisher", info.publisher);
		put("released", info.released);
		put("genre", info.genre);
		put("modes", info.modes);
		return out;
	}

	inline Info Read(std::string_view text)
	{
		Info info;
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string_view::npos)
				end = text.size();
			const std::string_view line = text.substr(start, end - start);
			start = end + 1;
			const size_t eq = line.find('=');
			if (eq == std::string_view::npos)
				continue;
			const std::string_view key = line.substr(0, eq);
			const std::string value = Unescape(line.substr(eq + 1));
			if (key == "found")
				info.found = (value == "1");
			else if (key == "looked")
				info.looked = std::strtoll(value.c_str(), nullptr, 10);
			else if (key == "article")
				info.article = value;
			else if (key == "summary")
				info.summary = value;
			else if (key == "developer")
				info.developer = value;
			else if (key == "publisher")
				info.publisher = value;
			else if (key == "released")
				info.released = value;
			else if (key == "genre")
				info.genre = value;
			else if (key == "modes")
				info.modes = value;
		}
		return info;
	}

	/// Percent-encoding for a URL's query or path.
	inline std::string Encode(std::string_view s)
	{
		static const char hex[] = "0123456789ABCDEF";
		std::string out;
		for (char ch : s)
		{
			const unsigned char c = static_cast<unsigned char>(ch);
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				out += static_cast<char>(c);
			else
			{
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 15];
			}
		}
		return out;
	}

} // namespace PS5About
