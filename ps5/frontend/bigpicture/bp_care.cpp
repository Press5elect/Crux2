// Crux2 - the interface: care for the screen.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// An OLED television keeps a trace of what stood still on it for hours: a bar, a row of words, the
// edge of a picture. The screens of this interface stand still whenever nobody touches the
// controller, and a paused game under the quick menu does so for as long as it is left. So:
//
//   * the screens go dark when nothing was pressed for a while (Options::idleDim, minutes), and
//     after as long again they are black, with the title's mark wandering over them, until a
//     button is pressed (that press does nothing else);
//   * everything on a screen moves by a few pixels every other minute, to one of nine places
//     around where it belongs, so that no edge stays on the same row of pixels;
//   * the hint bar's words go faint when nothing was pressed for a quarter of a minute.
//
// Under a game the picture is the emulator's, and what can be done is less: the picture can be
// made darker when nothing was pressed for long (a film that plays by itself counts as nothing
// pressed: so it is off unless asked for), the bars beside a picture narrower than the screen can
// be dark grey in place of black (lit a little, they age with the rest of the screen), and the
// emulator's own figures in a corner can go round the corners.
//
// None of it has been measured on a television: how fast a panel ages is not something this code
// can know. What it does is what the makers of such screens do themselves.

#include "bp_internal.h"

namespace bp
{

namespace
{
double s_touched = 0;        // the screens' clock when a button was last held
uint32_t s_heldBefore = 0;
bool s_asleep = false;       // the screen saver is up
bool s_woke = false;         // and a button ended it this frame
double s_wokeAt = -10;       // when one last did
float s_dark = 0;            // how far the screens are darkened, 0 to 1
float s_saver = 0;           // how far the screen saver is in
float s_gameDark = 0;        // the same under a game
int s_cornerAt = -1;         // the corner the emulator's figures were last sent to
bool s_cornerMoved = false;

constexpr float HINTS_FADE_AFTER = 15.f; // seconds
constexpr float SHIFT_EVERY = 120.f;     // seconds
constexpr float SHIFT_BY = 3.f;          // units: six pixels of a 4K picture
constexpr float CORNER_EVERY = 300.f;    // seconds

// Where everything is moved to now, in pixels.
ImVec2 shiftNow()
{
	if (!options().pixelShift)
		return ImVec2(0, 0);
	static constexpr int places[9][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
	const int at = static_cast<int>(static_cast<long long>(timeNow / SHIFT_EVERY) % 9);
	return ImVec2(std::round(places[at][0] * SHIFT_BY * S), std::round(places[at][1] * SHIFT_BY * S));
}
} // namespace

void careTick(uint32_t held)
{
	s_woke = false;
	if (held != 0 || held != s_heldBefore)
	{
		if (s_asleep && held != 0)
		{
			s_woke = true;
			s_wokeAt = timeNow;
		}
		s_asleep = false;
		s_touched = timeNow;
	}
	s_heldBefore = held;
}

float idleSeconds()
{
	return static_cast<float>(timeNow - s_touched);
}

void careSetIdle(float seconds)
{
	s_touched = timeNow - seconds;
}

bool careAsleep()
{
	return s_asleep;
}

bool careWoke()
{
	return s_woke;
}

bool careWokeLately()
{
	return timeNow - s_wokeAt < 1.5;
}

float steadyAlpha()
{
	if (!options().hintFade)
		return 1.f;
	const float idle = idleSeconds();
	if (idle <= HINTS_FADE_AFTER)
		return 1.f;
	// Over two seconds, down to a seventh.
	return 1.f - 0.86f * std::clamp((idle - HINTS_FADE_AFTER) / 2.f, 0.f, 1.f);
}

void careOverScreen(ImDrawList* dl)
{
	const Options& o = options();
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float idle = idleSeconds();
	const float after = o.idleDim * 60.f;
	// (Not over the start-up animation: it is short, and nobody presses anything during it.)
	const bool may = o.idleDim > 0 && splash.state != SplashState::Showing && splash.state != SplashState::Leaving;
	const bool dark = may && idle >= after;
	const bool saver = may && o.screenSaver && idle >= 2 * after;
	// Slowly there, at once back.
	s_dark = dark ? approach(s_dark, 1, 1.4f) : approach(s_dark, 0, 40);
	s_saver = saver ? approach(s_saver, 1, 1.0f) : approach(s_saver, 0, 40);
	if (s_dark > 0.004f)
		dl->AddRectFilled(ImVec2(0, 0), size, col::rgba(0, 0, 0, static_cast<int>(165 * s_dark)));
	if (s_saver > 0.004f)
	{
		dl->AddRectFilled(ImVec2(0, 0), size, col::rgba(0, 0, 0, static_cast<int>(255 * s_saver)));
		// The mark, faint, on a path that does not come back to where it was for hours.
		const float t = static_cast<float>(timeNow);
		const float mark = 150;
		const float x = W / 2 + (W / 2 - mark) * std::sin(t * 0.0310f + 1.3f);
		const float y = H / 2 + (H / 2 - mark) * std::sin(t * 0.0473f + 0.4f);
		cruxMark(dl, x, y, mark, 0.34f * s_saver);
	}
	s_asleep = saver && s_saver > 0.5f;

	const ImVec2 by = shiftNow();
	if (by.x != 0 || by.y != 0)
	{
		// What is at the edge the screen moves away from stays there: a ground that fills the
		// screen is stretched by those few pixels, and no strip of what is under it shows.
		const auto moved = [](float at, float by1, float most) {
			if ((by1 > 0 && at <= 0) || (by1 < 0 && at >= most))
				return at;
			return at + by1;
		};
		for (ImDrawVert& v : dl->VtxBuffer)
		{
			v.pos.x = moved(v.pos.x, by.x, size.x);
			v.pos.y = moved(v.pos.y, by.y, size.y);
		}
		for (ImDrawCmd& c : dl->CmdBuffer)
		{
			c.ClipRect.x = moved(c.ClipRect.x, by.x, size.x);
			c.ClipRect.z = moved(c.ClipRect.z, by.x, size.x);
			c.ClipRect.y = moved(c.ClipRect.y, by.y, size.y);
			c.ClipRect.w = moved(c.ClipRect.w, by.y, size.y);
		}
	}
	// (A screen is up: the game's own darkening starts from nothing when it is back.)
	s_gameDark = 0;
}

void careOverGame()
{
	if (be == nullptr)
		return;
	const Options& o = options();
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const Running running = be->running();
	if (!running.valid)
	{
		s_gameDark = 0;
		return;
	}

	// The bars beside (or above and below) a picture that does not fill the screen.
	if (o.greyBars)
	{
		float r[4];
		if (be->gameRect(r) && r[2] - r[0] > 16 && r[3] - r[1] > 16)
		{
			ImDrawList* const under = ImGui::GetBackgroundDrawList();
			const ImU32 grey = col::rgba(26, 26, 26);
			const float x0 = std::max(0.f, r[0]), y0 = std::max(0.f, r[1]), x1 = std::min(size.x, r[2]), y1 = std::min(size.y, r[3]);
			if (x0 >= 1)
				under->AddRectFilled(ImVec2(0, 0), ImVec2(x0, size.y), grey);
			if (x1 <= size.x - 1)
				under->AddRectFilled(ImVec2(x1, 0), ImVec2(size.x, size.y), grey);
			if (y0 >= 1)
				under->AddRectFilled(ImVec2(x0, 0), ImVec2(x1, y0), grey);
			if (y1 <= size.y - 1)
				under->AddRectFilled(ImVec2(x0, y1), ImVec2(x1, size.y), grey);
		}
	}

	// Darker when nothing was pressed for long, and as it was with the first button.
	const bool dark = o.gameDim > 0 && !running.paused && idleSeconds() >= o.gameDim * 60.f;
	s_gameDark = dark ? approach(s_gameDark, 1, 0.8f) : approach(s_gameDark, 0, 40);
	if (s_gameDark > 0.004f)
		ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), size, col::rgba(0, 0, 0, static_cast<int>(150 * s_gameDark)));

	// The emulator's figures: another corner every five minutes; where the settings have them
	// when that is switched off again.
	if (o.osdMove)
	{
		const int corner = static_cast<int>(static_cast<long long>(timeNow / CORNER_EVERY) % 4);
		if (corner != s_cornerAt)
		{
			s_cornerAt = corner;
			s_cornerMoved = true;
			be->overlayCorner(corner);
		}
	}
	else if (s_cornerMoved)
	{
		s_cornerMoved = false;
		s_cornerAt = -1;
		be->overlayCorner(-1);
	}
}

} // namespace bp
