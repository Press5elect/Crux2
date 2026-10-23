// Crux2 - the interface: what its screens share.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Layout, in units of 1/1080 of the screen height:
//   top bar    0..96       the app, the tabs (L1 / R1), the clock
//   content    96..1008    the library (hero + shelves) or the settings
//   hint bar   1008..1080  what each DualSense button does here

#pragma once

#include "bp.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "IconsFontAwesome.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace bp
{

extern Backend* be;

// ---------------------------------------------------------------- the theme

namespace col
{
constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
extern ImU32 bgTop, bgBottom, bar, panel, panelHi, card, sheet, sheetHi, dim, faint, accent, accentDark, accentGlow, warm, good, bad;
/// What is written (the skin's), and what is written on a shape filled with the accent.
extern ImU32 text, onAccent;
/// Hairlines and separators (the skin's: faint white on a dark skin, faint dark on a light one).
extern ImU32 line;
/// How round the skin's corners are: every radius is multiplied by it (0 square .. 2 very round).
extern float round;
/// How large the text is drawn (Settings > Interface > Text size): 1, 1.12 or 1.24.
extern float textScale;
/// The skin's page is light: the mark's cream is drawn in the skin's text colour.
extern bool lightSkin;
/// A picture as it is: the tint that changes nothing.
constexpr ImU32 white = rgba(255, 255, 255);
/// The mark's own two colours, the same in every skin.
constexpr ImU32 brandCream = rgba(243, 236, 228);
constexpr ImU32 brandOrange = rgba(255, 106, 61);
} // namespace col

extern float S;        // pixels per unit
extern float W, H;     // the screen, in units
// The screens' own clock and frame count: they go on when the emulator makes its graphics device
// (and with it ImGui, whose clock starts at nothing again) anew under a screen that is up.
extern double timeNow;
extern int frameNow;
/// Once a frame, before any screen: moves them on.
void tickClock();

inline ImVec2 V(float x, float y) { return ImVec2(x * S, y * S); }
ImU32 alpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);

ImFont* regular();
ImFont* bold();
/// The face the name is set in (the bold face until the Backend has given it).
ImFont* brand();

/// Eases v toward target; speed is roughly "per second".
float approach(float v, float target, float speed = 14.f);
float easeOut(float t);
float easeInOut(float t);

ImVec2 textSize(ImFont* font, float size, const char* s, float wrap = 0.f);
/// Text at (x, y) units; returns its size in units.
ImVec2 text(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 c, const char* s, float wrap = 0.f);
/// Text cut to a width with an ellipsis.
std::string fit(ImFont* font, float size, const std::string& s, float width);
/// Text in a box `width` units wide. Whole when it fits. When it does not: cut with an ellipsis,
/// or, with `moving` (what the focus is on, a dialog's title), sliding sideways by itself so
/// that all of it is read: it waits, slides to its end, waits there, and begins again. Returns
/// the size of what is shown.
ImVec2 textIn(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 c, const std::string& s, float width, bool moving);

void rect(ImDrawList* dl, float x, float y, float w, float h, ImU32 c, float r = 0.f, ImDrawFlags f = 0);
void outline(ImDrawList* dl, float x, float y, float w, float h, ImU32 c, float r, float t);
void gradientV(ImDrawList* dl, float x, float y, float w, float h, ImU32 top, ImU32 bottom);
void gradientH(ImDrawList* dl, float x, float y, float w, float h, ImU32 left, ImU32 right);
/// A soft glow: concentric rounded rectangles, fading out.
void glow(ImDrawList* dl, float x, float y, float w, float h, float r, ImU32 c, float spread = 18.f);
/// A disc of light, bright at its centre and gone at its edge.
void softDisc(ImDrawList* dl, float cx, float cy, float radius, ImU32 colour, int segments = 36);
/// A quad with a colour at each corner (top left, top right, bottom right, bottom left).
void quad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd);
/// A number between 0 and 1 from a whole number, the same each time.
float chance(unsigned n);

/// The image cropped to fill the rectangle, its centre kept.
void imageFill(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float r, ImU32 tint = col::white);
/// The whole image inside the rectangle, its shape kept.
void imageFit(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float r, ImU32 tint = col::white);
/// The image cropped to fill the rectangle, drawn as a grid whose vertex alphas are
/// ax[i] * ay[j] * k: art that fades into the backdrop with no seam.
void imageFade(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float k, const float (&xs)[3],
	const float (&ax)[3], const float (&ys)[3], const float (&ay)[3]);

// ------------------------------------------------------- skins and motion

enum Backdrop { BackStill, BackAurora, BackWaves, BackSparks, BackHorizon, BackMatrix, BackCover, BackPlay, BackOwn, BackdropCount };
extern const char* const backdropNames[BackdropCount];

struct Skin
{
	const char* name;
	ImU32 bgTop, bgBottom, bar, panel, panelHi, card, sheet, sheetHi, dim, faint, accent, warm;
	ImU32 text, onAccent;
	Backdrop backdrop;
	float round = 1.f;    // corner radii are multiplied by this (0 square .. 2 very round)
	bool light = false;   // a light page: what is drawn as a faint white line elsewhere is drawn dark
	ImU32 line = IM_COL32(255, 255, 255, 22); // hairlines and separators
	ImU32 good = IM_COL32(52, 199, 89, 255);  // success
	ImU32 bad = IM_COL32(229, 72, 77, 255);   // danger
	const char* family = "";  // the kit's design language name, shown under the name
};
extern const Skin skins[];
extern const int SkinCount;

struct Accent
{
	const char* name;
	ImU32 colour;
};
extern const Accent accents[];
extern const int AccentCount;

/// The front end's own options, kept with the settings for all games ([PS5UI]).
struct Options
{
	int view = 0;        // 0 shelves, 1 grid, 2 list, 3 flow, 4 bookcase, 5 table, 6 showcase, 7 wheel, 8 wall, 9 discs
	int source = 0;      // the library tab last open: 0 internal, 1 USB, 2 network
	bool groupDiscs = true;
	bool splash = true;
	int skin = 0;
	int accent = 0;      // 0: the skin's own
	int backdrop = 0;    // 0: the skin's own
	int motion = 0;      // 0 all of it, 1 less, 2 none
	int level = 0;          // the settings shown: 0 normal, 1 advanced, 2 expert ([PS5UI] SettingsLevel)
	bool advanced = false;  // level >= 1
	bool expert = false;    // level >= 2
	int stateSlot = 1;   // the quick menu's slot, 1..10
	int sort = 0;        // the library's order: 0 by name, 1 last played first, 2 most played first,
	                     // 3 the largest first, 4 the newest file first
	bool heroPlay = true;  // the shelves show a picture of play behind the focused game, when it has one
	int wallEvery = 60;    // "My pictures": seconds each one stays
	int wallDim = 50;      // and how much darker they are made, in hundredths
	// Care for the screen (bp_care.cpp).
	int idleDim = 3;       // minutes with nothing pressed after which the screens go dark; 0 never
	bool screenSaver = true; // and after as long again: black, with the mark wandering
	bool pixelShift = true;  // the screens move a few pixels every other minute
	bool hintFade = true;    // the hint bar's words go faint when nothing is pressed
	// Seeing (Settings > Interface): larger text, stronger contrast, colours told apart without red
	// and green.
	int textSize = 0;        // -2 smallest (84%), -1 smaller (92%), 0 normal, 1 larger (112%), 2 largest (124%)
	int uiSize = 100;        // the interface's size, in % (100, 90, 80, 70): the whole of it smaller
	bool highContrast = false;
	bool colourBlind = false;
	int gameDim = 0;       // minutes with nothing pressed in a game after which its picture goes darker; 0 never
	bool greyBars = false; // the bars beside a game's picture are dark grey, not black
	bool osdMove = false;  // the emulator's figures go from corner to corner
	bool swapConfirm = false; // Circle chooses and Cross goes back, as on a console of the Japanese kind
	int splashKind = 0;  // the start-up animation: 0 another one each start, 1..4 always that one
	int splashLast = 0;  // the one the last start played, 1..4 (0: none yet)
};
Options& options();
void loadOptions();
/// The settings shown: 0 normal, 1 advanced, 2 expert; kept, and PCSX2's own switch for advanced
/// settings kept in step.
void setSettingsLevel(int level);
extern const char* const settingsLevelNames[3];
void saveOptions();

void applySkin();
Backdrop backdropChoice();
enum Motion { MotionFull, MotionReduced, MotionOff };
Motion motion();
/// The game whose cover is behind the screens with "Cover colours" (null: none).
void ambientArt(const GameInfo* game);
/// What is behind every opaque screen: the skin's gradient, then its backdrop.
void drawBackdrop(ImDrawList* dl);

// ---- transitions: what is drawn between beginLayer and endLayer can fade, slide and grow as
// one piece.
struct Layer
{
	ImDrawList* dl;
	int from;
};
Layer beginLayer(ImDrawList* dl);
void endLayer(const Layer& layer, float opacity, float dx = 0, float dy = 0, float scale = 1, float pivotX = 0, float pivotY = 0);

/// A screen coming in. `screen` names it: a new name, or frames without it, start its entrance
/// again. The return is how far in it is, 0 to 1.
float entered(int screen);
/// Since when the screen that is on has been on.
double enteredAt();
/// The side the next screen comes in from: -1 the left, 1 the right.
void entranceSide(float side);
void endEntrance(const Layer& layer, float e);

/// A disc, its hub, and an accent arc on the rim: what a game without a cover has in its place.
void discMark(ImDrawList* dl, float cx, float cy, float radius, ImU32 ring, ImU32 arc, float thickness);

// ---- the title's own mark and name
//
// The mark is one broad bar crossing two thin ones, and it is the X of the name: C R U, the mark,
// 2. Both are drawn here, the mark from its geometry and the letters in the brand's face, so that
// the start-up animations can take them apart.

/// How the mark's bars stand, for the animations (lengths and slides in hundredths of its size;
/// at rest: thin bars at -45 degrees and 84 long, the broad one 84 long, nothing slid or turned).
struct MarkPose
{
	float turn = 0;                  // the whole mark, in degrees
	float thinAngle = -45;
	float thinLength[2] = {84, 84};
	float thinSlide[2] = {0, 0};     // along the bar itself
	float thinAlpha[2] = {1, 1};
	float broadLength = 84, broadSlide = 0, broadAlpha = 1;
	float glint = -2;                // a light passing along the broad bar: -1 one end, 1 the other; outside: none
	float scale = 1;                 // the whole mark
};
/// The mark in a square of `size` whose middle is (cx, cy).
void cruxMark(ImDrawList* dl, float cx, float cy, float size, float a = 1.f, const MarkPose* pose = nullptr);
/// Where the name's five parts (C, R, U, the mark, 2) are when it is set in type of size `type`:
/// each part's middle from the name's left edge, the name's width, and the mark's size.
enum { WordC, WordR, WordU, WordMark, Word2, WordParts };
struct WordLayout
{
	float centre[WordParts];
	float width;
	float mark;
};
WordLayout wordLayout(float type);
/// One of the name's letters (not the mark), its middle at (cx, cy), in type of size `type`.
void wordLetter(ImDrawList* dl, int part, float cx, float cy, float type, float a = 1.f);
/// The whole name, its left edge at x and its letters' middle at cy. Returns its width.
float wordMark(ImDrawList* dl, float x, float cy, float type, float a = 1.f, const MarkPose* pose = nullptr);
float wordMarkWidth(float type);
/// The type the top bar sets the name in, and where its left edge and middle are.
constexpr float BAR_TYPE = 44, BAR_NAME_X = 56, BAR_NAME_Y = 48;
/// A PlayStation 2 DVD seen from its shiny side (they were gold), turned by `spin`.
void goldDisc(ImDrawList* dl, float cx, float cy, float radius, float spin, float a = 1.f, bool blue = false);
/// How far the disc in the drive has turned by now; it stands still unless everything moves.
float discSpin();

// ------------------------------------------------------------ button glyphs

enum class Glyph { Cross, Circle, Square, Triangle, L1, R1, L1R1, L2, R2, L2R2, R3, Options, TouchPad, DPadLR, DPadUD };
float glyph(ImDrawList* dl, Glyph g, float x, float cy, float size = 34.f);

struct Hint
{
	Glyph glyph;
	const char* label;
};
void hintBar(ImDrawList* dl, const std::vector<Hint>& hints);

// ------------------------------------------------------------------ input

struct Input
{
	bool up, down, left, right;
	bool accept, back, triangle, square, l1, r1, l2, r2, l3, r3, options, touch;
	int repeats;   // how many times in a row a held direction has repeated (a slider speeds up)
	bool any;      // any button went down this frame
};
extern Input in;

// ----------------------------------------------------------- frame set-up

ImDrawList* beginScreen(const char* id, bool opaque);
void endScreen();

// --------------------------------------------------------------- top bar

// (DiscDrive is a tab only while a disc is in the console's drive, and is not kept as the tab
// last open.)
enum Source { Internal, Usb, Network, DiscDrive, SourceCount };
/// Which other tabs L1 / R1 go to from a screen; the rest are fainter.
enum class Reach { All, None };
void topBar(ImDrawList* dl, int active, bool inGame, Reach reach = Reach::All);
struct TopTab
{
	int id;
	const char* icon;
	const char* name;
};
std::vector<TopTab> topTabs();

// ---- care for the screen (bp_care.cpp): what an OLED television is spared
//
// Pictures that stand still for hours leave their mark on an OLED screen. So the screens go dark
// when nothing was pressed for a while, and black with the mark wandering after as long again;
// they move by a few pixels every other minute; and the hint bar's words go faint. Under a game,
// which is the emulator's picture: it can be made darker when nothing was pressed for long, the
// bars beside a 4:3 picture can be dark grey, and the emulator's own figures can go from corner to
// corner. Each has its switch (Settings > Screen care).

/// Once a frame, before any screen, with what the controller holds.
void careTick(uint32_t held);
/// The seconds since a button was last held.
float idleSeconds();
/// How much of what stands still (the hint bar's words) is to be seen now: 1, less when nothing
/// was pressed for a while.
float steadyAlpha();
/// Whether the screen saver is up, and whether a button has ended it this frame: that press is
/// nobody else's.
bool careAsleep();
bool careWoke();
/// Whether one did within the last second and a half (its release is nobody else's either).
bool careWokeLately();
/// As if nothing had been pressed for that long (the PC preview's, to look at what comes then).
void careSetIdle(float seconds);
/// Over a finished screen, by endScreen: the darkening, the screen saver, and the move of all
/// of it by a few pixels.
void careOverScreen(ImDrawList* dl);
/// With no screen on (a game runs): the grey bars, the darkening, the figures' corner.
void careOverGame();

/// A switch, as the settings draw it.
void drawSwitch(ImDrawList* dl, float right, float cy, bool on, bool focused, float scale = 1.f);
/// The two arrows around a value that left and right change.
void drawArrows(ImDrawList* dl, float left, float right, float cy, ImU32 c);

// ---------------------------------------------------------------- dialogs
//
// Over whatever screen is on: a message, a question, a list to choose from, the wait for a
// controller button. While one is up it has the controller (the screen under it sees none).

// (pick, the list to choose from, is in bp.h: the Backend's pages use it too.)
bool dialogOpen();
/// The card manager (bp_cards.cpp): whether it is up, its controller and its frame. It is under
/// any list or question it asks (those are dialogs), over the screen that opened it.
bool cardManagerOpen();
void cardManagerInput(const Input& now);
void cardManagerDraw(ImDrawList* dl);
void closeCardManager();
/// The lists and questions that are up go, unanswered (the screen that asked is gone).
void dropScreenDialogs();
/// A dialog or a toast with no screen under it (a game runs): this draws them.
void drawOverGame();
/// Nothing is read from the controller until every button was let go.
void holdInput();
/// Takes the controller for the dialog that is up. Called by beginScreen.
void dialogInput();
/// Draws the dialog and the toasts. Called by endScreen.
void dialogDraw(ImDrawList* dl);

// ---------------------------------------------------------------- screens

enum class Screen { None, Library, Disc, Settings, Quick, Loading };
extern Screen screen;
void setScreen(Screen s);

/// The start-up animation: not begun, showing, leaving, over. While it leaves, the name goes to
/// the top bar; flight is 1 once the top bar draws the name itself.
struct SplashState
{
	enum { NotBegun, Showing, Leaving, Over } state = NotBegun;
	/// Which one plays (bp_splash.cpp): one of the four, or the name standing still ("Less" motion).
	enum { Strike, Unfold, Spin, Slash, Kinds, Still = Kinds } kind = Still;
	double began = 0;    // the animation's time 0, on the screens' clock
	float leaveAt = 0;   // the seconds after it at which the name leaves for the top bar
	float leaveFor = 1;  // and how long that takes
	bool skipped = false; // a button ended it: what is there fades, and the library is there
	int frames = 0;
	float flight = 1.f;
	bool told[3] = {};   // the moments (bp::Sound) the Backend was told of, this start
};
extern SplashState splash;
/// The start-up animations (bp_splash.cpp). When the name leaves for the top bar, and for how long.
float splashLeaveAt(int kind);
float splashLeaveFor(int kind);
/// One frame: t the seconds since it began (all four are timed to the start-up sound, whose hit
/// is at 1.12 s). It draws its own ground over whatever is on `dl`, less and less of it as the
/// library under it comes; true when the top bar is to draw the name itself from here on.
bool drawStartup(ImDrawList* dl, int kind, float t);
/// Whether the name has reached the top bar by t (the top bar draws it from then on; the
/// animation stops drawing it at the same moment).
bool splashDelivered(int kind, float t);
/// The name standing in the middle of the screen, for "Less" motion: `a` of it is there.
void drawStartupStill(ImDrawList* dl, float a);
/// The start-up's ground alone, `a` of it: what fades when a button ended the animation.
void drawStartupGround(ImDrawList* dl, float a);

void libraryScreen(bool selectDisc);
void librarySplashOrScreen(bool selectDisc);
void loadingScreen();
void quickScreen();
void settingsScreen();

/// The library's tab that is open (for the top bar of the other screens).
int librarySource();
/// Once a frame, before any screen: asks the Backend what is in the console's disc drive, says so
/// when a disc has come, and takes the library off the Disc tab when the disc has gone.
void pollDisc();
/// The drive as pollDisc found it.
const DiscState& discNow();
/// A game of the library by its path; null when it is not there.
const GameInfo* libraryGame(const std::string& path);
/// The cover of a game as the library draws it (a placeholder with its title when it has none).
void drawGameCover(ImDrawList* dl, const GameInfo& game, float x, float y, float w, float h, float r, float lift);
/// The game the loading screen is for.
void setLoadingGame(const GameInfo& game);
/// Starts a game from the library's list (remembers it for the loading screen).
void launchGame(const GameInfo& game, int state = INT_MIN, int boot = 0);
/// The PlayStation 2's own menu: the BIOS with no game (its browser and system configuration).
void startPs2Menu();

/// Opens the settings: of all games, or (with a game) that game's own. `back` is the screen they
/// return to.
/// `page`: the page to open on, by its name, with the focus in its rows ("Cheats"); none: the first.
void openSettings(const GameInfo* game, Screen back, const char* page = nullptr);
/// The open settings' page of that name, with the focus on the row of that label (none: on the
/// page). False when there is no such page or row. For the PC preview's walk through the screens.
bool settingsFocus(const char* page, const char* row = nullptr);
/// The settings of all games, and of one game, as pages of options (bp_options.cpp).
std::vector<Page> settingsPages(bool forGame);

// A game's title without its dump's tags, and the discs of one game (bp_library.cpp).
std::string cleanTitle(const std::string& name);
bool splitDisc(const std::string& base, std::string& rest, int& order);

std::string formatSize(uint64_t bytes);
std::string formatPlayed(time_t seconds);
std::string formatWhen(time_t when);
const char* kindText(const GameInfo& game);

} // namespace bp
