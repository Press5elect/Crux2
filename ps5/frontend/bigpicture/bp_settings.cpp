// Crux2 - the interface: the settings, of all games and of one game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0.
//
// One screen for both: the categories on the left, the focused category's options on the right.
// Opened for a game (Details > Options in the library, "Game options" in the quick menu) it shows
// what that game has of its own: an option the game does not have says "As in Settings", changing
// it gives the game a value of its own, and Square takes that value away again.
//
// The options are descriptions (bp::Option): the table of bp_options.cpp for what is a plain
// value in the settings, and pages the Backend makes for what depends on the emulator's state
// (the controllers' buttons, the memory cards and BIOS files there are, a game's patches).

#include "bp_internal.h"

#include <cfloat>
#include <cstdio>

namespace bp
{

namespace
{

struct Category
{
	std::string icon, name;
	std::vector<Option> rows;
};

struct SettingsState
{
	bool forGame = false;
	GameInfo game;
	std::string gameTitle;
	Screen back = Screen::Library;
	std::vector<Category> cats;
	unsigned madeFor = ~0u; // the Backend's pages generation the categories were made for
	int madeLevel = -1;
	int cat = 0;
	int row = 0;
	bool inRows = false;
	float catAnim = 0, catScroll = 0, scroll = 0;
	// The row highlight's place and height, on their way to the focused row's; the category the
	// rows were last drawn for, and when it changed.
	float rowY = 0, rowH = 76;
	float noteH = 96, noteWanted = 96; // the focused option's description, under the rows
	std::string openOn;                // the page to open on and go into (openSettings); empty: the first
	int shownCat = -1;
	double catAt = 0;
} st;

int layer(const Option& o)
{
	return (st.forGame && !(o.flags & Option::Front)) ? Own : Shared;
}

// ---- an option's value: the game's own where it has one (and a game is open), else the
// settings', else what the emulator takes when nothing is written.

const char* sec(const Option& o)
{
	return o.section.c_str();
}
const char* key(const Option& o)
{
	return o.key.c_str();
}

bool hasOwn(const Option& o)
{
	if (layer(o) != Own)
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

bool boolNow(const Option& o, const char* section, const char* name, bool def)
{
	if (layer(o) == Own)
	{
		if (const std::optional<bool> own = be->getBool(Own, section, name))
			return *own;
	}
	return be->getBool(Shared, section, name).value_or(def);
}

int intNow(const Option& o, const char* section, const char* name, int def)
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
	if (layer(o) == Own)
	{
		if (const std::optional<int> own = be->getInt(Own, sec(o), key(o)))
			return *own;
	}
	return be->getInt(Shared, sec(o), key(o)).value_or(o.defInt);
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

// Which of its choices an option is on; -1 when its value is none of them.
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
						same = o.combos[c][p] == intNow(o, part.section.c_str(), part.key.c_str(), part.defInt);
					else
						same = (o.combos[c][p] != 0) == boolNow(o, part.section.c_str(), part.key.c_str(), part.def);
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

// A game's own value for an option, gone: it follows the Settings again.
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

std::string valueText(const Option& o)
{
	switch (o.kind)
	{
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

// Left and right on a row, and Cross on a switch.
void change(Option& o, int dir, int repeats)
{
	if (!usable(o))
		return;
	switch (o.kind)
	{
		case Option::Bool:
			be->setBool(layer(o), sec(o), key(o), !boolNow(o, sec(o), key(o), o.defBool));
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
			if (next != now)
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
	if (o.flags & Option::Front)
		loadOptions();
}

// Cross on a row.
void activate(Option& o)
{
	if (!usable(o))
		return;
	switch (o.kind)
	{
		case Option::Action:
			if (o.action)
				o.action();
			break;
		case Option::Bind:
			be->listen(layer(o), o);
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
				if (o.flags & Option::Front)
					loadOptions();
				break;
			}
			// A list to choose from; a game's has "As in Settings" at its top.
			const bool own_layer = (layer(o) == Own);
			std::vector<std::string> names;
			if (own_layer)
				names.push_back("As in Settings");
			for (int i = 0; i < n; i++)
				names.push_back(static_cast<size_t>(i) < o.names.size() ? o.names[static_cast<size_t>(i)] : std::to_string(i));
			const int offset = own_layer ? 1 : 0;
			const int current = (own_layer && !hasOwn(o)) ? 0 : (choiceNow(o) >= 0 ? choiceNow(o) + offset : -1);
			const Option copy = o;
			pick(o.label, std::move(names), current, [copy, offset, own_layer](int chosen) {
				if (own_layer && chosen == 0)
					clearOwn(copy);
				else
					setChoice(copy, chosen - offset);
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

// ---- the categories

void addRows(Category& cat, const std::vector<Option>& rows)
{
	for (const Option& o : rows)
	{
		if (st.forGame && (o.flags & Option::AllGames))
			continue;
		if (!st.forGame && (o.flags & Option::OneGame))
			continue;
		if ((o.flags & Option::Advanced) && !options().advanced)
			continue;
		if ((o.flags & Option::Expert) && !options().expert)
			continue;
		cat.rows.push_back(o);
	}
	// A heading with nothing under it says nothing.
	for (size_t i = 0; i < cat.rows.size();)
	{
		const bool heading = (cat.rows[i].kind == Option::Heading);
		const bool bare = heading && (i + 1 >= cat.rows.size() || cat.rows[i + 1].kind == Option::Heading);
		if (bare)
			cat.rows.erase(cat.rows.begin() + static_cast<std::ptrdiff_t>(i));
		else
			i++;
	}
}

void addPage(std::vector<Category>& cats, const Page& page)
{
	Category cat{page.icon, page.name, {}};
	addRows(cat, page.rows);
	if (!cat.rows.empty())
		cats.push_back(std::move(cat));
}

Option infoRow(const char* label, const std::string& value)
{
	Option o;
	o.kind = Option::Info;
	o.label = label;
	o.info = [value] { return value; };
	return o;
}

// What a game is, and what can be done with all of its own options at once.
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
			return own == 0 ? std::string("None: it follows the Settings") : std::to_string(own);
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
		o.desc = "Takes away every option this game has of its own, its patches and cheats included";
		o.action = [] {
			confirm("Take away this game's own options?",
				"Every option, patch and cheat chosen for this game goes; it follows the Settings again.\nThis cannot be undone.",
				[](bool yes) {
					if (yes)
					{
						be->clearGame();
						toast("This game follows the Settings again.");
					}
				},
				"Take them away", "Keep them");
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
			confirm("Forget the time played?", "The time played and the date last played of this game go back to none.",
				[game](bool yes) {
					if (yes)
					{
						be->resetPlayTime(game);
						toast("The time played was forgotten.");
					}
				},
				"Forget it", "Keep it");
		};
		page.rows.push_back(std::move(o));
	}
	return page;
}

void buildCategories()
{
	st.madeFor = be->pagesGeneration();
	st.madeLevel = options().level;
	std::vector<Category> cats;
	if (st.forGame)
		addPage(cats, summaryPage());
	for (const Page& page : settingsPages(st.forGame))
		addPage(cats, page);
	st.cats = std::move(cats);
	st.cat = std::clamp(st.cat, 0, std::max(0, static_cast<int>(st.cats.size()) - 1));
}

bool focusable(const Option& o)
{
	return o.kind != Option::Heading;
}

// The next row the focus can be on, from `from` in a direction; `from` itself when there is none.
int nextRow(const Category& cat, int from, int dir)
{
	for (int i = from + dir; i >= 0 && i < static_cast<int>(cat.rows.size()); i += dir)
	{
		if (focusable(cat.rows[static_cast<size_t>(i)]))
			return i;
	}
	return from;
}

int firstRow(const Category& cat)
{
	for (int i = 0; i < static_cast<int>(cat.rows.size()); i++)
	{
		if (focusable(cat.rows[static_cast<size_t>(i)]))
			return i;
	}
	return 0;
}

float rowHeight(const Option& o)
{
	if (o.kind == Option::Heading)
		return 64.f;
	return o.desc.empty() ? 76.f : 104.f;
}

void leaveSettings()
{
	st.inRows = false;
	if (st.forGame)
		be->closeGame();
	loadOptions();
	const Screen back = (st.back == Screen::Quick && !be->running().valid) ? Screen::Library : st.back;
	setScreen(back);
}

} // namespace

bool settingsFocus(const char* page, const char* row)
{
	if (st.cats.empty() || st.madeFor != be->pagesGeneration() || st.madeLevel != options().level)
		buildCategories();
	for (size_t c = 0; c < st.cats.size(); c++)
	{
		if (st.cats[c].name != page)
			continue;
		st.cat = static_cast<int>(c);
		st.inRows = false;
		st.row = firstRow(st.cats[c]);
		if (row == nullptr)
			return true;
		for (size_t r = 0; r < st.cats[c].rows.size(); r++)
		{
			if (st.cats[c].rows[r].label == row && focusable(st.cats[c].rows[r]))
			{
				st.row = static_cast<int>(r);
				st.inRows = true;
				return true;
			}
		}
		return false;
	}
	return false;
}

void openSettings(const GameInfo* game, Screen back, const char* page)
{
	const bool forGame = (game != nullptr);
	if (forGame && !be->openGame(*game))
	{
		toast("This game has no serial and no CRC to keep options of its own under.");
		return;
	}
	st.forGame = forGame;
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
	st.cat = 0;
	st.row = 0;
	st.inRows = false;
	st.openOn = (page != nullptr) ? page : "";
	st.scroll = 0;
	st.catScroll = 0;
	st.shownCat = -1;
	st.madeFor = ~0u;
	st.cats.clear();
	setScreen(Screen::Settings);
}

void settingsScreen()
{
	const Running running = be->running();
	ImDrawList* dl = beginScreen("##bp-settings", !running.valid);
	if (running.valid)
	{
		// Over the paused game.
		dl->AddRectFilled(V(0, 0), V(W, H), alpha(col::bgTop, 0.93f));
	}
	if (st.cats.empty() || st.madeFor != be->pagesGeneration() || st.madeLevel != options().level)
		buildCategories();
	if (st.cats.empty())
	{
		leaveSettings();
		endScreen();
		return;
	}
	// Opened for one of its pages (the quick menu's Cheats and Patches): that page, and into it.
	if (!st.openOn.empty())
	{
		for (size_t i = 0; i < st.cats.size(); i++)
		{
			if (st.cats[i].name == st.openOn && !st.cats[i].rows.empty())
			{
				st.cat = static_cast<int>(i);
				st.row = firstRow(st.cats[i]);
				st.inRows = true;
			}
		}
		st.openOn.clear();
	}

	Category& cat = st.cats[static_cast<size_t>(st.cat)];
	st.row = std::clamp(st.row, 0, std::max(0, static_cast<int>(cat.rows.size()) - 1));
	if (!cat.rows.empty() && !focusable(cat.rows[static_cast<size_t>(st.row)]))
		st.row = nextRow(cat, st.row, 1);

	// ---- input
	if (!st.inRows)
	{
		if (in.up && st.cat > 0)
		{
			st.cat--;
			st.row = 0;
		}
		if (in.down && st.cat < static_cast<int>(st.cats.size()) - 1)
		{
			st.cat++;
			st.row = 0;
		}
		if ((in.accept || in.right) && !st.cats[static_cast<size_t>(st.cat)].rows.empty())
		{
			st.inRows = true;
			st.row = firstRow(st.cats[static_cast<size_t>(st.cat)]);
		}
		if (in.back)
			leaveSettings();
	}
	else if (!cat.rows.empty())
	{
		Option& o = cat.rows[static_cast<size_t>(st.row)];
		if (in.up)
			st.row = nextRow(cat, st.row, -1);
		if (in.down)
			st.row = nextRow(cat, st.row, 1);
		if (in.l2)
		{
			for (int i = 0; i < 6; i++)
				st.row = nextRow(cat, st.row, -1);
		}
		if (in.r2)
		{
			for (int i = 0; i < 6; i++)
				st.row = nextRow(cat, st.row, 1);
		}
		const bool sided = (o.kind == Option::Bool || o.kind == Option::IntList || o.kind == Option::FloatList ||
							o.kind == Option::StringList || o.kind == Option::Combo || o.kind == Option::IntRange || o.kind == Option::FloatRange);
		if (in.left)
		{
			if (sided)
				change(o, -1, in.repeats);
			else
				st.inRows = false;
		}
		if (in.right && sided)
			change(o, 1, in.repeats);
		if (in.accept)
			activate(o);
		if (in.square && usable(o))
		{
			if (o.kind == Option::Bind)
				be->clearBinding(layer(o), o);
			else if (hasOwn(o))
				clearOwn(o);
		}
		if (in.back)
			st.inRows = false;
	}
	// TRIANGLE: the settings shown, Normal, Advanced and Expert in turn, from anywhere in them.
	if (in.triangle && screen == Screen::Settings)
	{
		const int next = (options().level + 1) % 3;
		setSettingsLevel(next);
		static const char* const what[3] = {"Normal: the settings most people need", "Advanced: the finer settings as well",
			"Expert: everything, the settings that can stop games from working included"};
		toast(what[next], 2.4f);
	}
	// OPTIONS, which opened the Settings, closes them from anywhere in them.
	if (in.options && screen == Screen::Settings)
		leaveSettings();
	if (screen != Screen::Settings)
	{
		endScreen();
		return;
	}
	// (An option may have changed what the pages are made of.)
	if (st.madeFor != be->pagesGeneration() || st.madeLevel != options().level)
	{
		const std::string name = cat.name;
		buildCategories();
		for (size_t i = 0; i < st.cats.size(); i++)
		{
			if (st.cats[i].name == name)
				st.cat = static_cast<int>(i);
		}
	}
	Category& shown = st.cats[static_cast<size_t>(st.cat)];
	st.row = std::clamp(st.row, 0, std::max(0, static_cast<int>(shown.rows.size()) - 1));
	if (shown.rows.empty())
		st.inRows = false;

	// ---- categories
	const float listX = 96, listW = 440, top = 150;
	const float e = entered(st.forGame ? 201 : 200);
	const Layer content = beginLayer(dl);
	if (st.forGame)
	{
		text(dl, bold(), 22, listX, top - 34, col::accent, "OPTIONS FOR THIS GAME ONLY");
		textIn(dl, bold(), 44, listX, top - 4, col::text, st.gameTitle, W - 2 * listX, true);
	}
	else
	{
		text(dl, bold(), 54, listX, top - 10, col::text, "Settings");
	}
	// The level, beside the title: Triangle goes through the three.
	{
		const float h = 34, size = 21;
		float x = listX + (st.forGame ? textSize(bold(), 22, "OPTIONS FOR THIS GAME ONLY").x : textSize(bold(), 54, "Settings").x) + 28;
		const float y = st.forGame ? top - 34 + 11 - h / 2 : top - 10 + 31 - h / 2;
		x += glyph(dl, Glyph::Triangle, x + 14, y + h / 2, 30) + 18;
		for (int i = 0; i < 3; i++)
		{
			const bool on = (i == options().level);
			const float w = textSize(bold(), size, settingsLevelNames[i]).x + 28;
			rect(dl, x, y, w, h, on ? col::accent : col::panel, h / 2);
			text(dl, bold(), size, x + 14, y + (h - size) / 2 - 2, on ? col::onAccent : col::dim, settingsLevelNames[i]);
			x += w + 8;
		}
	}
	const float catsTop = top + 90, catH = 64;
	const float catsView = H - 72 - catsTop - 24;
	const float catsVisible = catsView / catH;
	const float catTarget = std::clamp(static_cast<float>(st.cat) - (catsVisible - 1) / 2, 0.f,
		std::max(0.f, static_cast<float>(st.cats.size()) - catsVisible));
	st.catScroll = approach(st.catScroll, catTarget, 16);
	st.catAnim = approach(st.catAnim, static_cast<float>(st.cat), 18);
	dl->PushClipRect(V(0, catsTop - 6), V(listX + listW + 20, catsTop + catsView + 6), true);
	// The highlight slides from one category to the next.
	rect(dl, listX - 16, catsTop + (st.catAnim - st.catScroll) * catH, listW, catH - 8, st.inRows ? col::panelHi : col::accent, 12);
	for (int i = 0; i < static_cast<int>(st.cats.size()); i++)
	{
		const float y = catsTop + (i - st.catScroll) * catH;
		if (y > catsTop + catsView || y + catH < catsTop)
			continue;
		const bool on = (i == st.cat);
		const Category& c = st.cats[static_cast<size_t>(i)];
		text(dl, regular(), 26, listX + 10, y + 13, on ? col::text : col::dim, c.icon.c_str());
		textIn(dl, bold(), 26, listX + 62, y + 12, on ? col::text : col::dim, c.name, listW - 100, on);
	}
	dl->PopClipRect();

	// ---- rows
	const float px = listX + listW + 48, pw = W - px - 96;
	const float rowsTop = top + 90;
	// The focused option, described in full, under the list: as tall as what it says (a long
	// description is not cut off there).
	const float noteH = st.noteH = approach(st.noteH, st.noteWanted, 16);
	const float viewH = H - 72 - rowsTop - 24 - noteH;
	rect(dl, px, rowsTop - 24, pw, viewH + 24 + noteH, col::panel, 18);
	// Row heights: a description adds a line.
	std::vector<float> heights;
	float total = 0;
	for (const Option& o : shown.rows)
	{
		heights.push_back(rowHeight(o));
		total += heights.back();
	}
	float focusY = 0;
	for (int i = 0; i < st.row && i < static_cast<int>(heights.size()); i++)
		focusY += heights[static_cast<size_t>(i)];
	const float target = std::clamp(focusY - viewH * 0.4f, 0.f, std::max(0.f, total - viewH + 12));
	st.scroll = approach(st.scroll, target, 14);
	dl->PushClipRect(V(px, rowsTop - 12), V(px + pw, rowsTop + viewH), true);
	// Another category's rows come up in place of the last one's.
	if (st.shownCat != st.cat)
	{
		st.shownCat = st.cat;
		st.catAt = timeNow;
		st.rowY = focusY;
		st.scroll = target;
	}
	const float rowsIn = (motion() == MotionOff) ? 1.f : easeOut(static_cast<float>(timeNow - st.catAt) / 0.22f);
	const Layer rowsLayer = beginLayer(dl);
	float y = rowsTop - st.scroll;
	// The highlight slides from row to row.
	if (st.inRows && !shown.rows.empty())
	{
		const float h = heights[static_cast<size_t>(st.row)];
		st.rowY = approach(st.rowY, focusY, 20);
		st.rowH = approach(st.rowH, h, 20);
		rect(dl, px + 16, y + st.rowY, pw - 32, st.rowH - 8, alpha(col::accent, 0.16f), 12);
		rect(dl, px + 16, y + st.rowY + 10, 6, st.rowH - 28, col::accent, 3);
	}
	for (int i = 0; i < static_cast<int>(shown.rows.size()); i++)
	{
		const Option& o = shown.rows[static_cast<size_t>(i)];
		const float h = heights[static_cast<size_t>(i)];
		if (y + h < rowsTop - 40 || y > rowsTop + viewH + 40)
		{
			y += h;
			continue;
		}
		const bool on = st.inRows && i == st.row;
		const float tx = px + 52;
		const float right = px + pw - 52;
		const float cy = y + (h - 8) / 2;
		if (o.kind == Option::Heading)
		{
			text(dl, bold(), 20, tx, y + 30, col::accent, o.label.c_str());
			rect(dl, tx + textSize(bold(), 20, o.label.c_str()).x + 20, y + 42, std::max(0.f, right - tx - textSize(bold(), 20, o.label.c_str()).x - 20), 1,
				col::line);
			y += h;
			continue;
		}
		const bool can = usable(o);
		const bool own = st.forGame && !(o.flags & Option::OneGame) && hasOwn(o);
		// (What only a game can have, a patch or a cheat, has nothing to follow.)
		const bool follows = st.forGame && layer(o) == Own && changeable(o) && o.kind != Option::Action && !own && !(o.flags & Option::OneGame);
		// A dot marks an option this game has of its own.
		if (own)
			dl->AddCircleFilled(V(px + 36, y + 32), 5 * S, col::accent);
		const ImU32 labelColour = !can ? col::faint : (o.kind == Option::Info) ? col::dim : (follows && !on) ? col::dim : col::text;
		// The value's place is on the right: the label and the description stop short of it.
		// (What does not fit slides sideways by itself while the focus is on the row.)
		textIn(dl, bold(), 28, tx, y + 16, labelColour, o.label, pw * 0.56f, on);
		if (!o.desc.empty())
			textIn(dl, regular(), 22, tx, y + 56, on ? col::dim : col::faint, o.desc, pw * 0.62f, on);

		switch (o.kind)
		{
			case Option::Bool:
			{
				const bool v = boolNow(o, sec(o), key(o), o.defBool);
				const float sw = 84, sh = 44;
				const ImU32 onColour = follows ? alpha(col::accent, 0.45f) : col::accent;
				rect(dl, right - sw, cy - sh / 2, sw, sh, v ? (can ? onColour : alpha(col::accent, 0.3f)) : alpha(col::text, 40 / 255.f), sh / 2);
				dl->AddCircleFilled(V(right - sw + (v ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5) * S, can ? col::text : col::faint);
				if (follows)
				{
					const char* note = "As in Settings";
					const ImVec2 ns = textSize(regular(), 22, note);
					text(dl, regular(), 22, right - sw - 20 - ns.x, cy - ns.y / 2, col::faint, note);
				}
				break;
			}
			case Option::IntList:
			case Option::FloatList:
			case Option::StringList:
			case Option::Combo:
			case Option::IntRange:
			case Option::FloatRange:
			case Option::Bind:
			{
				std::string v = valueText(o);
				if (follows)
					v = "As in Settings: " + v;
				const float room = pw * 0.36f;
				const bool slides = on && textSize(bold(), 26, v.c_str()).x > room;
				if (!slides)
					v = fit(bold(), 26, v, room);
				const ImVec2 vs = slides ? ImVec2(room, textSize(bold(), 26, v.c_str()).y) : textSize(bold(), 26, v.c_str());
				float x = right - vs.x;
				if (o.kind == Option::IntRange || o.kind == Option::FloatRange)
				{
					const float bw = 240;
					const float frac = static_cast<float>(std::clamp((rangeNow(o) - o.lo) / std::max(1e-9, o.hi - o.lo), 0.0, 1.0));
					x = right - bw;
					rect(dl, x, cy - 4, bw, 8, alpha(col::text, 40 / 255.f), 4);
					rect(dl, x, cy - 4, bw * frac, 8, follows ? alpha(col::accent, 0.45f) : col::accent, 4);
					dl->AddCircleFilled(V(x + bw * frac, cy), 13 * S, can ? col::text : col::faint);
					x -= vs.x + 28;
				}
				const bool unset = (o.kind == Option::Bind && v == "Not set");
				textIn(dl, bold(), 26, x, cy - vs.y / 2, !can ? col::faint : unset ? alpha(col::warm, 0.9f) : (on && !follows) ? col::text : col::dim, v,
					room, slides);
				if (on && can && o.kind != Option::Bind && o.kind != Option::IntRange && o.kind != Option::FloatRange)
					drawArrows(dl, x, right, cy, col::accent);
				break;
			}
			case Option::Info:
			{
				const std::string v = valueText(o);
				const float room = pw * 0.58f;
				const ImVec2 vs = textSize(regular(), 24, v.c_str());
				textIn(dl, regular(), 24, right - std::min(vs.x, room), cy - vs.y / 2, col::text, v, room, on);
				break;
			}
			case Option::Action:
			{
				const char* s = ICON_FA_CHEVRON_RIGHT;
				const ImVec2 vs = textSize(regular(), 24, s);
				text(dl, regular(), 24, right - vs.x, cy - vs.y / 2, (on && can) ? col::accent : col::faint, s);
				// What the action is about (an address it changes, the size of what it makes).
				if (o.info)
				{
					const std::string about = o.info();
					const float room = pw * 0.50f;
					const ImVec2 as = textSize(regular(), 24, about.c_str());
					textIn(dl, regular(), 24, right - vs.x - 18 - std::min(as.x, room), cy - as.y / 2, can ? col::text : col::faint, about, room, on);
				}
				break;
			}
			default:
				break;
		}
		const bool nextIsHeading = (i + 1 < static_cast<int>(shown.rows.size()) && shown.rows[static_cast<size_t>(i + 1)].kind == Option::Heading);
		if (i + 1 < static_cast<int>(shown.rows.size()) && !on && !(st.inRows && i + 1 == st.row) && !nextIsHeading)
			rect(dl, px + 40, y + h - 4, pw - 80, 1, col::line);
		y += h;
	}
	endLayer(rowsLayer, rowsIn, 0, (1 - rowsIn) * 16);
	dl->PopClipRect();

	// The focused option, described in full.
	{
		const float ny = rowsTop + viewH;
		rect(dl, px + 40, ny, pw - 80, 1, col::line);
		std::string note;
		if (st.inRows && !shown.rows.empty())
		{
			const Option& o = shown.rows[static_cast<size_t>(st.row)];
			// (In full only when its row had to cut it short.)
			if (fit(regular(), 22, o.desc, pw * 0.62f) != o.desc)
				note = o.desc;
			const auto add = [&](const char* more) { note += (note.empty() ? "" : ". ") + std::string(more); };
			if (o.flags & Option::Restart)
				add("Takes effect the next time the title is started");
			else if ((o.flags & Option::NextStart) && running.valid)
				add("Takes effect the next time the game is started");
			if ((o.flags & Option::NoGame) && running.valid)
				add("Not while a game is running");
			else if (!usable(o))
				add("Not with the other options as they are");
		}
		else if (!shown.rows.empty())
		{
			note = st.forGame ? "An option this game does not have of its own says \"As in Settings\". Changing it gives the game its own; "
								"Square takes it away again." :
								"";
		}
		dl->PushClipRect(V(px + 40, ny + 4), V(px + pw - 40, ny + noteH), true);
		text(dl, regular(), 21, px + 52, ny + 14, col::dim, note.c_str(), pw - 104);
		dl->PopClipRect();
		st.noteWanted = std::clamp(textSize(regular(), 21, note.c_str(), pw - 104).y + 36, 96.f, 230.f);
	}
	endEntrance(content, e);

	// Over the library's tab, which stays lit: the Settings are not a tab.
	topBar(dl, librarySource(), running.valid, Reach::None);
	if (!st.inRows)
	{
		hintBar(dl, {{Glyph::DPadUD, "Category"}, {Glyph::Cross, "Select"}, {Glyph::Triangle, "Level"}, {Glyph::Options, "Close"},
						{Glyph::Circle, "Back"}});
	}
	else if (!shown.rows.empty())
	{
		const Option& o = shown.rows[static_cast<size_t>(st.row)];
		const bool own = st.forGame && !(o.flags & Option::OneGame) && hasOwn(o);
		std::vector<Hint> hints{{Glyph::DPadUD, "Move"}};
		switch (o.kind)
		{
			case Option::Action:
				hints.push_back({Glyph::Cross, "Run"});
				break;
			case Option::Info:
				break;
			case Option::Bind:
				hints.push_back({Glyph::Cross, "Set"});
				hints.push_back({Glyph::Square, "Clear"});
				break;
			case Option::Bool:
				hints.push_back({Glyph::Cross, "On / off"});
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
		if (own && o.kind != Option::Bind)
			hints.push_back({Glyph::Square, "As in Settings"});
		hints.push_back({Glyph::Triangle, "Level"});
		hints.push_back({Glyph::Circle, "Back"});
		hintBar(dl, hints);
	}
	else
	{
		hintBar(dl, {{Glyph::Circle, "Back"}});
	}
	endScreen();
}

} // namespace bp
