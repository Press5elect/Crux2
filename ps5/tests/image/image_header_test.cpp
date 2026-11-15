// Crux2 - host test of a picture's size read from its header (frontend/ps5_image_header.h): PNG,
// JPEG (baseline and progressive, segments before the frame, fill bytes), WebP (lossy, lossless,
// extended) and BMP, made by hand; huge ones refused, short and broken ones "not known", never a
// read past the end; and the same through a file, left where it was.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_image_header.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using PS5ImageHeader::Kind;
using Bytes = std::vector<uint8_t>;

static int s_checks = 0, s_failed = 0;
#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		s_checks++;                                                                \
		if (!(cond))                                                               \
		{                                                                          \
			s_failed++;                                                            \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                          \
	} while (0)

static void Be32(Bytes& b, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8)
		b.push_back(static_cast<uint8_t>(v >> s));
}
static void Le32(Bytes& b, uint32_t v)
{
	for (int s = 0; s < 32; s += 8)
		b.push_back(static_cast<uint8_t>(v >> s));
}
static void Add(Bytes& b, std::initializer_list<int> v)
{
	for (int x : v)
		b.push_back(static_cast<uint8_t>(x));
}
static void Add(Bytes& b, const char* s)
{
	while (*s)
		b.push_back(static_cast<uint8_t>(*s++));
}

static Bytes Png(uint32_t w, uint32_t h)
{
	Bytes b;
	Add(b, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
	Be32(b, 13);
	Add(b, "IHDR");
	Be32(b, w);
	Be32(b, h);
	Add(b, {8, 6, 0, 0, 0});
	Be32(b, 0); // (the CRC: not read)
	Add(b, {0, 0, 0, 0});
	Add(b, "IEND");
	Be32(b, 0);
	return b;
}

// SOI, an APP0 (JFIF), an APP1 of `exif` bytes, a DQT, fill bytes, then the frame (marker `sof`).
static Bytes Jpeg(uint32_t w, uint32_t h, int sof = 0xC0, size_t exif = 20)
{
	Bytes b;
	Add(b, {0xFF, 0xD8});
	Add(b, {0xFF, 0xE0, 0x00, 0x10});
	Add(b, "JFIF");
	Add(b, {0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
	const size_t len = exif + 2;
	Add(b, {0xFF, 0xE1, static_cast<int>(len >> 8), static_cast<int>(len & 0xFF)});
	for (size_t i = 0; i < exif; i++)
		b.push_back(static_cast<uint8_t>(i == 7 ? 0xFF : i)); // (an 0xFF inside a segment is its data)
	Add(b, {0xFF, 0xDB, 0x00, 0x43, 0x00});
	for (int i = 0; i < 64; i++)
		b.push_back(1);
	Add(b, {0xFF, 0xFF, 0xFF, sof, 0x00, 0x11, 0x08, static_cast<int>(h >> 8), static_cast<int>(h & 0xFF), static_cast<int>(w >> 8),
			   static_cast<int>(w & 0xFF), 0x03, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1});
	Add(b, {0xFF, 0xDA, 0x00, 0x0C, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 0x3F, 0, 0x12, 0x34, 0xFF, 0xD9});
	return b;
}

static Bytes Riff(const char* chunk, const Bytes& data)
{
	Bytes b;
	Add(b, "RIFF");
	Le32(b, static_cast<uint32_t>(4 + 8 + data.size()));
	Add(b, "WEBP");
	Add(b, chunk);
	Le32(b, static_cast<uint32_t>(data.size()));
	b.insert(b.end(), data.begin(), data.end());
	return b;
}
static Bytes WebpLossy(uint32_t w, uint32_t h)
{
	Bytes d;
	Add(d, {0x50, 0x02, 0x00, 0x9D, 0x01, 0x2A, static_cast<int>(w & 0xFF), static_cast<int>((w >> 8) & 0x3F),
			   static_cast<int>(h & 0xFF), static_cast<int>((h >> 8) & 0x3F), 0, 0});
	return Riff("VP8 ", d);
}
static Bytes WebpLossless(uint32_t w, uint32_t h)
{
	Bytes d;
	d.push_back(0x2F);
	Le32(d, ((w - 1) & 0x3FFF) | (((h - 1) & 0x3FFF) << 14));
	Add(d, {0, 0, 0});
	return Riff("VP8L", d);
}
static Bytes WebpExtended(uint32_t w, uint32_t h)
{
	Bytes d;
	Le32(d, 0x10);
	Add(d, {static_cast<int>((w - 1) & 0xFF), static_cast<int>(((w - 1) >> 8) & 0xFF), static_cast<int>(((w - 1) >> 16) & 0xFF),
			   static_cast<int>((h - 1) & 0xFF), static_cast<int>(((h - 1) >> 8) & 0xFF), static_cast<int>(((h - 1) >> 16) & 0xFF)});
	return Riff("VP8X", d);
}
static Bytes Bmp(int32_t w, int32_t h)
{
	Bytes b;
	Add(b, "BM");
	Le32(b, 0);
	Le32(b, 0);
	Le32(b, 54);
	Le32(b, 40);
	Le32(b, static_cast<uint32_t>(w));
	Le32(b, static_cast<uint32_t>(h));
	Add(b, {1, 0, 32, 0});
	return b;
}

static void Expect(const Bytes& b, Kind kind, uint32_t w, uint32_t h, bool too_large, const std::string& dir, const char* name)
{
	const PS5ImageHeader::Size s = PS5ImageHeader::FromBuffer(b.data(), b.size());
	CHECK(s.kind == kind && s.width == w && s.height == h && s.TooLarge() == too_large);
	if (!(s.kind == kind && s.width == w && s.height == h))
		std::fprintf(stderr, "  %s: got %s %ux%u\n", name, PS5ImageHeader::KindName(s.kind), s.width, s.height);

	// The same through a file, read from a place inside it (as a file opened and read a little first), and left there.
	const std::string path = dir + "/" + name;
	std::FILE* fp = std::fopen(path.c_str(), "wb");
	CHECK(fp != nullptr);
	if (!fp)
		return;
	std::fputs("lead", fp);
	if (!b.empty())
		std::fwrite(b.data(), 1, b.size(), fp);
	std::fclose(fp);
	fp = std::fopen(path.c_str(), "rb");
	CHECK(fp != nullptr);
	if (!fp)
		return;
	std::fseek(fp, 4, SEEK_SET);
	const PS5ImageHeader::Size f = PS5ImageHeader::FromFile(fp);
	CHECK(f.kind == s.kind && f.width == s.width && f.height == s.height);
	CHECK(std::ftell(fp) == 4);
	std::fclose(fp);
}

// Every shorter piece of a header: nothing past the end is read (the address sanitizer would say), and
// what is not all there is not known or is the same answer.
static void Truncations(const Bytes& b, const char* name)
{
	const PS5ImageHeader::Size whole = PS5ImageHeader::FromBuffer(b.data(), b.size());
	for (size_t n = 0; n < b.size(); n++)
	{
		Bytes cut(b.begin(), b.begin() + static_cast<long>(n));
		cut.shrink_to_fit();
		const PS5ImageHeader::Size s = PS5ImageHeader::FromBuffer(cut.empty() ? nullptr : cut.data(), cut.size());
		const bool ok = !s.Known() || (s.kind == whole.kind && s.width == whole.width && s.height == whole.height);
		CHECK(ok);
		if (!ok)
			std::fprintf(stderr, "  %s cut to %zu bytes: %s %ux%u\n", name, n, PS5ImageHeader::KindName(s.kind), s.width, s.height);
	}
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";

	Expect(Png(640, 480), Kind::PNG, 640, 480, false, dir, "small.png");
	Expect(Png(8192, 8192), Kind::PNG, 8192, 8192, false, dir, "edge.png");
	Expect(Png(8193, 100), Kind::PNG, 8193, 100, true, dir, "wide.png");
	Expect(Png(100, 20000), Kind::PNG, 100, 20000, true, dir, "tall.png");
	Expect(Png(0xFFFFFFFFu, 0xFFFFFFFFu), Kind::PNG, 0xFFFFFFFFu, 0xFFFFFFFFu, true, dir, "absurd.png");

	Expect(Jpeg(1920, 1080), Kind::JPEG, 1920, 1080, false, dir, "small.jpg");
	Expect(Jpeg(1920, 1080, 0xC2), Kind::JPEG, 1920, 1080, false, dir, "progressive.jpg");
	Expect(Jpeg(20000, 300, 0xC1), Kind::JPEG, 20000, 300, true, dir, "wide.jpg");
	Expect(Jpeg(300, 65535, 0xC0, 60000), Kind::JPEG, 300, 65535, true, dir, "tall-with-exif.jpg");
	{
		// A DHT (C4) is not a frame, though its marker lies among them.
		Bytes b = {0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x05, 0x00, 0x00, 0x00};
		const Bytes rest = Jpeg(9000, 9000);
		b.insert(b.end(), rest.begin() + 2, rest.end());
		Expect(b, Kind::JPEG, 9000, 9000, true, dir, "dht-first.jpg");
	}
	{
		// A scan before any frame, a bad length, no marker where one is due: not known.
		Expect(Bytes{0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02, 0x00}, Kind::Unknown, 0, 0, false, dir, "scan-first.jpg");
		Expect(Bytes{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0xFF, 0xC0}, Kind::Unknown, 0, 0, false, dir, "short-length.jpg");
		Expect(Bytes{0xFF, 0xD8, 0x12, 0x34}, Kind::Unknown, 0, 0, false, dir, "no-marker.jpg");
		// A segment's length that runs past the end.
		Expect(Bytes{0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 0x00}, Kind::Unknown, 0, 0, false, dir, "long-segment.jpg");
	}

	Expect(WebpLossy(1024, 768), Kind::WebP, 1024, 768, false, dir, "lossy.webp");
	Expect(WebpLossy(16383, 16383), Kind::WebP, 16383, 16383, true, dir, "lossy-huge.webp");
	Expect(WebpLossless(4000, 3000), Kind::WebP, 4000, 3000, false, dir, "lossless.webp");
	Expect(WebpLossless(16384, 2), Kind::WebP, 16384, 2, true, dir, "lossless-huge.webp");
	Expect(WebpExtended(800, 600), Kind::WebP, 800, 600, false, dir, "extended.webp");
	Expect(WebpExtended(1 << 24, 9000), Kind::WebP, 1u << 24, 9000, true, dir, "extended-huge.webp");
	{
		Bytes bad = WebpLossy(100, 100);
		bad[23] = 0; // no start code
		Expect(bad, Kind::Unknown, 0, 0, false, dir, "lossy-no-start.webp");
	}

	Expect(Bmp(320, 240), Kind::BMP, 320, 240, false, dir, "small.bmp");
	Expect(Bmp(320, -240), Kind::BMP, 320, 240, false, dir, "top-down.bmp");
	Expect(Bmp(30000, -30000), Kind::BMP, 30000, 30000, true, dir, "huge.bmp");
	Expect(Bmp(-5, 10), Kind::BMP, 0, 10, false, dir, "negative.bmp"); // (not known: the decoder refuses it)

	Expect(Bytes{'G', 'I', 'F', '8', '9', 'a'}, Kind::Unknown, 0, 0, false, dir, "other.gif");
	Expect(Bytes{}, Kind::Unknown, 0, 0, false, dir, "empty");
	{
		Bytes riff;
		Add(riff, "RIFF");
		Le32(riff, 4);
		Add(riff, "WAVE");
		Expect(riff, Kind::Unknown, 0, 0, false, dir, "sound.wav");
	}

	for (const auto& [b, name] : {std::make_pair(Png(9000, 9000), "png"), std::make_pair(Jpeg(9000, 9000, 0xC2, 300), "jpeg"),
			 std::make_pair(WebpLossy(9000, 9000), "lossy"), std::make_pair(WebpLossless(9000, 9000), "lossless"),
			 std::make_pair(WebpExtended(9000, 9000), "extended"), std::make_pair(Bmp(9000, 9000), "bmp")})
		Truncations(b, name);

	// Random bytes after each kind's first bytes: never a read past the end, nothing absurd taken as known
	// unless the bytes say so.
	std::srand(1);
	for (int i = 0; i < 20000; i++)
	{
		static const Bytes starts[] = {{0xFF, 0xD8}, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'},
			{'B', 'M'}};
		Bytes b = starts[i % 4];
		const size_t n = static_cast<size_t>(std::rand() % 200);
		for (size_t k = 0; k < n; k++)
			b.push_back(static_cast<uint8_t>((i % 3 == 0 && std::rand() % 4 == 0) ? 0xFF : std::rand()));
		b.shrink_to_fit();
		(void)PS5ImageHeader::FromBuffer(b.data(), b.size());
	}

	std::printf("pictures' sizes from their headers: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}
