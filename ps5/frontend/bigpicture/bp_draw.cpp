// Crux2 - the interface: what every screen draws with, the skins, the controller, the
// top and bottom bars and the dialogs.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From PSFlyCast's shell/ps5/bigpicture.cpp (GPL-2.0-or-later), used here under GPL-3.0.

#include "bp_internal.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <unordered_map>

// The title's build stamp (ps5_build_stamp.cpp); weak, as the PC preview builds this file without it.
extern "C" const char* ps5px_build_version(void) __attribute__((weak));
extern "C" const char* ps5px_build_number(void) __attribute__((weak));

namespace bp
{

Backend* be = nullptr;

// ---------------------------------------------------------------- the theme

// The colours every screen draws with. They are the skin's (applySkin, below): Midnight's until
// a skin is applied.
namespace col
{
ImU32 bgTop = rgba(14, 19, 26);
ImU32 bgBottom = rgba(23, 30, 40);
ImU32 bar = rgba(10, 13, 18, 235);
ImU32 panel = rgba(30, 38, 49);
ImU32 panelHi = rgba(41, 52, 66);
ImU32 card = rgba(36, 45, 58);
ImU32 sheet = rgba(18, 23, 31); // the quick menu's panel
ImU32 sheetHi = rgba(24, 30, 40);
ImU32 dim = rgba(160, 172, 186);
ImU32 faint = rgba(105, 117, 131);
ImU32 accent = rgba(26, 159, 255);
ImU32 text = rgba(255, 255, 255);
ImU32 onAccent = rgba(255, 255, 255);
ImU32 accentDark = rgba(18, 104, 176);
ImU32 accentGlow = rgba(26, 159, 255, 70);
ImU32 warm = rgba(255, 128, 48); // the warm accent (the mark, what is not a PlayStation 2 disc)
ImU32 good = rgba(92, 200, 120);
ImU32 bad = rgba(229, 72, 77);
ImU32 line = rgba(255, 255, 255, 22);
float round = 1.f;
float textScale = 1.f;
bool lightSkin = false;
} // namespace col

float S = 1.f;
float W = 1920.f, H = 1080.f;
double timeNow = 0;
int frameNow = 0;

namespace
{
ImFont* s_regular = nullptr;
ImFont* s_bold = nullptr;
ImFont* s_brand = nullptr;
} // namespace

void setBrandFont(ImFont* brand_font)
{
	s_brand = brand_font;
}

ImFont* brand()
{
	return s_brand != nullptr ? s_brand : bold();
}

void setFonts(ImFont* regular_font, ImFont* bold_font)
{
	s_regular = regular_font;
	s_bold = bold_font;
}

ImFont* regular()
{
	return s_regular != nullptr ? s_regular : ImGui::GetIO().Fonts->Fonts[0];
}

ImFont* bold()
{
	return s_bold != nullptr ? s_bold : regular();
}

ImU32 alpha(ImU32 c, float a)
{
	const int a0 = (c >> IM_COL32_A_SHIFT) & 0xff;
	return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a0 * a, 0.f, 255.f)) << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t)
{
	const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
	return ImGui::ColorConvertFloat4ToU32(
		ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

float approach(float v, float target, float speed)
{
	if (motion() == MotionOff)
		return target;
	const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
	return v + (target - v) * (1.f - std::exp(-speed * dt));
}

float easeOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}

float easeInOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

// The text of rows, hints and lists (30 units and smaller) is drawn larger with Settings >
// Interface > Text size; titles and the large figures are not.
static float readingSize(float size)
{
	return size <= 30.f ? size * col::textScale : size;
}

ImVec2 textSize(ImFont* font, float size, const char* s, float wrap)
{
	const ImVec2 px = font->CalcTextSizeA(readingSize(size) * S, FLT_MAX, wrap * S, s);
	return ImVec2(px.x / S, px.y / S);
}

ImVec2 text(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 c, const char* s, float wrap)
{
	// (Larger text keeps the line it was placed on: it grows from its middle.)
	const float grown = (readingSize(size) - size) * 0.5f;
	dl->AddText(font, readingSize(size) * S, V(x, y - grown), c, s, nullptr, wrap * S);
	return textSize(font, size, s, wrap);
}

std::string fit(ImFont* font, float size, const std::string& s, float width)
{
	if (textSize(font, size, s.c_str()).x <= width)
		return s;
	std::string out = s;
	while (!out.empty() && textSize(font, size, (out + "...").c_str()).x > width)
	{
		// (Not half of a character of several bytes.)
		do
			out.pop_back();
		while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80);
		if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0xc0)
			out.pop_back();
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out + "...";
}

namespace
{
// Texts that slide (textIn): when each began to, by what it says and where it is. One that was not
// drawn for a moment begins anew the next time (the focus came back to it).
struct Slide
{
	double began = 0, seen = 0;
};
std::unordered_map<size_t, Slide> s_slides;
} // namespace

ImVec2 textIn(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 c, const std::string& s, float width, bool moving)
{
	const ImVec2 whole = textSize(font, size, s.c_str());
	if (whole.x <= width || width <= 0)
		return text(dl, font, size, x, y, c, s.c_str());
	if (!moving)
	{
		const std::string cut = fit(font, size, s, width);
		return text(dl, font, size, x, y, c, cut.c_str());
	}
	// Waits, slides at a reading pace, waits at the end, and is back at its beginning.
	constexpr double WAIT = 1.4, END = 1.6, PACE = 110.0; // seconds, seconds, units a second
	// (Whole numbers first: a place off the screen's left or top is a negative one.)
	const size_t key = std::hash<std::string>()(s) ^ (static_cast<size_t>(static_cast<long long>(x)) * 0x9e3779b97f4a7c15ull) ^
					   (static_cast<size_t>(static_cast<long long>(y)) << 20);
	if (s_slides.size() > 64)
		std::erase_if(s_slides, [](const auto& entry) { return timeNow - entry.second.seen > 1.0; });
	Slide& slide = s_slides[key];
	if (timeNow - slide.seen > 0.4)
		slide.began = timeNow;
	slide.seen = timeNow;
	const double over = whole.x - width + 12;
	const double cycle = WAIT + over / PACE + END;
	const double at = std::fmod(timeNow - slide.began, cycle);
	const float offset = static_cast<float>(std::clamp((at - WAIT) * PACE, 0.0, over));
	dl->PushClipRect(V(x, y - 6), V(x + width, y + whole.y + 6), true);
	text(dl, font, size, x - offset, y, c, s.c_str());
	dl->PopClipRect();
	return ImVec2(width, whole.y);
}

void rect(ImDrawList* dl, float x, float y, float w, float h, ImU32 c, float r, ImDrawFlags f)
{
	dl->AddRectFilled(V(x, y), V(x + w, y + h), c, r * col::round * S, f);
}

void outline(ImDrawList* dl, float x, float y, float w, float h, ImU32 c, float r, float t)
{
	dl->AddRect(V(x, y), V(x + w, y + h), c, r * col::round * S, t * S);
}

void gradientV(ImDrawList* dl, float x, float y, float w, float h, ImU32 top, ImU32 bottom)
{
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), top, top, bottom, bottom);
}

void gradientH(ImDrawList* dl, float x, float y, float w, float h, ImU32 left, ImU32 right)
{
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), left, right, right, left);
}

void glow(ImDrawList* dl, float x, float y, float w, float h, float r, ImU32 c, float spread)
{
	for (int i = 6; i >= 1; i--)
	{
		const float g = spread * i / 6.f;
		dl->AddRectFilled(V(x - g, y - g), V(x + w + g, y + h + g), alpha(c, 0.22f / i), (r * col::round + g) * S);
	}
}

void softDisc(ImDrawList* dl, float cx, float cy, float radius, ImU32 colour, int segments)
{
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	const ImU32 edge = colour & ~IM_COL32_A_MASK;
	dl->PrimReserve(segments * 3, segments + 1);
	const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
	dl->PrimWriteVtx(V(cx, cy), uv, colour);
	for (int i = 0; i < segments; i++)
	{
		const float angle = 2 * IM_PI * i / segments;
		dl->PrimWriteVtx(V(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius), uv, edge);
	}
	for (int i = 0; i < segments; i++)
	{
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
		dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % segments));
	}
}

void quad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd)
{
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	dl->PrimReserve(6, 4);
	const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
	dl->PrimWriteVtx(a, uv, ca);
	dl->PrimWriteVtx(b, uv, cb);
	dl->PrimWriteVtx(c, uv, cc);
	dl->PrimWriteVtx(d, uv, cd);
	dl->PrimWriteIdx(base);
	dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
	dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
	dl->PrimWriteIdx(base);
	dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
	dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
}

float chance(unsigned n)
{
	n = (n ^ 61u) ^ (n >> 16);
	n *= 9u;
	n ^= n >> 4;
	n *= 0x27d4eb2du;
	n ^= n >> 15;
	return (n & 0xffffff) / 16777216.f;
}

namespace
{
// The part of a picture that fills a box of another shape, its centre kept.
void cropFor(const Texture& t, float w, float h, ImVec2& uv0, ImVec2& uv1)
{
	const float ar = (t.height > 0) ? t.width / t.height : 1.f;
	const float boxAr = w / h;
	uv0 = ImVec2(0, 0);
	uv1 = ImVec2(1, 1);
	if (ar > boxAr)
	{
		const float k = boxAr / ar;
		uv0.x = (1 - k) / 2;
		uv1.x = 1 - uv0.x;
	}
	else
	{
		const float k = ar / boxAr;
		uv0.y = (1 - k) / 2;
		uv1.y = 1 - uv0.y;
	}
}
} // namespace

void imageFill(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float r, ImU32 tint)
{
	if (!t.valid() || w <= 0 || h <= 0)
		return;
	ImVec2 uv0, uv1;
	cropFor(t, w, h, uv0, uv1);
	dl->AddImageRounded(t.id, V(x, y), V(x + w, y + h), uv0, uv1, tint, r * S);
}

void imageFit(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float r, ImU32 tint)
{
	if (!t.valid() || w <= 0 || h <= 0)
		return;
	const float ar = t.width / t.height;
	float dw = w, dh = h;
	if (ar > w / h)
		dh = w / ar;
	else
		dw = h * ar;
	const float dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
	dl->AddImageRounded(t.id, V(dx, dy), V(dx + dw, dy + dh), ImVec2(0, 0), ImVec2(1, 1), tint, r * S);
}

void imageFade(ImDrawList* dl, const Texture& t, float x, float y, float w, float h, float k, const float (&xs)[3],
	const float (&ax)[3], const float (&ys)[3], const float (&ay)[3])
{
	if (!t.valid() || w <= 0 || h <= 0)
		return;
	ImVec2 uv0, uv1;
	cropFor(t, w, h, uv0, uv1);
	dl->PushTexture(t.id);
	dl->PrimReserve(4 * 6, 9);
	const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
	for (int j = 0; j < 3; j++)
	{
		for (int i = 0; i < 3; i++)
		{
			dl->PrimWriteVtx(V(x + w * xs[i], y + h * ys[j]),
				ImVec2(uv0.x + (uv1.x - uv0.x) * xs[i], uv0.y + (uv1.y - uv0.y) * ys[j]), alpha(col::white, k * ax[i] * ay[j]));
		}
	}
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			const ImDrawIdx a = static_cast<ImDrawIdx>(base + j * 3 + i);
			dl->PrimWriteIdx(a);
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 4));
			dl->PrimWriteIdx(a);
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 4));
			dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
		}
	}
	dl->PopTexture();
}

// ------------------------------------------------------- skins and motion
//
// A skin is a palette and what moves behind the screens. Crux is the title's own and the default;
// Midnight is the look the interface had before it. Settings > Interface chooses the skin, an accent colour
// of one's own, the backdrop and how much the interface moves.

const char* const backdropNames[BackdropCount] = {"Still", "Aurora", "Waves", "Sparks", "Horizon", "Dot matrix", "Cover colours",
	"The game's picture", "My pictures"};

const Skin skins[] = {
	// The title's own: the warm black, the orange and the cream of its mark. What is written on the
	// orange is dark, as the mark's ground is.
	{"Crux", col::rgba(23, 18, 15), col::rgba(28, 21, 17), col::rgba(31, 25, 21, 240), col::rgba(43, 34, 28),
		col::rgba(58, 42, 32), col::rgba(51, 39, 31), col::rgba(28, 22, 18), col::rgba(38, 30, 25), col::rgba(184, 176, 168),
		col::rgba(128, 118, 110), col::rgba(255, 106, 61), col::rgba(242, 179, 61), col::rgba(243, 236, 228), col::rgba(23, 18, 15), BackStill},
	// The first look: slate and blue.
	{"Midnight", col::rgba(14, 19, 26), col::rgba(23, 30, 40), col::rgba(10, 13, 18, 235), col::rgba(30, 38, 49),
		col::rgba(41, 52, 66), col::rgba(36, 45, 58), col::rgba(18, 23, 31), col::rgba(24, 30, 40), col::rgba(160, 172, 186),
		col::rgba(105, 117, 131), col::rgba(26, 159, 255), col::rgba(255, 128, 48), col::rgba(255, 255, 255), col::rgba(255, 255, 255), BackStill},
	// Warm charcoal and the orange of the console's power light.
	{"Ember", col::rgba(22, 15, 12), col::rgba(40, 25, 18), col::rgba(16, 11, 9, 235), col::rgba(49, 35, 28),
		col::rgba(68, 48, 38), col::rgba(58, 41, 33), col::rgba(28, 19, 16), col::rgba(38, 27, 22), col::rgba(200, 180, 168),
		col::rgba(140, 120, 108), col::rgba(236, 100, 20), col::rgba(255, 196, 84), col::rgba(255, 255, 255), col::rgba(255, 255, 255), BackAurora},
	// Olive glass, and the dots of a small screen behind.
	{"Moss", col::rgba(13, 18, 13), col::rgba(24, 33, 23), col::rgba(9, 13, 9, 235), col::rgba(32, 43, 31),
		col::rgba(46, 60, 44), col::rgba(39, 51, 37), col::rgba(18, 25, 18), col::rgba(26, 35, 25), col::rgba(172, 188, 162),
		col::rgba(112, 129, 104), col::rgba(98, 148, 54), col::rgba(222, 204, 96), col::rgba(255, 255, 255), col::rgba(255, 255, 255), BackMatrix},
	// Violet, magenta and a floor that runs to the horizon.
	{"Neon", col::rgba(15, 9, 27), col::rgba(35, 16, 50), col::rgba(11, 7, 21, 235), col::rgba(45, 26, 63),
		col::rgba(62, 38, 86), col::rgba(53, 32, 74), col::rgba(23, 13, 35), col::rgba(33, 20, 48), col::rgba(194, 174, 214),
		col::rgba(130, 110, 152), col::rgba(224, 46, 138), col::rgba(255, 180, 40), col::rgba(255, 255, 255), col::rgba(255, 255, 255), BackHorizon},
	// For an OLED television: black, and little else lit.
	{"Carbon", col::rgba(0, 0, 0), col::rgba(7, 7, 8), col::rgba(0, 0, 0, 235), col::rgba(22, 22, 24), col::rgba(38, 38, 42),
		col::rgba(28, 28, 31), col::rgba(11, 11, 12), col::rgba(19, 19, 21), col::rgba(172, 172, 178), col::rgba(110, 110, 116),
		col::rgba(212, 42, 58), col::rgba(255, 172, 64), col::rgba(255, 255, 255), col::rgba(255, 255, 255), BackStill},
	// The thirty themes of the ps5-homebrew-ui kit, the look of the other titles of the same family
	// (generated by ps5/tools/make-kit-skins.py; the themes are BlackBearReloaded's, GPL-3.0+).
#include "bp_kit_skins.inc"
};
const int SkinCount = static_cast<int>(sizeof(skins) / sizeof(skins[0]));

// The accent colours one can choose in place of a skin's own.
const Accent accents[] = {
	{"The skin's own", 0},
	{"Blue", col::rgba(26, 159, 255)},
	{"Orange", col::rgba(236, 100, 20)},
	{"Green", col::rgba(52, 168, 96)},
	{"Teal", col::rgba(0, 158, 164)},
	{"Violet", col::rgba(126, 92, 240)},
	{"Magenta", col::rgba(224, 46, 138)},
	{"Red", col::rgba(212, 42, 58)},
};
const int AccentCount = static_cast<int>(sizeof(accents) / sizeof(accents[0]));

namespace
{
Options s_options;
constexpr const char* UI = "PS5UI";

const Skin& skin()
{
	return skins[std::clamp(s_options.skin, 0, SkinCount - 1)];
}
} // namespace

Options& options()
{
	return s_options;
}

const char* const settingsLevelNames[3] = {"Normal", "Advanced", "Expert"};

void setSettingsLevel(int level)
{
	level = std::clamp(level, 0, 2);
	be->setInt(Shared, UI, "SettingsLevel", level);
	be->setBool(Shared, "UI", "ShowAdvancedSettings", level >= 1);
	loadOptions();
}

void loadOptions()
{
	Options o;
	if (be != nullptr)
	{
		o.view = std::clamp(be->getInt(Shared, UI, "View").value_or(o.view), 0, 9);
		o.source = std::clamp(be->getInt(Shared, UI, "Source").value_or(o.source), 0, static_cast<int>(Network));
		o.groupDiscs = be->getBool(Shared, UI, "GroupDiscs").value_or(o.groupDiscs);
		o.splash = be->getBool(Shared, UI, "Splash").value_or(o.splash);
		// ("Look", not the "Skin" of the builds before Crux: their numbers named other skins.)
		o.skin = std::clamp(be->getInt(Shared, UI, "Look").value_or(o.skin), 0, SkinCount - 1);
		o.splashKind = std::clamp(be->getInt(Shared, UI, "SplashKind").value_or(o.splashKind), 0, 4);
		o.splashLast = std::clamp(be->getInt(Shared, UI, "SplashLast").value_or(o.splashLast), 0, 4);
		o.accent = std::clamp(be->getInt(Shared, UI, "Accent").value_or(o.accent), 0, AccentCount - 1);
		o.backdrop = std::clamp(be->getInt(Shared, UI, "Backdrop").value_or(o.backdrop), 0, static_cast<int>(BackdropCount));
		o.motion = std::clamp(be->getInt(Shared, UI, "Motion").value_or(o.motion), 0, 2);
		// The level, or before there were three of them, PCSX2's switch for advanced settings.
		{
			const bool shownAdvanced = be->getBool(Shared, "UI", "ShowAdvancedSettings").value_or(false);
			o.level = std::clamp(be->getInt(Shared, UI, "SettingsLevel").value_or(shownAdvanced ? 1 : 0), 0, 2);
			o.advanced = o.level >= 1;
			o.expert = o.level >= 2;
			if (shownAdvanced != o.advanced)
				be->setBool(Shared, "UI", "ShowAdvancedSettings", o.advanced);
		}
		o.stateSlot = std::clamp(be->getInt(Shared, UI, "StateSlot").value_or(o.stateSlot), 1, 10);
		o.sort = std::clamp(be->getInt(Shared, UI, "Sort").value_or(o.sort), 0, 4);
		o.heroPlay = be->getBool(Shared, UI, "HeroPlay").value_or(o.heroPlay);
		o.wallEvery = std::clamp(be->getInt(Shared, UI, "WallEvery").value_or(o.wallEvery), 10, 3600);
		o.wallDim = std::clamp(be->getInt(Shared, UI, "WallDim").value_or(o.wallDim), 0, 90);
		o.idleDim = std::clamp(be->getInt(Shared, UI, "IdleDim").value_or(o.idleDim), 0, 60);
		o.screenSaver = be->getBool(Shared, UI, "ScreenSaver").value_or(o.screenSaver);
		o.pixelShift = be->getBool(Shared, UI, "PixelShift").value_or(o.pixelShift);
		o.hintFade = be->getBool(Shared, UI, "HintFade").value_or(o.hintFade);
		o.gameDim = std::clamp(be->getInt(Shared, UI, "GameDim").value_or(o.gameDim), 0, 120);
		o.greyBars = be->getBool(Shared, UI, "GreyBars").value_or(o.greyBars);
		o.osdMove = be->getBool(Shared, UI, "OsdMove").value_or(o.osdMove);
		o.swapConfirm = be->getBool(Shared, UI, "SwapConfirm").value_or(o.swapConfirm);
		o.textSize = std::clamp(be->getInt(Shared, UI, "TextSize").value_or(o.textSize), -2, 2);
		o.uiSize = std::clamp(be->getInt(Shared, UI, "InterfaceSize").value_or(o.uiSize), 60, 100);
		o.highContrast = be->getBool(Shared, UI, "HighContrast").value_or(o.highContrast);
		o.colourBlind = be->getBool(Shared, UI, "ColourBlind").value_or(o.colourBlind);
	}
	s_options = o;
}

void saveOptions()
{
	if (be == nullptr)
		return;
	const Options& o = s_options;
	be->setInt(Shared, UI, "View", o.view);
	be->setBool(Shared, UI, "SwapConfirm", o.swapConfirm);
	be->setInt(Shared, UI, "Source", o.source);
	be->setBool(Shared, UI, "GroupDiscs", o.groupDiscs);
	be->setBool(Shared, UI, "Splash", o.splash);
	be->setInt(Shared, UI, "Look", o.skin);
	be->setInt(Shared, UI, "SplashKind", o.splashKind);
	be->setInt(Shared, UI, "SplashLast", o.splashLast);
	be->setInt(Shared, UI, "Accent", o.accent);
	be->setInt(Shared, UI, "Backdrop", o.backdrop);
	be->setInt(Shared, UI, "Motion", o.motion);
	be->setInt(Shared, UI, "StateSlot", o.stateSlot);
	be->setInt(Shared, UI, "Sort", o.sort);
}

// The palette the screens draw with, from the options; cheap enough for every frame.
void applySkin()
{
	const Skin& s = skin();
	col::bgTop = s.bgTop;
	col::bgBottom = s.bgBottom;
	col::bar = s.bar;
	col::panel = s.panel;
	col::panelHi = s.panelHi;
	col::card = s.card;
	col::sheet = s.sheet;
	col::sheetHi = s.sheetHi;
	col::dim = s.dim;
	col::faint = s.faint;
	col::warm = s.warm;
	col::text = s.text;
	col::line = s.line;
	col::round = s.round;
	col::lightSkin = s.light;
	col::good = s.good;
	col::bad = s.bad;
	static constexpr float textScales[5] = {0.84f, 0.92f, 1.f, 1.12f, 1.24f};
	col::textScale = textScales[std::clamp(s_options.textSize, -2, 2) + 2];
	// Stronger contrast: what is written is the skin's text colour or the next thing to it, and
	// the hairlines are drawn twice as strong.
	if (s_options.highContrast)
	{
		col::dim = mix(s.dim, s.text, 0.6f);
		col::faint = mix(s.faint, s.text, 0.45f);
		col::line = alpha(s.text, 0.32f);
	}
	// Colours told apart without red and green: blue for what went well, orange for what did not
	// (the Okabe-Ito colours).
	if (s_options.colourBlind)
	{
		col::good = col::rgba(0, 114, 178);
		col::bad = col::rgba(230, 159, 0);
	}
	const int chosen = std::clamp(s_options.accent, 0, AccentCount - 1);
	col::accent = (chosen == 0) ? s.accent : accents[chosen].colour;
	col::accentDark = mix(col::accent, col::rgba(0, 0, 0), 0.35f);
	col::accentGlow = alpha(col::accent, 70 / 255.f);
	// What is written on the accent: the skin's own choice on its own accent; on an accent of
	// one's own, dark on a light one and white on a dark one.
	if (chosen == 0)
	{
		col::onAccent = s.onAccent;
	}
	else
	{
		const float light = (0.299f * (col::accent & 0xff) + 0.587f * ((col::accent >> 8) & 0xff) + 0.114f * ((col::accent >> 16) & 0xff)) / 255.f;
		col::onAccent = light > 0.6f ? col::rgba(20, 18, 16) : col::rgba(255, 255, 255);
	}
}

Backdrop backdropChoice()
{
	const int chosen = s_options.backdrop;
	return (chosen <= 0 || chosen > BackdropCount) ? skin().backdrop : static_cast<Backdrop>(chosen - 1);
}

// How much moves: everything, only what answers the controller (no drifting backdrop, no
// entrances one by one, shorter fades), or nothing.
Motion motion()
{
	return static_cast<Motion>(std::clamp(s_options.motion, 0, 2));
}

namespace
{
// The backdrop's clock: it stands still unless everything moves.
double driftTime()
{
	return motion() == MotionFull ? timeNow : 40.0;
}

// The games whose covers are behind the screens: the pictures themselves are asked of the
// Backend each frame (they are its own, and may be gone the next).
struct Ambient
{
	GameInfo now, before;
	float fade = 1;
} s_ambient;
} // namespace

void ambientArt(const GameInfo* game)
{
	const std::string path = (game != nullptr) ? game->path : std::string();
	if (path == s_ambient.now.path)
		return;
	s_ambient.before = s_ambient.now;
	s_ambient.now = (game != nullptr) ? *game : GameInfo();
	s_ambient.fade = 0;
}

void drawBackdrop(ImDrawList* dl)
{
	gradientV(dl, 0, 0, W, H, col::bgTop, col::bgBottom);
	const float t = static_cast<float>(driftTime());
	const ImU32 second = mix(col::accent, col::warm, 0.5f);
	switch (backdropChoice())
	{
		case BackStill:
		default:
			// A wash of light from the top left, as Big Picture's backdrop has.
			dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 10 / 255.f), 96);
			dl->AddCircleFilled(V(W * 0.12f, -260), 600 * S, alpha(col::accent, 10 / 255.f), 96);
			break;

		case BackAurora:
		{
			// Four lights that wander slowly and never line up twice.
			const ImU32 lights[4] = {col::accent, col::warm, second, col::accent};
			for (int i = 0; i < 4; i++)
			{
				const float px = W * (0.5f + 0.42f * std::sin(t * (0.050f + 0.013f * i) + i * 1.9f));
				const float py = H * (0.42f + 0.36f * std::sin(t * (0.037f + 0.011f * i) + i * 2.7f + 1.f));
				softDisc(dl, px, py, 760 + 140 * std::sin(t * 0.07f + i), alpha(lights[i], i == 1 ? 0.13f : 0.17f), 48);
			}
			break;
		}

		case BackWaves:
		{
			// Ribbons of light across the lower half, each on its own swell.
			const int steps = 48;
			for (int band = 0; band < 4; band++)
			{
				const ImU32 colour = (band % 2 == 0) ? col::accent : second;
				const float baseY = H * (0.50f + 0.11f * band);
				const auto height = [&](float x) {
					return baseY + 46 * std::sin(x * 0.0031f + t * (0.22f + 0.05f * band) + band * 1.7f) +
						   24 * std::sin(x * 0.0072f - t * (0.15f + 0.03f * band) + band);
				};
				for (int i = 0; i < steps; i++)
				{
					const float x0 = W * i / steps, x1 = W * (i + 1) / steps;
					const float y0 = height(x0), y1 = height(x1);
					const ImU32 top = alpha(colour, 0.13f - 0.02f * band), none = colour & ~IM_COL32_A_MASK;
					quad(dl, V(x0, y0), V(x1, y1), V(x1, y1 + 300), V(x0, y0 + 300), top, top, none, none);
					dl->AddLine(V(x0, y0), V(x1, y1), alpha(colour, 0.30f - 0.05f * band), 2 * S);
				}
			}
			break;
		}

		case BackSparks:
		{
			// Motes rising at their own pace, the large ones slower and fainter.
			for (unsigned i = 0; i < 70; i++)
			{
				const float size = 6 + 46 * chance(i * 7 + 1) * chance(i * 7 + 2);
				const float speed = 26 - size * 0.4f;
				float y = std::fmod((H + 120) * chance(i * 7 + 3) - t * speed, H + 120);
				if (y < 0)
					y += H + 120;
				y -= 60;
				const float x = W * chance(i * 7 + 4) + 30 * std::sin(t * 0.3f + i);
				const ImU32 colour = (i % 5 == 0) ? col::warm : (i % 3 == 0) ? col::text : col::accent;
				softDisc(dl, x, y, size, alpha(colour, 0.10f + 0.16f * chance(i * 7 + 5)), 16);
			}
			break;
		}

		case BackHorizon:
		{
			// A floor of lines running to a horizon, and the glow where it ends.
			const float horizon = H * 0.56f, vx = W * 0.5f;
			softDisc(dl, vx, horizon, 620, alpha(col::accent, 0.20f), 48);
			softDisc(dl, vx, horizon, 260, alpha(col::warm, 0.16f), 32);
			gradientV(dl, 0, horizon, W, H - horizon, alpha(col::bgTop, 0.85f), alpha(col::bgBottom, 0.55f));
			for (int i = -14; i <= 14; i++)
			{
				const float bottom = vx + i * 260.f;
				dl->AddLine(V(vx + i * 6.f, horizon), V(bottom, H), alpha(col::accent, 0.26f), 2 * S);
			}
			const float phase = std::fmod(t * 0.35f, 1.f);
			for (int i = 0; i < 14; i++)
			{
				const float depth = (i + phase) / 14.f; // 0 at the horizon
				const float y = horizon + (H - horizon) * depth * depth;
				dl->AddLine(V(0, y), V(W, y), alpha(col::accent, 0.06f + 0.26f * depth), 2 * S);
			}
			break;
		}

		case BackMatrix:
		{
			// The dots of a small screen, with a swell of light passing through them.
			const float pitch = 40;
			for (int row = 0; row * pitch < H; row++)
			{
				for (int column = 0; column * pitch < W; column++)
				{
					const float wave = std::sin(column * 0.21f + row * 0.13f - t * 0.6f) * std::sin(row * 0.17f + t * 0.23f);
					const float lit = 0.028f + 0.075f * std::max(0.f, wave);
					const float x = column * pitch + 8, y = row * pitch + 8;
					dl->AddRectFilled(V(x, y), V(x + pitch - 14, y + pitch - 14), alpha(col::accent, lit));
				}
			}
			break;
		}

		case BackCover:
		{
			s_ambient.fade = approach(s_ambient.fade, 1, 3);
			const Texture before = (!s_ambient.before.path.empty() && s_ambient.fade < 0.99f) ? be->cover(s_ambient.before) : Texture();
			const Texture now = !s_ambient.now.path.empty() ? be->cover(s_ambient.now) : Texture();
			if (before.valid())
				imageFill(dl, before, -W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f, 0, alpha(col::white, 0.30f * (1 - s_ambient.fade)));
			if (now.valid())
				imageFill(dl, now, -W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f, 0, alpha(col::white, 0.30f * s_ambient.fade));
			else
				dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 14 / 255.f), 96);
			// Darker toward the bottom, where the shelves and the lists are read.
			gradientV(dl, 0, 0, W, H, alpha(col::bgTop, 0.25f), alpha(col::bgBottom, 0.80f));
			break;
		}

		case BackPlay:
		{
			// A picture of the focused game being played, where it has one (Settings > Library
			// fetches them); its cover's colours where it has none.
			s_ambient.fade = approach(s_ambient.fade, 1, 3);
			const auto of = [](const GameInfo& game, bool* play) {
				Texture t = be->picture(game, 1);
				*play = t.valid();
				return t.valid() ? t : be->cover(game);
			};
			bool playBefore = false, playNow = false;
			const Texture before = (!s_ambient.before.path.empty() && s_ambient.fade < 0.99f) ? of(s_ambient.before, &playBefore) : Texture();
			const Texture now = !s_ambient.now.path.empty() ? of(s_ambient.now, &playNow) : Texture();
			const auto draw = [&](const Texture& t, bool play, float a) {
				if (play)
					imageFill(dl, t, 0, 0, W, H, 0, alpha(col::white, 0.42f * a));
				else
					imageFill(dl, t, -W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f, 0, alpha(col::white, 0.30f * a));
			};
			if (before.valid())
				draw(before, playBefore, 1 - s_ambient.fade);
			if (now.valid())
				draw(now, playNow, s_ambient.fade);
			else
				dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 14 / 255.f), 96);
			gradientV(dl, 0, 0, W, H, alpha(col::bgTop, 0.30f), alpha(col::bgBottom, 0.85f));
			break;
		}

		case BackOwn:
		{
			// Pictures of one's own (the folder "backgrounds" in the title folder), one after
			// another: each stays for a while and the next fades in over it.
			const int n = be->wallpapers(false);
			if (n <= 0)
			{
				dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 10 / 255.f), 96);
				dl->AddCircleFilled(V(W * 0.12f, -260), 600 * S, alpha(col::accent, 10 / 255.f), 96);
				break;
			}
			const double every = std::max(10, s_options.wallEvery);
			const double turns = timeNow / every;
			const int index = static_cast<int>(static_cast<long long>(turns) % n);
			const float into = static_cast<float>((turns - std::floor(turns)) * every);
			const float fade = (motion() == MotionOff) ? 0.f : 1.6f;
			// (What was shown last is kept until the next one is loaded: no hole between them.)
			static int shown = -1;
			static float shownFade = 1;
			const Texture now = be->wallpaper(index);
			if (now.valid() && shown != index)
			{
				// It may come late (a large picture off a slow drive): it fades in from when it is there.
				if (shown >= 0 && fade > 0)
					shownFade = 0;
				else
					shownFade = 1;
				shown = index;
			}
			const int beforeIndex = (index + n - 1) % n;
			if (shown == index)
			{
				shownFade = (fade > 0) ? std::min(1.f, shownFade + ImGui::GetIO().DeltaTime / fade) : 1.f;
				if (shownFade < 1 && n > 1)
				{
					const Texture before = be->wallpaper(beforeIndex);
					if (before.valid())
						imageFill(dl, before, 0, 0, W, H, 0);
				}
				imageFill(dl, now, 0, 0, W, H, 0, alpha(col::white, shownFade));
			}
			else if (shown >= 0)
			{
				const Texture still = be->wallpaper(shown);
				if (still.valid())
					imageFill(dl, still, 0, 0, W, H, 0);
			}
			// The one after is asked for a little before its turn, so that it is there for it.
			if (n > 1 && shownFade >= 1 && every - into < 6.0)
				be->wallpaper((index + 1) % n);
			// Darker, so that what is written on it can be read; more so toward the bottom.
			const float dim = std::clamp(s_options.wallDim, 0, 90) / 100.f;
			gradientV(dl, 0, 0, W, H, alpha(col::bgTop, dim * 0.85f), alpha(col::bgBottom, std::min(0.95f, dim + 0.25f)));
			break;
		}
	}
}

// ---- transitions

Layer beginLayer(ImDrawList* dl)
{
	return {dl, dl->VtxBuffer.Size};
}

void endLayer(const Layer& layer, float opacity, float dx, float dy, float scale, float pivotX, float pivotY)
{
	if (opacity >= 0.999f && dx == 0 && dy == 0 && scale == 1)
		return;
	opacity = std::clamp(opacity, 0.f, 1.f);
	ImDrawList* dl = layer.dl;
	for (int i = layer.from; i < dl->VtxBuffer.Size; i++)
	{
		ImDrawVert& v = dl->VtxBuffer[i];
		v.pos.x = (pivotX + dx) * S + (v.pos.x - pivotX * S) * scale;
		v.pos.y = (pivotY + dy) * S + (v.pos.y - pivotY * S) * scale;
		const ImU32 a = static_cast<ImU32>(((v.col >> IM_COL32_A_SHIFT) & 0xff) * opacity);
		v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}
}

namespace
{
struct Entrance
{
	int screen = -1;
	int frame = -10;
	double at = 0;
	float side = 0, next = 0;
} s_entrance;
} // namespace

float entered(int screen_id)
{
	const int frame = frameNow;
	if (screen_id != s_entrance.screen || frame > s_entrance.frame + 2)
	{
		s_entrance.screen = screen_id;
		s_entrance.at = timeNow;
		s_entrance.side = s_entrance.next;
		s_entrance.next = 0;
	}
	s_entrance.frame = frame;
	const float seconds = (motion() == MotionFull) ? 0.32f : (motion() == MotionReduced) ? 0.16f : 0.f;
	return seconds <= 0 ? 1.f : easeOut(static_cast<float>(timeNow - s_entrance.at) / seconds);
}

double enteredAt()
{
	return s_entrance.at;
}

void entranceSide(float side)
{
	s_entrance.next = side;
}

void endEntrance(const Layer& layer, float e)
{
	endLayer(layer, e, (1 - e) * 56 * s_entrance.side, s_entrance.side == 0 ? (1 - e) * 18 : 0);
}

void discMark(ImDrawList* dl, float cx, float cy, float radius, ImU32 ring, ImU32 arc, float thickness)
{
	dl->AddCircle(V(cx, cy), radius * S, ring, 64, thickness * S);
	dl->AddCircle(V(cx, cy), radius * 0.62f * S, alpha(ring, 0.45f), 48, thickness * 0.5f * S);
	dl->AddCircleFilled(V(cx, cy), radius * 0.22f * S, ring, 32);
	dl->PathClear();
	dl->PathArcTo(V(cx, cy), radius * S, -IM_PI * 0.95f, -IM_PI * 0.35f, 24);
	dl->PathStroke(arc, thickness * 1.6f * S);
}

// ------------------------------------------------------------ the mark and the name

namespace
{
constexpr float BIG_TYPE = 220; // the type the start-up sets the name in; the top bar's is BAR_TYPE

// A piece of a bar: from `from` to `to` along its direction (distances from `middle`, in pixels),
// `width` wide, round at the ends named and cut straight at the others.
void barPiece(ImDrawList* dl, ImVec2 middle, float angle, float from, float to, float width, ImU32 colour, bool roundFrom, bool roundTo)
{
	if (to - from <= 0.01f || (colour >> IM_COL32_A_SHIFT) == 0)
		return;
	const float r = width / 2;
	const ImVec2 d(std::cos(angle), std::sin(angle)), n(-d.y, d.x);
	float a0 = from + (roundFrom ? r : 0), a1 = to - (roundTo ? r : 0);
	if (a1 < a0)
	{
		// Shorter than it is wide: a dot of its length.
		const float m = (from + to) / 2;
		dl->AddCircleFilled(ImVec2(middle.x + d.x * m, middle.y + d.y * m), (to - from) / 2, colour, 24);
		return;
	}
	const ImVec2 a(middle.x + d.x * a0, middle.y + d.y * a0), b(middle.x + d.x * a1, middle.y + d.y * a1);
	dl->PathClear();
	if (roundTo)
	{
		dl->PathArcTo(b, r, angle - IM_PI / 2, angle + IM_PI / 2, 14);
	}
	else
	{
		dl->PathLineTo(ImVec2(b.x - n.x * r, b.y - n.y * r));
		dl->PathLineTo(ImVec2(b.x + n.x * r, b.y + n.y * r));
	}
	if (roundFrom)
	{
		dl->PathArcTo(a, r, angle + IM_PI / 2, angle + IM_PI * 1.5f, 14);
	}
	else
	{
		dl->PathLineTo(ImVec2(a.x + n.x * r, a.y + n.y * r));
		dl->PathLineTo(ImVec2(a.x - n.x * r, a.y - n.y * r));
	}
	dl->PathFillConvex(colour);
}

const char* const s_wordLetters[WordParts] = {"C", "R", "U", nullptr, "2"};
} // namespace

// The bars are those of the drawing the mark was designed as, a square of 100: the thin ones 7
// wide and 7 to either side of the middle, the broad one 16 wide, all 84 long with round ends,
// the thin ones cut 2.5 clear of the broad one where it passes over them.
void cruxMark(ImDrawList* dl, float cx, float cy, float size, float a, const MarkPose* pose)
{
	static const MarkPose rest;
	const MarkPose& p = (pose != nullptr) ? *pose : rest;
	a = std::clamp(a, 0.f, 1.f);
	if (a <= 0.004f || size <= 0)
		return;
	const float u = size * p.scale / 100 * S;
	const ImVec2 c = V(cx, cy);
	// (A bar that stands upright in the drawing and is turned by t degrees runs along t + 90.)
	const float thinTurn = (p.thinAngle + p.turn) * IM_PI / 180, broadTurn = (45 + p.turn) * IM_PI / 180;
	const float thinAlong = thinTurn + IM_PI / 2, broadAlong = broadTurn + IM_PI / 2;
	const bool square = std::fabs(p.thinAngle + 45) < 0.5f; // the bars cross at right angles
	const float broadHalf = p.broadLength / 2;
	for (int i = 0; i < 2; i++)
	{
		const float aside = (i == 0) ? -7.f : 7.f;
		const float length = p.thinLength[i];
		const ImU32 colour = alpha(col::lightSkin ? col::text : col::brandCream, a * std::clamp(p.thinAlpha[i], 0.f, 1.f));
		if (length <= 0.01f)
			continue;
		const float slide = p.thinSlide[i];
		const ImVec2 middle(c.x + (std::cos(thinTurn) * aside + std::cos(thinAlong) * slide) * u,
			c.y + (std::sin(thinTurn) * aside + std::sin(thinAlong) * slide) * u);
		// Where the broad bar passes over this one: along the broad bar that is -aside from the
		// mark's middle, and along this one it is its own middle (less what it has slid).
		const bool under = square && p.broadAlpha > 0.5f && -aside >= p.broadSlide - broadHalf - 1 && -aside <= p.broadSlide + broadHalf + 1;
		const float g0 = -slide - 10.5f, g1 = -slide + 10.5f;
		if (under && g1 > -length / 2 && g0 < length / 2)
		{
			barPiece(dl, middle, thinAlong, -length / 2 * u, g0 * u, 7 * u, colour, true, false);
			barPiece(dl, middle, thinAlong, g1 * u, length / 2 * u, 7 * u, colour, false, true);
		}
		else
		{
			barPiece(dl, middle, thinAlong, -length / 2 * u, length / 2 * u, 7 * u, colour, true, true);
		}
	}
	if (p.broadLength > 0.01f)
	{
		const ImVec2 middle(c.x + std::cos(broadAlong) * p.broadSlide * u, c.y + std::sin(broadAlong) * p.broadSlide * u);
		barPiece(dl, middle, broadAlong, -broadHalf * u, broadHalf * u, 16 * u, alpha(col::brandOrange, a * std::clamp(p.broadAlpha, 0.f, 1.f)), true, true);
		// The light passing along it: a short, narrower bar of a lighter orange, kept inside the
		// broad one and fading in and out at its ends.
		if (p.glint > -1.2f && p.glint < 1.2f)
		{
			const float half = 9.f, inner = broadHalf - 8.f;
			const float at = std::clamp(p.glint, -1.f, 1.f) * inner;
			const float from = std::max(at - half, -inner), to = std::min(at + half, inner);
			const float edge = 1.f - std::clamp((std::fabs(p.glint) - 0.75f) / 0.45f, 0.f, 1.f);
			if (to > from)
				barPiece(dl, middle, broadAlong, from * u, to * u, 7 * u, col::rgba(255, 226, 180, static_cast<int>(150 * a * edge * p.broadAlpha)), true, true);
		}
	}
}

// The name is laid out as its drawing was: each letter as wide as the face makes it and a
// hundredth of the type more, the mark in a square of the type's size that the letters beside it
// reach into by an eleventh of it.
WordLayout wordLayout(float type)
{
	WordLayout out{};
	ImFontBaked* const baked = brand()->GetFontBaked(BIG_TYPE * S);
	float x = 0;
	for (int part = 0; part < WordParts; part++)
	{
		float width;
		if (part == WordMark)
		{
			width = type * (1 - 2 * 20.f / 220.f);
		}
		else
		{
			const ImFontGlyph* const glyph = (baked != nullptr) ? baked->FindGlyph(static_cast<ImWchar>(s_wordLetters[part][0])) : nullptr;
			width = ((glyph != nullptr) ? glyph->AdvanceX / (BIG_TYPE * S) : 0.6f) * type + 0.01f * type;
		}
		out.centre[part] = x + width / 2;
		x += width;
	}
	out.width = x;
	out.mark = type;
	return out;
}

// (The face is made at two sizes only, the start-up's and the top bar's; a letter on its way
// between them is one of the two, scaled: a new size every frame would be a new set of glyphs
// every frame.)
void wordLetter(ImDrawList* dl, int part, float cx, float cy, float type, float a)
{
	a = std::clamp(a, 0.f, 1.f);
	if (part < 0 || part >= WordParts || s_wordLetters[part] == nullptr || a <= 0.004f || type <= 0.5f)
		return;
	ImFont* const font = brand();
	const float made = (type >= 120) ? BIG_TYPE : BAR_TYPE;
	const float px = made * S;
	ImFontBaked* const baked = font->GetFontBaked(px);
	if (baked == nullptr)
		return;
	const ImFontGlyph* const glyph = baked->FindGlyph(static_cast<ImWchar>(s_wordLetters[part][0]));
	const ImFontGlyph* const capital = baked->FindGlyph(static_cast<ImWchar>('R'));
	if (glyph == nullptr || capital == nullptr)
		return;
	// Its box's middle at cx, and the middle of a capital's height at cy.
	const ImVec2 at(cx * S - glyph->AdvanceX / 2, cy * S - (capital->Y0 + capital->Y1) / 2);
	const int from = dl->VtxBuffer.Size;
	dl->AddText(font, px, at, alpha(part == Word2 ? col::brandOrange : (col::lightSkin ? col::text : col::brandCream), a), s_wordLetters[part]);
	const float k = type / made;
	if (k != 1)
	{
		for (int i = from; i < dl->VtxBuffer.Size; i++)
		{
			ImDrawVert& v = dl->VtxBuffer[i];
			v.pos.x = cx * S + (v.pos.x - cx * S) * k;
			v.pos.y = cy * S + (v.pos.y - cy * S) * k;
		}
	}
}

float wordMarkWidth(float type)
{
	return wordLayout(type).width;
}

float wordMark(ImDrawList* dl, float x, float cy, float type, float a, const MarkPose* pose)
{
	const WordLayout layout = wordLayout(type);
	for (int part = 0; part < WordParts; part++)
	{
		if (part == WordMark)
			cruxMark(dl, x + layout.centre[part], cy, layout.mark, a, pose);
		else
			wordLetter(dl, part, x + layout.centre[part], cy, type, a);
	}
	return layout.width;
}

// A disc in a drive, drawn: a PlayStation 2 DVD from its shiny side, gold, or a PlayStation 2
// CD, blue. The light on it stays where it is while the disc turns under it; three marks on its
// inner band (and the colours the light breaks into) are what shows the turning.
void goldDisc(ImDrawList* dl, float cx, float cy, float r, float spin, float a, bool blue)
{
	if (a <= 0.004f || r <= 0)
		return;
	const ImU32 base = blue ? col::rgba(96, 132, 214) : col::rgba(214, 190, 128);
	const bool large = r > 40;
	if (large)
		softDisc(dl, cx + r * 0.05f, cy + r * 0.10f, r * 1.12f, col::rgba(0, 0, 0, static_cast<int>(120 * a)), 48);
	const int segments = large ? 180 : 60;
	const auto at = [&](float radius, float angle) { return V(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius); };
	const auto rainbow = [](float hue) {
		hue -= std::floor(hue);
		float red, green, blue;
		ImGui::ColorConvertHSVtoRGB(hue, 0.60f, 1.f, red, green, blue);
		return ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 1.f));
	};
	// The colour at an angle: the gold, the colours of the light on it, a white streak, and how
	// deep in shadow that part of the disc is.
	const auto shade = [&](float angle, float depth, float sheen) {
		const float rel = angle - spin;
		const float streak = std::pow(std::fabs(std::cos(rel)), 14.f);
		const float band = std::pow(std::fabs(std::cos(rel - 1.0f)), 5.f);
		ImU32 c = mix(base, rainbow(rel / IM_PI + 0.08f), (0.16f + 0.30f * band) * sheen);
		c = mix(c, col::rgba(255, 255, 255), 0.80f * streak * sheen);
		c = mix(c, col::rgba(0, 0, 0), depth);
		return alpha(c, a);
	};
	const auto ring = [&](float r0, float r1, float d0, float d1, float sheen) {
		ImU32 inner = shade(0, d0, sheen), outer = shade(0, d1, sheen);
		for (int i = 0; i < segments; i++)
		{
			const float a0 = 2 * IM_PI * i / segments, a1 = 2 * IM_PI * (i + 1) / segments;
			const ImU32 nextInner = shade(a1, d0, sheen), nextOuter = shade(a1, d1, sheen);
			quad(dl, at(r0, a0), at(r1, a0), at(r1, a1), at(r0, a1), inner, outer, nextOuter, nextInner);
			inner = nextInner;
			outer = nextOuter;
		}
	};
	ring(r * 0.40f, r * 0.965f, 0.20f, 0.02f, 1.f); // the data
	ring(r * 0.965f, r, 0.0f, 0.18f, 0.6f);         // the rim
	ring(r * 0.33f, r * 0.40f, 0.42f, 0.30f, 0.5f); // the mirror band
	// The clear hub and the hole.
	dl->AddCircleFilled(V(cx, cy), r * 0.33f * S, col::rgba(226, 232, 244, static_cast<int>(64 * a)), 64);
	dl->AddCircleFilled(V(cx, cy), r * 0.15f * S, col::rgba(8, 12, 20, static_cast<int>(235 * a)), 48);
	if (large)
	{
		dl->AddCircle(V(cx, cy), r * 0.245f * S, col::rgba(255, 255, 255, static_cast<int>(70 * a)), 64, 1.5f * S);
		for (const float groove : {0.55f, 0.70f, 0.85f})
			dl->AddCircle(V(cx, cy), r * groove * S, col::rgba(255, 255, 255, static_cast<int>(16 * a)), 96, 1.f * S);
		for (int i = 0; i < 3; i++)
		{
			const float angle = spin + 2 * IM_PI * i / 3;
			dl->AddLine(at(r * 0.345f, angle), at(r * 0.39f, angle), col::rgba(255, 255, 255, static_cast<int>(150 * a)), 2.f * S);
		}
	}
	dl->AddCircle(V(cx, cy), r * S, col::rgba(255, 255, 255, static_cast<int>(70 * a)), large ? 96 : 40, (large ? 1.5f : 1.f) * S);
	dl->AddCircle(V(cx, cy), r * 0.33f * S, col::rgba(255, 255, 255, static_cast<int>(60 * a)), 64, 1.f * S);
}

float discSpin()
{
	// (A turn in 2.6 seconds; with less motion, or none, the disc is there and does not turn.)
	return (motion() == MotionFull) ? static_cast<float>(std::fmod(timeNow * 2.4, 2.0 * 3.14159265358979323846)) : 0.6f;
}

// ------------------------------------------------------------ button glyphs

// A DualSense button, as the face buttons are printed: white symbol on a dark disc.
float glyph(ImDrawList* dl, Glyph g, float x, float cy, float size)
{
	const float r = size / 2;
	const ImU32 bg = alpha(col::text, 34 / 255.f), fg = col::text;
	const float t = size * 0.075f;
	const auto label = [&](const char* s, float w) {
		rect(dl, x, cy - r, w, size, bg, r * 0.55f);
		const ImVec2 ts = textSize(bold(), size * 0.5f, s);
		text(dl, bold(), size * 0.5f, x + (w - ts.x) / 2, cy - ts.y / 2, fg, s);
		return w;
	};
	const auto pair = [&](const char* first, const char* second) {
		const float w = size * 1.35f, gap = size * 0.2f, x0 = x;
		label(first, w);
		x += w + gap;
		label(second, w);
		x = x0;
		return w * 2 + gap;
	};
	switch (g)
	{
		case Glyph::L1:
			return label("L1", size * 1.35f);
		case Glyph::R1:
			return label("R1", size * 1.35f);
		case Glyph::L1R1:
			return pair("L1", "R1");
		case Glyph::L2:
			return label("L2", size * 1.35f);
		case Glyph::R2:
			return label("R2", size * 1.35f);
		case Glyph::L2R2:
			return pair("L2", "R2");
		case Glyph::R3:
			return label("R3", size * 1.35f);
		case Glyph::Options:
		{
			rect(dl, x, cy - r * 0.8f, size * 1.1f, size * 0.8f, bg, r * 0.5f);
			for (int i = -1; i <= 1; i++)
				rect(dl, x + size * 0.3f, cy + i * size * 0.18f - t / 2, size * 0.5f, t, fg, t / 2);
			return size * 1.1f;
		}
		case Glyph::TouchPad:
		{
			rect(dl, x, cy - r * 0.8f, size * 1.5f, size * 0.8f, bg, r * 0.4f);
			outline(dl, x + size * 0.2f, cy - r * 0.5f, size * 1.1f, size * 0.5f, fg, r * 0.2f, t);
			return size * 1.5f;
		}
		case Glyph::DPadLR:
		case Glyph::DPadUD:
		{
			const float cx = x + r;
			dl->AddCircleFilled(V(cx, cy), r * S, bg);
			const float a = size * 0.16f;
			const bool lr = (g == Glyph::DPadLR);
			for (int s : {-1, 1})
			{
				const float ox = lr ? s * size * 0.22f : 0, oy = lr ? 0 : s * size * 0.22f;
				const ImVec2 tip = V(cx + ox + (lr ? s * a : 0), cy + oy + (lr ? 0 : s * a));
				const ImVec2 b1 = V(cx + ox + (lr ? -s * a * 0.4f : -a), cy + oy + (lr ? -a : -s * a * 0.4f));
				const ImVec2 b2 = V(cx + ox + (lr ? -s * a * 0.4f : a), cy + oy + (lr ? a : -s * a * 0.4f));
				dl->AddTriangleFilled(tip, b1, b2, fg);
			}
			return size;
		}
		default:
			break;
	}
	const float cx = x + r;
	dl->AddCircleFilled(V(cx, cy), r * S, bg);
	const float k = size * 0.22f;
	// (With Cross and Circle exchanged, a hint shows the button that does it.)
	if (options().swapConfirm && (g == Glyph::Cross || g == Glyph::Circle))
		g = (g == Glyph::Cross) ? Glyph::Circle : Glyph::Cross;
	switch (g)
	{
		case Glyph::Cross:
			dl->AddLine(V(cx - k, cy - k), V(cx + k, cy + k), fg, t * S);
			dl->AddLine(V(cx - k, cy + k), V(cx + k, cy - k), fg, t * S);
			break;
		case Glyph::Circle:
			dl->AddCircle(V(cx, cy), k * 1.15f * S, fg, 0, t * S);
			break;
		case Glyph::Square:
			outline(dl, cx - k, cy - k, k * 2, k * 2, fg, 0, t);
			break;
		case Glyph::Triangle:
			dl->AddTriangle(V(cx, cy - k * 1.15f), V(cx - k * 1.15f, cy + k * 0.85f), V(cx + k * 1.15f, cy + k * 0.85f), fg, t * S);
			break;
		default:
			break;
	}
	return size;
}

// The bottom bar: hints right-aligned, as on Steam's.
void hintBar(ImDrawList* dl, const std::vector<Hint>& hints)
{
	rect(dl, 0, H - 72, W, 72, col::bar);
	rect(dl, 0, H - 72, W, 1, col::line);
	// (Its words stand still for as long as a screen is up: they go faint when nothing is pressed
	// for a while, and are back with the next button.)
	const Layer words = beginLayer(dl);
	float x = W - 48;
	for (auto it = hints.rbegin(); it != hints.rend(); ++it)
	{
		const ImVec2 ts = textSize(regular(), 24, it->label);
		x -= ts.x;
		text(dl, regular(), 24, x, H - 36 - ts.y / 2, col::text, it->label);
		x -= 12;
		const float gw = glyph(dl, it->glyph, 0, -1000); // measure
		x -= gw;
		glyph(dl, it->glyph, x, H - 36);
		x -= 40;
	}
	endLayer(words, steadyAlpha());
}

void drawSwitch(ImDrawList* dl, float right, float cy, bool on, bool focused, float scale)
{
	const float sw = 68 * scale, sh = 36 * scale;
	rect(dl, right - sw, cy - sh / 2, sw, sh, on ? (focused ? col::text : col::accent) : alpha(col::text, 40 / 255.f), sh / 2);
	dl->AddCircleFilled(V(right - sw + (on ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5 * scale) * S,
		(on && focused) ? col::accent : col::text);
}

void drawArrows(ImDrawList* dl, float left, float right, float cy, ImU32 c)
{
	dl->AddTriangleFilled(V(left - 20, cy), V(left - 8, cy - 10), V(left - 8, cy + 10), c);
	dl->AddTriangleFilled(V(right + 20, cy), V(right + 8, cy - 10), V(right + 8, cy + 10), c);
}

// ------------------------------------------------------------------ input

Input in{};

namespace
{
uint32_t s_held = 0, s_pressed = 0;
// After a screen changes, a button that was down then is not read until it was let go: the press
// that opened a screen is not the first press in it. Button by button, so that one that never
// comes up (a controller that went away with it held) leaves the others usable.
uint32_t s_holdMask = 0;
// What the controller really holds, whatever is being ignored.
uint32_t s_down = 0;
int s_padsConnected = 1;

// Edge-triggered buttons, and directions that repeat while held, as a TV interface's lists do.
void readInput()
{
	const Pad pad = (be != nullptr) ? be->pad() : Pad();
	s_padsConnected = pad.connected;
	const uint32_t before = s_held;
	s_down = pad.held;
	// (The button that ended the screen saver did only that.)
	if (careWoke())
		s_holdMask |= s_down;
	s_holdMask &= s_down;
	s_held = s_down & ~s_holdMask;
	s_pressed = s_held & ~before;
	in = Input{};

	struct Repeat
	{
		double next = 0;
		bool held = false;
		int count = 0;
	};
	static Repeat rep[4];
	const bool held[4] = {(s_held & Pad::Up) != 0, (s_held & Pad::Down) != 0, (s_held & Pad::Left) != 0, (s_held & Pad::Right) != 0};

	in.accept = (s_pressed & (options().swapConfirm ? Pad::Circle : Pad::Cross)) != 0;
	in.back = (s_pressed & (options().swapConfirm ? Pad::Cross : Pad::Circle)) != 0;
	in.triangle = (s_pressed & Pad::Triangle) != 0;
	in.square = (s_pressed & Pad::Square) != 0;
	in.l1 = (s_pressed & Pad::L1) != 0;
	in.r1 = (s_pressed & Pad::R1) != 0;
	in.l3 = (s_pressed & Pad::L3) != 0;
	in.r3 = (s_pressed & Pad::R3) != 0;
	in.options = (s_pressed & Pad::Options) != 0;
	in.touch = (s_pressed & Pad::TouchPad) != 0;
	in.any = (s_pressed != 0);

	bool* out[4] = {&in.up, &in.down, &in.left, &in.right};
	for (int i = 0; i < 4; i++)
	{
		if (!held[i])
		{
			rep[i].held = false;
			rep[i].count = 0;
		}
		else if (!rep[i].held)
		{
			rep[i].held = true;
			rep[i].next = timeNow + 0.38;
			rep[i].count = 0;
			*out[i] = true;
		}
		else if (timeNow >= rep[i].next)
		{
			rep[i].next = timeNow + 0.075;
			rep[i].count++;
			*out[i] = true;
			in.repeats = std::max(in.repeats, rep[i].count);
		}
	}

	// The interface's sounds for what the buttons do (a dialog's own come with the dialog).
	if (be != nullptr)
	{
		if (in.up || in.down || in.left || in.right)
			be->cue(CueFocus);
		if (in.accept)
			be->cue(CueSelect);
		else if (in.back)
			be->cue(CueBack);
		if (in.l1 || in.r1)
			be->cue(CueTab);
	}

	// The triggers step through a list's letters and pages: held, they step on.
	static Repeat trig[2];
	const bool trigHeld[2] = {(s_held & Pad::L2) != 0, (s_held & Pad::R2) != 0};
	bool* trigOut[2] = {&in.l2, &in.r2};
	for (int i = 0; i < 2; i++)
	{
		if (!trigHeld[i])
			trig[i].held = false;
		else if (!trig[i].held)
		{
			trig[i].held = true;
			trig[i].next = timeNow + 0.40;
			*trigOut[i] = true;
		}
		else if (timeNow >= trig[i].next)
		{
			trig[i].next = timeNow + 0.18;
			*trigOut[i] = true;
		}
	}
}
} // namespace

// ----------------------------------------------------------- frame set-up

namespace
{
void measure()
{
	const ImGuiIO& io = ImGui::GetIO();
	// A smaller interface is the same screens on a taller page of units: everything in them smaller.
	H = 1080.f * 100.f / static_cast<float>(std::clamp(s_options.uiSize, 60, 100));
	S = std::max(io.DisplaySize.y, 1.f) / H;
	W = io.DisplaySize.x / S;
}

int s_inputFrame = -1;
} // namespace

ImDrawList* beginScreen(const char* id, bool opaque)
{
	const ImGuiIO& io = ImGui::GetIO();
	measure();
	// (Once a frame: a screen that hands over to another in the same frame does not read twice.)
	if (s_inputFrame != frameNow)
	{
		s_inputFrame = frameNow;
		readInput();
		dialogInput();
	}
	else
	{
		in = Input{};
	}

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
	ImGui::Begin(id, nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);
	ImGui::PopStyleVar(3);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	applySkin();
	if (opaque)
		drawBackdrop(dl);
	return dl;
}

void endScreen()
{
	dialogDraw(ImGui::GetWindowDrawList());
	careOverScreen(ImGui::GetWindowDrawList());
	ImGui::End();
}

// --------------------------------------------------------------- top bar

// The tabs, left to right; USB only while USB drives are on in the settings, Disc only while a
// disc is in a disc drive (the console's own or a USB one).
std::vector<TopTab> topTabs()
{
	std::vector<TopTab> tabs{{Internal, ICON_FA_HARD_DRIVE, "Internal"}};
	if (be != nullptr && be->usbWanted())
		tabs.push_back({Usb, ICON_FA_PLUG, "USB"});
	tabs.push_back({Network, ICON_FA_NETWORK_WIRED, "Network"});
	if (discNow().present)
		tabs.push_back({DiscDrive, ICON_FA_COMPACT_DISC, "Disc"});
	return tabs;
}

namespace
{
float clamp01(float t)
{
	return std::clamp(t, 0.f, 1.f);
}

// An ease that goes a little past its end and comes back.
float easeOutBack(float t)
{
	t = clamp01(t);
	constexpr float c1 = 1.70158f, c3 = c1 + 1.f;
	return 1.f + c3 * (t - 1.f) * (t - 1.f) * (t - 1.f) + c1 * (t - 1.f) * (t - 1.f);
}

// A swing that dies away: 0 at the start and the end, between them a few turns either side.
float dyingSwing(float t, float turns)
{
	t = clamp01(t);
	return std::sin(t * turns * 2.f * IM_PI) * (1.f - t) * (1.f - t);
}

// The pseudo-random number of a whole number: the same each time, so the mark's moves are not
// tied to the frame rate or the order of anything else.
float noiseOf(int n)
{
	unsigned x = static_cast<unsigned>(n) * 2654435761u;
	x ^= x >> 15;
	x *= 2246822519u;
	x ^= x >> 13;
	return static_cast<float>(x & 0xFFFFu) / 65535.f;
}

// The mark in the top bar is never quite still (Motion: everything): it breathes, a light runs
// along its broad bar now and then, and every so often it does one of four things: turns over,
// opens and closes like scissors, throws its thin bars out and catches them, or pulls its broad
// bar in and lets it spring back. Changing tabs gives it a nudge, in the way the focus went
// (and with Motion: less, that nudge is all it does).
MarkPose markLife(int tab)
{
	MarkPose p;
	const Motion m = motion();
	if (m == MotionOff)
		return p;
	const float t = static_cast<float>(timeNow);

	// The nudge: a turn that springs back, towards the tab the focus went to.
	static int lastTab = -1;
	static float nudgeAt = -100.f, nudgeWay = 0.f;
	if (tab != lastTab)
	{
		if (lastTab >= 0)
		{
			nudgeAt = t;
			nudgeWay = (tab > lastTab) ? 1.f : -1.f;
		}
		lastTab = tab;
	}
	p.turn += 22.f * nudgeWay * dyingSwing((t - nudgeAt) / 0.9f, 1.25f);
	if (m != MotionFull)
		return p;

	// Breathing: the bars a touch longer and shorter, the thin pair out of step with the broad one.
	const float breath = std::sin(t * 2.f * IM_PI / 4.2f);
	p.broadLength = 84.f + 1.6f * breath;
	p.thinLength[0] = p.thinLength[1] = 84.f - 1.2f * breath;

	// The light along the broad bar: every 7 seconds, for 0.9 of one.
	const float glintPhase = std::fmod(t + 2.5f, 7.f);
	if (glintPhase < 0.9f)
		p.glint = -1.2f + 2.4f * (glintPhase / 0.9f);

	// The moves: one every 13 seconds, chosen by the cycle's number, taking 1.4 seconds.
	const int cycle = static_cast<int>(std::floor(t / 13.f));
	const float into = (t - cycle * 13.f - 5.f) / 1.4f; // (5 seconds into each cycle)
	if (into >= 0.f && into <= 1.f)
	{
		switch (static_cast<int>(noiseOf(cycle) * 4.f) & 3)
		{
			case 0: // turns over: half a turn, a touch past and back
				p.turn += 180.f * easeOutBack(into);
				break;
			case 1: // scissors: the thin bars swing about their crossing and settle
				p.thinAngle += 26.f * dyingSwing(into, 2.f);
				break;
			case 2: // the thin bars thrown out along themselves and caught
				p.thinSlide[0] = -14.f * dyingSwing(into, 1.5f);
				p.thinSlide[1] = 14.f * dyingSwing(into, 1.5f);
				break;
			default: // the broad bar pulled in and let go
				p.broadLength *= 1.f - 0.32f * std::fabs(dyingSwing(into, 1.f)) - 0.08f * dyingSwing(into, 3.f);
				p.scale = 1.f + 0.06f * dyingSwing(into, 2.f);
				break;
		}
	}
	return p;
}
} // namespace

void topBar(ImDrawList* dl, int active_tab, bool inGame, Reach reach)
{
	rect(dl, 0, 0, W, 96, col::bar);
	rect(dl, 0, 95, W, 1, col::line);

	// The app: its mark and its name.
	if (splash.flight >= 1.f)
	{
		const MarkPose life = markLife(active_tab);
		wordMark(dl, BAR_NAME_X, BAR_NAME_Y, BAR_TYPE, 1.f, &life);
	}

	// What is on the right: the clock, the pads, and that a game runs.
	char clock[16] = "";
	const time_t t = time(nullptr);
	struct tm local;
	if (localtime_r(&t, &local) != nullptr)
		strftime(clock, sizeof(clock), "%H:%M", &local);
	const ImVec2 cs = textSize(bold(), 30, clock);
	const int pads = s_padsConnected;
	const std::string padText = std::string(ICON_FA_GAMEPAD) + (pads > 1 ? "  " + std::to_string(pads) : "");
	const ImVec2 ps = textSize(regular(), 28, padText.c_str());
	const char* const badge = ICON_FA_CIRCLE_PLAY "  Game running";
	const ImVec2 bs = textSize(regular(), 22, badge);
	const float padX = W - 48 - cs.x - 36 - ps.x;
	const float badgeX = padX - 36 - bs.x - 28;

	// Tabs, switched with L1 / R1: in the middle of the bar, moved aside (and drawn closer
	// together) when there are so many that they would reach the name or the clock.
	const std::vector<TopTab> tabs = topTabs();
	const float freeLeft = BAR_NAME_X + wordMarkWidth(BAR_TYPE) + 32, freeRight = (inGame ? badgeX : padX) - 32;
	const float shoulders = (reach == Reach::All) ? 64.f : 0.f; // the L1 and R1 glyphs at their ends
	float inset = 28, between = 12, total = 0;
	std::vector<float> widths;
	const auto measureTabs = [&] {
		widths.clear();
		total = 0;
		for (const TopTab& tab : tabs)
		{
			const std::string label = std::string(tab.icon) + "   " + tab.name;
			widths.push_back(textSize(bold(), 26, label.c_str()).x + inset * 2);
			total += widths.back() + between;
		}
	};
	measureTabs();
	if (total + shoulders * 2 > freeRight - freeLeft)
	{
		inset = 18;
		between = 4;
		measureTabs();
	}
	float x = (W - total) / 2 + between / 2;
	x = std::min(x, freeRight - shoulders - total + between / 2);
	x = std::max(x, freeLeft + shoulders + between / 2);
	if (reach == Reach::All)
		glyph(dl, Glyph::L1, x - 64, 48, 30);
	// The pill under the open tab slides to the next one.
	static float pillX = -1, pillW = 0;
	static int pillFrame = -10;
	{
		float at = x;
		for (size_t i = 0; i < tabs.size(); i++)
		{
			if (active_tab == tabs[i].id)
			{
				const bool fresh = pillX < 0 || frameNow > pillFrame + 2 || motion() == MotionOff;
				pillX = fresh ? at : approach(pillX, at, 20);
				pillW = fresh ? widths[i] : approach(pillW, widths[i], 20);
				pillFrame = frameNow;
				glow(dl, pillX, 24, pillW, 48, 24, alpha(col::accent, 0.5f), 10);
				rect(dl, pillX, 24, pillW, 48, col::accent, 24);
			}
			at += widths[i] + between;
		}
	}
	for (size_t i = 0; i < tabs.size(); i++)
	{
		const bool on = (active_tab == tabs[i].id);
		const bool reachable = on || reach == Reach::All;
		const ImU32 colour = on ? col::onAccent : reachable ? col::dim : col::faint;
		if (tabs[i].id == DiscDrive)
		{
			// The disc itself, turning, where the others have an icon.
			const float iconWidth = textSize(bold(), 26, tabs[i].icon).x;
			// (Blue while the only discs are CDs.)
			const DiscState& discs = discNow();
			const bool blue = !discs.games.empty() &&
							  std::all_of(discs.games.begin(), discs.games.end(), [](const GameInfo& game) { return game.cd; });
			goldDisc(dl, x + inset + iconWidth / 2, 48, 17, discSpin(), reachable ? 1.f : 0.6f, blue);
			text(dl, bold(), 26, x + inset + iconWidth, 33, colour, (std::string("   ") + tabs[i].name).c_str());
		}
		else
		{
			const std::string label = std::string(tabs[i].icon) + "   " + tabs[i].name;
			text(dl, bold(), 26, x + inset, 33, colour, label.c_str());
		}
		x += widths[i] + between;
	}
	if (reach == Reach::All)
		glyph(dl, Glyph::R1, x + 10, 48, 30);

	// Under the clock: the version, that this is a work in progress, the build number and the author.
	text(dl, bold(), 28, W - 48 - cs.x, 36 - cs.y / 2, col::text, clock);
	{
		static const std::string stamp = [] {
			const char* version = ps5px_build_version ? ps5px_build_version() : "";
			const char* number = ps5px_build_number ? ps5px_build_number() : "0";
			std::string s = (*version != '\0') ? std::string(version) + " WIP" : std::string("WIP");
			return s + "  \xC2\xB7  build " + number + "  \xC2\xB7  by Press5elect";
		}();
		const ImVec2 ss = textSize(regular(), 17, stamp.c_str());
		text(dl, regular(), 17, W - 48 - ss.x, 72 - ss.y / 2, col::dim, stamp.c_str());
	}
	text(dl, regular(), 28, padX, 48 - ps.y / 2, pads > 0 ? col::text : col::faint, padText.c_str());
	if (inGame)
	{
		rect(dl, badgeX, 30, bs.x + 28, 36, alpha(col::good, 0.18f), 18);
		text(dl, regular(), 22, badgeX + 14, 48 - bs.y / 2, col::good, badge);
	}
}

// ---------------------------------------------------------------- dialogs

namespace
{
struct Dialog
{
	enum Kind { Message, Confirm, Pick, Address, Text, Keyboard, Code } kind = Message;
	std::string title, body;
	std::string yes = "Yes", no = "No";
	std::vector<std::string> names;
	int current = -1;   // Pick: the choice in effect
	int focus = 0;      // Address: which of its four numbers
	float scroll = 0;
	float open = 0;
	int numbers[4] = {0, 0, 0, 0}; // Address
	bool set = false;              // Address: there was one when the dialog opened
	std::vector<std::string> shown; // Text: `names` as lines that fit the sheet
	float shownFor = 0;             // Text: the scale `shown` was made for (0: not yet)
	float top = -1;                 // Text: the first line that shows (-1: its end, when it opens)
	std::string value;              // Keyboard: what was typed so far
	bool secret = false;            // Keyboard: a password, shown as dots
	bool reveal = false;            // Keyboard: the password is shown after all
	int page = 0;                   // Keyboard: 0 small letters, 1 capitals, 2 signs
	int kx = 0, ky = 1;             // Keyboard: the key the directions are on
	int most = 64;                  // Keyboard: how long the text may be
	std::vector<uint8_t> code;      // Code: the QR code's modules, codeSize x codeSize
	int codeSize = 0;
	std::function<void()> done;
	std::function<void(bool)> answered;
	std::function<void(int)> picked;
	std::function<void(const std::string&)> entered;
};
std::deque<Dialog> s_dialogs; // the front one is up; the rest wait their turn

struct Toast
{
	std::string text;
	double until = 0;
	float shown = 0;
};
std::deque<Toast> s_toasts;

constexpr int PICK_ROWS = 9;
// The sheet of a text: its size, its lines.
constexpr float TEXT_W = 1720, TEXT_H = 880, TEXT_LINE = 27, TEXT_SIZE = 20;
constexpr int TEXT_ROWS = 26;

// The keyboard: three pages of four rows of ten keys, and a row of five wide ones under them.
constexpr int KEY_COLS = 10, KEY_ROWS = 4;
constexpr const char* s_keys[3][KEY_ROWS] = {
	{"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm._-"},
	{"1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM._-"},
	{"!\"#$%&'()*", "+,/:;<=>?[", "\\]^`{|}~@.", "_-.,:;!?#&"},
};
constexpr bool keysAreWhole()
{
	for (const auto& page : s_keys)
	{
		for (const char* row : page)
		{
			int n = 0;
			while (row[n] != 0)
				n++;
			if (n != KEY_COLS)
				return false;
		}
	}
	return true;
}
static_assert(keysAreWhole(), "every row of the keyboard has ten keys");
enum WideKey { KeyShift, KeySigns, KeySpace, KeyBack, KeyDone, WideKeys };
const char* const s_wide[WideKeys] = {"Capitals", "Signs", "Space", "Delete", "Done"};
// (The wide keys' places among the ten columns: where each begins and how many it spans.)
constexpr int s_wideFrom[WideKeys] = {0, 2, 4, 6, 8};

// The last character of a text that may hold characters of several bytes goes.
void dropLast(std::string& text)
{
	if (text.empty())
		return;
	text.pop_back();
	while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80)
		text.pop_back();
	// (The first byte of the character that the bytes just taken belonged to.)
	if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0xc0)
		text.pop_back();
}

int characters(const std::string& text)
{
	int n = 0;
	for (const char c : text)
		n += ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ? 1 : 0;
	return n;
}
} // namespace

void message(const std::string& title, const std::string& body, std::function<void()> then)
{
	Dialog d;
	d.kind = Dialog::Message;
	d.title = title;
	d.body = body;
	d.done = std::move(then);
	s_dialogs.push_back(std::move(d));
	// A button that is down as the dialog comes (in the game it comes over) does not answer it.
	holdInput();
}

void confirm(const std::string& title, const std::string& body, std::function<void(bool)> then, const char* yes, const char* no)
{
	Dialog d;
	d.kind = Dialog::Confirm;
	d.title = title;
	d.body = body;
	d.yes = yes;
	d.no = no;
	d.focus = 1; // on "No": a question is not answered yes by a button pressed too soon
	d.answered = std::move(then);
	s_dialogs.push_back(std::move(d));
	holdInput();
}

void pick(const std::string& title, std::vector<std::string> names, int current, std::function<void(int)> then)
{
	if (names.empty())
		return;
	Dialog d;
	d.kind = Dialog::Pick;
	d.title = title;
	d.names = std::move(names);
	d.current = current;
	d.focus = std::clamp(current, 0, static_cast<int>(d.names.size()) - 1);
	d.scroll = static_cast<float>(std::max(0, d.focus - PICK_ROWS / 2));
	d.picked = std::move(then);
	s_dialogs.push_back(std::move(d));
	holdInput();
}

bool parseAddress(const std::string& text, int numbers[4])
{
	int found[4] = {0, 0, 0, 0}, part = 0, digits = 0;
	for (const char c : text)
	{
		if (c >= '0' && c <= '9')
		{
			found[part] = found[part] * 10 + (c - '0');
			if (++digits > 3 || found[part] > 255)
				return false;
		}
		else if (c == '.' && digits > 0 && part < 3)
		{
			part++;
			digits = 0;
		}
		else
		{
			return false;
		}
	}
	if (part != 3 || digits == 0)
		return false;
	for (int i = 0; i < 4; i++)
		numbers[i] = found[i];
	return true;
}

void editAddress(const std::string& title, const std::string& now, const std::string& from, std::function<void(const std::string&)> then)
{
	Dialog d;
	d.kind = Dialog::Address;
	d.title = title;
	d.set = parseAddress(now, d.numbers);
	if (!d.set)
		parseAddress(from, d.numbers);
	d.entered = std::move(then);
	s_dialogs.push_back(std::move(d));
	holdInput();
}

void editText(const std::string& title, const std::string& now, bool secret, std::function<void(const std::string&)> then, int most)
{
	Dialog d;
	d.kind = Dialog::Keyboard;
	d.title = title;
	d.value = now;
	d.secret = secret;
	d.most = std::max(1, most);
	d.entered = std::move(then);
	s_dialogs.push_back(std::move(d));
	holdInput();
}

void showText(const std::string& title, std::vector<std::string> lines)
{
	Dialog d;
	d.kind = Dialog::Text;
	d.title = title;
	d.names = std::move(lines);
	s_dialogs.push_back(std::move(d));
	holdInput();
}

void showCode(const std::string& title, const std::string& body, std::vector<uint8_t> modules, int size)
{
	Dialog d;
	d.kind = Dialog::Code;
	d.title = title;
	d.body = body;
	d.code = std::move(modules);
	d.codeSize = (size > 0 && d.code.size() >= static_cast<size_t>(size) * size) ? size : 0;
	s_dialogs.push_back(std::move(d));
	holdInput();
}

void toast(const std::string& line, float seconds)
{
	if (line.empty())
		return;
	if (be != nullptr)
		be->cue(CueNotify);
	// The same line again is the same toast, shown for longer.
	for (Toast& t : s_toasts)
	{
		if (t.text == line)
		{
			t.until = timeNow + seconds;
			return;
		}
	}
	if (s_toasts.size() >= 4)
		s_toasts.pop_front();
	s_toasts.push_back({line, timeNow + seconds, 0.f});
}

// A list or a question belongs to the screen that asked: when another screen comes on, it goes
// unanswered (what it would have changed may be gone). A message stays until it is read.
void dropScreenDialogs()
{
	closeCardManager();
	for (auto it = s_dialogs.begin(); it != s_dialogs.end();)
	{
		if (it->kind != Dialog::Message && it->kind != Dialog::Text)
			it = s_dialogs.erase(it);
		else
			++it;
	}
}

bool dialogOpen()
{
	return !s_dialogs.empty() || cardManagerOpen() || (be != nullptr && be->listening() > 0);
}

void dialogInput()
{
	// A controller button is waited for: the Backend has the controller, and nobody else.
	if (be != nullptr && be->listening() > 0)
	{
		in = Input{};
		holdInput();
		return;
	}
	if (s_dialogs.empty())
	{
		// The card manager has the controller when no list or question of its own is up.
		if (cardManagerOpen())
		{
			const Input now = in;
			in = Input{};
			cardManagerInput(now);
		}
		return;
	}

	Dialog& d = s_dialogs.front();
	const Input now = in;
	in = Input{};
	switch (d.kind)
	{
		case Dialog::Code:
		case Dialog::Message:
			if (now.accept || now.back)
			{
				auto done = std::move(d.done);
				s_dialogs.pop_front();
				holdInput();
				if (done)
					done();
			}
			break;

		case Dialog::Confirm:
			if (now.left || now.right)
				d.focus = now.left ? 0 : 1;
			if (now.accept || now.back)
			{
				const bool yes = now.accept && d.focus == 0;
				auto answered = std::move(d.answered);
				s_dialogs.pop_front();
				holdInput();
				if (answered)
					answered(yes);
			}
			break;

		case Dialog::Pick:
		{
			const int n = static_cast<int>(d.names.size());
			if (now.up)
				d.focus = (d.focus > 0) ? d.focus - 1 : ((now.repeats > 0) ? 0 : n - 1);
			if (now.down)
				d.focus = (d.focus < n - 1) ? d.focus + 1 : ((now.repeats > 0) ? n - 1 : 0);
			if (now.l2)
				d.focus = std::max(0, d.focus - PICK_ROWS);
			if (now.r2)
				d.focus = std::min(n - 1, d.focus + PICK_ROWS);
			if (now.accept)
			{
				const int chosen = d.focus;
				auto picked = std::move(d.picked);
				s_dialogs.pop_front();
				holdInput();
				if (picked)
					picked(chosen);
			}
			else if (now.back)
			{
				s_dialogs.pop_front();
				holdInput();
			}
			break;
		}

		case Dialog::Text:
		{
			const float last = std::max(0.f, static_cast<float>(d.shown.size()) - TEXT_ROWS);
			if (d.top < 0)
				d.top = last;
			// A held direction goes faster the longer it is held.
			const float step = static_cast<float>(1 + std::min(now.repeats / 6, 8));
			if (now.up)
				d.top -= step;
			if (now.down)
				d.top += step;
			if (now.l1)
				d.top -= TEXT_ROWS - 2;
			if (now.r1)
				d.top += TEXT_ROWS - 2;
			if (now.l2)
				d.top = 0;
			if (now.r2)
				d.top = last;
			d.top = std::clamp(d.top, 0.f, last);
			if (now.back || now.accept)
			{
				s_dialogs.pop_front();
				holdInput();
			}
			break;
		}

		case Dialog::Keyboard:
		{
			// The directions, over the four rows of keys and the wide ones under them (ky == 4,
			// where kx is which wide key); past an edge, the other edge.
			if (now.up || now.down)
			{
				const int was = d.ky;
				d.ky = (d.ky + (now.down ? 1 : KEY_ROWS)) % (KEY_ROWS + 1);
				if (d.ky == KEY_ROWS && was != KEY_ROWS)
					d.kx = std::clamp(d.kx / 2, 0, WideKeys - 1);
				else if (was == KEY_ROWS && d.ky != KEY_ROWS)
					d.kx = s_wideFrom[std::clamp(d.kx, 0, WideKeys - 1)];
			}
			const int columns = (d.ky == KEY_ROWS) ? static_cast<int>(WideKeys) : KEY_COLS;
			if (now.left)
				d.kx = (d.kx + columns - 1) % columns;
			if (now.right)
				d.kx = (d.kx + 1) % columns;
			d.kx = std::clamp(d.kx, 0, columns - 1);

			const auto type = [&d](char c) {
				if (characters(d.value) < d.most)
					d.value.push_back(c);
			};
			bool done = now.options;
			bool leave = false;
			// A keyboard (a USB one on the console, or the phone's): what it typed goes in as it is.
			{
				std::string typed;
				unsigned keys = 0;
				if (be != nullptr && be->typed(typed, keys))
				{
					for (const char c : typed)
					{
						if (static_cast<unsigned char>(c) >= 0x20 || (static_cast<unsigned char>(c) & 0x80))
						{
							if (characters(d.value) < d.most || (static_cast<unsigned char>(c) & 0xc0) == 0x80)
								d.value.push_back(c);
						}
					}
					if (keys & 2u)
						dropLast(d.value);
					if (keys & 1u)
						done = true;
					if (keys & 4u)
						leave = true;
				}
			}
			if (now.accept)
			{
				if (d.ky < KEY_ROWS)
				{
					type(s_keys[d.page][d.ky][d.kx]);
				}
				else
				{
					switch (d.kx)
					{
						case KeyShift:
							d.page = (d.page == 1) ? 0 : 1;
							break;
						case KeySigns:
							d.page = (d.page == 2) ? 0 : 2;
							break;
						case KeySpace:
							type(' ');
							break;
						case KeyBack:
							dropLast(d.value);
							break;
						case KeyDone:
							done = true;
							break;
					}
				}
			}
			if (now.square)
				dropLast(d.value);
			if (now.triangle)
				type(' ');
			if (now.l1)
				d.page = (d.page == 1) ? 0 : 1;
			if (now.r1)
				d.page = (d.page == 2) ? 0 : 2;
			if (now.l2 && d.secret)
				d.reveal = !d.reveal;
			if (done)
			{
				const std::string value = d.value;
				auto entered = std::move(d.entered);
				s_dialogs.pop_front();
				holdInput();
				if (entered)
					entered(value);
			}
			else if (now.back || leave)
			{
				s_dialogs.pop_front();
				holdInput();
			}
			break;
		}

		case Dialog::Address:
		{
			if (now.left)
				d.focus = (d.focus + 3) % 4;
			if (now.right)
				d.focus = (d.focus + 1) % 4;
			d.focus = std::clamp(d.focus, 0, 3);
			// One at a time with the directions, ten with L1 and R1; past 255 it is at 0 again.
			const int step = (now.up ? 1 : 0) - (now.down ? 1 : 0) + (now.r1 ? 10 : 0) - (now.l1 ? 10 : 0);
			if (step != 0)
				d.numbers[d.focus] = ((d.numbers[d.focus] + step) % 256 + 256) % 256;
			if (now.accept || now.square)
			{
				char address[32] = "";
				if (now.accept)
					std::snprintf(address, sizeof(address), "%d.%d.%d.%d", d.numbers[0], d.numbers[1], d.numbers[2], d.numbers[3]);
				auto entered = std::move(d.entered);
				s_dialogs.pop_front();
				holdInput();
				if (entered)
					entered(address);
			}
			else if (now.back)
			{
				s_dialogs.pop_front();
				holdInput();
			}
			break;
		}
	}
}

namespace
{
void drawToasts(ImDrawList* dl)
{
	while (!s_toasts.empty() && timeNow > s_toasts.front().until + 0.3)
		s_toasts.pop_front();
	float y = H - 72 - 60;
	for (auto it = s_toasts.rbegin(); it != s_toasts.rend(); ++it)
	{
		Toast& t = *it;
		t.shown = approach(t.shown, timeNow < t.until ? 1.f : 0.f, 12);
		if (t.shown < 0.01f)
			continue;
		// (A long one goes over several lines: nothing of it is cut.)
		const float most = W - 400;
		const ImVec2 one = textSize(regular(), 24, t.text.c_str());
		const bool wraps = one.x > most;
		const ImVec2 ts = wraps ? textSize(regular(), 24, t.text.c_str(), most) : one;
		const float w = (wraps ? most : ts.x) + 56, h = ts.y + 22, x = (W - w) / 2;
		const float ty = y + 48 - h + (1 - t.shown) * 12;
		rect(dl, x, ty, w, h, alpha(col::sheetHi, 0.96f * t.shown), 24);
		outline(dl, x, ty, w, h, alpha(col::accent, 0.6f * t.shown), 24, 2);
		text(dl, regular(), 24, x + 28, ty + 11, alpha(col::text, t.shown), t.text.c_str(), wraps ? most : 0.f);
		y -= h + 10;
	}
}

// A button of a dialog.
float dialogButton(ImDrawList* dl, float x, float y, const char* label, bool on, float t)
{
	const ImVec2 ls = textSize(bold(), 26, label);
	const float w = std::max(ls.x + 72, 170.f);
	if (on)
		glow(dl, x, y, w, 60, 30, alpha(col::accent, 0.7f * t), 12);
	rect(dl, x, y, w, 60, alpha(on ? col::accent : col::panelHi, t), 30);
	text(dl, bold(), 26, x + (w - ls.x) / 2, y + 30 - ls.y / 2, alpha(on ? col::text : col::dim, t), label);
	return w;
}
} // namespace

void dialogDraw(ImDrawList* dl)
{
	// The wait for a controller button.
	const float listening = (be != nullptr) ? be->listening() : 0.f;
	if (listening > 0)
	{
		dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, 170));
		const float w = 820, h = 300, x = (W - w) / 2, y = (H - h) / 2;
		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, 255), 30);
		rect(dl, x, y, w, h, col::panel, 24);
		outline(dl, x, y, w, h, alpha(col::warm, 0.5f + 0.5f * std::sin(static_cast<float>(timeNow) * 6.f)), 24, 3);
		text(dl, bold(), 40, x + 56, y + 52, col::text, ICON_FA_GAMEPAD "   Press a button");
		text(dl, regular(), 26, x + 56, y + 124, col::dim,
			"Press the controller button (or move the stick) for it, and let go.", w - 112);
		char left[64];
		std::snprintf(left, sizeof(left), "Left as it is in %d s", static_cast<int>(std::ceil(listening)));
		text(dl, regular(), 22, x + 56, y + h - 64, col::faint, left);
		drawToasts(dl);
		return;
	}

	// The sounds of a dialog coming and going, and the keyboard typing into the one that types.
	{
		static size_t s_were = 0;
		static bool s_capturing = false;
		if (be != nullptr && s_dialogs.size() != s_were)
			be->cue(s_dialogs.size() > s_were ? CueModalOpen : CueModalClose);
		s_were = s_dialogs.size();
		const bool typing = !s_dialogs.empty() && s_dialogs.front().kind == Dialog::Keyboard;
		if (be != nullptr && typing != s_capturing)
			be->captureKeyboard(typing);
		s_capturing = typing;
	}

	// The card manager, under its own lists and questions.
	if (cardManagerOpen())
		cardManagerDraw(dl);

	if (s_dialogs.empty())
	{
		drawToasts(dl);
		return;
	}

	Dialog& d = s_dialogs.front();
	d.open = approach(d.open, 1, 16);
	const float t = d.open;
	dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, static_cast<int>(170 * t)));

	if (d.kind == Dialog::Code)
	{
		// The code on white with its quiet zone (four modules), the text beside it.
		const float w = 1320, h = 640;
		const float x = (W - w) / 2, y = (H - h) / 2 + (1 - t) * 30;
		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
		rect(dl, x, y, w, h, alpha(col::panel, t), 24);
		textIn(dl, bold(), 34, x + 48, y + 40, alpha(col::text, t), d.title, w - 96, true);
		const float side = 460, qx = x + 48, qy = y + 120;
		rect(dl, qx, qy, side, side, col::rgba(255, 255, 255, static_cast<int>(255 * t)), 8);
		if (d.codeSize > 0)
		{
			const float module = side / static_cast<float>(d.codeSize + 8);
			for (int my = 0; my < d.codeSize; my++)
			{
				for (int mx = 0; mx < d.codeSize; mx++)
				{
					if (d.code[static_cast<size_t>(my) * d.codeSize + mx] != 0)
						dl->AddRectFilled(V(qx + (mx + 4) * module, qy + (my + 4) * module), V(qx + (mx + 5) * module, qy + (my + 5) * module),
							col::rgba(0, 0, 0, static_cast<int>(255 * t)));
				}
			}
		}
		text(dl, regular(), 26, qx + side + 48, qy, alpha(col::text, t), d.body.c_str(), w - side - 48 * 3);
		float hx = x + 48;
		const float hy = y + h + 34;
		hx += glyph(dl, Glyph::Circle, hx, hy, 30) + 10;
		text(dl, regular(), 22, hx, hy - 13, alpha(col::text, t), "Close");
		drawToasts(dl);
		return;
	}

	if (d.kind == Dialog::Pick)
	{
		const int n = static_cast<int>(d.names.size());
		const int rows = std::min(n, PICK_ROWS);
		const float rowH = 64, w = 980;
		const float h = 120 + rows * rowH + 36;
		const float x = (W - w) / 2, y = (H - h) / 2 + (1 - t) * 30;
		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
		rect(dl, x, y, w, h, alpha(col::panel, t), 24);
		textIn(dl, bold(), 34, x + 48, y + 40, alpha(col::text, t), d.title, w - 96, true);
		const float listTop = y + 108;
		const float target = std::clamp(static_cast<float>(d.focus) - (rows - 1) / 2.f, 0.f, static_cast<float>(std::max(0, n - rows)));
		d.scroll = approach(d.scroll, target, 18);
		dl->PushClipRect(V(x, listTop), V(x + w, listTop + rows * rowH), true);
		for (int i = 0; i < n; i++)
		{
			const float ry = listTop + (i - d.scroll) * rowH;
			if (ry > listTop + rows * rowH || ry + rowH < listTop)
				continue;
			const bool on = (i == d.focus);
			if (on)
				rect(dl, x + 28, ry + 3, w - 56, rowH - 6, alpha(col::accent, t), 12);
			textIn(dl, on ? bold() : regular(), 26, x + 56, ry + 17, alpha(on ? col::text : col::dim, t), d.names[i], w - 190, on);
			if (i == d.current)
			{
				const ImVec2 cs = textSize(regular(), 24, ICON_FA_CHECK);
				text(dl, regular(), 24, x + w - 64 - cs.x, ry + 19, alpha(on ? col::text : col::accent, t), ICON_FA_CHECK);
			}
		}
		dl->PopClipRect();
		if (n > rows)
		{
			// Where the list is, when it is longer than what shows.
			const float track = rows * rowH - 16, thumb = std::max(40.f, track * rows / n);
			const float at = (n > rows) ? d.scroll / (n - rows) : 0.f;
			rect(dl, x + w - 18, listTop + 8, 6, track, col::rgba(255, 255, 255, static_cast<int>(26 * t)), 3);
			rect(dl, x + w - 18, listTop + 8 + (track - thumb) * at, 6, thumb, alpha(col::dim, t), 3);
		}
		float hx = x + 48;
		const float hy = y + h + 34;
		const auto hint = [&](Glyph g, const char* label) {
			hx += glyph(dl, g, hx, hy, 30) + 10;
			hx += text(dl, regular(), 22, hx, hy - 13, alpha(col::text, t), label).x + 30;
		};
		hint(Glyph::Cross, "Choose");
		hint(Glyph::Circle, "Back");
		drawToasts(dl);
		return;
	}

	if (d.kind == Dialog::Text)
	{
		const float w = TEXT_W, h = TEXT_H;
		const float x = (W - w) / 2, y = (H - h) / 2 - 20 + (1 - t) * 30;
		// The lines as they fit the sheet, made once (and again when the screen's size changes).
		if (d.shownFor != S)
		{
			d.shownFor = S;
			d.shown.clear();
			ImFont* const font = regular();
			const float room = (w - 110) * S;
			for (const std::string& line : d.names)
			{
				const char* at = line.c_str();
				const char* const end = at + line.size();
				if (at == end)
					d.shown.emplace_back();
				bool first = true;
				while (at < end)
				{
					const char* cut = font->CalcWordWrapPosition(TEXT_SIZE * S, at, end, first ? room : room - 60 * S);
					if (cut <= at)
					{
						// (Not a letter fits, or nothing was cut: one character at least, a whole one.)
						cut = at + 1;
						while (cut < end && (static_cast<unsigned char>(*cut) & 0xc0) == 0x80)
							cut++;
					}
					d.shown.push_back(first ? std::string(at, cut) : "\t" + std::string(at, cut));
					at = cut;
					while (at < end && *at == ' ')
						at++;
					first = false;
				}
			}
			d.top = -1;
		}
		const float last = std::max(0.f, static_cast<float>(d.shown.size()) - TEXT_ROWS);
		if (d.top < 0)
			d.top = last;
		d.top = std::clamp(d.top, 0.f, last);
		d.scroll = (d.open < 0.5f) ? d.top : approach(d.scroll, d.top, 22);

		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
		rect(dl, x, y, w, h, alpha(col::panel, t), 24);
		textIn(dl, bold(), 34, x + 48, y + 36, alpha(col::text, t), d.title, w - 420, true);
		{
			char where[64];
			if (d.shown.empty())
				std::snprintf(where, sizeof(where), "Nothing in it");
			else
				std::snprintf(where, sizeof(where), "Lines %d to %d of %d", static_cast<int>(d.top) + 1,
					std::min(static_cast<int>(d.shown.size()), static_cast<int>(d.top) + TEXT_ROWS), static_cast<int>(d.shown.size()));
			const ImVec2 ws = textSize(regular(), 22, where);
			text(dl, regular(), 22, x + w - 48 - ws.x, y + 46, alpha(col::faint, t), where);
		}
		const float listTop = y + 104, listH = TEXT_ROWS * TEXT_LINE;
		rect(dl, x + 28, listTop - 10, w - 56, listH + 20, alpha(col::sheet, t), 14);
		dl->PushClipRect(V(x + 28, listTop), V(x + w - 28, listTop + listH), true);
		const int from = std::max(0, static_cast<int>(d.scroll) - 1);
		const int to = std::min(static_cast<int>(d.shown.size()), from + TEXT_ROWS + 3);
		for (int i = from; i < to; i++)
		{
			const std::string& line = d.shown[i];
			const bool more = !line.empty() && line[0] == '\t';
			const float ly = listTop + (i - d.scroll) * TEXT_LINE + 2;
			// What went wrong stands out: an error, a warning, the report of a crash.
			ImU32 c = col::dim;
			if (line.find("error") != std::string::npos || line.find("FATAL") != std::string::npos || line.find("====") != std::string::npos ||
				line.find("FAILED") != std::string::npos)
				c = col::warm;
			else if (line.find("warning") != std::string::npos)
				c = mix(col::dim, col::warm, 0.5f);
			text(dl, regular(), TEXT_SIZE, x + 48 + (more ? 60 : 0), ly, alpha(c, t), line.c_str() + (more ? 1 : 0));
		}
		dl->PopClipRect();
		if (d.shown.size() > static_cast<size_t>(TEXT_ROWS))
		{
			const float track = listH - 16, thumb = std::max(40.f, track * TEXT_ROWS / d.shown.size());
			const float at = (last > 0) ? d.scroll / last : 0.f;
			rect(dl, x + w - 22, listTop + 8, 6, track, col::rgba(255, 255, 255, static_cast<int>(26 * t)), 3);
			rect(dl, x + w - 22, listTop + 8 + (track - thumb) * std::clamp(at, 0.f, 1.f), 6, thumb, alpha(col::dim, t), 3);
		}
		float hx = x + 48;
		const float hy = y + h + 34;
		const auto hint = [&](Glyph g, const char* label) {
			hx += glyph(dl, g, hx, hy, 30) + 10;
			hx += text(dl, regular(), 22, hx, hy - 13, alpha(col::text, t), label).x + 30;
		};
		hint(Glyph::DPadUD, "Line");
		hint(Glyph::L1R1, "Page");
		hint(Glyph::L2R2, "Start / End");
		hint(Glyph::Circle, "Close");
		drawToasts(dl);
		return;
	}

	if (d.kind == Dialog::Keyboard)
	{
		const float keyW = 104, keyH = 84, gap = 10;
		const float gridW = KEY_COLS * keyW + (KEY_COLS - 1) * gap;
		const float w = gridW + 112, h = 250 + (KEY_ROWS + 1) * (keyH + gap) + 30;
		const float x = (W - w) / 2, y = (H - h) / 2 - 20 + (1 - t) * 30;
		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
		rect(dl, x, y, w, h, alpha(col::panel, t), 24);
		textIn(dl, bold(), 34, x + 56, y + 40, alpha(col::text, t), d.title, w - 112, true);

		// What was typed, in a field of its own; a password as dots unless it is shown.
		const float fx = x + 56, fy = y + 104, fw = w - 112, fh = 84;
		rect(dl, fx, fy, fw, fh, alpha(col::sheet, t), 16);
		outline(dl, fx, fy, fw, fh, alpha(col::accent, 0.7f * t), 16, 2);
		std::string shown = d.value;
		if (d.secret && !d.reveal)
		{
			shown.clear();
			for (int i = characters(d.value); i > 0; i--)
				shown += "\xe2\x80\xa2"; // a dot for each character
		}
		// (The end of a text too long for the field is what shows: that is where one types.)
		while (textSize(bold(), 36, shown.c_str()).x > fw - 70 && !shown.empty())
		{
			size_t cut = 1;
			while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xc0) == 0x80)
				cut++;
			shown.erase(0, cut);
		}
		const ImVec2 vs = textSize(bold(), 36, shown.c_str());
		text(dl, bold(), 36, fx + 24, fy + (fh - 36) / 2, alpha(col::text, t), shown.c_str());
		if (std::fmod(timeNow, 1.0) < 0.55)
			rect(dl, fx + 24 + vs.x + 4, fy + 18, 3, fh - 36, alpha(col::accent, t), 1);
		{
			char count[32];
			std::snprintf(count, sizeof(count), "%d of %d", characters(d.value), d.most);
			const ImVec2 cs = textSize(regular(), 20, count);
			text(dl, regular(), 20, fx + fw - cs.x, fy + fh + 10, alpha(col::faint, t), count);
			if (d.value.empty())
				text(dl, regular(), 26, fx + 24, fy + (fh - 26) / 2, alpha(col::faint, t), d.secret ? "The password" : "Nothing yet");
		}

		const float gx = x + 56, gy = y + 236;
		for (int row = 0; row < KEY_ROWS; row++)
		{
			for (int column = 0; column < KEY_COLS; column++)
			{
				const float kx = gx + column * (keyW + gap), ky = gy + row * (keyH + gap);
				const bool on = (d.ky == row && d.kx == column);
				if (on)
					glow(dl, kx, ky, keyW, keyH, 14, alpha(col::accent, 0.6f * t), 12);
				rect(dl, kx, ky, keyW, keyH, alpha(on ? col::accent : col::panelHi, t), 14);
				const char label[2] = {s_keys[d.page][row][column], 0};
				const ImVec2 ls = textSize(bold(), 36, label);
				text(dl, bold(), 36, kx + (keyW - ls.x) / 2, ky + (keyH - 36) / 2, alpha(on ? col::text : col::dim, t), label);
			}
		}
		for (int key = 0; key < WideKeys; key++)
		{
			const float kx = gx + s_wideFrom[key] * (keyW + gap), ky = gy + KEY_ROWS * (keyH + gap);
			const float kw = 2 * keyW + gap;
			const bool on = (d.ky == KEY_ROWS && d.kx == key);
			const bool lit = (key == KeyShift && d.page == 1) || (key == KeySigns && d.page == 2);
			if (on)
				glow(dl, kx, ky, kw, keyH, 14, alpha(col::accent, 0.6f * t), 12);
			rect(dl, kx, ky, kw, keyH, alpha(on ? col::accent : (key == KeyDone) ? col::accentDark : col::panelHi, t), 14);
			if (lit && !on)
				outline(dl, kx, ky, kw, keyH, alpha(col::accent, t), 14, 3);
			const ImVec2 ls = textSize(bold(), 26, s_wide[key]);
			text(dl, bold(), 26, kx + (kw - ls.x) / 2, ky + (keyH - 26) / 2, alpha(on || lit ? col::text : col::dim, t), s_wide[key]);
		}

		float hx = x + 56;
		const float hy = y + h + 34;
		const auto hint = [&](Glyph g, const char* label) {
			hx += glyph(dl, g, hx, hy, 30) + 10;
			hx += text(dl, regular(), 22, hx, hy - 13, alpha(col::text, t), label).x + 26;
		};
		hint(Glyph::Cross, "Type");
		hint(Glyph::Square, "Delete");
		hint(Glyph::Triangle, "Space");
		hint(Glyph::L1, "Capitals");
		hint(Glyph::R1, "Signs");
		if (d.secret)
			hint(Glyph::L2, d.reveal ? "Hide" : "Show");
		hint(Glyph::Options, "Done");
		hint(Glyph::Circle, "Cancel");
		drawToasts(dl);
		return;
	}

	if (d.kind == Dialog::Address)
	{
		const float w = 1100, h = 392;
		const float x = (W - w) / 2, y = (H - h) / 2 + (1 - t) * 30;
		glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
		rect(dl, x, y, w, h, alpha(col::panel, t), 24);
		textIn(dl, bold(), 34, x + 48, y + 40, alpha(col::text, t), d.title, w - 96, true);
		const char* const was = d.set ? "" : "Not set until now";
		const ImVec2 ws = textSize(regular(), 22, was);
		text(dl, regular(), 22, x + w - 48 - ws.x, y + 50, alpha(col::faint, t), was);
		// The four numbers, a dot between them; the one the directions change is lit.
		const float boxW = 200, boxH = 132, dot = 44;
		const float left = x + (w - 4 * boxW - 3 * dot) / 2, top = y + 142;
		for (int i = 0; i < 4; i++)
		{
			const float bx = left + i * (boxW + dot);
			const bool on = (i == d.focus);
			if (on)
				glow(dl, bx, top, boxW, boxH, 18, alpha(col::accent, 0.6f * t), 14);
			rect(dl, bx, top, boxW, boxH, alpha(on ? col::accent : col::panelHi, t), 18);
			const std::string number = std::to_string(d.numbers[i]);
			const ImVec2 ns = textSize(bold(), 72, number.c_str());
			text(dl, bold(), 72, bx + (boxW - ns.x) / 2, top + (boxH - ns.y) / 2, alpha(on ? col::text : col::dim, t), number.c_str());
			if (on)
			{
				const float mx = bx + boxW / 2;
				dl->AddTriangleFilled(V(mx, top - 26), V(mx - 14, top - 10), V(mx + 14, top - 10), alpha(col::text, t));
				dl->AddTriangleFilled(V(mx, top + boxH + 26), V(mx - 14, top + boxH + 10), V(mx + 14, top + boxH + 10), alpha(col::text, t));
			}
			if (i < 3)
				dl->AddCircleFilled(V(bx + boxW + dot / 2, top + boxH - 30), 7 * S, alpha(col::dim, t), 16);
		}
		float hx = x + 48;
		const float hy = y + h + 34;
		const auto hint = [&](Glyph g, const char* label) {
			hx += glyph(dl, g, hx, hy, 30) + 10;
			hx += text(dl, regular(), 22, hx, hy - 13, alpha(col::text, t), label).x + 30;
		};
		hint(Glyph::DPadLR, "Number");
		hint(Glyph::DPadUD, "By 1");
		hint(Glyph::L1R1, "By 10");
		hint(Glyph::Cross, "Accept");
		hint(Glyph::Square, "Not set");
		hint(Glyph::Circle, "Cancel");
		drawToasts(dl);
		return;
	}

	const float w = 1040;
	const ImVec2 bodySize = textSize(regular(), 26, d.body.c_str(), w - 112);
	const float bodyH = std::min(bodySize.y, 520.f);
	const float h = 124 + bodyH + 132;
	const float x = (W - w) / 2, y = (H - h) / 2 + (1 - t) * 30;
	glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, static_cast<int>(255 * t)), 30);
	rect(dl, x, y, w, h, alpha(col::panel, t), 24);
	textIn(dl, bold(), 36, x + 56, y + 44, alpha(col::text, t), d.title, w - 112, true);
	dl->PushClipRect(V(x + 56, y + 112), V(x + w - 56, y + 112 + bodyH), true);
	text(dl, regular(), 26, x + 56, y + 112, alpha(col::dim, t), d.body.c_str(), w - 112);
	dl->PopClipRect();
	const float by = y + h - 96;
	if (d.kind == Dialog::Message)
	{
		dialogButton(dl, x + 56, by, "OK", true, t);
	}
	else
	{
		float bx = x + 56;
		bx += dialogButton(dl, bx, by, d.yes.c_str(), d.focus == 0, t) + 20;
		dialogButton(dl, bx, by, d.no.c_str(), d.focus == 1, t);
	}
	drawToasts(dl);
}

// ---------------------------------------------------------------- the rest

std::string formatSize(uint64_t bytes)
{
	char out[32];
	if (bytes >= (1ull << 30))
		std::snprintf(out, sizeof(out), "%.2f GB", static_cast<double>(bytes) / (1ull << 30));
	else if (bytes >= (1ull << 20))
		std::snprintf(out, sizeof(out), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
	else
		std::snprintf(out, sizeof(out), "%.0f KB", static_cast<double>(bytes) / 1024.0);
	return out;
}

std::string formatPlayed(time_t seconds)
{
	if (seconds <= 0)
		return "";
	char out[48];
	const long long hours = seconds / 3600, minutes = (seconds % 3600) / 60;
	if (hours > 0)
		std::snprintf(out, sizeof(out), "%lld h %lld min", hours, minutes);
	else if (minutes > 0)
		std::snprintf(out, sizeof(out), "%lld min", minutes);
	else
		std::snprintf(out, sizeof(out), "under a minute");
	return out;
}

std::string formatWhen(time_t when)
{
	if (when <= 0)
		return "";
	char out[48] = "";
	struct tm local;
	if (localtime_r(&when, &local) != nullptr)
		strftime(out, sizeof(out), "%Y-%m-%d  %H:%M", &local);
	return out;
}

const char* kindText(const GameInfo& game)
{
	switch (game.kind)
	{
		case GameInfo::PS1:
			return "PLAYSTATION";
		case GameInfo::Program:
			return "PROGRAM";
		case GameInfo::PS2:
		default:
			return "PLAYSTATION 2";
	}
}

// What is drawn with no screen on (a game runs): a dialog or a toast, if there is one.
void drawOverGame()
{
	measure();
	careOverGame();
	const bool dialogs = dialogOpen();
	if (!dialogs && s_toasts.empty())
		return;
	if (dialogs)
	{
		beginScreen("##bp-dialog", false);
		endScreen();
		return;
	}
	measure();
	applySkin();
	drawToasts(ImGui::GetForegroundDrawList());
}

void holdInput()
{
	// (Asked of the controller now: while a game runs with no screen over it nobody reads it,
	// and what was held when the last screen went is long let go.)
	if (be != nullptr)
		s_down = be->pad().held;
	s_holdMask |= s_down;
}

void tickClock()
{
	frameNow++;
	// (A frame that took long, the device being made anew, is not a jump in every animation.)
	timeNow += std::clamp(static_cast<double>(ImGui::GetIO().DeltaTime), 0.0, 0.1);
}

} // namespace bp
