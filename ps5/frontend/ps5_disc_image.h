// Crux2 - disc images in the formats PCSX2 plays from: ISO, CHD and CSO.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// No console in here: the PC tests make images with it and read them back with libchdr and a CSO
// reader of their own.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace PS5DiscImage
{
	enum class Format
	{
		Iso, ///< every sector as it is: the largest, read by everything
		ChdSmall, ///< CHD v5, LZMA: the smallest; every PCSX2 and MAME of the last years read it
		ChdFast, ///< CHD v5, Zstandard: a little larger, far quicker to read (PCSX2 2.x, MAME 0.263 on)
		Cso, ///< CSO v1, deflate in 2048-byte frames: PCSX2 and the PSP's tools read it
		Count,
	};

	/// ".iso", ".chd", ".cso".
	const char* Extension(Format format);
	/// "ISO", "CHD (smallest)", ...: for the interface.
	const char* Name(Format format);
	/// One sentence on what the format is good for.
	const char* Description(Format format);

	/// Bytes that are written after the rest (the header, which holds what is only known at the end).
	struct Patch
	{
		std::uint64_t offset = 0;
		std::vector<std::uint8_t> bytes;
	};

	/// Where the image's bytes go, in order. False stops the image.
	using Append = std::function<bool(const void* data, std::size_t size)>;

	/// Makes an image of `total` bytes (whole 2048-byte sectors) from the sectors given in order.
	/// The bytes go out through `append` as they are ready; Finish gives what must then be written
	/// over the start of the file. The compressed formats use `threads` threads.
	class Encoder
	{
	public:
		static std::unique_ptr<Encoder> Create(Format format, std::uint64_t total, unsigned threads, Append append);
		virtual ~Encoder() = default;

		/// `size` bytes of the disc, following those given before (any amount, whole sectors).
		virtual bool Add(const std::uint8_t* data, std::size_t size) = 0;
		/// After the last sector: the rest of the file goes out, and `patches` are to be written
		/// over it at their offsets. False when the image could not be finished.
		virtual bool Finish(std::vector<Patch>* patches) = 0;
		/// The file's size so far (all of it after Finish).
		virtual std::uint64_t Written() const = 0;
		/// A sentence when something went wrong.
		virtual std::string Error() const = 0;
	};

	// ---- pieces the tests check on their own

	std::uint16_t Crc16(const void* data, std::size_t size);

	class Sha1
	{
	public:
		Sha1();
		void Add(const void* data, std::size_t size);
		void Finish(std::uint8_t out[20]);

	private:
		void Block(const std::uint8_t* block);
		std::uint32_t m_h[5];
		std::uint8_t m_buffer[64];
		std::size_t m_used = 0;
		std::uint64_t m_bytes = 0;
	};
} // namespace PS5DiscImage
