// Crux2 - downsampling's and SMAA's decisions (frontend/ps5_aa.h) on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// When a downsample runs, the sizes the passes are given, the push constants they get (the shader's loop
// must reach every texel the kernel weighs, within its MAX_TAPS), the setting's values, and SMAA's size.
#include "ps5_aa.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what)
{
	if (!ok)
	{
		std::printf("  FAIL: %s\n", what);
		failures++;
	}
}

int main()
{
	using namespace PS5AA;

	// The setting.
	check(DownsampleOf(0) == DOWNSAMPLE_BILINEAR && DownsampleOf(1) == DOWNSAMPLE_SOFT && DownsampleOf(2) == DOWNSAMPLE_SHARP,
		"the setting's values");
	check(DownsampleOf(-1) == DOWNSAMPLE_BILINEAR && DownsampleOf(3) == DOWNSAMPLE_BILINEAR && DownsampleOf(1000) == DOWNSAMPLE_BILINEAR,
		"a value not among them is the default");
	check(DOWNSAMPLE_COUNT == 3 && std::strcmp(DOWNSAMPLE_CHOICES[0], "Bilinear") == 0 && std::strcmp(DOWNSAMPLE_CHOICES[1], "Soft") == 0 &&
			  std::strcmp(DOWNSAMPLE_CHOICES[2], "Sharp") == 0,
		"the choices' names (the Settings row lists them in this order)");

	// The place, as GSDevice's other passes size theirs.
	check(PlaceOf(0.0f, 0.0f, 3840.0f, 2160.0f) == Size{3840, 2160}, "a whole screen");
	check(PlaceOf(480.0f, 0.0f, 3360.0f, 2160.0f) == Size{2880, 2160}, "a 4:3 place on a 4K screen");
	check(PlaceOf(240.5f, 0.0f, 3599.5f, 2160.0f) == Size{3359, 2160}, "a place half a pixel over");

	// When it runs.
	const Size screen{3840, 2160};
	check(!ShouldDownsample(DOWNSAMPLE_BILINEAR, {5120, 3584}, screen), "Bilinear: never");
	check(ShouldDownsample(DOWNSAMPLE_SOFT, {5120, 3584}, screen) && ShouldDownsample(DOWNSAMPLE_SHARP, {5120, 3584}, screen),
		"8x (5120x3584) on a 4K screen: larger both ways");
	check(!ShouldDownsample(DOWNSAMPLE_SHARP, {3840, 2688}, screen) && !ShouldDownsample(DOWNSAMPLE_SHARP, {3840, 2160}, screen),
		"6x on 4K: as wide as the screen, not larger: the screen's filter");
	check(!ShouldDownsample(DOWNSAMPLE_SHARP, {4480, 2000}, screen), "larger one way only: not");
	check(!ShouldDownsample(DOWNSAMPLE_SHARP, {640, 448}, screen), "native: enlarged, not shrunk");
	check(!ShouldDownsample(DOWNSAMPLE_SOFT, {5120, 3584}, {0, 0}) && !ShouldDownsample(DOWNSAMPLE_SOFT, {5120, 3584}, {-4, 100}),
		"no place: not");
	check(ShouldDownsample(DOWNSAMPLE_SOFT, {2561, 1441}, {2560, 1440}), "one texel more each way: runs");

	// SMAA's size.
	check(SmaaSize(true, {5120, 3584}, {2880, 2160}) == Size{2880, 2160}, "SMAA after a downsample: at the place's size");
	check(SmaaSize(false, {2560, 1792}, {2880, 2160}) == Size{2560, 1792}, "SMAA alone: at the picture's own size");

	// The passes' constants: 8x on 4K, a 4:3 place, the picture in part of its texture.
	{
		const PassConstants across = Pass(DOWNSAMPLE_SHARP, 16, 5120, 2880, 8, 3584);
		check(across.origin == 16.0f && across.size == 5120.0f && across.out == 2880.0f && across.kernel == 1.0f,
			"across: the first column, the columns, the pixels, Sharp");
		check(across.across_first == 8.0f && across.across_last == 8.0f + 3583.0f, "across: the rows the picture is in");
		check(std::fabs(across.ratio - 5120.0f / 2880.0f) < 1e-5f && std::fabs(across.reach - 2.0f * 5120.0f / 2880.0f) < 1e-5f,
			"across: the ratio, and Sharp's reach of two pixels");
		const PassConstants down = Pass(DOWNSAMPLE_SOFT, 0, 3584, 2160, 0, 2880);
		check(down.origin == 0.0f && down.kernel == 0.0f && down.across_last == 2879.0f, "down: from the first pass's picture, Soft");
		check(std::fabs(down.reach - 3584.0f / 2160.0f) < 1e-5f, "down: Soft's reach of one pixel");
	}

	// The kernels widen with the ratio up to MAX_RATIO; the shader's loop always reaches all of it.
	for (int mode = DOWNSAMPLE_SOFT; mode <= DOWNSAMPLE_SHARP; mode++)
	{
		for (float ratio = 1.01f; ratio <= 25.0f; ratio += 0.37f)
		{
			const PassConstants p = Pass(static_cast<Downsample>(mode), 0, static_cast<int>(std::lround(1000.0f * ratio)), 1000, 0, 10);
			const int taps = static_cast<int>(std::floor(2.0f * p.reach)) + 2; // the most whole texels within the reach each side
			if (taps > MAX_TAPS)
			{
				std::printf("  FAIL: %s at %.2f: %d taps, the loop runs %d\n", DOWNSAMPLE_CHOICES[mode], ratio, taps, MAX_TAPS);
				failures++;
			}
			const float k = KernelRatio(p.ratio);
			check(k >= 1.0f && k <= MAX_RATIO && (p.ratio > MAX_RATIO || std::fabs(k - p.ratio) < 1e-6f), "the kernel's ratio");
		}
	}

	// The reference kernels.
	check(TentWeight(0.0, 2.0) == 1.0 && TentWeight(1.0, 2.0) == 0.5 && TentWeight(2.0, 2.0) == 0.0 && TentWeight(-3.0, 2.0) == 0.0,
		"the tent");
	check(LanczosWeight(0.0, 1.5) == 1.0 && std::fabs(LanczosWeight(1.5, 1.5)) < 1e-12 && LanczosWeight(3.0, 1.5) == 0.0 &&
			  LanczosWeight(2.0, 1.0) == 0.0 && LanczosWeight(1.5, 1.0) < 0.0,
		"Lanczos-2: 1 at the centre, 0 at each pixel, a negative lobe, nothing past two pixels");

	std::printf("downsampling's and SMAA's decisions (ps5_aa.h): %s\n", failures ? "FAILED" : "PASS");
	return failures ? 1 : 0;
}
