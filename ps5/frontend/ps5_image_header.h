// Crux2 - a picture's size read from its header, before it is decoded.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 35e1dab (GPL-3.0-or-later): a huge picture is refused, not decoded until the title runs
// out of memory. Porpoise's loader (stb_image) has its own limit for that (STBI_MAX_DIMENSIONS 8192);
// PCSX2's (common/Image.cpp: libpng, libjpeg, libwebp, its own BMP reader) has none, and decodes the
// whole picture before anything looks at its size. A covers folder or a background picture of
// 20000 x 20000 pixels (1.6 GB once decoded) was enough. So RGBA8Image's loaders first read the size
// from the file's header here, and refuse a picture of more than 8192 pixels on a side.
//
// Header-only and without anything of PCSX2's, so that ps5/tests/image runs it on a PC. Every read
// goes through `read(offset, buffer, bytes)`, which answers false past the end: a short or broken
// header is "not known" (the decoder then says what it thinks of the file), never a read past it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace PS5ImageHeader
{
inline constexpr uint32_t MAX_SIDE = 8192;

enum class Kind
{
	Unknown,
	PNG,
	JPEG,
	WebP,
	BMP,
};

struct Size
{
	Kind kind = Kind::Unknown;
	uint32_t width = 0, height = 0;
	bool Known() const { return kind != Kind::Unknown && width > 0 && height > 0; }
	/// More than MAX_SIDE pixels on a side: refused before it is decoded.
	bool TooLarge() const { return Known() && (width > MAX_SIDE || height > MAX_SIDE); }
};

namespace detail
{
inline uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint32_t Le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
inline uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
inline uint32_t Le32(const uint8_t* p) { return Le24(p) | (uint32_t(p[3]) << 24); }

/// JPEG: the markers walked from SOI to the first start of frame (SOF0..SOF15 but DHT, JPG and DAC),
/// whose height and width follow its precision. A start of scan first, or too many markers: not known.
template <typename Reader>
inline bool Jpeg(Reader& read, Size& out)
{
	uint64_t at = 2;
	for (int markers = 0; markers < 4096; markers++)
	{
		uint8_t b = 0;
		if (!read(at, &b, 1))
			return false;
		if (b != 0xFF)
			return false;
		// (fill bytes: any number of 0xFF before the marker)
		int fill = 0;
		while (b == 0xFF)
		{
			if (++fill > 64 || !read(++at, &b, 1))
				return false;
		}
		const uint8_t marker = b;
		at++;
		if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
			continue; // no length
		if (marker == 0xD9 || marker == 0xDA)
			return false; // the end, or a scan, before any frame
		uint8_t len[2];
		if (!read(at, len, 2))
			return false;
		const uint32_t length = Be16(len);
		if (length < 2)
			return false;
		const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
		if (sof)
		{
			uint8_t frame[5];
			if (length < 7 || !read(at + 2, frame, 5))
				return false;
			out.kind = Kind::JPEG;
			out.height = Be16(frame + 1);
			out.width = Be16(frame + 3);
			return true;
		}
		at += length;
	}
	return false;
}
} // namespace detail

/// The size in the header read through `read(uint64_t offset, void* buffer, size_t bytes) -> bool`.
template <typename Reader>
inline Size Parse(Reader&& read)
{
	Size s;
	uint8_t h[32];
	std::memset(h, 0, sizeof(h));
	if (!read(0, h, 2))
		return s;
	if (h[0] == 0xFF && h[1] == 0xD8)
	{
		if (!detail::Jpeg(read, s))
			s = Size();
		return s;
	}
	if (h[0] == 'B' && h[1] == 'M')
	{
		if (!read(0, h, 26))
			return s;
		const int32_t w = static_cast<int32_t>(detail::Le32(h + 18)), hh = static_cast<int32_t>(detail::Le32(h + 22));
		s.kind = Kind::BMP;
		s.width = w < 0 ? 0u : static_cast<uint32_t>(w);
		// (a negative height: the rows top to bottom)
		s.height = hh < 0 ? (hh == INT32_MIN ? 0x80000000u : static_cast<uint32_t>(-hh)) : static_cast<uint32_t>(hh);
		return s;
	}
	static constexpr uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (read(0, h, 8) && std::memcmp(h, png, 8) == 0)
	{
		// The first chunk is IHDR: its length (13), its name, the width and the height.
		if (!read(0, h, 24) || std::memcmp(h + 12, "IHDR", 4) != 0)
			return s;
		s.kind = Kind::PNG;
		s.width = detail::Be32(h + 16);
		s.height = detail::Be32(h + 20);
		return s;
	}
	if (read(0, h, 12) && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WEBP", 4) == 0)
	{
		// (each kind's header read only as far as it goes: a small lossless file is 28 bytes up to its size)
		if (!read(0, h, 20))
			return s;
		if (std::memcmp(h + 12, "VP8X", 4) == 0)
		{
			if (!read(0, h, 30))
				return s;
			// The extended format: the canvas, less one, in 24 bits each after 4 bytes of flags.
			s.kind = Kind::WebP;
			s.width = detail::Le24(h + 24) + 1;
			s.height = detail::Le24(h + 27) + 1;
		}
		else if (std::memcmp(h + 12, "VP8L", 4) == 0)
		{
			// Lossless: a signature byte, then 14 bits of width less one and 14 of height less one.
			if (!read(0, h, 25) || h[20] != 0x2F)
				return s;
			const uint32_t bits = detail::Le32(h + 21);
			s.kind = Kind::WebP;
			s.width = (bits & 0x3FFF) + 1;
			s.height = ((bits >> 14) & 0x3FFF) + 1;
		}
		else if (std::memcmp(h + 12, "VP8 ", 4) == 0)
		{
			// Lossy: a 3-byte frame tag, the start code 9D 01 2A, then 14 bits of width and of height.
			if (!read(0, h, 30) || h[23] != 0x9D || h[24] != 0x01 || h[25] != 0x2A)
				return s;
			s.kind = Kind::WebP;
			s.width = detail::Le16(h + 26) & 0x3FFF;
			s.height = detail::Le16(h + 28) & 0x3FFF;
		}
		return s;
	}
	return s;
}

/// The size in a picture held in memory.
inline Size FromBuffer(const void* data, size_t size)
{
	const uint8_t* const bytes = static_cast<const uint8_t*>(data);
	return Parse([bytes, size](uint64_t offset, void* buffer, size_t n) {
		if (offset > size || n > size - offset)
			return false;
		std::memcpy(buffer, bytes + offset, n);
		return true;
	});
}

/// The size in a picture's file, from where `fp` is; `fp` is left where it was.
inline Size FromFile(std::FILE* fp)
{
	const long start = std::ftell(fp);
	if (start < 0)
		return Size();
	const Size s = Parse([fp, start](uint64_t offset, void* buffer, size_t n) {
		if (offset > static_cast<uint64_t>(0x7FFFFFFF) - static_cast<uint64_t>(start))
			return false;
		if (std::fseek(fp, start + static_cast<long>(offset), SEEK_SET) != 0)
			return false;
		return std::fread(buffer, 1, n, fp) == n;
	});
	std::fseek(fp, start, SEEK_SET); // (which also clears an end of file met on the way)
	return s;
}

inline const char* KindName(Kind k)
{
	switch (k)
	{
		case Kind::PNG: return "PNG";
		case Kind::JPEG: return "JPEG";
		case Kind::WebP: return "WebP";
		case Kind::BMP: return "BMP";
		default: return "picture";
	}
}
} // namespace PS5ImageHeader
