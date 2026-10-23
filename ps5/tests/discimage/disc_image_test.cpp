// Crux2 - the disc image writer, checked on the PC: every format is made from a made-up disc and
// read back; CHD with libchdr (what PCSX2 reads it with), CSO with the reading PCSX2 does.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+

#include "ps5_disc_image.h"

#include <libchdr/chd.h>
#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace PS5DiscImage;

static int failures = 0;
#define CHECK(x)                                                              \
	do                                                                        \
	{                                                                         \
		if (!(x))                                                             \
		{                                                                     \
			std::printf("FAILED: %s (line %d)\n", #x, __LINE__);              \
			failures++;                                                       \
		}                                                                     \
	} while (0)

static std::string Hex(const std::uint8_t* p, int n)
{
	std::string s;
	char b[3];
	for (int i = 0; i < n; i++)
	{
		std::snprintf(b, sizeof(b), "%02x", p[i]);
		s += b;
	}
	return s;
}

// A disc of sorts: stretches of zeros, of text, of noise, of a repeated pattern.
static std::vector<std::uint8_t> MadeUpDisc(std::size_t sectors)
{
	std::vector<std::uint8_t> d(sectors * 2048);
	std::mt19937 rng(1234);
	for (std::size_t s = 0; s < sectors; s++)
	{
		std::uint8_t* p = d.data() + s * 2048;
		switch ((s / 37) % 4)
		{
			case 0:
				break; // zeros
			case 1:
				for (int i = 0; i < 2048; i++)
					p[i] = static_cast<std::uint8_t>("the quick brown fox jumps over the lazy dog "[(i + s) % 44]);
				break;
			case 2:
				for (int i = 0; i < 2048; i++)
					p[i] = static_cast<std::uint8_t>(rng());
				break;
			default:
				for (int i = 0; i < 2048; i++)
					p[i] = static_cast<std::uint8_t>((i * 7 + s) & 0x3f);
		}
	}
	return d;
}

struct Made
{
	std::vector<std::uint8_t> file;
	double seconds = 0;
};

static bool Make(Format format, const std::vector<std::uint8_t>& disc, Made* made, std::size_t piece)
{
	made->file.clear();
	const auto started = std::chrono::steady_clock::now();
	std::unique_ptr<Encoder> enc = Encoder::Create(format, disc.size(), 4, [&](const void* data, std::size_t size) {
		const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
		made->file.insert(made->file.end(), p, p + size);
		return true;
	});
	for (std::size_t at = 0; at < disc.size(); at += piece)
	{
		if (!enc->Add(disc.data() + at, std::min(piece, disc.size() - at)))
			return false;
	}
	std::vector<Patch> patches;
	if (!enc->Finish(&patches))
	{
		std::printf("finish: %s\n", enc->Error().c_str());
		return false;
	}
	CHECK(enc->Written() == made->file.size());
	for (const Patch& p : patches)
	{
		CHECK(p.offset + p.bytes.size() <= made->file.size());
		std::memcpy(made->file.data() + p.offset, p.bytes.data(), p.bytes.size());
	}
	made->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	return true;
}

static void CheckChd(const std::string& path, const std::vector<std::uint8_t>& disc)
{
	chd_file* chd = nullptr;
	const chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd);
	CHECK(err == CHDERR_NONE);
	if (err != CHDERR_NONE)
	{
		std::printf("chd_open: %s\n", chd_error_string(err));
		return;
	}
	const chd_header* h = chd_get_header(chd);
	CHECK(h->version == 5);
	CHECK(h->logicalbytes == disc.size());
	CHECK(h->unitbytes == 2048);
	CHECK(h->hunkbytes == 16384);
	// The raw SHA-1 is of the disc.
	{
		Sha1 s;
		s.Add(disc.data(), disc.size());
		std::uint8_t sha[20];
		s.Finish(sha);
		CHECK(std::memcmp(sha, h->rawsha1, 20) == 0);
	}
	// The DVD tag, as chdman writes it.
	{
		char meta[16];
		uint32_t length = 0, tag = 0;
		uint8_t flags = 0;
		const chd_error m = chd_get_metadata(chd, CHD_MAKE_TAG('D', 'V', 'D', ' '), 0, meta, sizeof(meta), &length, &tag, &flags);
		CHECK(m == CHDERR_NONE);
		CHECK(length == 1 && meta[0] == 0);
	}
	std::vector<std::uint8_t> hunk(h->hunkbytes);
	for (uint32_t i = 0; i < h->hunkcount; i++)
	{
		const chd_error r = chd_read(chd, i, hunk.data());
		if (r != CHDERR_NONE)
		{
			std::printf("chd_read %u: %s\n", i, chd_error_string(r));
			failures++;
			break;
		}
		const std::size_t start = static_cast<std::size_t>(i) * h->hunkbytes;
		const std::size_t real = std::min<std::size_t>(h->hunkbytes, disc.size() - start);
		if (std::memcmp(hunk.data(), disc.data() + start, real) != 0)
		{
			std::printf("hunk %u differs\n", i);
			failures++;
			break;
		}
	}
	chd_close(chd);
}

static std::uint32_t LE32(const std::uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// As PCSX2's CsoFileReader reads it.
static void CheckCso(const std::vector<std::uint8_t>& f, const std::vector<std::uint8_t>& disc)
{
	CHECK(std::memcmp(f.data(), "CISO", 4) == 0);
	CHECK(LE32(f.data() + 4) == 24);
	const std::uint64_t total = LE32(f.data() + 8) | (static_cast<std::uint64_t>(LE32(f.data() + 12)) << 32);
	CHECK(total == disc.size());
	const std::uint32_t frame = LE32(f.data() + 16);
	CHECK(frame == 2048);
	CHECK(f[20] == 1);
	const int align = f[21];
	const std::uint32_t frames = static_cast<std::uint32_t>((total + frame - 1) / frame);
	std::vector<std::uint8_t> out(frame);
	for (std::uint32_t i = 0; i < frames; i++)
	{
		const std::uint32_t a = LE32(f.data() + 24 + i * 4), b = LE32(f.data() + 24 + (i + 1) * 4);
		const std::uint64_t pos = static_cast<std::uint64_t>(a & 0x7fffffff) << align;
		const std::uint64_t size = static_cast<std::uint64_t>((b & 0x7fffffff) - (a & 0x7fffffff)) << align;
		if (pos + std::min<std::uint64_t>(size, frame) > f.size())
		{
			std::printf("frame %u beyond the file\n", i);
			failures++;
			return;
		}
		if (a & 0x80000000)
		{
			std::memcpy(out.data(), f.data() + pos, frame);
		}
		else
		{
			z_stream z = {};
			inflateInit2(&z, -15);
			z.next_in = const_cast<Bytef*>(f.data() + pos);
			z.avail_in = static_cast<uInt>(size);
			z.next_out = out.data();
			z.avail_out = frame;
			const int r = inflate(&z, Z_FINISH);
			inflateEnd(&z);
			if (r != Z_STREAM_END && !(r == Z_BUF_ERROR && z.avail_out == 0))
			{
				std::printf("frame %u: inflate %d\n", i, r);
				failures++;
				return;
			}
		}
		const std::size_t real = std::min<std::size_t>(frame, disc.size() - static_cast<std::size_t>(i) * frame);
		if (std::memcmp(out.data(), disc.data() + static_cast<std::size_t>(i) * frame, real) != 0)
		{
			std::printf("frame %u differs\n", i);
			failures++;
			return;
		}
	}
}

int main(int argc, char** argv)
{
	const std::string work = argc > 1 ? argv[1] : ".";

	// SHA-1 and CRC-16 against known answers.
	{
		Sha1 s;
		s.Add("abc", 3);
		std::uint8_t out[20];
		s.Finish(out);
		CHECK(Hex(out, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
		Sha1 l;
		const std::string million(1000000, 'a');
		l.Add(million.data(), 333);
		l.Add(million.data() + 333, million.size() - 333);
		l.Finish(out);
		CHECK(Hex(out, 20) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
		CHECK(Crc16("123456789", 9) == 0x29B1);
	}

	// A disc whose end is not a whole hunk, given in odd pieces.
	const std::vector<std::uint8_t> disc = MadeUpDisc(3001);
	for (int f = 0; f < static_cast<int>(Format::Count); f++)
	{
		const Format format = static_cast<Format>(f);
		Made made;
		CHECK(Make(format, disc, &made, 2048 * 77));
		const std::string path = work + "/image" + std::to_string(f) + Extension(format);
		FILE* fp = std::fopen(path.c_str(), "wb");
		std::fwrite(made.file.data(), 1, made.file.size(), fp);
		std::fclose(fp);
		std::printf("%-24s %8zu bytes of %zu (%.0f%%), %.2f s\n", Name(format), made.file.size(), disc.size(),
			100.0 * made.file.size() / disc.size(), made.seconds);
		if (format == Format::Iso)
			CHECK(made.file == disc);
		else if (format == Format::Cso)
			CheckCso(made.file, disc);
		else
			CheckChd(path, disc);
		// The same image whatever the pieces it was given in.
		Made again;
		CHECK(Make(format, disc, &again, 2048 * 3));
		CHECK(again.file == made.file);
	}

	// A disc of one sector.
	{
		const std::vector<std::uint8_t> one = MadeUpDisc(1);
		Made made;
		CHECK(Make(Format::ChdFast, one, &made, 2048));
		const std::string path = work + "/one.chd";
		FILE* fp = std::fopen(path.c_str(), "wb");
		std::fwrite(made.file.data(), 1, made.file.size(), fp);
		std::fclose(fp);
		CheckChd(path, one);
		Made cso;
		CHECK(Make(Format::Cso, one, &cso, 2048));
		CheckCso(cso.file, one);
	}

	std::printf(failures == 0 ? "disc images: all checks passed\n" : "disc images: %d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
