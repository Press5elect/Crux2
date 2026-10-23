// Crux2 - an NTSC television's input, shared by composite.slang and s-video.slang.
// SPDX-License-Identifier: GPL-3.0+
//
// Each texel of the game's picture, at the game's own size, as a television would show it:
//  * brightness (Y) and colour (I, Q) apart, as the console's encoder makes them;
//  * S-Video: they travel on two wires. The colour is cut to a narrow band (a blur along the
//    line some texels wide) and arrives a little late (it bleeds to the right); the brightness
//    is a little soft, with a faint ring on hard edges from the set's sharpening;
//  * composite (COMPOSITE defined): the colour rides on the brightness at the colour carrier's
//    frequency, its phase turning half a cycle each line and each frame. The set takes the colour
//    back out by demodulating (fine brightness detail comes back as false colour, the "rainbow"),
//    and its brightness filter leaves some of the carrier in: dots along colour edges, crawling
//    from frame to frame (dot crawl). Its bands are narrower than S-Video's.
// Every fetch reads a texel centre (the pass samples linearly, so a centre is that texel): 13
// taps along the line, each one fetch.

#define TAPS 6

const mat3 RGB_TO_YIQ = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
const mat3 YIQ_TO_RGB = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);

float gaussian(float d, float sigma)
{
	return exp(-0.5 * d * d / max(sigma * sigma, 1e-4));
}

vec3 signal(vec2 uv, vec4 size, uint frame, float lumaBlur, float chromaBlur, float ringing, float bleed, float crawl,
	float carrier)
{
	const vec2 texel = uv * size.xy - 0.5;
	const float x = floor(texel.x + 0.5);
	const float line = floor(texel.y + 0.5);
	const float v = (line + 0.5) * size.w;
	// The carrier's phase: half a turn a line, and a frame (NTSC's 227.5 cycles a line).
	const float phase = 3.14159265 * mod(line + float(frame & 1u), 2.0);
	const float cycle = 6.2831853 / max(carrier, 2.05);
	float y = 0.0, yWide = 0.0, yCarrier = 0.0, i = 0.0, q = 0.0;
	float wy = 0.0, wyWide = 0.0, wc = 0.0;
	vec3 here = vec3(0.0);
	for (int j = -TAPS; j <= TAPS; j++)
	{
		const float d = float(j);
		const vec3 yiq = RGB_TO_YIQ * texture(Source, vec2((x + d + 0.5) * size.z, v)).rgb;
		if (j == 0)
			here = yiq;
		const float ky = gaussian(d, lumaBlur);
		const float kw = gaussian(d, lumaBlur + 1.2);
		// The colour is late: its filter is centred left of the texel by `bleed`.
		const float kc = gaussian(d + bleed, chromaBlur);
#ifdef COMPOSITE
		const float theta = cycle * (x + d) + phase;
		const float c = cos(theta), s = sin(theta);
		const float composite = yiq.x + yiq.y * c + yiq.z * s;
		yCarrier += ky * (composite - yiq.x);
		i += kc * 2.0 * composite * c;
		q += kc * 2.0 * composite * s;
#else
		i += kc * yiq.y;
		q += kc * yiq.z;
#endif
		y += ky * yiq.x;
		yWide += kw * yiq.x;
		wy += ky;
		wyWide += kw;
		wc += kc;
	}
	y /= wy;
	yWide /= wyWide;
	// The set's sharpening: an overshoot on each edge.
	float luma = y + ringing * (y - yWide);
#ifdef COMPOSITE
	luma += crawl * yCarrier / wy;
#endif
	return YIQ_TO_RGB * vec3(luma, i / wc, q / wc);
}
