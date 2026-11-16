// Crux2 - the interface: the settings' model, shared by their screens (bp_settings.cpp: the values,
// the areas and an area's page; bp_settings_home.cpp: the settings' home and the search).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The rows are the pages' (settingsPages: bp_options.cpp's and the Backend's), regrouped by the area
// and section settings-text.yaml gives each (eight areas: Picture, Sound, Controls, Games and saves,
// Emulation, Network, Interface, System). What a row does is the row's own, as on the pages.

#pragma once

#include "bp_internal.h"

#include <string>
#include <vector>

namespace bp::settings
{

/// A row as the settings show it: the option, and the page it is on today (deep links name it).
struct Item
{
	Option o;
	std::string page;
	bool changed = false;  // differs from its default (all games), or the game has a value of its own
};

struct Section
{
	std::string name;
	std::vector<Item> rows;   // with the headings of today's pages where a section has several
	bool changed = false;     // a row of it, at any level, is changed
};

struct Area
{
	const char* name;
	const char* icon;
	const char* about;        // what is in it, in a line
	std::vector<Section> sections;
	int changed = 0;          // its rows changed, at every level
};

enum class View { Home, Area, Search };

struct State
{
	// What is open: the settings of all games, or a game's own (forGame). gameOpen: a game's settings
	// were opened (Backend::openGame), so its own values can be shown and switched to.
	bool forGame = false;
	bool gameOpen = false;
	GameInfo game;
	std::string gameTitle;
	Screen back = Screen::Library;
	View view = View::Home;

	std::vector<Area> areas;  // at the level shown
	std::vector<Item> all;    // every row at every level (the search, the pins), in the areas' order
	unsigned madeFor = ~0u;
	int madeLevel = -1;
	bool madeForGame = false;

	// The home: 0 the search bar, 1..8 the areas, 9 the pinned card, 10 the changed-lately card.
	int home = 1;
	int homeInner = 0;        // the entry of the pinned or changed-lately card in focus
	bool onlyChanged = false; // Triangle on the home: the areas' pages show only what is changed

	// An area's page.
	int area = 0;
	int section = 0;
	int row = 0;
	bool inRows = false;
	float sectionY = 0, rowY = 0, scroll = 0;
	int shownSection = -1;
	double sectionAt = 0;

	// The search.
	std::string query;
	int chip = 0;             // 0 everything, 1 settings, 2 games, 3 changed from default
	int result = -1;          // -1: the field
	float resultScroll = 0;
	View searchBack = View::Home;

	std::string openOn, openRow; // what openSettings was asked to open on (a page, a row)
	bool dirty = true;           // a value was written: what is changed is looked at again
	int builds = 0;              // how many times the areas were made (the search's results go with them)
	int changedAt = -1000;       // the frame it was last looked at
};

State& state();

// ---- the values (bp_settings.cpp)
int layer(const Option& o);
bool hasOwn(const Option& o);
/// The open game has a value of its own for it (whichever view is shown).
bool gameHasOwn(const Option& o);
bool usable(const Option& o);
bool changeable(const Option& o);
bool isSetting(const Option& o);
std::string valueText(const Option& o);
/// The value it takes when nothing is written, in words; empty for what has none.
std::string defaultText(const Option& o);
/// Which choice is the current one (-1: none), and how many there are.
int choiceNow(const Option& o);
int choiceCount(const Option& o);
bool boolNow(const Option& o);
bool differsFromDefault(const Option& o);
/// Left / right on a row; Cross on it. Both note what changed (changed lately).
void change(Option& o, int dir, int repeats);
void activate(Option& o);
/// L3: the game's own value gone (it uses the global setting again), or a binding cleared.
void reset(Option& o);
/// "At once", "Next game start", "Next start of Crux2".
const char* takesEffect(const Option& o);
const char* appliesTo(const Option& o);
/// The row's level: 0 Essentials, 1 More, 2 Everything.
int levelOf(const Option& o);
extern const char* const levelNames[3];

// ---- pins and what changed lately ([PS5UI] Pins, RecentChanges: ids separated by ";")
std::vector<std::string> pins();
bool pinned(const Option& o);
void togglePin(const Option& o);
std::vector<std::string> recent();
void noteChanged(const Option& o);
/// The row of that id among every row (null: none, at no level).
Item* findItem(const std::string& id);
/// Where a row is: its area and section, by index; false when it is in none.
bool placeOf(const std::string& id, int& area, int& section);

// ---- the model
void rebuild();
void refreshChanged();
/// The area's page, on a section, with the focus on a row of that id (none: the first row).
void goTo(int area, int section, const std::string& id);
/// Leaves the settings for the screen that opened them.
void leave();
/// The view of all games or of the open game (TouchPad, the switch in the top bar).
void switchLayer();

// ---- drawing (bp_settings_home.cpp, bp_settings.cpp)
struct Palette
{
	ImU32 ground, card, raised, text, secondary, faint, accent, onAccent, line;
};
Palette palette();
/// The 3-unit focus outline, a little outside the shape.
void focusRing(ImDrawList* dl, float x, float y, float w, float h, float r);
/// The settings' top bar under the title's: the breadcrumb, the switch of all games and the game,
/// the level switch (levels: whether to show it).
void header(ImDrawList* dl, const char* where, bool levels);
/// The focused row described in full: its name, description, choices, default, what it applies to and
/// when it takes effect.
void describe(ImDrawList* dl, const Item& item, float x, float y, float w, float h);
void homeScreen(ImDrawList* dl);
void searchScreen(ImDrawList* dl);
void areaScreen(ImDrawList* dl);
void openSearch(View from);

// ---- the pinned settings outside the settings: the quick menu's (bp_quick.cpp)
/// A pinned row there: of the running game's own settings where a game can have one of its own
/// (forGame), else of the settings of all games.
struct PinnedRow
{
	Item item;
	bool forGame = false;
};
/// The rows [PS5UI] Pins names, in its order, at every level: made for `game` (its own settings open,
/// Backend::openGame) where they can be, else for all games; with no game, all for all games. Made
/// anew at each call (the caller keeps them); what the settings' screens have is left as it was.
std::vector<PinnedRow> pinnedRows(const GameInfo* game);
/// Its value in words, whether it is changed (the game's own value; from its default for all games),
/// whether it can be changed now, and left / right and Cross on it: as the settings do them, in the
/// layer the row is of (Cross goes to the next choice, round, where the settings would ask in a list).
std::string pinnedValue(const PinnedRow& row);
bool pinnedChanged(const PinnedRow& row);
bool pinnedUsable(const PinnedRow& row);
void pinnedChange(PinnedRow& row, int dir, int repeats);
void pinnedActivate(PinnedRow& row);

} // namespace bp::settings
