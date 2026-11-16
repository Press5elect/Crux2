// Crux2 - downsampling and SMAA: what is decided before the passes run (ps5_aa.glsl's passes are
// recorded by GSDeviceVK_PS5.cpp; GSRenderer::VSync asks GSDevice::PS5Antialias for them).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Downsampling ([PS5] Downsample): a picture drawn larger than its place on the screen (the internal
// resolution above the screen's, e.g. 8x or more at 4K) is shown by one bilinear tap per screen pixel,
// which reads 4 texels of the 2x2 to 8x8 or more the pixel covers and lets the rest alias. Soft and Sharp
// make a picture exactly the place's size first, in two passes (across, then down), from every texel
// the kernel reaches; it is then shown 1:1.
//   Soft:  a tent (bilinear widened to the ratio): no overshoot, the least aliasing, a little softer.
//   Sharp: Lanczos-2 widened to the ratio: crisper edges (a few percent of over- and undershoot,
//          clamped), detail finer than a screen pixel suppressed rather than kept as shimmer.
// Both kernels are normalised (a flat colour comes out exactly) and widened up to MAX_RATIO; past it
// they keep that width (a picture more than 8 times the place's size aliases a little again).
//
// SMAA ([PS5] SMAA): iryoku's SMAA 1x, High preset, luma edges, at the place's size after a downsample,
// else at the picture's own size.
#pragma once

#include <algorithm>
#include <cmath>

namespace PS5AA
{
enum Downsample : int
{
	DOWNSAMPLE_BILINEAR = 0, // as PCSX2: the screen's one bilinear tap (the default)
	DOWNSAMPLE_SOFT = 1,
	DOWNSAMPLE_SHARP = 2,
};
constexpr int DOWNSAMPLE_COUNT = 3;
constexpr const char* DOWNSAMPLE_CHOICES[DOWNSAMPLE_COUNT] = {"Bilinear", "Soft", "Sharp"};

// The setting's value checked (anything else is the default).
inline Downsample DownsampleOf(int value)
{
	return (value == DOWNSAMPLE_SOFT || value == DOWNSAMPLE_SHARP) ? static_cast<Downsample>(value) : DOWNSAMPLE_BILINEAR;
}

// The kernels are widened with the ratio up to this many texels a pixel (Sharp then reaches 16 texels each side).
constexpr float MAX_RATIO = 8.0f;
// ps5_aa.glsl's loop runs at most this many taps a pixel along the pass's axis.
constexpr int MAX_TAPS = 40;

struct Size
{
	int w = 0, h = 0;
	bool operator==(const Size& o) const { return w == o.w && h == o.h; }
};

// The picture's place on the screen in whole pixels, as GSDevice's FSR, NIS and Look size theirs.
inline Size PlaceOf(float left, float top, float right, float bottom)
{
	return {static_cast<int>(std::ceil(right - left)), static_cast<int>(std::ceil(bottom - top))};
}

// Whether a downsample runs: chosen, a place, and a picture larger than it both ways (only then is
// one bilinear tap short of texels; a picture smaller one way is enlarged there, which is FSR's,
// NIS's or the screen's filter's).
inline bool ShouldDownsample(Downsample mode, Size picture, Size place)
{
	return mode != DOWNSAMPLE_BILINEAR && place.w > 0 && place.h > 0 && picture.w > place.w && picture.h > place.h;
}

// The size SMAA runs at: the place's after a downsample, else the picture's own.
inline Size SmaaSize(bool downsampled, Size picture, Size place)
{
	return downsampled ? place : picture;
}

// The kernel's half-width in texels for `ratio` texels a pixel (the loop's reach each side).
inline float KernelRatio(float ratio)
{
	return std::clamp(ratio, 1.0f, MAX_RATIO);
}
inline float Reach(Downsample mode, float ratio)
{
	return KernelRatio(ratio) * (mode == DOWNSAMPLE_SHARP ? 2.0f : 1.0f);
}

// One downsample pass's push constants (ps5_aa.glsl's cb_aa), the same numbers the device sends and the
// PC test records: along the pass's axis the picture's first texel, its texels, the pixels written and
// the kernel; across it the first texel, the last texel, the ratio and the reach.
struct PassConstants
{
	float origin, size, out, kernel;
	float across_first, across_last, ratio, reach;
};

inline PassConstants Pass(Downsample mode, int origin, int size, int out, int across_first, int across_size)
{
	const float ratio = static_cast<float>(size) / static_cast<float>(std::max(out, 1));
	return {static_cast<float>(origin), static_cast<float>(size), static_cast<float>(out), mode == DOWNSAMPLE_SHARP ? 1.0f : 0.0f,
		static_cast<float>(across_first), static_cast<float>(across_first + std::max(across_size, 1) - 1), ratio,
		Reach(mode, ratio)};
}

// The kernels in double precision (the PC test's reference; ps5_aa.glsl has the same in float):
// d is the distance in texels from the pixel's centre, k the kernel's ratio.
inline double TentWeight(double d, double k)
{
	return std::max(0.0, 1.0 - std::fabs(d) / k);
}
inline double LanczosWeight(double d, double k)
{
	const double x = std::fabs(d) / k;
	if (x < 1e-6)
		return 1.0;
	if (x >= 2.0)
		return 0.0;
	const double pi = 3.14159265358979323846;
	return 2.0 * std::sin(pi * x) * std::sin(pi * x * 0.5) / (pi * x * pi * x);
}
} // namespace PS5AA
