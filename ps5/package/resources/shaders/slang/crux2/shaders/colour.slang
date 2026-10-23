#version 450
// Crux2 - the picture's colours, drawn at the screen's size: colour temperature (warmer, cooler),
// vibrance (more colour where there is little), saturation, brightness, contrast and gamma. One
// fetch a pixel.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	float crux_col_temperature;
	float crux_col_vibrance;
	float crux_col_saturation;
	float crux_col_brightness;
	float crux_col_contrast;
	float crux_col_gamma;
} params;

#pragma parameter crux_col_temperature "Colour temperature (- cooler, + warmer)" 0.0 -1.0 1.0 0.05
#pragma parameter crux_col_vibrance "Vibrance" 0.15 -1.0 1.0 0.05
#pragma parameter crux_col_saturation "Saturation" 1.0 0.0 2.0 0.05
#pragma parameter crux_col_brightness "Brightness" 1.0 0.5 1.5 0.02
#pragma parameter crux_col_contrast "Contrast" 1.0 0.5 1.5 0.02
#pragma parameter crux_col_gamma "Gamma (above 1 darker)" 1.0 0.6 1.6 0.02

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
	vec3 c = texture(Source, vTexCoord).rgb;
	// In light (gamma 2.2) for the white point and saturation, so a grey stays grey.
	vec3 lin = pow(max(c, vec3(0.0)), vec3(2.2));
	const float t = params.crux_col_temperature;
	// Roughly 6500 K to 4500 K (warm) or 9300 K (cool), at the same brightness.
	const vec3 warm = vec3(1.0, 0.93, 0.78), cool = vec3(0.86, 0.93, 1.0);
	vec3 white = mix(vec3(1.0), t > 0.0 ? warm : cool, abs(t));
	white /= dot(white, vec3(0.2126, 0.7152, 0.0722));
	lin *= white;
	const float y = dot(lin, vec3(0.2126, 0.7152, 0.0722));
	const float chroma = max(lin.r, max(lin.g, lin.b)) - min(lin.r, min(lin.g, lin.b));
	const float amount = params.crux_col_saturation * (1.0 + params.crux_col_vibrance * (1.0 - clamp(chroma * 2.0, 0.0, 1.0)));
	lin = max(mix(vec3(y), lin, amount), vec3(0.0));
	c = pow(lin, vec3(1.0 / 2.2));
	c = (c - 0.5) * params.crux_col_contrast + 0.5;
	c *= params.crux_col_brightness;
	c = pow(clamp(c, 0.0, 1.0), vec3(params.crux_col_gamma));
	FragColor = vec4(c, 1.0);
}
