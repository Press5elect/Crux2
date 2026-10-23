#version 450
// Crux2 - the game's picture as a television's composite (one wire): narrower colour, rainbow on fine detail, crawling dots on colour edges.
// At the game's own size: 13 fetches a texel (signal.inc), a few hundred thousand texels.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	uint FrameCount;
	float crux_sig_strength;
	float crux_sig_luma;
	float crux_sig_chroma;
	float crux_sig_ring;
	float crux_sig_bleed;
	float crux_sig_crawl;
	float crux_sig_carrier;
} params;

#pragma parameter crux_sig_strength "Signal look strength" 1.0 0.0 1.0 0.05
#pragma parameter crux_sig_luma "Brightness softness (texels)" 1.00 0.0 2.0 0.05
#pragma parameter crux_sig_chroma "Colour bandwidth blur (texels)" 2.2 0.3 5.0 0.1
#pragma parameter crux_sig_ring "Edge ringing" 0.20 0.0 1.0 0.05
#pragma parameter crux_sig_bleed "Colour delay (texels)" 0.8 0.0 3.0 0.1
#pragma parameter crux_sig_crawl "Dot crawl" 0.35 0.0 1.0 0.05
#pragma parameter crux_sig_carrier "Carrier period (texels)" 3.4 2.2 8.0 0.1

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

#define COMPOSITE
#include "signal.inc"

void main()
{
	const vec3 original = texture(Source, vTexCoord).rgb;
	const vec3 tv = signal(vTexCoord, params.SourceSize, params.FrameCount, params.crux_sig_luma, params.crux_sig_chroma,
		params.crux_sig_ring, params.crux_sig_bleed, params.crux_sig_crawl, params.crux_sig_carrier);
	FragColor = vec4(clamp(mix(original, tv, params.crux_sig_strength), 0.0, 1.0), 1.0);
}
