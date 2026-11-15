// Crux2 - the interface: the start-up animations.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Four of them, and a start plays another one than the start before it (Settings > Interface can
// name one). Each builds the name out of its parts in its own way and takes it to the top bar in
// its own way, and all four are timed to the start-up sound, which begins at 0.11 s, has its hit
// at 1.12 s and pulses every half second from there:
//
//   Strike   the thin bars slide in along their diagonal, the broad one strikes across them on
//            the hit; the letters fly in from the left; the name glides to the top bar as one.
//   Unfold   the name is drawn where it stands: the bars grow out of their middles, the letters
//            rise from the line; the letters fold into the mark, the mark flies to the top bar in
//            a curve, turning once, and the letters unfold from it there.
//   Spin     the bars turn like a propeller and lock into the X on the hit; the letters drop in
//            and bounce; the parts go to the top bar one after another, each with a hop.
//   Slash    two lines are drawn across the whole screen and snap into the thin bars as an orange
//            streak cuts from corner to corner and draws in to the broad one; the letters pop up;
//            a diagonal wipe crosses the screen, and the library is behind it with the name
//            already in its place.
//
// Each was a mock-up first (a page drawn frame by frame, approved as a video); the numbers here
// are that page's. The screen is 1920 by 1080 units, as everywhere in the interface.
//
// On the console, the startup video (../ps5_intro.h, ps5/art/intro) plays in the animation's place
// when it can: Settings > Interface > Startup video ([PS5] StartupVideo, on by default), "All"
// motion, and its file in the title folder. Its frames go to the screen through a texture of
// ImGui's own (the renderer uploads it), its sound through the interface's sound in the start-up
// sound's place; any button skips it, and its end hands over to the library as the animations do,
// the name already in the top bar. Without it, the start is as it was.

#include "bp_internal.h"

#include <cmath>

#ifdef __PROSPERO__
#include "../ps5_intro.h"
#define BP_STARTUP_VIDEO 1
#endif

namespace bp
{

namespace
{
constexpr float BIG = 220;               // the type the name is set in while it stands in the middle
constexpr float K = BAR_TYPE / BIG;      // and how much smaller the top bar's is
constexpr float HIT = 1.12f;             // the sound's hit
constexpr float BEATS[] = {1.12f, 1.6f, 2.1f, 2.6f, 3.1f, 3.6f, 4.1f, 4.6f, 5.1f};

float span(float t, float a, float b)
{
	return std::clamp((t - a) / (b - a), 0.f, 1.f);
}
float lerp(float a, float b, float u)
{
	return a + (b - a) * u;
}
float inQuad(float u)
{
	return u * u;
}
float outCubic(float u)
{
	const float q = 1 - u;
	return 1 - q * q * q;
}
float inOutCubic(float u)
{
	return u < 0.5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.f) / 2;
}
float outBack(float u)
{
	const float c = 1.9f, q = u - 1;
	return 1 + (c + 1) * q * q * q + c * q * q;
}
float outBounce(float u)
{
	const float n = 7.5625f, d = 2.75f;
	if (u < 1 / d)
		return n * u * u;
	if (u < 2 / d)
	{
		u -= 1.5f / d;
		return n * u * u + 0.75f;
	}
	if (u < 2.5f / d)
	{
		u -= 2.25f / d;
		return n * u * u + 0.9375f;
	}
	u -= 2.625f / d;
	return n * u * u + 0.984375f;
}

// The name while it stands in the middle of the screen, and where its parts end in the top bar.
struct Scene
{
	WordLayout word;
	float left, cy;
	float zoom = 1; // Unfold looks at it from closer at first

	Scene()
	{
		word = wordLayout(BIG);
		left = (W - word.width) / 2;
		cy = H / 2 - 10;
	}
	ImVec2 base(int part) const { return ImVec2(left + word.centre[part], cy); }
	ImVec2 final(int part) const { return ImVec2(BAR_NAME_X + K * word.centre[part], BAR_NAME_Y); }
	// (The closer look: everything is that much larger, and that much further from the middle.)
	ImVec2 seen(ImVec2 p) const { return ImVec2(W / 2 + (p.x - W / 2) * zoom, H / 2 + (p.y - H / 2) * zoom); }
};

// The splash's own ground: the skin's dark, `a` of it.
void ground(ImDrawList* dl, float a)
{
	if (a > 0.004f)
		rect(dl, 0, 0, W, H, alpha(col::bgTop, std::min(a, 1.f)));
}

// The light behind the name: it comes on the hit, breathes with the sound's pulses, and goes at `out`.
void backLight(ImDrawList* dl, float t, float out)
{
	float pulse = 0;
	for (int i = 0; i < static_cast<int>(sizeof(BEATS) / sizeof(BEATS[0])); i++)
	{
		if (t >= BEATS[i])
			pulse += 0.55f * std::pow(0.86f, static_cast<float>(i)) * std::exp(-(t - BEATS[i]) / 0.17f);
	}
	const float a = std::min(1.f, span(t, HIT, HIT + 0.1f) * (0.62f + pulse)) * (1 - span(t, out, out + 0.7f));
	if (a > 0.004f)
		softDisc(dl, W / 2, H / 2, 760, alpha(col::brandOrange, 0.16f * a), 64);
}

// The ring that goes out from where the bars met.
void ring(ImDrawList* dl, float t, ImVec2 at, float zoom = 1)
{
	const float u = span(t, HIT, HIT + 0.73f);
	if (u > 0 && u < 1)
		dl->AddCircle(V(at.x, at.y), 200 * lerp(0.5f, 2.8f, outCubic(u)) * zoom * S, alpha(col::brandOrange, 0.6f * (1 - u)), 96, 4 * S);
}

// The mark large in the middle of the screen, then into its place as the name's X (Strike, Spin, Slash).
struct BigMark
{
	ImVec2 at;
	float size;
};
BigMark bigThenSettled(const Scene& scene, float t)
{
	const float settle = inOutCubic(span(t, 1.5f, 2.02f));
	const float hit = std::sin(IM_PI * span(t, HIT, HIT + 0.32f));
	const ImVec2 home = scene.base(WordMark);
	return {ImVec2(lerp(W / 2, home.x, settle), lerp(H / 2, home.y, settle)), scene.word.mark * lerp(1.9f * (1 + 0.07f * hit), 1, settle)};
}

// What is over the library while it comes in: the ground, less and less of it.
float groundOverLibrary(ImDrawList* dl, float t, float from, float to)
{
	const float in = span(t, from, to);
	ground(dl, 1 - outCubic(in));
	return in;
}

// ---------------------------------------------------------------- Strike
bool strike(ImDrawList* dl, float t)
{
	const Scene scene;
	groundOverLibrary(dl, t, 4.4f, 5.15f);
	backLight(dl, t, 4.3f);
	ring(dl, t, ImVec2(W / 2, H / 2));

	const float go = inOutCubic(span(t, 4.1f, 4.95f));
	if (go >= 1)
		return true;
	// The whole name as one, on its way to the top bar.
	const auto place = [&](int part) {
		const ImVec2 b = scene.base(part), f = scene.final(part);
		return ImVec2(lerp(b.x, f.x, go), lerp(b.y, f.y, go));
	};
	const float shrink = lerp(1, K, go);

	MarkPose pose;
	pose.thinSlide[0] = lerp(-260, 0, outCubic(span(t, 0.11f, 0.62f)));
	pose.thinSlide[1] = lerp(260, 0, outCubic(span(t, 0.50f, 1.00f)));
	pose.thinAlpha[0] = t >= 0.11f ? 1.f : 0.f;
	pose.thinAlpha[1] = t >= 0.50f ? 1.f : 0.f;
	pose.broadSlide = lerp(300, 0, inQuad(span(t, 0.80f, HIT)));
	pose.broadAlpha = t >= 0.80f ? 1.f : 0.f;
	const BigMark big = bigThenSettled(scene, t);
	if (go > 0)
	{
		const ImVec2 at = place(WordMark);
		cruxMark(dl, at.x, at.y, scene.word.mark * shrink, 1, &pose);
	}
	else
	{
		cruxMark(dl, big.at.x, big.at.y, big.size, 1, &pose);
	}

	// The letters fly in and are all there on the pulse at 2.1 s; the 2 lands on the next one.
	const auto letter = [&](int part, float start, float fromX, float fromY) {
		const float u = outCubic(span(t, start, start + 0.5f));
		const ImVec2 at = place(part);
		wordLetter(dl, part, at.x + lerp(fromX, 0, u), at.y + lerp(fromY, 0, u), BIG * shrink, span(t, start, start + 0.18f));
	};
	letter(WordU, 1.52f, -520, 160);
	letter(WordR, 1.56f, -620, 0);
	letter(WordC, 1.60f, -520, -160);
	{
		const float pop = 1 + 0.14f * std::sin(IM_PI * span(t, 2.6f, 2.86f));
		const ImVec2 at = place(Word2);
		wordLetter(dl, Word2, at.x + lerp(460, 0, inQuad(span(t, 2.28f, 2.6f))), at.y, BIG * shrink * pop, span(t, 2.28f, 2.38f));
	}
	return false;
}

// ---------------------------------------------------------------- Unfold
bool unfold(ImDrawList* dl, float t)
{
	Scene scene;
	scene.zoom = lerp(1.22f, 1, outCubic(span(t, 0, 3.2f)));
	groundOverLibrary(dl, t, 4.5f, 5.2f);
	backLight(dl, t, 4.0f);
	const ImVec2 home = scene.base(WordMark);
	ring(dl, t, scene.seen(home), scene.zoom);

	// The bars grow out of their middles, the broad one on the hit.
	MarkPose pose;
	pose.thinLength[0] = 84 * outCubic(span(t, 0.11f, 0.6f));
	pose.thinLength[1] = 84 * outCubic(span(t, 0.5f, 1.0f));
	pose.thinAlpha[0] = t >= 0.11f ? 1.f : 0.f;
	pose.thinAlpha[1] = t >= 0.5f ? 1.f : 0.f;
	pose.broadLength = 84 * inQuad(span(t, 1.0f, HIT)) * (1 + 0.2f * std::sin(IM_PI * span(t, HIT, HIT + 0.3f)));
	pose.broadAlpha = t >= 1.0f ? 1.f : 0.f;

	// To the top bar: the letters fold into the mark, the mark flies there in a curve, turning
	// once, and the letters unfold from it.
	const float fold = inOutCubic(span(t, 3.9f, 4.3f));
	const float fly = inOutCubic(span(t, 4.2f, 5.0f));
	const bool there = t >= 5.55f;
	if (there)
		return true;
	{
		const ImVec2 p0 = home, p2 = scene.final(WordMark), p1(p2.x + 80, p0.y + 40);
		const float q0 = (1 - fly) * (1 - fly), q1 = 2 * (1 - fly) * fly, q2 = fly * fly;
		const ImVec2 at = scene.seen(ImVec2(q0 * p0.x + q1 * p1.x + q2 * p2.x, q0 * p0.y + q1 * p1.y + q2 * p2.y));
		pose.turn = -360 * fly;
		cruxMark(dl, at.x, at.y, scene.word.mark * lerp(1, K, fly) * scene.zoom, 1, &pose);
	}
	static constexpr int order[4] = {WordC, WordR, WordU, Word2};
	static constexpr float risen[4] = {2.1f, 1.85f, 1.6f, 2.6f};
	for (int j = 0; j < 4; j++)
	{
		const int part = order[j];
		const ImVec2 b = scene.base(part);
		if (t < 4.4f)
		{
			const float up = outCubic(span(t, risen[j] - 0.32f, risen[j]));
			const float a = (t >= risen[j] - 0.32f ? 1.f : 0.f) * (1 - fold);
			if (a <= 0.004f)
				continue;
			const float type = BIG * (1 - 0.45f * fold) * scene.zoom;
			const ImVec2 at = scene.seen(ImVec2(b.x + (scene.word.centre[WordMark] - scene.word.centre[part]) * 0.85f * fold, b.y));
			// It rises from under the line its box stands on.
			const float half = type / 2;
			dl->PushClipRect(V(at.x - type, at.y - half), V(at.x + type, at.y + half), true);
			wordLetter(dl, part, at.x, at.y + (1 - up) * 1.05f * type, type, a);
			dl->PopClipRect();
		}
		else
		{
			const float open = outCubic(span(t, 4.95f + 0.05f * j, 5.35f + 0.05f * j));
			const ImVec2 f = scene.final(part);
			wordLetter(dl, part, f.x + K * (scene.word.centre[WordMark] - scene.word.centre[part]) * (1 - open), f.y, BAR_TYPE, open);
		}
	}
	return false;
}

// ---------------------------------------------------------------- Spin
bool spin(ImDrawList* dl, float t)
{
	const Scene scene;
	groundOverLibrary(dl, t, 4.45f, 5.2f);
	backLight(dl, t, 4.2f);
	ring(dl, t, ImVec2(W / 2, H / 2));
	if (t >= 4.1f + 0.09f * 4 + 0.62f)
		return true;

	// The bars turn like a propeller and stand still on the hit.
	MarkPose pose;
	pose.turn = lerp(-600, 0, outCubic(span(t, 0.11f, HIT)));
	pose.thinAlpha[0] = span(t, 0.11f, 0.22f);
	pose.thinLength[1] = 84 * outCubic(span(t, 0.5f, 0.82f));
	pose.thinAlpha[1] = t >= 0.5f ? 1.f : 0.f;
	pose.broadLength = 84 * inQuad(span(t, 0.8f, HIT));
	pose.broadAlpha = t >= 0.8f ? 1.f : 0.f;
	const BigMark big = bigThenSettled(scene, t);

	// To the top bar one after another, each with a hop.
	static constexpr float lands[WordParts] = {1.9f, 2.0f, 2.1f, 0, 2.6f};
	for (int part = 0; part < WordParts; part++)
	{
		const float go = inOutCubic(span(t, 4.1f + 0.09f * part, 4.72f + 0.09f * part));
		const ImVec2 b = scene.base(part), f = scene.final(part);
		const float hop = -110 * std::sin(IM_PI * go);
		const float shrink = lerp(1, K, go);
		if (part == WordMark)
		{
			cruxMark(dl, big.at.x + (f.x - b.x) * go, big.at.y + (f.y - b.y) * go + hop, big.size * shrink, 1, &pose);
			continue;
		}
		const float start = lands[part] - 0.42f;
		if (t < start)
			continue;
		const float fall = lerp(-640, 0, outBounce(span(t, start, lands[part])));
		wordLetter(dl, part, lerp(b.x, f.x, go), lerp(b.y, f.y, go) + fall + hop, BIG * shrink);
	}
	return false;
}

// ---------------------------------------------------------------- Slash

// The part of the screen where x + y is at least k, as a polygon (it is convex: the screen cut
// by a line).
int beyond(float k, ImVec2* out)
{
	const ImVec2 corners[4] = {ImVec2(0, 0), ImVec2(W, 0), ImVec2(W, H), ImVec2(0, H)};
	int count = 0;
	for (int i = 0; i < 4; i++)
	{
		const ImVec2 a = corners[i], b = corners[(i + 1) % 4];
		const float da = a.x + a.y - k, db = b.x + b.y - k;
		if (da >= 0)
			out[count++] = V(a.x, a.y);
		if ((da >= 0) != (db >= 0))
		{
			const float u = da / (da - db);
			out[count++] = V(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u);
		}
	}
	return count;
}

bool slash(ImDrawList* dl, float t)
{
	const Scene scene;
	// The wipe: a band along the broad bar's own direction, from the top left to the bottom
	// right. Behind it is the library; ahead of it, what was.
	const float wipe = span(t, 4.1f, 4.95f);
	const float edge = lerp(-340, 3340, inOutCubic(wipe)), band = 300;
	if (wipe >= 1)
		return true;
	if (wipe <= 0)
	{
		ground(dl, 1);
	}
	else
	{
		ImVec2 points[8];
		const int count = beyond(edge + band, points);
		if (count >= 3)
			dl->AddConvexPolyFilled(points, count, col::bgTop);
	}
	backLight(dl, t, 3.75f);
	ring(dl, t, ImVec2(W / 2, H / 2));

	// Two lines across the whole screen snap into the thin bars as the streak cuts through.
	MarkPose pose;
	const float snap = inQuad(span(t, 0.97f, HIT));
	pose.thinAngle = lerp(-90, -45, snap);
	pose.thinLength[0] = lerp(520 * outCubic(span(t, 0.11f, 0.55f)), 84, snap);
	pose.thinLength[1] = lerp(520 * outCubic(span(t, 0.5f, 0.95f)), 84, snap);
	pose.thinAlpha[0] = t >= 0.11f ? 1.f : 0.f;
	pose.thinAlpha[1] = t >= 0.5f ? 1.f : 0.f;
	pose.broadLength = t < HIT ? 640 : lerp(640, 84, outCubic(span(t, HIT, HIT + 0.34f)));
	pose.broadSlide = lerp(820, 0, inQuad(span(t, 0.98f, HIT)));
	pose.broadAlpha = t >= 0.98f ? 1.f : 0.f;
	const BigMark big = bigThenSettled(scene, t);

	static constexpr float pops[WordParts] = {1.85f, 1.97f, 2.1f, 0, 2.6f};
	const auto name = [&] {
		cruxMark(dl, big.at.x, big.at.y, big.size, 1, &pose);
		for (int part = 0; part < WordParts; part++)
		{
			if (part == WordMark)
				continue;
			const float u = span(t, pops[part] - 0.24f, pops[part]);
			if (u <= 0)
				continue;
			const ImVec2 b = scene.base(part);
			wordLetter(dl, part, b.x, b.y, BIG * outBack(u), span(t, pops[part] - 0.24f, pops[part] - 0.14f));
		}
	};
	if (wipe <= 0)
	{
		name();
	}
	else
	{
		// What was is there only ahead of the band: drawn in strips, each cut where the band's
		// far edge is at the strip's foot (the step that leaves is under the band).
		const float top = scene.cy - 180, strip = 60;
		for (int i = 0; i < 6; i++)
		{
			const float y0 = top + strip * i, y1 = y0 + strip;
			const float from = edge + band - y1;
			if (from >= W)
				continue;
			dl->PushClipRect(V(std::max(0.f, from), y0), V(W, y1), true);
			name();
			dl->PopClipRect();
		}
		const ImVec2 stripe[4] = {V(edge, 0), V(edge + band, 0), V(0, edge + band), V(0, edge)};
		dl->AddConvexPolyFilled(stripe, 4, col::brandOrange);
	}
	if (t >= HIT)
	{
		const float flash = 0.16f * std::exp(-(t - HIT) / 0.09f);
		if (flash > 0.004f)
			rect(dl, 0, 0, W, H, col::rgba(255, 176, 143, static_cast<int>(255 * flash)));
	}
	// (The top bar draws the name itself from the wipe's first frame: it is behind the band.)
	return wipe > 0;
}

// ---------------------------------------------------------------- the startup video
#ifdef BP_STARTUP_VIDEO
constexpr float VIDEO_LEAVE = 0.6f;   // its end (black) fades from over the library
constexpr float VIDEO_GIVE_UP = 5.f;  // no frame decoded by then: the library

struct StartupVideo
{
	PS5Intro::Player player;
	bool tried = false;    // once a run
	bool on = false;       // it plays, or played, this start
	bool over = false;     // its end was shown, or it was cut short: the library's turn
	float endAt = 0;       // the splash's time when it was over
	ImTextureData* tex = nullptr;
	bool retiring = false; // the texture is being given back to the renderer
	std::uint64_t serial = ~0ull;
};
StartupVideo& video()
{
	static StartupVideo* const v = new StartupVideo(); // (never destroyed: no thread is joined at exit)
	return *v;
}

// The texture goes: asked for on one frame, given back by the renderer on the next, freed here
// after that. (Called each frame while the splash still draws; one left over is a few megabytes.)
void videoRetire()
{
	StartupVideo& v = video();
	if (v.tex == nullptr)
		return;
	if (!v.retiring)
	{
		v.retiring = true;
		v.tex->WantDestroyNextFrame = true;
		return;
	}
	if (v.tex->Status == ImTextureStatus_Destroyed)
	{
		ImGui::UnregisterUserTexture(v.tex);
		IM_DELETE(v.tex);
		v.tex = nullptr;
	}
}

bool videoDrawable()
{
	const StartupVideo& v = video();
	return v.tex != nullptr && !v.retiring && v.serial != ~0ull && v.tex->Status != ImTextureStatus_WantDestroy &&
		v.tex->Status != ImTextureStatus_Destroyed;
}

// Whether it plays this start (asked once, when the splash begins).
bool videoBegin(int kind)
{
	StartupVideo& v = video();
	if (v.tried)
		return false;
	v.tried = true;
	if (kind == SplashState::Still || be == nullptr || !be->getBool(Shared, "PS5", "StartupVideo").value_or(true))
		return false;
	v.on = v.player.open(PS5Intro::Folder());
	return v.on;
}

void videoEnd(float t)
{
	StartupVideo& v = video();
	v.over = true;
	v.endAt = t;
	v.player.close();
}

// A frame of it while the splash shows; when it is over, the splash leaves for the library.
void videoShow(ImDrawList* dl, float t)
{
	StartupVideo& v = video();
	rect(dl, 0, 0, W, H, IM_COL32(0, 0, 0, 255));
	if (!v.player.started())
	{
		// Its clock starts once a few frames are decoded ahead.
		if (v.player.ready())
			v.player.start();
		else if (t > VIDEO_GIVE_UP)
			v.player.skip();
	}
	std::uint32_t w = 0, h = 0;
	std::uint64_t serial = 0;
	const std::uint8_t* pixels = v.player.started() ? v.player.frame(&w, &h, &serial) : nullptr;
	if (pixels != nullptr && serial != v.serial)
	{
		if (v.tex == nullptr)
		{
			v.tex = IM_NEW(ImTextureData)();
			v.tex->Create(ImTextureFormat_RGBA32, static_cast<int>(w), static_cast<int>(h));
			ImGui::RegisterUserTexture(v.tex);
		}
		if (v.tex->Width == static_cast<int>(w) && v.tex->Height == static_cast<int>(h) && v.tex->Status != ImTextureStatus_WantDestroy &&
			v.tex->Status != ImTextureStatus_Destroyed)
		{
			std::memcpy(v.tex->Pixels, pixels, static_cast<std::size_t>(w) * h * 4);
			if (v.tex->Status != ImTextureStatus_WantCreate)
				ImTextureDataQueueUpload(v.tex, 0, 0, static_cast<int>(w), static_cast<int>(h));
			v.serial = serial;
		}
	}
	if (videoDrawable())
		dl->AddImage(v.tex->GetTexRef(), V(0, 0), V(W, H));
	if (v.player.finished())
	{
		videoEnd(t);
		splash.leaveAt = t;
		splash.leaveFor = VIDEO_LEAVE;
	}
}

// Over the library while the splash leaves: the video's last frame (black) fades.
void videoLeave(ImDrawList* dl, float t)
{
	const StartupVideo& v = video();
	videoRetire();
	const float a = 1 - outCubic(span(t, v.endAt, v.endAt + VIDEO_LEAVE));
	if (a > 0.004f)
		rect(dl, 0, 0, W, H, IM_COL32(0, 0, 0, static_cast<int>(255 * a)));
}

// A button cut it short: the frame there fades with the ground (`a` of it), the sound with it.
void videoSkipped(ImDrawList* dl, float a)
{
	StartupVideo& v = video();
	if (!v.over)
	{
		v.player.skip();
		videoEnd(0);
	}
	if (a > 0.25f && videoDrawable())
	{
		dl->AddImage(v.tex->GetTexRef(), V(0, 0), V(W, H), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * a)));
		return;
	}
	videoRetire();
	if (a > 0.004f)
		rect(dl, 0, 0, W, H, IM_COL32(0, 0, 0, static_cast<int>(255 * a)));
}
#endif
} // namespace

bool splashDelivered(int kind, float t)
{
#ifdef BP_STARTUP_VIDEO
	if (video().on)
		return video().over; // (the video has no name to bring: the top bar has it from its end)
#endif
	switch (kind)
	{
		case SplashState::Strike:
			return t >= 4.95f;
		case SplashState::Unfold:
			return t >= 5.55f;
		case SplashState::Spin:
			return t >= 4.1f + 0.09f * 4 + 0.62f;
		case SplashState::Slash:
			return t > 4.1f;
		default:
			return true;
	}
}

void drawStartupGround(ImDrawList* dl, float a)
{
#ifdef BP_STARTUP_VIDEO
	if (video().on)
	{
		videoSkipped(dl, a);
		return;
	}
#endif
	ground(dl, a);
}

float splashLeaveAt(int kind)
{
#ifdef BP_STARTUP_VIDEO
	// (Asked once, when the splash begins: the video, when it plays, says itself when it is over.)
	if (videoBegin(kind))
		return 3600.f;
#endif
	return kind == SplashState::Unfold ? 3.9f : kind == SplashState::Still ? 1.0f : 4.1f;
}

float splashLeaveFor(int kind)
{
#ifdef BP_STARTUP_VIDEO
	if (video().on)
		return VIDEO_LEAVE;
#endif
	switch (kind)
	{
		case SplashState::Strike:
			return 1.1f;
		case SplashState::Unfold:
			return 1.65f;
		case SplashState::Spin:
			return 1.15f;
		case SplashState::Slash:
			return 0.9f;
		default:
			return 0.3f;
	}
}

bool drawStartup(ImDrawList* dl, int kind, float t)
{
#ifdef BP_STARTUP_VIDEO
	if (video().on)
	{
		if (video().over)
			videoLeave(dl, t);
		else
			videoShow(dl, t);
		return video().over;
	}
#endif
	switch (kind)
	{
		case SplashState::Strike:
			return strike(dl, t);
		case SplashState::Unfold:
			return unfold(dl, t);
		case SplashState::Spin:
			return spin(dl, t);
		case SplashState::Slash:
			return slash(dl, t);
		default:
			return true;
	}
}

void drawStartupStill(ImDrawList* dl, float a)
{
	const Scene scene;
	ground(dl, a);
	if (a > 0.004f)
		softDisc(dl, W / 2, H / 2, 760, alpha(col::brandOrange, 0.10f * a), 64);
	wordMark(dl, scene.left, scene.cy, BIG, a);
}

} // namespace bp
