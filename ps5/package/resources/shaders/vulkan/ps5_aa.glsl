// Crux2 - downsampling and SMAA, before the picture is sharpened and shown.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// After OGBox (Press5elect, GPL-3.0-or-later): its ssaa.frag and SMAA passes, made into PCSX2's Vulkan
// device's utility passes (GSDeviceVK_PS5.cpp: DoDownsample, DoSMAA; ps5/frontend/ps5_aa.h decides). The
// vertex shader is convert.glsl's; every pass reads its pixel from gl_FragCoord (row 0 at the top).
//
//   AA_PASS 0  Downsample across: from the part of samp0 the picture is in (u_axis.x, its first column;
//              u_axis.y columns; rows u_across.x .. u_across.y) to u_axis.z columns, every row kept. Into a
//              16-bit float picture: Sharp's lobes are kept for the second pass.
//   AA_PASS 1  Downsample down: the same along the columns of the first pass's picture; clamped to 0..1.
//              The kernel (u_axis.w): 0 Soft, a tent; 1 Sharp, Lanczos-2; both u_across.z texels a pixel
//              wide (the ratio, at most ps5_aa.h's MAX_RATIO), reaching u_across.w texels each side,
//              normalised, the picture's edge texels repeated past its edge.
//   AA_PASS 2  SMAA's edges (luma), 3 its blending weights (samp1 the area texture, samp2 the search
//              texture, point sampled), 4 the neighbourhood blending (samp1 the weights). SMAA 1x, High
//              preset, iryoku's SMAA.hlsl (MIT, shaders/common/smaa/), put in place of the #include line by
//              the device (there is no #include in the runtime compiler). u_metrics: 1/w, 1/h, w, h.

#ifdef FRAGMENT_SHADER

layout(location = 0) out vec4 o_col0;
layout(set = 0, binding = 0) uniform sampler2D samp0;

#if AA_PASS == 0 || AA_PASS == 1

layout(push_constant) uniform cb_aa
{
	vec4 u_axis;   // the first texel along the pass's axis, the texels, the pixels written, the kernel (0 Soft, 1 Sharp)
	vec4 u_across; // the first texel across, the last texel across, the ratio (texels a pixel; the kernel's), the reach
};

const float PI = 3.14159265358979;
const int MAX_TAPS = 40; // ps5_aa.h

float weight(float d)
{
	const float x = abs(d) / max(u_across.z, 1.0);
	if (u_axis.w < 0.5)
		return max(1.0 - x, 0.0);
	if (x < 1e-4)
		return 1.0;
	if (x >= 2.0)
		return 0.0;
	const float px = PI * x;
	return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

void main()
{
	const ivec2 pixel = ivec2(gl_FragCoord.xy);
#if AA_PASS == 0
	const int along = pixel.x;
	const int across = min(int(u_across.x) + pixel.y, int(u_across.y));
#else
	const int along = pixel.y;
	const int across = min(int(u_across.x) + pixel.x, int(u_across.y));
#endif
	const int first = int(u_axis.x);
	const int last = first + int(u_axis.y) - 1;
	const float texels_a_pixel = u_axis.y / max(u_axis.z, 1.0);
	// The pixel's centre in texels (a texel's centre is at its index).
	const float centre = u_axis.x + (float(along) + 0.5) * texels_a_pixel - 0.5;
	const int lo = int(ceil(centre - u_across.w));
	vec4 sum = vec4(0.0);
	float total = 0.0;
	for (int k = 0; k < MAX_TAPS; k++)
	{
		const int i = lo + k;
		const float d = float(i) - centre;
		if (d > u_across.w)
			break;
		const float w = weight(d);
		const int t = clamp(i, first, last);
#if AA_PASS == 0
		sum += w * texelFetch(samp0, ivec2(t, across), 0);
#else
		sum += w * texelFetch(samp0, ivec2(across, t), 0);
#endif
		total += w;
	}
	const vec4 c = sum / max(total, 1e-4);
#if AA_PASS == 0
	o_col0 = c;
#else
	o_col0 = clamp(c, 0.0, 1.0);
#endif
}

#elif AA_PASS >= 2 && AA_PASS <= 4

layout(push_constant) uniform cb_smaa
{
	vec4 u_metrics; // 1/w, 1/h, w, h of the pictures (all of one size)
};

#define SMAA_RT_METRICS u_metrics
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1
#include "smaa/SMAA.hlsl"

#if AA_PASS == 3
layout(set = 0, binding = 1) uniform sampler2D samp1; // the area texture (linear)
layout(set = 0, binding = 2) uniform sampler2D samp2; // the search texture (point)
#elif AA_PASS == 4
layout(set = 0, binding = 1) uniform sampler2D samp1; // the blending weights
#endif

void main()
{
	// (SMAA's vertex parts, which only add offsets to the place, are worked out here.)
	const vec2 uv = gl_FragCoord.xy * u_metrics.xy;
#if AA_PASS == 2
	vec4 offset[3];
	SMAAEdgeDetectionVS(uv, offset);
	o_col0 = vec4(SMAALumaEdgeDetectionPS(uv, offset, samp0), 0.0, 0.0); // (discards where there is no edge)
#elif AA_PASS == 3
	vec2 pixcoord;
	vec4 offset[3];
	SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
	o_col0 = SMAABlendingWeightCalculationPS(uv, pixcoord, offset, samp0, samp1, samp2, vec4(0.0));
#else
	vec4 offset;
	SMAANeighborhoodBlendingVS(uv, offset);
	o_col0 = SMAANeighborhoodBlendingPS(uv, offset, samp0, samp1);
#endif
}

#endif

#endif
