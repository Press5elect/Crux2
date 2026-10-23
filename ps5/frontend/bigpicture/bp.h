// Crux2 - the interface: a Big Picture style, controller-only front end.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The screens (bp_*.cpp) are the ones written for PSFlyCast (its shell/ps5/bigpicture.cpp,
// GPL-2.0-or-later, used here under GPL-3.0), made over for a PlayStation 2 emulator: the
// library with its tabs and its views (shelves, grid, list, flow, bookcase, table), a game's details, the loading screen, the quick
// menu over a paused game, the settings, the skins and the start-up animation. They replace
// PCSX2's own Big Picture interface (ImGui/FullscreenUI*.cpp), which this build leaves out.
//
// The screens know nothing of PCSX2. Everything they need of the emulator they ask of a Backend:
// the games, their covers, the settings (all games' and one game's), the running game and its
// saved states, the controllers. bp_pcsx2.cpp is that Backend on the console; the PC-side preview
// (ps5/tests/bigpicture) is another one, with invented games, so that every screen can be drawn,
// looked at and walked through on a PC.
//
// Threads: every function here is called on the thread that draws (PCSX2's graphics thread),
// between ImGui's NewFrame and Render, unless it says otherwise.

#pragma once

#include "imgui.h"

#include <climits>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bp
{

// ---------------------------------------------------------------------------------------- data

/// A picture the screens may draw this frame. No picture: id is ImTextureID().
struct Texture
{
	ImTextureID id = ImTextureID();
	float width = 0, height = 0;

	bool valid() const { return id != ImTextureID() && width > 0 && height > 0; }
};

/// A game of the library.
struct GameInfo
{
	enum Kind { PS2, PS1, Program };

	std::string path;         // as the emulator opens it
	std::string fileName;     // its last part
	std::string title;
	int source = 0;           // where it is: 0 the title's own folder, 1 a USB drive, 2 a network share,
	                          // 3 a disc drive, the console's own or a USB one (the screens put those on
	                          // DiscState::games themselves)
	bool cd = false;          // source 3: the disc is a CD (blue underside), not a DVD
	std::string serial;       // "SLUS-21376"; empty for a program
	std::string region;       // "NTSC-U", "PAL-E", ...; empty when not known
	uint32_t crc = 0;
	Kind kind = PS2;
	int compatibility = 0;    // 0 not known, else 1 (nothing works) to 6 (perfect)
	time_t lastPlayed = 0;    // 0: never
	time_t playedSeconds = 0;
	uint64_t size = 0;        // bytes
	bool favourite = false;   // marked as one (setFavourite)
	bool hidden = false;      // not to be listed (setHidden)
	time_t added = 0;         // when its file was last written; 0 when not known
	bool hasStates = false;   // a saved state of it is there
	bool hasCheats = false;   // a file of cheats for it is there
	bool hasTextures = false; // a texture pack for it is in the title folder
	bool renamed = false;     // its title is one it was given here (setTitle)
};

/// What a game is, from an encyclopedia (Backend::about).
struct GameAbout
{
	bool looking = false;     // being looked up now
	bool found = false;
	std::string summary;      // a paragraph or two
	std::string developer, publisher, released, genre, modes;
	std::string source;       // where it is from ("Wikipedia"), said under it
};

/// A saved state of a game.
struct StateInfo
{
	int slot = 0;             // 1..10; -1 is the state made when a game is closed, -2 the one made by
	                          // itself every few minutes
	bool backup = false;      // the state the slot held before it was last saved to
	time_t when = 0;
	std::string path;
};

/// The game that is running (or paused).
struct Running
{
	bool valid = false;       // a game is loaded
	bool stopping = false;    // none is any more, but the one that was is still being put away
	bool paused = false;
	std::string title, serial, path;
	uint32_t crc = 0;
	bool canSaveState = false;
	bool frameLimit = true;   // off: as fast as it goes
	bool hardware = true;     // the hardware renderer (not the software one)
	time_t sessionSeconds = 0;
};

/// The controller, as the screens read it: what is held now.
struct Pad
{
	enum : uint32_t
	{
		Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3,
		Cross = 1u << 4, Circle = 1u << 5, Square = 1u << 6, Triangle = 1u << 7,
		L1 = 1u << 8, R1 = 1u << 9, L2 = 1u << 10, R2 = 1u << 11,
		Options = 1u << 12, TouchPad = 1u << 13, L3 = 1u << 14, R3 = 1u << 15,
	};
	uint32_t held = 0;
	int connected = 1;        // how many controllers there are
};

/// One setting, described: what it is called, where it is kept and what it can be. The screens
/// draw it and change it through the Backend's values; they never know what it does.
struct Option
{
	enum Kind
	{
		Heading,      // a line that names the options under it
		Bool,
		IntList,      // one of `ints`, named by `names`
		FloatList,    // one of `floats`
		StringList,   // one of `strings`
		IntRange,     // lo..hi in steps
		FloatRange,   // lo..hi in steps, shown times `scale`
		Combo,        // one choice over several switches (`parts`)
		Action,       // something to run
		Info,         // a line of text
		Bind,         // a controller button for something (`key` in `section`)
	};
	enum : unsigned
	{
		AllGames = 1u << 0,    // not one a game can have of its own
		OneGame = 1u << 1,     // only a game can have it
		Advanced = 1u << 2,    // shown at the Advanced level and above (the Settings' Triangle)
		NextStart = 1u << 3,   // a running game takes it the next time it starts
		Restart = 1u << 4,     // takes effect the next time the title starts
		Front = 1u << 5,       // the front end's own (kept with the settings for all games)
		NoGame = 1u << 6,      // not while a game runs
		Expert = 1u << 7,      // shown at the Expert level only (the Settings' Triangle)
	};

	Kind kind = Info;
	std::string label, desc;
	std::string section, key;
	std::vector<std::string> names;
	std::vector<int> ints;
	std::vector<float> floats;
	std::vector<std::string> strings;
	bool defBool = false;
	int defInt = 0;
	float defFloat = 0;
	std::string defString;
	double lo = 0, hi = 100, step = 1;
	float scale = 1;
	std::string unit;          // after a range's value: "%", " ms", "px"
	int decimals = 0;          // of a float range's shown value
	unsigned flags = 0;
	/// Combo: the switches it is made of, and for each choice the value of each.
	struct Part
	{
		std::string section, key;
		bool def = false;
	};
	std::vector<Part> parts;
	std::vector<std::vector<int>> combos;
	std::function<void()> action;
	/// Info: the line's text. Action: what the action is about, shown on the row's right (it may be
	/// left out). Asked every frame the row is drawn, so the text may change while it is looked at:
	/// it must be cheap.
	std::function<std::string()> info;
	/// Bind: what kind of input it takes (the Backend's own number).
	int bindType = 0;
	/// Shown and changeable only while this says so (an option that depends on another).
	std::function<bool(int layer)> enabled;
};

/// A page of the settings made by the Backend (the controllers, the memory cards, the patches a
/// game has): its rows, made again whenever `generation` has moved.
struct Page
{
	std::string icon, name;
	std::vector<Option> rows;
};

/// What is being loaded, for the loading screen.
struct LoadState
{
	std::string text;         // "Reading the disc image into memory"
	float progress = -1;      // 0..1, or -1 when there is none to show
	std::string slow;         // a step that is taking long: what it is and what to look at ("" otherwise)
};

/// What the start-up has to be heard: the Backend plays (or does not play) a sound for each.
enum class Sound
{
	StartupBegins,  // the start-up animation's time 0: the first of its frames that moves
	StartupSkipped, // a button cut the animation short; the library is there 0.8 s later
	LibraryOpens,   // the start-up is over (or there was none) and the library is the screen
};

/// The disc drives (the console's own and USB DVD drives), and the discs in them.
struct DiscState
{
	unsigned generation = 0;  // bumped whenever anything else here has changed
	bool driveFound = false;  // there is a drive (the screens do not ask; the Backend's pages may say)
	bool present = false;     // a disc is in a drive, and it is known what it is
	bool playable = false;    // one the emulator can start is in a drive
	GameInfo game;            // playable: the first of `games`
	std::vector<GameInfo> games; // every disc that can be played, one a drive, the console's drive first;
	                          // each like a game of the library, its path naming its drive
	std::string note;         // a disc that cannot be played: why, as a sentence ("This is not a PlayStation 2 disc.")
};

// ------------------------------------------------------------------------------------- backend

/// The settings a value is read from and written to (the `layer` of the Backend's functions).
enum Store
{
	Shared = 0, // the settings of every game
	Own = 1,    // the game opened with Backend::openGame: only what it has of its own
};

class Backend
{
public:
	virtual ~Backend() = default;

	// ---- the library
	/// Bumped whenever games() would return something else.
	virtual unsigned gamesGeneration() = 0;
	virtual std::vector<GameInfo> games() = 0;
	/// The game's cover, once it is loaded; no picture until then, or when it has none.
	virtual Texture cover(const GameInfo& game) = 0;
	/// Marks a game as a favourite, or as one not to be listed; gamesGeneration() moves.
	virtual void setFavourite(const GameInfo& game, bool favourite) = 0;
	virtual void setHidden(const GameInfo& game, bool hidden) = 0;
	/// Gives a game another title in the lists; an empty one gives it back its own.
	/// gamesGeneration() moves.
	virtual void setTitle(const GameInfo& game, const std::string& title) = 0;
	/// A picture of the game other than its cover, once it is loaded: 0 its title screen, 1 a
	/// moment of play. No picture until then, or when it has none.
	virtual Texture picture(const GameInfo& game, int which) = 0;
	/// Fetches those pictures: of one game (`again`: also when nothing was found for it before),
	/// or with no game of every game that has none. Returns at once; backgroundNote() says how it
	/// goes, and a toast what came of it.
	virtual void fetchPictures(const GameInfo* game, bool again) = 0;
	/// The user's own pictures for behind the screens: how many there are, and one of them once
	/// it is loaded (no picture until then). `again`: the folder is looked into anew.
	virtual int wallpapers(bool again) = 0;
	virtual Texture wallpaper(int index) = 0;
	/// Looks for games again. `forget`: every file is read anew, not only the new ones.
	virtual void scan(bool forget) = 0;
	virtual bool scanning() = 0;
	/// What goes on in the background, to show quietly: "Downloading covers (3 left)"; empty for nothing.
	virtual std::string backgroundNote() = 0;
	/// The folders the library's tabs stand for, as the user reaches them.
	virtual std::string shownFolder(const char* which) = 0; // "games", "bios", "covers", "root"
	virtual bool usbWanted() = 0;        // the USB tab is there
	virtual bool usbAvailable() = 0;     // and the title could leave its sandbox for it
	virtual std::vector<std::string> networkFolders() = 0;

	// ---- starting and the running game
	/// Starts a game. `state`: INT_MIN from its beginning, -1 from the state made when it was
	/// last closed, 1..10 from that slot. `boot`: 0 as the settings say, 1 fast, 2 the whole
	/// start-up of the console.
	virtual void start(const GameInfo& game, int state, int boot) = 0;
	virtual void startStateFile(const GameInfo& game, const std::string& statePath) = 0;
	/// Starts the console with no disc in it.
	virtual void startBios() = 0;
	virtual Running running() = 0;
	/// Whether a game is being started (start() was called, and it neither runs nor failed yet).
	virtual bool starting() = 0;
	virtual LoadState loadState() = 0;
	virtual void setPaused(bool paused) = 0;
	virtual void saveState(int slot) = 0;
	virtual void loadStateFile(const StateInfo& state) = 0;
	virtual void reset() = 0;
	virtual void closeGame(bool saveState) = 0;
	virtual void changeDisc(const std::string& path) = 0;
	virtual void setFrameLimit(bool on) = 0;
	virtual void toggleSoftwareRenderer() = 0;
	/// Whether a memory card is being written to right now (closing the game would spoil it).
	virtual bool memoryCardBusy() = 0;
	/// Whether the running game can be put back to where it was before a state was last loaded
	/// into it, and doing so.
	virtual bool canUndoLoad() = 0;
	virtual void undoLoad() = 0;
	/// What the running game has at RetroAchievements, as bits: 1 achievements, 2 leaderboards;
	/// 0 when it has none, or nobody is signed in.
	virtual int achievementsOf() = 0;
	/// Opens the emulator's own list of them (1) or of the leaderboards (2) over the paused game.
	/// While overlayOpen() says so, that list has the screen and the controller: the screens draw
	/// nothing and read no button.
	virtual void openAchievements(int which) = 0;
	virtual bool overlayOpen() = 0;
	/// Where the running game's picture is on the screen, in pixels: left, top, right, bottom.
	/// False when that is not known (no frame was shown yet).
	virtual bool gameRect(float rect[4]) = 0;
	/// Puts the emulator's own figures on the screen (the speed, the frame rate) in another corner:
	/// 0 top left, 1 top right, 2 bottom right, 3 bottom left; -1 where the settings have them.
	virtual void overlayCorner(int corner) = 0;

	// ---- saved states
	virtual std::vector<StateInfo> states(const std::string& serial, uint32_t crc, bool withBackups) = 0;
	virtual Texture statePicture(const StateInfo& state) = 0;
	virtual bool deleteState(const StateInfo& state) = 0;
	virtual void resetPlayTime(const GameInfo& game) = 0;

	// ---- settings
	/// Opens a game's own settings as the Own layer. False if they cannot be (no serial and no CRC).
	virtual bool openGame(const GameInfo& game) = 0;
	virtual void closeGame() = 0;
	virtual std::optional<bool> getBool(int layer, const char* section, const char* key) = 0;
	virtual std::optional<int> getInt(int layer, const char* section, const char* key) = 0;
	virtual std::optional<float> getFloat(int layer, const char* section, const char* key) = 0;
	virtual std::optional<std::string> getString(int layer, const char* section, const char* key) = 0;
	virtual void setBool(int layer, const char* section, const char* key, bool value) = 0;
	virtual void setInt(int layer, const char* section, const char* key, int value) = 0;
	virtual void setFloat(int layer, const char* section, const char* key, float value) = 0;
	virtual void setString(int layer, const char* section, const char* key, const std::string& value) = 0;
	virtual void erase(int layer, const char* section, const char* key) = 0;
	/// Every value of the game's own, gone.
	virtual void clearGame() = 0;
	/// How many values the open game has of its own.
	virtual int ownCount() = 0;

	// ---- pages the Backend makes
	/// Bumped whenever a page below would be made of other rows.
	virtual unsigned pagesGeneration() = 0;
	virtual Page controllersPage(int layer) = 0;
	virtual Page hotkeysPage() = 0;
	virtual Page memoryCardsPage(int layer) = 0;
	virtual Page biosPage(int layer) = 0;
	/// The PlayStation 2's network adapter and hard drive, after BIOS in the settings of all games
	/// and of one game. No rows: no such page. No icon or name: the network's, "Network".
	virtual Page networkPage(int layer) = 0;
	virtual Page patchesPage(bool cheats) = 0; // of the open game
	/// Backups of the memory cards and the saved states, after Memory cards in the settings of all
	/// games. No rows: no such page.
	virtual Page backupPage() = 0;
	/// The logs, before About in the settings of all games. No rows: no such page.
	virtual Page logPage() = 0;
	/// RetroAchievements: the account and its switches, after Network in the settings of all games.
	/// No rows: no such page.
	virtual Page achievementsPage() = 0;
	/// What the Backend can do for the open game, as rows of its "This game" page (after "Options
	/// of its own"): the settings its database recommends for it.
	virtual std::vector<Option> gameRows() = 0;
	virtual Page aboutPage() = 0;
	/// Extra rows for the Interface page: what the port itself can be told.
	virtual std::vector<Option> portRows() = 0;

	// ---- controller buttons
	/// The button(s) a Bind option is on, as text; empty when none.
	virtual std::string binding(int layer, const Option& option) = 0;
	/// Waits for a button and puts the option on it. Until listening() is over, no screen reads
	/// the controller.
	virtual void listen(int layer, const Option& option) = 0;
	virtual float listening() = 0;   // seconds left, or 0 when not listening
	virtual void cancelListen() = 0;
	virtual void clearBinding(int layer, const Option& option) = 0;

	// ---- the rest
	virtual Pad pad() = 0;
	virtual void quit() = 0;
	virtual void log(const std::string& line) = 0;
	/// The title's own name for itself, as it is written in sentences ("Close Crux2?"). Its mark
	/// and the name as the top bar has it are the screens' own drawing (bp_draw.cpp).
	virtual const char* appName() = 0;
	/// A moment of the start-up, to be heard. Each is told once a start at most, in the order of
	/// the enum; StartupBegins and StartupSkipped only when the animation plays (it does not with
	/// "Start-up animation" off, nor with Motion on "Less" or "None"). Must return at once: the
	/// frame it is called from is being drawn.
	virtual void sound(Sound what) = 0;
	/// The disc drive as it is now. Asked once every frame, whatever screen is on and while a game
	/// runs: it answers from what the Backend already knows, and never reads the drive itself.
	virtual DiscState disc() = 0;

	// ---- what the console adds (the preview has none of it: these have plain defaults)
	/// Rows the Backend adds to a page of the settings, by the page's place: "motion", "picture",
	/// "controllers", "saves", "network", "interface", "system", "library". `layer`: Shared or Own.
	virtual std::vector<Option> extraRows(const char* where, int layer) { return {}; }
	/// HD texture packs from the Internet Archive for the game whose options are open: looks for
	/// its packs and asks which and where; the download that runs, and stopping it.
	virtual void findTexturePacks() {}
	virtual std::string texturePackDoing() { return {}; }
	virtual void stopTexturePack() {}
	/// An interface sound (Cue below). Returns at once.
	virtual void cue(int which) {}
	/// What was typed on a keyboard (a USB one, or the phone's) since the last call: the text, and
	/// keys as bits (1 Enter, 2 Backspace, 4 Escape). False when nothing was.
	virtual bool typed(std::string& text, unsigned& keys) { return false; }
	/// The keyboard types into the interface (true while a text is being typed), not into the game.
	virtual void captureKeyboard(bool on) {}
	/// Once a frame, before any screen is drawn: the Backend's own work that wants the drawing
	/// thread (the phone's requests).
	virtual void frame() {}
	/// What a game is (the Disc tab says it): looked up the first time it is asked for, so the
	/// first answers are empty with `looking` set. Cheap: every frame.
	virtual GameAbout about(const GameInfo& game) { return {}; }
	/// Makes an image of the disc in the drive of this game (source 3): asks which kind and where.
	virtual void makeImage(const GameInfo& game) {}
	/// An image of a disc is being made.
	virtual bool imageBusy() { return false; }
};

/// The interface's sounds (Backend::cue).
enum Cue
{
	CueFocus, CueSelect, CueBack, CueTab, CueOpen, CueModalOpen, CueModalClose, CueToggle, CueSlider, CueError, CueNotify,
	CueLaunch, CueFavouriteOn, CueFavouriteOff, CueSaved, CueWelcome, CueResume,
};

// ------------------------------------------------------------------------- the memory cards
//
// The saves on the memory cards (bp_cards.cpp): a card's menu, from which a save is brought in
// from a file, copied out to one, copied to another card or deleted, and the card itself renamed,
// copied or deleted. The Backend's Memory cards page has a row for each card that opens it
// (openCard), and gives it the cards through this. Whatever is asked for returns at once; what
// came of it is said in a toast or a message, and pagesGeneration() moves.

struct CardSave
{
	std::string name;   // its folder on the card: "BESLES-12345SAVE"
	std::string title;  // what it calls itself; may be empty
	std::string when;   // "2026-10-07 14:32"
	uint32_t bytes = 0; // what it takes of the card
};

struct CardView
{
	enum State
	{
		Listed,  // its saves are in `saves`
		New,     // never formatted: nothing on it yet
		Closed,  // it cannot be looked into; `why` says why
	};
	State state = Closed;
	std::string why;
	uint32_t freeBytes = 0, totalBytes = 0;
	bool undo = false; // the card as it was before its last change is kept
	std::vector<CardSave> saves;
};

/// Something found where saves are brought in from (v1.5.0).
struct CardFound
{
	enum Kind
	{
		SaveFile,   // .psu .max .cbs .sps .xps .psv
		SaveFolder, // a save copied off a card as it is
		CardImage,  // a whole card
		Ps2Save,    // a save on a real PlayStation 2's card
	};
	Kind kind = SaveFile;
	std::string path, name, where;
	uint64_t bytes = 0;
	std::string ps2; // the PlayStation 2's address
};

/// A save file found where they are looked for.
struct SaveFileFound
{
	std::string path;
	std::string name;  // the file's
	std::string where; // "USB drive 1", "saves folder"
	uint64_t bytes = 0;
};

class CardShelf
{
public:
	virtual ~CardShelf() = default;
	/// The card files there are, as the slots name them ("Card 1.ps2").
	virtual std::vector<std::string> cardNames() = 0;
	virtual CardView lookAtCard(const std::string& card) = 0;
	/// Why no card may be changed now ("Close the game first: ..."); empty when they may.
	virtual std::string cardsBusy() = 0;
	/// Looks for save files in the places they are brought in from. Returns at once; `then` gets
	/// what was found, on the thread that draws.
	virtual void findSaveFiles(std::function<void(std::vector<SaveFileFound>)> then) = 0;
	/// Where those places are, as a sentence for when nothing was found.
	virtual std::string saveFilePlaces() = 0;
	/// The places a save can be written out to, by name; the first is always there.
	virtual std::vector<std::string> exportPlaces() = 0;
	virtual void importSave(const std::string& card, const SaveFileFound& file) = 0;
	virtual void exportSave(const std::string& card, const std::string& save, int place) = 0;
	virtual void copySave(const std::string& card, const std::string& save, const std::string& toCard) = 0;
	virtual void deleteSave(const std::string& card, const std::string& save) = 0;
	virtual void undoCard(const std::string& card) = 0;
	/// `title`: the new name without its ending, as it was typed.
	virtual void renameCard(const std::string& card, const std::string& title) = 0;
	virtual void duplicateCard(const std::string& card) = 0;
	virtual void deleteCard(const std::string& card) = 0;

	// ---- v1.5.0: bringing saves and whole cards in from anywhere, new cards, formatting

	/// The places saves and cards are brought in from, by name; the first is a PlayStation 2 on
	/// the network (asked for by its address).
	virtual std::vector<std::string> cardSources() = 0;
	/// The address of the PlayStation 2 last asked ("" when none was).
	virtual std::string ps2Address() = 0;
	/// Looks in a source (an index into cardSources(); `address` for the PlayStation 2). `then`,
	/// on the thread that draws, gets what was found, or why nothing could be.
	virtual void findIn(int source, const std::string& address, std::function<void(std::vector<CardFound>, std::string why)> then) = 0;
	/// A save of any kind brought onto a card.
	virtual void bringIn(const std::string& card, const CardFound& found) = 0;
	/// A card image brought in as a new card.
	virtual void bringInCard(const CardFound& image, const std::string& title) = 0;
	/// What is on a card image found somewhere (not one of the cards).
	virtual CardView lookAtImage(const CardFound& image) = 0;
	virtual void copyFromImage(const CardFound& image, const std::string& save, const std::string& toCard) = 0;
	/// A new card: 8, 16, 32 or 64 MB, or 0 for a PlayStation 1 card.
	virtual void createCard(const std::string& title, int megabytes) = 0;
	virtual void formatCard(const std::string& card) = 0;
	/// Copies the card's file out to one of exportPlaces() (not the first: a drive or a share).
	virtual void exportCard(const std::string& card, int place) = 0;
	/// Which slot holds a card: 1, 2, or 0.
	virtual int slotOf(const std::string& card) = 0;

	// ---- v1.6.0: the card manager (openCardManager)

	/// The game a card is the card of, by its serial ("SCUS-97580.ps2" is SingStar's), as the game
	/// database names it; empty for a card of no game.
	virtual std::string cardGame(const std::string& card) { return std::string(); }
	/// The serial a game's card is named by ("SCUS-97580"); empty for a card of no game.
	virtual std::string cardSerial(const std::string& card) { return std::string(); }
	/// Whether the card is one of the emulator's own first two (Mcd001, Mcd002).
	virtual bool systemCard(const std::string& card) { return false; }
	/// The card's size in MB, or 0 for a PlayStation 1 card, -1 for a folder card.
	virtual int cardMegabytes(const std::string& card) { return 8; }
};

/// A card's name as it is shown: without its ending.
std::string cardTitle(const std::string& card);
/// What a card holds, as a row of a page says it: "3 saves, 7.1 MB free".
std::string cardSummary(const CardView& view);
/// Opens a card's menu.
void openCard(CardShelf* shelf, const std::string& card);
/// Asks where from, then what, and brings saves onto a card.
void bringSavesInto(CardShelf* shelf, const std::string& card);
/// Asks where from, then which card image, and brings it in as a new card.
void bringCardsIn(CardShelf* shelf);
/// Asks for a size and a name, and makes a card.
void newCard(CardShelf* shelf);
/// The card manager (v1.6.0): the cards on the left, by game (its cover and title), the cards made
/// or brought in, and the emulator's own two under System; the saves of the card in focus on the
/// right. It is over whatever screen opened it, until Circle closes it.
void openCardManager(CardShelf* shelf);

// ----------------------------------------------------------------------------------- the front

/// Once, before the first frame.
void initialize(Backend* backend);
void shutdown();

/// Draws whatever screen is on. Once a frame.
void render();
/// Whether a screen is on: the controller is the interface's then, not the game's.
bool active();

/// The emulator's events. On the drawing thread.
void gameStarted();
void gameStopped();
void gameResumed();
/// The quick menu's button was let go while a game is loaded: over the game it opens the menu;
/// in the menu, when it was pressed there too, it closes it again.
void openQuickMenu();
/// A start that did not work: back to the library, with the reason.
void startFailed(const std::string& why);

/// Something to tell the user: a dialog to acknowledge, a question, or a line that goes by itself.
void message(const std::string& title, const std::string& text, std::function<void()> then = {});
void confirm(const std::string& title, const std::string& text, std::function<void(bool)> then, const char* yes = "Yes",
	const char* no = "No");
void toast(const std::string& text, float seconds = 5.f);
/// A list to choose from, over whatever screen is on. `current`: the row it opens on, or -1 for
/// the first. `then` gets the index chosen; it is not called when the list is left.
void pick(const std::string& title, std::vector<std::string> names, int current, std::function<void(int)> then);
/// An IPv4 address, changed number by number with the controller. It opens on `now` ("a.b.c.d"),
/// or on `from` when `now` is no address. `then` gets the address accepted, or an empty text when
/// Square took it away ("Not set"); it is not called when the dialog is left.
void editAddress(const std::string& title, const std::string& now, const std::string& from, std::function<void(const std::string&)> then);
/// The four numbers of "a.b.c.d". False when the text is not such an address.
bool parseAddress(const std::string& text, int numbers[4]);

/// A line of text, typed on a keyboard drawn on the screen: the directions move over its keys,
/// Cross types one, Square takes the last letter away, Triangle is the space, L1 the capitals,
/// R1 the signs, Options accepts and Circle leaves. `secret`: a password, shown as dots (L2
/// shows and hides it). It opens on `now`; `then` gets what was accepted, and is not called when
/// the keyboard is left. At most `most` characters.
void editText(const std::string& title, const std::string& now, bool secret, std::function<void(const std::string&)> then, int most = 64);

/// Lines of text to read, in a sheet nearly as large as the screen: a log. It opens at its end
/// (the newest lines); the directions, L1 / R1 (a page) and L2 / R2 (the start, the end) move
/// through it, Circle closes it. A line too long for the sheet goes on on the next.
void showText(const std::string& title, std::vector<std::string> lines);

/// A QR code with lines of text beside it (the address to open on a phone): `modules` is size x
/// size, one byte a module, non-zero dark. Circle or Cross closes it.
void showCode(const std::string& title, const std::string& text, std::vector<uint8_t> modules, int size);

/// The fonts the screens draw with (the Backend's renderer has them).
void setFonts(ImFont* regular, ImFont* bold);
/// The face the title's name is set in.
void setBrandFont(ImFont* brand);

} // namespace bp
