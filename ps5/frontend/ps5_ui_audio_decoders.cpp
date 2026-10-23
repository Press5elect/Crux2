// Crux2 - the interface's sound: MP3, Ogg Vorbis and FLAC files, for the background sound.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The three decoders are single-file libraries in third_party/ (dr_mp3 and dr_flac: public
// domain or MIT-0; stb_vorbis: public domain or MIT; third_party/SOURCES.txt says where from),
// compiled here and nowhere else, without their file functions: each file is read into memory
// with fopen/fread first (DSP::ReadFile), which is all the console's libc is asked for.
//
// A file is the user's own, put into the title folder by him: these decoders are not hardened
// against a file made to break them (stb_vorbis in particular), and are not given anything that
// comes over a network.

#include "ps5_ui_audio_dsp.h"

// (Everything the three include, first: stb_vorbis is included inside a namespace below.)
#include <cassert>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// stb_vorbis takes its scratch memory from the stack; a libc whose stdlib.h does not say what
// alloca is (it looks for alloca.h on Linux only) gets the compiler's.
#ifndef alloca
#define alloca __builtin_alloca
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DRMP3_API static
#define DRMP3_PRIVATE static
#include "third_party/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG // an .ogg is Vorbis here
#define DR_FLAC_NO_WCHAR
#define DRFLAC_API static
#define DRFLAC_PRIVATE static
#include "third_party/dr_flac.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
// Its functions are not static, and a title this size may come to hold another copy of it: here
// they are this file's own.
namespace
{
#include "third_party/stb_vorbis.c"
}
// (stb_vorbis leaves short names of its own defined.)
#undef L
#undef C
#undef R

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <memory>
#include <new>
#include <string>

namespace PS5UIAudio::DSP
{
namespace
{
	constexpr std::size_t CHUNK_FRAMES = 4096;

	struct Bytes
	{
		std::unique_ptr<unsigned char[]> data;
		std::size_t size = 0;
		bool whole = true;
	};

	// The scratch the decoded frames pass through, from the heap (the worker's stack is small).
	std::unique_ptr<float[]> Scratch(unsigned channels, std::string* error)
	{
		std::unique_ptr<float[]> scratch(new (std::nothrow) float[CHUNK_FRAMES * channels]);
		if (!scratch)
			*error = "no memory to decode it";
		return scratch;
	}
} // namespace

bool DecodeMp3File(const char* path, FrameReceiver& to, std::string* format, std::string* error)
{
	Bytes file;
	if (!ReadFile(path, MAX_FILE_BYTES, &file.data, &file.size, &file.whole, error))
		return false;

	// (On the heap: it holds a decoded frame and the decoder's state, some 20 KiB.)
	std::unique_ptr<drmp3> mp3(new (std::nothrow) drmp3);
	if (!mp3 || !drmp3_init_memory(mp3.get(), file.data.get(), file.size, nullptr))
	{
		*error = mp3 ? "no MP3 sound is found in it" : "no memory to decode it";
		return false;
	}

	const unsigned channels = mp3->channels, rate = mp3->sampleRate;
	*format = "MP3";
	bool ok = false;
	if (channels >= 1 && channels <= 2 && to.begin(rate, channels, FrameReceiver::Order::Wav))
	{
		if (std::unique_ptr<float[]> scratch = Scratch(channels, error))
		{
			std::uint64_t total = 0;
			for (;;)
			{
				const drmp3_uint64 got = drmp3_read_pcm_frames_f32(mp3.get(), CHUNK_FRAMES, scratch.get());
				if (got == 0)
					break;
				total += got;
				if (!to.frames(scratch.get(), static_cast<std::size_t>(got)))
					break;
			}
			ok = (total > 0);
			if (!ok)
				*error = "no MP3 sound is found in it";
		}
	}
	else if (channels < 1 || channels > 2)
	{
		*error = "an MP3 file of " + std::to_string(channels) + " channels";
	}

	drmp3_uninit(mp3.get());
	return ok;
}

bool DecodeFlacFile(const char* path, FrameReceiver& to, std::string* format, std::string* error)
{
	Bytes file;
	if (!ReadFile(path, MAX_FILE_BYTES, &file.data, &file.size, &file.whole, error))
		return false;

	drflac* const flac = drflac_open_memory(file.data.get(), file.size, nullptr);
	if (!flac)
	{
		*error = "not a FLAC file";
		return false;
	}

	const unsigned channels = flac->channels, rate = flac->sampleRate;
	*format = "FLAC " + std::to_string(flac->bitsPerSample) + "-bit";
	bool ok = false;
	if (channels >= 1 && channels <= 8 && to.begin(rate, channels, FrameReceiver::Order::Wav))
	{
		if (std::unique_ptr<float[]> scratch = Scratch(channels, error))
		{
			std::uint64_t total = 0;
			for (;;)
			{
				const drflac_uint64 got = drflac_read_pcm_frames_f32(flac, CHUNK_FRAMES, scratch.get());
				if (got == 0)
					break;
				total += got;
				if (!to.frames(scratch.get(), static_cast<std::size_t>(got)))
					break;
			}
			ok = (total > 0);
			if (!ok)
				*error = "there is no sound in it";
		}
	}
	else if (channels < 1 || channels > 8)
	{
		*error = "a FLAC file of " + std::to_string(channels) + " channels";
	}

	drflac_close(flac);
	return ok;
}

bool DecodeVorbisFile(const char* path, FrameReceiver& to, std::string* format, std::string* error)
{
	Bytes file;
	if (!ReadFile(path, MAX_FILE_BYTES, &file.data, &file.size, &file.whole, error))
		return false;

	int code = 0;
	stb_vorbis* const vorbis = stb_vorbis_open_memory(file.data.get(), static_cast<int>(file.size), &code, nullptr);
	if (!vorbis)
	{
		*error = "not an Ogg Vorbis file (the decoder's error " + std::to_string(code) + ")";
		return false;
	}

	const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
	const unsigned channels = (info.channels > 0) ? static_cast<unsigned>(info.channels) : 0;
	*format = "Ogg Vorbis";
	bool ok = false;
	if (channels >= 1 && channels <= 8 && to.begin(info.sample_rate, channels, FrameReceiver::Order::Vorbis))
	{
		if (std::unique_ptr<float[]> scratch = Scratch(channels, error))
		{
			std::uint64_t total = 0;
			for (;;)
			{
				const int got = stb_vorbis_get_samples_float_interleaved(vorbis, static_cast<int>(channels), scratch.get(),
					static_cast<int>(CHUNK_FRAMES * channels));
				if (got <= 0)
					break;
				total += static_cast<unsigned>(got);
				if (!to.frames(scratch.get(), static_cast<std::size_t>(got)))
					break;
			}
			ok = (total > 0);
			if (!ok)
				*error = "there is no sound in it";
		}
	}
	else if (channels < 1 || channels > 8)
	{
		*error = "an Ogg Vorbis file of " + std::to_string(channels) + " channels";
	}

	stb_vorbis_close(vorbis);
	return ok;
}

} // namespace PS5UIAudio::DSP
