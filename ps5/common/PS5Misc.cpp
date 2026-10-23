// Crux2 - clocks, memory figures and the rest of common/HostSys.h, in place of
// common/Linux/LnxMisc.cpp.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "common/Console.h"
#include "common/HostSys.h"
#include "common/Pcsx2Types.h"
#include "common/Threading.h"

#include "PS5Timing.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
#include <memory>
#include <sched.h>
#include <thread>
#include <unistd.h>

#include <ps5platform/kernel.h>

extern "C" int sceKernelUsleep(unsigned microseconds);
extern "C" uint64_t ps5px_monotonic_ns(void); // frontend/ps5_clock.c


// The title's memory is the console's direct-memory pool: guest memory, the code area and the heap
// are all taken from it.
u64 GetPhysicalMemory()
{
	const int64_t size = sceKernelGetDirectMemorySize();
	return (size > 0) ? static_cast<u64>(size) : 0;
}

u64 GetAvailablePhysicalMemory()
{
	// The largest free run of the pool, which is what one more large allocation can have.
	int64_t start = 0;
	size_t available = 0;
	if (sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), PS5_KERNEL_DIRECT_ALIGNMENT, &start, &available) != 0)
		return 0;
	return available;
}

u64 GetTickFrequency()
{
	return 1000000000; // nanoseconds
}

// The title's monotonic clock: the processor's counter, not a system call (frontend/ps5_clock.h).
u64 GetCPUTicks()
{
	return ps5px_monotonic_ns();
}

std::string GetOSVersionString()
{
	return "PlayStation 5";
}

bool Common::InhibitScreensaver(bool inhibit)
{
	// The console dims its screen by its own rules; a title with a pad in use is not dimmed.
	return true;
}

void Common::SetMousePosition(int x, int y)
{
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
	return false;
}

void Common::DetachMousePositionCb()
{
}

// Achievement sounds (Settings > Achievements): a short 16-bit PCM WAV, played on a port of its own
// beside the game's, on a thread of its own. One at a time: a sound asked for while one plays is
// left out.
extern "C" {
int sceAudioOutOpen(int user_id, int type, int index, unsigned length, unsigned frequency, unsigned format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void* samples);
}

static std::atomic<bool> s_sound_playing{false};

bool Common::PlaySoundAsync(const char* path)
{
	std::FILE* file = std::fopen(path, "rb");
	if (!file)
		return false;
	std::vector<u8> data;
	{
		u8 chunk[16384];
		size_t got;
		while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && data.size() < (16u << 20))
			data.insert(data.end(), chunk, chunk + got);
		std::fclose(file);
	}
	// RIFF/WAVE: a "fmt " chunk (PCM, 16 bit) and a "data" chunk.
	if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
		return false;
	u32 rate = 0, channels = 0, bits = 0, format = 0;
	const u8* pcm = nullptr;
	size_t pcm_bytes = 0;
	for (size_t at = 12; at + 8 <= data.size();)
	{
		u32 size;
		std::memcpy(&size, data.data() + at + 4, 4);
		const u8* body = data.data() + at + 8;
		const size_t room = data.size() - at - 8;
		if (std::memcmp(data.data() + at, "fmt ", 4) == 0 && size >= 16 && room >= 16)
		{
			u16 f16, c16, b16;
			std::memcpy(&f16, body, 2);
			std::memcpy(&c16, body + 2, 2);
			std::memcpy(&rate, body + 4, 4);
			std::memcpy(&b16, body + 14, 2);
			format = f16;
			channels = c16;
			bits = b16;
		}
		else if (std::memcmp(data.data() + at, "data", 4) == 0)
		{
			pcm = body;
			pcm_bytes = std::min<size_t>(size, room);
		}
		at += 8 + size + (size & 1);
	}
	if (format != 1 || bits != 16 || channels < 1 || channels > 8 || rate < 8000 || !pcm || pcm_bytes < channels * 2u)
		return false;
	if (s_sound_playing.exchange(true))
		return false;

	// To 48 kHz stereo, by straight lines between the samples.
	const size_t frames = pcm_bytes / (2u * channels);
	std::vector<s16> in(frames * channels);
	std::memcpy(in.data(), pcm, in.size() * 2);
	const double step = static_cast<double>(rate) / 48000.0;
	auto out = std::make_shared<std::vector<s16>>();
	out->reserve(static_cast<size_t>(frames / step + 2) * 2);
	for (double pos = 0.0; pos < static_cast<double>(frames - 1); pos += step)
	{
		const size_t i = static_cast<size_t>(pos);
		const double t = pos - static_cast<double>(i);
		for (u32 c = 0; c < 2; c++)
		{
			const u32 from = (channels == 1) ? 0 : c;
			const int a = in[i * channels + from], b = in[(i + 1) * channels + from];
			out->push_back(static_cast<s16>(a + (b - a) * t));
		}
	}
	std::thread([out] {
		constexpr unsigned GRAIN = 256;
		const int port = sceAudioOutOpen(0xff, 0, 0, GRAIN, 48000, 1 /* 16-bit stereo */);
		if (port >= 0)
		{
			s16 block[GRAIN * 2];
			for (size_t at = 0; at < out->size(); at += GRAIN * 2)
			{
				const size_t n = std::min<size_t>(GRAIN * 2, out->size() - at);
				std::memcpy(block, out->data() + at, n * 2);
				std::memset(block + n, 0, (GRAIN * 2 - n) * 2);
				if (sceAudioOutOutput(port, block) < 0)
					break;
			}
			sceAudioOutOutput(port, nullptr); // (waits for what was given to be played)
			sceAudioOutClose(port);
		}
		s_sound_playing = false;
	}).detach();
	return true;
}

// What the start-up probe found (PS5Timing.h): the margin the frame limiter waits out awake, and
// which call sleeps.
static std::atomic<uint64_t> s_spin_margin_ns{PS5Timing::MARGIN_DEFAULT_NS};
static std::atomic<bool> s_sleep_with_usleep{false};

// One sleep with nanosleep. A sleep that a signal cut short goes on for what is left of it: a
// profiled test run interrupts its threads every two milliseconds (frontend/ps5_profiler.cpp), and
// a sleep that ended there would turn every wait into a busy one.
static bool NanosleepFor(u64 ns)
{
	const u64 until = GetCPUTicks() + ns;
	for (;;)
	{
		struct timespec ts;
		ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
		ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
		if (nanosleep(&ts, nullptr) == 0)
			return true;
		if (errno != EINTR)
			return false;
		const u64 now = GetCPUTicks();
		if (now >= until)
			return true;
		ns = until - now;
	}
}

// The same with sceKernelUsleep, which takes microseconds and answers with an error code.
static bool UsleepFor(u64 ns)
{
	const u64 until = GetCPUTicks() + ns;
	for (;;)
	{
		const u64 us = (ns + 999) / 1000;
		const int r = sceKernelUsleep(static_cast<unsigned>(std::min<u64>(us, 0xffffffffULL)));
		if (r == 0)
			return true;
		const u64 now = GetCPUTicks();
		if (now >= until)
			return true;
		// An interrupted sleep goes on; any other failure falls back to nanosleep for the rest.
		if (r != EINTR && r != static_cast<int>(0x80020004))
			return NanosleepFor(until - now);
		ns = until - now;
	}
}

static void SleepFor(u64 ns)
{
	if (s_sleep_with_usleep.load(std::memory_order_relaxed))
		UsleepFor(ns);
	else
		NanosleepFor(ns);
}

uint64_t PS5Timing::SpinMarginNs()
{
	return s_spin_margin_ns.load(std::memory_order_relaxed);
}

bool PS5Timing::SleepsWithUsleep()
{
	return s_sleep_with_usleep.load(std::memory_order_relaxed);
}

static void (*s_timing_report)(const char*) = nullptr;

void PS5Timing::StartProbe()
{
	static std::atomic<bool> started{false};
	if (started.exchange(true))
		return;
	std::thread([] {
		const auto now = [] { return GetCPUTicks(); };
		const Measured nano = Measure([](uint64_t ns) { return NanosleepFor(ns); }, now);
		const Measured usl = Measure([](uint64_t ns) { return UsleepFor(ns); }, now);
		const bool second = PreferSecond(nano, usl);
		const Measured& taken = second ? usl : nano;
		const uint64_t margin = taken.ok ? MarginFor(taken.lateness) : MARGIN_DEFAULT_NS;
		s_sleep_with_usleep.store(second, std::memory_order_relaxed);
		s_spin_margin_ns.store(margin, std::memory_order_relaxed);
		char line[256];
		std::snprintf(line, sizeof(line),
			"timing: sleeps wake late by %.2f ms typically, %.2f ms at worst (nanosleep %.2f/%.2f, "
			"sceKernelUsleep %.2f/%.2f ms at 90%%/worst); the frame limiter waits the last %.2f ms awake",
			taken.p50 / 1e6, taken.worst / 1e6, nano.p90 / 1e6, nano.worst / 1e6, usl.p90 / 1e6, usl.worst / 1e6,
			margin / 1e6);
		if (s_timing_report)
			s_timing_report(line);
		else
			Console.WriteLn("%s", line);
	}).detach();
}

void PS5Timing::SetReport(void (*report)(const char*))
{
	s_timing_report = report;
}

void Threading::Sleep(int ms)
{
	if (ms > 0)
		SleepFor(static_cast<u64>(ms) * 1000000ULL);
}

static std::mutex s_limiter_lock;
static PS5Timing::LimiterWaits s_limiter;

void PS5Timing::TakeLimiterLine(char* line, size_t size)
{
	if (!line || size == 0)
		return;
	line[0] = '\0';
	PS5Timing::LimiterWaits w;
	{
		std::lock_guard<std::mutex> lock(s_limiter_lock);
		w = s_limiter;
		s_limiter = PS5Timing::LimiterWaits();
	}
	if (w.waits == 0)
		return;
	const double waited = static_cast<double>(w.asleep_ns + w.awake_ns);
	std::snprintf(line, size,
		"frame limiter: %llu waits, %.2f ms asleep and %.2f ms awake each on average (%.0f%% awake, giving its turn "
		"away but for the last %.2f ms); %llu woke past their end, by %.2f ms at most",
		static_cast<unsigned long long>(w.waits), w.asleep_ns / 1e6 / w.waits, w.awake_ns / 1e6 / w.waits,
		waited > 0 ? 100.0 * w.awake_ns / waited : 0.0, PS5Timing::SPIN_LAST_NS / 1e6,
		static_cast<unsigned long long>(w.late), w.latest_ns / 1e6);
}

void Threading::SleepUntil(u64 ticks)
{
	// No clock_nanosleep() with an absolute time on the console. The frame limiter calls this once a
	// frame and wants to wake on time: sleep for what is left less the margin the start-up probe
	// measured for the kernel's wake-up, then wait out the margin awake, giving the core to the other
	// threads while more than SPIN_LAST_NS is left and spinning only the end (PS5Timing.h).
	PS5Timing::LimiterWaits waits;
	PS5Timing::WaitUntil(
		ticks, PS5Timing::SpinMarginNs(), [] { return GetCPUTicks(); }, [](uint64_t ns) { SleepFor(ns); },
		[] { sched_yield(); }, [] { Threading::SpinWait(); }, waits);
	if (waits.waits == 0)
		return;
	std::lock_guard<std::mutex> lock(s_limiter_lock);
	s_limiter.Add(waits.asleep_ns, waits.awake_ns, waits.late ? waits.latest_ns : 0);
}
