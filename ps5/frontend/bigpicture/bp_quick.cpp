// Crux2 - the interface: the quick menu over a paused game, and which screen is on.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0.
//
// The quick menu over the paused game (laid out anew in the interface's revamp, phase 4): at the foot
// of the veiled picture, how long the game was played this time and its name; a row of tiles, always
// there (Resume, Save state, Load state, Screenshot, Change disc, Quit game), each with what it would
// do now; the settings pinned in the Settings ([PS5UI] Pins: bp_settings.cpp's rows, changed with
// left and right as there, the game's own value where a game can have one); and More, the rest of
// the menu's entries.
//
// Triangle in the menu chooses what More shows and in which order (bp_quick_layout.h): its own
// entries, and any of the game's own options, changed there with left and right. A setting that a
// running game takes only when it starts (Option::NextStart), changed while it runs, puts "Apply
// changes now" at the top of More: the game is closed with its state saved and started again from it.

#include "bp_internal.h"
#include "bp_quick_layout.h"
#include "bp_settings_model.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace bp
{

Screen screen = Screen::Library;

namespace
{

struct QuickItem
{
	const char* icon;
	std::string label;
	std::string detail;
	bool enabled = true;
	std::function<void()> run;
	std::function<void(int)> side; // left/right
	bool stays = false;            // running it keeps the menu open
	bool state = false;            // a row of the saved states: the slot's card is shown beside it
	std::string id;                // its name in the layout (bp_quick_layout.h)
	bool applies = true;           // it is there now (the auto-save, the undo, the achievements are not always)
	bool option = false;           // one of the game's own options
	std::string page;              // an option's: the page of the settings it is on
};

// A value an option is made of (a combination's parts: several), as kept: for the game's own options
// and for what tells whether a setting the game takes at its start was changed.
struct ValuePart
{
	std::string section, key;
	int type = 0;                  // 0 switch, 1 whole number, 2 number, 3 text
	std::string def;
	std::function<std::optional<bool>()> gameDefault;
	bool own = false;              // the game can have a value of its own
};

struct QuickOpt
{
	Option o;
	std::string id, page;
};

struct QuickState
{
	int focus = 0;
	float open = 0;
	float focusAnim = 0;
	// The screen that was up when the Options button last went down: the quick menu's button
	// closes the menu only when it was pressed in the menu (a press that left the Settings for
	// the menu must not close the menu as it comes up).
	Screen optionsPressedIn = Screen::None;
	bool optionsWasDown = false;
	// A game is being closed with a state saved to go on from: what the wait says.
	bool closingSaves = false;
	// "Restart game" was chosen once: until then, choosing it again restarts.
	double restartAsked = -10;
	// The states of the running game, looked up when the menu opens and after one is saved.
	std::vector<StateInfo> states;
	double statesAt = -10;
	std::string statesFor;
	// The entry the cursor is on (it stays on it when entries come or go), and the first row shown
	// when there are more than the panel has room for.
	std::string focusId;
	int first = 0;
	// Triangle: choosing what the menu shows.
	bool edit = false;
	quicklayout::Edit editing;
	// The game's own settings, open while the menu shows any of its options (Backend::openGame).
	bool ownOpen = false;
	std::vector<QuickOpt> opts;
	std::string optsFor;
	// What the running game's settings it takes at its start were when the menu first opened for it;
	// changed since: "Apply changes now".
	bool entered = false;
	std::string snapFor;
	std::vector<ValuePart> nextStart;
	std::string snapshot;
	bool applyPending = false;
	// Chosen: the game is being closed, to be started again from its state.
	bool applyWanted = false;
	bool applying = false;
	GameInfo applyGame;
	time_t applyBefore = -1; // when the state made as it closes was saved before (-1: there was none)
	// Where the focus is: 0 the tiles, 1 the pinned settings, 2 More (`focus` is More's row, `first`
	// the first of them shown); the tile, the pinned row and the first of those shown.
	int zone = 0;
	int tile = 0;
	int pin = 0;
	int pinFirst = 0;
	// The pinned settings as they were last made, and what for (the game, its settings open, the
	// pages, the pins).
	std::vector<settings::PinnedRow> pinned;
	std::string pinnedFor;
	int pinnedAt = -1; // the frame they were last made in
	// How many of the game's patches and cheats are on (More says it), and for which game.
	int patchesOn = 0;
	std::string patchesFor;
} qm;

// The tiles, always there, in this order: the rest of the menu's entries are More's.
const char* const tileIds[] = {"resume", "save", "load", "screenshot", "disc", "quit"};

bool isTile(const std::string& id)
{
	return std::any_of(std::begin(tileIds), std::end(tileIds), [&id](const char* tile) { return id == tile; });
}

std::vector<std::string> withoutTiles(std::vector<std::string> ids)
{
	std::erase_if(ids, isTile);
	return ids;
}

// ---- what the menu shows: [PS5UI] QuickLayout (empty: the menu as it comes)

std::vector<std::string> layoutNow()
{
	return quicklayout::parse(be->getString(Shared, "PS5UI", "QuickLayout").value_or(std::string()));
}

void keepLayout(const std::vector<std::string>& ids)
{
	const std::string line = quicklayout::serialise(ids);
	if (be->getString(Shared, "PS5UI", "QuickLayout").value_or(std::string()) != line)
		be->setString(Shared, "PS5UI", "QuickLayout", line);
}

// Editing ends with the menu (and with the game): what was chosen is kept.
void endEdit()
{
	if (!qm.edit)
		return;
	qm.edit = false;
	// (More as it comes, whatever the tiles' places in the line: kept as the menu as it comes.)
	std::vector<std::string> ids = qm.editing.layout();
	if (withoutTiles(ids) == withoutTiles(quicklayout::defaults()))
		ids = quicklayout::defaults();
	keepLayout(ids);
	qm.focus = 0;
	qm.focusId.clear();
}

std::string runId(const Running& running)
{
	return running.serial + "/" + std::to_string(running.crc);
}

// The running game as the library (or, for one in a drive, the disc drive) has it.
GameInfo runningGame(const Running& running, bool fromDisc)
{
	const GameInfo* listed = libraryGame(running.path);
	if (listed == nullptr && fromDisc)
	{
		for (const GameInfo& disc : discNow().games)
		{
			if (disc.path == running.path)
				listed = &disc;
		}
	}
	GameInfo game = (listed != nullptr) ? *listed : GameInfo();
	if (listed == nullptr)
	{
		game.path = running.path;
		game.title = running.title;
		const size_t slash = running.path.rfind('/');
		game.fileName = (slash == std::string::npos) ? running.path : running.path.substr(slash + 1);
	}
	game.serial = running.serial;
	game.crc = running.crc;
	if (game.title.empty())
		game.title = running.title;
	return game;
}

// ---- the game's own settings, while the menu needs them

bool openOwn(const Running& running)
{
	if (!qm.ownOpen && running.valid && running.crc != 0)
	{
		qm.ownOpen = be->openGame(runningGame(running, false));
		ownSettingsChanged();
	}
	return qm.ownOpen;
}

// (Before anything that may open the settings, which open the game's own themselves.)
void closeOwn()
{
	if (!qm.ownOpen)
		return;
	qm.ownOpen = false;
	be->closeGame();
	ownSettingsChanged();
}

std::vector<ValuePart> partsOf(const Option& o, bool own)
{
	std::vector<ValuePart> parts;
	const auto add = [&](const std::string& section, const std::string& key, int type, std::string def) {
		ValuePart p;
		p.section = section;
		p.key = key;
		p.type = type;
		p.def = std::move(def);
		p.own = own;
		parts.push_back(std::move(p));
	};
	char number[32];
	switch (o.kind)
	{
		case Option::Bool:
			add(o.section, o.key, 0, o.defBool ? "1" : "0");
			parts.back().gameDefault = o.gameDefault;
			break;
		case Option::IntList:
		case Option::IntRange:
			add(o.section, o.key, 1, std::to_string(o.defInt));
			break;
		case Option::FloatList:
		case Option::FloatRange:
			std::snprintf(number, sizeof(number), "%g", o.defFloat);
			add(o.section, o.key, 2, number);
			break;
		case Option::StringList:
			add(o.section, o.key, 3, o.defString);
			break;
		case Option::Combo:
			for (const Option::Part& part : o.parts)
				add(part.section, part.key, part.isInt ? 1 : 0, part.isInt ? std::to_string(part.defInt) : std::string(part.def ? "1" : "0"));
			break;
		default:
			break;
	}
	return parts;
}

// A value as it is for the running game: its own (when it has one and its settings are open), else
// Crux2's default for it, else the settings'.
std::string partNow(const ValuePart& p, bool sharedOnly)
{
	const char* const s = p.section.c_str();
	const char* const k = p.key.c_str();
	const bool own = p.own && qm.ownOpen && !sharedOnly;
	char number[32];
	switch (p.type)
	{
		case 0:
		{
			std::optional<bool> v = own ? be->getBool(Own, s, k) : std::nullopt;
			if (!v && own && p.gameDefault)
				v = p.gameDefault();
			if (!v)
				v = be->getBool(Shared, s, k);
			return v ? std::string(*v ? "1" : "0") : p.def;
		}
		case 1:
		{
			std::optional<int> v = own ? be->getInt(Own, s, k) : std::nullopt;
			if (!v)
				v = be->getInt(Shared, s, k);
			return v ? std::to_string(*v) : p.def;
		}
		case 2:
		{
			std::optional<float> v = own ? be->getFloat(Own, s, k) : std::nullopt;
			if (!v)
				v = be->getFloat(Shared, s, k);
			if (!v)
				return p.def;
			std::snprintf(number, sizeof(number), "%g", *v);
			return number;
		}
		default:
		{
			std::optional<std::string> v = own ? be->getString(Own, s, k) : std::nullopt;
			if (!v)
				v = be->getString(Shared, s, k);
			return v ? *v : p.def;
		}
	}
}

std::string optionNow(const Option& o, bool sharedOnly)
{
	std::string out;
	for (const ValuePart& p : partsOf(o, true))
		out += partNow(p, sharedOnly) + "\n";
	return out;
}

bool choiceKind(const Option& o)
{
	return o.kind == Option::IntList || o.kind == Option::FloatList || o.kind == Option::StringList || o.kind == Option::Combo;
}

int choices(const Option& o)
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

// Which of its choices an option is on for the running game; -1 when none.
int choiceOf(const Option& o)
{
	const std::vector<ValuePart> parts = partsOf(o, true);
	std::vector<std::string> now;
	for (const ValuePart& p : parts)
		now.push_back(partNow(p, false));
	for (int c = 0; c < choices(o); c++)
	{
		const size_t i = static_cast<size_t>(c);
		switch (o.kind)
		{
			case Option::IntList:
				if (now[0] == std::to_string(o.ints[i]))
					return c;
				break;
			case Option::FloatList:
				if (std::fabs(std::strtof(now[0].c_str(), nullptr) - o.floats[i]) < 0.0005f)
					return c;
				break;
			case Option::StringList:
				if (now[0] == o.strings[i])
					return c;
				break;
			case Option::Combo:
			{
				bool same = true;
				for (size_t p = 0; p < parts.size() && same; p++)
					same = (parts[p].type == 1) ? (now[p] == std::to_string(o.combos[i][p])) : ((now[p] == "1") == (o.combos[i][p] != 0));
				if (same)
					return c;
				break;
			}
			default:
				break;
		}
	}
	return -1;
}

std::string optionText(const Option& o)
{
	if (o.kind == Option::Bool)
		return partNow(partsOf(o, true)[0], false) == "1" ? "On" : "Off";
	if (choiceKind(o))
	{
		const int c = choiceOf(o);
		if (c >= 0 && static_cast<size_t>(c) < o.names.size())
			return o.names[static_cast<size_t>(c)];
		const std::string v = partNow(partsOf(o, true)[0], false);
		return v.empty() ? std::string("None") : v;
	}
	const double v = std::strtod(partNow(partsOf(o, true)[0], false).c_str(), nullptr);
	char out[64];
	if (o.kind == Option::IntRange)
		std::snprintf(out, sizeof(out), "%d%s", static_cast<int>(std::lround(v * o.scale)), o.unit.c_str());
	else
		std::snprintf(out, sizeof(out), "%.*f%s", o.decimals, v * o.scale, o.unit.c_str());
	return out;
}

// Left and right on an option's row (Cross: on, going round): the game's own value, as its options
// set it; one that comes out as the settings' is taken away again (the game follows the settings).
void stepOption(const Option& o, int dir, bool round)
{
	if (!qm.ownOpen || (o.enabled && !o.enabled(Own)))
		return;
	const char* const s = o.section.c_str();
	const char* const k = o.key.c_str();
	if (o.kind == Option::Bool)
	{
		be->setBool(Own, s, k, partNow(partsOf(o, true)[0], false) != "1");
	}
	else if (choiceKind(o))
	{
		const int n = choices(o);
		if (n == 0)
			return;
		const int now = choiceOf(o);
		int next = (now < 0) ? (dir > 0 ? 0 : n - 1) : now + dir;
		next = round ? (next + n) % n : std::clamp(next, 0, n - 1);
		if (next == now)
			return;
		const size_t i = static_cast<size_t>(next);
		switch (o.kind)
		{
			case Option::IntList:
				be->setInt(Own, s, k, o.ints[i]);
				break;
			case Option::FloatList:
				be->setFloat(Own, s, k, o.floats[i]);
				break;
			case Option::StringList:
				be->setString(Own, s, k, o.strings[i]);
				break;
			default:
				for (size_t p = 0; p < o.parts.size(); p++)
				{
					if (o.parts[p].isInt)
						be->setInt(Own, o.parts[p].section.c_str(), o.parts[p].key.c_str(), o.combos[i][p]);
					else
						be->setBool(Own, o.parts[p].section.c_str(), o.parts[p].key.c_str(), o.combos[i][p] != 0);
				}
				break;
		}
	}
	else
	{
		const double v = std::strtod(partNow(partsOf(o, true)[0], false).c_str(), nullptr);
		double next = v + dir * o.step;
		if (round && next > o.hi + o.step / 2)
			next = o.lo;
		next = std::clamp(next, o.lo, o.hi);
		if (o.kind == Option::IntRange)
			be->setInt(Own, s, k, static_cast<int>(std::lround(next)));
		else
			be->setFloat(Own, s, k, static_cast<float>(next));
	}
	if (!o.gameDefault && optionNow(o, false) == optionNow(o, true))
	{
		for (const ValuePart& p : partsOf(o, true))
			be->erase(Own, p.section.c_str(), p.key.c_str());
	}
}

// The game's options the menu can show: switches, choices and ranges of its own pages that a game
// can have (not its patches, cards, network, controllers or BIOS), as the settings' level shows them.
bool quickOption(const Page& page, const Option& o)
{
	switch (o.kind)
	{
		case Option::Bool:
		case Option::IntRange:
		case Option::FloatRange:
			break;
		case Option::IntList:
		case Option::FloatList:
		case Option::StringList:
		case Option::Combo:
			if (choices(o) == 0)
				return false;
			break;
		default:
			return false;
	}
	if (o.label.empty() || (o.kind == Option::Combo ? o.parts.empty() : (o.section.empty() || o.key.empty())))
		return false;
	if (o.flags & (Option::AllGames | Option::Front | Option::NoGame | Option::Restart))
		return false;
	if (((o.flags & Option::Advanced) && !options().advanced) || ((o.flags & Option::Expert) && !options().expert))
		return false;
	static const char* const left[] = {"Patches", "Cheats", "Memory Cards", "Network & HDD", "Controllers", "BIOS"};
	for (const char* name : left)
	{
		if (page.name == name)
			return false;
	}
	return true;
}

std::string optionId(const Option& o)
{
	return "opt:" + ((o.kind == Option::Combo && o.key.empty()) ? "combo/" + o.label : o.section + "/" + o.key);
}

// (Made again when the game, the settings' level or the Backend's pages are other than they were.)
void makeOptions(const Running& running)
{
	const std::string key = runId(running) + "/" + std::to_string(options().level) + "/" + std::to_string(be->pagesGeneration());
	if (key == qm.optsFor)
		return;
	qm.optsFor = key;
	qm.opts.clear();
	for (const Page& page : settingsPagesNow(true))
	{
		for (const Option& o : page.rows)
		{
			if (!quickOption(page, o))
				continue;
			const std::string id = optionId(o);
			if (std::none_of(qm.opts.begin(), qm.opts.end(), [&id](const QuickOpt& q) { return q.id == id; }))
				qm.opts.push_back({o, id, page.name});
		}
	}
}

// ---- Apply changes now

std::string fingerprint()
{
	std::string out;
	for (const ValuePart& p : qm.nextStart)
		out += p.section + "/" + p.key + "=" + partNow(p, false) + "\n";
	return out;
}

// The settings a running game takes only at its start (Option::NextStart), of all games and of this
// one: each value once, the game's own where it can have one.
void collectNextStart()
{
	qm.nextStart.clear();
	const auto add = [](const Option& o, bool forGame) {
		if (!(o.flags & Option::NextStart))
			return;
		for (ValuePart& p : partsOf(o, forGame && !(o.flags & (Option::AllGames | Option::Front))))
		{
			const auto same = std::find_if(qm.nextStart.begin(), qm.nextStart.end(),
				[&p](const ValuePart& q) { return q.section == p.section && q.key == p.key; });
			if (same == qm.nextStart.end())
				qm.nextStart.push_back(std::move(p));
			else
				same->own = same->own || p.own;
		}
	};
	for (const Page& page : settingsPagesNow(false))
	{
		for (const Option& o : page.rows)
			add(o, false);
	}
	if (qm.ownOpen)
	{
		for (const Page& page : settingsPagesNow(true))
		{
			for (const Option& o : page.rows)
				add(o, true);
		}
	}
}

// As the menu comes up (opened, or back from the settings): the first time for this run of the game,
// what those settings are; after, whether they changed.
void checkApply(const Running& running)
{
	if (!running.canSaveState)
	{
		qm.applyPending = false;
		return;
	}
	const bool wasOpen = qm.ownOpen;
	openOwn(running);
	const std::string id = runId(running);
	if (qm.snapFor != id)
	{
		collectNextStart();
		qm.snapshot = fingerprint();
		qm.snapFor = id;
		qm.applyPending = false;
	}
	else
	{
		qm.applyPending = (fingerprint() != qm.snapshot);
	}
	if (!wasOpen)
		closeOwn();
}

void resumeGame()
{
	endEdit();
	closeOwn();
	be->setPaused(false);
	qm.open = 0;
	setScreen(Screen::None);
}

// A game is being put away (its state saved, its memory cards written): nothing can be done
// until it is, and the library that follows must find what it left.
void closingScreen()
{
	ImDrawList* dl = beginScreen("##bp-closing", true);
	in = Input{};
	const float cx = W / 2, cy = H / 2;
	const float turn = static_cast<float>(timeNow) * 3.2f;
	dl->PathClear();
	dl->PathArcTo(V(cx, cy - 60), 34 * S, turn, turn + IM_PI * 1.3f, 32);
	dl->PathStroke(col::accent, 5 * S);
	const char* title = qm.applying ? "Applying the changes" : "Closing the game";
	const ImVec2 ts = textSize(bold(), 40, title);
	text(dl, bold(), 40, cx - ts.x / 2, cy + 8, col::text, title);
	const char* note = qm.applying ? "Its state is saved, and it starts again from there" :
					   qm.closingSaves ? "Saving its state to go on from next time" : "Its memory cards are written first";
	const ImVec2 ns = textSize(regular(), 24, note);
	text(dl, regular(), 24, cx - ns.x / 2, cy + 70, col::dim, note);
	endScreen();
}

const StateInfo* slotState(int slot)
{
	for (const StateInfo& state : qm.states)
	{
		if (state.slot == slot && !state.backup)
			return &state;
	}
	return nullptr;
}

void refreshStates(const Running& running)
{
	const std::string id = running.serial + "/" + std::to_string(running.crc);
	if (id == qm.statesFor && timeNow < qm.statesAt + 1.5)
		return;
	qm.statesFor = id;
	qm.statesAt = timeNow;
	qm.states = running.canSaveState ? be->states(running.serial, running.crc, false) : std::vector<StateInfo>();
}

} // namespace

void setScreen(Screen s)
{
	if (s == screen)
		return;
	screen = s;
	// (The quick menu, as it comes up: whether a setting the game takes at its start was changed.)
	if (s == Screen::Quick)
		qm.entered = true;
	dropScreenDialogs();
	// The press that changed the screen is not the first press in the new one.
	holdInput();
}

void quickScreen()
{
	ImDrawList* dl = beginScreen("##bp-quick", false);
	const Running running = be->running();
	if (qm.open < 0.01f)
	{
		qm.focus = 0;
		qm.focusAnim = 0;
		qm.restartAsked = -10;
		qm.statesAt = -10;
		qm.focusId.clear();
		qm.first = 0;
		qm.edit = false; // (it opens as the menu, never in its editing)
		qm.zone = 0;
		qm.tile = 0;
		qm.pin = 0;
		qm.pinFirst = 0;
		qm.pinnedFor.clear();
		qm.patchesFor.clear();
	}
	qm.open = approach(qm.open, 1, 18);
	const float t = qm.open;
	refreshStates(running);

	// What the menu shows, and the game's own settings while it shows any of them (its options, or
	// pinned settings, which are the game's own where a game can have them).
	const std::vector<std::string> layout = layoutNow();
	const std::string pinsLine = be->getString(Shared, "PS5UI", "Pins").value_or(std::string());
	// (Again after the input, which may have closed them to run an entry that stays in the menu.)
	const auto prepare = [&] {
		if (qm.edit || !pinsLine.empty() || std::any_of(layout.begin(), layout.end(), quicklayout::isOption))
		{
			if (openOwn(running))
				makeOptions(running);
		}
		else
		{
			closeOwn();
		}
		// The pinned settings ([PS5UI] Pins), as the settings have them.
		const std::string key = runId(running) + (qm.ownOpen ? "/own/" : "/all/") + std::to_string(be->pagesGeneration()) + "/" + pinsLine;
		if (key != qm.pinnedFor)
		{
			qm.pinnedFor = key;
			qm.pinnedAt = frameNow;
			const GameInfo game = runningGame(running, false);
			qm.pinned = pinsLine.empty() ? std::vector<settings::PinnedRow>() : settings::pinnedRows(qm.ownOpen ? &game : nullptr);
		}
	};
	prepare();
	if (qm.entered)
	{
		qm.entered = false;
		checkApply(running);
		// (Made anew as the menu comes up; not twice in the frame it opens in.)
		if (qm.pinnedAt != frameNow)
			qm.pinnedFor.clear();
		qm.patchesFor.clear();
		prepare();
	}
	if (qm.ownOpen && qm.patchesFor != runId(running))
	{
		// How many of its patches and cheats are on, for More.
		qm.patchesFor = runId(running);
		qm.patchesOn = 0;
		const bool cheats = be->getBool(Own, "EmuCore", "EnableCheats").value_or(be->getBool(Shared, "EmuCore", "EnableCheats").value_or(false));
		for (int page = 0; page < 2; page++)
		{
			if (page == 1 && !cheats)
				continue;
			for (const Option& o : backendPageNow(page == 1 ? MadePage::Cheats : MadePage::Patches).rows)
			{
				if (o.kind == Option::Bool && (o.flags & Option::OneGame) && be->getBool(Own, o.section.c_str(), o.key.c_str()).value_or(false))
					qm.patchesOn++;
			}
		}
	}

	int& slot = options().stateSlot;
	slot = std::clamp(slot, 1, 10);
	const StateInfo* const saved = slotState(slot);
	const std::string slotName = "Slot " + std::to_string(slot);

	std::vector<QuickItem> items;
	items.push_back({ICON_FA_PLAY, "Resume", "Back to the game", true, [] { resumeGame(); }});
	items.back().id = "resume";
	{
		QuickItem item{ICON_FA_DOWNLOAD, "Save state", running.canSaveState ? slotName + (saved != nullptr ? "  \xc2\xb7  in use" : "  \xc2\xb7  empty") : std::string("Not for this game"),
			running.canSaveState, [] {
				be->saveState(options().stateSlot);
				qm.statesAt = -10;
				resumeGame();
			}};
		item.state = true;
		item.id = "save";
		items.push_back(std::move(item));
	}
	{
		QuickItem item{ICON_FA_CLOCK_ROTATE_LEFT, "Load state", saved == nullptr ? slotName + " is empty" : slotName + "  \xc2\xb7  " + formatWhen(saved->when),
			running.canSaveState && saved != nullptr, [] {
				if (const StateInfo* state = slotState(options().stateSlot))
				{
					be->loadStateFile(*state);
					resumeGame();
				}
			}};
		item.state = true;
		item.id = "load";
		items.push_back(std::move(item));
	}
	{
		// The game's picture, without this menu (the hotkey's own, Options + Square unless changed).
		Option hotkey;
		hotkey.kind = Option::Bind;
		hotkey.section = "Hotkeys";
		hotkey.key = "Screenshot";
		const std::string bound = be->binding(Shared, hotkey);
		QuickItem item{ICON_FA_CAMERA, "Screenshot", bound.empty() ? std::string("To the snaps folder") : bound, running.valid, [] {
						   if (!be->screenshot())
							   toast("Screenshots are not taken here.");
					   }};
		item.stays = true;
		item.id = "screenshot";
		items.push_back(std::move(item));
	}
	{
		QuickItem item{ICON_FA_LAYER_GROUP, "State slot", slotName, running.canSaveState, nullptr};
		item.side = [](int d) {
			options().stateSlot = (options().stateSlot - 1 + 10 + d) % 10 + 1;
			saveOptions();
		};
		item.run = [] {
			options().stateSlot = options().stateSlot % 10 + 1;
			saveOptions();
		};
		item.stays = true;
		item.state = true;
		item.id = "slot";
		items.push_back(std::move(item));
	}
	// (The entries below that are there only now and then are made all the same, for the menu's
	// editing: `applies` says whether they are shown.)
	{
		// The state the emulator saved by itself while the game was played ("Save a state by itself").
		const StateInfo* const self = slotState(-2);
		items.push_back({ICON_FA_CLOCK, "Load the auto-save", self != nullptr ? formatWhen(self->when) : std::string(), true, [] {
							 if (const StateInfo* state = slotState(-2))
							 {
								 be->loadStateFile(*state);
								 resumeGame();
							 }
						 }});
		items.back().id = "autosave";
		items.back().applies = (self != nullptr && running.canSaveState);
		items.back().state = true;
	}
	{
		// The game as it was just before a state was last loaded into it.
		items.push_back({ICON_FA_ROTATE_LEFT, "Undo the last load", "", running.canSaveState, [] {
							 be->undoLoad();
							 resumeGame();
						 }});
		items.back().id = "undo";
		items.back().applies = be->canUndoLoad();
	}
	{
		// The emulator's own lists, over the paused game.
		const int has = be->achievementsOf();
		QuickItem item{ICON_FA_TROPHY, "Achievements", "", true, [] { be->openAchievements(1); }};
		item.stays = true;
		item.id = "achievements";
		item.applies = (has & 1) != 0;
		items.push_back(std::move(item));
		QuickItem boards{ICON_FA_RANKING_STAR, "Leaderboards", "", true, [] { be->openAchievements(2); }};
		boards.stays = true;
		boards.id = "leaderboards";
		boards.applies = (has & 2) != 0;
		items.push_back(std::move(boards));
	}
	{
		// Another disc for the game: this one's place in its set, when the library has it as one.
		int disc = 0, discs = 0;
		const bool inSet = libraryDiscOf(running.path, disc, discs);
		QuickItem item{ICON_FA_COMPACT_DISC, "Change disc", inSet ? "Disc " + std::to_string(disc) + " of " + std::to_string(discs) : std::string("Another disc image"),
			true, [] { setScreen(Screen::Disc); }};
		item.stays = true;
		item.id = "disc";
		items.push_back(std::move(item));
	}
	{
		// For a graphics problem's report: the GS's input of the next seconds as the game plays on ([PS5] GsDumpSeconds,
		// 1 by default: a heavy scene gives about 10 MB a frame before compression), in a file that replays it (PCSX2's
		// GS dump), with whichever renderer draws.
		const int seconds = std::clamp(be->getInt(Shared, "PS5", "GsDumpSeconds").value_or(1), 1, 5);
		QuickItem item{ICON_FA_BUG, "Record a GS dump", std::to_string(seconds) + (seconds == 1 ? " second, to snaps/" : " seconds, to snaps/"), running.valid, [] {
						   be->recordGsDump();
						   resumeGame();
					   }};
		item.id = "gsdump";
		items.push_back(std::move(item));
	}
	{
		// (From OGBox 0.0.7) The log, kept in memory while nothing goes wrong, written out now: for something that
		// went wrong without Crux2 noticing.
		QuickItem item{ICON_FA_FILE_LINES, "Save the log", "to crux2-boot.log", true, [] {
						   bp::toast(be->saveLog() ? "The log was written to crux2-boot.log." : "The log is written to crux2-boot.log as it goes already.");
						   resumeGame();
					   }};
		item.id = "savelog";
		items.push_back(std::move(item));
	}
	{
		// This game's own settings: in effect at once (but those it takes at its start), and kept for
		// this game only.
		const GameInfo game = runningGame(running, false);
		QuickItem item{ICON_FA_SLIDERS, "Game Specific Settings", "Every setting, for this game", running.canSaveState, [game] { openSettings(&game, Screen::Quick); }};
		item.stays = true;
		item.id = "gameoptions";
		items.push_back(std::move(item));
		// Its patches and cheats, which are a page of those settings: straight to it.
		QuickItem patches{ICON_FA_BOLT, "Patches and cheats", qm.ownOpen ? (qm.patchesOn == 0 ? std::string("None on") : std::to_string(qm.patchesOn) + " on") : std::string(),
			running.canSaveState, [game] { openSettings(&game, Screen::Quick, "Patches"); }};
		patches.stays = true;
		patches.id = "patches";
		items.push_back(std::move(patches));
	}
	{
		QuickItem item{ICON_FA_WRENCH, "Settings", "Of every game", true, [] { openSettings(nullptr, Screen::Quick); }};
		item.stays = true;
		item.id = "settings";
		items.push_back(std::move(item));
	}
	{
		const bool limit = running.frameLimit;
		QuickItem item{ICON_FA_FORWARD_FAST, "Fast forward", limit ? "Off" : "On", true, [limit] {
						   be->setFrameLimit(!limit);
						   resumeGame();
					   }};
		item.id = "fastforward";
		items.push_back(std::move(item));
	}
	{
		// The console's reset button: the game starts again from its beginning. Asked twice, as
		// what was not saved is lost.
		const bool asked = timeNow < qm.restartAsked + 4;
		QuickItem item{ICON_FA_POWER_OFF, asked ? "Restart: press again" : "Restart game", "", true, [asked] {
						   if (!asked)
						   {
							   qm.restartAsked = timeNow;
							   return;
						   }
						   qm.restartAsked = -10;
						   if (be->memoryCardBusy())
						   {
							   toast("The game is writing to a memory card: not now.");
							   return;
						   }
						   be->reset();
						   resumeGame();
					   }};
		item.stays = !asked;
		item.id = "restart";
		items.push_back(std::move(item));
	}
	{
		const bool canSave = running.canSaveState;
		QuickItem item{ICON_FA_RIGHT_FROM_BRACKET, "Quit game", canSave ? "Can save where you are" : "", true, [canSave] {
						   const auto ask = [canSave] {
							   if (!canSave)
							   {
								   qm.closingSaves = false;
								   be->closeGame(false);
								   return;
							   }
							   pick("Quit the game", {"Quit, and save its state to go on from next time", "Quit without saving a state"}, -1,
								   [](int choice) {
									   qm.closingSaves = (choice == 0);
									   be->closeGame(choice == 0);
								   });
						   };
						   if (be->memoryCardBusy())
						   {
							   confirm("The game is writing to a memory card",
								   "Quitting now can spoil the memory card for good.\nLet the game finish saving first.",
								   [ask](bool yes) {
									   if (yes)
										   ask();
								   },
								   "Quit anyway", "Go back");
							   return;
						   }
						   ask();
					   }};
		item.stays = true;
		item.id = "quit";
		items.push_back(std::move(item));
	}

	// The game's own options the layout shows (all of them while it is edited): changed with left and
	// right, in effect at once (but those the game takes at its start).
	if (qm.ownOpen)
	{
		for (size_t i = 0; i < qm.opts.size(); i++)
		{
			const QuickOpt& q = qm.opts[i];
			if (!qm.edit && std::find(layout.begin(), layout.end(), q.id) == layout.end())
				continue;
			QuickItem item{ICON_FA_SLIDERS, q.o.label, optionText(q.o), !q.o.enabled || q.o.enabled(Own), nullptr};
			item.side = [i](int d) { stepOption(qm.opts[i].o, d, false); };
			item.run = [i] { stepOption(qm.opts[i].o, 1, true); };
			item.stays = true;
			item.id = q.id;
			item.option = true;
			item.page = q.page;
			items.push_back(std::move(item));
		}
	}
	// A setting the game takes at its start was changed while it runs: the game is closed with its
	// state saved, and started again from it (render(), once it is closed).
	QuickItem apply{ICON_FA_ROTATE, "Apply changes now", "Restarts the game from here", true, [running] {
						if (be->memoryCardBusy())
						{
							toast("The game is writing to a memory card: not now.");
							return;
						}
						qm.applyGame = runningGame(running, true);
						const StateInfo* const before = slotState(-1);
						qm.applyBefore = (before != nullptr) ? before->when : -1;
						qm.applyWanted = true;
						qm.applying = true;
						qm.closingSaves = true;
						be->closeGame(true);
					}};
	apply.stays = true;
	apply.id = "apply";

	// Every entry More can show, for the editing: the menu's own (not the tiles), and the game's options.
	const auto allIds = [&items] {
		std::vector<std::string> all;
		for (const QuickItem& item : items)
		{
			if (!item.option && !isTile(item.id))
				all.push_back(item.id);
		}
		for (const QuickOpt& q : qm.opts)
			all.push_back(q.id);
		return all;
	};

	// ---- the tiles, always there; More: the layout's entries that are there now, Apply changes now
	// above them; while the menu is edited, every entry More can show, shown or not
	std::vector<const QuickItem*> tiles;
	for (const char* id : tileIds)
	{
		for (const QuickItem& item : items)
		{
			if (item.id == id)
				tiles.push_back(&item);
		}
	}
	std::vector<const QuickItem*> shown;
	std::vector<QuickItem> unknown; // (an entry being edited that is not there any more: its name)
	// (Made again after the editing's input, which moves its rows.)
	const auto showEditing = [&] {
		shown.clear();
		unknown.clear();
		unknown.reserve(qm.editing.rows.size());
		for (const quicklayout::Edit::Row& row : qm.editing.rows)
		{
			const auto it = std::find_if(items.begin(), items.end(), [&row](const QuickItem& item) { return item.id == row.id; });
			if (it != items.end())
			{
				shown.push_back(&*it);
				continue;
			}
			unknown.push_back({ICON_FA_SLIDERS, row.id, "", false, nullptr});
			unknown.back().id = row.id;
			shown.push_back(&unknown.back());
		}
	};
	if (qm.edit)
		showEditing();
	else
	{
		if (qm.applyPending && running.canSaveState)
			shown.push_back(&apply);
		std::vector<std::string> have;
		for (const QuickItem& item : items)
			have.push_back(item.applies && !isTile(item.id) ? item.id : std::string());
		for (const int i : quicklayout::order(layout, have))
			shown.push_back(&items[static_cast<size_t>(i)]);
	}

	// ---- input
	int pinsCount = static_cast<int>(qm.pinned.size());
	const int tilesCount = static_cast<int>(tiles.size());
	int n = static_cast<int>(shown.size());
	if (!qm.edit && n > 0)
	{
		// (On the entry it was on, by name, when one comes or goes above it.)
		std::vector<std::string> ids;
		for (const QuickItem* item : shown)
			ids.push_back(item->id);
		qm.focus = quicklayout::keepFocus(ids, qm.focusId, qm.focus);
	}
	qm.focus = std::clamp(qm.focus, 0, std::max(0, n - 1));
	qm.tile = std::clamp(qm.tile, 0, std::max(0, tilesCount - 1));
	qm.pin = std::clamp(qm.pin, 0, std::max(0, pinsCount - 1));
	if (qm.zone == 1 && pinsCount == 0)
		qm.zone = 0;
	if (qm.zone == 2 && n == 0)
		qm.zone = 0;
	bool pinChanged = false;
	if (qm.edit)
	{
		// Cross shows or hides the entry, left and right (L1 and R1) move it, Square brings back the
		// menu as it comes; Triangle, Circle and the menu's own button end the editing, and the menu
		// with it (resumeGame keeps what was chosen).
		qm.zone = 2;
		if (in.up && qm.focus > 0)
			qm.focus--;
		if (in.down && qm.focus < n - 1)
			qm.focus++;
		if (in.triangle || in.back)
			resumeGame();
		else if (in.accept)
			qm.editing.toggle(qm.focus);
		else if (in.left || in.l1)
			qm.focus = qm.editing.move(qm.focus, -1);
		else if (in.right || in.r1)
			qm.focus = qm.editing.move(qm.focus, 1);
		else if (in.square)
		{
			qm.editing = quicklayout::Edit::of(quicklayout::defaults(), allIds());
			qm.focus = 0;
		}
	}
	else if (in.triangle)
	{
		// Choosing what More shows: the game's options are among what there is (none for a game that
		// cannot have options of its own).
		if (openOwn(running))
		{
			makeOptions(running);
		}
		else
		{
			qm.opts.clear();
			qm.optsFor.clear();
		}
		qm.editing = quicklayout::Edit::of(layout, allIds());
		qm.edit = true;
		qm.focus = 0;
		qm.zone = 2;
	}
	else if (in.back)
	{
		resumeGame();
	}
	else
	{
		const QuickItem* run = nullptr;
		switch (qm.zone)
		{
			case 0:
				if (in.left && qm.tile > 0)
					qm.tile--;
				if (in.right && qm.tile < tilesCount - 1)
					qm.tile++;
				if (in.down)
					qm.zone = (pinsCount > 0 && (qm.tile < tilesCount / 2 || n == 0)) ? 1 : (n > 0 ? 2 : 0);
				if (in.accept && tilesCount > 0 && tiles[static_cast<size_t>(qm.tile)]->enabled)
					run = tiles[static_cast<size_t>(qm.tile)];
				break;
			case 1:
			{
				settings::PinnedRow& row = qm.pinned[static_cast<size_t>(qm.pin)];
				if (in.up)
				{
					if (qm.pin > 0)
						qm.pin--;
					else
						qm.zone = 0;
				}
				if (in.down && qm.pin < pinsCount - 1)
					qm.pin++;
				if (in.left || in.right)
				{
					settings::pinnedChange(row, in.left ? -1 : 1, in.repeats);
					pinChanged = true;
				}
				if (in.accept)
				{
					// (An action may open the settings, which open the game's own themselves.)
					if (row.item.o.kind == Option::Action)
						closeOwn();
					settings::pinnedActivate(row);
					pinChanged = true;
				}
				// L3: the game's own value gone, the global setting used again (as in the settings).
				if (in.l3 && settings::pinnedHasOwn(row))
				{
					settings::pinnedUseGlobal(row);
					pinChanged = true;
				}
				if (in.r1 && n > 0)
					qm.zone = 2;
				break;
			}
			default:
			{
				const QuickItem& now = *shown[static_cast<size_t>(qm.focus)];
				if (in.up)
				{
					if (qm.focus > 0)
						qm.focus--;
					else
						qm.zone = 0;
				}
				if (in.down && qm.focus < n - 1)
					qm.focus++;
				if (now.side && now.enabled && (in.left || in.right))
					now.side(in.left ? -1 : 1);
				else if (in.left && pinsCount > 0)
					qm.zone = 1;
				if (in.l1 && pinsCount > 0)
					qm.zone = 1;
				if (in.accept && now.enabled && now.run)
					run = &now;
				// An option the game takes at its start, changed here: Apply changes now comes, or goes.
				if (now.option && (in.accept || in.left || in.right) && qm.snapFor == runId(running))
					qm.applyPending = running.canSaveState && fingerprint() != qm.snapshot;
				if (qm.zone == 2)
					qm.focusId = shown[static_cast<size_t>(qm.focus)]->id;
				break;
			}
		}
		// (The quick menu's own button closes it too: openQuickMenu(), when the button is let go.)
		if (run != nullptr)
		{
			// (What is run may open the settings, which open the game's own themselves.)
			if (!run->option)
				closeOwn();
			run->run();
		}
	}
	// A pinned setting the game takes at its start, changed here: Apply changes now comes, or goes.
	if (pinChanged)
		qm.patchesFor.clear(); // (a pinned patch or cheat may have been switched: counted again)
	if (pinChanged && qm.snapFor == runId(running))
		qm.applyPending = running.canSaveState && fingerprint() != qm.snapshot;
	if (screen != Screen::Quick)
	{
		endScreen();
		return;
	}
	if (qm.edit)
		showEditing();
	n = static_cast<int>(shown.size());
	qm.focus = std::clamp(qm.focus, 0, std::max(0, n - 1));
	prepare();
	pinsCount = static_cast<int>(qm.pinned.size());
	qm.pin = std::clamp(qm.pin, 0, std::max(0, pinsCount - 1));
	if (qm.zone == 1 && pinsCount == 0)
		qm.zone = 0;

	// ---- drawing: the paused game, veiled; at the foot, the game, the tiles, the pinned settings and
	// More, the hints
	const settings::Palette p = settings::palette();
	dl->AddRectFilled(V(0, 0), V(W, H), alpha(p.ground, 0.86f * t));
	const float slide = (motion() == MotionFull) ? (1 - t) * 60 : 0.f;
	const float m = 60, x0 = m, fullW = W - 2 * m;
	const int rowsShown = std::clamp(4 + static_cast<int>((H - 1080) / 66), 4, 8);
	const float rowH = 66, headH = 54;
	const float hintsCy = H - m - 18 + slide;
	const float panelH = headH + rowsShown * rowH + 14;
	const float panelY = hintsCy - 18 - 24 - panelH;
	const float tileH = 156, tileY = panelY - 24 - tileH;
	const float headY = tileY - 24 - 92;

	// The game: paused for how long, its name; what it runs with, on the right.
	{
		std::string paused = "PAUSED";
		if (qm.edit)
			paused = ICON_FA_PEN "  CHOOSING WHAT MORE SHOWS";
		else if (const std::string played = formatPlayed(running.sessionSeconds); !played.empty())
		{
			std::string upper = played;
			for (char& c : upper)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			paused += "  \xc2\xb7  " + upper + " THIS TIME";
		}
		text(dl, bold(), 19, x0, headY, alpha(qm.edit ? p.accent : col::good, t), paused.c_str());
		std::vector<std::string> facts;
		if (!running.hardware)
			facts.push_back("Software renderer");
		else
		{
			std::optional<float> scale = qm.ownOpen ? be->getFloat(Own, "EmuCore/GS", "upscale_multiplier") : std::nullopt;
			if (!scale)
				scale = be->getFloat(Shared, "EmuCore/GS", "upscale_multiplier");
			char upscale[32];
			std::snprintf(upscale, sizeof(upscale), "%gx resolution", static_cast<double>(scale.value_or(6.f)));
			facts.push_back(upscale);
		}
		if (!running.frameLimit)
			facts.push_back("Fast forward");
		if (!running.serial.empty())
			facts.push_back(running.serial);
		std::string line;
		for (const std::string& fct : facts)
			line += (line.empty() ? "" : "  \xc2\xb7  ") + fct;
		const float lw = std::min(textSize(regular(), 21, line.c_str()).x, fullW * 0.4f);
		textIn(dl, regular(), 21, x0 + fullW - lw, headY + 52, alpha(p.secondary, t), line, fullW * 0.4f, false);
		const std::string title = !running.title.empty() ? running.title : std::string("No disc");
		textIn(dl, bold(), 50, x0, headY + 32, alpha(p.text, t), title, fullW - lw - 48, true);
	}

	// The state in the slot, when what is in focus is of it: a card at the top right.
	const QuickItem* focusedItem = qm.zone == 0 ? (tilesCount > 0 ? tiles[static_cast<size_t>(qm.tile)] : nullptr) :
								   qm.zone == 2 && n > 0 ? shown[static_cast<size_t>(qm.focus)] : nullptr;
	if (focusedItem != nullptr && focusedItem->state && running.canSaveState && !qm.edit)
	{
		const StateInfo* const of = focusedItem->id == "autosave" ? slotState(-2) : saved;
		const float cw = 380, iw = cw - 32, ih = iw * 9 / 16, chh = ih + 32 + 52;
		const float cx = x0 + fullW - cw, cy = 40;
		if (cy + chh <= headY - 20)
		{
			settings::cardShape(dl, cx, cy, cw, chh, p.card, 16, 0.98f * t);
			rect(dl, cx + 16, cy + 16, iw, ih, alpha(p.raised, t), 10);
			if (of != nullptr)
			{
				const Texture picture = be->statePicture(*of);
				if (picture.valid())
					imageFill(dl, picture, cx + 16, cy + 16, iw, ih, 10, alpha(col::white, t));
			}
			else
			{
				const char* empty = ICON_FA_BOX_OPEN "   Empty slot";
				const ImVec2 es = textSize(regular(), 24, empty);
				text(dl, regular(), 24, cx + 16 + (iw - es.x) / 2, cy + 16 + (ih - es.y) / 2, alpha(p.faint, t), empty);
			}
			const std::string name = focusedItem->id == "autosave" ? std::string("Saved by itself") : slotName;
			text(dl, bold(), 22, cx + 20, cy + ih + 30, alpha(p.text, t), name.c_str());
			if (of != nullptr)
			{
				const std::string when = formatWhen(of->when);
				const ImVec2 ws = textSize(regular(), 20, when.c_str());
				text(dl, regular(), 20, cx + cw - 20 - ws.x, cy + ih + 32, alpha(p.secondary, t), when.c_str());
			}
		}
	}

	// The tiles.
	{
		const float gap = 18, tw = (fullW - gap * (tilesCount - 1)) / std::max(1, tilesCount);
		for (int i = 0; i < tilesCount; i++)
		{
			const QuickItem& it = *tiles[static_cast<size_t>(i)];
			const float x = x0 + i * (tw + gap);
			const bool on = qm.zone == 0 && i == qm.tile && !qm.edit;
			settings::cardShape(dl, x, tileY, tw, tileH, on ? p.accent : p.card, 16, t);
			if (on)
				settings::focusRing(dl, x, tileY, tw, tileH, 16);
			const ImU32 c = on ? p.onAccent : it.enabled ? p.text : p.faint;
			text(dl, regular(), 26, x + 22, tileY + 20, alpha(on ? p.onAccent : it.enabled ? p.accent : p.faint, t), it.icon);
			textIn(dl, bold(), 24, x + 22, tileY + 62, alpha(c, t), it.label, tw - 44, on);
			textIn(dl, regular(), 19, x + 22, tileY + tileH - 42, alpha(on ? p.onAccent : it.enabled ? p.secondary : p.faint, t), it.detail, tw - 44, false);
		}
	}

	// The two panels: the pinned settings, and More.
	const float pw = (fullW - 18) / 2;
	const auto panel = [&](float x, const char* title, const char* aside) {
		settings::cardShape(dl, x, panelY, pw, panelH, p.card, 16, 0.98f * t);
		text(dl, bold(), 17, x + 26, panelY + 20, alpha(p.secondary, t), title);
		if (aside != nullptr && aside[0] != '\0')
		{
			const float aw = std::min(textSize(regular(), 17, aside).x, pw * 0.55f);
			textIn(dl, regular(), 17, x + pw - 26 - aw, panelY + 20, alpha(p.faint, t), aside, pw * 0.55f, false);
		}
	};
	// (A list with more rows than there is room for moves with the focus.)
	const auto firstShown = [&](int& first, int focus, int count) {
		if (focus < first)
			first = focus;
		if (focus >= first + rowsShown)
			first = focus - rowsShown + 1;
		first = std::clamp(first, 0, std::max(0, count - rowsShown));
	};
	{
		const float x = x0;
		if (qm.edit)
		{
			panel(x, "HOW IT IS CHOSEN", nullptr);
			text(dl, regular(), 21, x + 26, panelY + headH + 10, alpha(p.secondary, t),
				"Cross shows an entry in More or takes it out; left and right move it up and down; Square brings back More as it comes. "
				"Resume, the states, the screenshot, the disc and Quit are always there, as tiles; any of this game's own settings can be "
				"added (they are changed with left and right).",
				pw - 52);
		}
		else
		{
			std::string aside;
			if (qm.zone == 1 && pinsCount > 0)
			{
				const Option& o = qm.pinned[static_cast<size_t>(qm.pin)].item.o;
				aside = std::string(qm.pinned[static_cast<size_t>(qm.pin)].forGame && !(o.flags & Option::Front) ? "This game" : "All games") +
						"  \xc2\xb7  " + settings::takesEffect(o);
			}
			else if (std::any_of(qm.pinned.begin(), qm.pinned.end(), [](const settings::PinnedRow& row) { return row.forGame; }))
				aside = "Changes apply to this game";
			panel(x, "YOUR PINNED SETTINGS", aside.c_str());
			if (pinsCount == 0)
			{
				text(dl, regular(), 21, x + 26, panelY + headH + 10, alpha(p.secondary, t),
					"Nothing is pinned yet. In the Settings, Square on a setting pins it: it is then here too, to change without leaving the "
					"game (this game's own value, where a game can have one).",
					pw - 52);
			}
			firstShown(qm.pinFirst, qm.pin, pinsCount);
			for (int i = qm.pinFirst; i < pinsCount && i < qm.pinFirst + rowsShown; i++)
			{
				const settings::PinnedRow& row = qm.pinned[static_cast<size_t>(i)];
				const float ry = panelY + headH + (i - qm.pinFirst) * rowH;
				const bool on = qm.zone == 1 && i == qm.pin;
				if (on)
				{
					rect(dl, x + 10, ry, pw - 20, rowH - 6, alpha(p.raised, t), 12);
					settings::focusRing(dl, x + 10, ry, pw - 20, rowH - 6, 12);
				}
				const bool can = settings::pinnedUsable(row);
				const std::string v = row.item.o.kind == Option::Action ? (row.item.o.info ? row.item.o.info() : std::string()) + "  " ICON_FA_CHEVRON_RIGHT : settings::pinnedValue(row);
				const float room = pw * 0.42f;
				const float vw = std::min(textSize(bold(), 23, v.c_str()).x, room);
				const float right = x + pw - 26 - (on && can ? 26 : 0);
				const float cyRow = ry + (rowH - 6) / 2;
				textIn(dl, bold(), 23, right - vw, cyRow - 14, alpha(!can ? p.faint : settings::pinnedChanged(row) ? p.accent : p.text, t), v, room, on);
				if (on && can && row.item.o.kind != Option::Action)
					drawArrows(dl, right - vw, right, cyRow, alpha(p.accent, t));
				textIn(dl, bold(), 23, x + 26, cyRow - 14, alpha(can ? p.text : p.faint, t), row.item.o.label, right - vw - 40 - (x + 26), on);
			}
		}
	}
	{
		const float x = x0 + pw + 18;
		panel(x, qm.edit ? "MORE: SHOWN OR NOT" : "MORE", nullptr);
		firstShown(qm.first, qm.focus, n);
		for (int i = qm.first; i < n && i < qm.first + rowsShown; i++)
		{
			const QuickItem& it = *shown[static_cast<size_t>(i)];
			const float ry = panelY + headH + (i - qm.first) * rowH;
			const bool on = qm.zone == 2 && i == qm.focus;
			const float cyRow = ry + (rowH - 6) / 2;
			if (on)
			{
				rect(dl, x + 10, ry, pw - 20, rowH - 6, alpha(p.raised, t), 12);
				settings::focusRing(dl, x + 10, ry, pw - 20, rowH - 6, 12);
			}
			// (While the menu is edited, an entry that cannot be chosen now is not shown fainter.)
			const bool can = it.enabled || qm.edit;
			const ImU32 c = !can ? p.faint : (&it == &apply) ? p.accent : p.text;
			text(dl, regular(), 22, x + 26, cyRow - 13, alpha(can ? p.accent : p.faint, t), it.icon);
			std::string detail = it.detail;
			float right = x + pw - 26;
			if (qm.edit)
			{
				const quicklayout::Edit::Row& row = qm.editing.rows[static_cast<size_t>(i)];
				detail = it.page;
				drawSwitch(dl, right, cyRow, row.shown, on, 0.85f);
				right -= 58 + 16;
			}
			float labelRoom = right - (x + 70) - 16;
			if (!detail.empty())
			{
				const bool arrows = it.side && on && it.enabled && !qm.edit;
				const float room = pw * 0.4f;
				const float dw = std::min(textSize(regular(), 21, detail.c_str()).x, room);
				const float dx = right - dw - (arrows ? 26 : 0);
				textIn(dl, regular(), 21, dx, cyRow - 13, alpha(on && can ? p.text : p.secondary, t), detail, room, false);
				if (arrows)
					drawArrows(dl, dx, dx + dw, cyRow, alpha(p.accent, t));
				labelRoom = dx - (arrows ? 30 : 16) - (x + 70);
			}
			textIn(dl, bold(), 23, x + 70, cyRow - 14, alpha(c, t), it.label, std::max(40.f, labelRoom), on);
		}
		if (n == 0 && !qm.edit)
			text(dl, regular(), 21, x + 26, panelY + headH + 10, alpha(p.secondary, t), "Nothing else is chosen to be here: Triangle chooses.", pw - 52);
	}

	// The hints, at the foot on the right.
	{
		std::vector<Hint> hints;
		if (qm.edit)
			hints = {{Glyph::Cross, "Show"}, {Glyph::DPadLR, "Move"}, {Glyph::Square, "As it comes"}, {Glyph::Triangle, "Done"}};
		else
		{
			if (qm.zone == 1)
			{
				hints.push_back({Glyph::DPadLR, "Change"});
				if (pinsCount > 0 && qm.pinned[static_cast<size_t>(qm.pin)].item.o.kind == Option::Action)
					hints.back() = {Glyph::Cross, "Run"};
				if (pinsCount > 0 && settings::pinnedHasOwn(qm.pinned[static_cast<size_t>(qm.pin)]))
					hints.push_back({Glyph::L3, "Use global setting"});
				if (n > 0)
					hints.push_back({Glyph::R1, "More"});
			}
			else
			{
				hints.push_back({Glyph::Cross, "Choose"});
				const QuickItem* now = qm.zone == 2 && n > 0 ? shown[static_cast<size_t>(qm.focus)] : nullptr;
				if (now != nullptr && now->side)
					hints.push_back({Glyph::DPadLR, now->option ? "Change" : "Slot"});
				if (qm.zone == 2 && pinsCount > 0)
					hints.push_back({Glyph::L1, "Pinned"});
			}
			hints.push_back({Glyph::Triangle, "Edit this menu"});
			hints.push_back({Glyph::Circle, "Resume"});
		}
		orderHints(hints);
		const Layer words = beginLayer(dl);
		float x = x0 + fullW;
		for (auto it = hints.rbegin(); it != hints.rend(); ++it)
		{
			const ImVec2 ts = textSize(regular(), 22, it->label);
			x -= ts.x;
			text(dl, regular(), 22, x, hintsCy - ts.y / 2, alpha(p.text, t), it->label);
			x -= 10;
			x -= glyph(dl, it->glyph, 0, -1000, 30);
			glyph(dl, it->glyph, x, hintsCy, 30);
			x -= 34;
		}
		endLayer(words, steadyAlpha());
	}
	endScreen();
}

bool quickFocus(const std::string& id)
{
	for (int i = 0; i < static_cast<int>(std::size(tileIds)); i++)
	{
		if (id == tileIds[i])
		{
			qm.zone = 0;
			qm.tile = i;
			return true;
		}
	}
	if (id.rfind("pin:", 0) == 0)
	{
		qm.zone = 1;
		qm.pin = std::atoi(id.c_str() + 4);
		return qm.pin >= 0 && qm.pin < static_cast<int>(qm.pinned.size());
	}
	qm.zone = 2;
	qm.focusId = id;
	return true;
}

// ------------------------------------------------------------- which screen

void initialize(Backend* backend)
{
	be = backend;
	loadOptions();
	screen = Screen::Library;
	splash = SplashState();
}

void shutdown()
{
	be = nullptr;
}

bool active()
{
	return be != nullptr && (screen != Screen::None || dialogOpen());
}

void render()
{
	if (be == nullptr)
		return;
	tickClock();
	be->frame();
	pollDisc();
	careTick(be->pad().held);

	// The emulator's own list of achievements is up, over the paused game: it has the screen and
	// the controller. (Nothing is read until every button was let go after it closes: the button
	// that closed it is not the quick menu's.)
	if (be->overlayOpen())
	{
		holdInput();
		return;
	}

	const Running running = be->running();
	const bool starting = be->starting();

	// Apply changes now: the game, closed with its state saved, is started again from it.
	if (qm.applyWanted && !running.valid && !running.stopping && !starting)
	{
		qm.applyWanted = false;
		qm.applying = false;
		// (Only from the state just saved: one left from an earlier time is not where the game was.)
		bool saved = false;
		for (const StateInfo& state : be->states(qm.applyGame.serial, qm.applyGame.crc, false))
			saved = saved || (state.slot == -1 && !state.backup && state.when != qm.applyBefore);
		if (saved)
		{
			be->log("quick menu: settings the game takes at its start were changed: it starts again from its state");
			launchGame(qm.applyGame, -1, 0);
			return;
		}
		be->log("quick menu: the state to apply the changes with was not saved: the game is not started again");
		message("The changes were not applied", "The game's state could not be saved, so it was not started again. The changes take effect the next time the game is started.");
	}
	// (The game's own settings are open only while the quick menu is up.)
	if (screen != Screen::Quick && qm.ownOpen)
		closeOwn();

	// Where the Options button went down, for openQuickMenu().
	const bool optionsDown = (be->pad().held & Pad::Options) != 0;
	if (optionsDown && !qm.optionsWasDown)
		qm.optionsPressedIn = screen;
	qm.optionsWasDown = optionsDown;

	if (running.stopping)
	{
		// (What was being asked about the game, to quit it or not, is answered.)
		dropScreenDialogs();
		closingScreen();
		return;
	}
	if (screen == Screen::Loading)
	{
		if (!starting)
			setScreen(running.valid ? Screen::None : Screen::Library);
	}
	else if (starting && !running.valid)
	{
		// (Started by something other than the library: a test run.)
		setScreen(Screen::Loading);
	}
	else if (!running.valid && (screen == Screen::None || screen == Screen::Quick || screen == Screen::Disc))
	{
		setScreen(Screen::Library);
	}

	switch (screen)
	{
		case Screen::Library:
			librarySplashOrScreen(false);
			break;
		case Screen::Disc:
			libraryScreen(true);
			break;
		case Screen::Settings:
			settingsScreen();
			break;
		case Screen::Quick:
			quickScreen();
			break;
		case Screen::Loading:
			loadingScreen();
			break;
		case Screen::None:
		default:
			drawOverGame();
			break;
	}
}

void gameStarted()
{
	if (be == nullptr)
		return;
	qm.open = 0;
	// (This run of the game: what its settings are is looked at anew when the menu first comes up.)
	qm.snapFor.clear();
	qm.applyPending = false;
	qm.applying = false;
	setScreen(Screen::None);
}

void gameStopped()
{
	if (be == nullptr)
		return;
	endEdit();
	closeOwn();
	qm.open = 0;
	qm.statesFor.clear();
	qm.snapFor.clear();
	qm.applyPending = false;
	// (The settings, if they are up, stay: leaving them finds the library.)
	if (!be->starting() && screen != Screen::Settings)
		setScreen(Screen::Library);
}

void gameResumed()
{
	if (be == nullptr)
		return;
	// A game runs only while no screen is over it: resumed under one (by a button for it let go
	// in the interface, or by a resume of ours that arrives after the menu was opened again), it
	// is held still again.
	const Running running = be->running();
	if ((screen == Screen::Quick || screen == Screen::Disc || screen == Screen::Settings) && running.valid && !running.paused)
		be->setPaused(true);
}

void openQuickMenu()
{
	if (be == nullptr || !be->running().valid)
		return;
	if (screen == Screen::Quick)
	{
		// Its own button, pressed and let go in the menu, closes it.
		// (Not when that press only ended the screen saver.)
		if (qm.optionsPressedIn == Screen::Quick && !dialogOpen() && !careWokeLately())
			resumeGame();
		return;
	}
	if (screen != Screen::None || dialogOpen())
		return;
	const Running running = be->running();
	if (!running.valid)
		return;
	qm.open = 0;
	be->setPaused(true);
	setScreen(Screen::Quick);
}

void startFailed(const std::string& why)
{
	if (be == nullptr)
		return;
	if (!be->running().valid)
		setScreen(Screen::Library);
	message("The game could not be started", why.empty() ? std::string("crux2-boot.log in the title folder has the reason.") : why);
}

} // namespace bp
