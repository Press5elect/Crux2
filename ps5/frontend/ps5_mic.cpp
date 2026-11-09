// Crux2 - the console's microphones for SingStar (ps5_mic.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_mic.h"
#include "ps5_keyboard.h" // PS5SystemModules
#include "ps5_priority.h"

#include "common/Pcsx2Defs.h"
#include "Host.h"
#include "USB/usb-mic/audiodev.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

extern "C" {
int sceUserServiceGetInitialUser(int32_t* user_id);
int sceUserServiceGetLoginUserIdList(int32_t* user_ids);
void ps5px_log(const char* format, ...);
}

namespace PS5Mic
{
namespace
{
using OpenFn = int32_t (*)(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t frequency, uint32_t param);
using InputFn = int32_t (*)(int32_t handle, void* dest);
using CloseFn = int32_t (*)(int32_t handle);

struct Api
{
	OpenFn open = nullptr;
	InputFn input = nullptr;
	CloseFn close = nullptr;
	bool tried = false;
};

std::mutex s_api_mutex;
Api s_api;

const Api& Functions()
{
	const std::lock_guard lock(s_api_mutex);
	if (s_api.tried)
		return s_api;
	s_api.tried = true;
	const auto find = [] {
		s_api.open = reinterpret_cast<OpenFn>(PS5SystemModules::FindFunction("libSceAudioIn", "sceAudioInOpen", "5NE8Sjc7VC8"));
		s_api.input = reinterpret_cast<InputFn>(PS5SystemModules::FindFunction("libSceAudioIn", "sceAudioInInput", "LozEOU8+anM"));
		s_api.close = reinterpret_cast<CloseFn>(PS5SystemModules::FindFunction("libSceAudioIn", "sceAudioInClose", "Jh6WbHhnI68"));
	};
	// In a module the loader gave a handle for (it is not in the list a title sees).
	const auto find_in = [](int32_t handle) {
		s_api.open = reinterpret_cast<OpenFn>(PS5SystemModules::FindInHandle(handle, "sceAudioInOpen", "5NE8Sjc7VC8"));
		s_api.input = reinterpret_cast<InputFn>(PS5SystemModules::FindInHandle(handle, "sceAudioInInput", "LozEOU8+anM"));
		s_api.close = reinterpret_cast<CloseFn>(PS5SystemModules::FindInHandle(handle, "sceAudioInClose", "Jh6WbHhnI68"));
	};
	find();
	if (!s_api.open)
	{
		// Not in the process yet. First as the console's own loaders bring it in: by its internal ID
		// through the system module service (libSceAudioIn is 0x80000002, as PS5_PayloadSDK's
		// rtld_sprx.c names it); a title loading the file itself is refused (0x80020063).
		const int loaded = PS5SystemModules::LoadInternal(0x80000002u);
		if (loaded == INT32_MIN)
			ps5px_log("mic: the system module service's internal loader was not found");
		else
		{
			ps5px_log("mic: libSceAudioIn loaded by the system module service: %#x", static_cast<unsigned>(loaded));
			find();
		}
	}
	if (!s_api.open)
	{
		// Else loaded from the system's libraries.
		// (sceKernelLoadStartModule found in libkernel while the title runs, not imported: a title that
		// imports what the console does not give it does not start.)
		using LoadFn = int (*)(const char*, size_t, const void*, uint32_t, void*, int*);
		const auto load = reinterpret_cast<LoadFn>(PS5SystemModules::FindFunction("libkernel", "sceKernelLoadStartModule", "wzvqT4UqKX8"));
		if (load)
		{
			for (const char* path : {"/system/common/lib/libSceAudioIn.sprx", "/system/priv/lib/libSceAudioIn.sprx"})
			{
				int result = 0;
				const int handle = load(path, 0, nullptr, 0, nullptr, &result);
				ps5px_log("mic: %s loaded: %#x", path, static_cast<unsigned>(handle));
				if (handle >= 0)
					find_in(handle);
				if (!s_api.open)
					find();
				if (s_api.open)
					break;
			}
		}
		else
		{
			ps5px_log("mic: libSceAudioIn is not in the process, and the module loader was not found");
		}
	}
	ps5px_log("mic: libSceAudioIn's functions %s", (s_api.open && s_api.input && s_api.close) ? "found" : "NOT found: the microphones hear nothing");
	return s_api;
}

/// The user of player `player` (0 = the one who started the title), or -1.
int32_t UserOf(int player)
{
	int32_t initial = -1;
	if (sceUserServiceGetInitialUser(&initial) < 0)
		initial = -1;
	std::array<int32_t, 16> listed;
	listed.fill(-1);
	if (sceUserServiceGetLoginUserIdList(listed.data()) < 0)
		listed.fill(-1);
	std::vector<int32_t> users;
	if (initial >= 0)
		users.push_back(initial);
	for (int i = 0; i < 4; i++)
	{
		if (listed[i] >= 0 && listed[i] != initial)
			users.push_back(listed[i]);
	}
	return (player >= 0 && player < static_cast<int>(users.size())) ? users[static_cast<size_t>(player)] : -1;
}

/// What a title may ask libSceAudioIn for, tried in turn; the first that opens is used.
/// (type 1: general input at 48 kHz; type 0: voice at 16 kHz. The grain is 128 or 256 samples.)
struct Kind
{
	uint32_t type, frequency, length;
};
// (The shortest grain first: what the microphone gives arrives that much sooner.)
constexpr Kind KINDS[] = {{1, 48000, 128}, {1, 48000, 256}, {0, 16000, 128}, {0, 16000, 256}, {1, 16000, 256}};

// A microphone that would not open is tried again this often, so a controller switched on, a
// headset plugged in or a player signed in later is heard from then on.
constexpr auto RETRY = std::chrono::seconds(2);

class Microphone final : public AudioDevice
{
public:
	Microphone(AudioDir dir, u32 channels, int player, s32 latency)
		: AudioDevice(dir, channels)
		, m_player(player)
		, m_latency(std::max<s32>(latency, 20))
	{
	}
	~Microphone() override { Stop(); }

	// The emulated microphone is plugged in whether or not the console's opens now: a game that finds
	// no USB microphone says so and never looks again, and one that hears silence until the
	// microphone opens loses nothing. (Returning false here would leave the game without the
	// converter: PCSX2 then attaches no device at all.)
	bool Start() override
	{
		Stop();
		if (mAudioDir != AUDIODIR_SOURCE)
			return true; // (a headset's earpiece: what the game plays to it is not played)
		Functions();
		m_running = true;
		m_thread = std::thread([this] { Capture(); });
		return true;
	}

	void Stop() override
	{
		{
			const std::lock_guard lock(m_wake_mutex);
			m_running = false;
		}
		m_wake.notify_all();
		if (m_thread.joinable())
			m_thread.join();
		CloseHandle();
		const std::lock_guard lock(m_mutex);
		m_buffer.clear();
	}

	// The game takes a millisecond of samples at a time; the microphone gives a grain (2.7 to 16 ms)
	// at a time. A singing game wants the voice now, so the buffer holds as little as can be: after it
	// ran dry the game is given nothing until one grain is there (less, and the next millisecond is
	// a gap again: crackle), and it never holds more than two grains and a millisecond.
	bool GetFrames(uint32_t* size) override
	{
		const std::lock_guard lock(m_mutex);
		if (!m_primed && m_buffer.size() >= PrimeSamples())
			m_primed = true;
		*size = m_primed ? static_cast<uint32_t>(m_buffer.size()) : 0;
		return true;
	}

	uint32_t GetBuffer(int16_t* out, uint32_t frames) override
	{
		const std::lock_guard lock(m_mutex);
		if (!m_primed)
			return 0;
		const uint32_t n = std::min<uint32_t>(frames, static_cast<uint32_t>(m_buffer.size()));
		m_stats_out += n;
		if (n < frames)
		{
			m_primed = false;
			m_stats_dry++;
		}
		for (uint32_t i = 0; i < n; i++)
		{
			const int16_t sample = m_buffer.front();
			m_buffer.pop_front();
			for (uint32_t c = 0; c < mChannels; c++)
				out[i * mChannels + c] = sample;
		}
		return n;
	}

	uint32_t SetBuffer(int16_t*, uint32_t frames) override { return frames; }

	void SetResampling(int samplerate) override
	{
		const std::lock_guard lock(m_mutex);
		m_target = std::max(samplerate, 8000);
		m_buffer.clear();
		m_phase = 0.0;
		m_primed = false;
		ps5px_log("mic: player %d: the game takes %d Hz", m_player + 1, m_target);
	}

private:
	/// One round of attempts; true when a microphone opened. `say` logs every refusal.
	bool TryOpen(bool say)
	{
		const Api& api = s_api;
		if (!api.open || !api.input || !api.close)
			return false;
		const int32_t user = UserOf(m_player);
		if (say && user < 0)
			ps5px_log("mic: player %d is not signed in: the console's own microphone (user 255) is tried", m_player + 1);
		for (const int32_t who : {user, int32_t{255}})
		{
			if (who < 0)
				continue;
			for (const Kind& k : KINDS)
			{
				const int32_t handle = api.open(who, k.type, 0, k.length, k.frequency, 0);
				if (handle >= 0)
				{
					m_handle = handle;
					m_ever_opened = true;
					m_rate = k.frequency;
					m_length = k.length;
					ps5px_log("mic: player %d's microphone open (user %d, type %u, %u Hz, %u samples)", m_player + 1, who, k.type,
						k.frequency, k.length);
					return true;
				}
				if (say)
					ps5px_log("mic: player %d: open (user %d, type %u, %u Hz, %u samples) refused: %#x", m_player + 1, who, k.type,
						k.frequency, k.length, static_cast<unsigned>(handle));
			}
		}
		return false;
	}

	void CloseHandle()
	{
		if (m_handle >= 0 && s_api.close)
			s_api.close(m_handle);
		m_handle = -1;
	}

	/// Sleeps up to `time`, or less when the microphone is stopped. False when it was stopped.
	bool Wait(std::chrono::milliseconds time)
	{
		std::unique_lock lock(m_wake_mutex);
		return !m_wake.wait_for(lock, time, [this] { return !m_running.load(); });
	}

	void Capture()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
		bool first = true, told_missing = false;
		while (m_running)
		{
			if (m_handle < 0)
			{
				if (!TryOpen(first))
				{
					if (first && !told_missing)
					{
						told_missing = true;
						Host::AddOSDMessage(fmt::format("SingStar: player {}'s microphone is not available yet (controller off, or "
														"player not signed in). The game sees it; it is heard as soon as it opens.",
												m_player + 1),
							8.0f);
					}
					first = false;
					if (!Wait(RETRY))
						break;
					continue;
				}
				if (!first || told_missing)
					Host::AddOSDMessage(fmt::format("SingStar: player {}'s microphone is listening.", m_player + 1), 4.0f);
				first = false;
			}

			std::vector<int16_t> block(m_length);
			const auto asked = std::chrono::steady_clock::now();
			const int32_t got = s_api.input(m_handle, block.data());
			const double took_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();
			if (got < 0)
			{
				// Unplugged, the controller switched off, the user signed out: closed, and opened again
				// when it can be.
				ps5px_log("mic: player %d: input stopped: %#x; opened again when it can be", m_player + 1, static_cast<unsigned>(got));
				CloseHandle();
				if (!Wait(RETRY))
					break;
				continue;
			}
			// What a read gave: a count of samples, or 0. A read that waits for its grain and answers
			// 0 gave a whole grain; one that answers 0 at once had nothing yet, and its buffer is
			// last time's samples - passing those on again is what crackles.
			const double grain_ms = 1000.0 * static_cast<double>(m_length) / static_cast<double>(m_rate);
			size_t samples = 0;
			if (got > 0)
				samples = std::min<size_t>(static_cast<size_t>(got), m_length);
			else if (took_ms >= grain_ms * 0.4)
				samples = m_length;
			Note(got, took_ms, samples, block.data());
			if (samples == 0)
			{
				m_stats_empty++;
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
				continue;
			}

			// To the rate the game asked for, by straight lines between the samples.
			const std::lock_guard lock(m_mutex);
			const double step = static_cast<double>(m_rate) / static_cast<double>(m_target);
			// (m_phase counts source samples from the last one of the block before, m_last.)
			const double n = static_cast<double>(samples);
			const size_t before = m_buffer.size();
			while (m_phase < n)
			{
				const size_t i = static_cast<size_t>(m_phase);
				const int a = (i == 0) ? m_last : block[i - 1];
				const int b = block[i];
				const double t = m_phase - static_cast<double>(i);
				m_buffer.push_back(static_cast<int16_t>(a + (b - a) * t));
				m_phase += step;
			}
			m_phase -= n;
			m_last = block[samples - 1];
			m_stats_in += m_buffer.size() - before;
			// The microphone's clock and the game's differ a little: past two grains and a millisecond,
			// back to one grain in one cut (a click now and then, never a voice that falls behind).
			const size_t most = PrimeSamples() * 2 + static_cast<size_t>(m_target) / 1000;
			if (m_buffer.size() > most)
			{
				const size_t keep = PrimeSamples();
				m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_buffer.size() - keep));
				m_stats_cut++;
			}
		}
	}

	/// Every ten seconds a line: what the reads answered and how long they took, the samples in and
	/// out, how often the game found it dry, and how loud it was (what to look at when it crackles).
	void Note(int32_t got, double took_ms, size_t samples, const int16_t* block)
	{
		m_stats_reads++;
		m_stats_took_ms += took_ms;
		m_stats_got_min = std::min(m_stats_got_min, got);
		m_stats_got_max = std::max(m_stats_got_max, got);
		for (size_t i = 0; i < samples; i++)
			m_stats_peak = std::max(m_stats_peak, std::abs(static_cast<int>(block[i])));
		const auto now = std::chrono::steady_clock::now();
		if (m_stats_since.time_since_epoch().count() == 0)
			m_stats_since = now;
		const double seconds = std::chrono::duration<double>(now - m_stats_since).count();
		if (seconds < 10.0)
			return;
		ps5px_log("mic: %s", StatsLine(seconds).c_str());
		m_stats_since = now;
		m_stats_reads = m_stats_empty = m_stats_in = m_stats_cut = 0;
		m_stats_took_ms = 0.0;
		m_stats_got_min = INT32_MAX;
		m_stats_got_max = INT32_MIN;
		m_stats_peak = 0;
	}

public:
	/// What the reads did since the last line (and starts counting again for the next).
	std::string StatsLine(double seconds)
	{
		uint64_t out, dry;
		{
			const std::lock_guard lock(m_mutex);
			out = m_stats_out;
			dry = m_stats_dry;
			m_stats_out = m_stats_dry = 0;
		}
		seconds = std::max(seconds, 0.001);
		const uint64_t reads = std::max<uint64_t>(m_stats_reads, 1);
		return fmt::format("player {}: {} reads in {:.1f} s (answers {} to {}, {:.2f} ms each, a grain is {:.2f} ms; {} empty), {:.0f} "
						   "samples a second in and {:.0f} out at {} Hz, dry {} time(s), {} cut(s), peak {} of 32767",
			m_player + 1, m_stats_reads, seconds, m_stats_reads ? m_stats_got_min : 0, m_stats_reads ? m_stats_got_max : 0,
			m_stats_took_ms / static_cast<double>(reads), 1000.0 * m_length / m_rate, m_stats_empty,
			static_cast<double>(m_stats_in) / seconds, static_cast<double>(out) / seconds, m_target, dry, m_stats_cut, m_stats_peak);
	}

	/// Whether the console's microphone is open (for the test; read after Stop, or as a hint).
	bool WasOpened() const { return m_ever_opened.load(); }

private:
	std::atomic_bool m_ever_opened{false};

	/// One of the microphone's grains, at the game's rate (under m_mutex).
	size_t PrimeSamples() const
	{
		return std::max<size_t>(1, static_cast<size_t>(static_cast<uint64_t>(m_length) * static_cast<uint64_t>(m_target) / m_rate));
	}

	bool m_primed = false; // (under m_mutex)
	uint64_t m_stats_out = 0, m_stats_dry = 0; // (under m_mutex)
	// The capture thread's own:
	std::chrono::steady_clock::time_point m_stats_since{};
	uint64_t m_stats_reads = 0, m_stats_empty = 0, m_stats_in = 0, m_stats_cut = 0;
	double m_stats_took_ms = 0.0;
	int32_t m_stats_got_min = INT32_MAX, m_stats_got_max = INT32_MIN;
	int m_stats_peak = 0;

	int m_player;
	s32 m_latency;
	int32_t m_handle = -1; // (the capture thread's own while it runs)
	uint32_t m_rate = 48000, m_length = 256;
	int m_target = 48000;
	std::atomic_bool m_running{false};
	std::mutex m_wake_mutex;
	std::condition_variable m_wake;
	std::thread m_thread;
	std::mutex m_mutex;
	std::deque<int16_t> m_buffer;
	double m_phase = 0.0;
	int16_t m_last = 0;
};
} // namespace

// ---- the test (Settings > Controllers > SingStar > Test the microphone)

extern "C" {
int sceAudioOutOpen(int user_id, int type, int index, unsigned length, unsigned frequency, unsigned format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void* samples);
}

TestResult Record(int player, double seconds, const std::atomic_bool* cancel)
{
	TestResult r;
	Microphone mic(AUDIODIR_SOURCE, 1, std::clamp(player, 0, 3), 100);
	mic.Start();
	mic.SetResampling(TEST_RATE);
	const auto start = std::chrono::steady_clock::now();
	std::vector<int16_t> chunk(4096);
	while (!(cancel && cancel->load()))
	{
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (elapsed >= seconds)
			break;
		uint32_t frames = 0;
		if (mic.GetFrames(&frames) && frames > 0)
		{
			const uint32_t n = mic.GetBuffer(chunk.data(), std::min<uint32_t>(frames, static_cast<uint32_t>(chunk.size())));
			r.samples.insert(r.samples.end(), chunk.begin(), chunk.begin() + n);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	mic.Stop();
	r.opened = mic.WasOpened();
	r.seconds = took;
	r.details = mic.StatsLine(took);
	double sum = 0.0;
	for (const int16_t v : r.samples)
	{
		r.peak = std::max(r.peak, std::abs(static_cast<int>(v)));
		sum += static_cast<double>(v) * v;
	}
	r.rms = r.samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(r.samples.size()));
	ps5px_log("mic test: %s; %zu samples kept (%.0f%% of %.1f s), peak %d, loudness (RMS) %.0f", r.opened ? "opened" : "NOT opened",
		r.samples.size(), 100.0 * static_cast<double>(r.samples.size()) / (TEST_RATE * took), took, r.peak, r.rms);
	ps5px_log("mic test: %s", r.details.c_str());
	return r;
}

bool PlayBack(const std::vector<int16_t>& samples)
{
	constexpr unsigned GRAIN = 256;
	const int port = sceAudioOutOpen(0xff, 0, 0, GRAIN, TEST_RATE, 1 /* 16-bit stereo */);
	if (port < 0)
	{
		ps5px_log("mic test: the playback port did not open: %#x", static_cast<unsigned>(port));
		return false;
	}
	std::array<int16_t, GRAIN * 2> block;
	for (size_t at = 0; at < samples.size(); at += GRAIN)
	{
		for (unsigned i = 0; i < GRAIN; i++)
		{
			const int16_t v = (at + i < samples.size()) ? samples[at + i] : 0;
			block[2 * i] = block[2 * i + 1] = v;
		}
		if (sceAudioOutOutput(port, block.data()) < 0)
			break;
	}
	block.fill(0);
	sceAudioOutOutput(port, block.data());
	sceAudioOutOutput(port, nullptr); // (waits for what was given to be played)
	sceAudioOutClose(port);
	return true;
}

std::unique_ptr<AudioDevice> Create(int dir, uint32_t channels, const std::string& name, int32_t latency)
{
	if (name.size() != 9 || name.compare(0, 8, "ps5-mic-") != 0 || name[8] < '1' || name[8] > '4')
		return nullptr;
	return std::make_unique<Microphone>(static_cast<AudioDir>(dir), channels, name[8] - '1', latency);
}

std::vector<std::pair<std::string, std::string>> Devices()
{
	return {{"ps5-mic-1", "Player 1's microphone"}, {"ps5-mic-2", "Player 2's microphone"}, {"ps5-mic-3", "Player 3's microphone"},
		{"ps5-mic-4", "Player 4's microphone"}};
}

UsbSetup SetupFor(int choice)
{
	switch (choice)
	{
		case 1: return {"singstar", "ps5-mic-1", ""};
		case 2: return {"singstar", "ps5-mic-1", "ps5-mic-2"};
		default: return {"None", "", ""};
	}
}
} // namespace PS5Mic
