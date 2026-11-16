// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - host test of the performance overlay's decisions (frontend/ps5_osd_logic.h): the levels
// (fine, close to the limit, over it) at and around their thresholds, the frame's budget, the
// sizes' presets, what is drawn for a size and its switches, what the settings keep after a switch
// is changed, a settings file's first size, and frame generation's two rates.
#include "ps5_osd_logic.h"

#include <cstdio>
#include <cstring>

using namespace PS5OsdLogic;

static int checks, failures;
#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

// Compile-time too: the thresholds are what the header's table says.
static_assert(LoadLevel(79.9f) == Level::Fine && LoadLevel(80.0f) == Level::Near && LoadLevel(95.0f) == Level::Over);
static_assert(Preset(Compact).fps && Preset(Compact).gpu && Preset(Compact).cpu && !Preset(Compact).frame_times);

static Switches Only(bool Switches::*field)
{
	Switches s;
	s.*field = true;
	return s;
}

static Switches minimal_only()
{
	return Only(&Switches::fps);
}

static void Levels()
{
	// Loads: below 80 fine, 80 up to 95 close, 95 and over (and nonsense above 100) over.
	CHECK(LoadLevel(0.0f) == Level::Fine);
	CHECK(LoadLevel(79.99f) == Level::Fine);
	CHECK(LoadLevel(80.0f) == Level::Near);
	CHECK(LoadLevel(94.99f) == Level::Near);
	CHECK(LoadLevel(95.0f) == Level::Over);
	CHECK(LoadLevel(140.0f) == Level::Over);
	CHECK(LoadLevel(-5.0f) == Level::Fine);

	// Speed against the target: 98 % of it and above fine; 90 up to 98 close; under 90 over.
	CHECK(SpeedLevel(100.0f, 1.0f) == Level::Fine);
	CHECK(SpeedLevel(98.0f, 1.0f) == Level::Fine);
	CHECK(SpeedLevel(97.9f, 1.0f) == Level::Near);
	CHECK(SpeedLevel(90.0f, 1.0f) == Level::Near);
	CHECK(SpeedLevel(89.9f, 1.0f) == Level::Over);
	CHECK(SpeedLevel(50.0f, 1.0f) == Level::Over);
	// Faster than asked is fine; at twice the speed (turbo) 190 % is close, 150 % over.
	CHECK(SpeedLevel(130.0f, 1.0f) == Level::Fine);
	CHECK(SpeedLevel(199.0f, 2.0f) == Level::Fine);
	CHECK(SpeedLevel(190.0f, 2.0f) == Level::Near);
	CHECK(SpeedLevel(150.0f, 2.0f) == Level::Over);
	// Unlimited (target 0) is never slow.
	CHECK(SpeedLevel(12.0f, 0.0f) == Level::Fine);
	// Host refresh sync (a target a hair over 1).
	CHECK(SpeedLevel(100.0f, 1.001f) == Level::Fine);

	// Frame times against the budget: up to 110 % fine, then close, over 150 % a spike.
	const float b = FrameBudgetMs(59.94f, 1.0f);
	CHECK(b > 16.68f && b < 16.69f);
	CHECK(FrameBudgetMs(50.0f, 1.0f) == 20.0f);
	CHECK(FrameBudgetMs(60.0f, 2.0f) > 8.33f && FrameBudgetMs(60.0f, 2.0f) < 8.34f);
	CHECK(FrameBudgetMs(0.0f, 0.0f) > 16.66f && FrameBudgetMs(0.0f, 0.0f) < 16.67f); // unknown: 60 Hz, normal speed
	CHECK(FrameLevel(16.7f, b) == Level::Fine);
	CHECK(FrameLevel(b * 1.10f, b) == Level::Fine);
	CHECK(FrameLevel(b * 1.11f, b) == Level::Near);
	CHECK(FrameLevel(b * 1.50f, b) == Level::Near);
	CHECK(FrameLevel(b * 1.51f, b) == Level::Over);
	CHECK(FrameLevel(33.4f, b) == Level::Over); // a frame shown twice
	CHECK(FrameLevel(100.0f, 0.0f) == Level::Fine); // no budget known: nothing marked

	CHECK(Worse(Level::Fine, Level::Near) == Level::Near);
	CHECK(Worse(Level::Over, Level::Near) == Level::Over);
	CHECK(Worse(Level::Fine, Level::Fine) == Level::Fine);
}

static void Presets()
{
	// Every size switches the FPS on; each larger one adds to the smaller; none sets the extras.
	for (int s = Minimal; s <= Full; s++)
	{
		const Switches p = Preset(s);
		CHECK(p.fps);
		CHECK(!p.vps && !p.gs_stats && !p.gpu_stats && !p.version);
		if (s > Minimal)
		{
			const Switches q = Preset(s - 1);
			for (const SwitchKey& k : SWITCH_KEYS)
				CHECK(!(q.*k.field) || p.*k.field);
			CHECK(!(p == q));
		}
	}
	Switches minimal;
	minimal.fps = true;
	CHECK(Preset(Minimal) == minimal);
	Switches compact = minimal;
	compact.gpu = compact.cpu = true;
	CHECK(Preset(Compact) == compact);
	Switches full = compact;
	full.speed = full.frame_times = full.resolution = full.hardware = true;
	CHECK(Preset(Full) == full);
	CHECK(!Preset(Custom).Any());
	CHECK(!Preset(7).Any() && !Preset(-1).Any());
	CHECK(DEFAULT_SIZE == Compact);

	// The keys: every field once, all OsdShow* of EmuCore/GS, none of PCSX2's other elements.
	CHECK(SWITCH_COUNT == 11);
	CHECK(std::strcmp(SWITCH_SECTION, "EmuCore/GS") == 0);
	for (size_t i = 0; i < SWITCH_COUNT; i++)
	{
		CHECK(std::strncmp(SWITCH_KEYS[i].key, "OsdShow", 7) == 0);
		CHECK(IsSwitchKey(SWITCH_KEYS[i].key));
		const Switches one = Only(SWITCH_KEYS[i].field);
		for (size_t j = 0; j < SWITCH_COUNT; j++)
			CHECK((i == j) == (one.*SWITCH_KEYS[j].field));
	}
	CHECK(!IsSwitchKey("OsdShowIndicators"));
	CHECK(!IsSwitchKey("OsdShowInputs"));
	CHECK(!IsSwitchKey("OsdShowSettings"));
	CHECK(!IsSwitchKey("OsdShowTextureReplacements"));
	CHECK(!IsSwitchKey("OsdShowFPSx"));
	CHECK(!IsSwitchKey("OsdShowFP"));
	CHECK(!IsSwitchKey(""));
}

static void Sizes()
{
	// Drawn as the size while its switches are as it set them; otherwise Custom's card.
	for (int s = Minimal; s <= Full; s++)
		CHECK(EffectiveSize(s, Preset(s)) == s);
	CHECK(EffectiveSize(Compact, Preset(Full)) == Custom);
	Switches more = Preset(Compact);
	more.vps = true;
	CHECK(EffectiveSize(Compact, more) == Custom);
	CHECK(EffectiveSize(Custom, Preset(Compact)) == Custom); // Custom chosen stays Custom
	CHECK(EffectiveSize(9, Preset(Compact)) == Custom);

	bool apply = true;
	// A switch changed in a preset: Custom.
	Switches s = Preset(Compact);
	s.frame_times = true;
	CHECK(SizeAfterSwitch(Compact, s, false, apply) == Custom && !apply);
	s = Preset(Full);
	s.hardware = false;
	CHECK(SizeAfterSwitch(Full, s, false, apply) == Custom && !apply);
	// Custom stays Custom whatever is switched.
	CHECK(SizeAfterSwitch(Custom, Preset(Compact), false, apply) == Custom && !apply);
	CHECK(SizeAfterSwitch(Custom, minimal_only(), true, apply) == Custom && !apply);
	// Everything off: the size is kept for next time.
	CHECK(SizeAfterSwitch(Full, Switches{}, false, apply) == Full && !apply);
	// The FPS counter switched on with nothing else shown: the size's figures (a new user: Compact).
	CHECK(SizeAfterSwitch(Compact, minimal_only(), true, apply) == Compact && apply);
	CHECK(SizeAfterSwitch(Full, minimal_only(), true, apply) == Full && apply);
	CHECK(SizeAfterSwitch(Minimal, minimal_only(), true, apply) == Minimal && !apply); // already as Minimal sets it
	// ...but only for the FPS switch turned on: another switch leaving the FPS alone makes it Custom.
	CHECK(SizeAfterSwitch(Compact, minimal_only(), false, apply) == Custom && !apply);
	// The FPS switched on with another figure already on: Custom (nothing overwritten).
	Switches fps_gpu;
	fps_gpu.fps = fps_gpu.gpu = true;
	CHECK(SizeAfterSwitch(Compact, fps_gpu, true, apply) == Custom && !apply);
	// A switch put back as the preset has it: the preset again.
	CHECK(SizeAfterSwitch(Compact, Preset(Compact), false, apply) == Compact && !apply);

	// A settings file's first size: nothing shown, the default; a preset's switches, that preset;
	// anything else, Custom.
	CHECK(InitialSize(Switches{}) == Compact);
	for (int z = Minimal; z <= Full; z++)
		CHECK(InitialSize(Preset(z)) == z);
	Switches old;
	old.fps = old.frame_times = true;
	CHECK(InitialSize(old) == Custom);
	CHECK(InitialSize(Only(&Switches::version)) == Custom);
}

static void Rates()
{
	CHECK(!ShowsTwoRates(60.0f, 0.0f));   // frame generation off
	CHECK(!ShowsTwoRates(60.0f, 0.4f));
	CHECK(ShowsTwoRates(30.0f, 60.0f));   // "30 → 60"
	CHECK(ShowsTwoRates(29.97f, 59.94f));
	CHECK(!ShowsTwoRates(60.0f, 59.5f));  // the same rate: one figure
	CHECK(ShowsTwoRates(0.0f, 60.0f));    // the game stalled while the screen is given pictures
}

int main()
{
	Levels();
	Presets();
	Sizes();
	Rates();
	std::printf("osd logic: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
