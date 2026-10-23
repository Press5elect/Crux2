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

#include "bp_internal.h"

#include <cstdio>

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
} qm;

void resumeGame()
{
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
	const char* title = "Closing the game";
	const ImVec2 ts = textSize(bold(), 40, title);
	text(dl, bold(), 40, cx - ts.x / 2, cy + 8, col::text, title);
	const char* note = qm.closingSaves ? "Saving its state to go on from next time" : "Its memory cards are written first";
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
	}
	qm.open = approach(qm.open, 1, 14);
	const float t = qm.open;
	refreshStates(running);

	int& slot = options().stateSlot;
	slot = std::clamp(slot, 1, 10);
	const StateInfo* const saved = slotState(slot);
	const std::string slotName = "Slot " + std::to_string(slot);

	std::vector<QuickItem> items;
	items.push_back({ICON_FA_PLAY, "Resume", "", true, [] { resumeGame(); }});
	{
		QuickItem item{ICON_FA_DOWNLOAD, "Save state", slotName, running.canSaveState, [] {
						   be->saveState(options().stateSlot);
						   qm.statesAt = -10;
						   resumeGame();
					   }};
		item.state = true;
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
		items.push_back(std::move(item));
	}
	if (const StateInfo* const self = slotState(-2); self != nullptr && running.canSaveState)
	{
		// The state the emulator saved by itself while the game was played ("Save a state by itself").
		items.push_back({ICON_FA_CLOCK, "Load the auto-save", formatWhen(self->when), true, [] {
							 if (const StateInfo* state = slotState(-2))
							 {
								 be->loadStateFile(*state);
								 resumeGame();
							 }
						 }});
	}
	if (be->canUndoLoad())
	{
		// The game as it was just before a state was last loaded into it.
		items.push_back({ICON_FA_ROTATE_LEFT, "Undo the last load", "", running.canSaveState, [] {
							 be->undoLoad();
							 resumeGame();
						 }});
	}
	if (const int has = be->achievementsOf(); has != 0)
	{
		// The emulator's own lists, over the paused game.
		if (has & 1)
		{
			QuickItem item{ICON_FA_TROPHY, "Achievements", "", true, [] { be->openAchievements(1); }};
			item.stays = true;
			items.push_back(std::move(item));
		}
		if (has & 2)
		{
			QuickItem item{ICON_FA_RANKING_STAR, "Leaderboards", "", true, [] { be->openAchievements(2); }};
			item.stays = true;
			items.push_back(std::move(item));
		}
	}
	{
		QuickItem item{ICON_FA_COMPACT_DISC, "Change disc", "", true, [] { setScreen(Screen::Disc); }};
		item.stays = true;
		items.push_back(std::move(item));
	}
	{
		// This game's own options: in effect at once, and kept for this game only.
		const GameInfo* listed = libraryGame(running.path);
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
		QuickItem item{ICON_FA_SLIDERS, "Game options", "", running.canSaveState, [game] { openSettings(&game, Screen::Quick); }};
		item.stays = true;
		items.push_back(std::move(item));
		// Its patches and cheats, which are a page of those options: straight to it.
		QuickItem patches{ICON_FA_BOLT, "Patches and cheats", "", running.canSaveState,
			[game] { openSettings(&game, Screen::Quick, "Patches"); }};
		patches.stays = true;
		items.push_back(std::move(patches));
	}
	{
		QuickItem item{ICON_FA_WRENCH, "Settings", "", true, [] { openSettings(nullptr, Screen::Quick); }};
		item.stays = true;
		items.push_back(std::move(item));
	}
	{
		const bool limit = running.frameLimit;
		QuickItem item{ICON_FA_FORWARD_FAST, "Fast forward", limit ? "Off" : "On", true, [limit] {
						   be->setFrameLimit(!limit);
						   resumeGame();
					   }};
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
		items.push_back(std::move(item));
	}

	// ---- input
	const int n = static_cast<int>(items.size());
	qm.focus = std::clamp(qm.focus, 0, n - 1);
	if (in.up)
		qm.focus = (qm.focus + n - 1) % n;
	if (in.down)
		qm.focus = (qm.focus + 1) % n;
	const QuickItem& now = items[static_cast<size_t>(qm.focus)];
	if (now.side && now.enabled && (in.left || in.right))
		now.side(in.left ? -1 : 1);
	// (The quick menu's own button closes it too: openQuickMenu(), when the button is let go.)
	if (in.accept && now.enabled && now.run)
		now.run();
	else if (in.back)
		resumeGame();
	if (screen != Screen::Quick)
	{
		endScreen();
		return;
	}

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
		text(dl, regular(), 22, gx + 148, y, col::good, ICON_FA_PAUSE "  Paused");
	}

	// The items.
	const float itemsTop = 244;
	// (As high as there is room for: every item is on the screen, however many a game has.)
	const float ih = std::clamp((H - itemsTop - 100) / static_cast<float>(n), 38.f, 78.f);
	qm.focusAnim = approach(qm.focusAnim, static_cast<float>(qm.focus), 22);
	rect(dl, px + 32, itemsTop + qm.focusAnim * ih, pw - 64, ih - 8, col::accent, 12);
	for (int i = 0; i < n; i++)
	{
		const QuickItem& it = items[static_cast<size_t>(i)];
		// The offsets below are for a row 78 high: a lower row is moved up by half the difference.
		const float y = itemsTop + i * ih + (ih - 78) / 2;
		const bool on = (i == qm.focus);
		const ImU32 c = !it.enabled ? col::faint : on ? col::text : col::dim;
		text(dl, regular(), 28, px + 64, y + 20, c, it.icon);
		text(dl, bold(), 28, px + 120, y + 19, c, it.label.c_str());
		if (!it.detail.empty())
		{
			const ImVec2 rs = textSize(regular(), 24, it.detail.c_str());
			const float rx = px + pw - 64 - rs.x;
			if (it.side && on && it.enabled)
			{
				dl->AddTriangleFilled(V(rx - 26, y + 35), V(rx - 14, y + 25), V(rx - 14, y + 45), col::text);
				dl->AddTriangleFilled(V(px + pw - 44, y + 35), V(px + pw - 56, y + 25), V(px + pw - 56, y + 45), col::text);
			}
			text(dl, regular(), 24, rx, y + 23, (on && it.enabled) ? col::text : col::faint, it.detail.c_str());
		}
	}

	// The state in the slot: a card beside the panel while a state row is focused.
	if (now.state && running.canSaveState)
	{
		const float cw = 432, chh = 320;
		const float cx = px - 32 - cw;
		const float rowY = itemsTop + qm.focusAnim * ih + (ih - 8) / 2;
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
		hint(Glyph::Cross, "Select");
		if (now.side)
			hint(Glyph::DPadLR, "Slot");
		hint(Glyph::Circle, "Resume");
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
	setScreen(Screen::None);
}

void gameStopped()
{
	if (be == nullptr)
		return;
	qm.open = 0;
	qm.statesFor.clear();
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
