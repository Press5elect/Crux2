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
		"How many settings are shown. Normal covers what most people need, Advanced adds finer options, and Expert "
			"shows everything, including options that can stop games from working. Press Triangle anywhere in Settings to "
			"switch.",
		FRONT, "SettingsLevel", 0, {"Normal", "Advanced", "Expert"}, 0, Option::Front));
	rows.push_back(toggle("Pause when a controller disconnects", "Pauses the emulator when a controller with bindings is disconnected.", "UI",
		"PauseOnControllerDisconnection", false, A));
	rows.push_back(toggle("Warn about unsafe settings", "Displays warnings when settings are enabled which may break games.", CORE,
		"WarnAboutUnsafeSettings", true, A));
	rows.push_back(toggle("Circle chooses, Cross goes back",
		"Swaps the Cross and Circle buttons in all menus, as on Japanese consoles.",
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
		"Changes the colours and style of the interface. Carbon uses a black background, which suits OLED screens.",
		FRONT, "Look", 0, skinNames, 0, F));
	rows.push_back(list("Accent colour", "The colour used to highlight the selected item and switched-on options.", FRONT, "Accent", 0, accentNames, 0, F));
	rows.push_back(list("Background",
		"What is shown behind the menus: a still or animated background, the selected game's artwork, or your own "
			"pictures from the \"backgrounds\" folder.",
		FRONT, "Backdrop", 0, backNames, 0, F));
	rows.push_back(adv(whenInt(listOf("My pictures: each stays for", "How long each of your pictures is shown before the next one.", FRONT, "WallEvery", 60,
							   {"30 seconds", "1 minute", "2 minutes", "5 minutes", "15 minutes", "1 hour"}, {30, 60, 120, 300, 900, 3600}, F),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1)));
	rows.push_back(adv(whenInt(listOf("My pictures: made darker by", "Darkens your pictures so the text on top of them stays readable.", FRONT, "WallDim", 50,
							   {"Nothing", "25%", "40%", "50%", "60%", "75%"}, {0, 25, 40, 50, 60, 75}, F),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1)));
	rows.push_back(whenInt(action("My pictures: look into the folder again", "Checks the folder again for new pictures.",
							   [] {
								   const int found = be->wallpapers(true);
								   toast(found == 0 ? std::string("No picture in the folder \"backgrounds\" of the title folder (PNG or JPEG).") :
													  std::to_string(found) + (found == 1 ? " picture." : " pictures."),
									   4.f);
							   }),
		FRONT, "Backdrop", 0, static_cast<int>(BackOwn) + 1));
	rows.push_back(list("Motion", "How much the interface animates. All: animated background and sliding menus. Less: fades only. None: no "
		"animation.",
		FRONT, "Motion", 0, {"All", "Less", "None"}, 0, F));
	rows.push_back(toggle("Start-up animation",
		"Plays the animated logo when Crux2 starts. Press any button to skip it.", FRONT, "Splash", true, F));
	rows.push_back(when(list("Which start-up", "Chooses which of the four start-up animations plays. Random picks a different one each time.",
							FRONT, "SplashKind", 0, {"Another one each start", "Strike", "Unfold", "Spin", "Slash"}, 0, F),
		FRONT, "Splash", true));
	rows.push_back(listOf("Interface size",
		"Makes the whole interface smaller so more fits on screen.",
		FRONT, "InterfaceSize", 100, {"100%", "90%", "80%", "70%"}, {100, 90, 80, 70}, F));
	rows.push_back(listOf("Text size", "Makes the text larger or smaller without changing the rest of the interface.", FRONT, "TextSize", 0,
		{"Smallest", "Smaller", "Normal", "Larger", "Largest"}, {-2, -1, 0, 1, 2}, F));
	rows.push_back(toggle("High contrast", "Makes text brighter and outlines stronger.", FRONT, "HighContrast", false, F));
	rows.push_back(toggle("Colours for colour blindness", "Uses blue and orange instead of green and red to show success and failure.", FRONT,
		"ColourBlind", false, F));

	addRows(rows, be->extraRows("interface", Shared));

	rows.push_back(heading("CARE FOR THE SCREEN"));
	rows.push_back(listOf("Go dark when nothing is pressed",
		"Dims the screen after this long without input, then turns it black. Press any button to wake it.", FRONT,
		"IdleDim", 3, {"Never", "After 1 minute", "After 2 minutes", "After 3 minutes", "After 5 minutes", "After 10 minutes"}, {0, 1, 2, 3, 5, 10}, F));
	{
		Option o = toggle("Then the black screen with the mark",
			"Shows a black screen with a moving logo after the screen dims. Off: the screen only dims.", FRONT, "ScreenSaver", true, F);
		o.enabled = [](int) { return be->getInt(Shared, FRONT, "IdleDim").value_or(3) != 0; };
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(toggle("Move everything a little",
		"Moves the whole picture by a few pixels every couple of minutes to reduce the risk of burn-in on OLED screens.", FRONT, "PixelShift", true, F)));
	rows.push_back(adv(toggle("Fade the hints", "Fades the button hints at the bottom of the screen when nothing has been pressed for a while.", FRONT,
		"HintFade", true, F)));
	rows.push_back(listOf("Darken the game when nothing is pressed",
		"Dims the game after this long without input. Pressing any button brings it back.",
		FRONT, "GameDim", 0, {"Never", "After 5 minutes", "After 10 minutes", "After 15 minutes", "After 30 minutes"}, {0, 5, 10, 15, 30}, F));
	return page;
}

// ------------------------------------------------------------------------------- Game List

Page gameListPage()
{
	Page page{ICON_FA_FOLDER_OPEN, "Game List", {}};
	auto& rows = page.rows;
	rows.push_back(heading("GAME SCANNING"));
	rows.push_back(action("Scan for games", "Looks for new games in the games folder, on USB drives and on network shares.",
		[] {
			be->scan(false);
			toast("Looking for new games...", 3.f);
		},
		Option::NoGame));
	rows.push_back(action("Scan again from nothing",
		"Rescans all game files, not just new ones. Use it if a game shows the wrong title. Can take a while on a "
			"network share.",
		[] {
			be->scan(true);
			toast("Reading every game again...", 3.f);
		},
		Option::NoGame));
	rows.push_back(heading("THE LIST"));
	rows.push_back(list("Library view",
		"How games are shown in the library. You can also switch views with the touch pad.",
		FRONT, "View", 0, {"Shelves", "Grid", "List", "Flow", "Bookcase", "Table", "Showcase", "Wheel", "Wall", "Discs"}, 0, Option::Front));
	rows.push_back(toggle("A picture of play behind the shelves",
		"Shows a screenshot of the selected game behind the shelves.", FRONT, "HeroPlay", true, Option::Front));
	rows.push_back(toggle("Group the discs of a game",
		"Shows a game that has several discs as one entry. It starts from the disc you played last.", FRONT, "GroupDiscs", true,
		Option::Front));
	rows.push_back(toggle("English titles", "For games with both a title in the game's native language and one in English, prefer the English title.", "UI",
		"PreferEnglishGameList", false, Option::AllGames));
	rows.push_back(action("Fetch the games' pictures",
		"Downloads title screens and screenshots for your games from libretro's collection.",
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
			"Starts the PS2 BIOS without a game, to use the memory card browser and System Configuration.",
			[] { startPs2Menu(); }, Option::NoGame));
	}
	addPageRows(rows, be->biosPage(layer), "BIOS SELECTION");
	// The language a game is told the console has (Crux2): many European games pick theirs from it.
	// Not written to the BIOS's own settings; for one game, in its options.
	rows.push_back(heading("LANGUAGE"));
	rows.push_back(list("Console language",
		"The language games are told the PS2 is set to. Many European games use it to choose their language. Your BIOS "
			"settings are not changed.",
		"PS5", "ConsoleLanguage", -1,
		{"As the BIOS has it", "Japanese", "English", "French", "Spanish", "German", "Italian", "Dutch", "Portuguese"}, -1,
		Option::NextStart));
	if (!forGame)
	{
		rows.push_back(heading("OPTIONS AND PATCHES"));
		rows.push_back(toggle("Fast boot", "Patches the BIOS to skip the console's boot animation.", CORE, "EnableFastBoot", true));
		rows.push_back(adv(when(toggle("Fast-forward to the game", "Removes emulation speed throttle until the game starts to reduce startup time.", CORE, "EnableFastBootFastForward",
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
	rows.push_back(floats("Normal speed", "Sets the speed when running without fast forwarding.", "Framerate", "NominalScalar", 1.f,
		SPEEDS, SPEED_VALUES));
	rows.push_back(floats("Fast-forward speed", "Sets the speed when using the fast forward hotkey.", "Framerate", "TurboScalar", 2.f,
		SPEEDS, SPEED_VALUES));
	rows.push_back(floats("Slow-motion speed", "Sets the speed when using the slow motion hotkey.", "Framerate", "SlomoScalar", 0.5f, SPEEDS,
		SPEED_VALUES));

	rows.push_back(heading("SYSTEM SETTINGS"));
	rows.push_back(list("EE cycle rate",
		"Underclocks or overclocks the emulated Emotion Engine CPU.",
		SPEED, "EECycleRate", 0, {"50%", "60%", "75%", "100% (the PS2's own)", "130%", "180%", "300%"}, -3));
	rows.push_back(expert(list("EE cycle skipping", "Makes the emulated Emotion Engine skip cycles. Helps a small subset of games like SOTC. Most of the time it's "
		"harmful to performance.", SPEED, "EECycleSkip", 0,
		{"Off", "Mild", "Moderate", "Maximum"})));
	rows.push_back(adv(toggle("Multi-threaded VU1 (MTVU)",
		"Generally a speedup on CPUs with 4 or more cores. Safe for most games, but a few are incompatible and may "
			"hang.", SPEED, "vuThread", false)));
	{
		// One choice for two switches: PCSX2's pinning and Crux2's own layout (which wins when both
		// are on, so that state is not offered).
		Option o;
		o.kind = Option::Combo;
		o.label = "Thread pinning";
		o.desc = "How the emulator's threads are placed on the CPU cores. Anywhere is best for most games. Try another option if "
			"a game is slow at every resolution.";
		o.section = "PS5";
		o.key = "ThreadLayout";
		o.names = {"Anywhere", "Pinned (PCSX2)", "Cores of their own (Crux2)"};
		o.parts = {{CORE, "EnableThreadPinning", false}, {"PS5", "ThreadLayout", false}};
		o.combos = {{0, 0}, {1, 0}, {0, 1}};
		o.flags = Option::AllGames | Option::NextStart;
		o.flags |= Option::Advanced;
		rows.push_back(std::move(o));
	}
	rows.push_back(toggle("Fast disc access", "Fast disc access, shorter loading times. Check HDLoader compatibility lists for games that are known to have "
		"issues with this.", SPEED, "fastCDVD", false));
	rows.push_back(toggle("Load the whole disc into memory",
		"Loads the disc image into RAM before starting the virtual machine.",
		CORE, "CdvdPrecache", false, Option::NextStart));
	rows.push_back(toggle("Cheats", "Enables loading cheats from pnach files.", CORE, "EnableCheats", false));
	rows.push_back(expert(toggle("Host file system", "Allows games and homebrew to access files and folders directly in the title folder.", CORE, "HostFs", false, Option::Advanced)));

	rows.push_back(heading("FRAME PACING / LATENCY CONTROL"));
	addRows(rows, be->extraRows("motion", layer));
	{
		Option o;
		o.kind = Option::Combo;
		o.label = "Frame pacing";
		o.desc = "By the screen: frames are paced by the display's refresh, the smoothest option. By the clock: for games whose "
			"frame rate the display can't match.";
		o.section = GS;
		o.key = "UseVSyncForTiming";
		o.names = {"By the screen (smoothest)", "By the clock, in step with the screen", "By the clock, at the game's own rate"};
		o.parts = {{GS, "VsyncEnable", true}, {GS, "UseVSyncForTiming", true}, {GS, "SyncToHostRefreshRate", true}};
		o.combos = {{1, 1, 1}, {1, 0, 1}, {1, 0, 0}};
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(list("Frames queued for the display",
		"Sets the number of frames which can be queued.", GS,
		"VsyncQueueSize", 0, {"None (hard sync)", "1 frame", "2 frames", "3 frames"}, 0)));
	{
		Option o = toggle("Queue frames while the game is behind",
			"When a game runs below full speed, lets up to 2 frames wait in the queue so the emulator and the graphics "
				"thread can work at the same time. Can add a little input lag while the game is slow.",
			GS, "PS5AutoFrameQueue", true);
		o.enabled = [](int l) { return effectiveInt(l, GS, "VsyncQueueSize", 0) == 0; };
		rows.push_back(adv(std::move(o)));
	}
	rows.push_back(adv(toggle("Show a frame as it begins",
		"Displays the newest frame immediately at the beginning of the frame raster time, instead of at the end. Can "
			"reduce perceived input lag, but may cause problems with some games, such as Soulcalibur II and Baldur's Gate "
			"Dark Alliance II.", GS, "AdvancedFrameDisplay", false)));
	{
		Option o = toggle("Skip repeated frames",
			"Skips displaying frames that don't change in 25/30fps games. Can improve speed, but increase input lag/make "
				"frame pacing worse.",
			GS, "SkipDuplicateFrames", false);
		o.enabled = [](int l) { return !effective(l, GS, "UseVSyncForTiming", true); };
		rows.push_back(std::move(o));
	}
	rows.push_back(adv(toggle("Black frame insertion",
		"At 120 Hz, inserts a black frame after each frame to reduce motion blur. Makes the picture darker. Can't be "
			"used together with frame generation.",
		GS, "BlackFrameInsertion", false)));

	if (forGame)
	{
		rows.push_back(heading("REAL-TIME CLOCK"));
		const unsigned f = Option::OneGame;
		rows.push_back(toggle("Set the clock by hand", "Manually set a real-time clock to use for the virtual PlayStation 2 instead of using your OS' system clock.", CORE,
			"ManuallySetRealTimeClock", false, f | Option::NextStart));
		Names years;
		for (int year = 2000; year < 2100; year++)
			years.push_back(std::to_string(year));
		rows.push_back(when(list("Year", "Calendar year for the virtual PS2 RTC.", CORE, "RtcYear", 0, years, 0, f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Month", "Month of the year (1-12).", CORE, "RtcMonth", 1, 1, 12, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Day", "Day of the month (1-31).", CORE, "RtcDay", 1, 1, 31, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Hour", "Hour of the day (0-23).", CORE, "RtcHour", 0, 0, 23, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Minute", "Minute of the hour (0-59).", CORE, "RtcMinute", 0, 0, 59, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
		rows.push_back(when(range("Second", "Second of the minute (0-59).", CORE, "RtcSecond", 0, 0, 59, 1, "", f), CORE, "ManuallySetRealTimeClock", false));
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
		"Disables automatic hardware fixes, allowing you to set fixes manually.", GS, "UserHacks", false, f);
	manual.enabled = [](int l) { return !effective(l, GS, "PS5ParallelGS", false); };
	rows.push_back(std::move(manual));
	rows.push_back(fix(list("CPU sprite render size", "The maximum target memory width that will allow the CPU Sprite Renderer to activate on.", GS, "UserHacks_CPUSpriteRenderBW", 0,
		{"Off", "1 (64 wide)", "2 (128)", "3 (192)", "4 (256)", "5 (320)", "6 (384)", "7 (448)", "8 (512)", "9 (576)", "10 (640)"}, 0, f)));
	rows.push_back(fix(list("CPU sprite render level", "Determines filter level for CPU sprite render.", GS, "UserHacks_CPUSpriteRenderLevel", 0,
		{"Sprites only", "Sprites and triangles", "Blended sprites and triangles"}, 0, f)));
	rows.push_back(fix(list("Software CLUT render", "Tries to detect when a game is drawing its own color palette and then renders it in software, instead of on "
		"the GPU.", GS, "UserHacks_CPUCLUTRender", 0,
		{"Off", "Normal", "Aggressive"}, 0, f)));
	rows.push_back(fix(list("GPU target CLUT", "Tries to detect when a game is drawing its own color palette and then renders it on the GPU with special "
		"handling.", GS, "UserHacks_GPUTargetCLUTMode", 0,
		{"Off", "On (exact match)", "On (check inside the target)"}, 0, f)));
	rows.push_back(fix(range("Skip draws from", "Completely skips drawing surfaces from the starting index up to the ending index.", GS, "UserHacks_SkipDraw_Start", 0, 0, 5000, 1, "", f)));
	rows.push_back(fix(range("Skip draws to", "Completely skips drawing surfaces from the starting index up to the ending index.", GS, "UserHacks_SkipDraw_End", 0, 0, 5000, 1, "", f)));
	rows.push_back(fix(list("Auto flush",
		"Forces a primitive flush when a framebuffer is also an input texture. Fixes some processing effects such as "
			"the shadows in the Jak series and radiosity in GTA:SA.", GS, "UserHacks_AutoFlushLevel", 0,
		{"Off", "Sprites only", "All primitives"}, 0, f)));
	rows.push_back(fix(toggle("Framebuffer conversion on the processor", "Convert 4-bit and 8-bit framebuffer on the CPU instead of the GPU.", GS,
		"UserHacks_CPU_FB_Conversion", false, f)));
	rows.push_back(fix(toggle("No depth conversion", "Disable the support of depth buffers in the texture cache.", GS, "UserHacks_DisableDepthSupport", false, f)));
	rows.push_back(fix(toggle("No safe features", "This option disables multiple safe features.", GS, "UserHacks_Disable_Safe_Features", false, f)));
	rows.push_back(fix(toggle("No render fixes", "This option disables game-specific render fixes.", GS, "UserHacks_DisableRenderFixes", false, f)));
	rows.push_back(fix(toggle("Preload frame data", "Uploads GS data when rendering a new frame to reproduce some effects accurately.", GS, "preload_frame_with_gs_data", false, f)));
	rows.push_back(fix(toggle("No partial invalidation", "Removes texture cache entries when there is any intersection, rather than only the intersected areas.", GS, "UserHacks_DisablePartialInvalidation",
		false, f)));
	rows.push_back(fix(list("Texture inside a render target", "Allows the texture cache to reuse as an input texture the inner portion of a previous framebuffer.", GS, "UserHacks_TextureInsideRt", 0,
		{"Off", "Inside the target", "Merge targets"}, 0, f)));
	rows.push_back(fix(toggle("Read targets when closing", "Flushes all targets in the texture cache back to local memory when shutting down.", GS,
		"UserHacks_ReadTCOnClose", false, f)));
	rows.push_back(fix(toggle("Estimate texture region", "Attempts to reduce the texture size when games do not set it themselves (e.g. Snowblind games).", GS,
		"UserHacks_EstimateTextureRegion", false, f)));
	rows.push_back(fix(toggle("Draw buffering", "Attempts to reduce draw calls in games which do heavy context switching for blending purposes.", GS, "UserHacks_DrawBuffering", false, f)));
	rows.push_back(fix(toggle("Rewrite large ST", "Rewrites large texture coordinates to avoid stretched or wrong textures in some games.", GS, "UserHacks_RewriteLargeSTCoords", false, f)));
	rows.push_back(fix(toggle("Target page rearranging", "Rearranges memory pages of a target if the game changes the layout at that current VRAM address.", GS, "UserHacks_TargetPageRearranging",
		false, f)));
	rows.push_back(fix(toggle("Palette conversion on the graphics chip", "When enabled GPU converts colormap-textures, otherwise the CPU will. It is a trade-off between GPU and CPU.", GS, "paltex",
		false, f)));
	rows.push_back(fix(list("Limit depth to 24 bits", "Truncate 32-bit depth values to 24 bits. Helps games struggling with Z-fighting.", GS, "UserHacks_Limit24BitDepth", 0,
		{"Off", "Keep the upper bits", "Keep the lower bits"}, 0, f)));
}

void upscalingFixRows(std::vector<Option>& rows, unsigned f)
{
	const auto fix = [](Option o) {
		o.enabled = [](int l) { return effective(l, GS, "UserHacks", false) && !effective(l, GS, "PS5ParallelGS", false); };
		return o;
	};
	rows.push_back(fix(list("Half pixel offset", "Might fix some misaligned fog, bloom, or blend effect.", GS, "UserHacks_HalfPixelOffset", 0,
		{"Off", "Normal (vertex)", "Special (texture)", "Special (texture, aggressive)", "Align to native", "Align to native, with texture offset"}, 0, f)));
	rows.push_back(fix(list("Native scaling", "Emulates native PS2 coordinate scaling behavior when upscaling to reduce misalignment artifacts and seams in "
		"games that draw custom 2D elements.", GS, "UserHacks_native_scaling", 0,
		{"Off", "Normal", "Aggressive", "Normal (keep the upscale)", "Aggressive (keep the upscale)"}, 0, f)));
	rows.push_back(fix(list("Round sprite", "Adjusts sprite coordinates.", GS, "UserHacks_round_sprite_offset", 0, {"Off", "Half", "Full"}, 0, f)));
	rows.push_back(fix(list("Bilinear dirty upscale", "Can smooth out textures due to be bilinear filtered when upscaling. E.g. Brave sun glare.", GS, "UserHacks_BilinearHack", 0,
		{"Automatic", "Force bilinear", "Force nearest"}, 0, f)));
	rows.push_back(fix(range("Texture offset X", "Adjusts target texture offsets.", GS, "UserHacks_TCOffsetX", 0, -4096, 4096, 1, "", f)));
	rows.push_back(fix(range("Texture offset Y", "Adjusts target texture offsets.", GS, "UserHacks_TCOffsetY", 0, -4096, 4096, 1, "", f)));
	rows.push_back(fix(toggle("Align sprite", "Fixes issues with upscaling (vertical lines) in some games.", GS, "UserHacks_align_sprite_X", false, f)));
	rows.push_back(fix(toggle("Merge sprite", "Replaces post-processing multiple paving sprites by a single fat sprite. It reduces various upscaling lines.", GS, "UserHacks_merge_pp_sprite", false, f)));
	rows.push_back(fix(toggle("Even sprite positions", "Lowers the GS precision to avoid gaps between pixels when upscaling. Fixes the text on Wild Arms games.", GS, "UserHacks_ForceEvenSpritePosition", false, f)));
	rows.push_back(fix(toggle("Palette draws at native resolution", "Forces palette texture draws to render at native resolution.", GS, "UserHacks_NativePaletteDraw",
		false, f)));
}

Page gameFixesPage()
{
	Page page{ICON_FA_TOOLBOX, "Game Fixes", {}};
	auto& rows = page.rows;
	const unsigned f = Option::OneGame;
	rows.push_back(heading("GAME FIXES (SET BY ITSELF FOR KNOWN GAMES)", f));
	rows.push_back(toggle("FPU multiply hack", "For Tales of Destiny.", FIX, "FpuMulHack", false, f));
	rows.push_back(toggle("Software renderer for videos", "Needed for some games with complex FMV rendering.", FIX, "SoftwareRendererFMVHack", false, f));
	rows.push_back(toggle("Skip videos", "Skips videos/FMVs in games to avoid game hanging/freezes.", FIX, "SkipMPEGHack", false, f));
	rows.push_back(toggle("Preload TLB hack", "To avoid TLB miss on Goemon.", FIX, "GoemonTlbHack", false, f));
	rows.push_back(toggle("EE timing hack", "General-purpose timing hack. Known to affect following games: Digital Devil Saga, SSX.", FIX, "EETimingHack", false, f));
	rows.push_back(toggle("Instant DMA hack", "Good for cache emulation problems. Known to affect following games: Fire Pro Wrestling Z.", FIX, "InstantDMAHack", false, f));
	rows.push_back(toggle("OPH flag hack", "Known to affect following games: Bleach Blade Battlers, Growlanser II and III, Wizardry.", FIX, "OPHFlagHack", false, f));
	rows.push_back(toggle("Emulate the GIF FIFO", "Correct but slower. Known to affect the following games: FIFA Street 2.", FIX, "GIFFIFOHack", false, f));
	rows.push_back(toggle("DMA busy hack", "Known to affect following games: Mana Khemia 1, Metal Saga, Pilot Down Behind Enemy Lines.", FIX, "DMABusyHack", false, f));
	rows.push_back(toggle("Delay VIF1 stalls", "For SOCOM 2 HUD and Spy Hunter loading hang.", FIX, "VIF1StallHack", false, f));
	rows.push_back(toggle("Emulate the VIF FIFO", "Simulate VIF1 FIFO read ahead. Known to affect following games: Test Drive Unlimited, Transformers.", FIX, "VIFFIFOHack", false, f));
	rows.push_back(toggle("Full VU0 synchronisation", "Forces tight VU0 sync on every COP2 instruction.", FIX, "FullVU0SyncHack", false, f));
	rows.push_back(toggle("VU I bit hack", "Avoids constant recompilation in some games. Known to affect the following games: Scarface The World is Yours, "
		"Crash Tag Team Racing.", FIX, "IbitHack", false, f));
	rows.push_back(toggle("VU add hack", "For Tri-Ace Games: Star Ocean 3, Radiata Stories, Valkyrie Profile 2.", FIX, "VuAddSubHack", false, f));
	rows.push_back(toggle("VU overflow hack", "To check for possible float overflows (Superman Returns).", FIX, "VUOverflowHack", false, f));
	rows.push_back(toggle("VU sync", "Run behind. To avoid sync problems when reading or writing VU registers.", FIX, "VUSyncHack", false, f));
	rows.push_back(toggle("VU XGKick sync", "Use accurate timing for VU XGKicks (slower).", FIX, "XgKickHack", false, f));
	rows.push_back(toggle("Count frames by blits", "Use alternative method to calculate internal FPS to avoid false readings in some games.", FIX, "BlitInternalFPSHack", false, f));
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
		"Lowers the internal resolution when the game can't hold full speed, and raises it again when there's room. "
			"Doesn't help when the CPU is the bottleneck.",
		GS, "PS5AutoQuality", true)));

	// ---- Display
	rows.push_back(heading("DISPLAY"));
	{
		// One choice for PCSX2's two renderers and paraLLEl-GS (which is PCSX2's hardware renderer with
		// [EmuCore/GS] PS5ParallelGS on: the picture goes on through PCSX2's presentation).
		Option o;
		o.kind = Option::Combo;
		o.label = "Renderer";
		o.desc = "Selects how the emulated GS is drawn: Hardware uses the GPU, Software uses the CPU like the PS2 did (slow but "
			"accurate), paraLLEl-GS emulates the GS on the GPU at high accuracy.";
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
		"How many samples paraLLEl-GS takes per pixel. More samples give a smoother, sharper picture but need more GPU "
			"power.",
		GS, "PS5ParallelGSSamples", 8, {"1 (the PS2's picture)", "2", "4", "8", "16"}, {1, 2, 4, 8, 16}, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS at twice the size",
		"Renders at twice the PS2's resolution when using 4 or more samples. Sharper, but needs more GPU power.",
		GS, "PS5ParallelGSHighRes", true, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS sharp 2-D",
		"Applies super-sampling to text, menus and other 2D elements. Sharper, but may cause seams in some games.",
		GS, "PS5ParallelGSSharp2D", false, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS hands work over as it goes (experimental)",
		"Sends work to the GPU while the frame is still being recorded. Can help games that read back the screen. "
			"Experimental.",
		GS, "PS5ParallelGSEarlySubmit", false, 0), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS full-detail effects",
		"Keeps full detail in effects that render to a texture, such as blur, glow and depth of field. Needs more GPU "
			"power.",
		GS, "PS5ParallelGSTextures", false, Option::NextStart), GS, "PS5ParallelGS", false)));
	rows.push_back(adv(when(toggle("paraLLEl-GS pictures shared with the graphics device (experimental)",
		"Passes paraLLEl-GS frames to the display without copying them on the CPU. Experimental.",
		GS, "PS5ParallelGSSharedFrames", false, Option::NextStart), GS, "PS5ParallelGS", false)));
	rows.push_back(strings("Aspect ratio", "Selects the aspect ratio to display the game content at.", GS, "AspectRatio", "Auto 4:3/3:2",
		{"Stretch to fill the screen", "Automatic (4:3 or 3:2)", "4:3", "16:9", "10:7"}, {"Stretch", "Auto 4:3/3:2", "4:3", "16:9", "10:7"}));
	rows.push_back(adv(strings("FMV aspect ratio override", "Selects the aspect ratio for display when a FMV is detected as playing.", GS, "FMVAspectRatioSwitch", "Off",
		{"The same as the game", "Automatic (4:3 or 3:2)", "4:3", "16:9", "10:7"}, {"Off", "Auto 4:3/3:2", "4:3", "16:9", "10:7"})));
	{
		Option o;
		o.kind = Option::Combo;
		o.label = "Beside a 4:3 picture";
		o.desc = "What fills the sides of the screen when the game is 4:3. Dark grey wears an OLED screen more evenly than "
			"black.";
		o.section = GS;
		o.key = "SidePicture";
		o.names = {"Black", "Dark grey", "A blurred copy of the picture"};
		o.parts = {{FRONT, "GreyBars", false}, {GS, "SidePicture", false}};
		o.combos = {{0, 0}, {1, 0}, {0, 1}};
		rows.push_back(std::move(o));
	}
	rows.push_back(list("Deinterlacing",
		"Determines the deinterlacing method to be used on the interlaced screen of the emulated console. Automatic "
			"should be able to correctly deinterlace most games, but if you see visibly shaky graphics, try one of the "
			"other options.",
		GS, "deinterlace_mode", 0,
		{"Automatic", "Off", "Weave (top field first)", "Weave (bottom field first)", "Bob (top field first)", "Bob (bottom field first)",
			"Blend (top field first)", "Blend (bottom field first)", "Adaptive (top field first)", "Adaptive (bottom field first)"}));
	rows.push_back(adv(list("Bilinear filtering", "Smooths out the image when upscaling the console to the screen.", GS,
		"linear_present_mode", 2, {"Off (pixels)", "Bilinear (soft)", "Bilinear (sharp)"})));
	rows.push_back(toggle("Widescreen patches", "Automatically loads and applies widescreen patches on game start. Can cause issues.",
		CORE, "EnableWideScreenPatches", true, Option::AllGames | Option::NextStart));
	rows.push_back(toggle("No-interlacing patches", "Automatically loads and applies no-interlacing patches on game start. Can cause issues.", CORE,
		"EnableNoInterlacingPatches", true, Option::AllGames | Option::NextStart));
	// A game's own: PCSX2 keeps these as the game's patch lists ([Patches] Enable and Disable, by
	// the patch's name), not as the switches above, which are every game's.
	rows.push_back(list("Widescreen patch", "Applies this game's widescreen patch, if one exists.", "PS5GamePatch", "Widescreen 16:9", 0,
		{"As in Settings", "On", "Off"}, 0, Option::OneGame | Option::NextStart));
	rows.push_back(list("No-interlacing patch", "Applies this game's no-interlacing patch, if one exists.", "PS5GamePatch", "No-Interlacing", 0,
		{"As in Settings", "On", "Off"}, 0, Option::OneGame | Option::NextStart));
	rows.push_back(toggle("Anti-blur", "Enables internal Anti-Blur hacks. Less accurate than PS2 rendering but will make a lot of games look less "
		"blurry.", GS, "pcrtc_antiblur", true));
	rows.push_back(toggle("Integer scaling", "Adds padding to the display area to ensure that the ratio between pixels on the host to pixels in the console "
		"is an integer number. May result in a sharper image in some 2D games.", GS, "IntegerScaling", false));
	rows.push_back(adv(toggle("Screen offsets", "Enables PCRTC Offsets which position the screen as the game requests.", GS, "pcrtc_offsets", false)));
	rows.push_back(adv(toggle("Show overscan", "Enables the option to show the overscan area on games which draw more than the safe area of the screen.", GS, "pcrtc_overscan", false)));
	rows.push_back(adv(notPgs(toggle("Disable interlace offset", "Disables interlacing offset which may reduce blurring in some situations.", GS, "disable_interlace_offset", true))));
	rows.push_back(adv(range("Vertical stretch", "Increases or decreases the virtual picture size vertically.", GS, "StretchY", 100, 10, 300, 1, "%")));
	rows.push_back(adv(range("Crop left", "Crops the image, while respecting aspect ratio.", GS, "CropLeft", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop top", "Crops the image, while respecting aspect ratio.", GS, "CropTop", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop right", "Crops the image, while respecting aspect ratio.", GS, "CropRight", 0, 0, 720, 1, " px")));
	rows.push_back(adv(range("Crop bottom", "Crops the image, while respecting aspect ratio.", GS, "CropBottom", 0, 0, 720, 1, " px")));
	addRows(rows, be->extraRows("output", layer));

	// ---- Rendering
	rows.push_back(heading("RENDERING"));
	{
		Option o = floats("Internal resolution",
			"Multiplies the render resolution by the specified factor (upscaling).",
			GS, "upscale_multiplier", 6.f,
			{"Native (PS2)", "2x (~720p)", "3x (~1080p)", "4x (~1440p)", "5x (~1800p)", "6x (~2160p, 4K)", "7x", "8x (~2880p)", "9x", "10x", "11x", "12x"},
			{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f, 12.f});
		o.enabled = no_fsr;
		rows.push_back(std::move(o));
	}
	rows.push_back(hwAny(listOf("Upscaling and sharpening",
		"Off uses the internal resolution as it is. FSR 1 renders the game at a lower resolution and upscales it with "
			"AMD FidelityFX Super Resolution; CAS sharpens the image with FidelityFX Contrast Adaptive Sharpening.",
		GS, "CASMode", 5,
		{"Off", "FSR 1 from 4x (sharpest)", "FSR 1 from 3x", "FSR 1 from 2x (fastest)", "NIS: enlarge and sharpen", "CAS: sharpen", "CAS: sharpen and resize"},
		{0, 5, 3, 4, 6, 1, 2})));
	{
		Option o = range("Sharpness", "Determines the intensity the sharpening effect in CAS post-processing.", GS, "CASSharpness", 50, 0, 100, 5, "%");
		o.enabled = [hardwareAny](int l) { return hardwareAny(l) && effectiveInt(l, GS, "CASMode", 5) != 0; };
		rows.push_back(std::move(o));
	}
	rows.push_back(hw(list("Texture filtering", "Selects where bilinear filtering is utilized when rendering textures.", GS, "filter", 2,
		{"Nearest (blocky)", "Bilinear (always)", "Bilinear (as the PS2)", "Bilinear (always, not sprites)"})));
	rows.push_back(adv(hw(list("Trilinear filtering", "Selects where trilinear filtering is utilized when rendering textures.", GS, "TriFilter", -1,
		{"Automatic", "Off", "As the PS2", "Always"}, -1))));
	rows.push_back(hw(listOf("Anisotropic filtering", "Reduces texture aliasing at extreme viewing angles.", GS, "MaxAnisotropy", 0,
		{"Off", "2x", "4x", "8x", "16x"}, {0, 2, 4, 8, 16})));
	rows.push_back(adv(hwAny(list("Dithering", "Selects the type of dithering applies when the game requests it.", GS, "dithering_ps2", 2,
		{"Off", "Scaled", "Unscaled (as the PS2)", "Force 32 bit"}))));
	rows.push_back(hw(list("Blending accuracy",
		"Determines the level of accuracy when emulating blend modes not supported by the host graphics API.",
		GS, "accurate_blending_unit", 1, {"Minimum", "Basic", "Medium", "High", "Full (slow)", "Maximum (very slow)"})));
	rows.push_back(adv(hwAny(toggle("Mipmapping", "Enables mipmapping, which some games require to render correctly. Mipmapping uses progressively lower "
		"resolution variants of textures at progressively further distances to reduce processing load and avoid visual "
		"artifacts.", GS, "hw_mipmap", true))));
	rows.push_back(adv(hw(toggle("Accurate alpha test", "Enables accurate alpha testing, which some games require to render correctly. This may require more draw calls "
		"and result in a speed penalty.", GS, "HWAccurateAlphaTest", false))));
	rows.push_back(adv(hw(toggle("AA1", "Enables AA1 (PS2 antialiasing), which some games require to render correctly. This may result in a heavy "
		"performance penalty.", GS, "HWAA1", false))));
	rows.push_back(adv(hw(toggle("Rasterizer ordered view",
		"Enables Rasterizer Ordered View (ROV), which allows feedback loops to be executed with fewer draw calls. Can "
			"improve performance in feedback heavy games with higher accuracy settings.", GS, "HWROV", true))));
	rows.push_back(adv(sw(range("Software renderer threads", "Number of threads to use in addition to the main GS thread for rasterization.", GS, "extrathreads", 2, 0, 10, 1, ""))));
	rows.push_back(adv(sw(toggle("Software auto flush", "Force a primitive flush when a framebuffer is also an input texture.", GS, "autoflush_sw", true))));
	rows.push_back(adv(sw(toggle("Software mipmapping", "Enables mipmapping, which some games require to render correctly. Mipmapping uses progressively lower "
		"resolution variants of textures at progressively further distances to reduce processing load and avoid visual "
		"artifacts.", GS, "mipmap", true))));

	// ---- Hardware Fixes and Upscaling Fixes: a game's own, or every game's (advanced)
	const unsigned fixFlags = forGame ? Option::OneGame : (Option::AllGames | Option::Expert);
	rows.push_back(heading("HARDWARE FIXES", fixFlags));
	hardwareFixRows(rows, fixFlags);
	rows.push_back(heading("UPSCALING FIXES", fixFlags));
	upscalingFixRows(rows, fixFlags);

	// ---- Texture Replacement
	rows.push_back(heading("TEXTURE REPLACEMENT"));
	rows.push_back(hw(toggle("Load textures",
		"Loads replacement textures where available and user-provided.",
		GS, "LoadTextureReplacements", true)));
	rows.push_back(adv(when(toggle("Asynchronous texture loading", "Loads replacement textures on a worker thread, reducing microstutter when replacements are enabled.", GS, "LoadTextureReplacementsAsync", true), GS,
		"LoadTextureReplacements", true)));
	rows.push_back(adv(when(toggle("Precache replacements", "Preloads all replacement textures to memory. Not necessary with asynchronous loading.", GS, "PrecacheTextureReplacements",
							true),
		GS, "LoadTextureReplacements", true)));
	{
		Option o = action("Download an HD texture pack",
			"Downloads this game's HD texture pack from the Internet Archive. Packs can be several GB.",
			[] { be->findTexturePacks(); }, Option::OneGame);
		o.info = [] { return be->texturePackDoing(); };
		rows.push_back(std::move(o));
	}
	{
		Option o = action("Stop the download", "Stops the download. Files already unpacked are kept.", [] { be->stopTexturePack(); }, Option::OneGame);
		o.enabled = [](int) { return !be->texturePackDoing().empty(); };
		rows.push_back(std::move(o));
	}
	rows.push_back(hw(toggle("Dump textures", "Dumps replaceable textures to disk. Will reduce performance.", GS,
		"DumpReplaceableTextures", false, ADV)));
	rows.push_back(when(toggle("Dump mipmaps", "Includes mipmaps when dumping textures.", GS, "DumpReplaceableMipmaps", false, ADV), GS, "DumpReplaceableTextures", false));
	rows.push_back(when(toggle("Dump FMV textures", "Allows texture dumping when FMVs are active. You should not enable this.", GS, "DumpTexturesWithFMVActive", false, ADV), GS, "DumpReplaceableTextures", false));

	// ---- Post-Processing
	rows.push_back(heading("POST-PROCESSING"));
	rows.push_back(toggle("FXAA", "Applies the FXAA anti-aliasing algorithm to improve the visual quality of games.", GS, "fxaa", false));
	rows.push_back(adv(list("TV shader", "Applies a shader which replicates the visual effects of different styles of television sets.", GS, "TVShader", 0,
		{"None", "Scanlines", "Diagonal", "Triangular", "Wave", "Lottes CRT", "4xRGSS", "NxAGSS"})));
	rows.push_back(strings("Look (shader preset)",
		"Makes the picture look like an old TV, with CRT shaders, composite or S-Video signal effects and film grain.",
		GS, "Look", "",
		{"None", "CRT (fast)", "CRT (fastest)", "CRT (finest; demanding at 4K)", "CRT through a composite signal", "Composite signal",
			"S-Video signal", "Film grain", "Warmer colours"},
		{"", "crt/crt-guest-advanced-fast.slangp", "crt/crt-guest-advanced-fastest.slangp", "crt/crt-guest-advanced.slangp",
			"crux2/crt-guest-advanced-composite.slangp", "crux2/composite.slangp", "crux2/s-video.slangp", "crux2/film-grain.slangp",
			"crux2/colour.slangp"}));
	addRows(rows, be->extraRows("picture", layer));
	rows.push_back(toggle("Shade boost", "Enables brightness/contrast/gamma/saturation adjustment.", GS, "ShadeBoost", false));
	rows.push_back(when(range("Brightness", "Adjusts brightness. 50 is normal.", GS, "ShadeBoost_Brightness", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Contrast", "Adjusts contrast. 50 is normal.", GS, "ShadeBoost_Contrast", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Gamma", "Adjusts gamma. 50 is normal.", GS, "ShadeBoost_Gamma", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(when(range("Saturation", "Adjusts saturation. 50 is normal.", GS, "ShadeBoost_Saturation", 50, 1, 100, 1, ""), GS, "ShadeBoost", false));
	rows.push_back(list("Frame generation",
		"Generates extra frames between the game's own, so a 30 fps game is shown at 60 fps. Adds a little input lag "
			"and can cause smearing on fast movement.",
		GS, "FrameGeneration", 0, {"Off", "30 to 60", "30 to 60, and 60 to 120 at 120 Hz"}));
	const auto fg = [](Option o) { return whenNot(std::move(o), GS, "FrameGeneration", 0, 0); };
	rows.push_back(fg(list("Frame generation method", "How the extra frames are generated. AMD FSR 3 optical flow handles small, fast movement better.", GS,
		"FrameGenerationMethod", 0, {"Crux2's own", "AMD FSR 3 optical flow"})));
	{
		Option o = list("Optical flow quality", "Higher quality needs more GPU power.", GS, "FrameGenerationQuality", 1,
			{"Performance", "Balanced", "Quality"});
		o.enabled = [](int l) { return effectiveInt(l, GS, "FrameGeneration", 0) != 0 && effectiveInt(l, GS, "FrameGenerationMethod", 0) == 1; };
		rows.push_back(std::move(o));
	}
	rows.push_back(fg(toggle("No frame generation during videos", "Turns frame generation off while the game plays a video.", GS, "FrameGenerationSkipVideos", true)));

	// ---- Advanced
	rows.push_back(heading("ADVANCED", ADV));
	rows.push_back(adv(hw(toggle("Keep read render targets uncompressed (experimental)",
		"Keeps render targets uncompressed when glow and flare effects read them. Can be faster in some games. "
			"Experimental; takes effect the next time a game starts.",
		GS, "PS5HoldFeedbackLayout", false, Option::NextStart))));
	rows.push_back(adv(hw(toggle("Render targets without compression (experimental)",
		"Creates render targets without colour compression. Can be faster or slower depending on the game. "
			"Experimental; takes effect the next time a game starts.",
		GS, "PS5RTNoCompression", false, Option::NextStart))));
	rows.push_back(adv(hw(toggle("Leave out barriers with nothing to wait for",
		"Skips GPU waits when nothing has been drawn since the last one. Turn off if effects look wrong. Takes effect "
			"the next time a game starts.",
		GS, "PS5SkipRedundantBarriers", true, Option::NextStart))));
	rows.push_back(adv(hw(toggle("Reuse copies of the picture",
		"Reuses copies of the screen for glow and blur effects when nothing has changed. Turn off if an effect looks a "
			"frame behind. Takes effect the next time a game starts.",
		GS, "PS5ReuseRTCopies", true, Option::NextStart))));
	rows.push_back(adv(hw(toggle("Keep the ordered view's render passes",
		"Keeps draws that use the rasterizer ordered view in the same render pass. Much less CPU work in scenes with "
			"many lights. Takes effect the next time a game starts.",
		GS, "PS5ROVKeepPass", true, Option::NextStart))));
	rows.push_back(list("Hardware download mode", "Changes synchronization behavior for GS downloads.", GS, "HWDownloadMode", 0,
		{"Accurate", "Accurate, always whole (fewer readbacks)", "No readbacks (the threads run in step)", "Unsynchronised (not the same twice)",
			"Off (the game reads nothing back)"},
		0, ADV));
	rows.push_back(hw(list("Texture preloading", "Uploads full textures to the GPU on use, rather than only the utilized regions. Can improve performance in "
		"some games.",
		GS, "texture_preloading", 2, {"Off", "Partial", "Full (hash cache)"}, 0, ADV)));
	rows.push_back(hw(toggle("Extended upscaling multipliers", "Displays additional, very high upscaling multipliers dependent on GPU and driver capability.", GS,
		"ExtendedUpscalingMultipliers", false, ADV)));
	{
		Names names;
		std::vector<float> values;
		for (int times = 13; times <= 25; times++)
		{
			names.push_back(std::to_string(times) + "x");
			values.push_back(static_cast<float>(times));
		}
		Option o = floats("Internal resolution above 12x", "Multiplies the render resolution by the specified factor (upscaling).", GS, "upscale_multiplier", 1.f,
			names, values, ADV);
		o.enabled = [hardware](int l) { return hardware(l) && effective(l, GS, "ExtendedUpscalingMultipliers", false); };
		rows.push_back(std::move(o));
	}
	rows.push_back(list("Override texture barriers", "Forces texture barrier functionality to the specified value.", GS, "OverrideTextureBarriers", -1, {"Automatic", "Off", "On"}, -1, ADV));
	rows.push_back(toggle("Extra barriers with the ordered view", "Forces extra barriers when using ROV with Vulkan to fix graphical issues present in some games and hardware "
		"configurations.", GS, "HWROVBarriersVK", false, ADV));
	rows.push_back(toggle("Disable shader cache", "Prevents the loading and saving of shaders/pipelines to disk.", GS, "DisableShaderCache", false,
		ADV | Option::Restart));
	rows.push_back(toggle("Disable vertex shader expand", "Falls back to the CPU for expanding sprites/lines.", GS, "DisableVertexShaderExpand",
		false, ADV | Option::Restart));
	rows.push_back(toggle("Spin CPU during readbacks", "Does useless work on the CPU during readbacks to prevent it from going into powersave modes. May improve "
		"performance during readbacks but with a significant increase in power usage.", GS, "HWSpinCPUForReadbacks", false, ADV));
	rows.push_back(toggle("Spin GPU during readbacks", "Submits useless work to the GPU during readbacks to prevent it from going into powersave modes. May improve "
		"performance during readbacks but with a significant increase in power usage.", GS, "HWSpinGPUForReadbacks", false, ADV));
	rows.push_back(toggle("Disable framebuffer fetch", "Prevents the usage of framebuffer fetch when supported by host GPU.", GS, "DisableFramebufferFetch", false,
		ADV | Option::AllGames | Option::Restart));
	rows.push_back(toggle("Use blit swap chain", "Uses a blit presentation model instead of flipping when using the Direct3D 11 graphics API. Has no effect on "
		"the PS5.", GS, "UseBlitSwapChain", false, ADV | Option::AllGames | Option::Restart));
	rows.push_back(frange("NTSC frame rate", "The frame rate used for NTSC games.", GS, "FramerateNTSC", 59.94f, 10, 300, 0.01, 2, " Hz", ADV));
	rows.push_back(frange("PAL frame rate", "The frame rate used for PAL games.", GS, "FrameratePAL", 50.f, 10, 300, 0.01, 2, " Hz", ADV));
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
	rows.push_back(range("OSD scale", "Scales the size of the onscreen OSD from 50% to 500%.", GS, "OsdScale", 100, 50, 500, 5, "%", A));
	rows.push_back(adv(frange("OSD margin", "Sets the distance in pixels from the edges of the screen for OSD elements.", GS, "OsdMargin", 10.f, 0, 100, 1, 0, " px", A)));
	rows.push_back(adv(list("OSD messages position", "Determines where on-screen display messages are positioned.", GS, "OsdMessagesPos", 0, places, 0, A)));
	rows.push_back(adv(list("OSD performance position", "Position of a variety of on-screen performance data points as selected by the user.", GS, "OsdPerformancePos", 3, places, 0, A)));
	rows.push_back(toggle("Bold figures", "Draws OSD text with heavier weight for improved readability.", GS, "OsdBoldText", true, A));
	rows.push_back(toggle("Move the figures from corner to corner", "Moves the on-screen figures to another corner every five minutes to prevent burn-in.", FRONT, "OsdMove",
		false, F));
	rows.push_back(list("Style", "Crux2: one panel with sections and a frame-time graph. PCSX2: PCSX2's own lines of text.", "PS5", "OsdStyle", 0, {"Crux2's panel", "PCSX2's lines"}, 0, A));
	rows.push_back(heading("SHOW"));
	rows.push_back(toggle("Speed percentages", "Shows the current emulation speed of the system as a percentage.", GS, "OsdShowSpeed", false, A));
	rows.push_back(toggle("FPS", "Shows the number of internal video frames displayed per second by the system.", GS,
		"OsdShowFPS", false, A));
	rows.push_back(adv(toggle("VPS", "Shows the number of Vsyncs performed per second by the system.", GS, "OsdShowVPS", false, A)));
	rows.push_back(toggle("Resolution", "Shows the internal resolution of the game.", GS, "OsdShowResolution", false, A));
	rows.push_back(toggle("CPU usage", "Shows the host's CPU utilization based on threads.", GS, "OsdShowCPU", false, A));
	rows.push_back(toggle("GPU usage", "Shows the host's GPU utilization.", GS, "OsdShowGPU", false, A));
	rows.push_back(toggle("Frame times", "Displays a graph showing the average frametimes.", GS, "OsdShowFrameTimes", false, A));
	rows.push_back(adv(toggle("Hardware info", "Shows the current system CPU and GPU information.", GS, "OsdShowHardwareInfo", false, A)));
	rows.push_back(adv(toggle("Statistics", "Shows statistics about the emulated GS such as primitives and draw calls.", GS, "OsdShowGSStats", false, A)));
	rows.push_back(adv(toggle("GPU statistics", "Shows GPU vertex shader and pixels shader invocations.", GS, "OsdShowGPUStats", false, A)));
	rows.push_back(toggle("Indicators", "Shows icon indicators for emulation states such as Pausing, Turbo, Fast-Forward, and Slow-Motion.", GS, "OsdShowIndicators", false, A));
	rows.push_back(adv(toggle("Settings", "Displays various settings and the current values of those settings in the bottom-right corner of the display.", GS, "OsdShowSettings", false, A)));
	rows.push_back(adv(toggle("Patches", "Shows the amount of currently active patches/cheats in the bottom-right corner of the display.", GS, "OsdshowPatches", false, A)));
	rows.push_back(adv(toggle("Inputs", "Shows the current controller state of the system in the bottom-left corner of the display.", GS, "OsdShowInputs", false, A)));
	rows.push_back(adv(toggle("Texture replacements", "Shows the status of the number of dumped and loaded texture replacements in the top-right corner of the "
		"display.", GS, "OsdShowTextureReplacements", true, A)));
	rows.push_back(toggle("Crux2 version", "Shows the current Crux2 version.", GS,
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
		"When the emulation isn't running at 100% speed, adjusts the tempo of the audio which produces much nicer sound "
			"during fast-forward/slowdown.", OUT,
		"SyncMode", "TimeStretch", {"None (the sound breaks up)", "Time stretching"}, {"Disabled", "TimeStretch"})));
	rows.push_back(strings("Expansion",
		"Determines how audio is expanded from stereo to surround for supported games.",
		OUT, "ExpansionMode", "Disabled", {"Off (stereo)", "Stereo with bass channel", "Four channels", "Four channels with bass", "5.1", "7.1"},
		{"Disabled", "StereoLFE", "Quadraphonic", "QuadraphonicLFE", "Surround51", "Surround71"}, A | Option::NextStart));
	rows.push_back(adv(range("Buffer size", "Determines the amount of audio buffered before being pulled by the host API.", OUT, "BufferMS", 50, 10, 500, 5, " ms",
		A)));
	rows.push_back(listOf("Output latency",
		"How much audio is sent to the console at once. Lower means less delay; raise it if the sound crackles.",
		"PS5", "AudioGrain", 256, {"Smallest (5 ms)", "11 ms", "16 ms", "21 ms", "32 ms", "43 ms"}, {256, 512, 768, 1024, 1536, 2048},
		A | Option::NextStart));

	rows.push_back(heading("CONTROLS"));
	rows.push_back(range("Standard volume", "Controls the volume of the audio played on the host at normal speed.", OUT, "StandardVolume", 100, 0, 200, 5, "%"));
	rows.push_back(range("Fast forward volume", "Controls the volume of the audio played on the host when fast forwarding.", OUT, "FastForwardVolume", 100, 0, 200, 5, "%"));
	rows.push_back(toggle("Mute all sound", "Prevents the emulator from producing any audible sound.", OUT, "OutputMuted", false));

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
	rows.push_back(list("Rounding mode", "Determines how the results of floating-point operations are rounded. Some games need specific settings.", CPU, "FPU.Roundmode", 3, rounding, 0, f));
	rows.push_back(list("Division rounding mode", "Determines how the results of floating-point division is rounded. Some games need specific settings.", CPU, "FPUDiv.Roundmode", 0, rounding, 0, f));
	rows.push_back(clamping("Clamping mode", "How numbers out of range are handled. Normal suits most games", "fpuOverflow", "fpuExtraOverflow",
		"fpuFullMode", {"None", "Normal", "Extra, keep the sign", "Full"}, f));
	rows.push_back(toggle("Enable recompiler", "Performs just-in-time binary translation of 64-bit MIPS-IV machine code to native code.", REC, "EnableEE", true, f));
	rows.push_back(toggle("Enable cache (slow)", "Enables emulation of the EE's hardware cache. Interpreter only, provided for diagnostics.", REC, "EnableEECache", false, f));
	rows.push_back(toggle("INTC spin detection", "Huge speedup for some games, with almost no compatibility side effects.", SPEED, "IntcStat", true, f));
	rows.push_back(toggle("Wait loop detection", "Moderate speedup for some games, with no known side effects.", SPEED, "WaitLoop", true, f));
	rows.push_back(toggle("Enable fast memory access", "Uses backpatching to avoid register flushing on every memory access.", REC, "EnableFastmem", true, f));
	rows.push_back(toggle("Pause on TLB miss", "Pauses the virtual machine when a TLB miss occurs, instead of ignoring it and continuing.", REC, "PauseOnTLBMiss", false, f));
	rows.push_back(toggle("Enable 128MB RAM (dev console)", "Exposes additional memory to the virtual machine, expanding the EE and IOP memory to 128MB and 8MB "
		"respectively.", CPU, "ExtraMemory", false, f | Option::NextStart));

	rows.push_back(heading("VECTOR UNITS (VU)", f));
	rows.push_back(list("VU0 rounding mode", "Determines how the results of floating-point operations are rounded. Some games need specific settings.", CPU, "VU0.Roundmode", 3, rounding, 0, f));
	rows.push_back(clamping("VU0 clamping mode", "", "vu0Overflow", "vu0ExtraOverflow", "vu0SignOverflow", {"None", "Normal", "Extra", "Extra, keep the sign"}, f));
	rows.push_back(list("VU1 rounding mode", "Determines how the results of floating-point operations are rounded. Some games need specific settings.", CPU, "VU1.Roundmode", 3, rounding, 0, f));
	rows.push_back(clamping("VU1 clamping mode", "", "vu1Overflow", "vu1ExtraOverflow", "vu1SignOverflow", {"None", "Normal", "Extra", "Extra, keep the sign"}, f));
	rows.push_back(toggle("Enable VU0 recompiler (micro mode)", "Performs just-in-time binary translation of Vector Unit 0 (VU0) microprograms to native code.", REC, "EnableVU0", true, f));
	rows.push_back(toggle("Enable VU1 recompiler", "Performs just-in-time binary translation of Vector Unit 1 (VU1) microprograms to native code.", REC, "EnableVU1", true, f));
	rows.push_back(toggle("mVU flag hack", "Good speedup and high compatibility, may cause graphical errors.", SPEED, "vuFlagHack", true, f));
	rows.push_back(toggle("Instant VU1", "Runs VU1 instantly. Provides a modest speed improvement in most games. Safe for most games, but a few games "
		"may exhibit graphical errors.", SPEED, "vu1Instant", true, f));

	rows.push_back(heading("I/O PROCESSOR (IOP, MIPS-I)", f));
	rows.push_back(toggle("Enable IOP recompiler", "Performs just-in-time binary translation of 32-bit MIPS-I machine code to native code.", REC, "EnableIOP", true, f));

	rows.push_back(heading("GAME SETTINGS", f));
	rows.push_back(toggle("Enable game fixes", "Automatically loads and applies fixes to known problematic games on game start.", CORE, "EnableGameFixes", true, f));
	rows.push_back(toggle("Enable compatibility patches", "Automatically loads and applies compatibility patches to known problematic games.", CORE, "EnablePatches", true, f));
	rows.push_back(toggle("Recommended picture settings",
		"Applies the higher blending accuracy, accurate alpha test or AA1 that the game database recommends. Settings "
			"made in a game's own options take priority.",
		"PS5", "GameDbRecommended", true, f | A));

	rows.push_back(heading("SAVESTATE SETTINGS"));
	rows.push_back(toggle("Create save state backups",
		"Creates a backup copy of a save state if it already exists when the save is created. The backup copy has a "
			".backup suffix", CORE, "BackupSavestate", true, A));
	rows.push_back(listOf("Save a state by itself",
		"Automatically saves the game's state at this interval. Load it from the quick menu with \"Load the "
			"auto-save\". The game may pause briefly while saving.",
		"PS5", "AutoSaveMinutes", 0, {"Off", "Every 2 minutes", "Every 5 minutes", "Every 10 minutes", "Every 15 minutes"}, {0, 2, 5, 10, 15}, A));
	rows.push_back(toggle("Undo for a loaded state",
		"Keeps the game's current state before you load a save state, so you can undo the load from the quick menu.", "PS5",
		"UndoLoad", true, A));
	addRows(rows, be->extraRows("saves", layer));
	rows.push_back(list("Compression method", "Sets the compression algorithm for savestate.", CORE, "SavestateCompressionType", 2, {"Uncompressed", "Deflate64", "Zstandard"}, 0, A | f));
	rows.push_back(list("Compression level", "Sets the compression level for savestate.", CORE, "SavestateCompressionRatio", 1, {"Low (fast)", "Medium", "High", "Very high (slow)"}, 0, A | f));
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
	rows.push_back(toggle("Log to a file", "Writes log messages to emulog.txt.", "Logging", "EnableFileLogging", true, f));
	rows.push_back(toggle("Verbose", "Writes dev log messages to log sinks.", "Logging", "EnableVerbose", false, f));
	rows.push_back(toggle("Timestamps", "Writes timestamps alongside log messages.", "Logging", "EnableTimestamps", true, f));
	rows.push_back(toggle("EE console", "Writes debug messages from the game's EE code to the console.", "Logging", "EnableEEConsole", true, f));
	rows.push_back(toggle("IOP console", "Writes debug messages from the game's IOP code to the console.", "Logging", "EnableIOPConsole", true, f));
	rows.push_back(toggle("CDVD verbose reads", "Logs disc reads from games.", CORE, "CdvdVerboseReads", false, f));

	// PCSX2's trace log: every kind of event, into the log. Very slow, very large.
	constexpr const char* TRACE = "EmuCore/TraceLog";
	rows.push_back(heading("TRACE LOGGING", f));
	rows.push_back(toggle("Enable trace logging", "Writes the selected kinds of events to the log. Slow, and the log grows quickly.", TRACE, "Enabled",
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
	rows.push_back(toggle("Use debug blend", "Forces SW blending and disables several optimizations.", GS, "UseDebugBlend", false, f));
	rows.push_back(list("GS dump compression", "Change the compression algorithm used when creating a GS dump.", GS, "GSDumpCompression", 2,
		{"Uncompressed", "LZMA (xz)", "Zstandard (zst)"}, 0, f));
	rows.push_back(toggle("Dump GS draws", "Dumps information about every draw to the snapshots folder. Very slow and very large.", GS, "DumpGSData",
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
	rows.push_back(when(range("Save frame count", "-1 saves every frame.", GS, "SaveFrameCount", -1, -1, 100000, 1, "", f), GS, "DumpGSData", false));
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
		"Gives sound, controllers and menus priority over the emulator, and runs downloads and copies after it. "
			"Prevents crackling and late button presses.",
		"PS5", "ThreadPriorities", false, A | Option::Restart));
	rows.push_back(toggle("Build shaders quickly first",
		"Draws new effects with a quickly built shader first, then switches to the optimised one. Reduces stutter the "
			"first time an effect appears.",
		"PS5", "FastPipelines", true, A | Option::Restart));

	rows.push_back(heading("NATIVE CODE MODULES (EXPERIMENTAL)", A));
	rows.push_back(toggle("Use native code modules",
		"Runs a game's code from a native module built with the recomp kit from your own copy of the game. "
			"Experimental.",
		"PS5", "NativeModules", true, A | Option::NextStart));
	const auto native = [](Option o) { return when(std::move(o), "PS5", "NativeModules", true); };
	rows.push_back(native(toggle("Put executables on the share",
		"Copies a game's executable to the network share when no module exists, so the recomp kit can build one.",
		"PS5", "NativeExport", true, A | Option::NextStart)));
	rows.push_back(native(toggle("Native code may use hardware",
		"Lets native modules access the console's hardware directly. Turn off if a game hangs with its module.", "PS5", "NativeHardware",
		true, A | Option::NextStart)));
	rows.push_back(native(toggle("Check native code",
		"Runs every function twice and compares the result with PCSX2's interpreter. Very slow; for finding bugs only.",
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
