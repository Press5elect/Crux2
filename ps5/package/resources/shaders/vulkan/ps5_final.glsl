// Crux2 - the last passes before the screen: debanding, and HDR10 output.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From OGBox (Press5elect, GPL-3.0-or-later): its hdr.frag and deband.frag, put together for PCSX2's
// Vulkan device (GSDeviceVK_PS5.cpp, the PS5Final passes). The vertex shader is convert.glsl's.
//
//   FINAL_PASS 0  Debanding, onto an SDR screen: the frame's picture (the presented game, at the
//                 screen's size) into the swapchain image; the menus are drawn over it after.
//   FINAL_PASS 1  HDR10: the picture (debanded first when u_deband.w is 1) and the menus, each drawn
//                 as for an SDR screen into a layer of its own, made into HDR10's pixels (BT.2020
//                 primaries, the PQ curve of SMPTE ST 2084) in an A2R10G10B10 image, which is copied
//                 into the framebuffer the graphics driver has retyped as HDR10
//                 (ps5/patches/mesa-27/0020).
//
// HDR10 (as OGBox, after VLC for PS5's SDR menus on PQ):
//  - the SDR values decoded with a 2.2 gamma (as an SDR TV shows them), then BT.709 to BT.2020;
//  - white at u_metrics.x nits (the setting; ITU-R BT.2408's 203 by default), the menus exactly so;
//  - the game's picture (u_metrics.w 1) with its highlights raised: linear luminance above 0.5 rises
//    on t^2 to u_metrics.y nits at full white, with the slope and value of the straight line at the
//    knee (no step), every colour scaled by its luminance's gain, so hues keep; u_metrics.y at or
//    below the white: none;
//  - the menus over the picture as an SDR screen blends them (in its encoded values), the gain kept
//    off them in proportion to their cover (the layer's alpha: the menus' pipeline for this layer
//    adds up their cover, GSDeviceVK_PS5.cpp).
// u_cal (Settings > Adjust HDR brightness): .x 1 the highlights' test: a square in the middle at PQ's
// top (10000 nits, which a TV shows at its brightest) with an X in it at .y nits; where .y reaches the
// TV's peak the X can no longer be seen. .x 2 the white level's: a grey scale of SDR's eleven tenths
// and a white patch below it, at the white level .y. Over the menus.
//
// Debanding (as OGBox's deband.frag): each pixel is compared with the average of four points around
// it, at four reaches up to about a band's width (u_deband.z texels) in random directions; wherever
// that average is within a band's step (u_deband.x) of the pixel, the pixel takes it, the farther
// reach last. A little triangular noise (u_deband.y) then hides what steps are left. Edges and
// detail differ from their surroundings by more than the step and are kept.

#ifdef FRAGMENT_SHADER

layout(push_constant) uniform cb_final
{
	vec4 u_metrics; // x white nits, y highlights' peak nits, z 1 the layers are already linear, w 1 the game's picture is shown
	vec4 u_cal;     // x the test screen (0 none, 1 highlights, 2 white level), y its nits
	vec4 u_deband;  // x a band's step, y the dither, z the reach in texels, w 1 on (FINAL_PASS 1)
};

layout(set = 0, binding = 0) uniform sampler2D samp0; // the picture
#if FINAL_PASS == 1
layout(set = 0, binding = 1) uniform sampler2D samp1; // the menus, premultiplied (over clear 0)
#endif

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

float hash(vec2 p)
{
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 deband(vec2 uv)
{
	const vec2 size = vec2(textureSize(samp0, 0));
	const vec2 texel = 1.0 / size;
	const vec3 c = textureLod(samp0, uv, 0.0).rgb;
	vec3 r = c;
	const float seed = hash(gl_FragCoord.xy);
	for (int k = 1; k <= 4; k++)
	{
		const float dist = u_deband.z * float(k) / 4.0 * (0.5 + 0.5 * hash(gl_FragCoord.xy + float(k) * 17.0));
		const float ang = 6.2831853 * fract(seed + float(k) * 0.618034);
		const vec2 off = vec2(cos(ang), sin(ang)) * dist * texel;
		const vec2 per = vec2(-off.y, off.x) * size.yx * texel; // at right angles, in texels
		const vec3 avg = (textureLod(samp0, uv + off, 0.0).rgb + textureLod(samp0, uv - off, 0.0).rgb +
			textureLod(samp0, uv + per, 0.0).rgb + textureLod(samp0, uv - per, 0.0).rgb) * 0.25;
		const vec3 diff = abs(avg - c);
		if (max(diff.r, max(diff.g, diff.b)) <= u_deband.x)
			r = avg;
	}
	const float noise = (hash(gl_FragCoord.xy + 0.37) + hash(gl_FragCoord.xy + 1.91) - 1.0) * u_deband.y;
	return clamp(r + noise, 0.0, 1.0);
}

#if FINAL_PASS == 0

void main()
{
	o_col0 = vec4(deband(v_tex), 1.0);
}

#else

const float M1 = 0.1593017578125, M2 = 78.84375;
const float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
const float KNEE = 0.5;

// BT.709 to BT.2020 (ITU-R BT.2087), columns as GLSL takes them
const mat3 BT709_TO_2020 = mat3(
	0.6274, 0.0691, 0.0164,
	0.3293, 0.9195, 0.0880,
	0.0433, 0.0114, 0.8956);

vec3 pq(vec3 nits)
{
	const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(M1));
	return pow((C1 + C2 * y) / (1.0 + C3 * y), vec3(M2));
}

vec3 decode(vec3 c)
{
	return u_metrics.z > 0.5 ? c : pow(max(c, vec3(0.0)), vec3(2.2));
}

// The test screen's pattern at p (screen pixels) for a screen of size s: true, and its nits, inside it.
bool pattern(vec2 p, vec2 s, out float nits)
{
	nits = 0.0;
	const vec2 c = s * 0.5;
	if (u_cal.x < 1.5)
	{
		const float side = 0.40 * s.y;
		const vec2 d = (p - c) / side; // -0.5 .. 0.5 across the square
		if (max(abs(d.x), abs(d.y)) >= 0.5)
			return false;
		const bool x = max(abs(d.x), abs(d.y)) < 0.3 && min(abs(d.x - d.y), abs(d.x + d.y)) < 0.07;
		nits = x ? u_cal.y : 10000.0;
		return true;
	}
	const vec2 size = vec2(0.64 * s.x, 0.40 * s.y);
	const vec2 d = (p - (c - size * 0.5)) / size; // 0 .. 1 across the chart
	if (any(lessThan(d, vec2(0.0))) || any(greaterThanEqual(d, vec2(1.0))))
		return false;
	float level = 1.0;
	if (d.y < 0.42)
	{
		const float k = d.x * 11.0;
		level = (fract(k) < 0.06 || d.y > 0.38) ? 0.0 : floor(k) / 10.0; // eleven patches, black between
	}
	nits = pow(level, 2.2) * u_cal.y;
	return true;
}

void main()
{
	const ivec2 at = ivec2(gl_FragCoord.xy);
	float test;
	if (u_cal.x > 0.5 && pattern(gl_FragCoord.xy, vec2(textureSize(samp1, 0)), test))
	{
		o_col0 = vec4(pq(vec3(test)), 1.0); // grey: the same in BT.2020
		return;
	}
	const vec3 picture = (u_deband.w > 0.5) ? deband(v_tex) : texelFetch(samp0, at, 0).rgb;
	const vec4 ui = texelFetch(samp1, at, 0);
	const float white = max(u_metrics.x, 1.0);
	float gain = 1.0;
	if (u_metrics.w > 0.5 && u_metrics.y > white)
	{
		const float l = dot(decode(picture), vec3(0.2126, 0.7152, 0.0722));
		if (l > KNEE)
		{
			const float t = (min(l, 1.0) - KNEE) / (1.0 - KNEE);
			gain = (l + (u_metrics.y / white - 1.0) * t * t) / l;
		}
	}
	const vec3 sdr = ui.rgb + picture * (1.0 - ui.a);
	const vec3 light = decode(sdr) * mix(gain, 1.0, ui.a);
	o_col0 = vec4(pq(max(BT709_TO_2020 * light, vec3(0.0)) * white), 1.0);
}

#endif

#endif
