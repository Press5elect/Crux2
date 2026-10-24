// Crux2 - the interface: the settings, as pages of rows.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// One table for the Settings (the values for every game) and for a game's own options (Details >
// Options in the library, "Game options" in the quick menu), so that neither lacks what the other
// has. Each row says where PCSX2 keeps the value (the section and key of inis/PCSX2.ini, and of a
// game's own file in gamesettings/) and what it can be.
//
// v1.6.0 groups the pages as PCSX2-Qt's settings window does (see the list below the helpers),
// with every setting it has that the console can use. Not here, because the console has nothing
// for them: the choice of graphics API and adapter, the window and fullscreen switches, video
// capture and screenshots (the console's Create button), the audio backend and device, the
// keyboard/mouse/XInput/DInput input sources, Discord, the automatic updater's channel (Crux2 has
// its own updater, under PlayStation 5), the debugger window and PINE (a local socket no program
// on the console could reach), folders (they are the title's own). Their values stay as
// inis/PCSX2.ini has them.

#include "bp_internal.h"

#include <cstdio>

namespace bp
{

namespace
{

constexpr const char* GS = "EmuCore/GS";
constexpr const char* CORE = "EmuCore";
constexpr const char* SPEED = "EmuCore/Speedhacks";
constexpr const char* CPU = "EmuCore/CPU";
constexpr const char* REC = "EmuCore/CPU/Recompiler";
constexpr const char* FIX = "EmuCore/Gamefixes";
constexpr const char* FRONT = "PS5UI";

using Names = std::vector<std::string>;

// The level a row is shown from (Settings shown: Normal, Advanced, Expert; Triangle in the Settings).
Option adv(Option o)
{
	o.flags |= Option::Advanced;
	return o;
}
Option expert(Option o)
{
	o.flags = (o.flags & ~Option::Advanced) | Option::Expert;
	return o;
}

Option heading(const char* label, unsigned flags = 0)
{
	Option o;
	o.kind = Option::Heading;
	o.label = label;
	o.flags = flags;
	return o;
}

Option toggle(const char* label, const char* desc, const char* section, const char* key, bool def, unsigned flags = 0)
{
	Option o;
	o.kind = Option::Bool;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defBool = def;
	o.flags = flags;
	return o;
}

// A list whose values are whole numbers counting up from `first`.
Option list(const char* label, const char* desc, const char* section, const char* key, int def, Names names, int first = 0,
	unsigned flags = 0)
{
	Option o;
	o.kind = Option::IntList;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defInt = def;
	for (size_t i = 0; i < names.size(); i++)
		o.ints.push_back(first + static_cast<int>(i));
	o.names = std::move(names);
	o.flags = flags;
	return o;
}

Option listOf(const char* label, const char* desc, const char* section, const char* key, int def, Names names, std::vector<int> values,
	unsigned flags = 0)
{
	Option o = list(label, desc, section, key, def, std::move(names), 0, flags);
	o.ints = std::move(values);
	return o;
}

Option floats(const char* label, const char* desc, const char* section, const char* key, float def, Names names, std::vector<float> values,
	unsigned flags = 0)
{
	Option o;
	o.kind = Option::FloatList;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defFloat = def;
	o.names = std::move(names);
	o.floats = std::move(values);
	o.flags = flags;
	return o;
}

Option strings(const char* label, const char* desc, const char* section, const char* key, const char* def, Names names, Names values,
	unsigned flags = 0)
{
	Option o;
	o.kind = Option::StringList;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defString = def;
	o.names = std::move(names);
	o.strings = std::move(values);
	o.flags = flags;
	return o;
}

Option range(const char* label, const char* desc, const char* section, const char* key, int def, int lo, int hi, int step, const char* unit,
	unsigned flags = 0)
{
	Option o;
	o.kind = Option::IntRange;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defInt = def;
	o.lo = lo;
	o.hi = hi;
	o.step = step;
	o.unit = unit;
	o.flags = flags;
	return o;
}

Option frange(const char* label, const char* desc, const char* section, const char* key, float def, double lo, double hi, double step,
	int decimals, const char* unit, unsigned flags = 0)
{
	Option o;
	o.kind = Option::FloatRange;
	o.label = label;
	o.desc = desc;
	o.section = section;
	o.key = key;
	o.defFloat = def;
	o.lo = lo;
	o.hi = hi;
	o.step = step;
	o.decimals = decimals;
	o.unit = unit;
	o.flags = flags;
	return o;
}

// How out-of-range numbers are clamped: PCSX2 keeps one choice as three switches.
Option clamping(const char* label, const char* desc, const char* first, const char* second, const char* third, Names names,
	unsigned flags = 0)
{
	Option o;
	o.kind = Option::Combo;
	o.label = label;
	o.desc = desc;
	o.section = REC;
	o.key = first;
	o.names = std::move(names);
	o.parts = {{REC, first, true}, {REC, second, false}, {REC, third, false}};
	o.combos = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}};
	o.flags = flags;
	return o;
}

Option action(const char* label, const char* desc, std::function<void()> run, unsigned flags = 0)
{
	Option o;
	o.kind = Option::Action;
	o.label = label;
	o.desc = desc;
	o.action = std::move(run);
	o.flags = flags;
	return o;
}

Option info(const char* label, std::function<std::string()> value, unsigned flags = 0)
{
	Option o;
	o.kind = Option::Info;
	o.label = label;
	o.info = std::move(value);
	o.flags = flags;
	return o;
}

// The value of a switch in effect: the open game's own where it has one, else the settings'.
bool effective(int layer, const char* section, const char* key, bool def)
{
	if (layer == Own)
	{
		if (const std::optional<bool> own = be->getBool(Own, section, key))
			return *own;
	}
	return be->getBool(Shared, section, key).value_or(def);
}

int effectiveInt(int layer, const char* section, const char* key, int def)
{
	if (layer == Own)
	{
		if (const std::optional<int> own = be->getInt(Own, section, key))
			return *own;
	}
	return be->getInt(Shared, section, key).value_or(def);
}

// Usable only while a list is on one of its choices.
Option whenInt(Option o, const char* section, const char* key, int def, int wanted)
{
	o.enabled = [section, key, def, wanted](int layer) { return effectiveInt(layer, section, key, def) == wanted; };
	return o;
}

// Usable only while a switch is on (or off).
Option when(Option o, const char* section, const char* key, bool def, bool wanted = true)
{
	o.enabled = [section, key, def, wanted](int layer) { return effective(layer, section, key, def) == wanted; };
	return o;
}

// Usable only while a list is not on one of its choices.
Option whenNot(Option o, const char* section, const char* key, int def, int unwanted)
{
	o.enabled = [section, key, def, unwanted](int layer) { return effectiveInt(layer, section, key, def) != unwanted; };
	return o;
}

const Names SPEEDS = {"2%", "10%", "25%", "50%", "75%", "90%", "100%", "110%", "120%", "150%", "175%", "200%", "300%", "400%", "500%", "1000%"};
const std::vector<float> SPEED_VALUES = {0.02f, 0.10f, 0.25f, 0.50f, 0.75f, 0.90f, 1.00f, 1.10f, 1.20f, 1.50f, 1.75f, 2.00f, 3.00f, 4.00f,
	5.00f, 10.00f};


// A Backend page under a heading of its own, when it does not begin with one.
void addPageRows(std::vector<Option>& rows, const Page& page, const char* heading_text)
{
	if (page.rows.empty())
		return;
	if (page.rows.front().kind != Option::Heading && heading_text != nullptr)
		rows.push_back(heading(heading_text));
	for (const Option& o : page.rows)
		rows.push_back(o);
}

void addRows(std::vector<Option>& rows, std::vector<Option> more)
{
	for (Option& o : more)
		rows.push_back(std::move(o));
}

// ===============================================================================================
// The pages, in PCSX2-Qt's order and with its groups (v1.6.0): Interface, Game List, BIOS,
// Emulation, Graphics (its tabs as headings: Display, Rendering, Hardware Fixes, Upscaling Fixes,
// Texture Replacement, Post-Processing, Advanced), On-Screen Display, Audio, Memory Cards,
// Network & HDD, Achievements, Controllers (with Hotkeys), Advanced, Debug; then what PCSX2 has
// no page for: PlayStation 5 (Crux2's own) and Statistics. A game's options: Emulation, Patches,
// Cheats, Game Fixes, Graphics, Audio, Memory Cards, Network & HDD, Controllers, BIOS.
// ===============================================================================================

// ------------------------------------------------------------------------------- Interface

Page interfacePage()
{
	Page page{ICON_FA_PALETTE, "Interface", {}};
	auto& rows = page.rows;
	const unsigned F = Option::Front;
	const unsigned A = Option::AllGames;

	rows.push_back(heading("BEHAVIOUR"));
	rows.push_back(list("Settings shown",
		"Normal: what most people need. Advanced: the finer settings as well. Expert: everything, the settings that can stop games "
		"from working included (the emulated processors, fixes for all games, PS5 tuning, native code, debugging; PCSX2's developers "
		"give no support for a configuration changed there). Triangle changes it from anywhere in the Settings",
		FRONT, "SettingsLevel", 0, {"Normal", "Advanced", "Expert"}, 0, Option::Front));
	rows.push_back(toggle("Pause when a controller disconnects", "Pauses the game when a controller it is played with goes away", "UI",
		"PauseOnControllerDisconnection", false, A));
	rows.push_back(toggle("Warn about unsafe settings", "A line on the screen when a game starts with settings that may break games", CORE,
		"WarnAboutUnsafeSettings", true, A));
	rows.push_back(toggle("Circle chooses, Cross goes back",
		"The two buttons change places in every menu, as on consoles of the Japanese kind. The hints at the bottom show the buttons as they are then",
		FRONT, "SwapConfirm", false, F));

	Names skinNames, accentNames, backNames{"The skin's own"};
	for (int i = 0; i < SkinCount; i++)
		skinNames.push_back(skins[i].family[0] ? std::string(skins[i].name) + " (" + skins[i].family + ")" : std::string(skins[i].name));
	for (int i = 0; i < AccentCount; i++)
		accentNames.push_back(accents[i].name);
	for (const char* name : backdropNames)
		backNames.push_back(name);
	rows.push_back(heading("APPEARANCE"));
	rows.push_back(list("Skin",
		"The colours and shapes of every screen. Crux is the title's own; Carbon is black, for an OLED screen; the thirty after them are the "
		"themes of the PS5 homebrew UI kit, as the other titles of the family have them",
		FRONT, "Look", 0, skinNames, 0, F));
	rows.push_back(list("Accent colour", "The colour of what is focused or switched on", FRONT, "Accent", 0, accentNames, 0, F));
	rows.push_back(list("Background",
		"What is behind the screens: still, alive in one of six ways, the focused game's picture, or pictures of your own (put them in the "
		"folder \"backgrounds\" of the title folder)",
		FRONT, "Backdrop", 0, backNames, 0, F));
	rows.push_back(adv(whenInt(listOf("My pictures: each stays for", "With several pictures in the folder, one follows the other", FRONT, "WallEvery", 60,
							   {"30 seconds", "1 minute", "2 minutes", "5 minutes", "15 minutes", "1 hour"}, {30, 60, 120, 300, 900, 3600}, F),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1)));
	rows.push_back(adv(whenInt(listOf("My pictures: made darker by", "So that what is written over a picture can be read", FRONT, "WallDim", 50,
							   {"Nothing", "25%", "40%", "50%", "60%", "75%"}, {0, 25, 40, 50, 60, 75}, F),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1)));
	rows.push_back(whenInt(action("My pictures: look into the folder again", "For pictures copied there while the title runs",
							   [] {
								   const int found = be->wallpapers(true);
								   toast(found == 0 ? std::string("No picture in the folder \"backgrounds\" of the title folder (PNG or JPEG).") :
													  std::to_string(found) + (found == 1 ? " picture." : " pictures."),
									   4.f);
							   }),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1));
	rows.push_back(list("Motion", "All: living background, screens that slide, cards that rise. Less: fades only. None: everything is at once",
		FRONT, "Motion", 0, {"All", "Less", "None"}, 0, F));
	rows.push_back(toggle("Start-up animation",
		"The name is built out of its parts when the title starts, and goes to the top bar. Any button ends it early", FRONT, "Splash", true, F));
	rows.push_back(when(list("Which start-up", "There are four, each timed to the start-up sound. Left alone, each start plays another one",
							FRONT, "SplashKind", 0, {"Another one each start", "Strike", "Unfold", "Spin", "Slash"}, 0, F),
		FRONT, "Splash", true));
	rows.push_back(listOf("Interface size",
		"The whole interface smaller, to see more of it at once: more rows, more covers, smaller text. 100% is the size it was made at",
		FRONT, "InterfaceSize", 100, {"100%", "90%", "80%", "70%"}, {100, 90, 80, 70}, F));
	rows.push_back(listOf("Text size", "The text of rows, lists and hints, smaller or larger than the rest of the interface", FRONT, "TextSize", 0,
		{"Smallest", "Smaller", "Normal", "Larger", "Largest"}, {-2, -1, 0, 1, 2}, F));
	rows.push_back(toggle("High contrast", "What is written is brighter, and the lines between things stronger", FRONT, "HighContrast", false, F));
	rows.push_back(toggle("Colours for colour blindness", "Blue and orange in place of green and red for what went well and what did not", FRONT,
		"ColourBlind", false, F));

	addRows(rows, be->extraRows("interface", Shared));

	rows.push_back(heading("CARE FOR THE SCREEN"));
	rows.push_back(listOf("Go dark when nothing is pressed",
		"After this long the screens go dark; after as long again they are black, with the mark wandering, until a button is pressed", FRONT,
		"IdleDim", 3, {"Never", "After 1 minute", "After 2 minutes", "After 3 minutes", "After 5 minutes", "After 10 minutes"}, {0, 1, 2, 3, 5, 10}, F));
	{
		Option o = toggle("Then the black screen with the mark",
			"Off: the screens only go dark, and stay so. The press that ends the black screen does nothing else", FRONT, "ScreenSaver", true, F);
		o.enabled = [](int) { return be->getInt(Shared, FRONT, "IdleDim").value_or(3) != 0; };
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(toggle("Move everything a little",
		"Every other minute all of a screen moves by a few pixels: no edge stays on the same row of pixels", FRONT, "PixelShift", true, F)));
	rows.push_back(adv(toggle("Fade the hints", "The words of the bar at the bottom go faint when nothing was pressed for a quarter of a minute", FRONT,
		"HintFade", true, F)));
	rows.push_back(listOf("Darken the game when nothing is pressed",
		"After this long without a button the picture is made darker, and is as it was with the next button. A film a game plays by itself is "
		"darkened too",
		FRONT, "GameDim", 0, {"Never", "After 5 minutes", "After 10 minutes", "After 15 minutes", "After 30 minutes"}, {0, 5, 10, 15, 30}, F));
	return page;
}

// ------------------------------------------------------------------------------- Game List

Page gameListPage()
{
	Page page{ICON_FA_FOLDER_OPEN, "Game List", {}};
	auto& rows = page.rows;
	rows.push_back(heading("GAME SCANNING"));
	rows.push_back(action("Scan for games", "Looks for new games in the games folder, on USB drives and on the network shares",
		[] {
			be->scan(false);
			toast("Looking for new games...", 3.f);
		},
		Option::NoGame));
	rows.push_back(action("Scan again from nothing",
		"Reads every game file anew, not only the new ones: for when a game shows the wrong title or serial. Takes a while on a network share",
		[] {
			be->scan(true);
			toast("Reading every game again...", 3.f);
		},
		Option::NoGame));
	rows.push_back(heading("THE LIST"));
	rows.push_back(list("Library view",
		"How the games are laid out: shelves, a grid of covers, a list, covers that flow past, books on a bookcase, a table, a showcase "
		"(the game large over its picture of play, a strip of covers under it), a wheel of titles beside the game's art, a wall of every "
		"cover, or the discs themselves. The touch pad goes from one to the next in the library",
		FRONT, "View", 0, {"Shelves", "Grid", "List", "Flow", "Bookcase", "Table", "Showcase", "Wheel", "Wall", "Discs"}, 0, Option::Front));
	rows.push_back(toggle("A picture of play behind the shelves",
		"In the shelves view, the focused game's picture from play is behind its title, where it has one", FRONT, "HeroPlay", true, Option::Front));
	rows.push_back(toggle("Group the discs of a game",
		"A game with several discs is one entry; it starts from the disc last played, and Details chooses another", FRONT, "GroupDiscs", true,
		Option::Front));
	rows.push_back(toggle("English titles", "For a game that has a title in its own language and one in English: the English one", "UI",
		"PreferEnglishGameList", false, Option::AllGames));
	rows.push_back(action("Fetch the games' pictures",
		"For each game, its title screen and a picture of it being played, from libretro's collection of thumbnails",
		[] { be->fetchPictures(nullptr, false); }));
	addRows(rows, be->extraRows("library", Shared));
	return page;
}

// ------------------------------------------------------------------------------- BIOS

Page biosPageOf(int layer, bool forGame)
{
	Page page{ICON_FA_MICROCHIP, "BIOS", {}};
	auto& rows = page.rows;
	if (!forGame)
	{
		rows.push_back(heading("THE PLAYSTATION 2'S OWN MENU"));
		rows.push_back(action("Start the PlayStation 2 menu",
			"The BIOS with no game: the PS2's browser (its memory cards and discs) and its System Configuration (clock, language, "
			"screen). The quick menu's Quit game goes back to the library",
			[] { startPs2Menu(); }, Option::NoGame));
	}
	addPageRows(rows, be->biosPage(layer), "BIOS SELECTION");
	// The language a game is told the console has (Crux2): many European games pick theirs from it.
	// Not written to the BIOS's own settings; for one game, in its options.
	rows.push_back(heading("LANGUAGE"));
	rows.push_back(list("Console language",
		"The language games are told the PlayStation 2 is set to: many European games show theirs in it. As the BIOS has it: the "
		"language its own System Configuration chose. The BIOS's setting is left as it is",
		"PS5", "ConsoleLanguage", -1,
		{"As the BIOS has it", "Japanese", "English", "French", "Spanish", "German", "Italian", "Dutch", "Portuguese"}, -1,
		Option::NextStart));
	if (!forGame)
	{
		rows.push_back(heading("OPTIONS AND PATCHES"));
		rows.push_back(toggle("Fast boot", "Starts the game straight away, without the PS2's start-up screens", CORE, "EnableFastBoot", true));
		rows.push_back(adv(when(toggle("Fast-forward to the game", "Runs at full tilt until the game itself starts", CORE, "EnableFastBootFastForward",
								false),
			CORE, "EnableFastBoot", true)));
	}
	return page;
}

// ------------------------------------------------------------------------------- Emulation

Page emulationPage(int layer, bool forGame)
{
	Page page{ICON_FA_GEARS, "Emulation", {}};
	auto& rows = page.rows;

	rows.push_back(heading("SPEED CONTROL"));
	rows.push_back(floats("Normal speed", "100% is the console's own: 60 frames a second for NTSC games, 50 for PAL", "Framerate", "NominalScalar", 1.f,
		SPEEDS, SPEED_VALUES));
	rows.push_back(floats("Fast-forward speed", "While fast forward is on (quick menu, or a button under Controllers)", "Framerate", "TurboScalar", 2.f,
		SPEEDS, SPEED_VALUES));
	rows.push_back(floats("Slow-motion speed", "While slow motion is on (a button under Controllers)", "Framerate", "SlomoScalar", 0.5f, SPEEDS,
		SPEED_VALUES));

	rows.push_back(heading("SYSTEM SETTINGS"));
	rows.push_back(list("EE cycle rate",
		"Runs the PS2's main processor slower or faster. Faster can steady a game that also slowed down on a real PS2, and needs more "
		"speed here. Anything but 100% can break games",
		SPEED, "EECycleRate", 0, {"50%", "60%", "75%", "100% (the PS2's own)", "130%", "180%", "300%"}, -3));
	rows.push_back(expert(list("EE cycle skipping", "Helps very few games; usually makes a game stutter or break", SPEED, "EECycleSkip", 0,
		{"Off", "Mild", "Moderate", "Maximum"})));
	rows.push_back(adv(toggle("Multi-threaded VU1 (MTVU)",
		"Runs a vector unit on a thread of its own: faster in most 3D games. A few games hang or glitch with it", SPEED, "vuThread", false)));
	{
		// One choice for two switches: PCSX2's pinning and Crux2's own layout (which wins when both
		// are on, so that state is not offered).
		Option o;
		o.kind = Option::Combo;
		o.label = "Thread pinning";
		o.desc = "Anywhere: the console decides, best in most games. Pinned: PCSX2's way, the three main threads each kept on a core. "
				 "Cores of their own (Crux2): the main threads get cores nothing else uses. For a game that is slow at every resolution";
		o.section = "PS5";
		o.key = "ThreadLayout";
		o.names = {"Anywhere", "Pinned (PCSX2)", "Cores of their own (Crux2)"};
		o.parts = {{CORE, "EnableThreadPinning", false}, {"PS5", "ThreadLayout", false}};
		o.combos = {{0, 0}, {1, 0}, {0, 1}};
		o.flags = Option::AllGames | Option::NextStart;
		o.flags |= Option::Advanced;
		rows.push_back(std::move(o));
	}
	rows.push_back(toggle("Fast disc access", "Shorter loading. A few games break with it", SPEED, "fastCDVD", false));
	rows.push_back(toggle("Load the whole disc into memory",
		"Reads the whole disc image before the game starts, so a network share is not needed while playing. Takes as long as copying it",
		CORE, "CdvdPrecache", false, Option::NextStart));
	rows.push_back(toggle("Cheats", "Uses the cheats chosen for a game (each game's Cheats page)", CORE, "EnableCheats", false));
	rows.push_back(expert(toggle("Host file system", "Lets games and homebrew read the title folder's files", CORE, "HostFs", false, Option::Advanced)));

	rows.push_back(heading("FRAME PACING / LATENCY CONTROL"));
	addRows(rows, be->extraRows("motion", layer));
	{
		Option o;
		o.kind = Option::Combo;
		o.label = "Frame pacing";
		o.desc = "By the screen: every frame is shown for the same time, the smoothest. By the clock: for a game whose frame rate the "
				 "screen does not have (Crux2 switches to it by itself when needed)";
		o.section = GS;
		o.key = "UseVSyncForTiming";
		o.names = {"By the screen (smoothest)", "By the clock, in step with the screen", "By the clock, at the game's own rate"};
		o.parts = {{GS, "VsyncEnable", true}, {GS, "UseVSyncForTiming", true}, {GS, "SyncToHostRefreshRate", true}};
		o.combos = {{1, 1, 1}, {1, 0, 1}, {1, 0, 0}};
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(list("Frames queued for the display",
		"How many finished frames may wait to be shown (PCSX2's optimal frame pacing is 0). None: the least delay, and the most demanding", GS,
		"VsyncQueueSize", 0, {"None (hard sync)", "1 frame", "2 frames", "3 frames"}, 0)));
	rows.push_back(adv(toggle("Show a frame as it begins",
		"Shows each frame earlier: can lower the controller's delay. A few games show glitches with it", GS, "AdvancedFrameDisplay", false)));
	{
		Option o = toggle("Skip repeated frames",
			"With pacing by the clock: a game at 25 or 30 frames a second shows each frame twice, and the repeat is left out. Faster, less even",
			GS, "SkipDuplicateFrames", false);
		o.enabled = [](int l) { return !effective(l, GS, "UseVSyncForTiming", true); };
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(toggle("Black frame insertion",
		"At 120 Hz, every other refresh is black: moving things look sharper, like on a CRT, and the picture is darker. Not together "
		"with frame generation's doubling",
		GS, "BlackFrameInsertion", false)));

	if (forGame)
	{
		rows.push_back(heading("REAL-TIME CLOCK"));
		const unsigned f = Option::OneGame;
		rows.push_back(toggle("Set the clock by hand", "The emulated PS2's clock starts at a date you choose instead of today", CORE,
			"ManuallySetRealTimeClock", false, f | Option::NextStart));
		Names years;
		for (int year = 2000; year < 2100; year++)
			years.push_back(std::to_string(year));
		rows.push_back(when(list("Year", "", CORE, "RtcYear", 0, years, 0, f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Month", "", CORE, "RtcMonth", 1, 1, 12, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Day", "", CORE, "RtcDay", 1, 1, 31, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Hour", "", CORE, "RtcHour", 0, 0, 23, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Minute", "", CORE, "RtcMinute", 0, 0, 59, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Second", "", CORE, "RtcSecond", 0, 0, 59, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
	}
	return page;
}

// ------------------------------------------------------------------------------- Game Fixes

// The renderer's fixes, the same rows for all games (advanced, as PCSX2 marks them) and for one game.
void hardwareFixRows(std::vector<Option>& rows, unsigned f)
{
	// (None of these with paraLLEl-GS, which has no texture cache and no upscaling: greyed out while it is on.)
	const auto fix = [](Option o) {
		o.enabled = [](int l) { return effective(l, GS, "UserHacks", false) && !effective(l, GS, "PS5ParallelGS", false); };
		return o;
	};
	Option manual = toggle("Manual hardware renderer fixes",
		"Crux2 applies the known fixes for each game by itself. Turn this on only if a game still draws wrong: the fixes below then take their "
		"place", GS, "UserHacks", false, f);
	manual.enabled = [](int l) { return !effective(l, GS, "PS5ParallelGS", false); };
	rows.push_back(std::move(manual));
	rows.push_back(fix(list("CPU sprite render size", "The widest target the processor draws sprites for", GS, "UserHacks_CPUSpriteRenderBW", 0,
		{"Off", "1 (64 wide)", "2 (128)", "3 (192)", "4 (256)", "5 (320)", "6 (384)", "7 (448)", "8 (512)", "9 (576)", "10 (640)"}, 0, f)));
	rows.push_back(fix(list("CPU sprite render level", "What the processor draws", GS, "UserHacks_CPUSpriteRenderLevel", 0,
		{"Sprites only", "Sprites and triangles", "Blended sprites and triangles"}, 0, f)));
	rows.push_back(fix(list("Software CLUT render", "Draws a palette the game draws itself on the processor", GS, "UserHacks_CPUCLUTRender", 0,
		{"Off", "Normal", "Aggressive"}, 0, f)));
	rows.push_back(fix(list("GPU target CLUT", "Draws a palette the game draws itself on the graphics chip", GS, "UserHacks_GPUTargetCLUTMode", 0,
		{"Off", "On (exact match)", "On (check inside the target)"}, 0, f)));
	rows.push_back(fix(range("Skip draws from", "Leaves out the draws from this one to the one below", GS, "UserHacks_SkipDraw_Start", 0, 0, 5000, 1, "", f)));
	rows.push_back(fix(range("Skip draws to", "", GS, "UserHacks_SkipDraw_End", 0, 0, 5000, 1, "", f)));
	rows.push_back(fix(list("Auto flush",
		"Fixes some effects (shadows, lighting) that read the picture back. Costs speed", GS, "UserHacks_AutoFlushLevel", 0,
		{"Off", "Sprites only", "All primitives"}, 0, f)));
	rows.push_back(fix(toggle("Framebuffer conversion on the processor", "Converts 4 and 8 bit framebuffers on the processor", GS,
		"UserHacks_CPU_FB_Conversion", false, f)));
	rows.push_back(fix(toggle("No depth conversion", "Depth buffers are not kept as textures", GS, "UserHacks_DisableDepthSupport", false, f)));
	rows.push_back(fix(toggle("No safe features", "Switches several safe features off", GS, "UserHacks_Disable_Safe_Features", false, f)));
	rows.push_back(fix(toggle("No render fixes", "The fixes for particular games are not applied", GS, "UserHacks_DisableRenderFixes", false, f)));
	rows.push_back(fix(toggle("Preload frame data", "Puts what the game wrote straight into video memory on the screen. Fixes black screens in "
		"games that draw on the processor (Tekken 5's Arcade History)", GS, "preload_frame_with_gs_data", false, f)));
	rows.push_back(fix(toggle("No partial invalidation", "A texture that is touched at all is thrown away whole", GS, "UserHacks_DisablePartialInvalidation",
		false, f)));
	rows.push_back(fix(list("Texture inside a render target", "Lets a part of an earlier picture be used as a texture", GS, "UserHacks_TextureInsideRt", 0,
		{"Off", "Inside the target", "Merge targets"}, 0, f)));
	rows.push_back(fix(toggle("Read targets when closing", "Writes every picture back to the PS2's memory when the game is closed", GS,
		"UserHacks_ReadTCOnClose", false, f)));
	rows.push_back(fix(toggle("Estimate texture region", "Makes textures smaller where the game does not say how large they are", GS,
		"UserHacks_EstimateTextureRegion", false, f)));
	rows.push_back(fix(toggle("Draw buffering", "Fewer draws in games that switch between two blending states", GS, "UserHacks_DrawBuffering", false, f)));
	rows.push_back(fix(toggle("Rewrite large ST", "Rewrites large texture coordinates and clamps them", GS, "UserHacks_RewriteLargeSTCoords", false, f)));
	rows.push_back(fix(toggle("Target page rearranging", "For games that change how a picture's memory is laid out", GS, "UserHacks_TargetPageRearranging",
		false, f)));
	rows.push_back(fix(toggle("Palette conversion on the graphics chip", "Paletted textures are converted there, not on the processor", GS, "paltex",
		false, f)));
	rows.push_back(fix(list("Limit depth to 24 bits", "For games whose surfaces flicker against each other", GS, "UserHacks_Limit24BitDepth", 0,
		{"Off", "Keep the upper bits", "Keep the lower bits"}, 0, f)));
}

void upscalingFixRows(std::vector<Option>& rows, unsigned f)
{
	const auto fix = [](Option o) {
		o.enabled = [](int l) { return effective(l, GS, "UserHacks", false) && !effective(l, GS, "PS5ParallelGS", false); };
		return o;
	};
	rows.push_back(fix(list("Half pixel offset", "Can fix misplaced fog, bloom or blur", GS, "UserHacks_HalfPixelOffset", 0,
		{"Off", "Normal (vertex)", "Special (texture)", "Special (texture, aggressive)", "Align to native", "Align to native, with texture offset"}, 0, f)));
	rows.push_back(fix(list("Native scaling", "Hides seams in games that draw their own 2D effects", GS, "UserHacks_native_scaling", 0,
		{"Off", "Normal", "Aggressive", "Normal (keep the upscale)", "Aggressive (keep the upscale)"}, 0, f)));
	rows.push_back(fix(list("Round sprite", "Corrects sprite coordinates", GS, "UserHacks_round_sprite_offset", 0, {"Off", "Half", "Full"}, 0, f)));
	rows.push_back(fix(list("Bilinear dirty upscale", "Can smooth textures that are filtered when upscaling", GS, "UserHacks_BilinearHack", 0,
		{"Automatic", "Force bilinear", "Force nearest"}, 0, f)));
	rows.push_back(fix(range("Texture offset X", "Moves target textures", GS, "UserHacks_TCOffsetX", 0, -4096, 4096, 1, "", f)));
	rows.push_back(fix(range("Texture offset Y", "", GS, "UserHacks_TCOffsetY", 0, -4096, 4096, 1, "", f)));
	rows.push_back(fix(toggle("Align sprite", "Fixes vertical lines in some games when upscaling", GS, "UserHacks_align_sprite_X", false, f)));
	rows.push_back(fix(toggle("Merge sprite", "Fewer lines in games whose screens are made of many sprites", GS, "UserHacks_merge_pp_sprite", false, f)));
	rows.push_back(fix(toggle("Even sprite positions", "Closes gaps between pixels when upscaling", GS, "UserHacks_ForceEvenSpritePosition", false, f)));
	rows.push_back(fix(toggle("Palette draws at native resolution", "Draws with a paletted texture are not upscaled", GS, "UserHacks_NativePaletteDraw",
		false, f)));
}

Page gameFixesPage()
{
	Page page{ICON_FA_TOOLBOX, "Game Fixes", {}};
	auto& rows = page.rows;
	const unsigned f = Option::OneGame;
	rows.push_back(heading("GAME FIXES (SET BY ITSELF FOR KNOWN GAMES)", f));
	rows.push_back(toggle("FPU multiply hack", "For a game whose maths needs it", FIX, "FpuMulHack", false, f));
	rows.push_back(toggle("Software renderer for videos", "For games whose videos the hardware renderer draws wrong", FIX, "SoftwareRendererFMVHack", false, f));
	rows.push_back(toggle("Skip videos", "Leaves out videos that make a game hang", FIX, "SkipMPEGHack", false, f));
	rows.push_back(toggle("Preload TLB hack", "Avoids a TLB miss in a game that has one at its start", FIX, "GoemonTlbHack", false, f));
	rows.push_back(toggle("EE timing hack", "A timing change for several games", FIX, "EETimingHack", false, f));
	rows.push_back(toggle("Instant DMA hack", "For problems with the emulated cache", FIX, "InstantDMAHack", false, f));
	rows.push_back(toggle("OPH flag hack", "For games that hang waiting for a GS flag", FIX, "OPHFlagHack", false, f));
	rows.push_back(toggle("Emulate the GIF FIFO", "More exact, and slower", FIX, "GIFFIFOHack", false, f));
	rows.push_back(toggle("DMA busy hack", "For games that wait on a DMA that is not busy", FIX, "DMABusyHack", false, f));
	rows.push_back(toggle("Delay VIF1 stalls", "For HUDs and loading that need the stalls later", FIX, "VIF1StallHack", false, f));
	rows.push_back(toggle("Emulate the VIF FIFO", "Reads ahead in the VIF1 FIFO", FIX, "VIFFIFOHack", false, f));
	rows.push_back(toggle("Full VU0 synchronisation", "Keeps the first vector unit in step at every instruction", FIX, "FullVU0SyncHack", false, f));
	rows.push_back(toggle("VU I bit hack", "Avoids constant recompiling", FIX, "IbitHack", false, f));
	rows.push_back(toggle("VU add hack", "For the games of one developer that need it", FIX, "VuAddSubHack", false, f));
	rows.push_back(toggle("VU overflow hack", "Checks for overflows", FIX, "VUOverflowHack", false, f));
	rows.push_back(toggle("VU sync", "Runs the vector units behind, for games that read their registers", FIX, "VUSyncHack", false, f));
	rows.push_back(toggle("VU XGKick sync", "Exact timing of the vector units' transfers. Slower", FIX, "XgKickHack", false, f));
	rows.push_back(toggle("Count frames by blits", "Another way to count the game's frames, where the usual one is wrong", FIX, "BlitInternalFPSHack", false, f));
	return page;
}

// ------------------------------------------------------------------------------- Graphics

// A preset of the picture's quality: a few values set at once, each still changeable after.
struct PicturePreset
{
	const char* name;
	const char* desc;
	float scale;  // internal resolution
	int cas;      // upscaling and sharpening
	int aniso;    // anisotropic filtering
	const char* said;
};

const PicturePreset PICTURE_PRESETS[] = {
	{"Fastest", "The PlayStation 2's own resolution, nothing extra: for a game that is slow at any setting", 1.f, 0, 0,
		"Picture: the PS2's own resolution, no sharpening."},
	{"Balanced", "Drawn at about 1080p, a little sharper textures: the choice for most games", 3.f, 0, 4,
		"Picture: about 1080p, 4x anisotropic filtering."},
	{"Sharpest", "Drawn at about 1440p and brought to 4K with AMD FSR 1, the sharpest textures: for a game with speed to spare", 4.f, 5, 16,
		"Picture: about 1440p with FSR 1 to 4K, 16x anisotropic filtering."},
};

Page graphicsPage(int layer, bool forGame)
{
	Page page{ICON_FA_IMAGE, "Graphics", {}};
	auto& rows = page.rows;
	const auto pgs = [](int l) { return effective(l, GS, "PS5ParallelGS", false); };
	// PCSX2's hardware renderer, as against the software renderer and against paraLLEl-GS: the rows marked hw() are
	// its own (its texture cache, its resolution, its filtering, its fixes), which paraLLEl-GS has no equivalent of;
	// hwAny() is for rows paraLLEl-GS takes too (ps5_pgs.cpp's SendScanout). Software draws like the PS2 and has
	// none of either.
	const auto hardware = [pgs](int l) { return effectiveInt(l, GS, "Renderer", 14) != 13 && !pgs(l); };
	const auto hardwareAny = [](int l) { return effectiveInt(l, GS, "Renderer", 14) != 13; };
	const auto software = [](int l) { return effectiveInt(l, GS, "Renderer", 14) == 13; };
	const auto hw = [hardware](Option o) {
		o.enabled = hardware;
		return o;
	};
	const auto hwAny = [hardwareAny](Option o) {
		o.enabled = hardwareAny;
		return o;
	};
	const auto notPgs = [pgs](Option o) {
		o.enabled = [pgs](int l) { return !pgs(l); };
		return o;
	};
	const auto sw = [software](Option o) {
		o.enabled = software;
		return o;
	};
	const auto no_fsr = [hardware](int l) {
		const int mode = effectiveInt(l, GS, "CASMode", 5);
		return hardware(l) && (mode < 3 || mode > 5);
	};
	const unsigned ADV = Option::Advanced;

	// Crux2's: three presets, and the resolution that follows the graphics chip's load.
	rows.push_back(heading("QUICK SETUP"));
	for (const PicturePreset& p : PICTURE_PRESETS)
	{
		Option o = action(p.name, p.desc, [layer, p] {
			be->setInt(layer, GS, "Renderer", 14);
			be->setBool(layer, GS, "PS5ParallelGS", false);
			be->setFloat(layer, GS, "upscale_multiplier", p.scale);
			be->setInt(layer, GS, "CASMode", p.cas);
			be->setInt(layer, GS, "MaxAnisotropy", p.aniso);
			toast(p.said, 3.f);
		});
		rows.push_back(std::move(o));
	}
	rows.push_back(hwAny(toggle("Automatic quality",
		"When the graphics chip cannot keep the game at full speed, the internal resolution goes down a step for as long as it needs to "
		"(said on the screen), and back up when there is room again; with paraLLEl-GS, the samples a pixel instead. What you chose stays "
		"what you chose. No help where the processor is the limit",
		GS, "PS5AutoQuality", true)));

	// ---- Display
	rows.push_back(heading("DISPLAY"));
	{
		// One choice for PCSX2's two renderers and paraLLEl-GS (which is PCSX2's hardware renderer with
		// [EmuCore/GS] PS5ParallelGS on: the picture goes on through PCSX2's presentation).
		Option o;
		o.kind = Option::Combo;
		o.label = "Renderer";
		o.desc = "Hardware: the console's graphics chip draws, at any resolution. Software: the processor draws, like the PS2 did; slow, always "
				 "at the PS2's resolution, but right in the few games the hardware renderer gets wrong. paraLLEl-GS: the whole GS drawn on "
				 "the graphics chip in compute shaders: far less work for the processor, no texture cache; the picture is the PS2's own "
				 "resolution made sharp by super-sampling (the rows below), so the resolution setting does not apply. It takes Deinterlacing, "
				 "Anti-blur, Screen offsets, Show overscan, Mipmapping and everything after the picture is made (FSR, looks, FXAA, shade "
				 "boost); what it has no equivalent of (texture filtering, the hardware fixes, texture packs) is greyed out. Its "
				 "save states are its own. Takes effect when a game starts";
		o.section = GS;
		o.key = "Renderer";
		o.names = {"Hardware (Vulkan)", "Software", "paraLLEl-GS"};
		Option::Part renderer;
		renderer.section = GS;
		renderer.key = "Renderer";
		renderer.isInt = true;
		renderer.defInt = 14;
		Option::Part pgs_part;
		pgs_part.section = GS;
		pgs_part.key = "PS5ParallelGS";
		pgs_part.def = false;
		o.parts = {renderer, pgs_part};
		o.combos = {{14, 0}, {13, 0}, {14, 1}};
		o.flags = Option::NextStart;
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(when(listOf("paraLLEl-GS samples",
		"Samples for each pixel: more is a smoother, sharper picture and more work for the graphics chip. With the picture at twice the size, 4 and 8 give the same size (8 smooths a little more, at about twice the work); 16 with it is the sharpest. Its picture is enlarged "
		"to the screen as Upscaling and sharpening says (an FSR 1 choice enlarges it with FSR)",
		GS, "PS5ParallelGSSamples", 8, {"1 (the PS2's picture)", "2", "4", "8", "16"}, {1, 2, 4, 8, 16}, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS at twice the size",
		"With 4 samples or more, the picture is made at twice the PS2's size from the samples: sharper, where the game draws its "
		"3-D straight to the screen",
		GS, "PS5ParallelGSHighRes", true, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS sharp 2-D",
		"Text, menus and other 2-D super-sampled like the 3-D. Sharper, at the risk of seams where pieces of 2-D meet",
		GS, "PS5ParallelGSSharp2D", false, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS hands work over as it goes (experimental)",
		"The graphics chip gets what is recorded so far while paraLLEl-GS's thread waits for the game, instead of a frame at a time: "
		"a game that reads its picture back waits for less. Off: a frame at a time, as before v1.0.8",
		GS, "PS5ParallelGSEarlySubmit", false, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS full-detail effects",
		"Keeps the samples where a game draws its picture into a texture and over the screen again (blur, glow, depth of field): "
		"without them such effects bring the picture back to the PS2's resolution, as the PS2 showed them. More work for the "
		"graphics chip",
		GS, "PS5ParallelGSTextures", false, Option::NextStart), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS pictures shared with the graphics device (experimental)",
		"Each picture goes from paraLLEl-GS to the rest of the picture's way (FSR, frame generation, the on-screen display) "
		"by the graphics chip, from memory both share, instead of being copied by the processor: less work for the GS "
		"thread, most at twice the size. Off: copied by the processor, as before",
		GS, "PS5ParallelGSSharedFrames", false, Option::NextStart), GS, "PS5ParallelGS", false)));
	rows.push_back(strings("Aspect ratio", "The shape of the picture", GS, "AspectRatio", "Auto 4:3/3:2",
		{"Stretch to fill the screen", "Automatic (4:3 or 3:2)", "4:3", "16:9", "10:7"}, {"Stretch", "Auto 4:3/3:2", "4:3", "16:9", "10:7"}));
	rows.push_back(adv(strings("FMV aspect ratio override", "A different shape while the game plays a video", GS, "FMVAspectRatioSwitch", "Off",
		{"The same as the game", "Automatic (4:3 or 3:2)", "4:3", "16:9", "10:7"}, {"Off", "Auto 4:3/3:2", "4:3", "16:9", "10:7"})));
	{
		Option o;
		o.kind = Option::Combo;
		o.label = "Beside a 4:3 picture";
		o.desc = "What fills the sides of the screen. Dark grey ages an OLED screen more evenly than black";
		o.section = GS;
		o.key = "SidePicture";
		o.names = {"Black", "Dark grey", "A blurred copy of the picture"};
		o.parts = {{FRONT, "GreyBars", false}, {GS, "SidePicture", false}};
		o.combos = {{0, 0}, {1, 0}, {0, 1}};
		rows.push_back(std::move(o));
	}
	rows.push_back(list("Deinterlacing",
		"Automatic suits most games. Try another if the picture shakes or shows thin lines. With paraLLEl-GS: Automatic shows a progressive "
		"picture where the game allows it; any other choice shows the fields as the PS2's own scan-out blended them",
		GS, "deinterlace_mode", 0,
		{"Automatic", "Off", "Weave (top field first)", "Weave (bottom field first)", "Bob (top field first)", "Bob (bottom field first)",
			"Blend (top field first)", "Blend (bottom field first)", "Adaptive (top field first)", "Adaptive (bottom field first)"}));
	rows.push_back(adv(list("Bilinear filtering", "How the picture is stretched to the screen. Sharp bilinear keeps pixels crisp without shimmering", GS,
		"linear_present_mode", 2, {"Off (pixels)", "Bilinear (soft)", "Bilinear (sharp)"})));
	rows.push_back(toggle("Widescreen patches", "Games that have a 16:9 patch start in widescreen. Set the aspect ratio to 16:9 with it. Can cause glitches",
		CORE, "EnableWideScreenPatches", true, Option::AllGames | Option::NextStart));
	rows.push_back(toggle("No-interlacing patches", "Games that have a patch for it stop interlacing: a steadier, sharper picture", CORE,
		"EnableNoInterlacingPatches", true, Option::AllGames | Option::NextStart));
	// A game's own: PCSX2 keeps these as the game's patch lists ([Patches] Enable and Disable, by
	// the patch's name), not as the switches above, which are every game's.
	rows.push_back(list("Widescreen patch", "This game's 16:9 patch, where it has one", "PS5GamePatch", "Widescreen 16:9", 0,
		{"As in Settings", "On", "Off"}, 0, Option::OneGame | Option::NextStart));
	rows.push_back(list("No-interlacing patch", "This game's patch that stops interlacing, where it has one", "PS5GamePatch", "No-Interlacing", 0,
		{"As in Settings", "On", "Off"}, 0, Option::OneGame | Option::NextStart));
	rows.push_back(toggle("Anti-blur", "Removes the blur some games add on purpose. Sharper, a little less like the console", GS, "pcrtc_antiblur", true));
	rows.push_back(toggle("Integer scaling", "Enlarges by whole numbers only: sharper pixels in 2D games, with borders", GS, "IntegerScaling", false));
	rows.push_back(adv(toggle("Screen offsets", "Places the picture where the game asks for it", GS, "pcrtc_offsets", false)));
	rows.push_back(adv(toggle("Show overscan", "Shows the edges an old television hid", GS, "pcrtc_overscan", false)));
	rows.push_back(adv(notPgs(toggle("Disable interlace offset", "Can make an interlaced picture less blurry", GS, "disable_interlace_offset", true))));
	rows.push_back(adv(range("Vertical stretch", "Makes the picture taller or flatter", GS, "StretchY", 100, 10, 300, 1, "%")));
	rows.push_back(adv(range("Crop left", "Cuts off the picture's edges, keeping its shape", GS, "CropLeft", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop top", "", GS, "CropTop", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop right", "", GS, "CropRight", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop bottom", "", GS, "CropBottom", 0, 0, 720, 1, " px")));
	addRows(rows, be->extraRows("output", layer));

	// ---- Rendering
	rows.push_back(heading("RENDERING"));
	{
		Option o = floats("Internal resolution",
			"How sharp the game is drawn. Higher looks better on a big television and needs more speed. With an FSR 1 choice in the next row, FSR "
			"picks it",
			GS, "upscale_multiplier", 6.f,
			{"Native (PS2)", "2x (~720p)", "3x (~1080p)", "4x (~1440p)", "5x (~1800p)", "6x (~2160p, 4K)", "7x", "8x (~2880p)", "9x", "10x", "11x", "12x"},
			{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f, 12.f});
		o.enabled = no_fsr;
		rows.push_back(std::move(o));
	}
	rows.push_back(hwAny(listOf("Upscaling and sharpening",
		"Off: the internal resolution above is what you get. FSR 1 draws the game smaller and enlarges it to the screen with AMD's filter: nearly as sharp, much faster; it sets the internal resolution itself. NIS enlarges and "
		"sharpens (NVIDIA's filter). CAS only sharpens",
		GS, "CASMode", 5,
		{"Off", "FSR 1 from 4x (sharpest)", "FSR 1 from 3x", "FSR 1 from 2x (fastest)", "NIS: enlarge and sharpen", "CAS: sharpen", "CAS: sharpen and resize"},
		{0, 5, 3, 4, 6, 1, 2})));
	{
		Option o = range("Sharpness", "How strong the sharpening is", GS, "CASSharpness", 50, 0, 100, 5, "%");
		o.enabled = [hardwareAny](int l) { return hardwareAny(l) && effectiveInt(l, GS, "CASMode", 5) != 0; };
		rows.push_back(std::move(o));
	}
	rows.push_back(hw(list("Texture filtering", "How textures are smoothed. \"As the PS2\" is right for nearly every game", GS, "filter", 2,
		{"Nearest (blocky)", "Bilinear (always)", "Bilinear (as the PS2)", "Bilinear (always, not sprites)"})));
	rows.push_back(adv(hw(list("Trilinear filtering", "Smooths between a texture's smaller copies", GS, "TriFilter", -1,
		{"Automatic", "Off", "As the PS2", "Always"}, -1))));
	rows.push_back(hw(listOf("Anisotropic filtering", "Sharper textures on floors and walls seen at an angle. Costs little", GS, "MaxAnisotropy", 0,
		{"Off", "2x", "4x", "8x", "16x"}, {0, 2, 4, 8, 16})));
	rows.push_back(adv(hwAny(list("Dithering", "The fine pattern the PS2 lays over colour gradients. With paraLLEl-GS: Off and Force 32 bit "
		"draw without it (Force 32 bit also shows the 32-bit picture a game copies into a 16-bit screen at the end), Scaled and Unscaled "
		"as the PS2", GS, "dithering_ps2", 2,
		{"Off", "Scaled", "Unscaled (as the PS2)", "Force 32 bit"}))));
	rows.push_back(hw(list("Blending accuracy",
		"How exactly transparent effects (smoke, glass, lights) are drawn. Basic suits most games; higher is more exact and slower. "
		"Some games get a higher level from the game database by themselves",
		GS, "accurate_blending_unit", 1, {"Minimum", "Basic", "Medium", "High", "Full (slow)", "Maximum (very slow)"})));
	rows.push_back(adv(hwAny(toggle("Mipmapping", "Smaller copies of textures for far surfaces. Some games need it to look right", GS, "hw_mipmap", true))));
	rows.push_back(adv(hw(toggle("Accurate alpha test", "Some games need it to draw right. Costs some speed", GS, "HWAccurateAlphaTest", false))));
	rows.push_back(adv(hw(toggle("AA1", "The PS2's own edge smoothing. A few games need it to draw right; can cost a lot of speed", GS, "HWAA1", false))));
	rows.push_back(adv(hw(toggle("Rasterizer ordered view",
		"The higher blending accuracies without a pause of the GPU before each draw: much faster on this console (the GPU orders "
		"the pixels itself). Used only where it saves pauses. Off for a game that shows glitches with it", GS, "HWROV", true))));
	rows.push_back(adv(sw(range("Software renderer threads", "How many threads help draw", GS, "extrathreads", 2, 0, 10, 1, ""))));
	rows.push_back(adv(sw(toggle("Software auto flush", "Needed by effects that read the picture back as a texture", GS, "autoflush_sw", true))));
	rows.push_back(adv(sw(toggle("Software mipmapping", "Smaller copies of textures for far surfaces", GS, "mipmap", true))));

	// ---- Hardware Fixes and Upscaling Fixes: a game's own, or every game's (advanced)
	const unsigned fixFlags = forGame ? Option::OneGame : (Option::AllGames | Option::Expert);
	rows.push_back(heading("HARDWARE FIXES", fixFlags));
	hardwareFixRows(rows, fixFlags);
	rows.push_back(heading("UPSCALING FIXES", fixFlags));
	upscalingFixRows(rows, fixFlags);

	// ---- Texture Replacement
	rows.push_back(heading("TEXTURE REPLACEMENT"));
	rows.push_back(hw(toggle("Load textures",
		"A folder or ZIP named after the game's serial, in textures/ of the title folder, or in a folder named Crux2 or PS2 on a USB drive "
		"or a network share. A game's options say where its pack was found",
		GS, "LoadTextureReplacements", true)));
	rows.push_back(adv(when(toggle("Asynchronous texture loading", "Less stutter while textures come in", GS, "LoadTextureReplacementsAsync", true), GS,
		"LoadTextureReplacements", true)));
	rows.push_back(adv(when(toggle("Precache replacements", "The whole pack into memory before the game starts", GS, "PrecacheTextureReplacements",
							true),
		GS, "LoadTextureReplacements", true)));
	{
		Option o = action("Download an HD texture pack",
			"Looks for this game's pack in the Internet Archive's collection of PCSX2 HD texture packs (archive.org/details/"
			"pcsx2-hd-texture-packs) and puts it on this console or a USB drive, as you choose. It is unpacked as it downloads and used as "
			"soon as it is whole, also in a game that runs. Packs are their makers' work, and large: often several GB",
			[] { be->findTexturePacks(); }, Option::OneGame);
		o.info = [] { return be->texturePackDoing(); };
		rows.push_back(std::move(o));
	}
	{
		Option o = action("Stop the download", "What was unpacked whole stays", [] { be->stopTexturePack(); }, Option::OneGame);
		o.enabled = [](int) { return !be->texturePackDoing().empty(); };
		rows.push_back(std::move(o));
	}
	rows.push_back(hw(toggle("Dump textures", "Writes the game's textures to textures/<serial>/dumps, for making a pack. Slows the game", GS,
		"DumpReplaceableTextures", false, ADV)));
	rows.push_back(when(toggle("Dump mipmaps", "", GS, "DumpReplaceableMipmaps", false, ADV), GS, "DumpReplaceableTextures", false));
	rows.push_back(when(toggle("Dump FMV textures", "", GS, "DumpTexturesWithFMVActive", false, ADV), GS, "DumpReplaceableTextures", false));

	// ---- Post-Processing
	rows.push_back(heading("POST-PROCESSING"));
	rows.push_back(toggle("FXAA", "Smooths jagged edges over the finished picture. Costs little, softens a little", GS, "fxaa", false));
	rows.push_back(adv(list("TV shader", "PCSX2's own simpler looks. Not together with a look below", GS, "TVShader", 0,
		{"None", "Scanlines", "Diagonal", "Triangular", "Wave", "Lottes CRT", "4xRGSS", "NxAGSS"})));
	rows.push_back(strings("Look (shader preset)",
		"Make it look like an old television: CRT shaders (the fast ones suit 4K), a composite or S-Video signal, film grain",
		GS, "Look", "",
		{"None", "CRT (fast)", "CRT (fastest)", "CRT (finest; demanding at 4K)", "CRT through a composite signal", "Composite signal",
			"S-Video signal", "Film grain", "Warmer colours"},
		{"", "crt/crt-guest-advanced-fast.slangp", "crt/crt-guest-advanced-fastest.slangp", "crt/crt-guest-advanced.slangp",
			"crux2/crt-guest-advanced-composite.slangp", "crux2/composite.slangp", "crux2/s-video.slangp", "crux2/film-grain.slangp",
			"crux2/colour.slangp"}));
	addRows(rows, be->extraRows("picture", layer));
	rows.push_back(toggle("Shade boost", "Brightness, contrast, gamma and saturation of your own", GS, "ShadeBoost", false));
	rows.push_back(when(range("Brightness", "50 is the game's own", GS, "ShadeBoost_Brightness", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Contrast", "50 is the game's own", GS, "ShadeBoost_Contrast", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Gamma", "50 is the game's own", GS, "ShadeBoost_Gamma", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Saturation", "50 is the game's own", GS, "ShadeBoost_Saturation", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(list("Frame generation",
		"Makes extra pictures between the game's own, like a television's motion smoothing: a game at 30 frames a second is shown at 60. "
		"Adds a little delay; fast movement in front of something else can smear. The last choice also doubles 60 to 120 on a 120 Hz screen",
		GS, "FrameGeneration", 0, {"Off", "30 to 60", "30 to 60, and 60 to 120 at 120 Hz"}));
	const auto fg = [](Option o) { return whenNot(std::move(o), GS, "FrameGeneration", 0, 0); };
	rows.push_back(fg(list("Frame generation method", "AMD FSR 3's optical flow follows small, fast movement better and keeps still things still", GS,
		"FrameGenerationMethod", 0, {"Crux2's own", "AMD FSR 3 optical flow"})));
	{
		Option o = list("Optical flow quality", "Finer costs more of the graphics chip", GS, "FrameGenerationQuality", 1,
			{"Performance", "Balanced", "Quality"});
		o.enabled = [](int l) { return effectiveInt(l, GS, "FrameGeneration", 0) != 0 && effectiveInt(l, GS, "FrameGenerationMethod", 0) == 1; };
		rows.push_back(std::move(o));
	}
	rows.push_back(fg(toggle("No frame generation during videos", "The game's own videos are shown as they are", GS, "FrameGenerationSkipVideos", true)));

	// ---- Advanced
	rows.push_back(heading("ADVANCED", ADV));
	rows.push_back(list("Hardware download mode", "How the emulation waits for pictures a game reads back. With paraLLEl-GS, a read waits for everything "
		"drawn before it: \"Unsynchronised\" does not wait for the card and answers zeros where it has not finished (a game that reads what it "
		"has just drawn gets zeros every time), \"Off\" reads nothing; both faster, both wrong where the game uses what it read", GS, "HWDownloadMode", 0,
		{"Accurate", "Accurate, always whole (fewer readbacks)", "No readbacks (the threads run in step)", "Unsynchronised (not the same twice)",
			"Off (the game reads nothing back)"},
		0, ADV));
	rows.push_back(hw(list("Texture preloading", "Uploads whole textures the first time they are used. Faster in some games; needed for texture packs",
		GS, "texture_preloading", 2, {"Off", "Partial", "Full (hash cache)"}, 0, ADV)));
	rows.push_back(hw(toggle("Extended upscaling multipliers", "Internal resolutions above 12x: far beyond what a 4K screen shows, and very demanding", GS,
		"ExtendedUpscalingMultipliers", false, ADV)));
	{
		Names names;
		std::vector<float> values;
		for (int times = 13; times <= 25; times++)
		{
			names.push_back(std::to_string(times) + "x");
			values.push_back(static_cast<float>(times));
		}
		Option o = floats("Internal resolution above 12x", "Takes the place of the internal resolution under Rendering", GS, "upscale_multiplier", 1.f,
			names, values, ADV);
		o.enabled = [hardware](int l) { return hardware(l) && effective(l, GS, "ExtendedUpscalingMultipliers", false); };
		rows.push_back(std::move(o));
	}
	rows.push_back(list("Override texture barriers", "For draws that read their own target", GS, "OverrideTextureBarriers", -1, {"Automatic", "Off", "On"}, -1, ADV));
	rows.push_back(toggle("Extra barriers with the ordered view", "For games with glitches under Rasterizer ordered view", GS, "HWROVBarriersVK", false, ADV));
	rows.push_back(toggle("Disable shader cache", "Shaders are not kept on disk: every start compiles them again", GS, "DisableShaderCache", false,
		ADV | Option::Restart));
	rows.push_back(toggle("Disable vertex shader expand", "Sprites are expanded on the processor, not in a vertex shader", GS, "DisableVertexShaderExpand",
		false, ADV | Option::Restart));
	rows.push_back(toggle("Spin CPU during readbacks", "Stops the processor lowering its clock while it waits", GS, "HWSpinCPUForReadbacks", false, ADV));
	rows.push_back(toggle("Spin GPU during readbacks", "The same, for the graphics chip", GS, "HWSpinGPUForReadbacks", false, ADV));
	rows.push_back(toggle("Disable framebuffer fetch", "For a driver that gets it wrong", GS, "DisableFramebufferFetch", false,
		ADV | Option::AllGames | Option::Restart));
	rows.push_back(toggle("Use blit swap chain", "For a driver that gets it wrong", GS, "UseBlitSwapChain", false, ADV | Option::AllGames | Option::Restart));
	rows.push_back(frange("NTSC frame rate", "The rate NTSC games run at", GS, "FramerateNTSC", 59.94f, 10, 300, 0.01, 2, " Hz", ADV));
	rows.push_back(frange("PAL frame rate", "The rate PAL games run at", GS, "FrameratePAL", 50.f, 10, 300, 0.01, 2, " Hz", ADV));
	return page;
}

// ------------------------------------------------------------------------------- On-Screen Display

Page osdPage()
{
	Page page{ICON_FA_MESSAGE, "On-Screen Display", {}};
	auto& rows = page.rows;
	const unsigned A = Option::AllGames;
	const unsigned F = Option::Front;
	const Names places = {"Not shown", "Top left", "Top centre", "Top right", "Centre left", "Centre", "Centre right", "Bottom left", "Bottom centre",
		"Bottom right"};
	rows.push_back(heading("ON-SCREEN DISPLAY"));
	rows.push_back(range("OSD scale", "The size of the figures and messages", GS, "OsdScale", 100, 50, 500, 5, "%", A));
	rows.push_back(adv(frange("OSD margin", "How far from the screen's edges they are", GS, "OsdMargin", 10.f, 0, 100, 1, 0, " px", A)));
	rows.push_back(adv(list("OSD messages position", "Where the emulator's messages are shown", GS, "OsdMessagesPos", 0, places, 0, A)));
	rows.push_back(adv(list("OSD performance position", "Where the figures chosen below are shown", GS, "OsdPerformancePos", 3, places, 0, A)));
	rows.push_back(toggle("Bold figures", "Heavier letters, easier to read from a sofa", GS, "OsdBoldText", true, A));
	rows.push_back(toggle("Move the figures from corner to corner", "The figures over a game go to another corner every five minutes", FRONT, "OsdMove",
		false, F));
	rows.push_back(list("Style", "Crux2's panel: the figures in sections (graphics chip, processor, frames) with a graph of the frame times. "
		"PCSX2's: its own lines of text", "PS5", "OsdStyle", 0, {"Crux2's panel", "PCSX2's lines"}, 0, A));
	rows.push_back(heading("SHOW"));
	rows.push_back(toggle("Speed percentages", "The speed of the emulation, in percent", GS, "OsdShowSpeed", false, A));
	rows.push_back(toggle("FPS", "The frames the game itself shows (and the pictures shown, where frame generation makes more)", GS,
		"OsdShowFPS", false, A));
	rows.push_back(adv(toggle("VPS", "The console's own refreshes a second", GS, "OsdShowVPS", false, A)));
	rows.push_back(toggle("Resolution", "The resolution the game is drawn at", GS, "OsdShowResolution", false, A));
	rows.push_back(toggle("CPU usage", "How busy the emulation, graphics and VU threads are", GS, "OsdShowCPU", false, A));
	rows.push_back(toggle("GPU usage", "", GS, "OsdShowGPU", false, A));
	rows.push_back(toggle("Frame times", "A graph of how long frames take", GS, "OsdShowFrameTimes", false, A));
	rows.push_back(adv(toggle("Hardware info", "The processor and graphics chip", GS, "OsdShowHardwareInfo", false, A)));
	rows.push_back(adv(toggle("Statistics", "Draws, primitives and the like of a frame", GS, "OsdShowGSStats", false, A)));
	rows.push_back(adv(toggle("GPU statistics", "What the graphics chip did in a frame", GS, "OsdShowGPUStats", false, A)));
	rows.push_back(toggle("Indicators", "Marks for pause, fast forward and slow motion", GS, "OsdShowIndicators", false, A));
	rows.push_back(adv(toggle("Settings", "The settings in effect, in the bottom right corner", GS, "OsdShowSettings", false, A)));
	rows.push_back(adv(toggle("Patches", "The patches in effect, with the settings", GS, "OsdshowPatches", false, A)));
	rows.push_back(adv(toggle("Inputs", "What is pressed, in the bottom left corner", GS, "OsdShowInputs", false, A)));
	rows.push_back(adv(toggle("Texture replacements", "How many were loaded and dumped", GS, "OsdShowTextureReplacements", true, A)));
	rows.push_back(toggle("Crux2 version", "Crux2's version and build, and the PCSX2 it is built on, beside the other figures", GS,
		"OsdShowVersion", false, A));
	return page;
}

// ------------------------------------------------------------------------------- Audio

Page audioPage(int layer)
{
	Page page{ICON_FA_VOLUME_HIGH, "Audio", {}};
	auto& rows = page.rows;
	constexpr const char* OUT = "SPU2/Output";
	const unsigned A = Option::AllGames;
	const unsigned ADV = Option::Expert | A; // the stretcher's and expansion's tuning
	rows.push_back(heading("CONFIGURATION"));
	rows.push_back(adv(strings("Synchronization",
		"Time stretching plays the sound slower or faster with the game, without changing its pitch: no crackling when a game slows down", OUT,
		"SyncMode", "TimeStretch", {"None (the sound breaks up)", "Time stretching"}, {"Disabled", "TimeStretch"})));
	rows.push_back(strings("Expansion",
		"Makes more channels out of a game's stereo sound for a 5.1 or 7.1 set. Needs the console's sound output set to that many channels; "
		"if the console refuses, the sound stays stereo",
		OUT, "ExpansionMode", "Disabled", {"Off (stereo)", "Stereo with bass channel", "Four channels", "Four channels with bass", "5.1", "7.1"},
		{"Disabled", "StereoLFE", "Quadraphonic", "QuadraphonicLFE", "Surround51", "Surround71"}, A | Option::NextStart));
	rows.push_back(adv(range("Buffer size", "How much sound is held ready. More is safer against crackling and adds delay", OUT, "BufferMS", 50, 10, 500, 5, " ms",
		A)));
	rows.push_back(listOf("Output latency",
		"How much sound the console is handed at a time. The smallest is the least delay; more can help where the sound crackles though the "
		"game holds its speed",
		"PS5", "AudioGrain", 256, {"Smallest (5 ms)", "11 ms", "16 ms", "21 ms", "32 ms", "43 ms"}, {256, 512, 768, 1024, 1536, 2048},
		A | Option::NextStart));

	rows.push_back(heading("CONTROLS"));
	rows.push_back(range("Standard volume", "The volume at normal speed", OUT, "StandardVolume", 100, 0, 200, 5, "%"));
	rows.push_back(range("Fast forward volume", "", OUT, "FastForwardVolume", 100, 0, 200, 5, "%"));
	rows.push_back(toggle("Mute all sound", "No sound at all", OUT, "OutputMuted", false));

	// PCSX2's two dialogs ("Stretch settings", "Expansion settings"), as rows: advanced.
	const auto stretch = [](Option o) {
		o.enabled = [](int l) {
			const std::optional<std::string> own = (l == Own) ? be->getString(Own, "SPU2/Output", "SyncMode") : std::nullopt;
			return own.value_or(be->getString(Shared, "SPU2/Output", "SyncMode").value_or("TimeStretch")) == "TimeStretch";
		};
		return o;
	};
	rows.push_back(heading("TIME STRETCH SETTINGS", ADV));
	rows.push_back(stretch(range("Sequence length", "", OUT, "StretchSequenceLengthMS", 30, 20, 100, 1, " ms", ADV | Option::NextStart)));
	rows.push_back(stretch(range("Seekwindow size", "", OUT, "StretchSeekWindowMS", 20, 10, 30, 1, " ms", ADV | Option::NextStart)));
	rows.push_back(stretch(range("Overlap", "", OUT, "StretchOverlapMS", 10, 5, 15, 1, " ms", ADV | Option::NextStart)));
	rows.push_back(stretch(toggle("Use quickseek", "", OUT, "StretchUseQuickSeek", false, ADV | Option::NextStart)));
	rows.push_back(stretch(toggle("Use anti-aliasing filter", "", OUT, "StretchUseAAFilter", false, ADV | Option::NextStart)));

	const auto expand = [](Option o) {
		o.enabled = [](int l) {
			const std::optional<std::string> own = (l == Own) ? be->getString(Own, "SPU2/Output", "ExpansionMode") : std::nullopt;
			return own.value_or(be->getString(Shared, "SPU2/Output", "ExpansionMode").value_or("Disabled")) != "Disabled";
		};
		return o;
	};
	const unsigned E = ADV | Option::NextStart;
	rows.push_back(heading("EXPANSION SETTINGS", ADV));
	rows.push_back(expand(listOf("Block size", "", OUT, "ExpandBlockSize", 2048, {"128", "256", "512", "1024", "2048", "4096", "8192"},
		{128, 256, 512, 1024, 2048, 4096, 8192}, E)));
	rows.push_back(expand(frange("Circular wrap", "", OUT, "ExpandCircularWrap", 90.f, 0, 360, 1, 0, " deg", E)));
	rows.push_back(expand(frange("Shift", "", OUT, "ExpandShift", 0.f, -1, 1, 0.05, 2, "", E)));
	rows.push_back(expand(frange("Depth", "", OUT, "ExpandDepth", 1.f, 0, 5, 0.1, 1, "", E)));
	rows.push_back(expand(frange("Focus", "", OUT, "ExpandFocus", 0.f, -1, 1, 0.05, 2, "", E)));
	rows.push_back(expand(frange("Center image", "", OUT, "ExpandCenterImage", 1.f, 0, 1, 0.05, 2, "", E)));
	rows.push_back(expand(frange("Front separation", "", OUT, "ExpandFrontSeparation", 1.f, 0, 10, 0.1, 1, "", E)));
	rows.push_back(expand(frange("Rear separation", "", OUT, "ExpandRearSeparation", 1.f, 0, 10, 0.1, 1, "", E)));
	rows.push_back(expand(range("Low cutoff", "", OUT, "ExpandLowCutoff", 40, 0, 100, 1, "", E)));
	rows.push_back(expand(range("High cutoff", "", OUT, "ExpandHighCutoff", 90, 0, 100, 1, "", E)));
	return page;
}

// ------------------------------------------------------------------------------- Memory Cards

Page memoryCardsPageOf(int layer, bool forGame)
{
	Page page{ICON_FA_SD_CARD, "Memory Cards", {}};
	auto& rows = page.rows;
	addPageRows(rows, be->memoryCardsPage(layer), "MEMORY CARDS");
	if (!forGame)
		addPageRows(rows, be->backupPage(), "BACKUPS");
	return page;
}

// ------------------------------------------------------------------------------- Advanced

Page advancedPage(int layer)
{
	Page page{ICON_FA_SCREWDRIVER_WRENCH, "Advanced", {}};
	auto& rows = page.rows;
	const unsigned A = Option::AllGames;
	const unsigned f = Option::Expert;
	const Names rounding = {"Nearest", "Negative", "Positive", "Chop / zero"};

	rows.push_back(heading("EMOTIONENGINE (MIPS-IV)", f));
	rows.push_back(list("Rounding mode", "Chop is the PS2's; some games need another", CPU, "FPU.Roundmode", 3, rounding, 0, f));
	rows.push_back(list("Division rounding mode", "Nearest suits most games", CPU, "FPUDiv.Roundmode", 0, rounding, 0, f));
	rows.push_back(clamping("Clamping mode", "How numbers out of range are handled. Normal suits most games", "fpuOverflow", "fpuExtraOverflow",
		"fpuFullMode", {"None", "Normal", "Extra, keep the sign", "Full"}, f));
	rows.push_back(toggle("Enable recompiler", "Off is many times slower", REC, "EnableEE", true, f));
	rows.push_back(toggle("Enable cache (slow)", "With the interpreter only; for diagnostics", REC, "EnableEECache", false, f));
	rows.push_back(toggle("INTC spin detection", "A large speed-up in some games, with next to no side effects", SPEED, "IntcStat", true, f));
	rows.push_back(toggle("Wait loop detection", "A moderate speed-up in some games, with no known side effects", SPEED, "WaitLoop", true, f));
	rows.push_back(toggle("Enable fast memory access", "", REC, "EnableFastmem", true, f));
	rows.push_back(toggle("Pause on TLB miss", "Pauses where the game would otherwise carry on", REC, "PauseOnTLBMiss", false, f));
	rows.push_back(toggle("Enable 128MB RAM (dev console)", "128 MB, as a development console had", CPU, "ExtraMemory", false, f | Option::NextStart));

	rows.push_back(heading("VECTOR UNITS (VU)", f));
	rows.push_back(list("VU0 rounding mode", "", CPU, "VU0.Roundmode", 3, rounding, 0, f));
	rows.push_back(clamping("VU0 clamping mode", "", "vu0Overflow", "vu0ExtraOverflow", "vu0SignOverflow", {"None", "Normal", "Extra", "Extra, keep the sign"}, f));
	rows.push_back(list("VU1 rounding mode", "", CPU, "VU1.Roundmode", 3, rounding, 0, f));
	rows.push_back(clamping("VU1 clamping mode", "", "vu1Overflow", "vu1ExtraOverflow", "vu1SignOverflow", {"None", "Normal", "Extra", "Extra, keep the sign"}, f));
	rows.push_back(toggle("Enable VU0 recompiler (micro mode)", "", REC, "EnableVU0", true, f));
	rows.push_back(toggle("Enable VU1 recompiler", "", REC, "EnableVU1", true, f));
	rows.push_back(toggle("mVU flag hack", "A good speed-up that suits nearly all games", SPEED, "vuFlagHack", true, f));
	rows.push_back(toggle("Instant VU1", "A modest speed-up; a few games show glitches", SPEED, "vu1Instant", true, f));

	rows.push_back(heading("I/O PROCESSOR (IOP, MIPS-I)", f));
	rows.push_back(toggle("Enable IOP recompiler", "", REC, "EnableIOP", true, f));

	rows.push_back(heading("GAME SETTINGS", f));
	rows.push_back(toggle("Enable game fixes", "The fixes known for each game, applied when it starts", CORE, "EnableGameFixes", true, f));
	rows.push_back(toggle("Enable compatibility patches", "The patches known for each game, applied when it starts", CORE, "EnablePatches", true, f));
	rows.push_back(toggle("Recommended picture settings",
		"Games the database marks as needing a higher blending accuracy, accurate alpha test or AA1 get it. A value set in a game's own "
		"options wins",
		"PS5", "GameDbRecommended", true, f | A));

	rows.push_back(heading("SAVESTATE SETTINGS"));
	rows.push_back(toggle("Create save state backups",
		"When a state is saved to a slot that has one, the old one is kept as that slot's backup", CORE, "BackupSavestate", true, A));
	rows.push_back(listOf("Save a state by itself",
		"While a game is played its state is saved every so often, to a place of its own beside the slots (\"Load the auto-save\" in the "
		"quick menu). The game may stand still for a moment each time",
		"PS5", "AutoSaveMinutes", 0, {"Off", "Every 2 minutes", "Every 5 minutes", "Every 10 minutes", "Every 15 minutes"}, {0, 2, 5, 10, 15}, A));
	rows.push_back(toggle("Undo for a loaded state",
		"Before a state is loaded into a running game, the game as it is, is kept: \"Undo the last load\" in the quick menu brings it back", "PS5",
		"UndoLoad", true, A));
	addRows(rows, be->extraRows("saves", layer));
	rows.push_back(list("Compression method", "", CORE, "SavestateCompressionType", 2, {"Uncompressed", "Deflate64", "Zstandard"}, 0, A | f));
	rows.push_back(list("Compression level", "", CORE, "SavestateCompressionRatio", 1, {"Low (fast)", "Medium", "High", "Very high (slow)"}, 0, A | f));
	return page;
}

// ------------------------------------------------------------------------------- Debug

Page debugPage()
{
	Page page{ICON_FA_BUG, "Debug", {}};
	auto& rows = page.rows;
	const unsigned f = Option::Expert | Option::AllGames;
	addPageRows(rows, be->logPage(), "LOGGING");
	rows.push_back(heading("LOGGING IN DETAIL", f));
	rows.push_back(toggle("Log to a file", "The emulator's log, logs/emulog.txt in the title folder", "Logging", "EnableFileLogging", true, f));
	rows.push_back(toggle("Verbose", "Developer messages too", "Logging", "EnableVerbose", false, f));
	rows.push_back(toggle("Timestamps", "A time before each line", "Logging", "EnableTimestamps", true, f));
	rows.push_back(toggle("EE console", "The game's own messages from the Emotion Engine", "Logging", "EnableEEConsole", true, f));
	rows.push_back(toggle("IOP console", "The game's own messages from the I/O processor", "Logging", "EnableIOPConsole", true, f));
	rows.push_back(toggle("CDVD verbose reads", "Every read a game makes of its disc", CORE, "CdvdVerboseReads", false, f));

	// PCSX2's trace log: every kind of event, into the log. Very slow, very large.
	constexpr const char* TRACE = "EmuCore/TraceLog";
	rows.push_back(heading("TRACE LOGGING", f));
	rows.push_back(toggle("Enable trace logging", "The kinds chosen below go into the log as they happen. Slow, and the log grows fast", TRACE, "Enabled",
		false, f));
	const auto trace = [](const char* label, const char* key) {
		return when(toggle(label, "", TRACE, key, false, f), TRACE, "Enabled", false);
	};
	static const std::pair<const char*, const char*> EE_KINDS[] = {{"EE: BIOS", "EE.bios"}, {"EE: memory", "EE.memory"}, {"EE: GIFtags", "EE.giftag"},
		{"EE: VIFcodes", "EE.vifcode"}, {"EE: MSKPATH3", "EE.mskpath3"}, {"EE: R5900", "EE.r5900"}, {"EE: COP0", "EE.cop0"},
		{"EE: COP1 (FPU)", "EE.cop1"}, {"EE: COP2 (VU0 macro)", "EE.cop2"}, {"EE: cache", "EE.cache"}, {"EE: HW registers (MMIO)", "EE.knownhw"},
		{"EE: unknown MMIO", "EE.unknownhw"}, {"EE: DMA registers", "EE.dmahw"}, {"EE: IPU", "EE.ipu"}, {"EE: DMA control", "EE.dmac"},
		{"EE: counters", "EE.counters"}, {"EE: SPR / MFIFO", "EE.spr"}, {"EE: VIF", "EE.vif"}, {"EE: GIF", "EE.gif"}, {"SIF", "MISC.sif"}};
	static const std::pair<const char*, const char*> IOP_KINDS[] = {{"IOP: BIOS", "IOP.bios"}, {"IOP: memory cards", "IOP.memcards"},
		{"IOP: pad", "IOP.pad"}, {"IOP: R3000A", "IOP.r3000a"}, {"IOP: COP2 (GPU)", "IOP.cop2"}, {"IOP: memory", "IOP.memory"},
		{"IOP: HW registers (MMIO)", "IOP.knownhw"}, {"IOP: unknown MMIO", "IOP.unknownhw"}, {"IOP: DMA registers", "IOP.dmahw"},
		{"IOP: DMA control", "IOP.dmac"}, {"IOP: counters", "IOP.counters"}, {"IOP: CDVD", "IOP.cdvd"}, {"IOP: MDEC", "IOP.mdec"}};
	for (const auto& [label, key] : EE_KINDS)
		rows.push_back(trace(label, key));
	for (const auto& [label, key] : IOP_KINDS)
		rows.push_back(trace(label, key));

	// The graphics side's debugging (PCSX2's Debug > GS tab and Graphics > Advanced > Debugging).
	rows.push_back(heading("GRAPHICS DEBUGGING", f));
	rows.push_back(toggle("Use debug blend", "Shows how blending was done, for finding faults", GS, "UseDebugBlend", false, f));
	rows.push_back(list("GS dump compression", "How a GS dump (the hotkey) is packed", GS, "GSDumpCompression", 2,
		{"Uncompressed", "LZMA (xz)", "Zstandard (zst)"}, 0, f));
	rows.push_back(toggle("Dump GS draws", "Writes what each draw used and made, into the snapshots folder. Very slow, very large", GS, "DumpGSData",
		false, f));
	const auto dump = [](const char* label, const char* key) { return when(toggle(label, "", GS, key, false, f), GS, "DumpGSData", false); };
	rows.push_back(dump("Save render target", "SaveRT"));
	rows.push_back(dump("Save frame", "SaveFrame"));
	rows.push_back(dump("Save texture", "SaveTexture"));
	rows.push_back(dump("Save depth", "SaveDepth"));
	rows.push_back(dump("Save alpha", "SaveAlpha"));
	rows.push_back(dump("Save info", "SaveInfo"));
	rows.push_back(dump("Save transfer image data", "SaveTransferImages"));
	rows.push_back(dump("Save draw stats", "SaveDrawStats"));
	rows.push_back(dump("Save frame stats", "SaveFrameStats"));
	rows.push_back(dump("Save HW config", "SaveHWConfig"));
	rows.push_back(when(range("Save draw start", "", GS, "SaveDrawStart", 0, 0, 100000, 1, "", f), GS, "DumpGSData", false));
	rows.push_back(when(range("Save draw count", "", GS, "SaveDrawCount", 5000, 0, 100000, 1, "", f), GS, "DumpGSData", false));
	rows.push_back(when(range("Save frame start", "", GS, "SaveFrameStart", 0, 0, 100000, 1, "", f), GS, "DumpGSData", false));
	rows.push_back(when(range("Save frame count", "-1: every frame", GS, "SaveFrameCount", -1, -1, 100000, 1, "", f), GS, "DumpGSData", false));
	return page;
}

// ------------------------------------------------------------------------------- PlayStation 5

// What Crux2 adds that PCSX2 has no page for: the console's own tuning, native code, updates.
Page ps5Page(int layer)
{
	Page page{ICON_FA_DESKTOP, "PlayStation 5", {}};
	auto& rows = page.rows;
	const unsigned A = Option::Expert | Option::AllGames;
	rows.push_back(heading("PS5 TUNING", A));
	rows.push_back(toggle("Sound and controls before the emulator",
		"The console runs sound, the controllers and the menus ahead of the emulator's threads, and downloads, copies and unpacking "
		"after them: no crackle or late buttons while the emulator is busy. Off: every thread is equal",
		"PS5", "ThreadPriorities", false, A | Option::Restart));
	rows.push_back(toggle("Build shaders quickly first",
		"A new effect is first drawn with a quickly built shader, swapped for the optimised one a frame later: fewer hitches the first "
		"time an effect is seen",
		"PS5", "FastPipelines", true, A | Option::Restart));

	rows.push_back(heading("NATIVE CODE MODULES (EXPERIMENTAL)", A));
	rows.push_back(toggle("Use native code modules",
		"Runs a game's code from a module you built on your PC from your own copy of the game (the recomp kit), put in the data "
		"folder's native folder or on the share. Code the module lacks runs as usual",
		"PS5", "NativeModules", true, A | Option::NextStart));
	const auto native = [](Option o) { return when(std::move(o), "PS5", "NativeModules", true); };
	rows.push_back(native(toggle("Put executables on the share",
		"A game started without a module copies its executable to Crux2-native/executables on its share, for the kit's native-batch.sh; "
		"the next start picks up the new module from Crux2-native/modules",
		"PS5", "NativeExport", true, A | Option::NextStart)));
	rows.push_back(native(toggle("Native code may use hardware",
		"Off: module code that talks to the console's hardware is left to PCSX2. For a game that hangs with its module", "PS5", "NativeHardware",
		true, A | Option::NextStart)));
	rows.push_back(native(toggle("Check native code",
		"Runs everything twice and compares with PCSX2's interpreter, naming each function that differs in the log. Slow: for finding "
		"faults, not for playing",
		"PS5", "NativeCheck", false, A | Option::NextStart)));

	addRows(rows, be->extraRows("system", layer));
	addPageRows(rows, be->aboutPage(), "ABOUT");
	return page;
}

// What the library holds and what was played, counted when the page is made.
Page statisticsPage()
{
	Page page{ICON_FA_CHART_BAR, "Statistics", {}};
	auto& rows = page.rows;
	const std::vector<GameInfo> games = be->games();
	int kinds[3] = {}, sources[3] = {}, regions[4] = {}, runs[7] = {};
	int played = 0, favourites = 0, hidden = 0, states = 0, cheats = 0, textures = 0;
	uint64_t bytes = 0;
	time_t seconds = 0, latest = 0;
	std::string latestTitle;
	for (const GameInfo& g : games)
	{
		kinds[std::clamp(static_cast<int>(g.kind), 0, 2)]++;
		sources[std::clamp(g.source, 0, 2)]++;
		const std::string& r = g.region;
		regions[r.compare(0, 6, "NTSC-U") == 0 ? 0 : r.compare(0, 3, "PAL") == 0 ? 1 : r.compare(0, 6, "NTSC-J") == 0 ? 2 : 3]++;
		runs[std::clamp(g.compatibility, 0, 6)]++;
		played += (g.lastPlayed > 0 || g.playedSeconds > 0) ? 1 : 0;
		favourites += g.favourite ? 1 : 0;
		hidden += g.hidden ? 1 : 0;
		states += g.hasStates ? 1 : 0;
		cheats += g.hasCheats ? 1 : 0;
		textures += g.hasTextures ? 1 : 0;
		bytes += g.size;
		seconds += g.playedSeconds;
		if (g.lastPlayed > latest)
		{
			latest = g.lastPlayed;
			latestTitle = g.title;
		}
	}
	const auto line = [&](const char* label, std::string value) { rows.push_back(info(label, [value] { return value; })); };
	const auto count = [](int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
	rows.push_back(heading("THE LIBRARY"));
	{
		std::string all = count(static_cast<int>(games.size()), "game", "games");
		if (kinds[1] + kinds[2] > 0)
			all += ":  " + std::to_string(kinds[0]) + " PlayStation 2, " + std::to_string(kinds[1]) + " PlayStation, " + count(kinds[2], "program", "programs");
		line("Games", all);
	}
	line("Where they are",
		std::to_string(sources[0]) + " in the title folder, " + std::to_string(sources[1]) + " on USB drives, " + std::to_string(sources[2]) + " on shares");
	line("Their size", bytes != 0 ? formatSize(bytes) : std::string("Not known"));
	line("By region", std::to_string(regions[0]) + " America, " + std::to_string(regions[1]) + " Europe, " + std::to_string(regions[2]) + " Japan, " +
						  std::to_string(regions[3]) + " other or not known");
	line("Known to run", std::to_string(runs[6]) + " perfect, " + std::to_string(runs[5]) + " playable, " + std::to_string(runs[4] + runs[3] + runs[2] + runs[1]) +
							 " less than that, " + std::to_string(runs[0]) + " not known");
	line("Marked", count(favourites, "favourite", "favourites") + ", " + std::to_string(hidden) + " hidden");
	line("With something of their own",
		std::to_string(states) + " with saved states, " + std::to_string(cheats) + " with cheats, " + std::to_string(textures) + " with a texture pack");
	rows.push_back(heading("PLAYED"));
	line("Games played", std::to_string(played) + " of " + std::to_string(games.size()));
	line("Time played in all", seconds > 0 ? formatPlayed(seconds) : std::string("None yet"));
	line("Played last", latest > 0 ? latestTitle + "   \xc2\xb7   " + formatWhen(latest) : std::string("Nothing yet"));
	std::vector<const GameInfo*> most;
	for (const GameInfo& g : games)
	{
		if (g.playedSeconds > 0)
			most.push_back(&g);
	}
	std::sort(most.begin(), most.end(), [](const GameInfo* a, const GameInfo* b) { return a->playedSeconds > b->playedSeconds; });
	if (!most.empty())
		rows.push_back(heading("PLAYED THE LONGEST"));
	static const char* const placesNames[] = {"First", "Second", "Third", "Fourth", "Fifth"};
	for (size_t i = 0; i < most.size() && i < 5; i++)
		line(placesNames[i], most[i]->title + "   \xc2\xb7   " + formatPlayed(most[i]->playedSeconds));
	return page;
}

} // namespace

// The pages of the settings, in their order: for every game, or for the game opened.
std::vector<Page> settingsPages(bool forGame)
{
	const int layer = forGame ? Own : Shared;
	std::vector<Page> pages;
	const auto network = [layer] {
		Page page = be->networkPage(layer);
		page.icon = ICON_FA_NETWORK_WIRED;
		page.name = "Network & HDD";
		return page;
	};
	if (forGame)
	{
		pages.push_back(emulationPage(layer, true));
		{
			Page patches{ICON_FA_BANDAGE, "Patches", {}};
			addPageRows(patches.rows, be->patchesPage(false), "PATCHES");
			pages.push_back(std::move(patches));
			Page cheats{ICON_FA_BOLT, "Cheats", {}};
			addPageRows(cheats.rows, be->patchesPage(true), "CHEATS");
			pages.push_back(std::move(cheats));
		}
		pages.push_back(gameFixesPage());
		pages.push_back(graphicsPage(layer, true));
		pages.push_back(audioPage(layer));
		pages.push_back(memoryCardsPageOf(layer, true));
		pages.push_back(network());
		pages.push_back(be->controllersPage(layer));
		pages.push_back(biosPageOf(layer, true));
		return pages;
	}

	pages.push_back(interfacePage());
	pages.push_back(gameListPage());
	pages.push_back(biosPageOf(layer, false));
	pages.push_back(emulationPage(layer, false));
	pages.push_back(graphicsPage(layer, false));
	pages.push_back(osdPage());
	pages.push_back(audioPage(layer));
	pages.push_back(memoryCardsPageOf(layer, false));
	{
		Page net = network();
		addRows(net.rows, be->extraRows("network", layer));
		pages.push_back(std::move(net));
	}
	{
		Page achievements = be->achievementsPage();
		if (!achievements.rows.empty())
		{
			achievements.icon = ICON_FA_TROPHY;
			achievements.name = "Achievements";
			pages.push_back(std::move(achievements));
		}
	}
	{
		Page controls = be->controllersPage(layer);
		addRows(controls.rows, be->extraRows("controllers", layer));
		addPageRows(controls.rows, be->hotkeysPage(), "HOTKEYS");
		pages.push_back(std::move(controls));
	}
	pages.push_back(advancedPage(layer));
	pages.push_back(debugPage());
	pages.push_back(ps5Page(layer));
	pages.push_back(statisticsPage());
	return pages;
}

} // namespace bp
