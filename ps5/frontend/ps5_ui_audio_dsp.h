// Crux2 - the interface's sound: reading sound files, bringing them to the output's rate, and
// making the start-up sound.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Plain C++ with no PCSX2 and no console in it: ps5/tests/uiaudio builds it for the PC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PS5UIAudio
{
using s16 = std::int16_t;

namespace DSP
{
	/// The rate of the console's main output, and so of everything that is mixed.
	constexpr unsigned OUTPUT_RATE = 48000;

	/// A sound in memory: 16-bit stereo at the file's own rate. It is kept in blocks, so that a
	/// long one needs no single huge allocation and nothing is copied while it grows.
	struct Sound
	{
		static constexpr unsigned BLOCK_SHIFT = 16;
		static constexpr std::size_t BLOCK_FRAMES = std::size_t(1) << BLOCK_SHIFT; // 256 KiB

		unsigned rate = 0;
		/// Where the sound begins and how long it is: a lossy file's silent lead-in and tail are
		/// left out by moving these, not the memory.
		std::uint64_t first = 0;
		std::uint64_t frames = 0;
		std::uint64_t stored = 0;
		std::vector<std::unique_ptr<s16[]>> blocks;

		/// Adds frames at the end (left, right, left...). False when memory ran out.
		bool append(const s16* stereo, std::size_t count);

		/// One frame (two values); index 0 is the sound's first frame.
		const s16* frame(std::uint64_t index) const
		{
			const std::uint64_t at = first + index;
			return &blocks[at >> BLOCK_SHIFT][(at & (BLOCK_FRAMES - 1)) * 2];
		}

		std::size_t bytes() const { return blocks.size() * BLOCK_FRAMES * 2 * sizeof(s16); }
	};

	/// What a decoder hands the sound to, as it decodes it.
	class FrameReceiver
	{
	public:
		/// Which channel is which in a file with more than two: WAV and FLAC agree, Vorbis has
		/// its own order.
		enum class Order
		{
			Wav,
			Vorbis
		};

		virtual ~FrameReceiver() = default;
		/// Once, before any frame. False: this cannot be played (the decoder then stops).
		virtual bool begin(unsigned rate, unsigned channels, Order order) = 0;
		/// Interleaved samples, nominally -1..1. False: enough (the decoder then stops).
		virtual bool frames(const float* interleaved, std::size_t count) = 0;
	};

	/// The decoders of ps5_ui_audio_decoders.cpp (dr_mp3, stb_vorbis, dr_flac). Each reads the
	/// file with fopen/fread, names what it found in `format`, and returns false with a reason
	/// when the file is not what its name says.
	bool DecodeMp3File(const char* path, FrameReceiver& to, std::string* format, std::string* error);
	bool DecodeVorbisFile(const char* path, FrameReceiver& to, std::string* format, std::string* error);
	bool DecodeFlacFile(const char* path, FrameReceiver& to, std::string* format, std::string* error);
	/// RIFF WAVE: PCM of 8, 16, 24 or 32 bits and floats of 32 or 64 bits, any rate, 1 to 32
	/// channels. A file that ends before its header says it does gives what is there.
	bool DecodeWavFile(const char* path, FrameReceiver& to, std::string* format, std::string* error);

	struct FileInfo
	{
		std::string format; // "WAV PCM 16-bit", "MP3", "Ogg Vorbis", "FLAC 24-bit"
		unsigned rate = 0;
		unsigned channels = 0;
		double seconds = 0.0; // of what is in memory
		bool lossy = false; // MP3, Vorbis: the file's ends do not meet exactly
		bool cut = false; // longer than the limit: only its beginning is in memory
	};

	/// No more of a file than this is read: twelve minutes of any of the formats fit.
	constexpr std::size_t MAX_FILE_BYTES = std::size_t(256) << 20;

	/// A whole file into memory (from the heap), for the decoders, which read from memory. Of a
	/// file larger than max_bytes only that much is read, and `whole` says so.
	bool ReadFile(const char* path, std::size_t max_bytes, std::unique_ptr<unsigned char[]>* data, std::size_t* size, bool* whole,
		std::string* error);

	/// Whether there is a file of this name that can be opened.
	bool FileExists(const char* path);

	/// Reads a sound file (by its extension: .wav, .mp3, .ogg, .flac) into memory as 16-bit
	/// stereo at its own rate; more than two channels are mixed down. At most max_seconds of it
	/// are kept (and no more memory than that long a sound at the output's rate takes).
	bool LoadSound(const char* path, double max_seconds, Sound* sound, FileInfo* info, std::string* error);

	/// A windowed-sinc low-pass for going from one rate to another: for a position between two
	/// input frames, the weights of the input frames around it.
	class SincKernel
	{
	public:
		SincKernel(double from_rate, double to_rate);

		/// How many input frames one output frame is made of (even). The frames are those from
		/// floor(position) - taps/2 + 1 to floor(position) + taps/2.
		unsigned taps() const { return m_taps; }
		/// Input frames per output frame.
		double step() const { return m_step; }
		/// The weights for a position `fraction` (0..1) past an input frame; they add up to 1.
		void weights(double fraction, float* out) const;

	private:
		static constexpr unsigned PHASES = 256;
		unsigned m_taps;
		double m_step;
		std::vector<float> m_table; // (PHASES + 1) rows of m_taps
	};

	/// One channel from one rate to another, with silence assumed before and after it.
	std::vector<float> ResampleMono(const std::vector<float>& in, double from_rate, double to_rate);

	/// Plays a sound round and round at the output's rate. A sound whose ends meet (WAV, FLAC)
	/// goes from its last frame to its first with nothing in between, whatever its rate; one
	/// whose ends do not (MP3, Vorbis, or a sound cut at the limit) has its end faded into its
	/// beginning.
	class LoopPlayer
	{
	public:
		LoopPlayer(std::shared_ptr<const Sound> sound, bool crossfade);

		const std::shared_ptr<const Sound>& sound() const { return m_sound; }
		/// The next `count` frames (left, right, ...; -1..1).
		void read(float* stereo, std::size_t count);
		/// Where it is, in seconds from the sound's beginning.
		double seconds() const { return m_position / static_cast<double>(m_sound->rate); }
		/// How long one round is, in seconds.
		double period() const { return static_cast<double>(m_period) / static_cast<double>(m_sound->rate); }
		double crossfadeSeconds() const { return static_cast<double>(m_fade) / static_cast<double>(m_sound->rate); }
		/// Goes to `seconds` from the sound's beginning (taken round: a place past its end is in
		/// a later round). For music that is read again after a game, from where it was.
		void seek(double seconds);

	private:
		void fetch(std::int64_t first, std::size_t count, float* stereo) const;

		std::shared_ptr<const Sound> m_sound;
		std::uint64_t m_period; // frames of the sound in one round
		std::uint64_t m_fade; // of them, those in which the end is faded into the beginning
		double m_position = 0.0; // in frames of the sound
		std::unique_ptr<SincKernel> m_kernel; // none: the sound is at the output's rate
		std::vector<float> m_window, m_weights;
	};

	/// The parts of the start-up sound, for RenderStartup's `parts`.
	enum : unsigned
	{
		PART_BED = 1u << 0, // the user's own sound, treated
		PART_WHOOSHES = 1u << 1, // a letter flying in
		PART_IMPACTS = 1u << 2, // a letter landing: a thud and a bell
		PART_ICON = 1u << 3, // the icon coming down
		PART_TELEPORT = 1u << 4,
		PART_ALL = 0x1f,
	};

	/// When things happen in the start-up sound, in seconds of the animation's clock.
	constexpr int STARTUP_LETTERS = 7;
	constexpr double STARTUP_LETTER_START[STARTUP_LETTERS] = {0.30, 0.45, 0.60, 0.75, 0.90, 1.05, 1.20};
	constexpr double STARTUP_LETTER_FLIGHT = 0.60;
	constexpr double STARTUP_ICON_LANDS = 2.45;
	constexpr double STARTUP_TELEPORT = 3.70;
	/// The sound is this much behind the animation's clock, as in the approved mock-up.
	constexpr double STARTUP_OFFSET = 0.05;
	/// When the background sound comes in under the start-up sound's tail.
	constexpr double STARTUP_BACKGROUND_AT = 6.1;
	/// How much of a start-up sound is played, and over how much of that it fades out: the
	/// start-up animations have handed over to the library by 5.6 s.
	constexpr double STARTUP_PLAIN_SECONDS = 8.0;
	constexpr double STARTUP_PLAIN_FADE = 1.8;

	struct StartupInfo
	{
		double seconds = 0.0;
		double bed_seconds = 0.0; // 0: no bed in it
		float peak = 0.0f; // of the result, 0..1
		float gain = 0.0f; // what the mix was multiplied by after the limiter
	};

	/// The hall the bed is put in: both channels come back 2.4 s longer and hold the echo alone
	/// (the caller mixes it with the dry sound). Public for the tests.
	void Hall(std::vector<float>& left, std::vector<float>& right);

	/// Makes the start-up sound: 16-bit stereo at the output's rate, its first frame at the
	/// animation's t = 0. `bed` (may be null) is the user's own sound; the effects are made here.
	/// False when there is not the memory for it (some 25 MiB while it is made).
	bool RenderStartup(const Sound* bed, std::vector<s16>* stereo, StartupInfo* info, unsigned parts = PART_ALL);

	/// A start-up sound that is played as it is (the start-up animations are timed to the one the
	/// title comes with): 16-bit stereo at the output's rate, its first `seconds` at most with the
	/// last `fade` of them faded out when it is longer, its highest peak brought to the level the
	/// start-up sound has always had. False when there is nothing to hear in it.
	bool RenderPlain(const Sound& sound, double seconds, double fade, std::vector<s16>* stereo, StartupInfo* info);

	/// Keeps a sum of sounds inside -1..1 without the corners of a hard clip: what is below the
	/// knee passes unchanged.
	float SoftLimit(float sample);

} // namespace DSP
} // namespace PS5UIAudio
