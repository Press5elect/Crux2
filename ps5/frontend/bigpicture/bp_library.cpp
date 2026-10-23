// Crux2 - the interface: the library, a game's details and the start-up animation.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0.
//
// The tabs are where the games come from: Internal (the title's own games folder), USB (when USB
// drives are on), Network (the SMB folders of network.cfg) and, while a disc is in the console's
// drive, Disc. The library has three views
// (Settings > Library, or the touch pad): shelves under a hero, a compact grid, and a list with
// the focused game's details beside it. Triangle opens a game's details: what is known of it, and
// what can be done before it starts - play, start from a saved state, options of its own, start
// with the console's own start-up.

#include "bp_internal.h"
#include "bp_shown_text.h"

#include <cctype>
#include <cfloat>
#include <map>
#include <unordered_map>
#include <strings.h>

namespace bp
{

SplashState splash;

namespace
{

// ---------------------------------------------------------------- the games

struct Game
{
	GameInfo info;
	std::string title;       // as the lists show it
	// A disc of a game with several (see the disc sets, below).
	std::string set;         // the set's key; empty for a game on its own
	int discOrder = 0;       // 1 for "Disc 1"
	int setSize = 0;         // the discs the library has of the set
	bool front = true;       // the disc that stands for the set when sets are grouped
	std::string fullTitle;   // with the disc: "Shadow Hearts  ·  Disc 2"
	std::string setTitle;    // without: "Shadow Hearts"
};

std::vector<Game> games;
unsigned shownGeneration = ~0u;

// ---- the disc drive
//
// The Backend is asked once a frame what is in the console's drive (pollDisc, below). A disc that
// can be played is a game like the others, of a source of its own: it is put among the games, so
// that the Disc tab, its details, its options and its start are the library's own.
DiscState drive;             // as last asked
std::string driveKey;        // what tells a disc from the next one; empty with none in the drive
unsigned driveRevision = 1;  // counts what the Backend says of the drive changing
unsigned shownDrive = 0;     // the revision the games were last made for
bool driveToSay = false;     // a disc has come and the user was not told yet

// ---- disc sets
//
// The discs of one game, by their file names: "Game (USA) (Disc 1).chd" and "Game (USA) (Disc
// 2).chd" are a set, as are "Game - CD1" and "Game - CD2". With "Group the discs of a game" on,
// the library shows a set as one entry, by the disc last played (the first disc until one was);
// Details chooses another disc. While a disc is being chosen for the running game, every disc
// is listed, the running game's own first.
std::map<std::string, std::vector<size_t>> discSets; // key -> indexes into games, in disc order
std::map<std::string, std::string> frontChoice;      // key -> the disc chosen in Details, this run
bool setsDirty = true;
// Counts what the library's lists are made from changing: the games, the disc that stands for
// each set and the titles shown. The lists (buildShelves, buildFlat) are made again when it has
// moved, and not before: sorting a few thousand titles every frame costs several times what
// drawing the screen does.
unsigned libraryGeneration = 1;
bool setsGrouped = false; // what the titles and fronts were last made for

bool grouping(bool selectDisc)
{
	return options().groupDiscs && !selectDisc;
}

std::string baseName(const std::string& fileName)
{
	const size_t dot = fileName.rfind('.');
	return (dot == std::string::npos || dot == 0) ? fileName : fileName.substr(0, dot);
}

void findDiscSets()
{
	discSets.clear();
	for (size_t i = 0; i < games.size(); i++)
	{
		if (!games[i].set.empty())
			discSets[games[i].set].push_back(i);
	}
	for (auto it = discSets.begin(); it != discSets.end();)
	{
		std::vector<size_t>& discs = it->second;
		std::sort(discs.begin(), discs.end(), [](size_t a, size_t b) { return games[a].discOrder < games[b].discOrder; });
		// A set is two discs or more, each number once: else its files are games of their own.
		bool distinct = discs.size() >= 2;
		for (size_t i = 1; i < discs.size(); i++)
			distinct = distinct && games[discs[i]].discOrder != games[discs[i - 1]].discOrder;
		if (!distinct)
		{
			for (size_t i : discs)
			{
				games[i].set.clear();
				games[i].title = games[i].fullTitle;
			}
			it = discSets.erase(it);
			continue;
		}
		for (size_t i : discs)
			games[i].setSize = static_cast<int>(discs.size());
		++it;
	}
	setsDirty = true;
}

// Which disc stands for each set, and the titles the lists show: made again when the games or
// the choice in Details changed, and when the library goes from its own lists to choosing a disc
// and back.
void updateDiscSets(bool selectDisc)
{
	const bool grouped = grouping(selectDisc);
	if (!setsDirty && grouped == setsGrouped)
		return;
	setsDirty = false;
	setsGrouped = grouped;
	libraryGeneration++;
	for (auto& [key, discs] : discSets)
	{
		size_t front = discs.front();
		const auto chosen = frontChoice.find(key);
		bool found = false;
		if (chosen != frontChoice.end())
		{
			for (size_t i : discs)
			{
				if (games[i].info.path == chosen->second)
				{
					front = i;
					found = true;
				}
			}
		}
		if (!found)
		{
			// The disc last played.
			time_t latest = 0;
			for (size_t i : discs)
			{
				if (games[i].info.lastPlayed > latest)
				{
					latest = games[i].info.lastPlayed;
					front = i;
				}
			}
		}
		for (size_t i : discs)
		{
			games[i].front = (i == front);
			games[i].title = grouped ? games[i].setTitle : games[i].fullTitle;
		}
	}
}

// Left out of the library's own lists: a set's other discs.
bool hiddenDisc(const Game& g, bool selectDisc)
{
	return grouping(selectDisc) && !g.set.empty() && !g.front;
}

// The games of every source, as the Backend has them. True when the list was made again.
bool refreshGames()
{
	const unsigned generation = be->gamesGeneration();
	if (generation == shownGeneration && driveRevision == shownDrive)
		return false;
	shownGeneration = generation;
	shownDrive = driveRevision;
	games.clear();
	std::vector<GameInfo> found = be->games();
	for (const GameInfo& disc : drive.games)
	{
		found.push_back(disc);
		found.back().source = DiscDrive;
	}
	for (GameInfo& info : found)
	{
		Game g;
		g.info = std::move(info);
		const std::string base = baseName(g.info.fileName);
		// A game the database does not know is named after its file: without the dump's tags.
		g.title = (g.info.title.empty() || g.info.title == base) ? cleanTitle(g.info.fileName) : g.info.title;
		// (A disc has no file to be named after.)
		if (g.title.empty() && g.info.source == DiscDrive)
			g.title = !g.info.serial.empty() ? g.info.serial : std::string("The disc in the drive");
		g.fullTitle = g.setTitle = g.title;
		std::string rest;
		if (g.info.kind != GameInfo::Program && g.info.source != DiscDrive && splitDisc(base, rest, g.discOrder))
		{
			// A set is of one source and one dump: the name's other tags stay in the key.
			g.set = std::to_string(g.info.source) + "\n" + rest;
			for (char& c : g.set)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			// The database's title may name the disc too; the file's name, cleaned, does not.
			std::string titleRest;
			int titleOrder = 0;
			g.setTitle = splitDisc(g.title, titleRest, titleOrder) ? cleanTitle(titleRest) : g.title;
			if (g.setTitle == g.title)
				g.fullTitle = g.title + "  \xc2\xb7  Disc " + std::to_string(g.discOrder);
		}
		games.push_back(std::move(g));
	}
	findDiscSets();
	libraryGeneration++;
	return true;
}

// ----------------------------------------------------------------- covers

// A card for a game with no art: its title on a tinted gradient, with the mark.
void placeholder(ImDrawList* dl, const std::string& title, float x, float y, float w, float h, float r)
{
	unsigned hash = 2166136261u;
	for (char c : title)
		hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
	const float hue = (hash % 360) / 360.f;
	float cr, cg, cb;
	ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.42f, cr, cg, cb);
	const ImU32 top = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1.f));
	rect(dl, x, y, w, h, col::card, r);
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h * 0.75f), alpha(top, 0.9f), alpha(top, 0.6f), alpha(top, 0.f), alpha(top, 0.f));
	discMark(dl, x + w * 0.5f, y + h * 0.33f, w * 0.20f, col::rgba(255, 255, 255, 60), col::rgba(255, 255, 255, 90), w * 0.022f);
	const float size = std::max(17.f, w * 0.095f);
	dl->PushClipRect(V(x + 10, y), V(x + w - 10, y + h), true);
	const ImVec2 ts = textSize(bold(), size, title.c_str(), w - 28);
	text(dl, bold(), size, x + 14, y + h - 14 - std::min(ts.y, h * 0.42f), col::text, title.c_str(), w - 28);
	dl->PopClipRect();
}

void drawCover(ImDrawList* dl, const Game& g, float x, float y, float w, float h, float r, float lift)
{
	// Drop shadow.
	rect(dl, x + 6, y + 12, w, h, col::rgba(0, 0, 0, static_cast<int>(70 + 60 * lift)), r);
	const Texture cover = be->cover(g.info);
	if (cover.valid())
	{
		rect(dl, x, y, w, h, col::card, r);
		imageFill(dl, cover, x, y, w, h, r);
	}
	else if (g.info.source == DiscDrive)
	{
		// The disc in the drive with no cover to show: the disc itself, on a card.
		rect(dl, x, y, w, h, col::card, r);
		dl->AddRectFilledMultiColor(V(x, y + h * 0.1f), V(x + w, y + h), alpha(col::accent, 0.f), alpha(col::accent, 0.f), alpha(col::accent, 0.28f),
			alpha(col::accent, 0.28f));
		goldDisc(dl, x + w / 2, y + h / 2, w * 0.44f, discSpin(), 1.f, g.info.cd);
	}
	else
	{
		placeholder(dl, g.title, x, y, w, h, r);
	}
	// A sheen on the top edge.
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h * 0.18f), col::rgba(255, 255, 255, static_cast<int>(18 + 20 * lift)),
		col::rgba(255, 255, 255, static_cast<int>(18 + 20 * lift)), 0, 0);
	// A favourite: a star in its corner.
	if (g.info.favourite && w >= 100)
	{
		const float size = std::clamp(w * 0.1f, 16.f, 24.f), bh = size * 1.8f;
		rect(dl, x + 10, y + 10, bh, bh, alpha(col::bar, 0.92f), bh / 2);
		const ImVec2 ss = textSize(regular(), size, ICON_FA_STAR);
		text(dl, regular(), size, x + 10 + (bh - ss.x) / 2, y + 10 + (bh - ss.y) / 2, col::warm, ICON_FA_STAR);
	}
	// A set shown as one entry: how many discs it has.
	if (setsGrouped && g.setSize > 1 && w >= 140)
	{
		const float size = std::clamp(w * 0.085f, 16.f, 22.f);
		const std::string label = std::string(ICON_FA_COMPACT_DISC) + "  " + std::to_string(g.setSize);
		const ImVec2 ls = textSize(bold(), size, label.c_str());
		const float bw = ls.x + size * 1.2f, bh = size * 1.8f;
		rect(dl, x + w - bw - 10, y + 10, bw, bh, alpha(col::bar, 0.92f), bh / 2);
		text(dl, bold(), size, x + w - bw - 10 + (bw - ls.x) / 2, y + 10 + (bh - ls.y) / 2, col::text, label.c_str());
	}
}

// ------------------------------------------------------- the library's state

struct Shelf
{
	std::string name;
	std::vector<size_t> items; // indexes into games
	int focus = 0;
	std::string focusPath; // the focused game, which survives the list being made again
	float scroll = 0;     // in cards, animated
	float focusAnim = -1; // where the focus ring is, on its way to focus
	bool byTitle = true;  // in the order of its titles (the triggers go by letter)
};

// The library as one tab shows it. Each tab keeps its own (the shelf, the focus, the scroll),
// put aside while another is open.
struct LibraryState
{
	int source = Internal;
	std::vector<Shelf> shelves;
	int shelf = 0;
	float shelfAnim = 0;
	bool details = false;
	float detailsAnim = 0;
	float heroFade = 1;
	std::string heroPath;
	// The grid and list views: every game of the tab, by title.
	std::vector<size_t> flat;
	int flatFocus = 0;
	std::string flatPath; // the focused game, which survives a new scan
	float flatScroll = 0; // rows
	// Where the focus is in the grid and the bookcase (a column and a row), in the list and the
	// table (a row) and in the flow (a place along it), on its way to the focused game.
	float ringColumn = -1, ringRow = 0, listRow = -1, flowAt = -1;
	// The showcase's strip, the wheel and the discs, on their way to the focused game; the
	// showcase's and the wheel's art fading in for the game it is of.
	float stripAt = -1, wheelAt = -1, discsAt = -1, artFade = 1;
	std::string artPath;
	// What the shelves and the flat list were last made for (listsKey).
	size_t shelvesKey = 0, flatKey = 0;
	// The letter L2 / R2 last jumped to, shown for a moment.
	char letter = 0;
	double letterAt = -10;
} lib;
LibraryState otherTabs[SourceCount];
bool sourceChosen = false;
// ---- what the list is narrowed to
//
// On every tab alike: a part of a title (typed on the keyboard), which games show at all, and
// what a game has to be to be listed: of a region, played or never played, known to run, with
// saved states, cheats or a texture pack of its own, in a collection.
enum Show { ShowAll, ShowFavourites, ShowHidden };
enum { RegionAny, RegionAmerica, RegionEurope, RegionJapan, RegionOther, RegionCount };
const char* const regionNames[RegionCount] = {"Any region", "America (NTSC-U)", "Europe (PAL)", "Japan (NTSC-J)", "Another region"};
const char* const playedNames[3] = {"Played or not", "Never played", "Played"};
const char* const runsNames[3] = {"However it runs", "Playable or better", "Not known to run"};

struct Narrow
{
	std::string search;
	int show = ShowAll;
	int region = RegionAny;
	int played = 0; // 1 never played, 2 played
	int runs = 0;   // 1 known as playable or perfect, 2 not known at all
	bool states = false, cheats = false, textures = false;
	std::string collection;

	// What Square > Filters sets (the search and which games show are apart from it).
	bool filtered() const { return region != RegionAny || played != 0 || runs != 0 || states || cheats || textures; }
	bool any() const { return !search.empty() || show != ShowAll || filtered() || !collection.empty(); }
	size_t key() const
	{
		size_t k = std::hash<std::string>()(search);
		k = k * 31 + std::hash<std::string>()(collection);
		k = k * 31 + static_cast<size_t>(show);
		k = k * 31 + static_cast<size_t>(region);
		k = k * 31 + static_cast<size_t>(played);
		k = k * 31 + static_cast<size_t>(runs);
		k = k * 31 + (states ? 1u : 0u) + (cheats ? 2u : 0u) + (textures ? 4u : 0u);
		return k;
	}
} nar;

bool narrowed()
{
	return nar.any();
}

bool containsNoCase(const std::string& text, const std::string& part)
{
	if (part.empty())
		return true;
	const auto found = std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
		return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b));
	});
	return found != text.end();
}

int regionOf(const GameInfo& info)
{
	const std::string& r = info.region;
	if (r.empty())
		return RegionAny;
	if (r.compare(0, 6, "NTSC-U") == 0)
		return RegionAmerica;
	if (r.compare(0, 3, "PAL") == 0)
		return RegionEurope;
	if (r.compare(0, 6, "NTSC-J") == 0)
		return RegionJapan;
	return RegionOther;
}

// ---- collections
//
// Shelves of one's own: a name and the games put on it (Details > More > Collections). A game is
// known by its serial, or by its file's name when it has none, so that it stays in its collections
// when its file is moved to another drive. Kept with the interface's own settings.
struct Collection
{
	std::string name;
	std::vector<std::string> members; // sorted
};
std::vector<Collection> collections;
bool collectionsRead = false;
constexpr int MOST_COLLECTIONS = 24;
constexpr const char* FRONT = "PS5UI";

std::vector<std::string> splitBar(const std::string& text)
{
	std::vector<std::string> out;
	size_t from = 0;
	while (from <= text.size())
	{
		const size_t bar = text.find('|', from);
		const std::string part = text.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
		if (!part.empty())
			out.push_back(part);
		if (bar == std::string::npos)
			break;
		from = bar + 1;
	}
	return out;
}

std::string joinBar(const std::vector<std::string>& parts)
{
	std::string out;
	for (const std::string& part : parts)
		out += (out.empty() ? "" : "|") + part;
	return out;
}

// A name as it can be kept: no bar (it parts the names), nothing at its ends.
std::string keptName(std::string name)
{
	for (char& c : name)
	{
		if (c == '|' || static_cast<unsigned char>(c) < 0x20)
			c = ' ';
	}
	while (!name.empty() && name.back() == ' ')
		name.pop_back();
	while (!name.empty() && name.front() == ' ')
		name.erase(name.begin());
	return name;
}

std::string memberKey(const GameInfo& info)
{
	return keptName(!info.serial.empty() ? info.serial : info.fileName);
}

void readCollections()
{
	if (collectionsRead || be == nullptr)
		return;
	collectionsRead = true;
	collections.clear();
	const std::vector<std::string> names = splitBar(be->getString(Shared, FRONT, "Collections").value_or(std::string()));
	for (size_t i = 0; i < names.size() && i < MOST_COLLECTIONS; i++)
	{
		Collection c;
		c.name = names[i];
		c.members = splitBar(be->getString(Shared, FRONT, ("Collection" + std::to_string(i + 1)).c_str()).value_or(std::string()));
		std::sort(c.members.begin(), c.members.end());
		collections.push_back(std::move(c));
	}
}

void writeCollections()
{
	std::vector<std::string> names;
	for (size_t i = 0; i < collections.size(); i++)
	{
		names.push_back(collections[i].name);
		be->setString(Shared, FRONT, ("Collection" + std::to_string(i + 1)).c_str(), joinBar(collections[i].members));
	}
	be->setString(Shared, FRONT, "Collections", joinBar(names));
	// (What a collection that is gone held is emptied: the settings of all games lose no key.)
	for (size_t i = collections.size(); i < MOST_COLLECTIONS; i++)
	{
		const std::string key = "Collection" + std::to_string(i + 1);
		if (!be->getString(Shared, FRONT, key.c_str()).value_or(std::string()).empty())
			be->setString(Shared, FRONT, key.c_str(), std::string());
	}
	libraryGeneration++;
}

Collection* collectionNamed(const std::string& name)
{
	readCollections();
	for (Collection& c : collections)
	{
		if (c.name == name)
			return &c;
	}
	return nullptr;
}

bool holds(const Collection& c, const GameInfo& info)
{
	return std::binary_search(c.members.begin(), c.members.end(), memberKey(info));
}

// Whether a game is in a collection: a game with several discs when any of them is.
bool inCollection(const Collection& c, const Game& g)
{
	if (holds(c, g.info))
		return true;
	if (g.set.empty())
		return false;
	const auto set = discSets.find(g.set);
	if (set == discSets.end())
		return false;
	for (size_t disc : set->second)
	{
		if (holds(c, games[disc].info))
			return true;
	}
	return false;
}

// Puts a game into a collection or takes it out; all the discs of a game that has several.
void setInCollection(Collection& c, const Game& g, bool in)
{
	std::vector<std::string> keys = {memberKey(g.info)};
	if (!g.set.empty())
	{
		const auto set = discSets.find(g.set);
		if (set != discSets.end())
		{
			for (size_t disc : set->second)
				keys.push_back(memberKey(games[disc].info));
		}
	}
	for (const std::string& key : keys)
	{
		if (key.empty())
			continue;
		const auto at = std::lower_bound(c.members.begin(), c.members.end(), key);
		const bool there = (at != c.members.end() && *at == key);
		if (in && !there)
			c.members.insert(at, key);
		else if (!in && there)
			c.members.erase(at);
	}
	writeCollections();
}

// Whether a game is one the list is narrowed to. A hidden game shows only among the hidden ones.
bool matches(const Game& g)
{
	if (nar.show == ShowHidden)
	{
		if (!g.info.hidden)
			return false;
	}
	else if (g.info.hidden)
	{
		return false;
	}
	if (nar.show == ShowFavourites && !g.info.favourite)
		return false;
	if (nar.region != RegionAny && regionOf(g.info) != nar.region)
		return false;
	const bool played = g.info.lastPlayed > 0 || g.info.playedSeconds > 0;
	if ((nar.played == 1 && played) || (nar.played == 2 && !played))
		return false;
	if ((nar.runs == 1 && g.info.compatibility < 5) || (nar.runs == 2 && g.info.compatibility != 0))
		return false;
	if ((nar.states && !g.info.hasStates) || (nar.cheats && !g.info.hasCheats) || (nar.textures && !g.info.hasTextures))
		return false;
	if (!nar.collection.empty())
	{
		const Collection* const c = collectionNamed(nar.collection);
		if (c == nullptr || !inCollection(*c, g))
			return false;
	}
	return containsNoCase(g.title, nar.search);
}

// What the list is narrowed to, in a line: for the chip at the list's top right.
std::string narrowText()
{
	std::string out;
	const auto add = [&](const std::string& part) { out += (out.empty() ? "" : "   \xc2\xb7   ") + part; };
	if (!nar.search.empty())
		add(std::string(ICON_FA_MAGNIFYING_GLASS) + "  \"" + nar.search + "\"");
	if (nar.show == ShowFavourites)
		add(ICON_FA_STAR "  Favourites only");
	if (nar.show == ShowHidden)
		add(ICON_FA_EYE_SLASH "  Hidden games");
	if (!nar.collection.empty())
		add(std::string(ICON_FA_LAYER_GROUP) + "  " + nar.collection);
	if (nar.region != RegionAny)
		add(regionNames[nar.region]);
	if (nar.played != 0)
		add(playedNames[nar.played]);
	if (nar.runs != 0)
		add(runsNames[nar.runs]);
	if (nar.states)
		add("With saved states");
	if (nar.cheats)
		add("With cheats");
	if (nar.textures)
		add("With a texture pack");
	return out;
}

// ---- quick views
//
// What the list is narrowed to and its order, kept under a name: R3 in the library goes from one
// to the next, and after the last back to all games as they were. Up to eight, kept with the
// interface's own settings; three are there to begin with.
struct QuickView
{
	std::string name;
	Narrow narrow;
	int sort = 0;
	int view = -1; // the view it was kept in; -1: whichever is on
};
std::vector<QuickView> quickViews;
bool quickRead = false;
int quickNow = -1; // the one that is on; -1: none
int quickSortBefore = 0, quickViewBefore = 0;
constexpr int MOST_QUICK = 8;

std::string packQuick(const QuickView& q)
{
	const Narrow& n = q.narrow;
	const int bits = (n.states ? 1 : 0) | (n.cheats ? 2 : 0) | (n.textures ? 4 : 0);
	// (An empty part would be lost when the line is parted again: a dot stands for it.)
	const auto part = [](const std::string& text) { return text.empty() ? std::string(".") : keptName(text); };
	return joinBar({part(q.name), std::to_string(q.sort), std::to_string(q.view), std::to_string(n.show), std::to_string(n.region),
		std::to_string(n.played), std::to_string(n.runs), std::to_string(bits), part(n.collection), part(n.search)});
}

bool unpackQuick(const std::string& line, QuickView* q)
{
	const std::vector<std::string> parts = splitBar(line);
	if (parts.size() != 10)
		return false;
	const auto number = [&](size_t i, int lo, int hi) { return std::clamp(std::atoi(parts[i].c_str()), lo, hi); };
	const auto word = [&](size_t i) { return parts[i] == "." ? std::string() : parts[i]; };
	q->name = word(0);
	q->sort = number(1, 0, 4);
	q->view = number(2, -1, 5);
	q->narrow.show = number(3, 0, 2);
	q->narrow.region = number(4, 0, RegionCount - 1);
	q->narrow.played = number(5, 0, 2);
	q->narrow.runs = number(6, 0, 2);
	const int bits = number(7, 0, 7);
	q->narrow.states = (bits & 1) != 0;
	q->narrow.cheats = (bits & 2) != 0;
	q->narrow.textures = (bits & 4) != 0;
	q->narrow.collection = word(8);
	q->narrow.search = word(9);
	return !q->name.empty();
}

void writeQuickViews()
{
	for (int i = 0; i < MOST_QUICK; i++)
	{
		const std::string key = "QuickView" + std::to_string(i + 1);
		if (i < static_cast<int>(quickViews.size()))
			be->setString(Shared, FRONT, key.c_str(), packQuick(quickViews[static_cast<size_t>(i)]));
		else if (!be->getString(Shared, FRONT, key.c_str()).value_or(std::string()).empty())
			be->setString(Shared, FRONT, key.c_str(), std::string());
	}
	be->setBool(Shared, FRONT, "QuickViewsMade", true);
}

void readQuickViews()
{
	if (quickRead || be == nullptr)
		return;
	quickRead = true;
	quickViews.clear();
	if (!be->getBool(Shared, FRONT, "QuickViewsMade").value_or(false))
	{
		// The three to begin with.
		QuickView favourites, recent, fresh;
		favourites.name = "Favourites";
		favourites.narrow.show = ShowFavourites;
		recent.name = "Last played";
		recent.narrow.played = 2;
		recent.sort = 1;
		fresh.name = "Never played";
		fresh.narrow.played = 1;
		quickViews = {favourites, recent, fresh};
		writeQuickViews();
		return;
	}
	for (int i = 0; i < MOST_QUICK; i++)
	{
		QuickView q;
		if (unpackQuick(be->getString(Shared, FRONT, ("QuickView" + std::to_string(i + 1)).c_str()).value_or(std::string()), &q))
			quickViews.push_back(std::move(q));
	}
}

// Puts a quick view on; -1: all games again, in the order and the view they had before the first.
void applyQuick(int index)
{
	readQuickViews();
	if (index >= static_cast<int>(quickViews.size()))
		index = -1;
	if (quickNow < 0 && index >= 0)
	{
		quickSortBefore = options().sort;
		quickViewBefore = options().view;
	}
	if (index < 0)
	{
		nar = Narrow();
		if (quickNow >= 0)
		{
			options().sort = quickSortBefore;
			options().view = quickViewBefore;
		}
		quickNow = -1;
		saveOptions();
		toast("All games again.", 2.f);
		return;
	}
	const QuickView& q = quickViews[static_cast<size_t>(index)];
	nar = q.narrow;
	// (A collection that is gone narrows to nothing: the view shows all then.)
	if (!nar.collection.empty() && collectionNamed(nar.collection) == nullptr)
		nar.collection.clear();
	options().sort = q.sort;
	if (q.view >= 0)
		options().view = q.view;
	saveOptions();
	quickNow = index;
	toast(std::string(ICON_FA_BOLT "   ") + q.name, 2.f);
}

// The list was narrowed or ordered by hand: it is no quick view's any more.
void quickLeft()
{
	quickNow = -1;
}

// The disc in the drive of the running game, while another is chosen.
std::string insertedPath;
// The game on the loading screen.
GameInfo loadingGame;
std::string loadingTitle;

void switchSource(int source)
{
	if (source == lib.source)
		return;
	entranceSide(source > lib.source ? 1.f : -1.f);
	otherTabs[lib.source] = lib;
	lib = otherTabs[source];
	lib.source = source;
	lib.details = false;
	lib.detailsAnim = 0;
	lib.heroPath.clear();
	// (The Disc tab is there only with a disc: it is not the tab the library opens on next time.)
	if (source != DiscDrive)
	{
		options().source = source;
		saveOptions();
	}
}

// The tab the library opens on: the one last open, if it is still there.
void chooseSource()
{
	if (!sourceChosen)
	{
		sourceChosen = true;
		lib.source = std::clamp(options().source, 0, static_cast<int>(Network));
	}
	if ((lib.source == Usb && !be->usbWanted()) || (lib.source == DiscDrive && !drive.present))
		switchSource(Internal);
}

// The views: shelves by what the games are; every cover in a grid; a list with the focused game
// beside it; the covers in a row that flows past; the games as books on shelves, their backs
// out; a table of what is known of each; the focused game as a dashboard shows it, a strip of
// covers under it; a wheel of titles beside the game's art; a wall of every cover; the discs
// themselves, the focused one turning.
enum View { Shelves, Grid, List, Flow, Bookcase, Table, Showcase, Wheel, Wall, Discs, ViewCount };
const char* const viewNames[ViewCount] = {"Shelves", "Grid", "List", "Flow", "Bookcase", "Table", "Showcase", "Wheel", "Wall", "Discs"};

View view()
{
	// The Disc tab is one disc: it has its own screen, laid out as the shelves are.
	if (lib.source == DiscDrive)
		return Shelves;
	return static_cast<View>(std::clamp(options().view, 0, static_cast<int>(ViewCount) - 1));
}

constexpr int GridColumns = 8;
constexpr int WallColumns = 12;
constexpr float CARD_SHAPE = 1.42f; // a PlayStation 2 case: taller than wide
// The bookcase: a book's height, the width of its back, and of its front when it is pulled out.
constexpr float BookH = 272, SpineW = 50, SpineGap = 6, BookRowGap = 66;

float bookCoverW()
{
	return std::round(BookH / CARD_SHAPE);
}

int bookColumns()
{
	return std::max(4, static_cast<int>((W - 192 - (bookCoverW() - SpineW)) / (SpineW + SpineGap)));
}

// How many games a row of the view holds (the flow is one row of all of them).
int columnsOf(View v)
{
	return v == Grid ? GridColumns : v == Wall ? WallColumns : v == Bookcase ? bookColumns() : 1;
}

// What the open tab's lists depend on, as one number.
size_t listsKey(bool selectDisc)
{
	size_t key = libraryGeneration;
	key = key * 31 + static_cast<size_t>(lib.source);
	key = key * 31 + (selectDisc ? 1 : 0);
	key = key * 31 + (grouping(selectDisc) ? 1 : 0);
	if (selectDisc)
		key = key * 31 + std::hash<std::string>()(insertedPath);
	key = key * 31 + nar.key();
	key = key * 31 + static_cast<size_t>(options().sort);
	return key;
}

bool listed(const Game& g, bool selectDisc)
{
	// (A disc is chosen for a running game from all there are: nothing is narrowed there.)
	return g.info.source == lib.source && (!selectDisc || g.info.kind != GameInfo::Program) && !hiddenDisc(g, selectDisc) &&
		   (selectDisc || lib.source == DiscDrive || matches(g));
}

bool byTitle(size_t a, size_t b)
{
	const int order = strcasecmp(games[a].title.c_str(), games[b].title.c_str());
	return order != 0 ? order < 0 : games[a].info.path < games[b].info.path;
}

// The library's order (Options::sort): by name, the game last played first, the one played the
// longest, the largest, or the one whose file is the newest; games that are alike in it follow by
// name.
bool sortedByName()
{
	return options().sort == 0;
}

bool ordered(size_t a, size_t b)
{
	if (options().sort == 1 && games[a].info.lastPlayed != games[b].info.lastPlayed)
		return games[a].info.lastPlayed > games[b].info.lastPlayed;
	if (options().sort == 2 && games[a].info.playedSeconds != games[b].info.playedSeconds)
		return games[a].info.playedSeconds > games[b].info.playedSeconds;
	if (options().sort == 3 && games[a].info.size != games[b].info.size)
		return games[a].info.size > games[b].info.size;
	if (options().sort == 4 && games[a].info.added != games[b].info.added)
		return games[a].info.added > games[b].info.added;
	return byTitle(a, b);
}

void buildFlat(bool selectDisc)
{
	const size_t key = listsKey(selectDisc);
	if (key == lib.flatKey)
		return;
	lib.flatKey = key;
	lib.flat.clear();
	for (size_t i = 0; i < games.size(); i++)
	{
		if (listed(games[i], selectDisc))
			lib.flat.push_back(i);
	}
	std::sort(lib.flat.begin(), lib.flat.end(), selectDisc ? byTitle : ordered);
	// The focus stays on its game when the list changes around it.
	if (!lib.flatPath.empty())
	{
		for (size_t i = 0; i < lib.flat.size(); i++)
		{
			if (games[lib.flat[i]].info.path == lib.flatPath)
				lib.flatFocus = static_cast<int>(i);
		}
	}
	lib.flatFocus = std::clamp(lib.flatFocus, 0, std::max(0, static_cast<int>(lib.flat.size()) - 1));
}

// The letter a game is filed under: its title's first letter, '#' for a digit.
char letterOf(const Game& g)
{
	for (char c : g.title)
	{
		if (isalnum(static_cast<unsigned char>(c)))
			return isdigit(static_cast<unsigned char>(c)) ? '#' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
	}
	return '#';
}

// L2 and R2 in a list sorted by title: R2 goes to the first game of the next letter, L2 to the
// first of this letter and, from there, of the one before.
int letterJump(const std::vector<size_t>& items, int focus, int direction)
{
	const int n = static_cast<int>(items.size());
	if (n == 0)
		return 0;
	focus = std::clamp(focus, 0, n - 1);
	const auto letter = [&](int i) { return letterOf(games[items[static_cast<size_t>(i)]]); };
	int to = focus;
	if (direction > 0)
	{
		while (to < n - 1 && letter(to) == letter(focus))
			to++;
		if (letter(to) == letter(focus))
			to = n - 1; // the last letter: its last game
	}
	else
	{
		while (to > 0 && letter(to - 1) == letter(focus))
			to--;
		if (to == focus && to > 0)
		{
			to--;
			while (to > 0 && letter(to - 1) == letter(to))
				to--;
		}
	}
	lib.letter = letter(to);
	lib.letterAt = timeNow;
	return to;
}

// The focus follows the game from one view to the next.
void focusPath(const std::string& path)
{
	lib.flatPath = path;
	for (size_t i = 0; i < lib.flat.size(); i++)
	{
		if (games[lib.flat[i]].info.path == path)
			lib.flatFocus = static_cast<int>(i);
	}
	for (size_t si = 0; si < lib.shelves.size(); si++)
	{
		for (size_t i = 0; i < lib.shelves[si].items.size(); i++)
		{
			if (games[lib.shelves[si].items[i]].info.path == path && (lib.shelves[si].byTitle || lib.shelves.size() == 1))
			{
				lib.shelf = static_cast<int>(si);
				lib.shelves[si].focus = static_cast<int>(i);
			}
		}
	}
}

const char* const compatibilityWords[] = {"", "Nothing works", "Shows its intro", "Reaches its menus", "Gets in game", "Playable", "Perfect"};

// What is known of a game, in a line: its region, its serial, how well it is known to run.
std::string metaText(const Game& g)
{
	std::string meta;
	const auto add = [&](const std::string& part) {
		if (!part.empty())
			meta += (meta.empty() ? "" : "   \xc2\xb7   ") + part;
	};
	add(g.info.region);
	add(g.info.serial);
	if (g.info.compatibility >= 1 && g.info.compatibility <= 6)
		add(compatibilityWords[g.info.compatibility]);
	// A set shown as one entry: which of its discs this is.
	if (setsGrouped && g.setSize > 1)
		add("Disc " + std::to_string(g.discOrder) + " of " + std::to_string(g.setSize));
	return meta;
}

ImU32 kindColour(const Game& g)
{
	return g.info.kind == GameInfo::PS2 ? col::accent : col::warm;
}

void buildShelves(bool selectDisc)
{
	updateDiscSets(selectDisc);
	const size_t key = listsKey(selectDisc);
	if (key == lib.shelvesKey)
		return;
	lib.shelvesKey = key;
	std::vector<Shelf> shelves;
	if (lib.source == DiscDrive)
	{
		// One shelf, of one disc.
	}
	else if (!selectDisc && !narrowed())
	{
		// The games last played, the latest first.
		Shelf recent;
		recent.name = "Recently played";
		recent.byTitle = false;
		for (size_t i = 0; i < games.size(); i++)
		{
			if (games[i].info.source != lib.source || games[i].info.lastPlayed <= 0 || games[i].info.hidden)
				continue;
			// A set is there once, by the disc that stands for it.
			size_t shown = i;
			if (hiddenDisc(games[i], selectDisc))
			{
				for (size_t disc : discSets[games[i].set])
				{
					if (games[disc].front)
						shown = disc;
				}
			}
			if (std::find(recent.items.begin(), recent.items.end(), shown) == recent.items.end())
				recent.items.push_back(shown);
		}
		const auto played = [](size_t i) {
			time_t latest = games[i].info.lastPlayed;
			if (!games[i].set.empty())
			{
				for (size_t disc : discSets[games[i].set])
					latest = std::max(latest, games[disc].info.lastPlayed);
			}
			return latest;
		};
		std::sort(recent.items.begin(), recent.items.end(), [&](size_t a, size_t b) { return played(a) > played(b); });
		if (recent.items.size() > 15)
			recent.items.resize(15);
		if (!recent.items.empty())
			shelves.push_back(std::move(recent));
		// The favourites, on a shelf of their own.
		Shelf favourites;
		favourites.name = "Favourites";
		favourites.byTitle = sortedByName();
		for (size_t i = 0; i < games.size(); i++)
		{
			if (listed(games[i], selectDisc) && games[i].info.favourite)
				favourites.items.push_back(i);
		}
		std::sort(favourites.items.begin(), favourites.items.end(), ordered);
		if (!favourites.items.empty())
			shelves.push_back(std::move(favourites));
		// Each collection, on a shelf of its own.
		readCollections();
		for (const Collection& c : collections)
		{
			Shelf shelf;
			shelf.name = c.name;
			shelf.byTitle = sortedByName();
			for (size_t i = 0; i < games.size(); i++)
			{
				if (listed(games[i], selectDisc) && inCollection(c, games[i]))
					shelf.items.push_back(i);
			}
			std::sort(shelf.items.begin(), shelf.items.end(), ordered);
			if (!shelf.items.empty())
				shelves.push_back(std::move(shelf));
		}
	}
	else if (selectDisc)
	{
		// The running game's own discs first, in their order.
		Shelf own;
		own.name = "This game's discs";
		own.byTitle = false;
		for (const Game& g : games)
		{
			if (g.info.path == insertedPath && !g.set.empty() && g.info.source == lib.source)
				own.items = discSets[g.set];
		}
		if (!own.items.empty())
			shelves.push_back(std::move(own));
	}
	Shelf all, ps1, programs;
	all.name = (lib.source == DiscDrive) ? (drive.games.size() > 1 ? "In the drives" : "In the drive") : selectDisc ? "Choose a disc" :
	           (quickNow >= 0 && quickNow < static_cast<int>(quickViews.size())) ? quickViews[static_cast<size_t>(quickNow)].name.c_str() :
	           (nar.show == ShowHidden) ? "Hidden games" : !nar.collection.empty() ? nar.collection.c_str() :
	           (nar.show == ShowFavourites) ? "Favourites only" : narrowed() ? "Found" : "All games";
	ps1.name = "PlayStation";
	programs.name = "Programs";
	const bool sorted = !selectDisc && lib.source != DiscDrive; // by what each is, on shelves of their own
	for (size_t i = 0; i < games.size(); i++)
	{
		if (!listed(games[i], selectDisc))
			continue;
		if (sorted && games[i].info.kind == GameInfo::PS1)
			ps1.items.push_back(i);
		else if (sorted && games[i].info.kind == GameInfo::Program)
			programs.items.push_back(i);
		else
			all.items.push_back(i);
	}
	for (Shelf* shelf : {&all, &ps1, &programs})
	{
		const bool inOrder = !selectDisc && lib.source != DiscDrive;
		std::sort(shelf->items.begin(), shelf->items.end(), inOrder ? ordered : byTitle);
		shelf->byTitle = !inOrder || sortedByName();
		if (!shelf->items.empty())
			shelves.push_back(std::move(*shelf));
	}
	// Keep the focus where it was.
	for (Shelf& s : shelves)
	{
		for (const Shelf& old : lib.shelves)
		{
			if (old.name == s.name)
			{
				s.focus = std::clamp(old.focus, 0, std::max(0, static_cast<int>(s.items.size()) - 1));
				// The same game, wherever it is now (the list was scanned again, or a game was
				// played and the shelf of the last played is in another order).
				for (size_t i = 0; i < s.items.size(); i++)
				{
					if (!old.focusPath.empty() && games[s.items[i]].info.path == old.focusPath)
						s.focus = static_cast<int>(i);
				}
				s.focusPath = old.focusPath;
				s.scroll = old.scroll;
				s.focusAnim = old.focusAnim;
			}
		}
	}
	lib.shelves = std::move(shelves);
	lib.shelf = std::clamp(lib.shelf, 0, std::max(0, static_cast<int>(lib.shelves.size()) - 1));
}

// --------------------------------------------------------------- starting

// The state made when the game was last closed, if it has one.
std::optional<StateInfo> resumeState(const Game& g)
{
	if (g.info.crc == 0)
		return std::nullopt;
	for (const StateInfo& state : be->states(g.info.serial, g.info.crc, false))
	{
		if (state.slot == -1 && !state.backup)
			return state;
	}
	return std::nullopt;
}

void launch(const Game& g, bool selectDisc)
{
	if (selectDisc)
	{
		insertedPath = g.info.path;
		be->changeDisc(g.info.path);
		be->setPaused(false);
		setScreen(Screen::None);
		return;
	}
	// A game closed with "save state on shutdown" can go on from there, or start over.
	if (const std::optional<StateInfo> state = resumeState(g))
	{
		const GameInfo info = g.info;
		const StateInfo saved = *state;
		pick(g.title + ": it was saved when it was last closed (" + formatWhen(saved.when) + ")",
			{"Go on from there", "Start from the beginning", "Delete that state and start from the beginning"}, 0,
			[info, saved](int choice) {
				if (choice == 2 && !be->deleteState(saved))
				{
					toast("That state could not be deleted.");
					return;
				}
				launchGame(info, choice == 0 ? -1 : INT_MIN, 0);
			});
		return;
	}
	launchGame(g.info, INT_MIN, 0);
}

// A tab with no games: what it shows, and what to do about it.
void emptyLibrary(ImDrawList* dl, bool selectDisc)
{
	const std::string root = be->shownFolder("root");
	const char* const formats = "Disc images: .iso  .chd  .cso  .zso  .bin/.cue  .gz";
	std::string title, body, foot = "Press Square to scan again.";
	const bool busy = be->scanning() && lib.source != DiscDrive;
	if (!selectDisc && lib.source != DiscDrive && narrowed() && !busy)
	{
		// The tab has games, or may have: none of them is one the list is narrowed to.
		title = "Nothing to show here";
		if (nar.filtered() || !nar.collection.empty())
			body = "No game on this tab is one of these:\n" + narrowText();
		else if (!nar.search.empty())
			body = "No game on this tab has \"" + nar.search + "\" in its name" +
				   (nar.show == ShowFavourites ? " among the favourites." : nar.show == ShowHidden ? " among the hidden games." : ".");
		else if (nar.show == ShowFavourites)
			body = "No game on this tab is a favourite yet.\nTriangle on a game opens its details: L1 there makes it one.";
		else
			body = "No game on this tab is hidden.\nTriangle on a game opens its details: R1 there hides it.";
		foot = "Circle shows all games again; Square opens the list's menu.";
	}
	else if (lib.source == DiscDrive)
	{
		title = "This disc cannot be played";
		body = !drive.note.empty() ? drive.note : std::string("It is not a PlayStation 2 game.");
		foot = "PlayStation 2 DVDs are read by every drive; CDs (blue underside) only by a USB DVD drive.";
	}
	else if (busy)
	{
		title = "Looking for games";
		body = (lib.source == Network) ? "Reading the folders on the share...\nA NAS that let its disks sleep takes a while to answer." :
										 "Reading the folders...";
		foot = "";
	}
	else if (lib.source == Network)
	{
		const std::vector<std::string> folders = be->networkFolders();
		std::string named;
		for (const std::string& folder : folders)
			named += (named.empty() ? "" : "\n") + folder;
		if (folders.empty())
		{
			title = "No network share yet";
			body = std::string(be->appName()) + " reads games from an SMB share (Windows sharing, a NAS) or an NFS one.\nName the folder in\n" + root +
				   "network.cfg\n\npath = 192.168.1.10/Share/Folder\nuser = guest\npassword =\n\nor, for NFS:  path = nfs://192.168.1.10/export/folder";
			foot = std::string("Then start ") + be->appName() + " again.";
		}
		else
		{
			title = selectDisc ? "No disc images on the share" : "No games on the share";
			body = named + "\n\n" + formats + "\nIf the share did not answer, crux2-boot.log in the title folder says why.";
		}
	}
	else if (lib.source == Usb)
	{
		if (!be->usbAvailable())
		{
			title = "USB drives are not available";
			body = std::string(be->appName()) + " leaves its sandbox to read USB drives, and that takes the payload loader (elfldr) "
												"running on the console, port 9021, when " +
				   be->appName() + " starts.";
			foot = std::string("Start elfldr, then start ") + be->appName() + " again.";
		}
		else
		{
			title = selectDisc ? "No disc images on a USB drive" : "No games on a USB drive";
			body = std::string("Put your games in a folder named\ncrux2, ps2, ps2iso or playstation2\nat the top of the drive.\n\n") + formats;
			foot = std::string("A drive plugged in after the start is found the next time ") + be->appName() + " starts.";
		}
	}
	else
	{
		title = selectDisc ? "No disc images found" : "Your library is empty";
		body = "Copy your games to\n" + be->shownFolder("games") + "\n\n" + formats + "\nA BIOS from your own console (without one, a free BIOS starts some games): " +
			   be->shownFolder("bios") + "\nCovers (optional): " + be->shownFolder("covers") + "<the game's serial or title>.jpg";
	}
	const float w = 1160, h = 500;
	const float x = (W - w) / 2, y = 230;
	rect(dl, x, y, w, h, col::panel, 24);
	cruxMark(dl, x + 120, y + 130, 170);
	if (busy)
	{
		// An arc going round the mark while the folders are read.
		const float turn = static_cast<float>(timeNow) * 3.2f;
		dl->PathClear();
		dl->PathArcTo(V(x + 120, y + 130), 74 * S, turn, turn + IM_PI * 0.7f, 24);
		dl->PathStroke(col::accent, 6 * S);
	}
	text(dl, bold(), 46, x + 220, y + 70, col::text, title.c_str());
	text(dl, regular(), 26, x + 220, y + 140, col::dim, body.c_str(), w - 280);
	text(dl, regular(), 24, x + 220, y + h - 64, col::faint, foot.c_str());
}

// ---- the details panel: the game, what is known of it, and what can be done with it before it
// starts.

enum DetailsPage { PageActions, PageStates };
enum { ActionPlay, ActionState, ActionOptions, ActionImage, ActionMore };

// The details' buttons for a game: a disc in a drive also has its image to make.
std::vector<int> actionsOf(const Game& g)
{
	if (g.info.source == DiscDrive)
		return {ActionPlay, ActionState, ActionOptions, ActionImage, ActionMore};
	return {ActionPlay, ActionState, ActionOptions, ActionMore};
}

struct DetailsState
{
	int page = PageActions;
	int action = 0;
	int row = 0; // in the page's list
	float scroll = 0;
	std::vector<StateInfo> states;
	std::string path; // the game they are of
	// What stands in the cover's place: 0 the cover, 1 the game's title screen, 2 a picture of it
	// being played (up and down go through those it has).
	int picture = 0;
	bool hasPicture[3] = {true, false, false}; // as the panel last found them
} det;

Game* gameByPath(const std::string& path)
{
	for (Game& g : games)
	{
		if (g.info.path == path)
			return &g;
	}
	return nullptr;
}

// The collections a game is in, to put it into one or take it out: the list comes back after
// each change, until it is left.
void collectionsOf(const std::string& path)
{
	const Game* const g = gameByPath(path);
	if (g == nullptr)
		return;
	readCollections();
	std::vector<std::string> names;
	for (const Collection& c : collections)
		names.push_back(std::string(inCollection(c, *g) ? ICON_FA_SQUARE_CHECK : ICON_FA_SQUARE) + "   " + c.name);
	names.push_back(ICON_FA_PLUS "   A new collection");
	pick("Collections of " + g->title, std::move(names), -1, [path](int choice) {
		const Game* const game = gameByPath(path);
		if (game == nullptr || choice < 0)
			return;
		if (choice < static_cast<int>(collections.size()))
		{
			Collection& c = collections[static_cast<size_t>(choice)];
			const bool in = !inCollection(c, *game);
			setInCollection(c, *game, in);
			toast(in ? "It is in \"" + c.name + "\" now." : "Taken out of \"" + c.name + "\".", 2.5f);
			collectionsOf(path);
			return;
		}
		if (collections.size() >= MOST_COLLECTIONS)
		{
			toast("There are as many collections as can be kept.", 4.f);
			return;
		}
		editText("The new collection's name", std::string(), false, [path](const std::string& typed) {
			const std::string name = keptName(typed);
			const Game* const game = gameByPath(path);
			if (name.empty() || game == nullptr)
				return;
			Collection* c = collectionNamed(name);
			if (c == nullptr)
			{
				collections.push_back(Collection{name, {}});
				c = &collections.back();
			}
			setInCollection(*c, *game, true);
			toast("\"" + name + "\" has it: a shelf of its own, and Square > Collection in the library.", 5.f);
		}, 32);
	});
}

// Details > More: what is done with a game less often.
void moreMenu(const Game& g)
{
	const GameInfo info = g.info;
	const std::string title = g.title;
	const bool renamed = info.renamed;
	std::vector<std::string> names = {ICON_FA_POWER_OFF "   Start with...", ICON_FA_PEN "   Rename", ICON_FA_LAYER_GROUP "   Collections",
		ICON_FA_IMAGES "   Fetch its pictures"};
	if (renamed)
		names.push_back(ICON_FA_ROTATE_LEFT "   Its own name again");
	pick(title, std::move(names), -1, [info, title](int choice) {
		switch (choice)
		{
			case 0:
				pick("Start " + title + " with", {"The game straight away (fast boot)", "The console's own start-up first (full boot)"}, -1,
					[info](int boot) {
						lib.details = false;
						launchGame(info, INT_MIN, boot == 0 ? 1 : 2);
					});
				break;
			case 1:
				editText("Its title in the lists", title, false, [info](const std::string& typed) {
					const std::string name = keptName(typed);
					if (name.empty())
						return;
					be->setTitle(info, name);
					toast("It is listed as \"" + name + "\" now. More > Its own name again undoes that.", 5.f);
				}, 80);
				break;
			case 2:
				collectionsOf(info.path);
				break;
			case 3:
				be->fetchPictures(&info, true);
				break;
			case 4:
				be->setTitle(info, std::string());
				toast("It has its own name again.", 3.f);
				break;
		}
	});
}

void openDetails(const Game& g)
{
	lib.details = true;
	det = DetailsState();
	det.path = g.info.path;
	if (g.info.crc != 0)
		det.states = be->states(g.info.serial, g.info.crc, true);
}

std::string stateName(const StateInfo& state)
{
	if (state.slot == -2)
		return "Saved by itself while playing";
	if (state.slot < 0)
		return state.backup ? "When it was closed the time before" : "When the game was last closed";
	return "Slot " + std::to_string(state.slot) + (state.backup ? "  (the one before)" : "");
}

void detailsInput(Game& g, bool selectDisc)
{
	if (selectDisc)
	{
		if (in.back || in.triangle)
			lib.details = false;
		else if (in.accept)
		{
			lib.details = false;
			launch(g, true);
		}
		return;
	}
	switch (det.page)
	{
		case PageActions:
			if (in.up || in.down)
			{
				// The next of the pictures the game has, round and round.
				for (int step = 1; step <= 3; step++)
				{
					const int next = (det.picture + (in.down ? step : 3 - step)) % 3;
					if (det.hasPicture[next])
					{
						det.picture = next;
						break;
					}
				}
			}
			if (in.left && det.action > 0)
				det.action--;
			if (in.right && det.action < static_cast<int>(actionsOf(g).size()) - 1)
				det.action++;
			if (in.square && setsGrouped && g.setSize > 1)
			{
				// The set's next disc: it stands for the set from here, and the panel is its own.
				const std::vector<size_t>& discs = discSets[g.set];
				size_t at = 0;
				for (size_t i = 0; i < discs.size(); i++)
				{
					if (&games[discs[i]] == &g)
						at = i;
				}
				Game& next = games[discs[(at + 1) % discs.size()]];
				frontChoice[next.set] = next.info.path;
				setsDirty = true;
				lib.flatPath = next.info.path;
				const int action = det.action;
				openDetails(next);
				det.action = action;
				return;
			}
			if (in.l1)
			{
				// A favourite, or not one any more: at once.
				g.info.favourite = !g.info.favourite;
				be->setFavourite(g.info, g.info.favourite);
				toast(g.info.favourite ? "A favourite now: it is on the Favourites shelf." : "Not a favourite any more.", 3.f);
			}
			if (in.r1)
			{
				const GameInfo info = g.info;
				if (info.hidden)
				{
					g.info.hidden = false;
					be->setHidden(info, false);
					lib.details = false;
					toast("It is listed again.", 3.f);
				}
				else
				{
					confirm("Hide " + g.title + "?",
						"It is not listed any more; nothing of it is deleted.\nTo see it again: Square in the library, Show, Hidden games; R1 in its "
						"details brings it back.",
						[info](bool yes) {
							if (!yes)
								return;
							be->setHidden(info, true);
							lib.details = false;
							toast("Hidden. Square > Show > Hidden games has it.", 5.f);
						},
						"Hide it", "Keep it");
				}
				return;
			}
			if (in.back || in.triangle)
				lib.details = false;
			else if (in.accept)
			{
				det.row = 0;
				det.scroll = 0;
				const std::vector<int> actions = actionsOf(g);
				switch (actions[static_cast<size_t>(std::clamp(det.action, 0, static_cast<int>(actions.size()) - 1))])
				{
					case ActionPlay:
						lib.details = false;
						launch(g, false);
						break;
					case ActionState:
						if (!det.states.empty())
							det.page = PageStates;
						break;
					case ActionOptions:
						openSettings(&g.info, Screen::Library);
						break;
					case ActionImage:
						be->makeImage(g.info);
						break;
					case ActionMore:
						moreMenu(g);
						break;
				}
			}
			break;

		case PageStates:
		{
			const int n = static_cast<int>(det.states.size());
			if (in.up && det.row > 0)
				det.row--;
			if (in.down && det.row < n - 1)
				det.row++;
			if (in.back)
				det.page = PageActions;
			else if (in.accept && n > 0)
			{
				const StateInfo state = det.states[static_cast<size_t>(std::clamp(det.row, 0, n - 1))];
				lib.details = false;
				setLoadingGame(g.info);
				be->startStateFile(g.info, state.path);
				setScreen(Screen::Loading);
			}
			else if (in.square && n > 0)
			{
				const StateInfo state = det.states[static_cast<size_t>(std::clamp(det.row, 0, n - 1))];
				const GameInfo info = g.info;
				confirm("Delete this state?", stateName(state) + ", saved " + formatWhen(state.when) + ".\nIt cannot be brought back.",
					[state, info](bool yes) {
						if (!yes)
							return;
						if (!be->deleteState(state))
							toast("That state could not be deleted.");
						det.states = be->states(info.serial, info.crc, true);
						if (det.states.empty())
							det.page = PageActions;
					},
					"Delete", "Keep");
			}
			break;
		}
	}
}

// A row of the facts: what it is on the left, the fact on the right of it.
float fact(ImDrawList* dl, float x, float y, float w, float t, const char* label, const std::string& value)
{
	if (value.empty())
		return y;
	text(dl, regular(), 24, x, y, alpha(col::faint, t), label);
	textIn(dl, regular(), 24, x + 230, y, alpha(col::text, t), value, w - 230, true);
	return y + 42;
}

void detailsPanel(ImDrawList* dl, Game& g, float t, bool selectDisc)
{
	dl->AddRectFilled(V(0, 96), V(W, H - 72), col::rgba(0, 0, 0, static_cast<int>(150 * t)));
	const float w = 1440, h = 760;
	const float x = (W - w) / 2, y = 96 + (H - 72 - 96 - h) / 2 + (1 - t) * 40;
	glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
	rect(dl, x, y, w, h, alpha(col::panel, t), 24);

	// The cover; on the states page, the picture the focused state was saved with.
	const float cw = 400, ch = 568;
	bool pictured = false;
	if (!selectDisc && det.page == PageStates && !det.states.empty())
	{
		const StateInfo& state = det.states[static_cast<size_t>(std::clamp(det.row, 0, static_cast<int>(det.states.size()) - 1))];
		const Texture picture = be->statePicture(state);
		rect(dl, x + 48, y + 48, cw, ch, alpha(col::card, t), 16);
		if (picture.valid())
		{
			imageFit(dl, picture, x + 48, y + 48, cw, ch, 8, alpha(col::white, t));
			pictured = true;
		}
		else
		{
			const char* none = "No picture in this state";
			const ImVec2 ns = textSize(regular(), 22, none);
			text(dl, regular(), 22, x + 48 + (cw - ns.x) / 2, y + 48 + (ch - ns.y) / 2, alpha(col::faint, t), none);
			pictured = true;
		}
	}
	if (!pictured)
	{
		// The cover; or, with up and down, one of the game's other pictures in its place.
		const Texture shots[3] = {Texture(), selectDisc ? Texture() : be->picture(g.info, 0), selectDisc ? Texture() : be->picture(g.info, 1)};
		det.hasPicture[1] = shots[1].valid();
		det.hasPicture[2] = shots[2].valid();
		if (!det.hasPicture[det.picture])
			det.picture = 0;
		if (det.picture == 0)
		{
			drawCover(dl, g, x + 48, y + 48, cw, ch, 16, 1);
		}
		else
		{
			rect(dl, x + 48, y + 48, cw, ch, alpha(col::card, t), 16);
			imageFit(dl, shots[det.picture], x + 48, y + 48, cw, ch, 8, alpha(col::white, t));
			const char* const what = (det.picture == 1) ? "Its title screen" : "In play";
			const ImVec2 ws = textSize(bold(), 18, what);
			rect(dl, x + 60, y + 60, ws.x + 24, 32, alpha(col::bar, 0.92f * t), 16);
			text(dl, bold(), 18, x + 72, y + 76 - ws.y / 2, alpha(col::text, t), what);
		}
		if (det.hasPicture[1] || det.hasPicture[2])
		{
			// Which of them is shown: a dot for each.
			int count = 0, at = 0;
			for (int i = 0; i < 3; i++)
			{
				if (!det.hasPicture[i])
					continue;
				if (i == det.picture)
					at = count;
				count++;
			}
			const float gap = 18, dx = x + 48 + cw - 20 - (count - 1) * gap;
			for (int i = 0; i < count; i++)
				dl->AddCircleFilled(V(dx + i * gap, y + 48 + ch + 34), (i == at ? 5.5f : 4.f) * S, alpha(i == at ? col::text : col::faint, t));
		}
	}
	textIn(dl, regular(), 20, x + 48, y + 48 + ch + 22, alpha(col::faint, t), g.info.fileName, cw - 90, true);

	const float tx = x + 48 + cw + 56, tw = w - (48 + cw + 56) - 56;
	{
		// The title, with a star before it when the game is a favourite.
		float titleX = tx;
		if (g.info.favourite && !selectDisc)
		{
			text(dl, regular(), 38, tx, y + 54, alpha(col::warm, t), ICON_FA_STAR);
			titleX += 56;
		}
		textIn(dl, bold(), 48, titleX, y + 48, alpha(col::text, t), g.title, tw - (titleX - tx), true);
	}
	std::string meta = kindText(g.info);
	const std::string more = metaText(g);
	if (!more.empty())
		meta += "   \xc2\xb7   " + more;
	textIn(dl, bold(), 24, tx, y + 118, alpha(kindColour(g), t), meta, tw, true);

	const float top = y + 176, bottom = y + h - 48;
	if (selectDisc || det.page == PageActions)
	{
		// What is known of the game.
		float fy = top + 6;
		if (g.info.compatibility >= 1 && g.info.compatibility <= 6)
		{
			text(dl, regular(), 24, tx, fy, alpha(col::faint, t), "Known to run");
			float sx = tx + 230;
			for (int star = 1; star <= 5; star++)
			{
				text(dl, regular(), 22, sx, fy + 1, alpha(star < g.info.compatibility ? col::warm : alpha(col::text, 40 / 255.f), t), ICON_FA_STAR);
				sx += 32;
			}
			text(dl, regular(), 24, sx + 12, fy, alpha(col::text, t), compatibilityWords[g.info.compatibility]);
			fy += 42;
		}
		fy = fact(dl, tx, fy, tw, t, "Last played", g.info.lastPlayed > 0 ? formatWhen(g.info.lastPlayed) : std::string("Not yet"));
		fy = fact(dl, tx, fy, tw, t, "Time played", formatPlayed(g.info.playedSeconds));
		fy = fact(dl, tx, fy, tw, t, "Serial", g.info.serial);
		char crc[16] = "";
		if (g.info.crc != 0)
			std::snprintf(crc, sizeof(crc), "%08X", g.info.crc);
		fy = fact(dl, tx, fy, tw, t, "CRC", crc);
		fy = fact(dl, tx, fy, tw, t, "Size", g.info.size != 0 ? formatSize(g.info.size) : std::string());
		{
			std::string folder = g.info.path;
			const size_t slash = folder.rfind('/');
			if (slash != std::string::npos)
				folder.resize(slash + 1);
			fy = fact(dl, tx, fy, tw, t, "Folder", folder);
		}
		if (!det.states.empty() && !selectDisc)
		{
			const std::string count = std::to_string(det.states.size()) + (det.states.size() == 1 ? " state" : " states");
			fy = fact(dl, tx, fy, tw, t, "Saved states", count);
		}

		// The actions, side by side.
		struct Action
		{
			const char* label;
			bool enabled;
		};
		std::vector<Action> actions;
		if (selectDisc)
			actions = {{ICON_FA_COMPACT_DISC "   Insert disc", true}};
		else
		{
			for (const int id : actionsOf(g))
			{
				switch (id)
				{
					case ActionPlay:
						actions.push_back({ICON_FA_PLAY "   Play", true});
						break;
					case ActionState:
						actions.push_back({ICON_FA_CLOCK_ROTATE_LEFT "   Load state", !det.states.empty()});
						break;
					case ActionOptions:
						actions.push_back({ICON_FA_SLIDERS "   Options", true});
						break;
					case ActionImage:
						actions.push_back({ICON_FA_FLOPPY_DISK "   Image", !be->imageBusy()});
						break;
					default:
						actions.push_back({ICON_FA_ELLIPSIS "   More", true});
				}
			}
		}
		float bx = tx;
		const float by = bottom - 68;
		if (!selectDisc && setsGrouped && g.setSize > 1)
		{
			// The set's discs: the one this panel is for, and Square for the next.
			float px = tx;
			const float py = by - 64;
			for (size_t disc : discSets[g.set])
			{
				const bool current = (&games[disc] == &g);
				const std::string label = "Disc " + std::to_string(games[disc].discOrder);
				const ImVec2 ls = textSize(bold(), 20, label.c_str());
				const float pw = ls.x + 36;
				rect(dl, px, py, pw, 40, alpha(current ? col::text : col::panelHi, t), 20);
				text(dl, bold(), 20, px + 18, py + 20 - ls.y / 2, alpha(current ? col::panel : col::dim, t), label.c_str());
				px += pw + 10;
			}
			px += 14;
			px += glyph(dl, Glyph::Square, px, py + 20, 28) + 10;
			text(dl, regular(), 20, px, py + 8, alpha(col::faint, t), "Next disc");
		}
		// (Five buttons are made smaller to stay in the panel.)
		float size = 26, pad = 52, gap = 16;
		for (int pass = 0; pass < 3; pass++)
		{
			float total = 0;
			for (const Action& action : actions)
				total += textSize(bold(), size, action.label).x + pad + gap;
			if (total - gap <= tw)
				break;
			size -= 2;
			pad -= 8;
			gap -= 3;
		}
		for (int i = 0; i < static_cast<int>(actions.size()); i++)
		{
			const bool on = selectDisc || i == det.action;
			const ImVec2 ls = textSize(bold(), size, actions[static_cast<size_t>(i)].label);
			const float bw = ls.x + pad;
			if (on)
				glow(dl, bx, by, bw, 64, 32, alpha(col::accent, 0.7f * t), 12);
			rect(dl, bx, by, bw, 64, alpha(on ? col::accent : col::panelHi, t), 32);
			text(dl, bold(), size, bx + pad / 2, by + 32 - ls.y / 2,
				alpha(!actions[static_cast<size_t>(i)].enabled ? col::faint : on ? col::text : col::dim, t), actions[static_cast<size_t>(i)].label);
			bx += bw + gap;
		}
		if (!selectDisc)
		{
			// L1 and R1 here: a favourite, and out of the list. Said under the cover.
			float hx = x + 48;
			const float hy = y + 48 + 568 + 74;
			hx += glyph(dl, Glyph::L1, hx, hy, 26) + 8;
			hx += text(dl, regular(), 20, hx, hy - 11, alpha(col::faint, t), g.info.favourite ? "Not a favourite" : "Favourite").x + 22;
			hx += glyph(dl, Glyph::R1, hx, hy, 26) + 8;
			text(dl, regular(), 20, hx, hy - 11, alpha(col::faint, t), g.info.hidden ? "List it again" : "Hide");
		}
		const int focusedAction = actionsOf(g)[static_cast<size_t>(std::clamp(det.action, 0, static_cast<int>(actionsOf(g).size()) - 1))];
		if (!selectDisc && focusedAction == ActionImage)
		{
			text(dl, regular(), 20, tx, by - 34, alpha(col::faint, t),
				be->imageBusy() ? "An image is being made: its progress is at the bottom of the library." :
								  "An ISO, a CHD or a CSO of this disc, to play from without the drive: asks which, and where it goes.");
		}
		if (!selectDisc && focusedAction == ActionState && det.states.empty() && !(setsGrouped && g.setSize > 1))
		{
			text(dl, regular(), 20, tx, by - 34, alpha(col::faint, t),
				"No saved state for this game yet: save one from the quick menu while playing.");
		}
		return;
	}

	// The states page: its name, then its rows.
	text(dl, bold(), 30, tx, top, alpha(col::text, t), ICON_FA_CLOCK_ROTATE_LEFT "   Start from a saved state");
	const float listTop = top + 58, rowH = 58;
	const float viewH = bottom - listTop;
	const int rows = static_cast<int>(det.states.size());
	det.row = std::clamp(det.row, 0, std::max(0, rows - 1));
	const float visible = viewH / rowH;
	const float target = std::clamp(static_cast<float>(det.row) - (visible - 1) / 2, 0.f, std::max(0.f, static_cast<float>(rows) - visible));
	det.scroll = approach(det.scroll, target, 18);
	dl->PushClipRect(V(tx - 16, listTop), V(tx + tw + 16, bottom), true);
	for (int i = 0; i < rows; i++)
	{
		const float ry = listTop + (i - det.scroll) * rowH;
		if (ry > bottom || ry + rowH < listTop - rowH)
			continue;
		const bool on = (i == det.row);
		if (on)
			rect(dl, tx - 12, ry + 2, tw + 24, rowH - 6, col::accent, 12);
		const StateInfo& state = det.states[static_cast<size_t>(i)];
		text(dl, bold(), 26, tx + 8, ry + 14, on ? col::text : col::dim, stateName(state).c_str());
		const std::string when = formatWhen(state.when);
		const ImVec2 ws = textSize(regular(), 24, when.c_str());
		text(dl, regular(), 24, tx + tw - 16 - ws.x, ry + 16, on ? col::text : col::faint, when.c_str());
	}
	dl->PopClipRect();
}

// ---- the shelves view: the focused game's art behind its title, shelves below

void drawShelves(ImDrawList* dl, Game* focused, bool selectDisc)
{
	const float heroTop = 96, heroH = 410;
	if (focused != nullptr)
	{
		if (focused->info.path != lib.heroPath)
		{
			lib.heroPath = focused->info.path;
			lib.heroFade = 0;
		}
		lib.heroFade = approach(lib.heroFade, 1, 8);
		// A picture of the game being played, where it has one; else its cover.
		Texture cover = options().heroPlay ? be->picture(focused->info, 1) : Texture();
		if (!cover.valid())
			cover = be->cover(focused->info);
		if (cover.valid())
		{
			// The art fills the right of the hero, fading into the backdrop from its left edge
			// and toward the shelves. It breathes: a slow push in and out, when everything moves.
			const float push = (motion() == MotionFull) ? 1.035f + 0.03f * std::sin(static_cast<float>(timeNow) * 0.21f) : 1.f;
			const float aw = W * 0.66f * push, ah = (heroH + 150) * push;
			imageFade(dl, cover, W - W * 0.66f - (aw - W * 0.66f) * 0.5f, heroTop - (ah - heroH - 150) * 0.3f, aw, ah, 0.6f * lib.heroFade,
				{0.f, 0.5f, 1.f}, {0.f, 1.f, 1.f}, {0.f, 0.55f, 1.f}, {1.f, 0.8f, 0.f});
		}
		const float tx = 96;
		const float a = lib.heroFade;
		const std::string badge = kindText(focused->info);
		const ImVec2 bs = textSize(bold(), 20, badge.c_str());
		const ImU32 tint = kindColour(*focused);
		rect(dl, tx, heroTop + 70, bs.x + 28, 36, alpha(tint, 0.25f * a), 8);
		text(dl, bold(), 20, tx + 14, heroTop + 88 - bs.y / 2, alpha(tint, a), badge.c_str());
		textIn(dl, bold(), 72, tx, heroTop + 122 + (1 - a) * 16, alpha(col::text, a), focused->title, W * 0.6f, true);
		std::string meta = metaText(*focused);
		const std::string played = formatPlayed(focused->info.playedSeconds);
		if (!played.empty())
			meta += (meta.empty() ? "" : "   \xc2\xb7   ") + ("Played " + played);
		textIn(dl, regular(), 26, tx, heroTop + 220, alpha(col::dim, a), meta, W * 0.6f, true);

		// The Play button.
		const char* play = selectDisc ? ICON_FA_COMPACT_DISC "   Insert disc" : ICON_FA_PLAY "   Play";
		const ImVec2 ps = textSize(bold(), 30, play);
		const float bw = ps.x + 96, by = heroTop + 276;
		const float pulse = (motion() == MotionFull) ? 0.5f + 0.5f * std::sin(static_cast<float>(timeNow) * 2.4f) : 0.5f;
		glow(dl, tx, by, bw, 68, 34, alpha(col::accent, 0.6f + 0.4f * pulse), 16);
		rect(dl, tx, by, bw, 68, col::accent, 34);
		glyph(dl, Glyph::Cross, tx + 18, by + 34, 34);
		text(dl, bold(), 30, tx + 66, by + 34 - ps.y / 2, col::text, play);
	}

	const float cardW = 200, cardH = std::round(cardW * CARD_SHAPE), gap = 28;
	const float shelfH = cardH + 92;
	const float shelvesTop = heroTop + heroH + 10;
	lib.shelfAnim = approach(lib.shelfAnim, static_cast<float>(lib.shelf), 12);
	dl->PushClipRect(V(0, shelvesTop - 30), V(W, H - 72), true);
	for (int si = 0; si < static_cast<int>(lib.shelves.size()); si++)
	{
		Shelf& sh = lib.shelves[static_cast<size_t>(si)];
		const float y = shelvesTop + (si - lib.shelfAnim) * shelfH;
		if (y > H || y + shelfH < shelvesTop - shelfH)
			continue;
		const bool on_shelf = (si == lib.shelf);
		const std::string header = sh.name + "   " + std::to_string(sh.items.size());
		text(dl, bold(), 28, 96, y, on_shelf ? col::text : col::faint, header.c_str());

		// Keep the focused card near the left third, as Steam does.
		const int visible = std::max(1, static_cast<int>((W - 96) / (cardW + gap)));
		const float target = std::clamp(static_cast<float>(sh.focus) - 1.f, 0.f, std::max(0.f, static_cast<float>(sh.items.size()) - visible + 0.6f));
		sh.scroll = approach(sh.scroll, target, 12);
		// The focus travels from card to card: each card is lifted by how near the focus is.
		sh.focusAnim = (sh.focusAnim < 0 || motion() == MotionOff) ? static_cast<float>(sh.focus) :
																	 approach(sh.focusAnim, static_cast<float>(sh.focus), 18);
		const float sinceEntrance = static_cast<float>(timeNow - enteredAt());
		for (int i = 0; i < static_cast<int>(sh.items.size()); i++)
		{
			const float x = 96 + (i - sh.scroll) * (cardW + gap);
			if (x > W || x + cardW < -cardW)
				continue;
			const Game& g = games[sh.items[static_cast<size_t>(i)]];
			const float near = (on_shelf && !lib.details) ? std::clamp(1.f - std::fabs(static_cast<float>(i) - sh.focusAnim), 0.f, 1.f) : 0.f;
			const bool on = on_shelf && i == sh.focus && !lib.details;
			const float grow = 18.f * near, growH = grow * CARD_SHAPE;
			const float cx = x - grow / 2, cy = y + 48 - growH / 2;
			// The cards of a screen that has just come in rise one after another.
			const bool rising = (motion() == MotionFull && sinceEntrance < 1.2f);
			const Layer card = beginLayer(dl);
			if (near > 0.02f)
			{
				const float breath = (motion() == MotionFull) ? 0.78f + 0.22f * std::sin(static_cast<float>(timeNow) * 2.4f) : 1.f;
				glow(dl, cx, cy, cardW + grow, cardH + growH, 14, alpha(col::accent, near * breath), 22);
			}
			drawCover(dl, g, cx, cy, cardW + grow, cardH + growH, 12, near);
			if (i == 0 && sh.name == "Recently played")
			{
				// The game played last: the one to go on with.
				const char* const label = ICON_FA_PLAY "  Continue";
				const ImVec2 ls = textSize(bold(), 18, label);
				const float bw = ls.x + 24, bh = 34;
				rect(dl, cx + (cardW + grow - bw) / 2, cy + cardH + growH - bh - 12, bw, bh, alpha(col::accent, 0.95f), bh / 2);
				text(dl, bold(), 18, cx + (cardW + grow - bw) / 2 + 12, cy + cardH + growH - bh - 12 + (bh - ls.y) / 2, col::text, label);
			}
			if (on && motion() == MotionFull)
			{
				// A band of light crosses the focused cover now and then.
				const float sweep = std::fmod(static_cast<float>(timeNow), 5.f) / 0.9f;
				if (sweep < 1.f)
				{
					const float w = cardW + grow, hgt = cardH + growH, bx = cx - w * 0.6f + sweep * w * 1.8f;
					dl->PushClipRect(V(cx + 4, cy + 4), V(cx + w - 4, cy + hgt - 4), true);
					const ImU32 lit = col::rgba(255, 255, 255, 34), none = col::rgba(255, 255, 255, 0);
					quad(dl, V(bx, cy), V(bx + w * 0.22f, cy), V(bx + w * 0.22f - 70, cy + hgt), V(bx - 70, cy + hgt), none, lit, lit, none);
					quad(dl, V(bx + w * 0.22f, cy), V(bx + w * 0.44f, cy), V(bx + w * 0.44f - 70, cy + hgt), V(bx + w * 0.22f - 70, cy + hgt), lit,
						none, none, lit);
					dl->PopClipRect();
				}
			}
			if (near > 0.02f)
				outline(dl, cx - 4, cy - 4, cardW + grow + 8, cardH + growH + 8, alpha(col::text, near), 15, 4);
			else if (!on_shelf)
				rect(dl, cx, cy, cardW, cardH, col::rgba(0, 0, 0, 90), 12);
			if (rising)
			{
				const float e = easeOut((sinceEntrance - 0.05f * std::min(i - static_cast<int>(sh.scroll), 8) - 0.08f * si) / 0.4f);
				endLayer(card, e, 0, (1 - e) * 34);
			}
		}
	}
	dl->PopClipRect();
}

// ---- the Disc tab: the disc in the drive, large, with what the game is and what can be done

// How far the image being made is, from the note under the library ("...: 45%..."); -1 when none.
float imageDone()
{
	if (!be->imageBusy())
		return -1;
	const std::string note = be->backgroundNote();
	const size_t percent = note.find('%');
	if (percent == std::string::npos)
		return 0;
	size_t start = percent;
	while (start > 0 && std::isdigit(static_cast<unsigned char>(note[start - 1])))
		start--;
	return start < percent ? std::clamp(std::atoi(note.substr(start, percent - start).c_str()) / 100.f, 0.f, 1.f) : 0.f;
}

// Text from an encyclopedia, with what the font has no letter for taken out (Japanese names in
// brackets, mostly): kept per text, it is asked every frame.
std::string shown(const std::string& text)
{
	static std::unordered_map<std::string, std::string> done;
	if (text.empty())
		return text;
	if (const auto it = done.find(text); it != done.end())
		return it->second;
	if (done.size() > 64)
		done.clear();
	ImFont* const font = regular();
	std::string out = StripUnshown(text, [font](char32_t c) {
		return c < 0x80 || (c <= IM_UNICODE_CODEPOINT_MAX && font->IsGlyphInFont(static_cast<ImWchar>(c)));
	});
	done.emplace(text, out);
	return out;
}

// The disc from its printed side: the cover on it as its label, turning, with the clear hub and
// the hole over it.
void labelDisc(ImDrawList* dl, const Texture& cover, float cx, float cy, float r, float spin, float a)
{
	softDisc(dl, cx + r * 0.05f, cy + r * 0.10f, r * 1.12f, col::rgba(0, 0, 0, static_cast<int>(120 * a)), 48);
	// The rim: the bare disc's edge.
	dl->AddCircleFilled(V(cx, cy), r * S, col::rgba(206, 210, 218, static_cast<int>(255 * a)), 128);
	// The label: the middle square of the cover, as a circle, turned.
	const float lr = r * 0.975f;
	float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
	if (cover.width > cover.height)
	{
		const float k = cover.height / cover.width;
		u0 = (1 - k) / 2;
		u1 = u0 + k;
	}
	else
	{
		const float k = cover.width / cover.height;
		v0 = (1 - k) / 2;
		v1 = v0 + k;
	}
	const int first = dl->VtxBuffer.Size;
	dl->AddImageRounded(cover.id, V(cx - lr, cy - lr), V(cx + lr, cy + lr), ImVec2(u0, v0), ImVec2(u1, v1), alpha(col::white, a), lr * S);
	const float cs = std::cos(spin * 0.35f), sn = std::sin(spin * 0.35f);
	const ImVec2 c = V(cx, cy);
	for (int i = first; i < dl->VtxBuffer.Size; i++)
	{
		ImVec2& p = dl->VtxBuffer[i].pos;
		const float x = p.x - c.x, y = p.y - c.y;
		p = ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
	}
	// A light that stays where it is while the disc turns: a gloss across its upper left.
	for (int i = 0; i < 6; i++)
		dl->PathArcTo(c, (lr - 6 - i * 3) * S, -2.6f, -1.2f, 48), dl->PathStroke(col::rgba(255, 255, 255, static_cast<int>((14 - i * 2) * a)), 3.f * S);
	dl->AddCircle(c, lr * S, col::rgba(255, 255, 255, static_cast<int>(60 * a)), 128, 1.5f * S);
	// The clear hub, its ring and the hole.
	dl->AddCircleFilled(c, r * 0.33f * S, col::rgba(214, 220, 232, static_cast<int>(235 * a)), 96);
	dl->AddCircle(c, r * 0.33f * S, col::rgba(255, 255, 255, static_cast<int>(90 * a)), 96, 1.5f * S);
	dl->AddCircleFilled(c, r * 0.245f * S, col::rgba(176, 184, 198, static_cast<int>(200 * a)), 96);
	dl->AddCircle(c, r * 0.245f * S, col::rgba(255, 255, 255, static_cast<int>(70 * a)), 96, 1.5f * S);
	dl->AddCircleFilled(c, r * 0.15f * S, col::rgba(8, 12, 20, static_cast<int>(240 * a)), 64);
}

// A button with its glyph: the one to press for it, then what it does. Returns its width.
float discButton(ImDrawList* dl, float x, float y, Glyph g, const char* label, bool main, bool enabled, float a)
{
	const ImVec2 ls = textSize(bold(), 26, label);
	const float w = ls.x + 96, h = 64;
	if (main)
	{
		const float pulse = (motion() == MotionFull) ? 0.5f + 0.5f * std::sin(static_cast<float>(timeNow) * 2.4f) : 0.5f;
		glow(dl, x, y, w, h, h / 2, alpha(col::accent, (0.55f + 0.35f * pulse) * a), 16);
	}
	rect(dl, x, y, w, h, alpha(main ? col::accent : col::panelHi, a), h / 2);
	if (!main)
		outline(dl, x, y, w, h, alpha(col::text, 0.10f * a), h / 2, 1.5f);
	glyph(dl, g, x + 16, y + h / 2, 34);
	text(dl, bold(), 26, x + 66, y + h / 2 - ls.y / 2, alpha(!enabled ? col::faint : col::text, a), label);
	return w;
}

void drawDiscBay(ImDrawList* dl, Game* focused)
{
	if (focused == nullptr)
		return;
	if (focused->info.path != lib.heroPath)
	{
		lib.heroPath = focused->info.path;
		lib.heroFade = 0;
	}
	lib.heroFade = approach(lib.heroFade, 1, 6);
	const float a = lib.heroFade;
	const GameInfo& info = focused->info;
	GameAbout about = be->about(info);
	for (std::string* field : {&about.summary, &about.developer, &about.publisher, &about.genre, &about.modes})
		*field = shown(*field);
	const bool full = motion() == MotionFull;
	const float tnow = static_cast<float>(timeNow);

	// ---- the disc, on the right: out of its case where there is a cover, turning; a ring of
	// light around it while its image is made.
	const float cx = W - 470, cy = 392, r = 236;
	const float slide = (1 - easeOut(a)) * 160;
	softDisc(dl, cx, cy, r * 1.9f, alpha(col::accent, (0.10f + (full ? 0.04f * std::sin(tnow * 0.8f) : 0.f)) * a), 64);
	// (With a cover, it is the disc's label: the disc shows its printed side.)
	const Texture cover = be->cover(info);
	const float dx = cx + slide;
	if (cover.valid())
		labelDisc(dl, cover, dx, cy, r, discSpin(), a);
	else
		goldDisc(dl, dx, cy, r, discSpin(), a, info.cd);
	const float done = imageDone();
	if (done >= 0)
	{
		// The part read so far, around the disc.
		const float ring = r + 26;
		dl->PathArcTo(V(dx, cy), ring * S, 0, 2 * IM_PI, 96);
		dl->PathStroke(alpha(col::text, 0.12f * a), 8 * S);
		if (done > 0.001f)
		{
			dl->PathArcTo(V(dx, cy), ring * S, -IM_PI / 2, -IM_PI / 2 + 2 * IM_PI * done, 96);
			dl->PathStroke(alpha(col::accent, a), 8 * S);
		}
		char pct[32];
		std::snprintf(pct, sizeof(pct), "Image  %d%%", static_cast<int>(done * 100));
		const ImVec2 ps = textSize(bold(), 22, pct);
		rect(dl, dx - ps.x / 2 - 18, cy + ring + 18, ps.x + 36, 40, alpha(col::sheet, 0.94f * a), 20);
		text(dl, bold(), 22, dx - ps.x / 2, cy + ring + 38 - ps.y / 2, alpha(col::text, a), pct);
	}

	// Its title screen and a picture of it being played, under the disc.
	{
		const Texture shots[2] = {be->picture(info, 0), be->picture(info, 1)};
		const char* const names[2] = {"Title screen", "In play"};
		const int count = (shots[0].valid() ? 1 : 0) + (shots[1].valid() ? 1 : 0);
		if (count > 0 && done < 0)
		{
			const float sw = 300, sh = 225, gap = 24;
			float sx = cx - (count * sw + (count - 1) * gap) / 2;
			const float sy = cy + r + 44 + (1 - a) * 30;
			for (int i = 0; i < 2; i++)
			{
				if (!shots[i].valid())
					continue;
				rect(dl, sx + 6, sy + 10, sw, sh, col::rgba(0, 0, 0, static_cast<int>(90 * a)), 14);
				rect(dl, sx, sy, sw, sh, alpha(col::card, a), 14);
				imageFill(dl, shots[i], sx, sy, sw, sh, 14, alpha(col::white, a));
				const ImVec2 ns = textSize(bold(), 17, names[i]);
				rect(dl, sx + 12, sy + 12, ns.x + 22, 30, alpha(col::bar, 0.9f * a), 15);
				text(dl, bold(), 17, sx + 23, sy + 27 - ns.y / 2, alpha(col::text, a), names[i]);
				sx += sw + gap;
			}
		}
	}

	// ---- what it is, on the left
	const float tx = 96, tw = 1010;
	float y = 150 + (1 - a) * 18;
	{
		// Pills: the console, the kind of disc, its drive; with two discs, which of them this is.
		std::vector<std::pair<std::string, ImU32>> pills;
		pills.emplace_back(kindText(info), kindColour(*focused));
		const std::string kind = info.cd ? "CD" : (info.size > 4700000000ull ? "DVD, two layers" : "DVD");
		pills.emplace_back(kind, col::dim);
		const bool own = info.path.ends_with("cd0");
		pills.emplace_back(own ? "The console's drive" : "USB drive", col::dim);
		if (lib.shelves.size() == 1 && lib.shelves[0].items.size() > 1)
			pills.emplace_back(std::to_string(lib.shelves[0].focus + 1) + " of " + std::to_string(lib.shelves[0].items.size()) + " discs", col::warm);
		float px = tx;
		for (const auto& [label, tint] : pills)
		{
			const ImVec2 ls = textSize(bold(), 19, label.c_str());
			rect(dl, px, y, ls.x + 28, 36, alpha(tint, 0.20f * a), 18);
			text(dl, bold(), 19, px + 14, y + 18 - ls.y / 2, alpha(tint == col::dim ? col::text : tint, a), label.c_str());
			px += ls.x + 40;
		}
	}
	y += 52;
	textIn(dl, bold(), 66, tx, y, alpha(col::text, a), focused->title, tw, true);
	y += 86;
	{
		// The serial, the region, how well it runs.
		std::string meta = info.serial;
		if (!info.region.empty())
			meta += "   \xc2\xb7   " + info.region;
		const ImVec2 ms = text(dl, regular(), 25, tx, y, alpha(col::dim, a), meta.c_str());
		if (info.compatibility >= 1 && info.compatibility <= 6)
		{
			float sx = tx + ms.x + 32;
			for (int star = 1; star <= 5; star++)
			{
				text(dl, regular(), 21, sx, y + 2, alpha(star < info.compatibility ? col::warm : alpha(col::text, 40 / 255.f), a), ICON_FA_STAR);
				sx += 28;
			}
			text(dl, regular(), 25, sx + 8, y, alpha(col::dim, a), compatibilityWords[info.compatibility]);
		}
	}
	y += 56;

	// The facts: who made it, who published it, when, what kind of game.
	{
		struct Fact
		{
			const char* label;
			std::string value;
		};
		std::vector<Fact> facts;
		if (!about.developer.empty())
			facts.push_back({"DEVELOPER", about.developer});
		if (!about.publisher.empty())
			facts.push_back({"PUBLISHER", about.publisher});
		if (!about.released.empty())
			facts.push_back({"RELEASED", about.released});
		if (!about.genre.empty())
			facts.push_back({"GENRE", about.genre});
		if (facts.size() < 4 && !about.modes.empty())
			facts.push_back({"PLAYERS", about.modes});
		if (facts.empty())
		{
			// Until it is known, the disc's own facts.
			facts.push_back({"SIZE", info.size ? formatSize(info.size) : std::string("-")});
			facts.push_back({"PLAYED", info.playedSeconds > 0 ? formatPlayed(info.playedSeconds) : std::string("Not yet")});
			if (info.lastPlayed > 0)
				facts.push_back({"LAST PLAYED", formatWhen(info.lastPlayed)});
		}
		// Two by two: room for a studio's whole name.
		const float cellW = tw / 2;
		for (size_t i = 0; i < facts.size() && i < 4; i++)
		{
			const float fx = tx + cellW * static_cast<float>(i % 2), fy = y + 66 * static_cast<float>(i / 2);
			const float e = full ? easeOut(std::clamp(a * 1.6f - 0.15f * static_cast<float>(i), 0.f, 1.f)) : a;
			if (i % 2 == 1)
				dl->AddLine(V(fx - 22, fy + 6), V(fx - 22, fy + 54), alpha(col::text, 0.10f * a), 1.5f * S);
			text(dl, bold(), 16, fx, fy + (1 - e) * 10, alpha(col::faint, e), facts[i].label);
			textIn(dl, bold(), 24, fx, fy + 22 + (1 - e) * 10, alpha(col::text, e), facts[i].value, cellW - 44, true);
		}
		y += facts.size() > 2 ? 132 : 66;
	}
	y += 22;

	// A few sentences of it, or what is happening with them.
	{
		const float maxH = 5 * 30;
		if (!about.summary.empty())
		{
			dl->PushClipRect(V(tx, y), V(tx + tw, y + maxH), true);
			const ImVec2 ss = text(dl, regular(), 25, tx, y, alpha(mix(col::text, col::dim, 0.35f), a), about.summary.c_str(), tw - 20);
			dl->PopClipRect();
			float sy = y + std::min(ss.y, maxH);
			if (ss.y > maxH)
			{
				// (Cut: it fades out where it is cut.)
				gradientV(dl, tx, y + maxH - 40, tw, 40, alpha(col::bgTop, 0.f), alpha(col::bgTop, 0.85f * a));
			}
			if (!about.source.empty())
				text(dl, regular(), 17, tx, sy + 10, alpha(col::faint, a), ("From " + about.source).c_str());
		}
		else if (about.looking)
		{
			// Lines that shimmer while it is looked up.
			for (int i = 0; i < 4; i++)
			{
				const float lw = (i == 3) ? tw * 0.55f : tw - 40 - 30.f * static_cast<float>(i % 2);
				const float shimmer = full ? 0.06f + 0.05f * std::sin(tnow * 3.f - static_cast<float>(i) * 0.6f) : 0.08f;
				rect(dl, tx, y + 6 + i * 38, lw, 20, alpha(col::text, shimmer * a), 10);
			}
			text(dl, regular(), 17, tx, y + 6 + 4 * 38, alpha(col::faint, a), "Looking it up on Wikipedia...");
		}
		else
		{
			std::string line = std::string("A PlayStation 2 ") + (info.cd ? "CD" : "DVD");
			if (info.compatibility >= 1 && info.compatibility <= 6)
				line += std::string(" that is known to run: ") + compatibilityWords[info.compatibility] + ".";
			else
				line += ".";
			line += " Cross plays it from the drive; Square makes an image of it, to play from without the disc (quicker loading, no noise).";
			text(dl, regular(), 25, tx, y, alpha(col::dim, a), line.c_str(), tw - 20);
		}
	}

	// ---- what can be done: play, make an image, the details
	{
		const float by = H - 72 - 64 - 150;
		float bx = tx;
		bx += discButton(dl, bx, by, Glyph::Cross, ICON_FA_PLAY "  Play", true, true, a) + 18;
		const bool busy = be->imageBusy();
		bx += discButton(dl, bx, by, Glyph::Square, busy ? ICON_FA_FLOPPY_DISK "  Image being made" : ICON_FA_FLOPPY_DISK "  Make an image", false, !busy, a) + 18;
		discButton(dl, bx, by, Glyph::Triangle, ICON_FA_CIRCLE_INFO "  Details", false, true, a);

		// The disc's figures, small, under them.
		const float sy = by + 92;
		std::vector<std::string> bits;
		if (info.size)
			bits.push_back(ICON_FA_COMPACT_DISC "  " + formatSize(info.size));
		bits.push_back(ICON_FA_CLOCK "  " + (info.playedSeconds > 0 ? "Played " + formatPlayed(info.playedSeconds) : std::string("Not played yet")));
		if (info.lastPlayed > 0)
			bits.push_back(ICON_FA_CALENDAR "  Last " + formatWhen(info.lastPlayed));
		if (info.hasStates)
			bits.push_back(ICON_FA_CLOCK_ROTATE_LEFT "  Saved states");
		float fx = tx;
		for (const std::string& bit : bits)
		{
			const ImVec2 bs = text(dl, regular(), 21, fx, sy, alpha(col::faint, a), bit.c_str());
			fx += bs.x + 34;
		}
	}
}

// ---- the grid view: every game's cover, eight to a row

// The focused game, named above the grid and the bookcase, and how many games there are.
void gameHeader(ImDrawList* dl, const Game* focused, float top)
{
	if (focused != nullptr)
	{
		textIn(dl, bold(), 44, 96, top + 34, col::text, focused->title, W - 96 - 520, true);
		const std::string badge = kindText(focused->info);
		const std::string meta = metaText(*focused);
		const ImU32 tint = kindColour(*focused);
		const ImVec2 bs = textSize(bold(), 20, badge.c_str());
		rect(dl, 96, top + 96, bs.x + 24, 32, alpha(tint, 0.25f), 8);
		text(dl, bold(), 20, 108, top + 112 - bs.y / 2, tint, badge.c_str());
		text(dl, regular(), 24, 96 + bs.x + 44, top + 98, col::dim, meta.c_str());
	}
	const std::string count = std::to_string(lib.flat.size()) + (lib.flat.size() == 1 ? " game" : " games");
	const ImVec2 cs = textSize(regular(), 24, count.c_str());
	text(dl, regular(), 24, W - 96 - cs.x, top + 98, col::faint, count.c_str());
}

void drawGrid(ImDrawList* dl, Game* focused)
{
	const float top = 96;
	gameHeader(dl, focused, top);

	const float gap = 24;
	const float cardW = (W - 192 - gap * (GridColumns - 1)) / GridColumns;
	const float cardH = std::round(cardW * CARD_SHAPE);
	const float gridTop = top + 160;
	const float viewH = H - 72 - gridTop;
	const int rows = (static_cast<int>(lib.flat.size()) + GridColumns - 1) / GridColumns;
	const int focusRow = lib.flatFocus / GridColumns;
	const float rowH = cardH + gap;
	const float visibleRows = viewH / rowH;
	const float target =
		std::clamp(static_cast<float>(focusRow) - (visibleRows - 1) / 2, 0.f, std::max(0.f, static_cast<float>(rows) - visibleRows + 0.1f));
	lib.flatScroll = approach(lib.flatScroll, target, 14);
	{
		const float column = static_cast<float>(lib.flatFocus % GridColumns), row = static_cast<float>(lib.flatFocus / GridColumns);
		const bool jump = lib.ringColumn < 0 || motion() == MotionOff;
		lib.ringColumn = jump ? column : approach(lib.ringColumn, column, 20);
		lib.ringRow = jump ? row : approach(lib.ringRow, row, 20);
	}
	dl->PushClipRect(V(0, gridTop - 20), V(W, H - 72), true);
	for (int i = 0; i < static_cast<int>(lib.flat.size()); i++)
	{
		const int row = i / GridColumns, column = i % GridColumns;
		const float y = gridTop + (row - lib.flatScroll) * rowH;
		if (y > H || y + cardH < gridTop - cardH)
			continue;
		const float x = 96 + column * (cardW + gap);
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		// Lifted by how near the travelling focus is.
		const float near = lib.details ? 0.f :
										 std::clamp(1.f - std::fabs(column - lib.ringColumn), 0.f, 1.f) *
											 std::clamp(1.f - std::fabs(row - lib.ringRow), 0.f, 1.f);
		const float grow = 14.f * near, growH = grow * CARD_SHAPE;
		if (near > 0.02f)
			glow(dl, x - grow / 2, y - growH / 2, cardW + grow, cardH + growH, 12, alpha(col::accent, near), 20);
		drawCover(dl, g, x - grow / 2, y - growH / 2, cardW + grow, cardH + growH, 10, near);
		if (near > 0.02f)
			outline(dl, x - grow / 2 - 4, y - growH / 2 - 4, cardW + grow + 8, cardH + growH + 8, alpha(col::text, near), 13, 4);
	}
	dl->PopClipRect();
}

// ---- the list view: titles on the left, the focused game on the right

void drawList(ImDrawList* dl, Game* focused, bool selectDisc)
{
	const float top = 96 + 40;
	const float listX = 96, listW = W * 0.56f - 96;
	const float rowH = 76;
	const float viewH = H - 72 - top - 24;
	const float visibleRows = viewH / rowH;
	const float target =
		std::clamp(static_cast<float>(lib.flatFocus) - (visibleRows - 1) / 2, 0.f, std::max(0.f, static_cast<float>(lib.flat.size()) - visibleRows));
	lib.flatScroll = approach(lib.flatScroll, target, 16);
	rect(dl, listX - 16, top - 12, listW + 32, viewH + 24, col::panel, 18);
	dl->PushClipRect(V(listX - 16, top), V(listX + listW + 16, top + viewH), true);
	// The highlight slides from row to row.
	lib.listRow = (lib.listRow < 0 || motion() == MotionOff) ? static_cast<float>(lib.flatFocus) :
															   approach(lib.listRow, static_cast<float>(lib.flatFocus), 22);
	if (!lib.details)
		rect(dl, listX, top + (lib.listRow - lib.flatScroll) * rowH + 2, listW, rowH - 4, col::accent, 12);
	for (int i = 0; i < static_cast<int>(lib.flat.size()); i++)
	{
		const float y = top + (i - lib.flatScroll) * rowH;
		if (y > H || y + rowH < top - rowH)
			continue;
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		const bool on = (i == lib.flatFocus && !lib.details);
		const Texture cover = be->cover(g.info);
		const float th = rowH - 12, tw = std::round(th / CARD_SHAPE);
		if (cover.valid())
			imageFill(dl, cover, listX + 14, y + 6, tw, th, 6);
		else
			rect(dl, listX + 14, y + 6, tw, th, col::card, 6);
		const std::string side = !g.info.serial.empty() ? g.info.serial : std::string(kindText(g.info));
		const ImVec2 ss = textSize(regular(), 20, side.c_str());
		textIn(dl, bold(), 28, listX + tw + 36, y + 22, on ? col::text : col::dim, g.title, listW - tw - 84 - ss.x, on);
		text(dl, regular(), 20, listX + listW - 24 - ss.x, y + 28,
			on ? col::text : (g.info.kind != GameInfo::PS2 ? alpha(col::warm, 0.8f) : col::faint), side.c_str());
	}
	dl->PopClipRect();

	if (focused == nullptr)
		return;
	const float px = listX + listW + 64, pw = W - px - 96;
	const float coverH = 430, coverW = std::round(coverH / CARD_SHAPE);
	drawCover(dl, *focused, px, top, coverW, coverH, 16, 1);
	// Beside the cover: the title and what is known of the game.
	const float ix = px + coverW + 40, iw = pw - coverW - 40;
	float y = top + 4;
	y += text(dl, bold(), 38, ix, y, col::text, focused->title.c_str(), iw).y + 14;
	const ImU32 tint = kindColour(*focused);
	y += text(dl, bold(), 22, ix, y, tint, kindText(focused->info), iw).y + 14;
	const auto line = [&](const std::string& s) {
		if (!s.empty())
			y += text(dl, regular(), 22, ix, y, col::dim, s.c_str(), iw).y + 8;
	};
	line(focused->info.region);
	line(focused->info.serial);
	if (focused->info.compatibility >= 1 && focused->info.compatibility <= 6)
		line(compatibilityWords[focused->info.compatibility]);
	if (setsGrouped && focused->setSize > 1)
		line("Disc " + std::to_string(focused->discOrder) + " of " + std::to_string(focused->setSize));

	y = top + coverH + 40;
	const auto row = [&](const char* label, const std::string& value) {
		if (value.empty())
			return;
		text(dl, regular(), 22, px, y, col::faint, label);
		textIn(dl, regular(), 22, px + 190, y, col::text, value, pw - 190, true);
		y += 38;
	};
	row("Last played", focused->info.lastPlayed > 0 ? formatWhen(focused->info.lastPlayed) : std::string("Not yet"));
	row("Time played", formatPlayed(focused->info.playedSeconds));
	row("Size", focused->info.size != 0 ? formatSize(focused->info.size) : std::string());
	row("File", focused->info.fileName);

	// The Play button.
	const char* play = selectDisc ? ICON_FA_COMPACT_DISC "   Insert disc" : ICON_FA_PLAY "   Play";
	const ImVec2 ps = textSize(bold(), 28, play);
	const float bw = ps.x + 90, by = H - 72 - 110;
	rect(dl, px, by, bw, 62, col::accent, 31);
	glyph(dl, Glyph::Cross, px + 16, by + 31, 32);
	text(dl, bold(), 28, px + 62, by + 31 - ps.y / 2, col::text, play);
}

// ---- the flow view: the covers in a row, the focused one facing, the others turned aside

// The part of a picture that fills a box of another shape, its centre kept (as imageFill has it).
void coverCrop(const Texture& t, float w, float h, ImVec2& uv0, ImVec2& uv1)
{
	const float ar = (t.height > 0) ? t.width / t.height : 1.f;
	const float boxAr = w / h;
	uv0 = ImVec2(0, 0);
	uv1 = ImVec2(1, 1);
	if (ar > boxAr)
	{
		const float k = boxAr / ar;
		uv0.x = (1 - k) / 2;
		uv1.x = 1 - uv0.x;
	}
	else
	{
		const float k = ar / boxAr;
		uv0.y = (1 - k) / 2;
		uv1.y = 1 - uv0.y;
	}
}

// A colour of a game's own, for where it has no cover to give one: from its title.
ImU32 titleColour(const std::string& title, float saturation, float value)
{
	unsigned hash = 2166136261u;
	for (char c : title)
		hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB((hash % 360) / 360.f, saturation, value, r, g, b);
	return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.f));
}

void drawFlow(ImDrawList* dl, Game* focused)
{
	const int n = static_cast<int>(lib.flat.size());
	const float focus = static_cast<float>(lib.flatFocus);
	lib.flowAt = (lib.flowAt < 0 || motion() == MotionOff) ? focus : approach(lib.flowAt, focus, 11);
	const float mid = W / 2, cy = 96 + 58 + 270;
	const float ch = 540, cw = std::round(ch / CARD_SHAPE);
	// A cover turned aside is narrower and a little smaller; the first of them stands clear of
	// the one that faces, the others overlap behind it.
	const float sideW = cw * 0.44f, first = cw * 0.5f + 92 + sideW * 0.5f, spacing = 132;
	const int reach = static_cast<int>((W / 2 - first) / spacing) + 3;
	const int lo = std::max(0, static_cast<int>(std::floor(lib.flowAt)) - reach), hi = std::min(n - 1, static_cast<int>(std::ceil(lib.flowAt)) + reach);
	std::vector<int> order;
	for (int i = lo; i <= hi; i++)
		order.push_back(i);
	// The farthest first: each one nearer is drawn over them.
	std::sort(order.begin(), order.end(), [](int a, int b) { return std::fabs(a - lib.flowAt) > std::fabs(b - lib.flowAt); });
	for (int i : order)
	{
		const float d = static_cast<float>(i) - lib.flowAt, away = std::fabs(d), a = std::min(away, 1.f), side = (d < 0) ? -1.f : 1.f;
		const float x = mid + side * (a * first + std::max(0.f, away - 1) * spacing);
		const float h = ch * (1 - 0.24f * a), w = cw + (sideW - cw) * a;
		const float farH = h * (1 - 0.15f * a);
		const float lit = 1 - a * (0.36f + std::min(0.30f, 0.07f * std::max(0.f, away - 1)));
		const float leftH = (side < 0) ? farH : h, rightH = (side < 0) ? h : farH;
		const ImVec2 tl = V(x - w / 2, cy - leftH / 2), tr = V(x + w / 2, cy - rightH / 2), br = V(x + w / 2, cy + rightH / 2),
					 bl = V(x - w / 2, cy + leftH / 2);
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		const ImVec2 drop = V(7, 14);
		dl->AddQuadFilled(tl + drop, tr + drop, br + drop, bl + drop, col::rgba(0, 0, 0, 95));
		const Texture cover = be->cover(g.info);
		if (cover.valid())
		{
			ImVec2 uv0, uv1;
			coverCrop(cover, cw, ch, uv0, uv1);
			const int c = static_cast<int>(255 * lit);
			dl->AddImageQuad(cover.id, tl, tr, br, bl, uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), col::rgba(c, c, c));
		}
		else if (a < 0.3f)
		{
			// (Nearly) facing: the card a game without a cover has everywhere, with its title.
			placeholder(dl, g.title, x - w / 2, cy - h / 2, w, h, 6);
		}
		else
		{
			dl->AddQuadFilled(tl, tr, br, bl, mix(col::rgba(0, 0, 0), mix(col::card, titleColour(g.title, 0.55f, 0.42f), 0.75f), lit));
		}
		if (a < 0.98f && !lib.details)
			dl->AddQuad(tl - V(3, 3), tr + V(3, -3), br + V(3, 3), bl + V(-3, 3), alpha(col::text, 1 - a), 4 * S);
	}

	// What faces, said under it.
	if (focused != nullptr)
	{
		const float ty = cy + ch / 2 + 36;
		const std::string title = fit(bold(), 46, focused->title, W * 0.72f);
		const ImVec2 ts = textSize(bold(), 46, title.c_str());
		text(dl, bold(), 46, mid - ts.x / 2, ty, col::text, title.c_str());
		std::string meta = kindText(focused->info);
		const std::string more = metaText(*focused);
		if (!more.empty())
			meta += "   \xc2\xb7   " + more;
		meta = fit(regular(), 24, meta, W * 0.72f);
		const ImVec2 ms = textSize(regular(), 24, meta.c_str());
		text(dl, regular(), 24, mid - ms.x / 2, ty + 64, col::dim, meta.c_str());
	}
	// Where in the row this is: a line, and a mark on it.
	if (n > 1)
	{
		const float lw = W * 0.36f, lx = mid - lw / 2, ly = H - 72 - 44;
		rect(dl, lx, ly, lw, 4, alpha(col::text, 30 / 255.f), 2);
		const float at = lib.flowAt / static_cast<float>(n - 1);
		rect(dl, lx + (lw - 56) * std::clamp(at, 0.f, 1.f), ly - 2, 56, 8, col::accent, 4);
		const std::string where = std::to_string(lib.flatFocus + 1) + " of " + std::to_string(n);
		const ImVec2 ws = textSize(regular(), 20, where.c_str());
		text(dl, regular(), 20, lx + lw + 24, ly + 2 - ws.y / 2, col::faint, where.c_str());
	}
}

// ---- the bookcase view: the games as books on shelves, their backs out; the focused one is
// pulled out and shows its front

// Text turned a quarter, its middle at (cx, cy): it reads from the bottom up.
void textTurned(ImDrawList* dl, ImFont* font, float size, float cx, float cy, ImU32 c, const char* s)
{
	const ImVec2 ts = textSize(font, size, s);
	// (Written in the middle of the clip rectangle, then turned and put in its place: ImGui leaves
	// out the letters that are outside the rectangle at the moment they are written.)
	const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax();
	const float ax = std::round((lo.x + hi.x) / 2), ay = std::round((lo.y + hi.y) / 2);
	const int from = dl->VtxBuffer.Size;
	dl->AddText(font, size * S, ImVec2(ax - std::round(ts.x * S / 2), ay - std::round(ts.y * S / 2)), c, s);
	const float px = std::round(cx * S), py = std::round(cy * S);
	for (int i = from; i < dl->VtxBuffer.Size; i++)
	{
		ImDrawVert& v = dl->VtxBuffer[i];
		const float dx = v.pos.x - ax, dy = v.pos.y - ay;
		v.pos.x = px + dy;
		v.pos.y = py - dx;
	}
}

void bookSpine(ImDrawList* dl, const Game& g, float x, float y, float w, float h)
{
	rect(dl, x, y, w, h, titleColour(g.title, 0.50f, 0.34f), 4);
	const Texture cover = be->cover(g.info);
	if (cover.valid())
	{
		// A strip of the cover gives the book its own colours; darker, so that its title can be read.
		dl->AddImage(cover.id, V(x, y), V(x + w, y + h), ImVec2(0.05f, 0.f), ImVec2(0.21f, 1.f), col::rgba(255, 255, 255, 235));
		rect(dl, x, y, w, h, col::rgba(0, 0, 0, 125));
	}
	gradientH(dl, x, y, w * 0.3f, h, col::rgba(255, 255, 255, 34), col::rgba(255, 255, 255, 0));
	gradientH(dl, x + w * 0.62f, y, w * 0.38f, h, col::rgba(0, 0, 0, 0), col::rgba(0, 0, 0, 100));
	const ImU32 band = alpha(kindColour(g), 0.95f);
	rect(dl, x, y + 12, w, 5, band);
	rect(dl, x, y + h - 17, w, 5, band);
	float room = h - 64, shift = 0;
	if (g.info.favourite)
	{
		const ImVec2 ss = textSize(regular(), 17, ICON_FA_STAR);
		text(dl, regular(), 17, x + (w - ss.x) / 2, y + 26, col::warm, ICON_FA_STAR);
		room -= 28;
		shift = 14;
	}
	const std::string shown = fit(bold(), 21, g.title, room);
	textTurned(dl, bold(), 21, x + w / 2, y + h / 2 + shift, col::rgba(255, 255, 255, 236), shown.c_str());
}

void drawBookcase(ImDrawList* dl, Game* focused)
{
	const float top = 96;
	gameHeader(dl, focused, top);
	const int columns = bookColumns();
	const int n = static_cast<int>(lib.flat.size());
	const float caseTop = top + 184, rowH = BookH + BookRowGap, viewH = H - 72 - caseTop;
	const int rows = (n + columns - 1) / columns, focusRow = lib.flatFocus / columns;
	const float visibleRows = viewH / rowH;
	const float target =
		std::clamp(static_cast<float>(focusRow) - (visibleRows - 1) / 2, 0.f, std::max(0.f, static_cast<float>(rows) - visibleRows + 0.12f));
	lib.flatScroll = approach(lib.flatScroll, target, 14);
	{
		const float column = static_cast<float>(lib.flatFocus % columns), row = static_cast<float>(focusRow);
		const bool jump = lib.ringColumn < 0 || motion() == MotionOff;
		lib.ringColumn = jump ? column : approach(lib.ringColumn, column, 20);
		lib.ringRow = jump ? row : approach(lib.ringRow, row, 20);
	}
	const float coverW = bookCoverW();
	dl->PushClipRect(V(0, caseTop - 44), V(W, H - 72), true);
	for (int row = 0; row < rows; row++)
	{
		const float y = caseTop + (row - lib.flatScroll) * rowH;
		if (y > H || y + rowH < caseTop - rowH)
			continue;
		// The board the books stand on, its lit edge, and its shadow on the wall behind.
		rect(dl, 64, y + BookH, W - 128, 16, col::panelHi, 5);
		rect(dl, 68, y + BookH, W - 136, 3, alpha(col::text, 28 / 255.f), 2);
		gradientV(dl, 64, y + BookH + 16, W - 128, 28, col::rgba(0, 0, 0, 95), col::rgba(0, 0, 0, 0));
		float x = 96;
		for (int column = 0; column < columns; column++)
		{
			const int i = row * columns + column;
			if (i >= n)
				break;
			const Game& g = games[lib.flat[static_cast<size_t>(i)]];
			const float near = lib.details ? 0.f :
											 std::clamp(1.f - std::fabs(column - lib.ringColumn), 0.f, 1.f) *
												 std::clamp(1.f - std::fabs(row - lib.ringRow), 0.f, 1.f);
			const float w = SpineW + (coverW - SpineW) * near;
			if (near > 0.5f)
			{
				// Pulled out: its front, standing a little higher than the others.
				const float up = 24 * near;
				glow(dl, x, y - up, w, BookH, 10, alpha(col::accent, near), 18);
				drawCover(dl, g, x, y - up, w, BookH, 8, near);
				outline(dl, x - 4, y - up - 4, w + 8, BookH + 8, alpha(col::text, near), 11, 4);
			}
			else
			{
				bookSpine(dl, g, x, y, w, BookH);
			}
			x += w + SpineGap;
		}
	}
	dl->PopClipRect();
}

// ---- the table view: a row for each game, with what is known of it in columns

void drawTable(ImDrawList* dl, Game* focused)
{
	const float top = 96 + 36, x0 = 96, w = W - 192, headH = 50, rowH = 56;
	const float viewTop = top + headH, viewH = H - 72 - viewTop - 28;
	const int n = static_cast<int>(lib.flat.size());
	const float visibleRows = viewH / rowH;
	const float target =
		std::clamp(static_cast<float>(lib.flatFocus) - (visibleRows - 1) / 2, 0.f, std::max(0.f, static_cast<float>(n) - visibleRows));
	lib.flatScroll = approach(lib.flatScroll, target, 16);
	rect(dl, x0 - 16, top - 12, w + 32, headH + viewH + 24, col::panel, 18);

	// The columns after the title, and the order each stands for (-1: none).
	struct Column
	{
		const char* name;
		float width;
		int order;
	};
	static const Column columns[] = {{"Region", 150, -1}, {"Known to run", 230, -1}, {"Last played", 250, 1}, {"Time played", 190, 2}, {"Size", 150, 3}};
	float fixed = 0;
	for (const Column& c : columns)
		fixed += c.width;
	const float titleX = x0 + 64, titleW = w - fixed - 64 - 24;
	const int sort = options().sort;
	const auto head = [&](const char* name, float x, bool ordered) {
		const float used = text(dl, bold(), 20, x, top + 10, ordered ? col::accent : col::faint, name).x;
		if (ordered)
			text(dl, regular(), 18, x + used + 10, top + 12, col::accent, ICON_FA_CARET_DOWN);
	};
	head(sort == 4 ? "Title  (the newest file first)" : "Title", titleX, sort == 0 || sort == 4);
	{
		float x = x0 + w - fixed;
		for (const Column& c : columns)
		{
			head(c.name, x, c.order == sort);
			x += c.width;
		}
	}
	rect(dl, x0, top + headH - 6, w, 2, col::line);

	dl->PushClipRect(V(x0 - 16, viewTop), V(x0 + w + 16, viewTop + viewH), true);
	lib.listRow = (lib.listRow < 0 || motion() == MotionOff) ? static_cast<float>(lib.flatFocus) :
															   approach(lib.listRow, static_cast<float>(lib.flatFocus), 22);
	if (!lib.details)
		rect(dl, x0, viewTop + (lib.listRow - lib.flatScroll) * rowH + 2, w, rowH - 4, col::accent, 10);
	for (int i = 0; i < n; i++)
	{
		const float y = viewTop + (i - lib.flatScroll) * rowH;
		if (y > H || y + rowH < viewTop - rowH)
			continue;
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		const bool on = (i == lib.flatFocus && !lib.details);
		const ImU32 main = on ? col::text : col::dim, side = on ? col::text : col::faint;
		const Texture cover = be->cover(g.info);
		const float th = rowH - 14, tw = std::round(th / CARD_SHAPE);
		if (cover.valid())
			imageFill(dl, cover, x0 + 16, y + 7, tw, th, 4);
		else
			rect(dl, x0 + 16, y + 7, tw, th, col::card, 4);
		float tx = titleX;
		if (g.info.favourite)
		{
			text(dl, regular(), 20, tx, y + 18, on ? col::text : col::warm, ICON_FA_STAR);
			tx += 32;
		}
		textIn(dl, bold(), 24, tx, y + 15, main, g.title, titleW - (tx - titleX), on);
		float x = x0 + w - fixed;
		const auto cell = [&](const std::string& value, float width) {
			text(dl, regular(), 22, x, y + 16, value.empty() ? alpha(side, 0.5f) : side, value.empty() ? "\xe2\x80\x94" : fit(regular(), 22, value, width - 16).c_str());
			x += width;
		};
		cell(g.info.region, columns[0].width);
		cell((g.info.compatibility >= 1 && g.info.compatibility <= 6) ? compatibilityWords[g.info.compatibility] : "", columns[1].width);
		cell(g.info.lastPlayed > 0 ? formatWhen(g.info.lastPlayed) : std::string(), columns[2].width);
		cell(formatPlayed(g.info.playedSeconds), columns[3].width);
		cell(g.info.size != 0 ? formatSize(g.info.size) : std::string(), columns[4].width);
	}
	dl->PopClipRect();
}

// ---- the showcase view, as a console's dashboard shows a game: its picture of play large behind,
// its cover and what is known of it, and a strip of the covers along the bottom

// How far the art of the game now focused has faded in (it starts again with each new game).
float artFadeFor(const Game& g)
{
	if (g.info.path != lib.artPath)
	{
		lib.artPath = g.info.path;
		lib.artFade = (motion() == MotionOff) ? 1.f : 0.f;
	}
	lib.artFade = approach(lib.artFade, 1, 7);
	return lib.artFade;
}

// The game's picture of play (else its title screen, else its cover) in a box, fading out at its
// left and bottom edges into the backdrop; a slow push in and out, when everything moves.
void gameArt(ImDrawList* dl, const Game& g, float a, float x, float y, float w, float h)
{
	Texture art = be->picture(g.info, 1);
	if (!art.valid())
		art = be->picture(g.info, 0);
	if (!art.valid())
		art = be->cover(g.info);
	if (!art.valid())
		return;
	const float push = (motion() == MotionFull) ? 1.03f + 0.025f * std::sin(static_cast<float>(timeNow) * 0.19f) : 1.f;
	const float aw = w * push, ah = h * push;
	imageFade(dl, art, x - (aw - w) / 2, y - (ah - h) / 2, aw, ah, 0.72f * a, {0.f, 0.38f, 1.f}, {0.f, 1.f, 1.f}, {0.f, 0.6f, 1.f},
		{1.f, 0.85f, 0.f});
}

// The Play button, with its glyph; its width.
float playButton(ImDrawList* dl, float x, float y, bool selectDisc, float a)
{
	const char* play = selectDisc ? ICON_FA_COMPACT_DISC "   Insert disc" : ICON_FA_PLAY "   Play";
	const ImVec2 ps = textSize(bold(), 28, play);
	const float bw = ps.x + 92;
	const float pulse = (motion() == MotionFull) ? 0.5f + 0.5f * std::sin(static_cast<float>(timeNow) * 2.4f) : 0.5f;
	glow(dl, x, y, bw, 62, 31, alpha(col::accent, (0.5f + 0.4f * pulse) * a), 16);
	rect(dl, x, y, bw, 62, alpha(col::accent, a), 31);
	glyph(dl, Glyph::Cross, x + 16, y + 31, 32);
	text(dl, bold(), 28, x + 62, y + 31 - ps.y / 2, alpha(col::text, a), play);
	return bw;
}

// The title, the kind, and the facts of a game, from (x, y) in a column `w` wide; where it ends.
float gameFacts(ImDrawList* dl, const Game& g, float x, float y, float w, float a, float titleSize)
{
	const std::string badge = kindText(g.info);
	const ImVec2 bs = textSize(bold(), 20, badge.c_str());
	const ImU32 tint = kindColour(g);
	rect(dl, x, y, bs.x + 28, 36, alpha(tint, 0.25f * a), 8);
	text(dl, bold(), 20, x + 14, y + 18 - bs.y / 2, alpha(tint, a), badge.c_str());
	y += 52;
	y += text(dl, bold(), titleSize, x, y, alpha(col::text, a), fit(bold(), titleSize, g.title, w * 2.f).c_str(), w).y + 12;
	const std::string meta = metaText(g);
	if (!meta.empty())
		y += text(dl, regular(), 24, x, y, alpha(col::dim, a), meta.c_str(), w).y + 18;
	const auto row = [&](const char* label, const std::string& value) {
		if (value.empty())
			return;
		text(dl, regular(), 22, x, y, alpha(col::faint, a), label);
		textIn(dl, regular(), 22, x + 180, y, alpha(col::text, a), value, w - 180, false);
		y += 34;
	};
	row("Last played", g.info.lastPlayed > 0 ? formatWhen(g.info.lastPlayed) : std::string("Not yet"));
	row("Time played", formatPlayed(g.info.playedSeconds));
	if (g.info.compatibility >= 1 && g.info.compatibility <= 6)
		row("Known to run", compatibilityWords[g.info.compatibility]);
	if (setsGrouped && g.setSize > 1)
		row("Discs", "Disc " + std::to_string(g.discOrder) + " of " + std::to_string(g.setSize));
	return y;
}

void drawShowcase(ImDrawList* dl, Game* focused, bool selectDisc)
{
	const int n = static_cast<int>(lib.flat.size());
	const float top = 96;
	const float stripH = 196, stripW = std::round(stripH / CARD_SHAPE), stripGap = 18, stripY = H - 72 - stripH - 40;
	if (focused != nullptr)
	{
		const float a = artFadeFor(*focused);
		gameArt(dl, *focused, a, W * 0.28f, top - 24, W * 0.72f, stripY - top + 60);
		const float coverH = std::min(600.f, stripY - top - 80), coverW = std::round(coverH / CARD_SHAPE);
		const float cx = 96, cy = top + 30 + (1 - a) * 14;
		glow(dl, cx, cy, coverW, coverH, 16, alpha(col::accent, 0.3f * a), 28);
		drawCover(dl, *focused, cx, cy, coverW, coverH, 14, 1);
		const float tx = cx + coverW + 56, tw = std::min(W * 0.46f, W - tx - 96);
		const float end = gameFacts(dl, *focused, tx, top + 40, tw, a, 56);
		playButton(dl, tx, std::min(end + 22, stripY - 98), selectDisc, a);
	}
	// The strip: the focus travels along it, which moves only to keep it in sight.
	const float at = static_cast<float>(lib.flatFocus);
	lib.stripAt = (lib.stripAt < 0 || motion() == MotionOff) ? at : approach(lib.stripAt, at, 16);
	const float step = stripW + stripGap;
	const float shown = (W - 192) / step;
	static float scroll = 0;
	const float want = std::clamp(at - 2.f, 0.f, std::max(0.f, static_cast<float>(n) - shown + 0.2f));
	scroll = (motion() == MotionOff) ? want : approach(scroll, want, 12);
	dl->PushClipRect(V(64, stripY - 40), V(W - 64, H - 72), true);
	for (int i = std::max(0, static_cast<int>(scroll) - 1); i < std::min(n, static_cast<int>(scroll + shown) + 2); i++)
	{
		const float x = 96 + (static_cast<float>(i) - scroll) * step;
		const float near = lib.details ? 0.f : std::clamp(1.f - std::fabs(static_cast<float>(i) - lib.stripAt), 0.f, 1.f);
		const float up = 22 * near;
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		if (near > 0.02f)
			glow(dl, x, stripY - up, stripW, stripH, 10, alpha(col::accent, near), 18);
		drawCover(dl, g, x, stripY - up, stripW, stripH, 8, near);
		if (near < 0.98f)
			rect(dl, x, stripY - up, stripW, stripH, col::rgba(0, 0, 0, static_cast<int>(80 * (1 - near))), 8);
		if (near > 0.02f)
			outline(dl, x - 4, stripY - up - 4, stripW + 8, stripH + 8, alpha(col::text, near), 11, 4);
	}
	dl->PopClipRect();
	const std::string where = std::to_string(lib.flatFocus + 1) + " of " + std::to_string(n);
	const ImVec2 ws = textSize(regular(), 20, where.c_str());
	text(dl, regular(), 20, W - 96 - ws.x, stripY - 44 - ws.y, col::faint, where.c_str());
}

// ---- the wheel view, as front ends of arcade cabinets have it: the titles on a wheel at the right
// that turns past the focus, the game's art and its facts at the left

void drawWheel(ImDrawList* dl, Game* focused, bool selectDisc)
{
	const int n = static_cast<int>(lib.flat.size());
	const float top = 96;
	const float wheelX = W * 0.6f, wheelW = W - 64 - wheelX, cy = top + (H - 72 - top) / 2, rowH = 78;
	if (focused != nullptr)
	{
		const float a = artFadeFor(*focused);
		gameArt(dl, *focused, a, 0, top - 24, wheelX + 80, H - 72 - top + 24);
		// Darker toward the wheel, so that its titles read over the art.
		gradientH(dl, wheelX - 220, top, 220, H - 72 - top, col::rgba(0, 0, 0, 0), col::rgba(0, 0, 0, 120));
		rect(dl, wheelX, top, W - wheelX, H - 72 - top, col::rgba(0, 0, 0, 120));
		const float coverH = 330, coverW = std::round(coverH / CARD_SHAPE);
		const float bottom = H - 72 - 48;
		const float cx = 96, cyCover = bottom - coverH;
		drawCover(dl, *focused, cx, cyCover + (1 - a) * 14, coverW, coverH, 12, 1);
		const float tx = cx + coverW + 40, tw = wheelX - tx - 64;
		gameFacts(dl, *focused, tx, cyCover, tw, a, 44);
		playButton(dl, tx, bottom - 62, selectDisc, a);
	}
	const float at = static_cast<float>(lib.flatFocus);
	lib.wheelAt = (lib.wheelAt < 0 || motion() == MotionOff) ? at : approach(lib.wheelAt, at, 13);
	// The focus's place: a band across the wheel.
	if (!lib.details)
	{
		glow(dl, wheelX + 12, cy - rowH / 2, wheelW - 12, rowH, 14, alpha(col::accent, 0.5f), 14);
		rect(dl, wheelX + 12, cy - rowH / 2, wheelW - 12, rowH, col::accent, 14);
	}
	dl->PushClipRect(V(wheelX, top), V(W, H - 72), true);
	const int reach = static_cast<int>((H - 72 - top) / 2 / (rowH * 0.8f)) + 2;
	for (int i = std::max(0, static_cast<int>(lib.wheelAt) - reach); i <= std::min(n - 1, static_cast<int>(lib.wheelAt) + reach); i++)
	{
		const float d = static_cast<float>(i) - lib.wheelAt, ad = std::fabs(d);
		// On a wheel: the farther from the focus, the smaller, the fainter, and the farther right.
		const float size = std::max(20.f, 34.f - 3.2f * ad);
		const float y = cy + d * rowH * (1.f - 0.035f * std::min(ad, 6.f));
		const float x = wheelX + 40 + 26 * (1 - std::cos(std::min(ad, 6.f) * 0.26f)) * 4;
		const float fade = std::clamp(1.f - 0.15f * ad, 0.15f, 1.f);
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		const bool on = ad < 0.5f && !lib.details;
		const std::string t = fit(bold(), size, g.title, W - 96 - x);
		const ImVec2 ts = textSize(bold(), size, t.c_str());
		text(dl, on ? bold() : regular(), size, x, y - ts.y / 2, alpha(on ? col::text : col::dim, fade), t.c_str());
	}
	dl->PopClipRect();
	const std::string where = std::to_string(lib.flatFocus + 1) + " of " + std::to_string(n);
	const ImVec2 ws = textSize(regular(), 20, where.c_str());
	text(dl, regular(), 20, W - 96 - ws.x, top + 8, col::faint, where.c_str());
}

// ---- the wall view: every cover, small and close together; the focused one stands out of it

void drawWall(ImDrawList* dl, Game* focused)
{
	const float top = 96 + 20;
	const int n = static_cast<int>(lib.flat.size());
	const float gap = 10;
	const float cardW = (W - 128 - gap * (WallColumns - 1)) / WallColumns, cardH = std::round(cardW * CARD_SHAPE);
	const float viewH = H - 72 - top - 70;
	const int rows = (n + WallColumns - 1) / WallColumns;
	const float rowH = cardH + gap, visibleRows = viewH / rowH;
	const float target = std::clamp(static_cast<float>(lib.flatFocus / WallColumns) - (visibleRows - 1) / 2, 0.f,
		std::max(0.f, static_cast<float>(rows) - visibleRows + 0.1f));
	lib.flatScroll = approach(lib.flatScroll, target, 14);
	{
		const float column = static_cast<float>(lib.flatFocus % WallColumns), row = static_cast<float>(lib.flatFocus / WallColumns);
		const bool jump = lib.ringColumn < 0 || motion() == MotionOff;
		lib.ringColumn = jump ? column : approach(lib.ringColumn, column, 22);
		lib.ringRow = jump ? row : approach(lib.ringRow, row, 22);
	}
	dl->PushClipRect(V(0, top - 30), V(W, top + viewH + 40), true);
	float fx = -1, fy = 0;
	for (int i = 0; i < n; i++)
	{
		const int row = i / WallColumns, column = i % WallColumns;
		const float y = top + (row - lib.flatScroll) * rowH;
		if (y > H || y + cardH < top - cardH)
			continue;
		const float x = 64 + column * (cardW + gap);
		const float near = lib.details ? 0.f :
										 std::clamp(1.f - std::fabs(column - lib.ringColumn), 0.f, 1.f) *
											 std::clamp(1.f - std::fabs(row - lib.ringRow), 0.f, 1.f);
		if (near > 0.5f)
		{
			fx = x;
			fy = y;
			continue; // drawn last, over its neighbours
		}
		drawCover(dl, games[lib.flat[static_cast<size_t>(i)]], x, y, cardW, cardH, 5, 0);
		rect(dl, x, y, cardW, cardH, col::rgba(0, 0, 0, static_cast<int>(70 * (1 - near))), 5);
	}
	if (fx >= 0 && focused != nullptr)
	{
		// Half as large again, from its middle, kept on the screen.
		const float w = cardW * 1.5f, h = cardH * 1.5f;
		const float x = std::clamp(fx - (w - cardW) / 2, 40.f, W - 40 - w), y = std::max(top - 24, fy - (h - cardH) / 2);
		glow(dl, x, y, w, h, 10, col::accent, 22);
		drawCover(dl, *focused, x, y, w, h, 8, 1);
		outline(dl, x - 4, y - 4, w + 8, h + 8, col::text, 11, 4);
	}
	dl->PopClipRect();
	// The focused game, said under the wall.
	if (focused != nullptr)
	{
		const float y = H - 72 - 58;
		std::string line = focused->title;
		const std::string meta = metaText(*focused);
		const float tw = textIn(dl, bold(), 30, 64, y, col::text, focused->title, W * 0.55f, true).x;
		if (!meta.empty())
			textIn(dl, regular(), 22, 64 + tw + 28, y + 6, col::dim, meta, W - 64 - tw - 28 - 260, false);
		const std::string where = std::to_string(lib.flatFocus + 1) + " of " + std::to_string(n);
		const ImVec2 ws = textSize(regular(), 20, where.c_str());
		text(dl, regular(), 20, W - 64 - ws.x, y + 8, col::faint, where.c_str());
	}
}

// ---- the discs view: the games' discs in a row, the focused one large and turning, as it would in
// the drive; its title under it

void drawDiscs(ImDrawList* dl, Game* focused)
{
	const int n = static_cast<int>(lib.flat.size());
	const float at = static_cast<float>(lib.flatFocus);
	lib.discsAt = (lib.discsAt < 0 || motion() == MotionOff) ? at : approach(lib.discsAt, at, 11);
	const float mid = W / 2, cy = 96 + 70 + 250;
	const float bigR = 250, smallR = 118, first = bigR + 44 + smallR, spacing = smallR * 2 + 26;
	const int reach = static_cast<int>((W / 2 - first) / spacing) + 3;
	std::vector<int> order;
	for (int i = std::max(0, static_cast<int>(std::floor(lib.discsAt)) - reach); i <= std::min(n - 1, static_cast<int>(std::ceil(lib.discsAt)) + reach); i++)
		order.push_back(i);
	std::sort(order.begin(), order.end(), [](int a, int b) { return std::fabs(a - lib.discsAt) > std::fabs(b - lib.discsAt); });
	const float t = static_cast<float>(timeNow);
	for (int i : order)
	{
		const float d = static_cast<float>(i) - lib.discsAt, away = std::fabs(d), a = std::min(away, 1.f), side = (d < 0) ? -1.f : 1.f;
		const float x = mid + side * (a * first + std::max(0.f, away - 1) * spacing);
		const float r = bigR + (smallR - bigR) * a;
		const float fade = std::clamp(1.f - 0.12f * std::max(0.f, away - 1), 0.25f, 1.f);
		const Game& g = games[lib.flat[static_cast<size_t>(i)]];
		// The focused disc turns as in a drive; the others, slowly. Still, with motion off.
		const float spin = (motion() == MotionOff) ? 0.f : (a < 0.5f ? t * 9.f : t * 0.8f + static_cast<float>(i));
		const Texture cover = be->cover(g.info);
		if (cover.valid())
			labelDisc(dl, cover, x, cy, r, spin, fade * (1 - 0.3f * a));
		else
		{
			softDisc(dl, x + r * 0.05f, cy + r * 0.1f, r * 1.12f, col::rgba(0, 0, 0, static_cast<int>(110 * fade)), 48);
			dl->AddCircleFilled(V(x, cy), r * S, alpha(titleColour(g.title, 0.5f, 0.45f), fade), 96);
			dl->AddCircleFilled(V(x, cy), r * 0.33f * S, col::rgba(214, 220, 232, static_cast<int>(220 * fade)), 64);
			dl->AddCircleFilled(V(x, cy), r * 0.15f * S, col::rgba(8, 12, 20, static_cast<int>(240 * fade)), 48);
		}
		if (a < 0.5f && !lib.details)
			dl->AddCircle(V(x, cy), (r + 8) * S, alpha(col::accent, 1 - 2 * a), 128, 4 * S);
	}
	if (focused != nullptr)
	{
		const float ty = cy + bigR + 44;
		const std::string title = fit(bold(), 46, focused->title, W * 0.72f);
		const ImVec2 ts = textSize(bold(), 46, title.c_str());
		text(dl, bold(), 46, mid - ts.x / 2, ty, col::text, title.c_str());
		std::string meta = kindText(focused->info);
		const std::string more = metaText(*focused);
		if (!more.empty())
			meta += "   \xc2\xb7   " + more;
		meta = fit(regular(), 24, meta, W * 0.72f);
		const ImVec2 ms = textSize(regular(), 24, meta.c_str());
		text(dl, regular(), 24, mid - ms.x / 2, ty + 64, col::dim, meta.c_str());
	}
	if (n > 1)
	{
		const std::string where = std::to_string(lib.flatFocus + 1) + " of " + std::to_string(n);
		const ImVec2 ws = textSize(regular(), 20, where.c_str());
		text(dl, regular(), 20, mid - ws.x / 2, H - 72 - 50, col::faint, where.c_str());
	}
}

// ------------------------------------------------------------- the splash

// The start-up animation: the app's mark and name in the middle of the screen, alive (the mark
// spins up, rings of light leave it, sparks go round it, the letters hop one after another), then
// both fly to their places in the top bar. t is the time since it began; flight how far the mark
// and the name are on their way, 0 to 1; rest how much of everything else is left, 1 to 0. Not
// lively ("Less" motion): the mark and the name, still.
} // namespace

// ----------------------------------------------------- what the others ask

int librarySource()
{
	return lib.source;
}

const GameInfo* libraryGame(const std::string& path)
{
	for (const Game& g : games)
	{
		if (g.info.path == path)
			return &g.info;
	}
	return nullptr;
}

void drawGameCover(ImDrawList* dl, const GameInfo& game, float x, float y, float w, float h, float r, float lift)
{
	for (const Game& g : games)
	{
		if (g.info.path == game.path)
		{
			drawCover(dl, g, x, y, w, h, r, lift);
			return;
		}
	}
	Game g;
	g.info = game;
	g.title = game.title;
	drawCover(dl, g, x, y, w, h, r, lift);
}

void setLoadingGame(const GameInfo& game)
{
	loadingGame = game;
	loadingTitle = game.title;
	for (const Game& g : games)
	{
		if (g.info.path == game.path)
			loadingTitle = g.title;
	}
}

void launchGame(const GameInfo& game, int state, int boot)
{
	setLoadingGame(game);
	insertedPath = game.path;
	be->start(game, state, boot);
	setScreen(Screen::Loading);
}

// ------------------------------------------------------------ the list's menu
//
// Square in the library: what the list is narrowed to, its order and its view, the quick views,
// the collections, a game at random, and the scan. (A list to choose from, and for most of its
// rows a second one.)
namespace
{
const char* const orderNames[] = {"By name", "Last played first", "Most played first", "Largest first", "Newest file first"};
const char* const showNames[] = {"All games", "Favourites only", "Hidden games"};

// A game anywhere on the open tab gets the focus: in the list of the other views, and on the
// last shelf that has it (the shelf of all games, not the one of those last played).
void focusAnywhere(const std::string& path)
{
	lib.flatPath = path;
	for (size_t i = 0; i < lib.flat.size(); i++)
	{
		if (games[lib.flat[i]].info.path == path)
			lib.flatFocus = static_cast<int>(i);
	}
	for (int si = static_cast<int>(lib.shelves.size()) - 1; si >= 0; si--)
	{
		Shelf& shelf = lib.shelves[static_cast<size_t>(si)];
		for (size_t i = 0; i < shelf.items.size(); i++)
		{
			if (games[shelf.items[i]].info.path == path)
			{
				lib.shelf = si;
				shelf.focus = static_cast<int>(i);
				shelf.focusPath = path;
				return;
			}
		}
	}
}

// One of the games the list shows now, by chance: it gets the focus and its details open.
void randomGame()
{
	if (lib.flat.empty())
	{
		toast("There is no game to choose from here.", 3.f);
		return;
	}
	static uint32_t seed = 0;
	if (seed == 0)
		seed = static_cast<uint32_t>(time(nullptr)) ^ (static_cast<uint32_t>(frameNow) * 2654435761u) ^ 0x9e3779b9u;
	// (Not the game that has the focus already, when there is another.)
	size_t chosen = 0;
	for (int tries = 0; tries < 8; tries++)
	{
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		chosen = seed % lib.flat.size();
		if (lib.flat.size() == 1 || games[lib.flat[chosen]].info.path != lib.flatPath)
			break;
	}
	const Game& g = games[lib.flat[chosen]];
	focusAnywhere(g.info.path);
	lib.heroPath = g.info.path;
	openDetails(g);
}

void filtersMenu()
{
	const auto check = [](bool on, const char* name) { return std::string(on ? ICON_FA_SQUARE_CHECK : ICON_FA_SQUARE) + "   " + name; };
	std::vector<std::string> names = {std::string(ICON_FA_EARTH_AMERICAS "   Region: ") + regionNames[nar.region],
		std::string(ICON_FA_CLOCK "   ") + playedNames[nar.played], std::string(ICON_FA_STAR_HALF_STROKE "   ") + runsNames[nar.runs],
		check(nar.states, "Only games with saved states"), check(nar.cheats, "Only games with cheats"),
		check(nar.textures, "Only games with a texture pack")};
	if (nar.filtered())
		names.push_back(ICON_FA_XMARK "   No filters");
	// (It comes back after each change, so that several can be set; Circle leaves it.)
	pick("Filters", std::move(names), -1, [](int choice) {
		quickLeft();
		switch (choice)
		{
			case 0:
				pick("Region", {regionNames[0], regionNames[1], regionNames[2], regionNames[3], regionNames[4]}, nar.region, [](int region) {
					nar.region = region;
					filtersMenu();
				});
				return;
			case 1:
				pick("Played", {playedNames[0], playedNames[1], playedNames[2]}, nar.played, [](int played) {
					nar.played = played;
					filtersMenu();
				});
				return;
			case 2:
				pick("Known to run", {runsNames[0], runsNames[1], runsNames[2]}, nar.runs, [](int runs) {
					nar.runs = runs;
					filtersMenu();
				});
				return;
			case 3:
				nar.states = !nar.states;
				break;
			case 4:
				nar.cheats = !nar.cheats;
				break;
			case 5:
				nar.textures = !nar.textures;
				break;
			case 6:
			{
				const Narrow before = nar;
				nar = Narrow();
				nar.search = before.search;
				nar.show = before.show;
				nar.collection = before.collection;
				return;
			}
		}
		filtersMenu();
	});
}

void collectionsMenu()
{
	readCollections();
	if (collections.empty())
	{
		message("No collections yet",
			"A collection is a shelf of your own: a name, and the games you put on it.\nTriangle on a game opens its details; More > "
			"Collections there makes one and puts the game into it.");
		return;
	}
	std::vector<std::string> names = {"All games"};
	int current = 0;
	for (size_t i = 0; i < collections.size(); i++)
	{
		const Collection& c = collections[i];
		names.push_back(c.name + "   (" + std::to_string(c.members.size()) + ")");
		if (c.name == nar.collection)
			current = static_cast<int>(i) + 1;
	}
	names.push_back(ICON_FA_PEN "   Rename a collection");
	names.push_back(ICON_FA_TRASH "   Delete a collection");
	pick("Collection", std::move(names), current, [](int choice) {
		const int n = static_cast<int>(collections.size());
		const auto which = [](const char* title, std::function<void(int)> then) {
			std::vector<std::string> all;
			for (const Collection& c : collections)
				all.push_back(c.name);
			pick(title, std::move(all), -1, std::move(then));
		};
		if (choice <= n)
		{
			quickLeft();
			nar.collection = (choice == 0) ? std::string() : collections[static_cast<size_t>(choice - 1)].name;
		}
		else if (choice == n + 1)
		{
			which("Rename which?", [](int index) {
				if (index < 0 || index >= static_cast<int>(collections.size()))
					return;
				const std::string old = collections[static_cast<size_t>(index)].name;
				editText("The collection's name", old, false, [old](const std::string& typed) {
					const std::string name = keptName(typed);
					Collection* const c = collectionNamed(old);
					if (name.empty() || c == nullptr || name == old)
						return;
					if (collectionNamed(name) != nullptr)
					{
						toast("There is a collection of that name already.", 4.f);
						return;
					}
					c->name = name;
					if (nar.collection == old)
						nar.collection = name;
					// (The quick views that show it go on showing it.)
					readQuickViews();
					bool renamedAny = false;
					for (QuickView& q : quickViews)
					{
						if (q.narrow.collection == old)
						{
							q.narrow.collection = name;
							renamedAny = true;
						}
					}
					if (renamedAny)
						writeQuickViews();
					writeCollections();
				}, 32);
			});
		}
		else if (choice == n + 2)
		{
			which("Delete which?", [](int index) {
				if (index < 0 || index >= static_cast<int>(collections.size()))
					return;
				const std::string name = collections[static_cast<size_t>(index)].name;
				confirm("Delete the collection \"" + name + "\"?", "Its games stay in the library: only the shelf goes.", [name](bool yes) {
					if (!yes)
						return;
					for (size_t i = 0; i < collections.size(); i++)
					{
						if (collections[i].name == name)
						{
							collections.erase(collections.begin() + static_cast<std::ptrdiff_t>(i));
							break;
						}
					}
					if (nar.collection == name)
						nar.collection.clear();
					writeCollections();
				}, "Delete it", "Keep it");
			});
		}
	});
}

void quickViewsMenu()
{
	readQuickViews();
	std::vector<std::string> names;
	for (const QuickView& q : quickViews)
		names.push_back(std::string(ICON_FA_BOLT "   ") + q.name);
	const int n = static_cast<int>(quickViews.size());
	names.push_back(ICON_FA_PLUS "   Keep what is shown now as a quick view");
	if (n > 0)
		names.push_back(ICON_FA_TRASH "   Delete a quick view");
	pick("Quick views  (R3 goes through them)", std::move(names), quickNow, [n](int choice) {
		if (choice < n)
		{
			applyQuick(choice);
		}
		else if (choice == n)
		{
			if (n >= MOST_QUICK)
			{
				toast("There are as many quick views as can be kept: delete one first.", 4.f);
				return;
			}
			editText("The quick view's name", std::string(), false, [](const std::string& typed) {
				QuickView q;
				q.name = keptName(typed);
				if (q.name.empty())
					return;
				q.narrow = nar;
				q.sort = options().sort;
				q.view = options().view;
				for (QuickView& old : quickViews)
				{
					if (old.name == q.name)
					{
						old = q;
						writeQuickViews();
						toast("\"" + q.name + "\" is what is shown now.", 3.f);
						return;
					}
				}
				quickViews.push_back(q);
				quickNow = static_cast<int>(quickViews.size()) - 1;
				writeQuickViews();
				toast("Kept as \"" + q.name + "\": R3 comes to it.", 4.f);
			}, 24);
		}
		else
		{
			std::vector<std::string> all;
			for (const QuickView& q : quickViews)
				all.push_back(q.name);
			pick("Delete which?", std::move(all), -1, [](int index) {
				if (index < 0 || index >= static_cast<int>(quickViews.size()))
					return;
				quickViews.erase(quickViews.begin() + index);
				if (quickNow == index)
					quickNow = -1;
				else if (quickNow > index)
					quickNow--;
				writeQuickViews();
			});
		}
	});
}

void listMenu()
{
	enum { Search, Quick, Order, ShowRow, Filters, CollectionRow, ViewRow, Random, Scan, Whole };
	std::vector<std::string> names;
	names.push_back(nar.search.empty() ? std::string(ICON_FA_MAGNIFYING_GLASS "   Search by name") :
	                                     std::string(ICON_FA_MAGNIFYING_GLASS "   Search: \"") + nar.search + "\"  (change it)");
	readQuickViews();
	names.push_back((quickNow >= 0 && quickNow < static_cast<int>(quickViews.size())) ?
						std::string(ICON_FA_BOLT "   Quick view: ") + quickViews[static_cast<size_t>(quickNow)].name :
						std::string(ICON_FA_BOLT "   Quick views"));
	names.push_back(std::string(ICON_FA_ARROW_DOWN_A_Z "   Order: ") + orderNames[std::clamp(options().sort, 0, 4)]);
	names.push_back(std::string(ICON_FA_EYE "   Show: ") + showNames[std::clamp(nar.show, 0, 2)]);
	{
		const int set = (nar.region != RegionAny ? 1 : 0) + (nar.played != 0 ? 1 : 0) + (nar.runs != 0 ? 1 : 0) + (nar.states ? 1 : 0) +
						(nar.cheats ? 1 : 0) + (nar.textures ? 1 : 0);
		names.push_back(std::string(ICON_FA_FILTER "   Filters: ") + (set == 0 ? std::string("none") : std::to_string(set) + " set"));
	}
	names.push_back(std::string(ICON_FA_LAYER_GROUP "   Collection: ") + (nar.collection.empty() ? std::string("all games") : nar.collection));
	names.push_back(std::string(ICON_FA_TABLE_CELLS "   View: ") + viewNames[std::clamp(options().view, 0, static_cast<int>(ViewCount) - 1)]);
	names.push_back(ICON_FA_DICE "   A game at random");
	names.push_back(ICON_FA_ROTATE "   Look for new games");
	if (narrowed())
		names.push_back(ICON_FA_XMARK "   Show all games again");
	pick("The list", std::move(names), -1, [](int choice) {
		switch (choice)
		{
			case Search:
				editText("Search by name", nar.search, false, [](const std::string& typed) {
					quickLeft();
					nar.search = typed;
					while (!nar.search.empty() && nar.search.back() == ' ')
						nar.search.pop_back();
				}, 40);
				break;
			case Quick:
				quickViewsMenu();
				break;
			case Order:
				pick("Order of the games", {orderNames[0], orderNames[1], orderNames[2], orderNames[3], orderNames[4]},
					std::clamp(options().sort, 0, 4), [](int order) {
						quickLeft();
						options().sort = order;
						saveOptions();
					});
				break;
			case ShowRow:
				pick("Which games show", {showNames[0], showNames[1], showNames[2]}, std::clamp(nar.show, 0, 2), [](int show) {
					quickLeft();
					nar.show = show;
				});
				break;
			case Filters:
				filtersMenu();
				break;
			case CollectionRow:
				collectionsMenu();
				break;
			case ViewRow:
			{
				std::vector<std::string> views(viewNames, viewNames + ViewCount);
				pick("View", std::move(views), std::clamp(options().view, 0, static_cast<int>(ViewCount) - 1), [](int chosen) {
					options().view = chosen;
					saveOptions();
				});
				break;
			}
			case Random:
				randomGame();
				break;
			case Scan:
				// (Not under a running game: a scan reads discs too.)
				be->scan(false);
				toast("Looking for new games...", 3.f);
				break;
			case Whole:
				applyQuick(-1);
				break;
		}
	});
}
} // namespace

// ------------------------------------------------------------ the library

void librarySplashOrScreen(bool selectDisc)
{
	// A moment of the start-up, for the Backend to make heard: once a start each.
	const auto tell = [](Sound what) {
		bool& told = splash.told[static_cast<int>(what)];
		if (!told)
		{
			told = true;
			be->sound(what);
		}
	};
	// The first time the library is on, the start-up animation is, before it.
	if (splash.state == SplashState::NotBegun)
	{
		splash.state = (!selectDisc && options().splash && motion() != MotionOff) ? SplashState::Showing : SplashState::Over;
		if (splash.state == SplashState::Showing)
		{
			int kind = SplashState::Still;
			if (motion() == MotionFull)
			{
				kind = options().splashKind - 1;
				if (kind < 0)
				{
					// Another one than the last start played: whichever the clock picks of the others.
					const int last = options().splashLast - 1;
					const unsigned seed = static_cast<unsigned>(time(nullptr)) * 2654435761u;
					kind = static_cast<int>((seed >> 16) % SplashState::Kinds);
					if (kind == last)
						kind = (kind + 1 + static_cast<int>((seed >> 8) % (SplashState::Kinds - 1))) % SplashState::Kinds;
				}
				if (options().splashLast != kind + 1)
				{
					options().splashLast = kind + 1;
					saveOptions();
				}
			}
			splash.kind = static_cast<decltype(splash.kind)>(kind);
			splash.leaveAt = splashLeaveAt(kind);
			splash.leaveFor = splashLeaveFor(kind);
			splash.skipped = false;
		}
	}
	if (splash.state == SplashState::Over)
	{
		tell(Sound::LibraryOpens);
		libraryScreen(selectDisc);
		return;
	}
	const bool lively = (splash.kind != SplashState::Still);
	if (splash.state == SplashState::Showing)
	{
		ImDrawList* dl = beginScreen("##bp-splash", true);
		// It has the controller: any button ends it early, and none reaches the library under it.
		const bool skip = in.any || in.up || in.down || in.left || in.right;
		in = Input{};
		// The first frames wait for the display and the fonts: its clock starts after them.
		if (splash.frames++ < 3)
			splash.began = timeNow;
		else if (lively)
			tell(Sound::StartupBegins);
		const float t = static_cast<float>(timeNow - splash.began);
		// The games are looked at meanwhile.
		chooseSource();
		refreshGames();
		if (lively)
			drawStartup(dl, splash.kind, t);
		else
			drawStartupStill(dl, 1);
		endScreen();
		if (t >= splash.leaveAt || (skip && t > 0.25f))
		{
			if (t < splash.leaveAt)
			{
				// Ended by a button: what is there goes, and the library is there.
				if (lively && splash.told[static_cast<int>(Sound::StartupBegins)])
					tell(Sound::StartupSkipped);
				splash.skipped = true;
				splash.leaveAt = t;
				splash.leaveFor = 0.35f;
			}
			splash.state = SplashState::Leaving;
			holdInput();
		}
		return;
	}
	// Leaving: the library is drawn, and over it what the animation still has to show. The top
	// bar draws the name itself from the moment the animation has brought it there.
	const float t = static_cast<float>(timeNow - splash.began);
	const float u = std::clamp((t - splash.leaveAt) / splash.leaveFor, 0.f, 1.f);
	const bool whole = lively && !splash.skipped;
	splash.flight = (!whole || splashDelivered(splash.kind, t)) ? 1.f : 0.f;
	libraryScreen(selectDisc);
	ImDrawList* fg = ImGui::GetForegroundDrawList();
	if (whole)
		drawStartup(fg, splash.kind, t);
	else if (splash.skipped)
		drawStartupGround(fg, 1 - easeOut(u));
	else
		drawStartupStill(fg, 1 - u);
	if (u >= 1)
	{
		splash.state = SplashState::Over;
		splash.flight = 1.f;
		tell(Sound::LibraryOpens);
	}
}

// ------------------------------------------------------------ the disc drive

void pollDisc()
{
	if (be == nullptr)
		return;
	static bool asked = false;
	static std::vector<std::string> knownDiscs; // what told each disc in a drive apart, last time
	DiscState now = be->disc();
	if (!asked || now.generation != drive.generation)
	{
		asked = true;
		// What tells each disc from the one before it (and from none): a disc that came, in any
		// drive, is said; one that stays is not said again when another drive changes.
		std::vector<std::string> discs;
		for (const GameInfo& game : now.games)
			discs.push_back("game:" + game.path + ":" + game.serial);
		if (now.present && !now.note.empty())
			discs.push_back("other:" + now.note);
		std::vector<std::string> came;
		for (const std::string& key : discs)
		{
			if (std::find(knownDiscs.begin(), knownDiscs.end(), key) == knownDiscs.end())
				came.push_back(key);
		}
		knownDiscs = discs;
		const std::string key = came.empty() ? std::string() : came.size() > 1 && came.front().starts_with("game:") && came[1].starts_with("game:") ? "games" : came.front();
		if (!came.empty())
		{
			driveKey = key;
			driveToSay = true;
		}
		else if (discs.empty())
		{
			driveKey.clear();
		}
		drive = std::move(now);
		driveRevision++;
	}
	// A disc that has come is said once, when the library (or a game) is there to be said over.
	if (driveToSay && splash.state == SplashState::Over && screen != Screen::Loading)
	{
		driveToSay = false;
		const GameInfo* said = nullptr;
		for (const GameInfo& game : drive.games)
		{
			if (driveKey == "game:" + game.path + ":" + game.serial)
				said = &game;
		}
		if (driveKey == "games")
			toast(std::to_string(drive.games.size()) + " discs are in the drives: " + drive.games.front().title + ", " + drive.games[1].title +
					  (drive.games.size() > 2 ? "..." : ""),
				5.f);
		else if (said != nullptr)
			toast(std::string(said->cd ? "A CD" : "A disc") + " is in a drive: " + (!said->title.empty() ? said->title : said->serial), 5.f);
		else if (driveKey.starts_with("other:"))
			toast(!drive.note.empty() ? drive.note : std::string("The disc in the drive is not a PlayStation 2 game."), 6.f);
	}
}

const DiscState& discNow()
{
	return drive;
}

void libraryScreen(bool selectDisc)
{
	ImDrawList* dl = beginScreen("##bp-library", true);
	chooseSource();
	refreshGames();
	// A disc is to be chosen for the running game: its own tab, and the disc after the one in the
	// drive, if the game has several.
	static bool choosing = false;
	static int drawnFrame = -10;
	static bool drawnForDisc = false;
	// Come to anew (it was not on the frame before): a disc is chosen from the start, and a
	// details panel left open in the other kind of library is not this one's.
	if (frameNow > drawnFrame + 1)
	{
		choosing = false;
		if (drawnForDisc != selectDisc)
			lib.details = false;
	}
	drawnFrame = frameNow;
	drawnForDisc = selectDisc;
	std::string nextDisc;
	if (selectDisc && !choosing)
	{
		insertedPath = be->running().path;
		for (size_t i = 0; i < games.size(); i++)
		{
			if (games[i].info.path == insertedPath)
			{
				switchSource(games[i].info.source);
				if (!games[i].set.empty())
				{
					const std::vector<size_t>& discs = discSets[games[i].set];
					const size_t at = static_cast<size_t>(std::find(discs.begin(), discs.end(), i) - discs.begin());
					nextDisc = games[discs[(at + 1) % discs.size()]].info.path;
				}
				break;
			}
		}
	}
	choosing = selectDisc;
	buildShelves(selectDisc);
	buildFlat(selectDisc);
	if (!nextDisc.empty())
	{
		focusPath(nextDisc);
		if (!lib.shelves.empty() && lib.shelves[0].name == "This game's discs")
		{
			lib.shelf = 0;
			for (size_t i = 0; i < lib.shelves[0].items.size(); i++)
			{
				if (games[lib.shelves[0].items[i]].info.path == nextDisc)
					lib.shelves[0].focus = static_cast<int>(i);
			}
		}
	}
	const View v = view();

	const auto focusedGame = [&]() -> Game* {
		if (view() == Shelves)
		{
			if (lib.shelves.empty())
				return nullptr;
			Shelf& sh = lib.shelves[static_cast<size_t>(lib.shelf)];
			return sh.items.empty() ? nullptr : &games[sh.items[static_cast<size_t>(std::clamp(sh.focus, 0, static_cast<int>(sh.items.size()) - 1))]];
		}
		return lib.flat.empty() ? nullptr : &games[lib.flat[static_cast<size_t>(std::clamp(lib.flatFocus, 0, static_cast<int>(lib.flat.size()) - 1))]];
	};
	Game* focused = focusedGame();

	// ---- input
	// The details are of one game: gone from under the focus (the list was made again), they close.
	if (lib.details && (focused == nullptr || focused->info.path != det.path))
		lib.details = false;
	if (lib.details)
	{
		detailsInput(*focused, selectDisc);
	}
	else
	{
		if (v == Shelves && !lib.shelves.empty())
		{
			Shelf& sh = lib.shelves[static_cast<size_t>(lib.shelf)];
			const int n = static_cast<int>(sh.items.size());
			if (in.left && sh.focus > 0)
				sh.focus--;
			if (in.right && sh.focus < n - 1)
				sh.focus++;
			// By letter where the shelf is in the order of its titles; six at a time on the
			// others (the games last played, a game's discs).
			if (in.l2)
				sh.focus = sh.byTitle ? letterJump(sh.items, sh.focus, -1) : std::max(0, sh.focus - 6);
			if (in.r2)
				sh.focus = sh.byTitle ? letterJump(sh.items, sh.focus, 1) : std::min(n - 1, sh.focus + 6);
			if (in.up && lib.shelf > 0)
				lib.shelf--;
			if (in.down && lib.shelf < static_cast<int>(lib.shelves.size()) - 1)
				lib.shelf++;
		}
		else if (v != Shelves && !lib.flat.empty())
		{
			const int n = static_cast<int>(lib.flat.size());
			const int step = columnsOf(v);
			int& f = lib.flatFocus;
			if (v == Flow || v == Showcase || v == Discs)
			{
				// One row of them all.
				if (in.left && f > 0)
					f--;
				if (in.right && f < n - 1)
					f++;
			}
			else
			{
				if (step > 1)
				{
					if (in.left && f % step > 0)
						f--;
					if (in.right && f % step < step - 1 && f < n - 1)
						f++;
				}
				if (in.up && f - step >= 0)
					f -= step;
				if (in.down)
					f = (f + step < n) ? f + step : ((step > 1 && f / step < (n - 1) / step) ? n - 1 : f);
			}
			// By letter in the order of the names; a screenful in another order.
			const bool letters = selectDisc || sortedByName();
			const int far = (v == Grid) ? GridColumns * 3 : (v == Wall) ? WallColumns * 3 : (v == Bookcase) ? step :
							(v == Flow || v == Showcase || v == Discs) ? 6 : (v == Table || v == Wheel) ? 13 : 8;
			if (in.l2)
				f = letters ? letterJump(lib.flat, f, -1) : std::max(0, f - far);
			if (in.r2)
				f = letters ? letterJump(lib.flat, f, 1) : std::min(n - 1, f + far);
			lib.flatPath = games[lib.flat[static_cast<size_t>(f)]].info.path;
		}
		if (in.accept && focused != nullptr)
			launch(*focused, selectDisc);
		if (in.triangle && focused != nullptr)
			openDetails(*focused);
		// Square: the list's menu (search, order, which games show, the scan); on the Disc tab, an
		// image of the disc.
		if (in.square && !selectDisc && lib.source == DiscDrive)
		{
			if (focused != nullptr)
				be->makeImage(focused->info);
		}
		else if (in.square && !selectDisc)
			listMenu();
		// The touch pad goes through the views.
		if (in.touch && lib.source != DiscDrive)
		{
			options().view = (std::clamp(options().view, 0, static_cast<int>(ViewCount) - 1) + 1) % ViewCount;
			saveOptions();
			toast(std::string("View: ") + viewNames[options().view], 1.6f);
		}
		// R3 goes through the quick views, and after the last back to all games.
		if (in.r3 && !selectDisc && lib.source != DiscDrive)
		{
			readQuickViews();
			if (quickViews.empty())
				toast("There are no quick views: Square > Quick views keeps what is shown as one.", 4.f);
			else
				applyQuick(quickNow + 1);
		}
		// OPTIONS opens the Settings (not while a disc is chosen for a running game: its options
		// are in the quick menu).
		if (in.options && !selectDisc)
			openSettings(nullptr, Screen::Library);
		if (selectDisc && in.back)
			setScreen(Screen::Quick);
		// Circle: a list that is narrowed is whole again; with nothing to go back to, it closes
		// the title, asked first.
		if (!selectDisc && in.back && narrowed())
		{
			applyQuick(-1);
		}
		else if (!selectDisc && in.back)
		{
			confirm(std::string("Close ") + be->appName() + "?", "Are you sure?", [](bool yes) {
				if (yes && be != nullptr)
					be->quit();
			}, "Yes, close it", "No");
		}
		if (in.l1 || in.r1)
		{
			// The next tab, round and round.
			std::vector<int> sources;
			for (const TopTab& tab : topTabs())
				sources.push_back(tab.id);
			const int n = static_cast<int>(sources.size());
			const int at = static_cast<int>(std::find(sources.begin(), sources.end(), lib.source) - sources.begin()) + (in.r1 ? 1 : -1);
			switchSource(sources[static_cast<size_t>((at + n) % n)]);
			// It comes in from the side the button points to, also when it went round.
			entranceSide(in.r1 ? 1.f : -1.f);
		}
	}
	if (screen != Screen::Library && screen != Screen::Disc)
	{
		// The library handed over (a game is starting, the settings were opened).
		endScreen();
		return;
	}
	// The tab may have changed: its own shelves and list.
	buildShelves(selectDisc);
	buildFlat(selectDisc);
	focused = focusedGame();
	for (Shelf& shelf : lib.shelves)
	{
		const int at = std::clamp(shelf.focus, 0, std::max(0, static_cast<int>(shelf.items.size()) - 1));
		shelf.focusPath = shelf.items.empty() ? std::string() : games[shelf.items[static_cast<size_t>(at)]].info.path;
	}

	const bool empty = (view() == Shelves) ? lib.shelves.empty() : lib.flat.empty();
	// The tab comes in from the side it was reached from; the backdrop takes the focused cover's
	// colours where it is asked to.
	const float e = entered(lib.source + (selectDisc ? 100 : 0));
	ambientArt(focused != nullptr ? &focused->info : nullptr);
	// When the view has changed, the focus stays on its game.
	{
		static int shownView = -1;
		if (shownView != static_cast<int>(view()))
		{
			if (shownView >= 0 && !lib.heroPath.empty())
			{
				focusPath(lib.heroPath);
				lib.flatScroll = static_cast<float>(lib.flatFocus / columnsOf(view()));
				lib.ringColumn = -1;
				lib.listRow = -1;
				lib.flowAt = -1;
				lib.stripAt = lib.wheelAt = lib.discsAt = -1;
				focused = focusedGame();
			}
			shownView = static_cast<int>(view());
		}
		if (focused != nullptr && view() != Shelves)
			lib.heroPath = focused->info.path;
	}
	const Layer content = beginLayer(dl);
	if (empty)
		emptyLibrary(dl, selectDisc);
	else if (lib.source == DiscDrive && !selectDisc)
		drawDiscBay(dl, focused);
	else if (view() == Shelves)
		drawShelves(dl, focused, selectDisc);
	else if (view() == Grid)
		drawGrid(dl, focused);
	else if (view() == List)
		drawList(dl, focused, selectDisc);
	else if (view() == Flow)
		drawFlow(dl, focused);
	else if (view() == Bookcase)
		drawBookcase(dl, focused);
	else if (view() == Showcase)
		drawShowcase(dl, focused, selectDisc);
	else if (view() == Wheel)
		drawWheel(dl, focused, selectDisc);
	else if (view() == Wall)
		drawWall(dl, focused);
	else if (view() == Discs)
		drawDiscs(dl, focused);
	else
		drawTable(dl, focused);
	endEntrance(content, e);
	// The letter just jumped to, large, for a moment.
	if (const float since = static_cast<float>(timeNow - lib.letterAt); since < 0.9f && lib.letter != 0)
	{
		const float a = (since < 0.6f) ? 1.f : 1.f - (since - 0.6f) / 0.3f;
		const float size = 190, lx = W - 96 - size, ly = 150;
		glow(dl, lx, ly, size, size, 28, alpha(col::accent, 0.6f * a), 18);
		rect(dl, lx, ly, size, size, alpha(col::sheet, 0.94f * a), 28);
		outline(dl, lx, ly, size, size, alpha(col::accent, a), 28, 3);
		const char shown[2] = {lib.letter, 0};
		const ImVec2 ls = textSize(bold(), 120, shown);
		text(dl, bold(), 120, lx + (size - ls.x) / 2, ly + (size - ls.y) / 2, alpha(col::text, a), shown);
	}

	// What the list is narrowed to, said at its top right.
	if (!selectDisc && lib.source != DiscDrive && narrowed())
	{
		// (A quick view by its name; else what was set by hand.)
		std::string chip = (quickNow >= 0 && quickNow < static_cast<int>(quickViews.size())) ?
							   std::string(ICON_FA_BOLT "   ") + quickViews[static_cast<size_t>(quickNow)].name :
							   narrowText();
		chip = fit(bold(), 22, chip, W * 0.5f);
		const ImVec2 cs = textSize(bold(), 22, chip.c_str());
		const float cw = cs.x + 44, cx = W - 96 - cw, cy = 112;
		rect(dl, cx, cy, cw, 44, alpha(col::sheet, 0.94f), 22);
		outline(dl, cx, cy, cw, 44, alpha(col::accent, 0.8f), 22, 2);
		text(dl, bold(), 22, cx + 22, cy + 22 - cs.y / 2, col::text, chip.c_str());
	}

	// What goes on in the background, quietly, above the hint bar: the scan, cover downloads.
	std::vector<std::string> notes;
	if (!empty && be->scanning())
		notes.push_back(std::string(ICON_FA_ROTATE) + "   Looking for games");
	const std::string background = be->backgroundNote();
	if (!background.empty())
		notes.push_back(std::string(ICON_FA_DOWNLOAD) + "   " + background);
	float noteY = H - 72 - 52;
	for (const std::string& note : notes)
	{
		const ImVec2 ns = textSize(regular(), 20, note.c_str());
		rect(dl, W - 96 - ns.x - 32, noteY, ns.x + 32, 36, col::rgba(0, 0, 0, 150), 18);
		text(dl, regular(), 20, W - 96 - ns.x - 16, noteY + 18 - ns.y / 2, col::dim, note.c_str());
		noteY -= 44;
	}

	if (lib.details && focused != nullptr)
		lib.detailsAnim = approach(lib.detailsAnim, 1, 16);
	else
		lib.detailsAnim = approach(lib.detailsAnim, 0, 20);
	if (lib.detailsAnim > 0.01f && focused != nullptr)
		detailsPanel(dl, *focused, lib.detailsAnim, selectDisc);

	topBar(dl, lib.source, selectDisc, Reach::All);
	const Glyph browse = (view() == List || view() == Table || view() == Wheel) ? Glyph::DPadUD : Glyph::DPadLR;
	const bool pictures = det.hasPicture[1] || det.hasPicture[2];
	if (lib.details && selectDisc)
		hintBar(dl, {{Glyph::Cross, "Insert"}, {Glyph::Circle, "Back"}});
	else if (lib.details && det.page == PageActions)
	{
		std::vector<Hint> hints = {{Glyph::DPadLR, "Choose"}};
		if (pictures)
			hints.push_back({Glyph::DPadUD, "Pictures"});
		if (setsGrouped && focused != nullptr && focused->setSize > 1)
			hints.push_back({Glyph::Square, "Next disc"});
		hints.insert(hints.end(), {{Glyph::L1, "Favourite"}, {Glyph::R1, "Hide"}, {Glyph::Cross, "Select"}, {Glyph::Circle, "Back"}});
		hintBar(dl, hints);
	}
	else if (lib.details)
		hintBar(dl, {{Glyph::DPadUD, "State"}, {Glyph::Square, "Delete"}, {Glyph::Cross, "Start from it"}, {Glyph::Circle, "Back"}});
	else if (selectDisc)
		hintBar(dl, {{Glyph::L1R1, "Source"}, {browse, "Browse"}, {Glyph::L2R2, "Letter"}, {Glyph::Cross, "Insert disc"},
						{Glyph::Triangle, "Details"}, {Glyph::Circle, "Back"}});
	else if (empty)
		hintBar(dl, {{Glyph::Circle, narrowed() ? "All games" : "Quit"}, {Glyph::L1R1, "Tab"}, {Glyph::Square, "List"}, {Glyph::Options, "Settings"}});
	else if (lib.source == DiscDrive)
	{
		std::vector<Hint> hints = {{Glyph::Circle, "Quit"}, {Glyph::L1R1, "Tab"}};
		if (lib.shelves.size() == 1 && lib.shelves[0].items.size() > 1)
			hints.push_back({Glyph::DPadLR, "Other disc"});
		hints.insert(hints.end(), {{Glyph::Options, "Settings"}, {Glyph::Square, "Make an image"}, {Glyph::Triangle, "Details"}, {Glyph::Cross, "Play"}});
		hintBar(dl, hints);
	}
	else
		hintBar(dl, {{Glyph::Circle, narrowed() ? "All games" : "Quit"}, {Glyph::L1R1, "Tab"}, {browse, "Browse"},
						{Glyph::L2R2, sortedByName() ? "Letter" : "Far"}, {Glyph::R3, "Quick view"}, {Glyph::TouchPad, "View"}, {Glyph::Square, "List"},
						{Glyph::Options, "Settings"}, {Glyph::Triangle, "Details"}, {Glyph::Cross, "Play"}});
	endScreen();
}

// ------------------------------------------------------------ loading screen

// The game being loaded, what the loader is doing, and how far it is.
void loadingScreen()
{
	ImDrawList* dl = beginScreen("##bp-loading", true);
	const LoadState state = be->loadState();
	const float done = std::clamp(state.progress, 0.f, 1.f);

	const float coverH = 480, coverW = std::round(coverH / CARD_SHAPE);
	const float w = coverW + 64 + 780, x = (W - w) / 2, y = 96 + (H - 72 - 96 - coverH) / 2;
	// The cover grows into place over a light of its own; the words come in beside it.
	const float e = entered(300);
	const Layer coverLayer = beginLayer(dl);
	if (motion() != MotionOff)
	{
		const float breath = (motion() == MotionFull) ? 0.75f + 0.25f * std::sin(static_cast<float>(timeNow) * 1.8f) : 1.f;
		softDisc(dl, x + coverW / 2, y + coverH / 2, coverH * 0.9f, alpha(col::accent, 0.20f * breath), 48);
	}
	if (!loadingGame.path.empty())
	{
		drawGameCover(dl, loadingGame, x, y, coverW, coverH, 18, 1);
	}
	else
	{
		rect(dl, x, y, coverW, coverH, col::card, 18);
		cruxMark(dl, x + coverW / 2, y + coverH / 2, coverW * 0.7f);
	}
	endLayer(coverLayer, e, 0, 0, 0.88f + 0.12f * e, x + coverW / 2, y + coverH / 2);
	const Layer words = beginLayer(dl);
	const float tx = x + coverW + 64, tw = w - coverW - 64;
	const std::string title = !loadingTitle.empty() ? loadingTitle : std::string("The console's own start-up");
	text(dl, bold(), 20, tx, y + 44, col::accent, "LOADING");
	const float titleH = text(dl, bold(), 56, tx, y + 78, col::text, title.c_str(), tw).y;
	float ly = y + 78 + std::min(titleH, 200.f) + 22;
	if (loadingGame.source == Network && !loadingGame.path.empty())
	{
		text(dl, regular(), 22, tx, ly, col::faint, ICON_FA_NETWORK_WIRED "   From the network share");
		ly += 40;
	}
	// The bar: the part done, or a light going along it while there is none to show.
	const float by = y + coverH - 96;
	const std::string line = !state.text.empty() ? state.text : std::string("Starting...");
	textIn(dl, regular(), 26, tx, by - 46, col::dim, line, tw, true);
	rect(dl, tx, by, tw, 12, alpha(col::text, 30 / 255.f), 6);
	if (state.progress >= 0 && done > 0.001f)
	{
		rect(dl, tx, by, std::max(12.f, tw * done), 12, col::accent, 6);
		char percent[16];
		std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(done * 100));
		const ImVec2 ps = textSize(bold(), 22, percent);
		text(dl, bold(), 22, tx + tw - ps.x, by + 24, col::dim, percent);
	}
	else
	{
		const float span = tw * 0.22f;
		const float at = static_cast<float>(std::fmod(timeNow * 0.7, 1.0)) * (tw + span) - span;
		const float from = std::max(0.f, at), to = std::min(tw, at + span);
		if (to > from)
			rect(dl, tx + from, by, to - from, 12, alpha(col::accent, 0.85f), 6);
	}
	// A step taking long: which one, and what to look at (ps5_boot_step.h).
	if (!state.slow.empty())
		text(dl, regular(), 22, tx, by + 56, col::faint, state.slow.c_str(), tw);
	endLayer(words, e, (1 - e) * 44, 0);

	topBar(dl, loadingGame.path.empty() ? lib.source : loadingGame.source, false, Reach::None);
	hintBar(dl, {});
	endScreen();
}

// ------------------------------------------------------------ names of games

// A game's name as shown: its file name without the extension and without the dump's tags,
// "Some Game (USA) (En,Fr,De).chd" -> "Some Game"; a disc number stays.
std::string cleanTitle(const std::string& name)
{
	std::string base = name;
	{
		const size_t dot = name.rfind('.');
		if (dot != std::string::npos && dot > 0 && name.size() - dot <= 5)
		{
			std::string e = name.substr(dot + 1);
			for (char& c : e)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			for (const char* known : {"iso", "chd", "cso", "zso", "bin", "cue", "gz", "elf", "mdf", "img", "irx", "nrg"})
			{
				if (e == known)
					base = name.substr(0, dot);
			}
		}
	}
	std::string out, disc;
	for (size_t i = 0; i < base.size(); i++)
	{
		const char c = base[i];
		if (c == '(' || c == '[')
		{
			const size_t end = base.find(c == '(' ? ')' : ']', i);
			if (end == std::string::npos)
				break;
			const std::string tag = base.substr(i + 1, end - i - 1);
			if (tag.rfind("Disc ", 0) == 0 || tag.rfind("Disk ", 0) == 0 || tag.rfind("DVD ", 0) == 0)
				disc = tag;
			i = end;
			continue;
		}
		out += (c == '_') ? ' ' : c;
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
		out.pop_back();
	while (!out.empty() && out.front() == ' ')
		out.erase(out.begin());
	size_t pos;
	while ((pos = out.find("  ")) != std::string::npos)
		out.erase(pos, 1);
	if (out.empty())
		return base;
	if (!disc.empty())
		out += "  \xc2\xb7  " + disc;
	return out;
}

// The disc number in a dump's name, and the name up to it. Taken: "Disc", "Disk", "CD" or "DVD"
// at the start of a word, then a number or a letter A to D, alone ("Disc 2", "CD2", "Disk B") -
// and with its brackets and "of 4" when it has them.
bool splitDisc(const std::string& base, std::string& rest, int& order)
{
	const auto alnum = [&](size_t i) { return i < base.size() && isalnum(static_cast<unsigned char>(base[i])); };
	for (size_t i = 0; i < base.size(); i++)
	{
		if (i > 0 && alnum(i - 1))
			continue;
		size_t at = std::string::npos;
		for (const char* word : {"disc", "disk", "cd", "dvd"})
		{
			if (strncasecmp(base.c_str() + i, word, strlen(word)) == 0)
			{
				at = i + strlen(word);
				break;
			}
		}
		if (at == std::string::npos)
			continue;
		while (at < base.size() && (base[at] == ' ' || base[at] == '_'))
			at++;
		size_t end = at;
		int number = 0;
		while (end < base.size() && isdigit(static_cast<unsigned char>(base[end])) && end - at < 2)
			number = number * 10 + (base[end++] - '0');
		if (end == at && at < base.size() && toupper(static_cast<unsigned char>(base[at])) >= 'A' &&
			toupper(static_cast<unsigned char>(base[at])) <= 'D')
		{
			number = toupper(static_cast<unsigned char>(base[at])) - 'A' + 1;
			end = at + 1;
		}
		if (end == at || number == 0)
			continue;
		// " of 4", "of4"
		for (const char* of : {" of ", "of"})
		{
			if (strncasecmp(base.c_str() + end, of, strlen(of)) == 0)
			{
				size_t count = end + strlen(of);
				const size_t digits = count;
				while (count < base.size() && isdigit(static_cast<unsigned char>(base[count])))
					count++;
				if (count > digits && !alnum(count))
				{
					end = count;
					break;
				}
			}
		}
		if (alnum(end))
			continue;
		size_t from = i;
		if (i > 0 && (base[i - 1] == '(' || base[i - 1] == '[') && end < base.size() && base[end] == (base[i - 1] == '(' ? ')' : ']'))
		{
			from = i - 1;
			end++;
		}
		// What comes before the tag is the game (and its region); what comes after is the disc's own.
		rest = base.substr(0, from);
		while (!rest.empty() && (rest.back() == ' ' || rest.back() == '-' || rest.back() == '_'))
			rest.pop_back();
		order = number;
		return !rest.empty();
	}
	return false;
}

} // namespace bp
