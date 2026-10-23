// Crux2 - frame generation for PCSX2's Vulkan renderer: a picture between two finished pictures.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A PlayStation 2 game hands over finished pictures and nothing else: no motion vectors, no depth.
// So the in-between picture is made from the two pictures themselves, the way a television's
// motion smoothing or a program like Lossless Scaling does it (this is neither's code, and it is
// not AMD's FSR 3 or 4, which need the game's motion vectors and depth):
//
//   FG_PASS 0  the two pictures' brightness, side by side in one small picture (a quarter size)
//   FG_PASS 1  that, halved (four times: down to a sixty-fourth, where long moves are short)
//   FG_PASS 2  where each block went. In the smallest pair it is searched from nothing; in each
//              larger pair from the answer of the one before, and there the block walks downhill
//              from that answer to the nearest move that fits. A move further off is taken only
//              when it fits much better: a tiled floor fits one tile on as well as where it
//              really went, and must not be believed
//   FG_PASS 3  the move most of the picture makes (the camera's), from the blocks that are sure
//              of theirs. The smallest pair is then searched again from that move
//   FG_PASS 4  blocks that are not sure take what the sure ones around them say: a plain wall
//              moves with the picture around it
//   FG_PASS 5  blocks whose answer stands out from their neighbours' take a neighbour's
//   FG_PASS 6  two numbers for the whole picture: how much of it found no match, and how much of
//              it moves wildly (after a cut to another scene, and then nothing is made up)
//   FG_PASS 7  the in-between picture at the time u_params.x (0 the older picture, 1 the newer):
//              each pixel fetched from where it was in the older and where it will be in the
//              newer, and the two mixed; where they disagree, the pictures are mixed in place
//   FG_PASS 8  not frame generation: the small, soft, darkened picture shown beside a picture
//              narrower than the screen (GSDevice::SidePicture), its brightness u_params.x
//
// A block's move is searched symmetrically about the middle of the two pictures: the older one is
// read half the move back and the newer one half the move on, and the move for which the two
// agree best is taken. The vertex shader is convert.glsl's. GSDevice::FrameGenMake drives the passes.

#ifdef FRAGMENT_SHADER

layout(push_constant) uniform cb_fg
{
	vec4 u_target; // the picture written: width, height, one over each
	vec4 u_source; // samp0: width, height, one over each
	vec4 u_params; // x: the time of the picture made (pass 7)
	               // y: pass 2: 0 searched from nothing, 1 from samp1's answer, 2 the same and the
	               //    last search (the largest pair); pass 3: 1 the second run
	               // z: pass 2: 1 the pair is read between its pixels (all but the last search);
	               //    2 the same, and the moves around the start are tried two pixels apart
	               // w: pass 2: how many moves each way around the start are tried
};

layout(set = 0, binding = 0) uniform sampler2D samp0;
#if FG_PASS == 0 || FG_PASS == 2 || FG_PASS == 3 || FG_PASS == 6 || FG_PASS == 7
layout(set = 0, binding = 1) uniform sampler2D samp1;
#endif
#if FG_PASS == 7
layout(set = 0, binding = 2) uniform sampler2D samp2;
layout(set = 0, binding = 3) uniform sampler2D samp3;
#endif

layout(location = 0) out vec4 o_col0;

const vec3 LUMA = vec3(0.299f, 0.587f, 0.114f);

// A cost above MATCH_BAD is no match at all, one below MATCH_GOOD is one (brightness, 0..1, a
// pixel of the block on average).
const float MATCH_GOOD = 0.035f;
const float MATCH_BAD = 0.10f;

#if FG_PASS == 0

void main()
{
	// Four taps between pixels, each the mean of 2 x 2: the mean of 4 x 4 for a quarter-size target.
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	float older = 0.0f, newer = 0.0f;
	for (int y = -1; y <= 1; y += 2)
	{
		for (int x = -1; x <= 1; x += 2)
		{
			const vec2 at = uv + vec2(x, y) * u_source.zw;
			older += dot(textureLod(samp0, at, 0.0f).rgb, LUMA);
			newer += dot(textureLod(samp1, at, 0.0f).rgb, LUMA);
		}
	}
	o_col0 = vec4(older * 0.25f, newer * 0.25f, 0.0f, 1.0f);
}

#elif FG_PASS == 1

void main()
{
	// Four taps three quarters of a pixel from the middle: each row and column weighed 1 3 3 1,
	// which lets less of what is too fine for the smaller picture through than a plain mean does
	// (it would come out as a pattern that is not there).
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	vec4 sum = vec4(0.0f);
	for (int y = -1; y <= 1; y += 2)
	{
		for (int x = -1; x <= 1; x += 2)
			sum += textureLod(samp0, uv + vec2(x, y) * 0.75f * u_source.zw, 0.0f);
	}
	o_col0 = sum * 0.25f;
}

#elif FG_PASS == 2

// How badly the two pictures agree around `uv` for the move `v` (both in parts of the picture),
// and by how much the older one is brighter there (`brighter`).
//
// It is how much the difference of the two varies over the block, not the difference itself: a
// picture that fades, a flash or a lamp going on changes every pixel alike and leaves the move
// findable. A place one of whose ends is outside the picture says nothing and is left out.
//
// The smaller pairs are read between their pixels, four at a time: a pattern about as fine as
// the pixels themselves (a tiled floor far away) fits a pixel's step by chance or not at all, and
// would send the search a whole repeat off; read like this it is plain grey, and the block takes
// the move of what is around it. The largest pair is read as it is, for the exact place.
float Cost(vec2 uv, vec2 v, out float brighter)
{
	const vec2 soft = vec2((u_params.z > 0.5f) ? 0.5f : 0.0f);
	vec3 sum = vec3(0.0f);
	for (int y = -2; y <= 2; y++)
	{
		for (int x = -2; x <= 2; x++)
		{
			const vec2 at = uv + (vec2(x, y) + soft) * u_source.zw;
			const vec2 was = at - 0.5f * v, will = at + 0.5f * v;
			const vec2 low = min(was, will), high = max(was, will);
			if (min(low.x, low.y) < 0.0f || max(high.x, high.y) > 1.0f)
				continue;
			const float apart = textureLod(samp0, was, 0.0f).r - textureLod(samp0, will, 0.0f).g;
			sum += vec3(apart, apart * apart, 1.0f);
		}
	}
	// (Fewer than a row of the block inside the picture: no answer.)
	if (sum.z < 5.0f)
	{
		brighter = 0.0f;
		return 1.0f;
	}
	brighter = sum.x / sum.z;
	return sqrt(max(sum.y / sum.z - brighter * brighter, 0.0f));
}

float Cost(vec2 uv, vec2 v)
{
	float brighter;
	return Cost(uv, v, brighter);
}

// The place between the grid's places where the cost is least: along one direction it falls to
// the best place and rises after it at about the same slope, and the two lines cross there.
float Between(float before, float at, float after)
{
	const float slope = max(before, after) - at;
	return (slope > 1e-6f) ? clamp(0.5f * (before - after) / slope, -0.5f, 0.5f) : 0.0f;
}

// Downhill from `v`, a pixel at a time, to where no neighbouring move fits better; then to the
// place between the pixels. Returns how well the move fits there.
float Settle(vec2 uv, int steps, inout vec2 v)
{
	const vec2 px = u_source.zw;
	float cost = Cost(uv, v);
	for (int turn = 0; turn < steps; turn++)
	{
		vec2 best = v;
		float best_cost = cost;
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				if (x == 0 && y == 0)
					continue;
				const vec2 other = v + vec2(x, y) * px;
				const float other_cost = Cost(uv, other);
				if (other_cost < best_cost)
				{
					best_cost = other_cost;
					best = other;
				}
			}
		}
		if (best == v)
			break;
		v = best;
		cost = best_cost;
	}
	v.x += Between(Cost(uv, v - vec2(px.x, 0.0f)), cost, Cost(uv, v + vec2(px.x, 0.0f))) * px.x;
	v.y += Between(Cost(uv, v - vec2(0.0f, px.y)), cost, Cost(uv, v + vec2(0.0f, px.y))) * px.y;
	return Cost(uv, v);
}

void main()
{
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	const vec2 px = u_source.zw;
	const bool from_answer = (u_params.y > 0.5f);
	const bool last = (u_params.y > 1.5f);
	const vec2 home = from_answer ? textureLod(samp1, uv, 0.0f).xy : vec2(0.0f);

	// A block that is in one picture only with the move it starts from cannot be searched for (at
	// the picture's edge while the camera turns: it has just come in, or is about to leave). It
	// keeps that move; the last pass takes it from the one picture it is in.
	const vec2 low = min(uv - 0.5f * home, uv + 0.5f * home), high = max(uv - 0.5f * home, uv + 0.5f * home);
	if (min(low.x, low.y) < 0.0f || max(high.x, high.y) > 1.0f)
	{
		o_col0 = vec4(home, 0.0f, 0.0f);
		return;
	}

	// The nearest move to the start that fits.
	vec2 best = home;
	float best_cost = Settle(uv, 2, best);

	// And the moves around the start, u_params.w of them each way, with standing still and what
	// the neighbours started from: the block may belong to something that moves on its own in
	// front of the rest. One of them is taken only if it fits much better than the nearest one
	// does. (They are tried on a grid and the nearest one was not, which is to their
	// disadvantage, as it should be. In the smaller pairs the grid is two pixels wide: read
	// between its pixels a pair has nothing finer than that, and it reaches twice as far.)
	const float pace = (u_params.z > 1.5f) ? 2.0f : 1.0f;
	vec2 other = best;
	float other_cost = 1e9f, sum = 0.0f, count = 0.0f;
	const int reach = int(u_params.w + 0.5f);
	for (int y = -reach; y <= reach; y++)
	{
		for (int x = -reach; x <= reach; x++)
		{
			const vec2 v = home + vec2(x, y) * pace * px;
			const float cost = Cost(uv, v);
			sum += min(cost, MATCH_BAD * 2.0f);
			count += 1.0f;
			if (cost < other_cost)
			{
				other_cost = cost;
				other = v;
			}
		}
	}
	if (from_answer)
	{
		const vec2 one = 1.0f / vec2(textureSize(samp1, 0));
		vec2 tries[5];
		tries[0] = vec2(0.0f);
		tries[1] = textureLod(samp1, uv + vec2(one.x, 0.0f), 0.0f).xy;
		tries[2] = textureLod(samp1, uv - vec2(one.x, 0.0f), 0.0f).xy;
		tries[3] = textureLod(samp1, uv + vec2(0.0f, one.y), 0.0f).xy;
		tries[4] = textureLod(samp1, uv - vec2(0.0f, one.y), 0.0f).xy;
		for (int i = 0; i < 5; i++)
		{
			const float cost = Cost(uv, tries[i]);
			if (cost < other_cost)
			{
				other_cost = cost;
				other = tries[i];
			}
		}
	}
	// (Also when searched from nothing, where the start is standing still: measured on the test
	// scenes, the camera's move comes out truer when blocks do not jump to a far move lightly.)
	if (other_cost < 0.6f * best_cost - 0.004f)
	{
		best = other;
		best_cost = Settle(uv, 2, best);
	}

	float brighter;
	const float fit = Cost(uv, best, brighter);
	// Sure: this move fits much better than the moves around it do on the whole. (After the last
	// search that is no longer asked; the fourth number is then how much brighter the older
	// picture is here.)
	const float mean = sum / max(count, 1.0f);
	o_col0 = vec4(best, fit, last ? brighter : smoothstep(0.2f, 0.6f, (mean - fit) / (mean + 0.004f)));
}

#elif FG_PASS == 3

void main()
{
	// The mean of all blocks' moves, each as loud as it is sure. The second run leaves out the
	// blocks that are more than two pixels from the first run's mean: what moves on its own in
	// front of the picture, and blocks that matched a repeat of their pattern.
	const ivec2 size = textureSize(samp0, 0);
	const vec2 first = textureLod(samp1, vec2(0.5f), 0.0f).xy;
	vec3 sum = vec3(0.0f);
	for (int y = 0; y < size.y; y++)
	{
		for (int x = 0; x < size.x; x++)
		{
			const vec4 move = texelFetch(samp0, ivec2(x, y), 0);
			float loud = move.w;
			if (u_params.y > 0.5f)
				loud *= 1.0f - smoothstep(1.5f, 2.5f, length((move.xy - first) * vec2(size)));
			sum += vec3(move.xy, 1.0f) * loud;
		}
	}
	// (No sure block at all, or none near the first mean: the picture is taken to stand still.)
	o_col0 = (sum.z > 1e-3f) ? vec4(sum.xy / sum.z, 0.0f, 1.0f) : vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

#elif FG_PASS == 4

void main()
{
	// What the blocks up to three away say, each as loud as it is sure; this block keeps its own
	// answer as far as it is sure itself.
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	const vec4 own = textureLod(samp0, uv, 0.0f);
	vec3 sum = vec3(0.0f);
	for (int y = -3; y <= 3; y++)
	{
		for (int x = -3; x <= 3; x++)
		{
			const vec4 other = textureLod(samp0, uv + vec2(x, y) * u_source.zw, 0.0f);
			const float loud = other.w * other.w / (1.0f + float(x * x + y * y));
			sum += vec3(other.xy, 1.0f) * loud;
		}
	}
	const vec2 around = (sum.z > 1e-4f) ? sum.xy / sum.z : own.xy;
	// (What was taken from others is passed on in the next round, a little less loudly.)
	o_col0 = vec4(mix(around, own.xy, own.w), own.z, max(own.w, 0.8f * min(sum.z * 4.0f, 1.0f) * step(1e-4f, sum.z)));
}

#elif FG_PASS == 5

void main()
{
	// The move, of this block's and its eight neighbours', that is nearest to all the others: one
	// block with an answer of its own among neighbours that agree is taken to be wrong.
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	vec4 moves[9];
	for (int i = 0; i < 9; i++)
		moves[i] = textureLod(samp0, uv + vec2(i % 3 - 1, i / 3 - 1) * u_source.zw, 0.0f);
	int best = 4;
	float best_sum = 1e9f;
	for (int i = 0; i < 9; i++)
	{
		float sum = 0.0f;
		for (int j = 0; j < 9; j++)
			sum += length((moves[i].xy - moves[j].xy) * u_source.xy);
		// (This block's own answer wins a tie.)
		sum -= (i == 4) ? 0.25f : 0.0f;
		if (sum < best_sum)
		{
			best_sum = sum;
			best = i;
		}
	}
	o_col0 = vec4(moves[best].xy, max(moves[best].z, moves[4].z), moves[best].w);
}

#elif FG_PASS == 6

void main()
{
	// Two numbers for the whole picture, from 32 x 32 places over it: how much of it found no
	// match, and how much of it moves wildly (a block whose move, as it was found, is more than
	// a pixel or two from its four neighbours' on average). Things that really move do it
	// together; after a cut to another scene every block finds something, somewhere.
	const vec2 one = 1.0f / vec2(textureSize(samp1, 0));
	vec2 sum = vec2(0.0f);
	for (int y = 0; y < 32; y++)
	{
		for (int x = 0; x < 32; x++)
		{
			const vec2 at = (vec2(x, y) + 0.5f) * (1.0f / 32.0f);
			const vec2 own = textureLod(samp1, at, 0.0f).xy;
			float away = 0.0f;
			away += length((textureLod(samp1, at + vec2(one.x, 0.0f), 0.0f).xy - own) / one);
			away += length((textureLod(samp1, at - vec2(one.x, 0.0f), 0.0f).xy - own) / one);
			away += length((textureLod(samp1, at + vec2(0.0f, one.y), 0.0f).xy - own) / one);
			away += length((textureLod(samp1, at - vec2(0.0f, one.y), 0.0f).xy - own) / one);
			sum += vec2(smoothstep(MATCH_GOOD, MATCH_BAD, textureLod(samp0, at, 0.0f).z), smoothstep(1.0f, 2.5f, away * 0.25f));
		}
	}
	o_col0 = vec4(sum * (1.0f / 1024.0f), 0.0f, 1.0f);
}

#elif FG_PASS == 8

void main()
{
	// A 5 x 5 tent of taps between pixels (the mean of 2 x 2 each): soft enough that the small
	// picture, stretched over the screen, is light and colour and no shapes.
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	vec3 sum = vec3(0.0f);
	float weight = 0.0f;
	for (int y = -2; y <= 2; y++)
	{
		for (int x = -2; x <= 2; x++)
		{
			const float w = float(3 - abs(x)) * float(3 - abs(y));
			sum += textureLod(samp0, uv + vec2(x, y) * 1.5f * u_source.zw, 0.0f).rgb * w;
			weight += w;
		}
	}
	o_col0 = vec4(sum / weight * u_params.x, 1.0f);
}

#else

// More than this much of the picture without a match, or moving wildly: another scene.
const float CUT_NO_MATCH = 0.5f;
const float CUT_WILD = 0.35f;

void main()
{
	const vec2 uv = gl_FragCoord.xy * u_target.zw;
	const float t = u_params.x;
	const vec3 older = textureLod(samp0, uv, 0.0f).rgb;

	// Another scene: nothing between the two can be made up, the older picture stays.
	const vec2 whole = textureLod(samp3, vec2(0.5f), 0.0f).xy;
	if (whole.x > CUT_NO_MATCH || whole.y > CUT_WILD)
	{
		o_col0 = vec4(older, 1.0f);
		return;
	}

	const vec3 newer = textureLod(samp1, uv, 0.0f).rgb;
	const vec4 move = textureLod(samp2, uv, 0.0f);
	const vec2 from = uv - move.xy * t, to = uv + move.xy * (1.0f - t);
	const vec3 was = textureLod(samp0, from, 0.0f).rgb;
	const vec3 will = textureLod(samp1, to, 0.0f).rgb;
	// (move.w: how much brighter the older picture is around here, of itself.)
	const bool from_inside = (min(from.x, from.y) >= 0.0f && max(from.x, from.y) <= 1.0f);
	const bool to_inside = (min(to.x, to.y) >= 0.0f && max(to.x, to.y) <= 1.0f);
	vec3 moved;
	float trust = 1.0f - smoothstep(MATCH_GOOD, MATCH_BAD, move.z);
	if (from_inside && to_inside)
	{
		// Where the two ends of the move show the same thing, that thing is put between them.
		// Where they do not (something was uncovered, or the search went wrong), the pictures
		// are mixed in place: a faint double edge, and never something that was in neither.
		const vec3 apart = abs(was - will - vec3(move.w));
		trust *= 1.0f - smoothstep(0.08f, 0.25f, max(apart.r, max(apart.g, apart.b)));
		moved = mix(was, will, t);
	}
	else
	{
		// What has just come into the picture, or is about to leave it, is in one of the two only.
		moved = from_inside ? (was - vec3(move.w) * t) : (will + vec3(move.w) * (1.0f - t));
		trust *= (from_inside || to_inside) ? 1.0f : 0.0f;
	}
	o_col0 = vec4(mix(mix(older, newer, t), moved, trust), 1.0f);
}

#endif

#endif
