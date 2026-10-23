// Crux2 - audio output through the console's AudioOut.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2's stream hands out frames on request (AudioStream::ReadFrames, which also does the time
// stretching); the console takes them in blocking writes of a fixed size, at 48 kHz, which is the
// rate the emulated sound chip produces. Each stream is therefore one AudioOut port and one
// thread: the thread reads one write's frames, converts them to 16-bit, and blocks in
// sceAudioOutOutput, which is what paces it. (A PlayStation 1 game, which PCSX2 runs at 44.1 kHz,
// is brought to 48 kHz by linear interpolation on the way.)
//
// The emulator selects its backend by name ("Cubeb" by default, "SDL"): both names give this
// stream on the PS5, so a settings file from another machine works.
//
// Two things can be asked of it in the settings, and both fall back to what every version before
// v0.8.0 did (stereo, 256 frames a write) when the console refuses them:
//
//   * [PS5] AudioGrain: how many frames one write holds, 256 to 2048 in steps of 256. More is
//     more delay and fewer wake-ups of the thread.
//   * [SPU2/Output] ExpansionMode: PCSX2's surround expansion (FreeSurround) makes more channels
//     out of the game's two. They go to a port of eight channels, in the order the console's
//     library is said to take them: left, right, centre, bass, the surround pair, the back pair.
//     Not for a 44.1 kHz stream. This has not been heard on a console.

#include "Host/AudioStream.h"
#include "ps5_priority.h"

#include "Host.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Threading.h"

#include "ps5_ui_audio.h" // Crux2 - the interface's sounds over the game (the quick menu)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
// No SDK header declares these; the signatures are the ones the PS5 Vulkan Template family of
// titles calls.
int sceAudioOutInit(void);
int sceAudioOutOpen(s32 user_id, s32 type, s32 index, u32 length, u32 frequency, u32 format);
int sceAudioOutClose(s32 handle);
int sceAudioOutOutput(s32 handle, const void* samples);
int sceKernelUsleep(u32 microseconds);
void ps5px_log(const char* format, ...);
}

namespace
{
	constexpr s32 AUDIO_OUT_USER_SYSTEM = 0xff; // the port belongs to no single user
	constexpr s32 AUDIO_OUT_PORT_MAIN = 0; // the main mix
	constexpr u32 AUDIO_OUT_FORMAT_S16_STEREO = 1;
	constexpr u32 AUDIO_OUT_FORMAT_S16_8CH = 2; // left, right, centre, bass, surround pair, back pair
	constexpr u32 AUDIO_OUT_RATE = 48000; // the only rate the main port takes
	constexpr u32 AUDIO_OUT_FRAMES = 256; // one write of the smallest size: 5.3 ms
	constexpr u32 AUDIO_OUT_FRAMES_MOST = 2048;
	constexpr u32 PORT_CHANNELS_MOST = 8;

	class PS5AudioStream final : public AudioStream
	{
	public:
		PS5AudioStream(u32 sample_rate, const AudioStreamParameters& parameters, s32 port, u32 grain, bool eight);
		~PS5AudioStream() override;

		void SetPaused(bool paused) override;

		bool Start(bool stretch_enabled, Error* error);

	private:
		// Where each channel of an expansion mode goes in a frame of eight (-1: nowhere), in the
		// order the mode's reader hands them out.
		struct Layout
		{
			SampleReader reader;
			s8 slot[PORT_CHANNELS_MOST];
		};
		static const Layout& LayoutOf(AudioExpansionMode mode);

		void Run();
		void ReadResampled(SampleType* frames);

		// For a stream at another rate than the port's: the two input frames around the read
		// position, the position between them, and the input block the frames are taken from.
		static constexpr u32 INPUT_BLOCK_FRAMES = 64;
		std::array<SampleType, INPUT_BLOCK_FRAMES * 2> m_input_block{};
		u32 m_input_position = INPUT_BLOCK_FRAMES;
		SampleType m_previous[2] = {};
		SampleType m_current[2] = {};
		double m_fraction = 1.0;

		s32 m_port = -1;
		u32 m_grain = AUDIO_OUT_FRAMES;
		bool m_eight = false; // the port takes frames of eight channels
		Threading::Thread m_thread;
		std::atomic_bool m_running{true};
		std::atomic_bool m_quit{false};
	};

	std::once_flag s_audio_out_once;

	// For test runs (ps5px_audio_counters): writes made while a game's sound was playing, and those
	// of them that started with less buffered than a write holds.
	std::atomic<unsigned long long> s_writes{0};
	std::atomic<unsigned long long> s_short_writes{0};
} // namespace

extern "C" void ps5px_audio_counters(unsigned long long* writes, unsigned long long* short_writes)
{
	*writes = s_writes.load(std::memory_order_relaxed);
	*short_writes = s_short_writes.load(std::memory_order_relaxed);
}

const PS5AudioStream::Layout& PS5AudioStream::LayoutOf(AudioExpansionMode mode)
{
	// (The readers hand the channels out in the order of a WAVE file, as PCSX2's other outputs
	// ask for them: front pair, centre, bass, rear pair, side pair.)
	static const std::array<Layout, static_cast<size_t>(AudioExpansionMode::Count)> layouts = {{
		// Disabled
		{&StereoSampleReaderImpl, {0, 1, -1, -1, -1, -1, -1, -1}},
		// StereoLFE: left, right, bass
		{&SampleReaderImpl<AudioExpansionMode::StereoLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_LFE>,
			{0, 1, 3, -1, -1, -1, -1, -1}},
		// Quadraphonic: left, right, rear pair
		{&SampleReaderImpl<AudioExpansionMode::Quadraphonic, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_REAR_LEFT,
			 READ_CHANNEL_REAR_RIGHT>,
			{0, 1, 4, 5, -1, -1, -1, -1}},
		// QuadraphonicLFE: left, right, bass, rear pair
		{&SampleReaderImpl<AudioExpansionMode::QuadraphonicLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_LFE,
			 READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
			{0, 1, 3, 4, 5, -1, -1, -1}},
		// Surround51: left, right, centre, bass, rear pair (which a 5.1 set plays as its surrounds)
		{&SampleReaderImpl<AudioExpansionMode::Surround51, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_FRONT_CENTER,
			 READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
			{0, 1, 2, 3, 4, 5, -1, -1}},
		// Surround71: left, right, centre, bass, rear pair, side pair: the sides are the port's
		// surround pair, the rears its back pair
		{&SampleReaderImpl<AudioExpansionMode::Surround71, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_FRONT_CENTER,
			 READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT, READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT>,
			{0, 1, 2, 3, 6, 7, 4, 5}},
	}};
	return layouts[std::min(static_cast<size_t>(mode), layouts.size() - 1)];
}

PS5AudioStream::PS5AudioStream(u32 sample_rate, const AudioStreamParameters& parameters, s32 port, u32 grain, bool eight)
	: AudioStream(sample_rate, parameters)
	, m_port(port)
	, m_grain(grain)
	, m_eight(eight)
{
}

PS5AudioStream::~PS5AudioStream()
{
	m_quit.store(true, std::memory_order_release);
	if (m_thread.Joinable())
		m_thread.Join();
	if (m_port >= 0)
		sceAudioOutClose(m_port);
}

bool PS5AudioStream::Start(bool stretch_enabled, Error* error)
{
	BaseInitialize(LayoutOf(m_parameters.expansion_mode).reader, stretch_enabled);

	if (!m_thread.Start([this]() { Run(); }))
	{
		Error::SetStringView(error, "The audio thread could not be started.");
		return false;
	}

	ps5px_log("audio: port %d, %u Hz in, %u Hz out, %u channel(s) of %u (%s), %u frames per write, time stretching %s", m_port, m_sample_rate,
		AUDIO_OUT_RATE, static_cast<u32>(m_output_channels), m_eight ? 8u : 2u, GetExpansionModeName(m_parameters.expansion_mode), m_grain,
		stretch_enabled ? "on" : "off");
	return true;
}

void PS5AudioStream::SetPaused(bool paused)
{
	if (m_paused == paused)
		return;

	m_running.store(!paused, std::memory_order_release);
	m_paused = paused;
}

// (Stereo only: a stream at another rate than the port's is never expanded.)
void PS5AudioStream::ReadResampled(SampleType* frames)
{
	const double step = static_cast<double>(m_sample_rate) / static_cast<double>(AUDIO_OUT_RATE);
	for (u32 i = 0; i < m_grain; i++)
	{
		while (m_fraction >= 1.0)
		{
			if (m_input_position == INPUT_BLOCK_FRAMES)
			{
				ReadFrames(m_input_block.data(), INPUT_BLOCK_FRAMES);
				m_input_position = 0;
			}

			m_previous[0] = m_current[0];
			m_previous[1] = m_current[1];
			m_current[0] = m_input_block[m_input_position * 2 + 0];
			m_current[1] = m_input_block[m_input_position * 2 + 1];
			m_input_position++;
			m_fraction -= 1.0;
		}

		const float t = static_cast<float>(m_fraction);
		frames[i * 2 + 0] = m_previous[0] + (m_current[0] - m_previous[0]) * t;
		frames[i * 2 + 1] = m_previous[1] + (m_current[1] - m_previous[1]) * t;
		m_fraction += step;
	}
}

void PS5AudioStream::Run()
{
	Threading::SetNameOfCurrentThread("PS5 Audio");
	PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);

	// (On the heap: a write of the largest size in eight channels is too much for this thread's stack.)
	const u32 in_channels = m_output_channels, out_channels = m_eight ? PORT_CHANNELS_MOST : 2;
	std::vector<SampleType> frames(static_cast<size_t>(m_grain) * std::max<u32>(in_channels, 2));
	std::vector<s16> output(static_cast<size_t>(m_grain) * out_channels);
	const Layout& layout = LayoutOf(m_parameters.expansion_mode);
	bool was_running = false;

	while (!m_quit.load(std::memory_order_acquire))
	{
		const bool running = m_running.load(std::memory_order_acquire);
		// Crux2 - the interface's sounds over the game's, while the quick menu is open (and a
		// moment after it closes): ps5_ui_audio.h. The menu pauses the game, so then silence is
		// written with them in it, where a paused stream would write nothing. One atomic read
		// when the menu is closed; MixIntoGame never waits for a lock.
		const bool interface_sounds = PS5UIAudio::GameMixInWanted();
		if (running || interface_sounds)
		{
			if (running)
			{
				s_writes.fetch_add(1, std::memory_order_relaxed);
				if (GetBufferedFramesRelaxed() < m_grain)
					s_short_writes.fetch_add(1, std::memory_order_relaxed);

				if (m_sample_rate == AUDIO_OUT_RATE)
					ReadFrames(frames.data(), m_grain);
				else
					ReadResampled(frames.data());
			}
			else
			{
				std::fill(frames.begin(), frames.end(), 0.0f);
			}
			// (The frames are at the port's rate by now, whatever the game's; left and right are
			// the first two of each frame in every layout.)
			if (interface_sounds)
				PS5UIAudio::MixIntoGame(frames.data(), m_grain, m_eight ? std::max<u32>(in_channels, 2) : 2, AUDIO_OUT_RATE);
			if (!m_eight)
			{
				for (u32 i = 0; i < m_grain * 2; i++)
				{
					const float sample = std::clamp(frames[i], -1.0f, 1.0f);
					output[i] = static_cast<s16>(std::lrintf(sample * 32767.0f));
				}
			}
			else
			{
				std::fill(output.begin(), output.end(), static_cast<s16>(0));
				for (u32 i = 0; i < m_grain; i++)
				{
					for (u32 c = 0; c < in_channels && c < PORT_CHANNELS_MOST; c++)
					{
						if (layout.slot[c] < 0)
							continue;
						const float sample = std::clamp(frames[i * in_channels + c], -1.0f, 1.0f);
						output[i * PORT_CHANNELS_MOST + static_cast<u32>(layout.slot[c])] = static_cast<s16>(std::lrintf(sample * 32767.0f));
					}
				}
			}
		}
		else
		{
			if (!was_running)
			{
				// Paused, and the silence that ends the sound is out: nothing to write.
				sceKernelUsleep(5000);
				continue;
			}
			std::fill(output.begin(), output.end(), static_cast<s16>(0));
		}
		was_running = running || interface_sounds;

		// Blocks until the port has taken the previous write: this is the stream's clock.
		if (sceAudioOutOutput(m_port, output.data()) < 0)
			sceKernelUsleep(5000);
	}
}

static std::unique_ptr<AudioStream> CreatePS5AudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	if (sample_rate < 8000 || sample_rate > 192000)
	{
		Error::SetStringFmt(error, "The PS5 audio output cannot play a {} Hz stream.", sample_rate);
		return {};
	}

	std::call_once(s_audio_out_once, []() {
		const int rc = sceAudioOutInit();
		ps5px_log("audio: sceAudioOutInit: 0x%x", static_cast<u32>(rc));
	});

	// What was asked for; then, where the console refuses it, what every version before did.
	AudioStreamParameters wanted = parameters;
	if (sample_rate != AUDIO_OUT_RATE)
		wanted.expansion_mode = AudioExpansionMode::Disabled;
	u32 grain = static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("PS5", "AudioGrain", static_cast<int>(AUDIO_OUT_FRAMES)),
		static_cast<int>(AUDIO_OUT_FRAMES), static_cast<int>(AUDIO_OUT_FRAMES_MOST)));
	grain = grain / AUDIO_OUT_FRAMES * AUDIO_OUT_FRAMES;
	bool eight = (wanted.expansion_mode != AudioExpansionMode::Disabled);

	s32 port = sceAudioOutOpen(AUDIO_OUT_USER_SYSTEM, AUDIO_OUT_PORT_MAIN, 0, grain, AUDIO_OUT_RATE,
		eight ? AUDIO_OUT_FORMAT_S16_8CH : AUDIO_OUT_FORMAT_S16_STEREO);
	if (port < 0 && (eight || grain != AUDIO_OUT_FRAMES))
	{
		ps5px_log("audio: the console refused a port of %u channels with %u frames per write (0x%x): stereo with %u is used", eight ? 8u : 2u,
			grain, static_cast<u32>(port), AUDIO_OUT_FRAMES);
		eight = false;
		grain = AUDIO_OUT_FRAMES;
		wanted.expansion_mode = AudioExpansionMode::Disabled;
		port = sceAudioOutOpen(AUDIO_OUT_USER_SYSTEM, AUDIO_OUT_PORT_MAIN, 0, grain, AUDIO_OUT_RATE, AUDIO_OUT_FORMAT_S16_STEREO);
	}
	if (port < 0)
	{
		Error::SetStringFmt(error, "sceAudioOutOpen failed: {:#x}", static_cast<u32>(port));
		return {};
	}

	std::unique_ptr<PS5AudioStream> stream = std::make_unique<PS5AudioStream>(sample_rate, wanted, port, grain, eight);
	if (!stream->Start(stretch_enabled, error))
		stream.reset();
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
	return CreatePS5AudioStream(sample_rate, parameters, stretch_enabled, error);
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	return CreatePS5AudioStream(sample_rate, parameters, stretch_enabled, error);
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	std::vector<std::pair<std::string, std::string>> names;
	names.emplace_back(std::string(), "PS5 AudioOut");
	return names;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver)
{
	std::vector<DeviceInfo> devices;
	devices.emplace_back(std::string(), "TV / AV receiver (main output)", AUDIO_OUT_FRAMES);
	return devices;
}
