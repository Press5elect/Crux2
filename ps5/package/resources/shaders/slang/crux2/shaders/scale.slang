#version 450
// Crux2 - the picture brought to the screen's size: bilinear, or sharp bilinear (each texel grown
// by the whole number of times it fits, only the steps between texels blended). Adapted from
// PSSwanStation's look_scale.frag (GPL-3.0-or-later, the same author).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	float crux_scale_sharp;
} params;

#pragma parameter crux_scale_sharp "Sharp bilinear" 1.0 0.0 1.0 1.0

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

void main()
{
	vec2 texel = vTexCoord * params.SourceSize.xy;
	if (params.crux_scale_sharp > 0.5)
	{
		const vec2 scale = max(floor(params.OutputSize.xy * params.SourceSize.zw), vec2(1.0));
		const vec2 centre = fract(texel) - 0.5;
		const vec2 region = 0.5 - 0.5 / scale;
		texel = floor(texel) + (centre - clamp(centre, -region, region)) * scale + 0.5;
	}
	FragColor = vec4(texture(Source, texel * params.SourceSize.zw).rgb, 1.0);
}
