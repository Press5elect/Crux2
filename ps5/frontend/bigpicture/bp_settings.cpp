// Crux2 - the interface: the settings, of all games and of one game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0; laid out
// anew in eight areas (the settings' revamp, phase 3).
//
// Three views: the home (a search bar, the eight areas, the pinned settings and the ones changed
// lately: bp_settings_home.cpp), an area's page (its sections, their rows, the focused row described
// in full: here) and the search (bp_settings_home.cpp). The rows are the pages' (bp_options.cpp's
// table, and the Backend's pages for what depends on the emulator's state: the controllers'
// buttons, the memory cards and BIOS files there are, a game's patches), regrouped by the area and
// section settings-text.yaml gives each; their words are that file's (bp_settings_words.cpp).
//
// Opened for a game (Details > Options in the library, "Game options" in the quick menu) they show
// what that game has of its own: a row the game does not have uses the global setting, changing it
// gives the game a value of its own, and L3 takes that value away again. The switch in the top bar
// (the touch pad) goes between the game's and the settings of all games while its settings are open.

#include "bp_settings_model.h"
#include "bp_settings_text.h"

#include <cfloat>
#include <cstdio>
#include <map>

namespace bp
{

namespace settings
{

namespace
{

State st;

const char* sec(const Option& o)
{
	return o.section.c_str();
}
const char* key(const Option& o)
{
	return o.key.c_str();
}

bool boolOf(const Option& o, const char* section, const char* name, bool def)
{
	if (layer(o) == Own)
	{
		if (const std::optional<bool> own = be->getBool(Own, section, name))
			return *own;
		if (o.gameDefault)
		{
			if (const std::optional<bool> crux2 = o.gameDefault())
				return *crux2;
		}
	}
	return be->getBool(Shared, section, name).value_or(def);
}

int intOf(const Option& o, const char* section, const char* name, int def)
{
	if (layer(o) == Own)
	{
		if (const std::optional<int> own = be->getInt(Own, section, name))
			return *own;
	}
	return be->getInt(Shared, section, name).value_or(def);
}

int intNow(const Option& o)
{
	return intOf(o, sec(o), key(o), o.defInt);
}

float floatNow(const Option& o)
{
	if (layer(o) == Own)
	{
		if (const std::optional<float> own = be->getFloat(Own, sec(o), key(o)))
			return *own;
	}
	return be->getFloat(Shared, sec(o), key(o)).value_or(o.defFloat);
}

std::string stringNow(const Option& o)
{
	if (layer(o) == Own)
	{
		if (const std::optional<std::string> own = be->getString(Own, sec(o), key(o)))
			return *own;
	}
	return be->getString(Shared, sec(o), key(o)).value_or(o.defString);
}

void setChoice(const Option& o, int index)
{
	if (index < 0 || index >= choiceCount(o))
		return;
	const int to = layer(o);
	const size_t i = static_cast<size_t>(index);
	switch (o.kind)
	{
		case Option::IntList:
			be->setInt(to, sec(o), key(o), o.ints[i]);
			break;
		case Option::FloatList:
			be->setFloat(to, sec(o), key(o), o.floats[i]);
			break;
		case Option::StringList:
			be->setString(to, sec(o), key(o), o.strings[i]);
			break;
		case Option::Combo:
			for (size_t p = 0; p < o.parts.size(); p++)
			{
				if (o.parts[p].isInt)
					be->setInt(to, o.parts[p].section.c_str(), o.parts[p].key.c_str(), o.combos[i][p]);
				else
					be->setBool(to, o.parts[p].section.c_str(), o.parts[p].key.c_str(), o.combos[i][p] != 0);
			}
			break;
		default:
			break;
	}
}

// A game's own value for an option, gone: it uses the global setting again.
void clearOwn(const Option& o)
{
	if (layer(o) != Own)
		return;
	if (o.kind == Option::Combo)
	{
		for (const Option::Part& part : o.parts)
			be->erase(Own, part.section.c_str(), part.key.c_str());
	}
	else if (o.kind == Option::Bind)
	{
		be->clearBinding(Own, o);
	}
	else
	{
		be->erase(Own, sec(o), key(o));
	}
}

double rangeNow(const Option& o)
{
	return (o.kind == Option::IntRange) ? static_cast<double>(intNow(o)) : static_cast<double>(floatNow(o));
}

void setRange(const Option& o, double v)
{
	v = std::clamp(v, o.lo, o.hi);
	if (o.kind == Option::IntRange)
		be->setInt(layer(o), sec(o), key(o), static_cast<int>(std::lround(v)));
	else
		be->setFloat(layer(o), sec(o), key(o), static_cast<float>(v));
}

std::string rangeText(const Option& o, double v)
{
	char out[64];
	if (o.kind == Option::IntRange)
		std::snprintf(out, sizeof(out), "%d%s", static_cast<int>(std::lround(v * o.scale)), o.unit.c_str());
	else
		std::snprintf(out, sizeof(out), "%.*f%s", o.decimals, v * o.scale, o.unit.c_str());
	return out;
}

std::vector<std::string> split(const std::string& text)
{
	std::vector<std::string> out;
	size_t from = 0;
	while (from < text.size())
	{
		size_t to = text.find(';', from);
		if (to == std::string::npos)
			to = text.size();
		if (to > from)
			out.push_back(text.substr(from, to - from));
		from = to + 1;
	}
	return out;
}

std::string join(const std::vector<std::string>& ids)
{
	std::string out;
	for (const std::string& id : ids)
		out += (out.empty() ? "" : ";") + id;
	return out;
}

// ---- the areas

struct AreaInfo
{
	const char* name;
	const char* icon;
	const char* about;
	// Its sections in this order; one not named here comes after them, in the order the rows have.
	std::vector<const char*> order;
};

const std::vector<AreaInfo>& areaInfo()
{
	static const std::vector<AreaInfo> info = {
		{"Picture", ICON_FA_DISPLAY, "Renderer, resolution, HDR, looks, on-screen display",
			{"Graphics", "Output and HDR", "Looks", "Frame generation", "On-Screen Display"}},
		{"Sound", ICON_FA_VOLUME_HIGH, "Volume, latency, interface sounds and music", {"Audio", "Interface sounds"}},
		{"Controls", ICON_FA_GAMEPAD, "Controllers, bindings, light gun, hotkeys",
			{"Controllers", "Light gun", "PS Move", "USB devices", "Hotkeys"}},
		{"Games and saves", ICON_FA_FOLDER_OPEN, "Game list, folders, BIOS, memory cards, patches",
			{"This game", "Game List", "Folders", "BIOS", "Memory Cards", "Backups", "Save states", "Patches", "Cheats", "Texture packs"}},
		{"Emulation", ICON_FA_MICROCHIP, "Speed, frame pacing, game fixes, the CPUs",
			{"Emulation", "Frame Pacing", "Game Fixes", "Advanced (EE, VU, IOP)"}},
		{"Network", ICON_FA_NETWORK_WIRED, "Network adapter, shares, achievements, updates",
			{"Network & HDD", "Shares", "Achievements", "Remote Control", "Updates"}},
		{"Interface", ICON_FA_PALETTE, "Skins, text size, motion, screen care", {"Interface", "Skins", "Library layouts", "Screen care"}},
		{"System", ICON_FA_GEAR, "The console, logs, debugging, about", {"PlayStation 5", "Log", "Debug", "Statistics", "About"}},
	};
	return info;
}

Option infoRow(const char* label, const std::string& value)
{
	Option o;
	o.kind = Option::Info;
	o.label = label;
	o.info = [value] { return value; };
	return o;
}

// What a game is, and what can be done with all of its own options at once. (The labels are the
// rows' handles: the words shown are settings-text.yaml's.)
Page summaryPage()
{
	Page page{ICON_FA_CIRCLE_INFO, "This game", {}};
	const GameInfo& g = st.game;
	page.rows.push_back(infoRow("Title", st.gameTitle));
	if (!g.serial.empty())
		page.rows.push_back(infoRow("Serial", g.serial));
	if (g.crc != 0)
	{
		char crc[16];
		std::snprintf(crc, sizeof(crc), "%08X", g.crc);
		page.rows.push_back(infoRow("CRC", crc));
	}
	page.rows.push_back(infoRow("Kind", kindText(g)));
	if (!g.region.empty())
		page.rows.push_back(infoRow("Region", g.region));
	if (!g.path.empty())
		page.rows.push_back(infoRow("File", g.path));
	{
		Option o;
		o.kind = Option::Info;
		o.label = "Options of its own";
		o.info = [] {
			const int own = be->ownCount();
			return own == 0 ? std::string("None: uses the global settings") : std::to_string(own);
		};
		page.rows.push_back(std::move(o));
	}
	// What the Backend can do for this game: the settings its database recommends.
	for (Option& o : be->gameRows())
		page.rows.push_back(std::move(o));
	{
		Option o;
		o.kind = Option::Action;
		o.label = "Back to the Settings for everything";
		o.desc = "Removes all of this game's own settings, including its patches and cheats.";
		o.action = [] {
			confirm("Clear this game's settings?",
				"Every setting, patch and cheat chosen for this game goes; it uses the global settings again.\nThis cannot be undone.",
				[](bool yes) {
					if (yes)
					{
						be->clearGame();
						st.dirty = true;
						toast("This game uses the global settings again.");
					}
				},
				"Clear them", "Keep them");
		};
		page.rows.push_back(std::move(o));
	}
	if (g.playedSeconds > 0 && !g.serial.empty())
	{
		Option o;
		o.kind = Option::Action;
		o.label = "Forget the time played";
		o.desc = "Played " + formatPlayed(g.playedSeconds) + " so far";
		const GameInfo game = g;
		o.action = [game] {
			confirm("Reset the time played?", "The time played and the date last played of this game go back to none.",
				[game](bool yes) {
					if (yes)
					{
						be->resetPlayTime(game);
						toast("The time played was reset.");
					}
				},
				"Reset it", "Keep it");
		};
		page.rows.push_back(std::move(o));
	}
	applySettingsText(page);
	return page;
}

bool shownAtLevel(const Option& o)
{
	if ((o.flags & Option::Advanced) && !options().advanced)
		return false;
	if ((o.flags & Option::Expert) && !options().expert)
		return false;
	return true;
}

// The areas made from the pages: every row at the level shown (allLevels: at every level).
std::vector<Area> build(const std::vector<Page>& pages, bool allLevels)
{
	std::vector<Area> areas;
	for (const AreaInfo& a : areaInfo())
		areas.push_back(Area{a.name, a.icon, a.about, {}, 0});
	const auto areaIndex = [](const std::string& name) {
		const auto& info = areaInfo();
		for (size_t i = 0; i < info.size(); i++)
		{
			if (name == info[i].name)
				return static_cast<int>(i);
		}
		return -1;
	};
	const auto sectionOf = [&areas](int area, const std::string& name) -> Section& {
		std::vector<Section>& list = areas[static_cast<size_t>(area)].sections;
		for (Section& s : list)
		{
			if (s.name == name)
				return s;
		}
		list.push_back(Section{name, {}, false});
		return list.back();
	};
	// Which heading of which page each section last had a row under (so that a heading is put once).
	std::map<std::string, const Option*> lastHeading;
	for (const Page& page : pages)
	{
		// Each row's place: its entry's, else the row's before it, else the next one's that has one.
		std::vector<std::pair<int, std::string>> place(page.rows.size(), {-1, std::string()});
		for (size_t i = 0; i < page.rows.size(); i++)
		{
			if (const SettingsText* t = settingsTextOf(page.rows[i]))
				place[i] = {areaIndex(t->area), t->section};
		}
		for (size_t i = 1; i < page.rows.size(); i++)
		{
			if (place[i].first < 0)
				place[i] = place[i - 1];
		}
		for (size_t i = page.rows.size(); i-- > 1;)
		{
			if (place[i - 1].first < 0)
				place[i - 1] = place[i];
		}
		// (A page none of whose rows has an entry, a game's patches with no line of its own: where the
		// table puts the rows of that page.)
		if (!place.empty() && place.front().first < 0)
		{
			for (const SettingsText& t : settingsText)
			{
				if (page.name == t.page)
				{
					std::fill(place.begin(), place.end(), std::pair<int, std::string>(areaIndex(t.area), t.section));
					break;
				}
			}
		}
		const Option* heading = nullptr;
		for (size_t i = 0; i < page.rows.size(); i++)
		{
			const Option& o = page.rows[i];
			if (o.kind == Option::Heading)
			{
				heading = &o;
				continue;
			}
			if (st.forGame && (o.flags & Option::AllGames))
				continue;
			if (!st.forGame && (o.flags & Option::OneGame))
				continue;
			if (!allLevels && !shownAtLevel(o))
				continue;
			const int area = place[i].first >= 0 ? place[i].first : static_cast<int>(areas.size()) - 1;
			Section& s = sectionOf(area, place[i].second.empty() ? page.name : place[i].second);
			if (heading != nullptr)
			{
				const Option*& last = lastHeading[std::to_string(area) + "/" + s.name];
				if (last != heading)
				{
					// (Only where a section has rows under several headings does a heading say anything:
					// a lone one is taken away below.)
					s.rows.push_back(Item{*heading, page.name, false});
					last = heading;
				}
			}
			s.rows.push_back(Item{o, page.name, false});
		}
	}
	for (size_t a = 0; a < areas.size(); a++)
	{
		std::vector<Section>& list = areas[a].sections;
		for (Section& s : list)
		{
			// A heading with nothing under it says nothing; one that is the section's only one neither.
			for (size_t i = 0; i < s.rows.size();)
			{
				const bool isHeading = (s.rows[i].o.kind == Option::Heading);
				const bool bare = isHeading && (i + 1 >= s.rows.size() || s.rows[i + 1].o.kind == Option::Heading);
				if (bare)
					s.rows.erase(s.rows.begin() + static_cast<std::ptrdiff_t>(i));
				else
					i++;
			}
			const int headings = static_cast<int>(std::count_if(s.rows.begin(), s.rows.end(), [](const Item& it) { return it.o.kind == Option::Heading; }));
			if (headings == 1 && !s.rows.empty() && s.rows.front().o.kind == Option::Heading)
				s.rows.erase(s.rows.begin());
		}
		std::erase_if(list, [](const Section& s) { return s.rows.empty(); });
		// The area's order of its sections.
		const std::vector<const char*>& order = areaInfo()[a].order;
		const auto rank = [&order](const Section& s) {
			for (size_t i = 0; i < order.size(); i++)
			{
				if (s.name == order[i])
					return static_cast<int>(i);
			}
			return static_cast<int>(order.size());
		};
		std::stable_sort(list.begin(), list.end(), [&rank](const Section& x, const Section& y) { return rank(x) < rank(y); });
	}
	return areas;
}

bool focusable(const Option& o)
{
	return o.kind != Option::Heading;
}

int firstRow(const Section& s)
{
	for (int i = 0; i < static_cast<int>(s.rows.size()); i++)
	{
		if (focusable(s.rows[static_cast<size_t>(i)].o))
			return i;
	}
	return 0;
}

int nextRow(const Section& s, int from, int dir)
{
	for (int i = from + dir; i >= 0 && i < static_cast<int>(s.rows.size()); i += dir)
	{
		if (focusable(s.rows[static_cast<size_t>(i)].o))
			return i;
	}
	return from;
}

// The next heading's first row in a direction (L1 / R1): the controllers' ports, the hotkeys' groups.
int nextGroup(const Section& s, int from, int dir)
{
	if (dir > 0)
	{
		for (int i = from + 1; i < static_cast<int>(s.rows.size()); i++)
		{
			if (s.rows[static_cast<size_t>(i)].o.kind == Option::Heading)
				return nextRow(s, i, 1);
		}
		return nextRow(s, static_cast<int>(s.rows.size()), -1);
	}
	// Back: to the start of this group, or of the one before when at it already.
	int start = -1;
	for (int i = from - 1; i >= 0; i--)
	{
		if (s.rows[static_cast<size_t>(i)].o.kind == Option::Heading)
		{
			start = i;
			break;
		}
	}
	const int first = (start < 0) ? firstRow(s) : nextRow(s, start, 1);
	if (first < from)
		return first;
	for (int i = start - 1; i >= 0; i--)
	{
		if (s.rows[static_cast<size_t>(i)].o.kind == Option::Heading)
			return nextRow(s, i, 1);
	}
	return firstRow(s);
}

bool sided(const Option& o)
{
	return o.kind == Option::Bool || o.kind == Option::IntList || o.kind == Option::FloatList || o.kind == Option::StringList ||
		   o.kind == Option::Combo || o.kind == Option::IntRange || o.kind == Option::FloatRange;
}

bool matchesRow(const Option& o, const char* row)
{
	return o.label == row || (!o.codeLabel.empty() && o.codeLabel == row) || settingsIdOf(o) == row;
}

// Opens on a page by its name (a section's, an area's, or the page it is on today: "Cheats",
// "Graphics") and a row of it by its label (the words shown, or the code's) or its id.
bool locate(const char* page, const char* row)
{
	if (page == nullptr || page[0] == '\0')
		return false;
	if (row != nullptr && row[0] != '\0')
	{
		for (size_t a = 0; a < st.areas.size(); a++)
		{
			for (size_t s = 0; s < st.areas[a].sections.size(); s++)
			{
				const Section& sec = st.areas[a].sections[s];
				for (size_t r = 0; r < sec.rows.size(); r++)
				{
					const Item& it = sec.rows[r];
					if (!focusable(it.o) || !matchesRow(it.o, row))
						continue;
					if (sec.name != page && st.areas[a].name != std::string(page) && it.page != page)
						continue;
					st.view = View::Area;
					st.area = static_cast<int>(a);
					st.section = static_cast<int>(s);
					st.row = static_cast<int>(r);
					st.inRows = true;
					return true;
				}
			}
		}
		return false;
	}
	const auto open = [](size_t a, size_t s) {
		st.view = View::Area;
		st.area = static_cast<int>(a);
		st.section = static_cast<int>(s);
		st.row = firstRow(st.areas[a].sections[s]);
		st.inRows = false;
		return true;
	};
	for (size_t a = 0; a < st.areas.size(); a++)
	{
		for (size_t s = 0; s < st.areas[a].sections.size(); s++)
		{
			if (st.areas[a].sections[s].name == page)
				return open(a, s);
		}
	}
	for (size_t a = 0; a < st.areas.size(); a++)
	{
		if (st.areas[a].name == std::string(page) && !st.areas[a].sections.empty())
			return open(a, 0);
	}
	for (size_t a = 0; a < st.areas.size(); a++)
	{
		for (size_t s = 0; s < st.areas[a].sections.size(); s++)
		{
			for (const Item& it : st.areas[a].sections[s].rows)
			{
				if (it.page == page)
					return open(a, s);
			}
		}
	}
	return false;
}

bool stale()
{
	return st.areas.empty() || st.madeFor != be->pagesGeneration() || st.madeLevel != options().level || st.madeForGame != st.forGame;
}

} // namespace

State& state()
{
	return st;
}

const char* const levelNames[3] = {"Essentials", "More", "Everything"};

// ---- the values

int layer(const Option& o)
{
	return (st.forGame && !(o.flags & Option::Front)) ? Own : Shared;
}

bool hasOwn(const Option& o)
{
	if (layer(o) != Own)
		return false;
	return gameHasOwn(o);
}

bool gameHasOwn(const Option& o)
{
	if (!st.gameOpen || (o.flags & (Option::Front | Option::AllGames)))
		return false;
	switch (o.kind)
	{
		case Option::Bool:
			return be->getBool(Own, sec(o), key(o)).has_value();
		case Option::IntList:
		case Option::IntRange:
			return be->getInt(Own, sec(o), key(o)).has_value();
		case Option::FloatList:
		case Option::FloatRange:
			return be->getFloat(Own, sec(o), key(o)).has_value();
		case Option::StringList:
			return be->getString(Own, sec(o), key(o)).has_value();
		case Option::Combo:
			for (const Option::Part& part : o.parts)
			{
				if (part.isInt ? be->getInt(Own, part.section.c_str(), part.key.c_str()).has_value() :
								 be->getBool(Own, part.section.c_str(), part.key.c_str()).has_value())
					return true;
			}
			return false;
		case Option::Bind:
			return !be->binding(Own, o).empty();
		default:
			return false;
	}
}

bool boolNow(const Option& o)
{
	return boolOf(o, sec(o), key(o), o.defBool);
}

int choiceCount(const Option& o)
{
	switch (o.kind)
	{
		case Option::IntList:
			return static_cast<int>(o.ints.size());
		case Option::FloatList:
			return static_cast<int>(o.floats.size());
		case Option::StringList:
			return static_cast<int>(o.strings.size());
		case Option::Combo:
			return static_cast<int>(o.combos.size());
		default:
			return 0;
	}
}

int choiceNow(const Option& o)
{
	switch (o.kind)
	{
		case Option::IntList:
		{
			const int v = intNow(o);
			for (size_t i = 0; i < o.ints.size(); i++)
			{
				if (o.ints[i] == v)
					return static_cast<int>(i);
			}
			return -1;
		}
		case Option::FloatList:
		{
			const float v = floatNow(o);
			for (size_t i = 0; i < o.floats.size(); i++)
			{
				if (std::fabs(o.floats[i] - v) < 0.0005f)
					return static_cast<int>(i);
			}
			return -1;
		}
		case Option::StringList:
		{
			const std::string v = stringNow(o);
			for (size_t i = 0; i < o.strings.size(); i++)
			{
				if (o.strings[i] == v)
					return static_cast<int>(i);
			}
			return -1;
		}
		case Option::Combo:
		{
			// The first choice whose switches are all as they are now.
			for (size_t c = 0; c < o.combos.size(); c++)
			{
				bool same = true;
				for (size_t p = 0; p < o.parts.size() && same; p++)
				{
					const Option::Part& part = o.parts[p];
					if (part.isInt)
						same = o.combos[c][p] == intOf(o, part.section.c_str(), part.key.c_str(), part.defInt);
					else
						same = (o.combos[c][p] != 0) == boolOf(o, part.section.c_str(), part.key.c_str(), part.def);
				}
				if (same)
					return static_cast<int>(c);
			}
			return -1;
		}
		default:
			return -1;
	}
}

bool usable(const Option& o)
{
	if ((o.flags & Option::NoGame) && be->running().valid)
		return false;
	return !o.enabled || o.enabled(layer(o));
}

bool changeable(const Option& o)
{
	return o.kind != Option::Heading && o.kind != Option::Info;
}

bool isSetting(const Option& o)
{
	return o.kind != Option::Heading && o.kind != Option::Info && o.kind != Option::Action;
}

std::string valueText(const Option& o)
{
	switch (o.kind)
	{
		case Option::Bool:
			return boolNow(o) ? "On" : "Off";
		case Option::IntList:
		case Option::FloatList:
		case Option::StringList:
		case Option::Combo:
		{
			const int choice = choiceNow(o);
			if (choice >= 0 && static_cast<size_t>(choice) < o.names.size())
				return o.names[static_cast<size_t>(choice)];
			if (o.kind == Option::IntList)
				return std::to_string(intNow(o));
			if (o.kind == Option::StringList)
			{
				const std::string v = stringNow(o);
				return v.empty() ? std::string("None") : v;
			}
			if (o.kind == Option::FloatList)
			{
				char out[32];
				std::snprintf(out, sizeof(out), "%g", floatNow(o));
				return out;
			}
			return "Mixed";
		}
		case Option::IntRange:
		case Option::FloatRange:
			return rangeText(o, rangeNow(o));
		case Option::Info:
			return o.info ? o.info() : std::string();
		case Option::Action:
			return o.info ? o.info() : std::string();
		case Option::Bind:
		{
			std::string bound = be->binding(layer(o), o);
			if (bound.empty() && layer(o) == Own)
				bound = be->binding(Shared, o);
			return bound.empty() ? std::string("Not set") : bound;
		}
		default:
			return "";
	}
}

std::string defaultText(const Option& o)
{
	const auto named = [&o](int i) { return (i >= 0 && static_cast<size_t>(i) < o.names.size()) ? o.names[static_cast<size_t>(i)] : std::string(); };
	switch (o.kind)
	{
		case Option::Bool:
			return o.defBool ? "On" : "Off";
		case Option::IntList:
			for (size_t i = 0; i < o.ints.size(); i++)
			{
				if (o.ints[i] == o.defInt)
					return named(static_cast<int>(i));
			}
			return std::to_string(o.defInt);
		case Option::FloatList:
			for (size_t i = 0; i < o.floats.size(); i++)
			{
				if (std::fabs(o.floats[i] - o.defFloat) < 0.0005f)
					return named(static_cast<int>(i));
			}
			return std::string();
		case Option::StringList:
			for (size_t i = 0; i < o.strings.size(); i++)
			{
				if (o.strings[i] == o.defString)
					return named(static_cast<int>(i));
			}
			return o.defString.empty() ? std::string("None") : o.defString;
		case Option::Combo:
			for (size_t c = 0; c < o.combos.size(); c++)
			{
				bool same = true;
				for (size_t p = 0; p < o.parts.size() && same; p++)
					same = o.parts[p].isInt ? o.combos[c][p] == o.parts[p].defInt : (o.combos[c][p] != 0) == o.parts[p].def;
				if (same)
					return named(static_cast<int>(c));
			}
			return std::string();
		case Option::IntRange:
			return rangeText(o, o.defInt);
		case Option::FloatRange:
			return rangeText(o, o.defFloat);
		default:
			return std::string();
	}
}

bool differsFromDefault(const Option& o)
{
	switch (o.kind)
	{
		case Option::Bool:
			return boolNow(o) != o.defBool;
		case Option::IntList:
		case Option::IntRange:
			return intNow(o) != o.defInt;
		case Option::FloatList:
		case Option::FloatRange:
			return std::fabs(floatNow(o) - o.defFloat) > 0.0005f;
		case Option::StringList:
			return stringNow(o) != o.defString;
		case Option::Combo:
			for (const Option::Part& part : o.parts)
			{
				if (part.isInt ? intOf(o, part.section.c_str(), part.key.c_str(), part.defInt) != part.defInt :
								 boolOf(o, part.section.c_str(), part.key.c_str(), part.def) != part.def)
					return true;
			}
			return false;
		default:
			return false;
	}
}

void change(Option& o, int dir, int repeats)
{
	if (!usable(o))
		return;
	switch (o.kind)
	{
		case Option::Bool:
			be->setBool(layer(o), sec(o), key(o), !boolNow(o));
			break;
		case Option::IntList:
		case Option::FloatList:
		case Option::StringList:
		case Option::Combo:
		{
			const int n = choiceCount(o);
			if (n == 0)
				return;
			const int now = choiceNow(o);
			const int next = (now < 0) ? (dir > 0 ? 0 : n - 1) : std::clamp(now + dir, 0, n - 1);
			if (next == now)
				return;
			setChoice(o, next);
			break;
		}
		case Option::IntRange:
		case Option::FloatRange:
		{
			// Held, it goes faster.
			const double speed = (repeats > 60) ? 25 : (repeats > 25) ? 5 : 1;
			const double steps = std::round((rangeNow(o) - o.lo) / o.step) + dir * speed;
			setRange(o, o.lo + steps * o.step);
			break;
		}
		default:
			return;
	}
	noteChanged(o);
	if (o.flags & Option::Front)
		loadOptions();
}

void activate(Option& o)
{
	if (!usable(o))
		return;
	switch (o.kind)
	{
		case Option::Action:
			if (o.action)
				o.action();
			st.dirty = true;
			break;
		case Option::Bind:
			// (What it is put on is the Backend's to wait for: not noted as changed lately.)
			be->listen(layer(o), o);
			st.dirty = true;
			break;
		case Option::IntList:
		case Option::FloatList:
		case Option::StringList:
		case Option::Combo:
		{
			const int n = choiceCount(o);
			if (n <= 3 && layer(o) != Own)
			{
				// Few choices: the next one, round and round.
				setChoice(o, (std::max(choiceNow(o), -1) + 1) % std::max(n, 1));
				noteChanged(o);
				if (o.flags & Option::Front)
					loadOptions();
				break;
			}
			// A list to choose from; a game's has "Use Global Setting" at its top.
			const bool ownLayer = (layer(o) == Own);
			std::vector<std::string> names;
			if (ownLayer)
				names.push_back("Use Global Setting");
			for (int i = 0; i < n; i++)
				names.push_back(static_cast<size_t>(i) < o.names.size() ? o.names[static_cast<size_t>(i)] : std::to_string(i));
			const int offset = ownLayer ? 1 : 0;
			const int current = (ownLayer && !hasOwn(o)) ? 0 : (choiceNow(o) >= 0 ? choiceNow(o) + offset : -1);
			const Option copy = o;
			pick(o.label, std::move(names), current, [copy, offset, ownLayer](int chosen) {
				if (ownLayer && chosen == 0)
					clearOwn(copy);
				else
					setChoice(copy, chosen - offset);
				noteChanged(copy);
				if (copy.flags & Option::Front)
					loadOptions();
			});
			break;
		}
		default:
			change(o, 1, 0);
			break;
	}
}

void reset(Option& o)
{
	if (!usable(o))
		return;
	if (o.kind == Option::Bind)
	{
		be->clearBinding(layer(o), o);
		noteChanged(o);
	}
	else if (hasOwn(o))
	{
		clearOwn(o);
		noteChanged(o);
	}
	if (o.flags & Option::Front)
		loadOptions();
}

const char* takesEffect(const Option& o)
{
	if (o.flags & Option::Restart)
		return "Next start of Crux2";
	if (o.flags & Option::NextStart)
		return "Next game start";
	return "At once";
}

const char* appliesTo(const Option& o)
{
	if (o.flags & Option::OneGame)
		return "This game";
	if (o.flags & (Option::AllGames | Option::Front))
		return "All games";
	return st.forGame ? "This game" : "All games";
}

int levelOf(const Option& o)
{
	return (o.flags & Option::Expert) ? 2 : (o.flags & Option::Advanced) ? 1 : 0;
}

// ---- pins and what changed lately

std::vector<std::string> pins()
{
	return split(be->getString(Shared, "PS5UI", "Pins").value_or(""));
}

bool pinned(const Option& o)
{
	const std::vector<std::string> list = pins();
	return std::find(list.begin(), list.end(), settingsIdOf(o)) != list.end();
}

void togglePin(const Option& o)
{
	if (!isSetting(o) && o.kind != Option::Action)
		return;
	std::vector<std::string> list = pins();
	const std::string id = settingsIdOf(o);
	const auto it = std::find(list.begin(), list.end(), id);
	if (it != list.end())
	{
		list.erase(it);
		toast("Unpinned: " + o.label, 2.f);
	}
	else
	{
		// (At most 24: the oldest goes.)
		list.push_back(id);
		if (list.size() > 24)
			list.erase(list.begin());
		toast("Pinned to the settings' home: " + o.label, 2.f);
	}
	be->setString(Shared, "PS5UI", "Pins", join(list));
}

std::vector<std::string> recent()
{
	return split(be->getString(Shared, "PS5UI", "RecentChanges").value_or(""));
}

void noteChanged(const Option& o)
{
	st.dirty = true;
	if (!isSetting(o))
		return;
	std::vector<std::string> list = recent();
	const std::string id = settingsIdOf(o);
	std::erase(list, id);
	list.insert(list.begin(), id);
	if (list.size() > 3)
		list.resize(3);
	be->setString(Shared, "PS5UI", "RecentChanges", join(list));
}

Item* findItem(const std::string& id)
{
	for (Item& it : st.all)
	{
		if (focusable(it.o) && settingsIdOf(it.o) == id)
			return &it;
	}
	return nullptr;
}

bool placeOf(const std::string& id, int& area, int& section)
{
	for (size_t a = 0; a < st.areas.size(); a++)
	{
		for (size_t s = 0; s < st.areas[a].sections.size(); s++)
		{
			for (const Item& it : st.areas[a].sections[s].rows)
			{
				if (focusable(it.o) && settingsIdOf(it.o) == id)
				{
					area = static_cast<int>(a);
					section = static_cast<int>(s);
					return true;
				}
			}
		}
	}
	return false;
}

// ---- the model

void rebuild()
{
	st.madeFor = be->pagesGeneration();
	st.madeLevel = options().level;
	st.madeForGame = st.forGame;
	std::vector<Page> pages;
	if (st.forGame)
		pages.push_back(summaryPage());
	for (Page& page : settingsPages(st.forGame))
		pages.push_back(std::move(page));
	st.areas = build(pages, false);
	std::vector<Area> every = build(pages, true);
	st.all.clear();
	for (Area& a : every)
	{
		for (Section& s : a.sections)
		{
			for (Item& it : s.rows)
			{
				if (focusable(it.o))
					st.all.push_back(std::move(it));
			}
		}
	}
	st.area = std::clamp(st.area, 0, static_cast<int>(st.areas.size()) - 1);
	st.builds++;
	st.dirty = true;
	refreshChanged();
}

void refreshChanged()
{
	st.dirty = false;
	st.changedAt = frameNow;
	const auto isChanged = [](const Option& o) {
		if (!isSetting(o) || o.kind == Option::Bind)
			return st.forGame && o.kind == Option::Bind && hasOwn(o);
		if (st.forGame)
			return !(o.flags & Option::OneGame) && hasOwn(o);
		return differsFromDefault(o);
	};
	for (Item& it : st.all)
		it.changed = isChanged(it.o);
	for (Area& a : st.areas)
	{
		a.changed = 0;
		for (Section& s : a.sections)
		{
			s.changed = false;
			for (Item& it : s.rows)
				it.changed = isChanged(it.o);
		}
	}
	// The areas' counts and the sections' dots: at every level.
	for (const Item& it : st.all)
	{
		if (!it.changed)
			continue;
		const SettingsText* t = settingsTextOf(it.o);
		for (Area& a : st.areas)
		{
			if (t == nullptr ? false : a.name == std::string(t->area))
			{
				a.changed++;
				for (Section& s : a.sections)
				{
					if (s.name == t->section)
						s.changed = true;
				}
			}
		}
	}
	// (A row with no entry: its section is its own.)
	for (Area& a : st.areas)
	{
		for (Section& s : a.sections)
		{
			for (const Item& it : s.rows)
			{
				if (it.changed && settingsTextOf(it.o) == nullptr)
				{
					s.changed = true;
					a.changed++;
				}
			}
		}
	}
}

void goTo(int area, int section, const std::string& id)
{
	if (area < 0 || area >= static_cast<int>(st.areas.size()) || st.areas[static_cast<size_t>(area)].sections.empty())
		return;
	Area& a = st.areas[static_cast<size_t>(area)];
	st.view = View::Area;
	st.area = area;
	st.section = std::clamp(section, 0, static_cast<int>(a.sections.size()) - 1);
	const Section& s = a.sections[static_cast<size_t>(st.section)];
	st.row = firstRow(s);
	st.inRows = !id.empty();
	for (size_t r = 0; r < s.rows.size(); r++)
	{
		if (focusable(s.rows[r].o) && settingsIdOf(s.rows[r].o) == id)
			st.row = static_cast<int>(r);
	}
}

void leave()
{
	st.inRows = false;
	if (st.gameOpen)
		be->closeGame();
	st.gameOpen = false;
	loadOptions();
	const Screen back = (st.back == Screen::Quick && !be->running().valid) ? Screen::Library : st.back;
	setScreen(back);
}

void switchLayer()
{
	if (!st.gameOpen)
		return;
	// Where it was: the same area, section and row in the other view where it has them.
	const std::string sectionName =
		(st.view == View::Area && st.area < static_cast<int>(st.areas.size()) && st.section < static_cast<int>(st.areas[static_cast<size_t>(st.area)].sections.size())) ?
			st.areas[static_cast<size_t>(st.area)].sections[static_cast<size_t>(st.section)].name :
			std::string();
	st.forGame = !st.forGame;
	rebuild();
	if (st.view == View::Area)
	{
		bool found = false;
		const Area& a = st.areas[static_cast<size_t>(st.area)];
		for (size_t s = 0; s < a.sections.size(); s++)
		{
			if (a.sections[s].name == sectionName)
			{
				goTo(st.area, static_cast<int>(s), "");
				found = true;
			}
		}
		if (!found)
		{
			st.view = View::Home;
			st.home = 1 + st.area;
		}
	}
	toast(st.forGame ? "This game's own settings" : "The settings of all games", 2.f);
}

// ---- drawing

Palette palette()
{
	Palette p;
	// The default skin's are the design's own; any other skin's are its own colours.
	const bool crux = options().skin == 0 && !options().highContrast;
	p.ground = crux ? col::rgba(18, 17, 16) : col::bgTop;
	p.card = crux ? col::rgba(30, 27, 25) : col::panel;
	p.raised = crux ? col::rgba(42, 38, 35) : col::panelHi;
	p.text = crux ? col::rgba(243, 236, 228) : col::text;
	p.secondary = crux ? col::rgba(185, 176, 165) : col::dim;
	// (A skin whose panels are its page's colour, as the kit's neumorphic ones: cards a shade toward the
	// text, so that they are seen.)
	const auto near = [](ImU32 a, ImU32 b) {
		int most = 0;
		for (int shift = 0; shift < 24; shift += 8)
			most = std::max(most, std::abs(static_cast<int>((a >> shift) & 0xff) - static_cast<int>((b >> shift) & 0xff)));
		return most < 8;
	};
	if (near(p.card, p.ground))
		p.card = mix(p.ground, p.text, 0.06f);
	if (near(p.raised, p.card))
		p.raised = mix(p.card, p.text, 0.08f);
	p.faint = col::faint;
	p.accent = col::accent;
	p.onAccent = col::onAccent;
	p.line = col::line;
	return p;
}

void focusRing(ImDrawList* dl, float x, float y, float w, float h, float r)
{
	const float off = 5;
	outline(dl, x - off, y - off, w + 2 * off, h + 2 * off, palette().accent, (r + off) * col::round, 3);
}

void header(ImDrawList* dl, const char* where, bool levels)
{
	const Palette p = palette();
	const float y = 112, h = 48;
	// The breadcrumb: "Settings / Picture"; a game's: "<game> / Picture".
	{
		std::string first = st.forGame ? st.gameTitle : std::string("Settings");
		first = fit(bold(), 34, first, 620);
		float x = 72;
		x += text(dl, bold(), 34, x, y + 4, (where != nullptr) ? p.secondary : p.text, first.c_str()).x;
		if (where != nullptr)
		{
			x += text(dl, regular(), 34, x + 12, y + 4, p.faint, "/").x + 24;
			text(dl, bold(), 34, x, y + 4, p.text, where);
		}
	}
	float right = W - 72;
	const auto segments = [&](const std::vector<std::string>& names, int on, Glyph g) {
		float total = 0;
		for (const std::string& n : names)
			total += textSize(bold(), 21, n.c_str()).x + 36;
		const float x0 = right - total;
		rect(dl, x0 - 4, y - 4 + 4, total + 8, h - 8 + 8, p.card, (h / 2) * col::round);
		float x = x0;
		for (size_t i = 0; i < names.size(); i++)
		{
			const float w = textSize(bold(), 21, names[i].c_str()).x + 36;
			if (static_cast<int>(i) == on)
				rect(dl, x, y + 4, w, h - 8, p.accent, ((h - 8) / 2) * col::round);
			const ImVec2 ts = textSize(bold(), 21, names[i].c_str());
			text(dl, bold(), 21, x + 18, y + h / 2 - ts.y / 2, static_cast<int>(i) == on ? p.onAccent : p.secondary, names[i].c_str());
			x += w;
		}
		right = x0 - 14;
		right -= glyph(dl, g, 0, -1000, 30);
		glyph(dl, g, right, y + h / 2, 30);
		right -= 36;
	};
	if (levels)
		segments({levelNames[0], levelNames[1], levelNames[2]}, options().level, Glyph::Triangle);
	if (st.gameOpen)
		segments({"All games", fit(bold(), 21, st.gameTitle, 300)}, st.forGame ? 1 : 0, Glyph::TouchPad);
}

void describe(ImDrawList* dl, const Item& item, float x, float y, float w, float h)
{
	const Palette p = palette();
	const Option& o = item.o;
	rect(dl, x, y, w, h, p.card, 16 * col::round);
	dl->PushClipRect(V(x, y), V(x + w, y + h), true);
	const float pad = 32, tw = w - 2 * pad;
	float cy = y + 30;
	cy += text(dl, bold(), 30, x + pad, cy, p.text, o.label.c_str(), tw).y + 14;
	if (const SettingsText* t = settingsTextOf(o); t != nullptr && t->level > 0)
	{
		const char* tag = levelNames[std::clamp(t->level, 0, 2)];
		const float tagW = textSize(bold(), 17, tag).x + 20;
		outline(dl, x + pad, cy, tagW, 28, p.secondary, 14 * col::round, 1.5f);
		text(dl, bold(), 17, x + pad + 10, cy + 4, p.secondary, tag);
		cy += 40;
	}
	if (!o.desc.empty())
		cy += text(dl, regular(), 22, x + pad, cy, p.secondary, o.desc.c_str(), tw).y + 22;
	// The choices, the current one marked.
	std::vector<std::string> choices;
	int current = -1;
	if (o.kind == Option::Bool)
	{
		choices = {"On", "Off"};
		current = boolNow(o) ? 0 : 1;
	}
	else if (choiceCount(o) > 0)
	{
		for (int i = 0; i < choiceCount(o); i++)
			choices.push_back(static_cast<size_t>(i) < o.names.size() ? o.names[static_cast<size_t>(i)] : std::to_string(i));
		current = choiceNow(o);
	}
	if (!choices.empty())
	{
		// (A long list: the ones around the current choice.)
		const int most = 8;
		int from = 0;
		if (static_cast<int>(choices.size()) > most)
			from = std::clamp(current - most / 2, 0, static_cast<int>(choices.size()) - most);
		const int to = std::min(static_cast<int>(choices.size()), from + most);
		if (from > 0)
		{
			text(dl, regular(), 20, x + pad + 34, cy - 4, p.faint, "...");
			cy += 22;
		}
		for (int i = from; i < to; i++)
		{
			const bool on = (i == current);
			const float rcy = cy + 15;
			dl->AddCircle(V(x + pad + 11, rcy), 10 * S, on ? p.accent : p.secondary, 0, 2 * S);
			if (on)
				dl->AddCircleFilled(V(x + pad + 11, rcy), 5 * S, p.accent);
			textIn(dl, on ? bold() : regular(), 22, x + pad + 34, cy + 2, on ? p.text : p.secondary, choices[static_cast<size_t>(i)], tw - 34, false);
			cy += 36;
		}
		if (to < static_cast<int>(choices.size()))
		{
			text(dl, regular(), 20, x + pad + 34, cy - 8, p.faint, "...");
			cy += 18;
		}
		cy += 10;
	}
	else if (o.kind == Option::IntRange || o.kind == Option::FloatRange)
	{
		const std::string span = "From " + rangeText(o, o.lo) + " to " + rangeText(o, o.hi);
		cy += text(dl, regular(), 22, x + pad, cy, p.secondary, span.c_str(), tw).y + 18;
	}
	if (isSetting(o))
	{
		rect(dl, x + pad, cy, tw, 1, p.line);
		cy += 16;
		const auto fact = [&](const char* name, const std::string& value) {
			text(dl, regular(), 21, x + pad, cy, p.faint, name);
			textIn(dl, bold(), 21, x + pad + 170, cy, p.text, value, tw - 170, false);
			cy += 34;
		};
		const std::string def = defaultText(o);
		if (!def.empty())
			fact("Default", def);
		fact("Applies to", appliesTo(o));
		fact("Takes effect", takesEffect(o));
		if (st.forGame && layer(o) == Own && !hasOwn(o) && o.kind != Option::Bind)
			fact("This game", "Uses the global setting");
		else if (!st.forGame && gameHasOwn(o))
			fact("This game", "Has a value of its own");
	}
	if ((o.flags & Option::NoGame) && be->running().valid)
		cy += text(dl, regular(), 21, x + pad, cy + 8, col::warm, "Not while a game is running.", tw).y + 8;
	else if (!usable(o))
		cy += text(dl, regular(), 21, x + pad, cy + 8, col::warm, "Not with the other settings as they are.", tw).y + 8;
	dl->PopClipRect();
}

void areaScreen(ImDrawList* dl)
{
	const Palette p = palette();
	Area& area = st.areas[static_cast<size_t>(st.area)];
	if (area.sections.empty())
	{
		st.view = View::Home;
		return;
	}
	st.section = std::clamp(st.section, 0, static_cast<int>(area.sections.size()) - 1);
	{
		Section& s = area.sections[static_cast<size_t>(st.section)];
		st.row = std::clamp(st.row, 0, std::max(0, static_cast<int>(s.rows.size()) - 1));
		if (!s.rows.empty() && !focusable(s.rows[static_cast<size_t>(st.row)].o))
			st.row = nextRow(s, st.row, 1);
	}

	// ---- input
	const std::string sectionName = area.sections[static_cast<size_t>(st.section)].name;
	std::string rowId;
	{
		Section& s = area.sections[static_cast<size_t>(st.section)];
		if (in.l2 || in.r2)
		{
			st.section = std::clamp(st.section + (in.r2 ? 1 : -1), 0, static_cast<int>(area.sections.size()) - 1);
			st.row = firstRow(area.sections[static_cast<size_t>(st.section)]);
		}
		else if (!st.inRows)
		{
			if (in.up && st.section > 0)
			{
				st.section--;
				st.row = firstRow(area.sections[static_cast<size_t>(st.section)]);
			}
			if (in.down && st.section < static_cast<int>(area.sections.size()) - 1)
			{
				st.section++;
				st.row = firstRow(area.sections[static_cast<size_t>(st.section)]);
			}
			if (in.accept || in.right)
			{
				st.inRows = true;
				st.row = firstRow(area.sections[static_cast<size_t>(st.section)]);
			}
			else if (in.back)
			{
				st.view = View::Home;
				st.home = 1 + st.area;
			}
		}
		else if (!s.rows.empty())
		{
			Option& o = s.rows[static_cast<size_t>(st.row)].o;
			// (With only what is changed shown, the rows that are not are passed over.)
			const auto step = [&s](int from, int dir) {
				int at = nextRow(s, from, dir);
				while (st.onlyChanged && at != from && !s.rows[static_cast<size_t>(at)].changed)
				{
					const int next = nextRow(s, at, dir);
					if (next == at)
						return from;
					at = next;
				}
				return at;
			};
			if (in.up)
				st.row = step(st.row, -1);
			if (in.down)
				st.row = step(st.row, 1);
			if (in.l1)
				st.row = nextGroup(s, st.row, -1);
			if (in.r1)
				st.row = nextGroup(s, st.row, 1);
			if (in.left)
			{
				if (sided(o))
					change(o, -1, in.repeats);
				else
					st.inRows = false;
			}
			if (in.right && sided(o))
				change(o, 1, in.repeats);
			if (in.accept)
				activate(o);
			if (in.square)
				togglePin(o);
			if (in.l3)
				reset(o);
			// (Back: to the home, as from the sections; left on a row that has no sides goes to them.)
			if (in.back)
			{
				st.inRows = false;
				st.view = View::Home;
				st.home = 1 + st.area;
			}
		}
		const Section& now = area.sections[static_cast<size_t>(st.section)];
		if (!now.rows.empty() && static_cast<size_t>(st.row) < now.rows.size())
			rowId = settingsIdOf(now.rows[static_cast<size_t>(st.row)].o);
	}
	// TRIANGLE: the level shown, Essentials, More and Everything in turn.
	if (in.triangle && screen == Screen::Settings)
	{
		const int next = (options().level + 1) % 3;
		setSettingsLevel(next);
		static const char* const what[3] = {"Essentials: the settings most people need", "More: the finer settings as well",
			"Everything: every setting, the ones that can stop games from working included"};
		toast(what[next], 2.4f);
	}
	if (in.touch && screen == Screen::Settings && st.gameOpen)
		switchLayer();
	if (screen != Screen::Settings || st.view != View::Area)
		return;
	// (A change may have made the pages of other rows, or another level is shown: the same section and
	// row again, where they are.)
	if (stale())
	{
		const int areaWas = st.area;
		const bool rows = st.inRows;
		rebuild();
		st.area = areaWas;
		if (st.areas[static_cast<size_t>(st.area)].sections.empty())
		{
			st.view = View::Home;
			return;
		}
		int s = 0;
		const Area& a = st.areas[static_cast<size_t>(st.area)];
		for (size_t i = 0; i < a.sections.size(); i++)
		{
			if (a.sections[i].name == sectionName)
				s = static_cast<int>(i);
		}
		goTo(st.area, s, rowId);
		st.inRows = rows;
	}
	Area& shownArea = st.areas[static_cast<size_t>(st.area)];
	st.section = std::clamp(st.section, 0, static_cast<int>(shownArea.sections.size()) - 1);
	Section& shown = shownArea.sections[static_cast<size_t>(st.section)];
	st.row = std::clamp(st.row, 0, std::max(0, static_cast<int>(shown.rows.size()) - 1));

	// Only what is changed (Triangle on the home): the rows that are, and their headings.
	std::vector<int> visible;
	for (int i = 0; i < static_cast<int>(shown.rows.size()); i++)
	{
		const Item& it = shown.rows[static_cast<size_t>(i)];
		if (!st.onlyChanged || it.changed || it.o.kind == Option::Heading || i == st.row)
			visible.push_back(i);
	}

	const float e = entered(st.forGame ? 205 : 204);
	const Layer content = beginLayer(dl);
	const bool still = motion() == MotionOff;
	header(dl, shownArea.name, true);

	const float top = 196, bottom = H - 72 - 24;
	// ---- the sections
	const float sx = 72, sw = 330, sh = 60;
	{
		st.sectionY = still ? static_cast<float>(st.section) : approach(st.sectionY, static_cast<float>(st.section), 18);
		const float view = bottom - top;
		const float first = std::clamp(st.sectionY - (view / sh - 1) / 2, 0.f, std::max(0.f, static_cast<float>(shownArea.sections.size()) - view / sh));
		dl->PushClipRect(V(sx - 12, top - 12), V(sx + sw + 12, bottom + 12), true);
		for (int i = 0; i < static_cast<int>(shownArea.sections.size()); i++)
		{
			const float y = top + (i - first) * sh;
			if (y > bottom || y + sh < top)
				continue;
			const Section& s = shownArea.sections[static_cast<size_t>(i)];
			const bool on = (i == st.section);
			if (on)
			{
				rect(dl, sx, y, sw, sh - 8, st.inRows ? p.card : p.raised, 12 * col::round);
				if (!st.inRows)
					focusRing(dl, sx, y, sw, sh - 8, 12);
				else
					rect(dl, sx, y + 12, 5, sh - 32, p.accent, 2.5f);
			}
			textIn(dl, bold(), 25, sx + 22, y + (sh - 8) / 2 - 15, on ? p.text : p.secondary, s.name, sw - 64, on && !st.inRows);
			if (s.changed)
				dl->AddCircleFilled(V(sx + sw - 22, y + (sh - 8) / 2), 5 * S, p.accent);
		}
		dl->PopClipRect();
	}

	// ---- the rows
	const float dw = 560, dx = W - 72 - dw;
	const float rx = sx + sw + 28, rw = dx - 28 - rx;
	rect(dl, rx, top, rw, bottom - top, p.card, 16 * col::round);
	const float rowH = 64, headH = 52;
	std::vector<float> ys;
	float total = 0, focusY = 0, focusH = rowH;
	for (int i : visible)
	{
		const float h = shown.rows[static_cast<size_t>(i)].o.kind == Option::Heading ? headH : rowH;
		if (i == st.row)
		{
			focusY = total;
			focusH = h;
		}
		ys.push_back(total);
		total += h;
	}
	const float viewH = bottom - top - 24;
	const float target = std::clamp(focusY - viewH * 0.4f, 0.f, std::max(0.f, total - viewH));
	if (st.shownSection != st.section)
	{
		st.shownSection = st.section;
		st.sectionAt = timeNow;
		st.scroll = target;
		st.rowY = focusY;
	}
	st.scroll = still ? target : approach(st.scroll, target, 14);
	st.rowY = still ? focusY : approach(st.rowY, focusY, 22);
	const float rowsIn = still ? 1.f : easeOut(static_cast<float>(timeNow - st.sectionAt) / 0.2f);
	dl->PushClipRect(V(rx - 10, top + 4), V(rx + rw + 10, bottom - 4), true);
	const Layer rowsLayer = beginLayer(dl);
	const float y0 = top + 12 - st.scroll;
	if (st.inRows && !shown.rows.empty())
	{
		rect(dl, rx + 12, y0 + st.rowY + 2, rw - 24, focusH - 4, p.raised, 12 * col::round);
		focusRing(dl, rx + 12, y0 + st.rowY + 2, rw - 24, focusH - 4, 12);
	}
	for (size_t v = 0; v < visible.size(); v++)
	{
		const int i = visible[v];
		const Item& it = shown.rows[static_cast<size_t>(i)];
		const Option& o = it.o;
		const float y = y0 + ys[v];
		const float h = (o.kind == Option::Heading) ? headH : rowH;
		if (y + h < top || y > bottom)
			continue;
		const float cy = y + h / 2;
		if (o.kind == Option::Heading)
		{
			const ImVec2 ts = textSize(bold(), 18, o.label.c_str());
			text(dl, bold(), 18, rx + 40, cy - ts.y / 2 + 4, p.accent, o.label.c_str());
			rect(dl, rx + 40 + ts.x + 16, cy + 4, std::max(0.f, rw - 80 - ts.x - 16), 1, p.line);
			continue;
		}
		const bool on = st.inRows && i == st.row;
		const bool can = usable(o);
		const bool own = gameHasOwn(o) && !(o.flags & Option::OneGame);
		const bool follows = st.forGame && layer(o) == Own && isSetting(o) && !own && !(o.flags & Option::OneGame);
		if (it.changed)
			dl->AddCircleFilled(V(rx + 26, cy), 5 * S, p.accent);
		// The value on the right, then the label in what is left.
		const float right = rx + rw - 32;
		float valueLeft = right;
		const ImU32 valueColour = !can ? p.faint : it.changed ? p.accent : p.secondary;
		switch (o.kind)
		{
			case Option::Bool:
			{
				const bool v = boolNow(o);
				const float sw2 = 60, sh2 = 32;
				rect(dl, right - sw2, cy - sh2 / 2, sw2, sh2, v ? (can ? (follows ? alpha(p.accent, 0.45f) : p.accent) : alpha(p.accent, 0.3f)) : alpha(p.text, 40 / 255.f),
					sh2 / 2);
				dl->AddCircleFilled(V(right - sw2 + (v ? sw2 - sh2 / 2 : sh2 / 2), cy), (sh2 / 2 - 4) * S, can ? p.text : p.faint);
				valueLeft = right - sw2;
				break;
			}
			case Option::Action:
			{
				const char* chevron = ICON_FA_CHEVRON_RIGHT;
				const ImVec2 cs = textSize(regular(), 22, chevron);
				text(dl, regular(), 22, right - cs.x, cy - cs.y / 2, (on && can) ? p.accent : p.faint, chevron);
				valueLeft = right - cs.x;
				if (o.info)
				{
					const std::string about = o.info();
					const float room = rw * 0.42f;
					const ImVec2 as = textSize(regular(), 22, about.c_str());
					valueLeft -= 16 + std::min(as.x, room);
					textIn(dl, regular(), 22, valueLeft, cy - as.y / 2, can ? p.secondary : p.faint, about, room, on);
				}
				break;
			}
			default:
			{
				std::string v = valueText(o);
				const float room = (o.kind == Option::Info) ? rw * 0.55f : rw * 0.42f;
				const ImVec2 vs = textSize(bold(), 23, v.c_str());
				const float vw = std::min(vs.x, room);
				valueLeft = right - vw;
				const bool unset = (o.kind == Option::Bind && v == "Not set");
				textIn(dl, o.kind == Option::Info ? regular() : bold(), 23, valueLeft, cy - vs.y / 2,
					unset ? alpha(col::warm, 0.9f) : (o.kind == Option::Info ? p.text : valueColour), v, room, on);
				if (on && can && sided(o) && o.kind != Option::IntRange && o.kind != Option::FloatRange)
					drawArrows(dl, valueLeft, right, cy, p.accent);
				break;
			}
		}
		float labelRoom = valueLeft - 36 - (rx + 44);
		if (own)
		{
			// The game's own value overrides the global one.
			const char* tag = "THIS GAME";
			const float tagW = textSize(bold(), 15, tag).x + 18;
			labelRoom -= tagW + 14;
			const ImVec2 ls = textSize(bold(), 25, o.label.c_str());
			const float tagX = rx + 44 + std::min(ls.x, std::max(0.f, labelRoom)) + 14;
			rect(dl, tagX, cy - 13, tagW, 26, alpha(p.accent, 0.18f), 13 * col::round);
			text(dl, bold(), 15, tagX + 9, cy - 9, p.accent, tag);
		}
		const ImU32 labelColour = !can ? p.faint : (o.kind == Option::Info || (follows && !on)) ? p.secondary : p.text;
		const ImVec2 ls = textSize(bold(), 25, o.label.c_str());
		textIn(dl, bold(), 25, rx + 44, cy - ls.y / 2, labelColour, o.label, std::max(40.f, labelRoom), on);
		if (pinned(o))
			text(dl, regular(), 15, rx + 8, cy - 8, p.faint, ICON_FA_THUMBTACK);
	}
	endLayer(rowsLayer, rowsIn, 0, (1 - rowsIn) * 14);
	dl->PopClipRect();
	if (visible.size() <= 1 && st.onlyChanged)
		text(dl, regular(), 22, rx + 44, top + 90, p.faint, "Nothing changed here (Triangle on the home shows everything again).");

	// ---- the focused row, described
	if (!shown.rows.empty())
	{
		const Item& focused = shown.rows[static_cast<size_t>(st.inRows ? st.row : firstRow(shown))];
		if (st.inRows)
			describe(dl, focused, dx, top, dw, bottom - top);
		else
		{
			rect(dl, dx, top, dw, bottom - top, p.card, 16 * col::round);
			text(dl, bold(), 30, dx + 32, top + 30, p.text, shown.name.c_str(), dw - 64);
			int settingsCount = 0, changedCount = 0;
			for (const Item& it : shown.rows)
			{
				settingsCount += focusable(it.o) ? 1 : 0;
				changedCount += it.changed ? 1 : 0;
			}
			char line[96];
			std::snprintf(line, sizeof(line), "%d rows, %d changed", settingsCount, changedCount);
			text(dl, regular(), 22, dx + 32, top + 80, p.secondary, line);
		}
	}
	endEntrance(content, e);

	// ---- the hints
	std::vector<Hint> hints;
	if (!st.inRows)
	{
		hints = {{Glyph::DPadUD, "Section"}, {Glyph::Cross, "Open"}, {Glyph::Triangle, "Level"}};
	}
	else if (!shown.rows.empty())
	{
		const Option& o = shown.rows[static_cast<size_t>(st.row)].o;
		hints.push_back({Glyph::L2R2, "Section"});
		switch (o.kind)
		{
			case Option::Action:
				hints.push_back({Glyph::Cross, "Run"});
				break;
			case Option::Info:
				break;
			case Option::Bind:
				hints.push_back({Glyph::Cross, "Set"});
				hints.push_back({Glyph::L3, "Clear"});
				break;
			case Option::Bool:
				hints.push_back({Glyph::Cross, "On / Off"});
				break;
			case Option::IntRange:
			case Option::FloatRange:
				hints.push_back({Glyph::DPadLR, "Change"});
				break;
			default:
				hints.push_back({Glyph::DPadLR, "Change"});
				hints.push_back({Glyph::Cross, (choiceCount(o) > 3 || st.forGame) ? "List" : "Next"});
				break;
		}
		if (hasOwn(o) && o.kind != Option::Bind)
			hints.push_back({Glyph::L3, "Use Global"});
		if (isSetting(o) || o.kind == Option::Action)
			hints.push_back({Glyph::Square, pinned(o) ? "Unpin" : "Pin"});
		hints.push_back({Glyph::Triangle, "Level"});
	}
	if (st.gameOpen)
		hints.push_back({Glyph::TouchPad, st.forGame ? "All Games" : "This Game"});
	hints.push_back({Glyph::Circle, "Back"});
	hintBar(dl, hints);
}

} // namespace settings

using namespace settings;

Page settingsSummaryPage()
{
	return summaryPage();
}

bool settingsFocus(const char* page, const char* row)
{
	if (stale())
		rebuild();
	return locate(page, row);
}

std::vector<std::string> settingsSections()
{
	if (stale())
		rebuild();
	std::vector<std::string> out;
	for (const Area& a : st.areas)
	{
		for (const Section& s : a.sections)
			out.push_back(std::string(a.name) + "/" + s.name);
	}
	return out;
}

bool settingsFocusAt(int section, int row)
{
	if (stale())
		rebuild();
	for (size_t a = 0; a < st.areas.size(); a++)
	{
		const int count = static_cast<int>(st.areas[a].sections.size());
		if (section >= count)
		{
			section -= count;
			continue;
		}
		const Section& s = st.areas[a].sections[static_cast<size_t>(section)];
		int n = 0;
		for (size_t r = 0; r < s.rows.size(); r++)
		{
			if (!focusable(s.rows[r].o))
				continue;
			if (n++ == row)
			{
				st.view = View::Area;
				st.area = static_cast<int>(a);
				st.section = section;
				st.row = static_cast<int>(r);
				st.inRows = true;
				return true;
			}
		}
		return false;
	}
	return false;
}

void settingsSearchFor(const std::string& query, int chip)
{
	if (stale())
		rebuild();
	if (st.view != View::Search)
		st.searchBack = st.view;
	st.view = View::Search;
	st.query = query;
	st.chip = std::clamp(chip, 0, 3);
	st.result = 0;
}

const GameInfo& settingsGame()
{
	return st.game;
}

// Whether the settings or the quick menu are the screen (the Backend writes the settings when they
// close: bp_pcsx2.cpp). Declared where it is used.
bool menusOpen()
{
	return screen == Screen::Settings || screen == Screen::Quick;
}

void openSettings(const GameInfo* game, Screen back, const char* page)
{
	const bool forGame = (game != nullptr);
	if (forGame && !be->openGame(*game))
	{
		toast("This game has no serial and no CRC to keep settings of its own under.");
		return;
	}
	st.forGame = forGame;
	st.gameOpen = forGame;
	st.game = forGame ? *game : GameInfo();
	st.gameTitle = st.game.title;
	if (forGame)
	{
		// As the library names it (a file's name without its tags).
		if (const GameInfo* listed = libraryGame(game->path); listed != nullptr && st.gameTitle.empty())
			st.gameTitle = listed->title;
		if (st.gameTitle.empty())
			st.gameTitle = cleanTitle(game->fileName);
	}
	st.back = back;
	st.view = View::Home;
	// (A game's settings open on Games and saves, whose first section is the game itself.)
	st.home = forGame ? 4 : 1;
	st.homeInner = 0;
	st.area = 0;
	st.section = 0;
	st.row = 0;
	st.inRows = false;
	st.onlyChanged = false;
	st.query.clear();
	st.chip = 0;
	st.openOn = (page != nullptr) ? page : "";
	st.scroll = 0;
	st.shownSection = -1;
	st.madeFor = ~0u;
	st.areas.clear();
	st.all.clear();
	setScreen(Screen::Settings);
}

void settingsScreen()
{
	const Running running = be->running();
	ImDrawList* dl = beginScreen("##bp-settings", !running.valid);
	const Palette p = palette();
	// The flat ground (over the paused game, nearly opaque).
	dl->AddRectFilled(V(0, 0), V(W, H), alpha(p.ground, running.valid ? 0.95f : 0.90f));
	if (stale())
		rebuild();
	else if (st.dirty || frameNow - st.changedAt > 30)
		refreshChanged();
	// Opened for one of its pages (the quick menu's Cheats and Patches): that page, and into it.
	if (!st.openOn.empty())
	{
		if (locate(st.openOn.c_str(), nullptr))
		{
			st.inRows = true;
			goTo(st.area, st.section, "");
			st.inRows = true;
		}
		st.openOn.clear();
	}
	// OPTIONS, which opened the Settings, closes them from anywhere in them.
	if (in.options)
	{
		leave();
		topBar(dl, librarySource(), running.valid, Reach::None);
		endScreen();
		return;
	}
	switch (st.view)
	{
		case View::Home:
			homeScreen(dl);
			break;
		case View::Area:
			areaScreen(dl);
			break;
		case View::Search:
			searchScreen(dl);
			break;
	}
	// Over the library's tab, which stays lit: the Settings are not a tab.
	topBar(dl, librarySource(), running.valid, Reach::None);
	endScreen();
}

} // namespace bp
