// Crux2 - the interface: Home, the first screen after the start-up.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Home: the game played last, to go on with (Resume starts it from its newest saved state; Game page
// opens its page in the library); what goes on right now (a texture pack being downloaded, a disc in
// a drive, the share, a game running); and the games played lately, a row of covers. With no game
// played yet, or none at all, it says so and leads to the library.
//
// It is a tab of the library screen (libraryScreen hands it the frame): L1 / R1 go on to the
// other tabs, OPTIONS opens the settings, as everywhere in the library. While a texture pack is
// being downloaded (or moved, or waits), the "Right now" card can have the focus: Cross on it opens
// the texture pack manager.

#include "bp_internal.h"
#include "bp_settings_model.h"

#include <ctime>
#include <map>

namespace bp
{

namespace
{

using settings::Palette;
using settings::palette;
using settings::focusRing;

// Where the focus is on Home: the hero's buttons (0 Resume, 1 Game page) and the "Right now" card
// (while it has texture packs to show: Cross opens their manager), or a cover of the row.
struct HomeState
{
	int row = 0;      // 0 the hero, 1 the covers
	int button = 0;
	int cover = 0;
	float scroll = 0; // the covers' row, in covers
	std::string heroPath;
	float heroFade = 1;
} hs;

constexpr int MOST_RECENT = 12;
constexpr float X0 = 72;

// "12 minutes ago", "yesterday", or the date.
std::string ago(time_t when)
{
	if (when <= 0)
		return "";
	const long long d = static_cast<long long>(time(nullptr) - when);
	char out[48];
	if (d < 0)
		return formatWhen(when);
	if (d < 60)
		return "just now";
	if (d < 3600)
		std::snprintf(out, sizeof(out), "%lld minute%s ago", d / 60, d / 60 == 1 ? "" : "s");
	else if (d < 86400)
		std::snprintf(out, sizeof(out), "%lld hour%s ago", d / 3600, d / 3600 == 1 ? "" : "s");
	else if (d < 2 * 86400)
		return "yesterday";
	else if (d < 14 * 86400)
		std::snprintf(out, sizeof(out), "%lld days ago", d / 86400);
	else
		return formatWhen(when);
	return out;
}

// A game's newest state to resume from, asked of the Backend again only when the games changed or
// a few seconds went by (it reads the states' folder).
bool savedState(const GameInfo& game, StateInfo& out)
{
	struct Known
	{
		bool found = false;
		StateInfo state;
		unsigned generation = 0;
		double at = -100;
	};
	static std::map<std::string, Known> known;
	if (known.size() > 64)
		known.clear();
	Known& k = known[game.path];
	const unsigned generation = be->gamesGeneration();
	if (k.generation != generation || timeNow - k.at > 5.0)
	{
		k.generation = generation;
		k.at = timeNow;
		k.found = newestState(game, k.state);
	}
	out = k.state;
	return k.found;
}

std::string joined(const std::vector<std::string>& parts)
{
	std::string out;
	for (const std::string& p : parts)
	{
		if (!p.empty())
			out += (out.empty() ? "" : "  \xc2\xb7  ") + p;
	}
	return out;
}

void card(ImDrawList* dl, float x, float y, float w, float h, ImU32 c)
{
	settings::cardShape(dl, x, y, w, h, c, 16);
}

// A pill button; its width. `main`: the accent's.
float button(ImDrawList* dl, float x, float y, const char* label, bool main, bool focus, float h = 70)
{
	const Palette p = palette();
	const ImVec2 ls = textSize(bold(), 26, label);
	const float w = ls.x + 72;
	rect(dl, x, y, w, h, main ? p.accent : p.raised, h / 2);
	text(dl, bold(), 26, x + 36, y + h / 2 - ls.y / 2, main ? p.onAccent : p.text, label);
	if (focus)
		focusRing(dl, x, y, w, h, h / 2);
	return w;
}

// ---- what goes on right now: only what is so

struct Fact
{
	std::string key, value;
	float progress = -1; // a bar under it: 0..1, or -1 none (2: going, not known how far)
	bool packs = false;  // of the texture packs: the card opens their manager
};

// Whether the card has texture packs to show (it can then have the focus).
bool packsNow()
{
	TexturePackShelf* const shelf = be->texturePackShelf();
	return shelf != nullptr &&
		   (!be->texturePackBar().title.empty() || !shelf->packJob().empty() || !shelf->queuedDownloads().empty());
}

std::vector<Fact> rightNow()
{
	std::vector<Fact> facts;
	const Running running = be->running();
	if (running.valid)
		facts.push_back({"Game running", !running.title.empty() ? running.title : running.serial});
	// A texture pack being downloaded, and what waits after it.
	const Backend::DownloadBar bar = be->texturePackBar();
	TexturePackShelf* const shelf = be->texturePackShelf();
	const size_t queued = (shelf != nullptr) ? shelf->queuedDownloads().size() : 0;
	if (!bar.title.empty())
	{
		const float done = (bar.total > 0) ? std::min(1.f, static_cast<float>(static_cast<double>(bar.got) / static_cast<double>(bar.total))) : -1.f;
		std::string value = bar.title;
		if (done >= 0)
			value += "  \xc2\xb7  " + std::to_string(static_cast<int>(done * 100)) + "%";
		facts.push_back({queued > 0 ? "Texture packs (" + std::to_string(queued + 1) + ")" : std::string("Texture pack"), value, done >= 0 ? done : 2.f, true});
	}
	else if (shelf != nullptr && !shelf->packJob().empty())
	{
		facts.push_back({"Texture packs", shelf->packJob(), 2.f, true});
	}
	else if (queued > 0)
	{
		facts.push_back({"Texture packs", std::to_string(queued) + " waiting to download", -1, true});
	}
	// The disc in a drive.
	const DiscState& disc = discNow();
	if (disc.present)
	{
		if (!disc.games.empty())
		{
			const GameInfo& g = disc.games.front();
			facts.push_back({disc.games.size() > 1 ? "Discs" : (g.cd ? "CD in the drive" : "Disc in the drive"),
				disc.games.size() > 1 ? std::to_string(disc.games.size()) + " in the drives" : (!g.title.empty() ? g.title : g.serial)});
		}
		else
		{
			facts.push_back({"Disc in the drive", "Not a PlayStation 2 game"});
		}
	}
	// The library's places.
	if (be->scanning())
		facts.push_back({"Library", "Looking for games", 2.f});
	const std::vector<std::string> shares = be->networkFolders();
	if (!shares.empty())
	{
		const int n = libraryCount(Network);
		facts.push_back({"Share", n > 0 ? joined({shares.front(), std::to_string(n) + (n == 1 ? " game" : " games")}) :
										be->scanning() ? std::string("Being read") : std::string("No games found on it")});
	}
	if (be->usbWanted())
	{
		const int n = libraryCount(Usb);
		facts.push_back({"USB", !be->usbAvailable() ? std::string("Not available") : n > 0 ? std::to_string(n) + (n == 1 ? " game" : " games") : std::string("No games found")});
	}
	{
		const int n = libraryCount(Internal);
		facts.push_back({"Console", std::to_string(n) + (n == 1 ? " game" : " games")});
	}
	const std::string background = be->backgroundNote();
	if (!background.empty())
		facts.push_back({"Now", background});
	return facts;
}

void drawRightNow(ImDrawList* dl, float x, float y, float w, float h, bool focus)
{
	const Palette p = palette();
	card(dl, x, y, w, h, focus ? p.raised : p.card);
	if (focus)
		focusRing(dl, x, y, w, h, 16);
	text(dl, bold(), 20, x + 36, y + 34, p.secondary, "RIGHT NOW");
	const std::vector<Fact> facts = rightNow();
	const float rowH = 56;
	float ry = y + 78;
	for (const Fact& f : facts)
	{
		if (ry + rowH > y + h - 16)
			break;
		const float kw = std::min(textSize(regular(), 22, f.key.c_str()).x, w * 0.42f);
		textIn(dl, regular(), 22, x + 36, ry + 10, p.secondary, f.key, kw, false);
		const float room = w - 72 - kw - 20;
		const float vw = std::min(textSize(bold(), 22, f.value.c_str()).x, room);
		textIn(dl, bold(), 22, x + w - 36 - vw, ry + 10, p.text, f.value, vw, true);
		if (f.progress >= 0)
		{
			const float bx = x + 36, bw = w - 72, by = ry + rowH - 12;
			rect(dl, bx, by, bw, 5, alpha(p.text, 0.12f), 2.5f);
			if (f.progress <= 1)
				rect(dl, bx, by, std::max(6.f, bw * f.progress), 5, p.accent, 2.5f);
			else
			{
				const float t = static_cast<float>(std::fmod(timeNow, 1.6) / 1.6);
				const float f0 = std::max(0.f, t * 1.3f - 0.3f), f1 = std::min(1.f, t * 1.3f);
				rect(dl, bx + bw * f0, by, std::max(6.f, bw * (f1 - f0)), 5, p.accent, 2.5f);
			}
		}
		else
		{
			rect(dl, x + 36, ry + rowH - 4, w - 72, 1, p.line);
		}
		ry += rowH;
	}
}

// ---- Home

void homeInput(const std::vector<HomeGame>& row, const HomeGame* hero, bool anyGames)
{
	const int covers = static_cast<int>(row.size());
	hs.cover = std::clamp(hs.cover, 0, std::max(0, covers - 1));
	if (covers == 0)
		hs.row = 0;
	if (in.up)
		hs.row = 0;
	if (in.down && covers > 0)
		hs.row = 1;
	if (hs.row == 0)
	{
		// (The hero's buttons, then the "Right now" card while it has texture packs to show.)
		const int buttons = ((hero != nullptr) ? 2 : 1) + (packsNow() ? 1 : 0);
		if (in.left && hs.button > 0)
			hs.button--;
		if (in.right && hs.button < buttons - 1)
			hs.button++;
		hs.button = std::clamp(hs.button, 0, buttons - 1);
	}
	else
	{
		if (in.left && hs.cover > 0)
			hs.cover--;
		if (in.right && hs.cover < covers - 1)
			hs.cover++;
	}
	const GameInfo* focused = (hs.row == 1 && covers > 0) ? row[static_cast<size_t>(hs.cover)].info : hero != nullptr ? hero->info : nullptr;
	const bool onCard = (hs.row == 0 && hs.button == ((hero != nullptr) ? 2 : 1));
	if (in.accept && onCard)
	{
		if (TexturePackShelf* shelf = be->texturePackShelf())
			openTexturePacks(shelf);
		return;
	}
	if (in.accept)
	{
		if (hs.row == 0 && hero == nullptr)
			showLibraryTab(TabLibrary);
		else if (hs.row == 0 && hs.button == 0)
			resumeFromNewest(*hero->info);
		else if (hs.row == 0)
			goToGame(*hero->info, true);
		else if (focused != nullptr && !anyGames)
			showLibraryTab(TabLibrary);
		else if (focused != nullptr)
		{
			// A game of the row: from its newest state, as Resume, when it has one (they were played).
			if (hs.row == 1 && !row.empty() && row[static_cast<size_t>(std::clamp(hs.cover, 0, static_cast<int>(row.size()) - 1))].lastPlayed > 0)
				resumeFromNewest(*focused);
			else
				goToGame(*focused, true);
		}
		return;
	}
	if (in.triangle && focused != nullptr)
	{
		goToGame(*focused, true);
		return;
	}
	if (in.square && focused != nullptr)
	{
		// The game's own settings (the game page's Options).
		openSettings(focused, Screen::Library);
		return;
	}
	if (in.options)
	{
		openSettings(nullptr, Screen::Library);
		return;
	}
	if (in.back)
	{
		confirm(std::string("Close ") + be->appName() + "?", "Are you sure?", [](bool yes) {
			if (yes && be != nullptr)
				be->quit();
		}, "Yes, close it", "No");
		return;
	}
	if (in.l1 || in.r1)
		stepTab(in.r1 ? 1 : -1);
}

void drawHero(ImDrawList* dl, const HomeGame* hero, float x, float y, float w, float h, bool anyGames)
{
	const Palette p = palette();
	card(dl, x, y, w, h, p.card);
	const float pad = 48;
	if (hero == nullptr)
	{
		// Nothing played yet, or no game at all: what there is to do.
		cruxMark(dl, x + w - 220, y + h / 2, 230, 0.5f);
		const bool busy = be->scanning();
		text(dl, bold(), 20, x + pad, y + 48, p.accent, anyGames ? "START" : busy ? "ONE MOMENT" : "WELCOME");
		const char* title = anyGames ? "Nothing played yet" : busy ? "Looking for games" : "Your library is empty";
		textIn(dl, bold(), 62, x + pad, y + 84, p.text, title, w - pad * 2 - 300, false);
		std::string body;
		if (anyGames)
			body = "Choose one of your " + std::to_string(libraryCount()) + " games in the library. What you play last waits for you here.";
		else if (busy)
			body = "The folders, the USB drives and the share are being read.";
		else
			body = "Copy your games to " + be->shownFolder("games") + ", put them on a USB drive, or name a network share. Disc images: .iso .chd .cso .zso .bin/.cue";
		text(dl, regular(), 24, x + pad, y + 176, p.secondary, body.c_str(), w - pad * 2 - 300);
		button(dl, x + pad, y + h - pad - 70, ICON_FA_TABLE_CELLS_LARGE "   Open the library", true, hs.row == 0 && hs.button == 0);
		return;
	}
	const GameInfo& g = *hero->info;
	// The game's picture of play (else its cover), fading in from the card's left.
	{
		if (g.path != hs.heroPath)
		{
			hs.heroPath = g.path;
			hs.heroFade = (motion() == MotionOff) ? 1.f : 0.f;
		}
		hs.heroFade = approach(hs.heroFade, 1, 12);
		Texture art = options().heroPlay ? be->picture(g, 1) : Texture();
		if (!art.valid())
			art = be->picture(g, 0);
		if (!art.valid())
			art = be->cover(g);
		if (art.valid())
		{
			dl->PushClipRect(V(x, y), V(x + w, y + h), true);
			imageFade(dl, art, x + w * 0.38f, y, w * 0.62f, h, 0.55f * hs.heroFade, {0.f, 0.45f, 1.f}, {0.f, 1.f, 1.f}, {0.f, 0.5f, 1.f}, {0.6f, 1.f, 0.6f});
			dl->PopClipRect();
		}
	}
	text(dl, bold(), 20, x + pad, y + 48, p.accent, "CONTINUE");
	textIn(dl, bold(), 66, x + pad, y + 80, p.text, hero->title, w - pad * 2, true);
	StateInfo state;
	const bool saved = savedState(g, state);
	const std::string played = formatPlayed(hero->playedSeconds);
	const std::string meta = joined({saved ? "Saved " + ago(state.when) : "Last played " + ago(hero->lastPlayed), played.empty() ? std::string() : "Played " + played, g.serial});
	textIn(dl, regular(), 24, x + pad, y + 172, p.secondary, meta, w - pad * 2, false);
	const float by = y + h - pad - 70;
	const float rw = button(dl, x + pad, by, saved ? ICON_FA_PLAY "   Resume" : ICON_FA_PLAY "   Play", true, hs.row == 0 && hs.button == 0);
	button(dl, x + pad + rw + 18, by, "Game page", false, hs.row == 0 && hs.button == 1);
}

void drawRow(ImDrawList* dl, const std::vector<HomeGame>& row, bool played, float y)
{
	const Palette p = palette();
	// (With no game at all the hero says what to do; there is no row to show.)
	if (row.empty() && !played)
		return;
	text(dl, bold(), 30, X0, y, p.text, played ? "Recently played" : "Newest in your library");
	const int total = libraryCount();
	if (total > 0)
	{
		const std::string count = std::to_string(row.size()) + " of " + std::to_string(total) + (total == 1 ? " game" : " games");
		const float tw = textSize(bold(), 30, played ? "Recently played" : "Newest in your library").x;
		text(dl, regular(), 22, X0 + tw + 20, y + 6, p.secondary, count.c_str());
	}
	const float cw = 222, ch = std::round(cw * 1.32f), gap = 24, top = y + 54;
	if (row.empty())
	{
		text(dl, regular(), 24, X0, top + 20, p.faint, "The games you play are put here, the latest first.");
		return;
	}
	const float visible = (W - 2 * X0 + gap) / (cw + gap);
	const float want = std::clamp(static_cast<float>(hs.cover) - visible + 2.f, 0.f, std::max(0.f, static_cast<float>(row.size()) - visible));
	hs.scroll = (motion() == MotionOff) ? want : approach(hs.scroll, want, 12);
	dl->PushClipRect(V(X0 - 20, top - 20), V(W - X0 + 20, H - 72), true);
	for (size_t i = 0; i < row.size(); i++)
	{
		const float x = X0 + (static_cast<float>(i) - hs.scroll) * (cw + gap);
		if (x > W || x + cw < 0)
			continue;
		const bool on = (hs.row == 1 && static_cast<int>(i) == hs.cover);
		drawHomeCover(dl, row[i], x, top, cw, ch, 14, on ? 1.f : 0.f);
		if (on)
			focusRing(dl, x, top, cw, ch, 14);
		textIn(dl, bold(), 21, x, top + ch + 14, on ? p.text : p.secondary, row[i].title, cw, on);
	}
	dl->PopClipRect();
}

} // namespace

void homeFrame(ImDrawList* dl)
{
	const std::vector<HomeGame> played = recentGames(MOST_RECENT);
	const bool anyGames = libraryCount() > 0;
	const std::vector<HomeGame> row = !played.empty() ? played : newestGames(MOST_RECENT);
	const HomeGame* hero = played.empty() ? nullptr : &played.front();
	homeInput(row, hero, anyGames);
	if (screen != Screen::Library || libraryTab() != TabHome)
	{
		// (It handed over: a game starts, the settings or another tab opened.)
		topBar(dl, libraryTab(), false, Reach::All);
		hintBar(dl, {});
		return;
	}
	ambientArt(hero != nullptr ? hero->info : nullptr);
	const bool onCard = (hs.row == 0 && hs.button == ((hero != nullptr) ? 2 : 1));
	const float e = entered(400);
	const Layer content = beginLayer(dl);
	{
		const float top = 124, heroH = 420, gap = 30;
		const float colW = (W - 2 * X0 - 2 * gap) / 3;
		drawHero(dl, hero, X0, top, colW * 2 + gap, heroH, anyGames);
		drawRightNow(dl, X0 + colW * 2 + gap * 2, top, colW, heroH, onCard);
		drawRow(dl, row, !played.empty(), top + heroH + 40);
	}
	endEntrance(content, e);
	topBar(dl, libraryTab(), false, Reach::All);
	// What Cross does where the focus is.
	const char* open = "Game page";
	StateInfo state;
	if (onCard)
		open = "Texture packs";
	else if (hs.row == 0 && hero == nullptr)
		open = "Open the library";
	else if (hs.row == 0 && hs.button == 0)
		open = savedState(*hero->info, state) ? "Resume" : "Play";
	else if (hs.row == 1 && !row.empty())
	{
		const HomeGame& h = row[static_cast<size_t>(std::clamp(hs.cover, 0, static_cast<int>(row.size()) - 1))];
		if (h.lastPlayed > 0)
			open = savedState(*h.info, state) ? "Resume" : "Play";
	}
	std::vector<Hint> hints{{Glyph::Circle, "Quit"}, {Glyph::L1R1, "Sections"}, {Glyph::Options, "Settings"}};
	// (Square and Triangle act on the game in focus: the hero's, or a cover of the row.)
	if ((hs.row == 0 && hero != nullptr) || (hs.row == 1 && !row.empty()))
	{
		hints.push_back({Glyph::Square, "Game settings"});
		hints.push_back({Glyph::Triangle, "Game page"});
	}
	hints.push_back({Glyph::Cross, open});
	hintBar(dl, hints);
}

} // namespace bp
