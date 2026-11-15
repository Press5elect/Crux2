// Crux2 - the interface: the quick menu over a paused game, and which screen is on.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0.
//
// The quick menu slides in from the right over the paused game: resume, the saved states (with
// the picture each was saved with), another disc, this game's own options, the settings, and
// the ways out.
//
// Triangle in the menu chooses what it shows and in which order (bp_quick_layout.h): its own
// entries, and any of the game's own options, changed there with left and right. A setting that a
// running game takes only when it starts (Option::NextStart), changed while it runs, puts "Apply
// changes now" at the top: the game is closed with its state saved and started again from it.

#include "bp_internal.h"
#include "bp_quick_layout.h"

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
} qm;

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
	keepLayout(qm.editing.layout());
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
		qm.ownOpen = be->openGame(runningGame(running, false));
	return qm.ownOpen;
}

// (Before anything that may open the settings, which open the game's own themselves.)
void closeOwn()
{
	if (!qm.ownOpen)
		return;
	qm.ownOpen = false;
	be->closeGame();
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
	for (const Page& page : settingsPages(true))
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
	for (const Page& page : settingsPages(false))
	{
		for (const Option& o : page.rows)
			add(o, false);
	}
	if (qm.ownOpen)
	{
		for (const Page& page : settingsPages(true))
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
	}
	qm.open = approach(qm.open, 1, 14);
	const float t = qm.open;
	refreshStates(running);

	// What the menu shows, and the game's own settings while it shows any of them.
	const std::vector<std::string> layout = layoutNow();
	if (qm.edit || std::any_of(layout.begin(), layout.end(), quicklayout::isOption))
	{
		if (openOwn(running))
			makeOptions(running);
	}
	else
	{
		closeOwn();
	}
	if (qm.entered)
	{
		qm.entered = false;
		checkApply(running);
	}

	int& slot = options().stateSlot;
	slot = std::clamp(slot, 1, 10);
	const StateInfo* const saved = slotState(slot);
	const std::string slotName = "Slot " + std::to_string(slot);

	std::vector<QuickItem> items;
	items.push_back({ICON_FA_PLAY, "Resume", "", true, [] { resumeGame(); }});
	items.back().id = "resume";
	{
		QuickItem item{ICON_FA_DOWNLOAD, "Save state", slotName, running.canSaveState, [] {
						   be->saveState(options().stateSlot);
						   qm.statesAt = -10;
						   resumeGame();
					   }};
		item.state = true;
		item.id = "save";
		items.push_back(std::move(item));
	}
	{
		QuickItem item{ICON_FA_CLOCK_ROTATE_LEFT, "Load state", saved == nullptr ? "Empty slot" : slotName, running.canSaveState && saved != nullptr,
			[] {
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
		QuickItem item{ICON_FA_COMPACT_DISC, "Change disc", "", true, [] { setScreen(Screen::Disc); }};
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
		// This game's own options: in effect at once (but those it takes at its start), and kept for
		// this game only.
		const GameInfo game = runningGame(running, false);
		QuickItem item{ICON_FA_SLIDERS, "Game options", "", running.canSaveState, [game] { openSettings(&game, Screen::Quick); }};
		item.stays = true;
		item.id = "gameoptions";
		items.push_back(std::move(item));
		// Its patches and cheats, which are a page of those options: straight to it.
		QuickItem patches{ICON_FA_BOLT, "Patches and cheats", "", running.canSaveState,
			[game] { openSettings(&game, Screen::Quick, "Patches"); }};
		patches.stays = true;
		patches.id = "patches";
		items.push_back(std::move(patches));
	}
	{
		QuickItem item{ICON_FA_WRENCH, "Settings", "", true, [] { openSettings(nullptr, Screen::Quick); }};
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
		QuickItem item{ICON_FA_RIGHT_FROM_BRACKET, "Quit game", "", true, [canSave] {
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
	QuickItem apply{ICON_FA_ROTATE, "Apply changes now", "", true, [running] {
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

	// Every entry there can be, for the editing: the menu's own, and the game's options.
	const auto allIds = [&items] {
		std::vector<std::string> all;
		for (const QuickItem& item : items)
		{
			if (!item.option)
				all.push_back(item.id);
		}
		for (const QuickOpt& q : qm.opts)
			all.push_back(q.id);
		return all;
	};

	// ---- what is shown: the layout's entries that are there now, Apply changes now above them; while
	// the menu is edited, every entry there can be, shown or not
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
			have.push_back(item.applies ? item.id : std::string());
		for (const int i : quicklayout::order(layout, have))
			shown.push_back(&items[static_cast<size_t>(i)]);
	}

	// ---- input
	const int n = static_cast<int>(shown.size());
	if (n == 0)
	{
		endScreen();
		return;
	}
	if (!qm.edit)
	{
		// (On the entry it was on, by name, when one comes or goes above it.)
		std::vector<std::string> ids;
		for (const QuickItem* item : shown)
			ids.push_back(item->id);
		qm.focus = quicklayout::keepFocus(ids, qm.focusId, qm.focus);
	}
	qm.focus = std::clamp(qm.focus, 0, n - 1);
	if (in.up)
		qm.focus = (qm.focus + n - 1) % n;
	if (in.down)
		qm.focus = (qm.focus + 1) % n;
	const QuickItem& now = *shown[static_cast<size_t>(qm.focus)];
	if (qm.edit)
	{
		// Cross shows or hides the entry, left and right (L1 and R1) move it, Square brings back the
		// menu as it comes; Triangle, Circle and the menu's own button end the editing, and the menu
		// with it (resumeGame keeps what was chosen).
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
		// Choosing what the menu shows: the game's options are among what there is (none for a game
		// that cannot have options of its own).
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
	}
	else
	{
		qm.focusId = now.id;
		if (now.side && now.enabled && (in.left || in.right))
			now.side(in.left ? -1 : 1);
		// (The quick menu's own button closes it too: openQuickMenu(), when the button is let go.)
		if (in.accept && now.enabled && now.run)
		{
			// (What is run may open the settings, which open the game's own themselves.)
			if (!now.option)
				closeOwn();
			now.run();
		}
		else if (in.back)
		{
			resumeGame();
		}
		// An option the game takes at its start, changed here: Apply changes now comes, or goes.
		if (now.option && (in.accept || in.left || in.right) && qm.snapFor == runId(running))
			qm.applyPending = running.canSaveState && fingerprint() != qm.snapshot;
	}
	if (screen != Screen::Quick)
	{
		endScreen();
		return;
	}
	if (qm.edit)
		showEditing();
	if (shown.empty())
	{
		endScreen();
		return;
	}
	const int count = static_cast<int>(shown.size());
	qm.focus = std::clamp(qm.focus, 0, count - 1);
	const QuickItem& cur = *shown[static_cast<size_t>(qm.focus)];

	// ---- the paused game, dimmed
	dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, static_cast<int>(140 * t)));

	// ---- the panel, sliding in from the right
	const float pw = 640;
	const float px = W - pw * (0.15f + 0.85f * t);
	gradientH(dl, px - 120, 0, 120, H, col::rgba(0, 0, 0, 0), col::rgba(0, 0, 0, static_cast<int>(120 * t)));
	rect(dl, px, 0, pw, H, alpha(col::sheet, 0.97f * t));
	rect(dl, px, 0, 1, H, col::line);

	// The game.
	{
		GameInfo game;
		if (const GameInfo* listed = libraryGame(running.path))
			game = *listed;
		else
		{
			game.path = running.path;
			game.title = running.title;
		}
		const float gx = px + 48;
		drawGameCover(dl, game, gx, 44, 120, 170, 10, 0.5f);
		const std::string title = !running.title.empty() ? running.title : std::string("No disc");
		const float titleH = textIn(dl, bold(), 32, gx + 148, 52, col::text, title, pw - 48 - 148 - 40, true).y;
		float y = 52 + titleH + 12;
		if (!running.serial.empty())
		{
			text(dl, regular(), 22, gx + 148, y, col::dim, running.serial.c_str());
			y += 32;
		}
		const std::string played = formatPlayed(running.sessionSeconds);
		if (!played.empty())
		{
			text(dl, regular(), 22, gx + 148, y, col::faint, ("This time: " + played).c_str());
			y += 32;
		}
		if (qm.edit)
			text(dl, regular(), 22, gx + 148, y, col::accent, ICON_FA_PEN "  Choosing what the menu shows");
		else
			text(dl, regular(), 22, gx + 148, y, col::good, ICON_FA_PAUSE "  Paused");
	}

	// The items.
	const float itemsTop = 244;
	// (As high as there is room for: every item is on the screen, however many a game has, down to
	// rows 38 high (56 while the menu is edited); with more, the rows move with the cursor.)
	const float room = H - itemsTop - 100;
	const float ih = std::clamp(room / static_cast<float>(count), qm.edit ? 56.f : 38.f, 78.f);
	const int fits = std::max(1, static_cast<int>(room / ih));
	if (qm.focus < qm.first)
		qm.first = qm.focus;
	if (qm.focus >= qm.first + fits)
		qm.first = qm.focus - fits + 1;
	qm.first = std::clamp(qm.first, 0, std::max(0, count - fits));
	qm.focusAnim = approach(qm.focusAnim, static_cast<float>(qm.focus), 22);
	const float lit = std::clamp(qm.focusAnim - static_cast<float>(qm.first), 0.f, static_cast<float>(fits - 1));
	rect(dl, px + 32, itemsTop + lit * ih, pw - 64, ih - 8, col::accent, 12);
	for (int i = qm.first; i < count && i < qm.first + fits; i++)
	{
		const QuickItem& it = *shown[static_cast<size_t>(i)];
		// The offsets below are for a row 78 high: a lower row is moved up by half the difference.
		const float y = itemsTop + (i - qm.first) * ih + (ih - 78) / 2;
		const bool on = (i == qm.focus);
		// (While the menu is edited, an entry that cannot be chosen now is not shown fainter.)
		const ImU32 c = (!it.enabled && !qm.edit) ? col::faint : on ? col::text : col::dim;
		text(dl, regular(), 28, px + 64, y + 20, c, it.icon);
		// While the menu is edited: whether the entry is shown (Resume and Quit always are), and of
		// an option, its page of the settings.
		std::string detail = it.detail;
		float right = px + pw - 64;
		if (qm.edit)
		{
			const quicklayout::Edit::Row& row = qm.editing.rows[static_cast<size_t>(i)];
			detail = quicklayout::fixed(row.id) ? std::string("Always shown") : it.page;
			if (!quicklayout::fixed(row.id))
			{
				drawSwitch(dl, right, y + 39, row.shown, on);
				right -= 68 + 20;
			}
		}
		float labelRoom = right - (px + 120) - 16;
		if (!detail.empty())
		{
			const ImVec2 rs = textSize(regular(), 24, detail.c_str());
			const float rx = right - rs.x;
			labelRoom = rx - (it.side && on && it.enabled && !qm.edit ? 34 : 16) - (px + 120);
			if (it.side && on && it.enabled && !qm.edit)
			{
				dl->AddTriangleFilled(V(rx - 26, y + 35), V(rx - 14, y + 25), V(rx - 14, y + 45), col::text);
				dl->AddTriangleFilled(V(px + pw - 44, y + 35), V(px + pw - 56, y + 25), V(px + pw - 56, y + 45), col::text);
			}
			text(dl, regular(), 24, rx, y + 23, (on && (it.enabled || qm.edit)) ? col::text : col::faint, detail.c_str());
		}
		// (An option's label, and any while the menu is edited, is cut short where the row has no more
		// room; the menu's own entries are drawn as they always were.)
		const std::string label = (it.option || qm.edit) ? fit(bold(), 28, it.label, std::max(40.f, labelRoom)) : it.label;
		text(dl, bold(), 28, px + 120, y + 19, c, label.c_str());
	}

	// The state in the slot: a card beside the panel while a state row is focused.
	if (cur.state && running.canSaveState && !qm.edit)
	{
		const float cw = 432, chh = 320;
		const float cx = px - 32 - cw;
		const float rowY = itemsTop + lit * ih + (ih - 8) / 2;
		const float cy = std::clamp(rowY - chh / 2, 40.f, H - chh - 40);
		glow(dl, cx, cy, cw, chh, 18, col::rgba(0, 0, 0, 200), 24);
		rect(dl, cx, cy, cw, chh, alpha(col::sheetHi, 0.98f), 18);
		const float iw = cw - 32, ihh = iw * 9 / 16;
		rect(dl, cx + 16, cy + 16, iw, ihh, col::card, 12);
		if (saved != nullptr)
		{
			const Texture picture = be->statePicture(*saved);
			if (picture.valid())
				imageFill(dl, picture, cx + 16, cy + 16, iw, ihh, 12);
		}
		else
		{
			const char* empty = ICON_FA_BOX_OPEN "   Empty slot";
			const ImVec2 es = textSize(regular(), 26, empty);
			text(dl, regular(), 26, cx + 16 + (iw - es.x) / 2, cy + 16 + (ihh - es.y) / 2, col::faint, empty);
		}
		text(dl, bold(), 26, cx + 20, cy + ihh + 32, col::text, slotName.c_str());
		if (saved != nullptr)
		{
			const std::string when = formatWhen(saved->when);
			const ImVec2 ws = textSize(regular(), 22, when.c_str());
			text(dl, regular(), 22, cx + cw - 20 - ws.x, cy + ihh + 36, col::dim, when.c_str());
		}
	}

	// Hints, in the panel.
	{
		float x = px + 48;
		const float y = H - 52;
		const auto hint = [&](Glyph gl, const char* label) {
			x += glyph(dl, gl, x, y, 30) + 10;
			x += text(dl, regular(), 22, x, y - 13, col::text, label).x + 30;
		};
		if (qm.edit)
		{
			hint(Glyph::Cross, "Show");
			hint(Glyph::DPadLR, "Move");
			hint(Glyph::Square, "Default");
			hint(Glyph::Triangle, "Done");
		}
		else
		{
			if (cur.option)
				hint(Glyph::DPadLR, "Change");
			else
			{
				hint(Glyph::Cross, "Select");
				if (cur.side)
					hint(Glyph::DPadLR, "Slot");
			}
			hint(Glyph::Triangle, "Edit menu");
			hint(Glyph::Circle, "Resume");
		}
	}
	endScreen();
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
