#version 450
// Crux2 - film grain over the picture, drawn at the screen's size: soft grains a pixel or two
// across, a new pattern every frame (FrameCount), strongest in the middle tones as film's is and
// fainter in the blacks and whites. One fetch and a little arithmetic a pixel.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	uint FrameCount;
	float crux_grain_amount;
	float crux_grain_size;
	float crux_grain_colour;
	float crux_grain_luma;
} params;

#pragma parameter crux_grain_amount "Grain amount" 0.07 0.0 0.30 0.01
#pragma parameter crux_grain_size "Grain size (screen pixels)" 1.6 0.5 4.0 0.1
#pragma parameter crux_grain_colour "Coloured grain" 0.0 0.0 1.0 1.0
#pragma parameter crux_grain_luma "Weaker in blacks and whites" 1.0 0.0 1.0 0.05

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

// A hash of a place and the frame to 0..1 (Dave Hoskins' "hash without sine", float only).
float hash(vec3 p)
{
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.x + p.y) * p.z);
}

// Value noise: the hashes of the four grain corners around p, blended smoothly.
float grain(vec2 p, float seed)
{
	const vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	const float a = hash(vec3(i, seed));
	const float b = hash(vec3(i + vec2(1.0, 0.0), seed));
	const float c = hash(vec3(i + vec2(0.0, 1.0), seed));
	const float d = hash(vec3(i + vec2(1.0, 1.0), seed));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
	const vec3 colour = texture(Source, vTexCoord).rgb;
	// The frame's seed: kept small, so the hash keeps its precision.
	const float seed = float(params.FrameCount % 4096u) * 1.37 + 0.5;
	const vec2 p = gl_FragCoord.xy / max(params.crux_grain_size, 0.5);
	// Two layers, one finer, added: a triangular spread around 0, which reads as grain rather
	// than as static.
	float n = grain(p, seed) + grain(p * 1.93 + 17.0, seed + 0.5) - 1.0;
	vec3 noise = vec3(n);
	if (params.crux_grain_colour > 0.5)
	{
		noise.r = grain(p + 31.0, seed + 0.21) + grain(p * 1.93 + 47.0, seed + 0.71) - 1.0;
		noise.b = grain(p + 59.0, seed + 0.33) + grain(p * 1.93 + 83.0, seed + 0.93) - 1.0;
	}
	const float luma = dot(colour, vec3(0.2126, 0.7152, 0.0722));
	const float midtones = 0.25 + 0.75 * clamp(4.0 * luma * (1.0 - luma), 0.0, 1.0);
	const float weight = mix(1.0, midtones, params.crux_grain_luma);
	FragColor = vec4(clamp(colour + noise * (params.crux_grain_amount * weight), 0.0, 1.0), 1.0);
}
