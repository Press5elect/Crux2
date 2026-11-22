// Crux2 - the interface: the settings' home and their search.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The home: a search bar (Square opens the search from anywhere on it), the eight areas as a grid of
// cards (each with what is in it, what is set now and how many of its settings are changed), and two
// cards under them, the pinned settings and the three changed last. Triangle shows only what is
// changed in the areas' pages. The search: every setting at every level, by its name, its
// description and its choices' names, and the library's games; a result is changed where it is
// (Cross, left and right), gone to (Triangle) or pinned (Square).

#include "bp_settings_model.h"
#include "bp_settings_text.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace bp::settings
{

namespace
{

// What each area's card says is set now: two of its settings (the first two of its rows when a game's
// settings have neither).
const char* const summaryIds[8][2] = {
	{"EmuCore/GS.upscale_multiplier", "PS5.HDR"},
	{"SPU2/Output.StandardVolume", "PS5UI.SoundSet"},
	{"Pad1.Type", "Pad2.Type"},
	{"Filenames.BIOS", "MemoryCards.Slot1_Filename"},
	{"Framerate.NominalScalar", "EmuCore/GS.UseVSyncForTiming"},
	{"DEV9/Eth.EthEnable", "Achievements.Enabled"},
	{"PS5UI.Look", "PS5UI.TextSize"},
	{"PS5.LogMode", "PS5.ThreadLayout"},
};

const Item* byBase(const std::string& base)
{
	// (Each area's card asks for two each frame: by an index made again with the rows.)
	State& st = state();
	static std::map<std::string, size_t> index;
	static int madeFor = -1;
	static const Item* madeAt = nullptr;
	static size_t madeSize = 0;
	if (madeFor != st.builds || madeAt != st.all.data() || madeSize != st.all.size())
	{
		madeFor = st.builds;
		madeAt = st.all.data();
		madeSize = st.all.size();
		index.clear();
		for (size_t i = 0; i < st.all.size(); i++)
			index.emplace(settingsBaseId(st.all[i].o), i);
	}
	const auto found = index.find(base);
	return (found == index.end()) ? nullptr : &st.all[found->second];
}

std::vector<const Item*> summaryOf(int area)
{
	State& st = state();
	std::vector<const Item*> items;
	for (const char* id : summaryIds[area])
	{
		if (const Item* it = byBase(id))
			items.push_back(it);
	}
	if (items.empty() && area < static_cast<int>(st.areas.size()))
	{
		for (const Section& s : st.areas[static_cast<size_t>(area)].sections)
		{
			for (const Item& it : s.rows)
			{
				if (isSetting(it.o) && it.o.kind != Option::Bind && items.size() < 2)
					items.push_back(&it);
			}
		}
	}
	return items;
}

// One line of an area's card: a setting's name, then its value, each cut to what is left of `w`
// (the value has the more of it). They stand still, in focus too: two moving side by side do not
// read (the area's page has them whole).
void summaryLine(ImDrawList* dl, float x, float y, float w, const Item& it)
{
	const Palette p = palette();
	const float size = 20, gap = 12;
	const std::string v = valueText(it.o);
	const float lw = textSize(regular(), size, it.o.label.c_str()).x, vw = textSize(bold(), size, v.c_str()).x;
	float labelW = lw, valueW = vw;
	if (lw + gap + vw > w)
	{
		valueW = std::min(vw, std::max(w * 0.55f, w - gap - lw));
		labelW = std::max(0.f, w - gap - valueW);
	}
	textIn(dl, regular(), size, x, y, p.secondary, it.o.label, labelW, false);
	textIn(dl, bold(), size, x + labelW + gap, y, it.changed ? p.accent : p.text, v, valueW, false);
}

// Goes to a row's page, the level raised when the row is not shown at the level shown now.
void goToId(const std::string& id)
{
	State& st = state();
	int area = 0, section = 0;
	if (!placeOf(id, area, section))
	{
		const Item* it = findItem(id);
		if (it == nullptr)
			return;
		const int level = levelOf(it->o);
		if (level > options().level)
		{
			setSettingsLevel(level);
			toast(std::string("Shown from ") + levelNames[level] + " up: the level is now " + levelNames[level] + ".", 3.f);
		}
		rebuild();
		if (!placeOf(id, area, section))
			return;
	}
	goTo(area, section, id);
	st.inRows = true;
}

// The entries of the pinned and changed-lately cards: the rows they name that are there.
std::vector<const Item*> entriesOf(const std::vector<std::string>& ids)
{
	std::vector<const Item*> out;
	for (const std::string& id : ids)
	{
		if (const Item* it = findItem(id))
			out.push_back(it);
	}
	return out;
}

// ---- the search's results

struct Result
{
	int item = -1;  // into State::all
	GameInfo game;  // a game (item -1)
};

struct Search
{
	std::string key;
	std::vector<Result> results;
	int at = -1000;
} s_search;

std::string lower(std::string s)
{
	for (char& c : s)
		c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	return s;
}

std::vector<std::string> words(const std::string& query)
{
	std::vector<std::string> out;
	std::string w;
	for (const char c : lower(query) + " ")
	{
		if (c == ' ')
		{
			if (!w.empty())
				out.push_back(w);
			w.clear();
		}
		else
		{
			w += c;
		}
	}
	return out;
}

bool matches(const std::string& haystack, const std::vector<std::string>& needles)
{
	for (const std::string& n : needles)
	{
		if (haystack.find(n) == std::string::npos)
			return false;
	}
	return true;
}

std::string pathOf(const Option& o)
{
	if (const SettingsText* t = settingsTextOf(o))
		return std::string(t->area) + "  \xc2\xb7  " + t->section;
	return std::string();
}

const std::vector<Result>& results()
{
	State& st = state();
	char key[160];
	std::snprintf(key, sizeof(key), "%d/%d/%d/%d/", st.chip, st.builds, st.forGame ? 1 : 0, options().level);
	const std::string k = key + st.query;
	// (Changed from default: looked at again now and then, the values may have changed.)
	if (k == s_search.key && !(st.chip == 3 && frameNow - s_search.at > 30))
		return s_search.results;
	// (Looked at again with the same words: the row in focus stays, even when it has reached its
	// default, until the focus moves; the focus follows it.)
	int keep = -1;
	if (st.chip == 3 && k == s_search.key && st.result >= 0 && st.result < static_cast<int>(s_search.results.size()))
		keep = s_search.results[static_cast<size_t>(st.result)].item;
	s_search.key = k;
	s_search.at = frameNow;
	s_search.results.clear();
	const std::vector<std::string> needles = words(st.query);
	const bool settingsToo = st.chip != 2;
	const bool gamesToo = (st.chip == 0 || st.chip == 2) && !needles.empty();
	if (settingsToo && (!needles.empty() || st.chip == 3))
	{
		for (size_t i = 0; i < st.all.size(); i++)
		{
			const Item& it = st.all[i];
			if (it.o.kind == Option::Heading)
				continue;
			if (st.chip == 3 && !it.changed && static_cast<int>(i) != keep)
				continue;
			std::string hay = it.o.label + " " + it.o.codeLabel + " " + it.o.desc + " " + pathOf(it.o);
			for (const std::string& n : it.o.names)
				hay += " " + n;
			if (matches(lower(hay), needles))
			{
				if (static_cast<int>(i) == keep)
					st.result = static_cast<int>(s_search.results.size());
				s_search.results.push_back({static_cast<int>(i), GameInfo()});
			}
		}
	}
	if (gamesToo)
	{
		for (const GameInfo& g : knownGames())
		{
			if (!g.hidden && matches(lower(g.title + " " + g.serial), needles))
				s_search.results.push_back({-1, g});
		}
	}
	return s_search.results;
}

void askQuery()
{
	editText("Search Settings", state().query, false, [](const std::string& typed) {
		State& st = state();
		st.query = typed;
		st.result = results().empty() ? -1 : 0;
	});
}

void card(ImDrawList* dl, float x, float y, float w, float h, bool on, bool raised)
{
	const Palette p = palette();
	cardShape(dl, x, y, w, h, (on || raised) ? p.raised : p.card, 16);
	if (on)
		focusRing(dl, x, y, w, h, 16);
}

} // namespace

void openSearch(View from)
{
	State& st = state();
	if (st.view != View::Search)
		st.searchBack = from;
	st.view = View::Search;
	st.result = -1;
	askQuery();
}

void homeScreen(ImDrawList* dl)
{
	State& st = state();
	const Palette p = palette();
	// (Pointers into the rows: taken again after the input, which may have made the rows anew.)
	std::vector<const Item*> pinnedItems = entriesOf(pins());
	std::vector<const Item*> lately = entriesOf(recent());
	int pinnedShown = static_cast<int>(pinnedItems.size()); // (every pin reachable, in the quick menu's order)
	int latelyShown = std::min(static_cast<int>(lately.size()), 3);

	// ---- input
	int& f = st.home;
	f = std::clamp(f, 0, 10);
	const auto areaUsable = [&st](int a) { return a >= 0 && a < static_cast<int>(st.areas.size()) && !st.areas[static_cast<size_t>(a)].sections.empty(); };
	if (in.up)
	{
		if (f >= 1 && f <= 4)
			f = 0;
		else if (f >= 5 && f <= 8)
			f -= 4;
		else if (f == 9 || f == 10)
		{
			if (st.homeInner > 0)
				st.homeInner--;
			else
				f = (f == 9) ? 5 : 7;
		}
	}
	if (in.down)
	{
		if (f == 0)
			f = 1;
		else if (f >= 1 && f <= 4)
			f += 4;
		else if (f >= 5 && f <= 8)
		{
			f = (f <= 6) ? 9 : 10;
			st.homeInner = 0;
		}
		else if (f == 9 && st.homeInner + 1 < pinnedShown)
			st.homeInner++;
		else if (f == 10 && st.homeInner + 1 < latelyShown)
			st.homeInner++;
	}
	if (in.left)
	{
		if ((f >= 2 && f <= 4) || (f >= 6 && f <= 8))
			f--;
		else if (f == 10)
		{
			f = 9;
			st.homeInner = 0;
		}
	}
	if (in.right)
	{
		if ((f >= 1 && f <= 3) || (f >= 5 && f <= 7))
			f++;
		else if (f == 9)
		{
			f = 10;
			st.homeInner = 0;
		}
	}
	if (in.accept)
	{
		if (f == 0)
			openSearch(View::Home);
		else if (f >= 1 && f <= 8)
		{
			if (areaUsable(f - 1))
				goTo(f - 1, 0, "");
			else
				toast(st.forGame ? "A game has no settings of its own here." : "Nothing here at this level.", 2.5f);
		}
		else if (f == 9 && st.homeInner < pinnedShown)
			goToId(settingsIdOf(pinnedItems[static_cast<size_t>(st.homeInner)]->o));
		else if (f == 10 && st.homeInner < latelyShown)
			goToId(settingsIdOf(lately[static_cast<size_t>(st.homeInner)]->o));
		if (st.view != View::Home)
			return;
	}
	if (in.square)
	{
		openSearch(View::Home);
		return;
	}
	if (in.triangle)
	{
		st.onlyChanged = !st.onlyChanged;
		toast(st.onlyChanged ? "Only the settings changed from their defaults are shown." : "Every setting is shown.", 2.4f);
	}
	if (in.touch && st.gameOpen)
		switchLayer();
	if (in.back)
	{
		leave();
		return;
	}

	// ---- drawing
	pinnedItems = entriesOf(pins());
	lately = entriesOf(recent());
	pinnedShown = static_cast<int>(pinnedItems.size());
	latelyShown = std::min(static_cast<int>(lately.size()), 3);
	st.homeInner = std::clamp(st.homeInner, 0, std::max(0, (f == 9 ? pinnedShown : latelyShown) - 1));
	const float e = entered(st.forGame ? 203 : 202);
	const Layer content = beginLayer(dl);
	header(dl, nullptr, false);
	if (st.onlyChanged)
	{
		const char* chip = "Showing only changed";
		const float cw = textSize(bold(), 20, chip).x + 32;
		rect(dl, W - 72 - cw - (st.gameOpen ? 520 : 0), 116, cw, 40, alpha(p.accent, 0.2f), 20);
		text(dl, bold(), 20, W - 72 - cw - (st.gameOpen ? 520 : 0) + 16, 125, p.accent, chip);
	}
	const float x0 = 72, fullW = W - 144;
	// The search bar.
	{
		const float y = 196, h = 68;
		card(dl, x0, y, fullW, h, f == 0, false);
		text(dl, regular(), 26, x0 + 28, y + h / 2 - 15, p.secondary, ICON_FA_MAGNIFYING_GLASS);
		text(dl, regular(), 26, x0 + 72, y + h / 2 - 15, p.secondary,
			st.query.empty() ? "Search every setting and game" : ("\"" + st.query + "\"").c_str());
		const char* hint = "Search";
		const float hw = textSize(regular(), 22, hint).x;
		text(dl, regular(), 22, x0 + fullW - 28 - hw, y + h / 2 - 13, p.secondary, hint);
		glyph(dl, Glyph::Square, x0 + fullW - 28 - hw - 46, y + h / 2, 34);
	}
	// The eight areas. (Laid out by the text's measured heights: the name and what is in it from the
	// top, what is set now, two lines, from the bottom; at every text size.)
	const float gap = 24, cw = (fullW - 3 * gap) / 4, ch = 224, areasY = 288;
	const float lineH = textSize(regular(), 20, "Ag").y, nameH = textSize(bold(), 30, "Ag").y;
	for (int i = 0; i < 8 && i < static_cast<int>(st.areas.size()); i++)
	{
		const Area& a = st.areas[static_cast<size_t>(i)];
		const float x = x0 + (i % 4) * (cw + gap), y = areasY + (i / 4) * (ch + gap);
		const bool on = (f == 1 + i);
		const bool can = areaUsable(i);
		card(dl, x, y, cw, ch, on, false);
		const ImU32 t1 = can ? p.text : p.faint, t2 = can ? p.secondary : p.faint;
		const float nameY = y + 66, aboutY = nameY + nameH + 6;
		const float nowY = std::max(aboutY + lineH + 10, y + ch - 24 - 2 * lineH - 4);
		text(dl, regular(), 30, x + 28, y + 24, can ? p.accent : p.faint, a.icon);
		textIn(dl, bold(), 30, x + 28, nameY, t1, a.name, cw - 56, on);
		textIn(dl, regular(), 20, x + 28, aboutY, t2, a.about, cw - 56, on);
		if (can)
		{
			const std::vector<const Item*> now = summaryOf(i);
			for (size_t k = 0; k < now.size() && k < 2; k++)
				summaryLine(dl, x + 28, nowY + static_cast<float>(k) * (lineH + 4), cw - 56, *now[k]);
		}
		else if (st.forGame)
			textIn(dl, regular(), 20, x + 28, nowY, p.faint, "Uses the global settings", cw - 56, on);
		if (a.changed > 0)
		{
			char pill[32];
			std::snprintf(pill, sizeof(pill), "%d changed", a.changed);
			const float pw = textSize(bold(), 18, pill).x + 24;
			rect(dl, x + cw - 24 - pw, y + 28, pw, 32, p.accent, 16);
			text(dl, bold(), 18, x + cw - 24 - pw + 12, y + 33, p.onAccent, pill);
		}
	}
	// Pinned, and changed lately.
	{
		const float y = areasY + 2 * (ch + gap), h = H - 72 - 24 - y, w = (fullW - gap) / 2;
		const auto list = [&](int which, const std::string& title, const std::vector<const Item*>& items, int count, const char* none) {
			const float x = x0 + which * (w + gap);
			const bool on = (f == 9 + which);
			card(dl, x, y, w, h, false, false);
			text(dl, bold(), 18, x + 28, y + 20, p.accent, title.c_str());
			if (items.empty())
			{
				text(dl, regular(), 21, x + 28, y + 62, p.faint, none, w - 56);
				if (on)
					focusRing(dl, x, y, w, h, 16);
				return;
			}
			// As many rows as the card holds, in the quick menu's order; when they do not all fit, the focus
			// scrolls them (one column, as the quick menu's panel).
			const float lh = 34.f;
			const int rows = std::max(1, static_cast<int>((h - 60) / lh));
			const int shown = std::min(count, rows);
			const int first = on ? std::clamp(st.homeInner - (shown - 1), 0, count - shown) : 0;
			if (count > shown)
			{
				// Where the rows shown are among them all.
				const std::string where = std::to_string(first + 1) + "\xe2\x80\x93" + std::to_string(first + shown) + " of " + std::to_string(count);
				const float ww = textSize(bold(), 18, where.c_str()).x;
				text(dl, bold(), 18, x + w - 28 - ww, y + 20, p.secondary, where.c_str());
			}
			const float colW = w - 28;
			for (int i = 0; i < shown && first + i < count; i++)
			{
				const Item& it = *items[static_cast<size_t>(first + i)];
				const float cx = x + 14;
				const float ly = y + 56 + static_cast<float>(i) * lh;
				const bool here = on && first + i == st.homeInner;
				const float boxW = colW;
				if (here)
				{
					rect(dl, cx, ly - 1, boxW, lh - 2, p.raised, 10);
					focusRing(dl, cx, ly - 1, boxW, lh - 2, 10);
				}
				const float size = 21.f;
				const std::string v = valueText(it.o);
				const float vw = std::min(textSize(bold(), size, v.c_str()).x, boxW * 0.4f);
				const float right = cx + boxW - 14;
				textIn(dl, bold(), size, right - vw, ly + 4, it.changed ? p.accent : p.secondary, v, boxW * 0.4f, here);
				textIn(dl, regular(), size, cx + 14, ly + 4, p.text, it.o.label, right - vw - 12 - (cx + 14), here);
			}
		};
		list(0, pinnedShown > 1 ? "PINNED (" + std::to_string(pinnedShown) + ")" : std::string("PINNED"), pinnedItems, pinnedShown,
			"Press Square on any setting to pin it here.");
		list(1, "CHANGED LATELY", lately, latelyShown, "The last three settings changed are shown here.");
	}
	endEntrance(content, e);

	std::vector<Hint> hints{{Glyph::Cross, "Open"}, {Glyph::Square, "Search"}, {Glyph::Triangle, st.onlyChanged ? "Show all" : "Show only changed"}};
	if (st.gameOpen)
		hints.push_back({Glyph::TouchPad, st.forGame ? "All games" : "This game"});
	hints.push_back({Glyph::Options, "Close"});
	hints.push_back({Glyph::Circle, "Back"});
	hintBar(dl, hints);
}

void searchScreen(ImDrawList* dl)
{
	State& st = state();
	const Palette p = palette();
	const std::vector<Result>* list = &results();
	const int n = static_cast<int>(list->size());
	st.result = std::clamp(st.result, -1, n - 1);

	// ---- input
	if (in.l1 || in.r1)
	{
		st.chip = std::clamp(st.chip + (in.r1 ? 1 : -1), 0, 3);
		list = &results();
		st.result = list->empty() ? -1 : std::min(std::max(st.result, 0), static_cast<int>(list->size()) - 1);
	}
	const int count = static_cast<int>(list->size());
	if (in.up)
		st.result = std::max(-1, st.result - 1);
	if (in.down)
		st.result = std::min(count - 1, st.result + 1);
	if (in.l2)
		st.result = std::max(-1, st.result - 6);
	if (in.r2)
		st.result = std::min(count - 1, st.result + 6);
	if (st.result < 0)
	{
		if (in.accept || in.square)
		{
			askQuery();
			return;
		}
	}
	else if (st.result < count)
	{
		const Result r = (*list)[static_cast<size_t>(st.result)];
		if (r.item < 0)
		{
			if (in.accept)
			{
				// (The settings of the game that are open, if any, are closed first.)
				const GameInfo game = r.game;
				if (st.gameOpen)
				{
					be->closeGame();
					ownSettingsChanged();
					st.gameOpen = false;
					st.forGame = false;
				}
				openSettings(&game, st.back);
				if (!st.gameOpen)
					st.madeFor = ~0u;
				return;
			}
		}
		else if (static_cast<size_t>(r.item) < st.all.size())
		{
			Option& o = st.all[static_cast<size_t>(r.item)].o;
			if (in.accept)
				activate(o);
			if (in.left && (o.kind != Option::Action && o.kind != Option::Info && o.kind != Option::Bind))
				change(o, -1, in.repeats);
			if (in.right && (o.kind != Option::Action && o.kind != Option::Info && o.kind != Option::Bind))
				change(o, 1, in.repeats);
			if (in.square)
				togglePin(o);
			if (in.l3)
				reset(o);
			if (in.triangle)
			{
				goToId(settingsIdOf(o));
				return;
			}
		}
	}
	if (in.touch && st.gameOpen)
		switchLayer();
	if (in.back)
	{
		st.view = st.searchBack == View::Area ? View::Area : View::Home;
		return;
	}
	if (screen != Screen::Settings || st.view != View::Search)
		return;
	list = &results();
	st.result = std::clamp(st.result, -1, static_cast<int>(list->size()) - 1);

	// ---- drawing
	const float e = entered(st.forGame ? 207 : 206);
	const Layer content = beginLayer(dl);
	header(dl, "Search", false);
	const float x0 = 72, dw = 560, dx = W - 72 - dw;
	const float lw = dx - 28 - x0;
	// The field.
	{
		const float y = 196, h = 68;
		card(dl, x0, y, lw, h, st.result < 0, false);
		text(dl, regular(), 26, x0 + 28, y + h / 2 - 15, p.secondary, ICON_FA_MAGNIFYING_GLASS);
		const bool empty = st.query.empty();
		textIn(dl, empty ? regular() : bold(), 26, x0 + 72, y + h / 2 - 15, empty ? p.faint : p.text, empty ? std::string("Type to search every setting and game") : st.query,
			lw - 200, false);
		const char* hint = "Type";
		const float hw = textSize(regular(), 22, hint).x;
		text(dl, regular(), 22, x0 + lw - 28 - hw, y + h / 2 - 13, p.secondary, hint);
		glyph(dl, Glyph::Cross, x0 + lw - 28 - hw - 46, y + h / 2, 34);
	}
	// The chips.
	{
		static const char* const chips[4] = {"Everything", "Settings", "Games", "Changed from Default"};
		float x = x0;
		x += glyph(dl, Glyph::L1, x, 304, 30) + 14;
		for (int i = 0; i < 4; i++)
		{
			const float w = textSize(bold(), 20, chips[i]).x + 36;
			const bool on = (i == st.chip);
			rect(dl, x, 284, w, 40, on ? p.accent : p.card, 20);
			text(dl, bold(), 20, x + 18, 293, on ? p.onAccent : p.secondary, chips[i]);
			x += w + 10;
		}
		glyph(dl, Glyph::R1, x + 4, 304, 30);
		char found[48];
		std::snprintf(found, sizeof(found), "%d found", static_cast<int>(list->size()));
		const float fw = textSize(regular(), 20, found).x;
		text(dl, regular(), 20, x0 + lw - fw, 293, p.secondary, found);
	}
	// The results.
	const float top = 344, bottom = H - 72 - 24, rh = 76;
	rect(dl, x0, top, lw, bottom - top, p.card, 16);
	if (list->empty())
	{
		const char* none = st.query.empty() && st.chip != 3 ? "Press Cross to type what to look for." : "Nothing found.";
		text(dl, regular(), 22, x0 + 32, top + 32, p.faint, none);
	}
	const float view = bottom - top - 16;
	const float target = std::clamp(std::max(0, st.result) * rh - view * 0.4f, 0.f, std::max(0.f, list->size() * rh - view));
	st.resultScroll = motion() == MotionOff ? target : approach(st.resultScroll, target, 14);
	dl->PushClipRect(V(x0 - 10, top + 4), V(x0 + lw + 10, bottom - 4), true);
	for (int i = 0; i < static_cast<int>(list->size()); i++)
	{
		const float y = top + 8 + i * rh - st.resultScroll;
		if (y + rh < top || y > bottom)
			continue;
		const Result& r = (*list)[static_cast<size_t>(i)];
		const bool on = (i == st.result);
		if (on)
		{
			rect(dl, x0 + 12, y + 2, lw - 24, rh - 6, p.raised, 12);
			focusRing(dl, x0 + 12, y + 2, lw - 24, rh - 6, 12);
		}
		const float right = x0 + lw - 32;
		if (r.item < 0)
		{
			const char* what = "Open Settings";
			const float ww = textSize(regular(), 21, what).x;
			text(dl, regular(), 21, right - ww, y + rh / 2 - 13, p.secondary, what);
			textIn(dl, bold(), 24, x0 + 44, y + 10, p.text, r.game.title, lw - 120 - ww, on);
			text(dl, regular(), 19, x0 + 44, y + 44, p.faint, ("Game  \xc2\xb7  " + (r.game.serial.empty() ? std::string(kindText(r.game)) : r.game.serial)).c_str());
			continue;
		}
		if (static_cast<size_t>(r.item) >= st.all.size())
			continue;
		const Item& it = st.all[static_cast<size_t>(r.item)];
		const std::string v = valueText(it.o);
		const float vw = std::min(textSize(bold(), 22, v.c_str()).x, lw * 0.36f);
		textIn(dl, bold(), 22, right - vw, y + rh / 2 - 13, it.changed ? p.accent : p.secondary, v, lw * 0.36f, on);
		if (it.changed)
			dl->AddCircleFilled(V(x0 + 26, y + 24), 5 * S, p.accent);
		textIn(dl, bold(), 24, x0 + 44, y + 10, usable(it.o) ? p.text : p.faint, it.o.label, lw - 120 - vw, on);
		float px = x0 + 44;
		const int level = levelOf(it.o);
		if (level > 0)
		{
			const float tw = textSize(bold(), 15, levelNames[level]).x + 16;
			outline(dl, px, y + 44, tw, 24, p.secondary, 12, 1.5f);
			text(dl, bold(), 15, px + 8, y + 47, p.secondary, levelNames[level]);
			px += tw + 10;
		}
		textIn(dl, regular(), 19, px, y + 45, p.faint, pathOf(it.o), lw - 120 - vw - (px - x0 - 44), false);
	}
	dl->PopClipRect();
	// The side panel.
	if (st.result >= 0 && st.result < static_cast<int>(list->size()) && (*list)[static_cast<size_t>(st.result)].item >= 0 &&
		static_cast<size_t>((*list)[static_cast<size_t>(st.result)].item) < st.all.size())
	{
		describe(dl, st.all[static_cast<size_t>((*list)[static_cast<size_t>(st.result)].item)], dx, 196, dw, bottom - 196);
	}
	else
	{
		rect(dl, dx, 196, dw, bottom - 196, p.card, 16);
		text(dl, bold(), 26, dx + 32, 226, p.text, "Search", dw - 64);
		text(dl, regular(), 21, dx + 32, 272, p.secondary,
			"Looks through every setting at every level, by its name, its description and its choices, and through the games of the library. "
			"Cross changes a setting where it is, Triangle goes to its page and Square pins it to the settings' home.",
			dw - 64);
	}
	endEntrance(content, e);

	std::vector<Hint> hints{{Glyph::L1R1, "Filter"}};
	if (st.result < 0)
		hints.push_back({Glyph::Cross, "Type"});
	else if (st.result < static_cast<int>(list->size()) && (*list)[static_cast<size_t>(st.result)].item < 0)
		hints.push_back({Glyph::Cross, "Open"});
	else
	{
		hints.push_back({Glyph::Cross, "Change"});
		hints.push_back({Glyph::Triangle, "Go to its page"});
		hints.push_back({Glyph::Square, "Pin"});
	}
	hints.push_back({Glyph::Circle, "Back"});
	hintBar(dl, hints);
}

} // namespace bp::settings
