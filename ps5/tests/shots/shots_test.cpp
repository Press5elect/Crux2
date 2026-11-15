// Crux2 - its own screenshots (frontend/ps5_shots.h) on the PC: the names, the folder, and the
// picture of play made smaller with its shape kept.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_shots.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int s_failures = 0;
#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

int main()
{
	using namespace PS5Shots;
	setenv("TZ", "UTC", 1);
	tzset();
	CHECK(Name(1763200800, 1) == "2025-11-15_10-00-00");
	CHECK(Name(1763200800, 2) == "2025-11-15_10-00-00_2");
	CHECK(FolderName("SLUS-20312") == "SLUS-20312");
	CHECK(FolderName("") == "BIOS");
	CHECK(FolderName("../x y") == "___x_y");
	CHECK(std::strstr(DEFAULT_BINDING, "SDL-0/Start") != nullptr && std::strstr(DEFAULT_BINDING, "SDL-0/FaceWest") != nullptr);

	// 1280x896 of two halves: red on the left, blue on the right, alpha 0 (as a game may leave it).
	const uint32_t w = 1280, h = 896;
	std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			pixels[static_cast<size_t>(y) * w + x] = (x < w / 2) ? 0x000000ffu : 0x00ff0000u;
	uint32_t ow = 0, oh = 0;
	const std::vector<uint32_t> small = Smaller(pixels, w, h, 640, &ow, &oh);
	CHECK(ow == 640 && oh == 448);
	CHECK(small.size() == static_cast<size_t>(ow) * oh);
	CHECK(small[0] == 0xff0000ffu);
	CHECK(small[static_cast<size_t>(447) * 640 + 639] == 0xffff0000u);
	CHECK(small[319] == 0xff0000ffu && small[320] == 0xffff0000u);

	// An odd size: every pixel covered, the shape kept to the nearest line.
	const std::vector<uint32_t> odd(static_cast<size_t>(1001) * 3, 0x80808080u);
	const std::vector<uint32_t> odd_small = Smaller(odd, 1001, 3, 640, &ow, &oh);
	CHECK(ow == 640 && oh == 2);
	CHECK(odd_small[0] == 0xff808080u);

	// Never larger, and a picture with too few pixels is given back as it is.
	const std::vector<uint32_t> same = Smaller(pixels, w, h, 2000, &ow, &oh);
	CHECK(ow == w && oh == h && same.size() == pixels.size());
	const std::vector<uint32_t> none = Smaller(std::vector<uint32_t>(4), 1280, 720, 640, &ow, &oh);
	CHECK(ow == 1280 && none.size() == 4);

	if (s_failures == 0)
		std::printf("screenshots: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
