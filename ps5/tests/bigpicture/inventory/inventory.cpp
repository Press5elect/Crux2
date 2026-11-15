// Crux2 - the settings inventory: every row the interface can show, as JSON, from the real code.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// This file includes bp_pcsx2.cpp itself, so that the console's Backend (its pages, extraRows, the
// controllers' and hotkeys' rows PCSX2 describes) is the one asked, not the PC preview's stand-in.
// It is built for the PC against PCSX2's headers (run-bigpicture-tests.sh inventory): the few
// PCSX2 pieces the pages read while they are made (the settings layers, the controller types, the
// hotkey lists, the memory card list) are linked for real or faked in host_fakes.cpp, and every
// other symbol bp_pcsx2.cpp names is a stub that stops the program with its name
// (make-stubs.py), so a page that starts to need something new fails loudly, never quietly.
//
//   inventory <out.json>
//
// It is compiled without __PROSPERO__ (the PC has no SDK); bp_pcsx2.cpp has nothing of its own under
// it, and the two VMManager functions below are the only ones of ps5.patch's it names that are.
//
// Writes, for the settings of all games and for one game's own, on a new console and on one set up
// (host_fakes.cpp: Crux2InventorySetUp), every row of every page in order,
// with its page, heading, kind, section and key, default, choices, range, flags and description;
// the "This game" page; the library's layouts and the skins. Deterministic: nothing in it depends
// on the time, the files on the PC or the order of a hash.

#include "VMManager.h"

// Crux2's own VMManager functions (ps5.patch), declared under __PROSPERO__ only.
namespace VMManager
{
std::string GetSpecialSaveStateFileName(const char* game_serial, u32 game_crc, const char* kind);
std::string GetCurrentSpecialSaveStateFileName(const char* kind);
} // namespace VMManager

#include "bp_pcsx2.cpp"

#include "bp_internal.h"

#include <cstdio>
#include <string>

namespace
{

std::string jsonText(const std::string& text)
{
	std::string out = "\"";
	for (const unsigned char c : text)
	{
		switch (c)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char escaped[8];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
					out += escaped;
				}
				else
				{
					out += static_cast<char>(c);
				}
		}
	}
	return out + "\"";
}

// A float as the shortest text that reads back as the same float.
std::string jsonFloat(double value)
{
	char text[64];
	for (int digits = 1; digits <= 17; digits++)
	{
		std::snprintf(text, sizeof(text), "%.*g", digits, value);
		if (static_cast<float>(std::strtod(text, nullptr)) == static_cast<float>(value))
			break;
	}
	return text;
}

const char* const KINDS[] = {"heading", "toggle", "intlist", "floatlist", "stringlist", "intrange", "floatrange", "combo", "action",
	"info", "bind"};

std::string flagNames(unsigned flags)
{
	static const std::pair<unsigned, const char*> names[] = {{bp::Option::AllGames, "AllGames"}, {bp::Option::OneGame, "OneGame"},
		{bp::Option::Advanced, "Advanced"}, {bp::Option::Expert, "Expert"}, {bp::Option::NextStart, "NextStart"},
		{bp::Option::Restart, "Restart"}, {bp::Option::Front, "Front"}, {bp::Option::NoGame, "NoGame"}};
	std::string out = "[";
	unsigned known = 0;
	for (const auto& [bit, name] : names)
	{
		known |= bit;
		if (flags & bit)
			out += std::string(out.size() > 1 ? ", " : "") + jsonText(name);
	}
	if (flags & ~known)
		out += std::string(out.size() > 1 ? ", " : "") + jsonText("0x" + fmt::format("{:x}", flags & ~known));
	return out + "]";
}

void row(std::FILE* f, bool& first, const char* state, const char* layer, const bp::Page& page, int index, const std::string& heading,
	const bp::Option& o)
{
	std::string line = fmt::format("{}    {{\"state\": {}, \"layer\": {}, \"page\": {}, \"index\": {}, \"heading\": {}, \"kind\": \"{}\"",
		first ? "" : ",\n", jsonText(state), jsonText(layer), jsonText(page.name), index, jsonText(heading), KINDS[o.kind]);
	first = false;
	line += ", \"label\": " + jsonText(o.label);
	if (o.kind != bp::Option::Heading)
	{
		line += ", \"desc\": " + jsonText(o.desc);
		line += ", \"section\": " + jsonText(o.section) + ", \"key\": " + jsonText(o.key);
		line += ", \"flags\": " + flagNames(o.flags);
	}
	std::string choices;
	const auto choice = [&](const std::string& name, const std::string& value) {
		choices += std::string(choices.empty() ? "" : ", ") + "{\"name\": " + jsonText(name) + ", \"value\": " + value + "}";
	};
	const auto nameOf = [&](size_t i) { return i < o.names.size() ? o.names[i] : std::string(); };
	switch (o.kind)
	{
		case bp::Option::Bool:
			line += std::string(", \"default\": ") + (o.defBool ? "true" : "false");
			break;
		case bp::Option::IntList:
			line += ", \"default\": " + std::to_string(o.defInt);
			for (size_t i = 0; i < o.ints.size(); i++)
				choice(nameOf(i), std::to_string(o.ints[i]));
			break;
		case bp::Option::FloatList:
			line += ", \"default\": " + jsonFloat(o.defFloat);
			for (size_t i = 0; i < o.floats.size(); i++)
				choice(nameOf(i), jsonFloat(o.floats[i]));
			break;
		case bp::Option::StringList:
			line += ", \"default\": " + jsonText(o.defString);
			for (size_t i = 0; i < o.strings.size(); i++)
				choice(nameOf(i), jsonText(o.strings[i]));
			break;
		case bp::Option::IntRange:
			line += fmt::format(", \"default\": {}, \"range\": {{\"lo\": {}, \"hi\": {}, \"step\": {}, \"unit\": {}}}", o.defInt, jsonFloat(o.lo),
				jsonFloat(o.hi), jsonFloat(o.step), jsonText(o.unit));
			break;
		case bp::Option::FloatRange:
			line += fmt::format(", \"default\": {}, \"range\": {{\"lo\": {}, \"hi\": {}, \"step\": {}, \"scale\": {}, \"decimals\": {}, \"unit\": {}}}",
				jsonFloat(o.defFloat), jsonFloat(o.lo), jsonFloat(o.hi), jsonFloat(o.step), jsonFloat(o.scale), o.decimals, jsonText(o.unit));
			break;
		case bp::Option::Combo:
		{
			std::string parts;
			for (const bp::Option::Part& p : o.parts)
			{
				parts += fmt::format("{}{{\"section\": {}, \"key\": {}, \"default\": {}}}", parts.empty() ? "" : ", ", jsonText(p.section), jsonText(p.key),
					p.isInt ? std::to_string(p.defInt) : std::string(p.def ? "true" : "false"));
			}
			line += ", \"parts\": [" + parts + "]";
			for (size_t i = 0; i < o.combos.size(); i++)
			{
				std::string values;
				for (const int v : o.combos[i])
					values += (values.empty() ? "" : ", ") + std::to_string(v);
				choice(nameOf(i), "[" + values + "]");
			}
			break;
		}
		case bp::Option::Bind:
			line += ", \"bind_type\": " + std::to_string(o.bindType);
			break;
		default:
			break;
	}
	if (!choices.empty())
		line += ", \"choices\": [" + choices + "]";
	if (o.enabled)
		line += ", \"conditional\": true";
	if (o.gameDefault)
		line += ", \"game_default\": true";
	if (o.kind == bp::Option::Action && o.info)
		line += ", \"shows_value\": true";
	std::fputs((line + "}").c_str(), f);
}

void pages(std::FILE* f, bool& first, const char* state, const char* layer, const std::vector<bp::Page>& list)
{
	for (const bp::Page& page : list)
	{
		std::string heading;
		int index = 0;
		for (const bp::Option& o : page.rows)
		{
			if (o.kind == bp::Option::Heading)
				heading = o.label;
			row(f, first, state, layer, page, index++, heading, o);
		}
	}
}

} // namespace

// host_fakes.cpp: the console the pages are made on. 0: new, with one USB drive in and nothing set up.
// 1: set up: signed in at RetroAchievements, a game folder and a network share added, USB controllers on.
void Crux2InventorySetUp(int state);

namespace
{

// Runs the action of the row of that label on a page, as Cross on it would (what it opens is shown
// on the page then: the share being added).
bool press(const char* pageName, const char* label)
{
	for (const bp::Page& page : bp::settingsPages(false))
	{
		if (page.name != pageName)
			continue;
		for (const bp::Option& o : page.rows)
		{
			if (o.kind == bp::Option::Action && o.label == label && o.action)
			{
				o.action();
				return true;
			}
		}
	}
	std::fprintf(stderr, "inventory: no row \"%s\" on the page \"%s\" to press\n", label, pageName);
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <out.json>\n", argv[0]);
		return 2;
	}
	Crux2InventorySetUp(0);
	s_backend.SetHotkeys(InputManager::GetHotkeyList());
	ImGui::CreateContext();
	bp::initialize(&s_backend);

	std::FILE* const f = std::fopen(argv[1], "w");
	if (f == nullptr)
		return 2;
	std::fputs("{\n  \"rows\": [\n", f);
	bool first = true;
	// A game's own: a PS2 game whose settings have nothing of its own yet (no file), played for an hour.
	bp::GameInfo game;
	game.path = "/inventory/game.iso";
	game.fileName = "game.iso";
	game.title = "A game";
	game.serial = "SLUS-00000";
	game.crc = 0x12345678;
	game.kind = bp::GameInfo::PS2;
	game.playedSeconds = 3600;
	for (const int state : {0, 1})
	{
		const char* const name = state == 0 ? "new" : "set-up";
		Crux2InventorySetUp(state);
		if (state == 1 && !press("Folders", "Add a network share"))
			return 1;
		pages(f, first, name, "shared", bp::settingsPages(false));
		if (!s_backend.openGame(game))
		{
			std::fprintf(stderr, "inventory: the game's own settings could not be opened\n");
			return 1;
		}
		bp::openSettings(&game, bp::Screen::Library);
		std::vector<bp::Page> own;
		own.push_back(bp::settingsSummaryPage());
		for (bp::Page& page : bp::settingsPages(true))
			own.push_back(std::move(page));
		pages(f, first, name, "game", own);
		s_backend.closeGame();
	}
	std::fputs("\n  ],\n  \"layouts\": [\n", f);
	for (int i = 0; i < bp::LibraryViewCount; i++)
		std::fprintf(f, "%s    {\"id\": %d, \"name\": %s}", i ? ",\n" : "", i, jsonText(bp::libraryViewNames[i]).c_str());
	std::fputs("\n  ],\n  \"skins\": [\n", f);
	for (int i = 0; i < bp::SkinCount; i++)
		std::fprintf(f, "%s    {\"id\": %d, \"name\": %s, \"family\": %s}", i ? ",\n" : "", i, jsonText(bp::skins[i].name).c_str(),
			jsonText(bp::skins[i].family).c_str());
	std::fputs("\n  ]\n}\n", f);
	std::fclose(f);
	return 0;
}
