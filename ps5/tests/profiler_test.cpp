// Crux2 - the profiler itself (frontend/ps5_profiler.cpp), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The profiler's source is built for the PC as it is (it has Linux's names for the interrupted
// registers beside the console's) and run on threads whose work is known:
//
//   - a "GS" thread that spends most of its time in one function called from another, and the
//     rest in a second: the profile has to name them, in that order, with the caller and the
//     thread's own function on the stack;
//   - a "CPU Thread" that works and sleeps in turns (a sampled thread's waits are interrupted
//     500 times a second and have to go on);
//   - "MTVU" threads that start and end every few milliseconds while the sampler runs (a thread
//     that ends takes itself off the sampler's list; the sampler must never signal one that
//     is gone, and a new thread takes over the slot).
//
// run-host-tests.sh builds it three ways: plain (the figures are checked), with the address
// sanitizer, and with the thread sanitizer (which delivers signals late, so only the absence of
// races and crashes is checked there: "relaxed").
//
// What this cannot show is the console's kernel doing the same: pthread_kill reaching a thread
// of a title, and the interrupted context holding the registers. PS5 RetroArch's sampler, which
// this mechanism is taken from, has shown that on a console.

#include "ps5_platform.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <time.h>

static std::mutex s_log_mutex;
static std::vector<std::string> s_log;
static const char* s_title_dir = ".";

extern "C" const char* ps5px_title_dir(void)
{
	return s_title_dir;
}

extern "C" void ps5px_log(const char* format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	const std::lock_guard<std::mutex> lock(s_log_mutex);
	s_log.emplace_back(text);
}

// As the title's threads have one (frontend/ps5_crash.cpp): the handler runs on it.
// The thread layout (frontend/ps5_thread_layout.cpp): off here, so paraLLEl-GS's thread is left where it starts.
#include "ps5_thread_layout.h"
namespace PS5ThreadLayout
{
	bool Wanted() { return false; }
	bool DriverOwnCore() { return false; }
	bool PinCallingThread(uint64_t, const char*) { return false; }
} // namespace PS5ThreadLayout

extern "C" void ps5px_install_crash_stack(void)
{
	struct Stack
	{
		void* memory = nullptr;
		~Stack()
		{
			if (!memory)
				return;
			stack_t none{};
			none.ss_flags = SS_DISABLE;
			sigaltstack(&none, nullptr);
			std::free(memory);
		}
	};
	static thread_local Stack stack;
	if (stack.memory)
		return;
	stack.memory = std::malloc(128 * 1024);
	stack_t ss{};
	ss.ss_sp = stack.memory;
	ss.ss_size = 128 * 1024;
	sigaltstack(&ss, nullptr);
}

extern "C" void ps5_title_thread_named(const char* name);

static std::atomic<bool> s_stop{false};
static std::atomic<unsigned long long> s_sink{0};

// The work: nothing the compiler can fold away or inline.
__attribute__((noinline)) unsigned long long test_hot_leaf(unsigned long long seed, unsigned rounds)
{
	unsigned long long value = seed;
	for (unsigned i = 0; i < rounds; i++)
		value = value * 6364136223846793005ull + 1442695040888963407ull + (value >> 29);
	return value;
}

__attribute__((noinline)) unsigned long long test_hot_parent(unsigned long long seed)
{
	unsigned long long value = seed;
	for (unsigned i = 0; i < 64; i++)
		value ^= test_hot_leaf(value + i, 2000);
	return value;
}

__attribute__((noinline)) unsigned long long test_side_leaf(unsigned long long seed, unsigned rounds)
{
	unsigned long long value = seed;
	for (unsigned i = 0; i < rounds; i++)
		value = (value ^ (value << 13)) + (value >> 7) + 0x9e3779b97f4a7c15ull;
	return value;
}

__attribute__((noinline)) void test_graphics_thread()
{
	ps5px_install_crash_stack();
	ps5px_profile_thread_named("GS");
	unsigned long long value = 1;
	while (!s_stop.load(std::memory_order_relaxed))
	{
		// About four parts of the hot pair to one of the side function.
		value ^= test_hot_parent(value);
		value ^= test_side_leaf(value, 64 * 2000 / 4);
	}
	s_sink += value;
}

__attribute__((noinline)) void test_emulation_thread(std::atomic<unsigned>* sleeps_cut_short)
{
	ps5px_install_crash_stack();
	ps5px_profile_thread_named("CPU Thread");
	unsigned long long value = 2;
	while (!s_stop.load(std::memory_order_relaxed))
	{
		value ^= test_side_leaf(value, 20000);
		// A sleep as the port's are (common/PS5Misc.cpp): what a signal cuts off is slept again.
		struct timespec left = {0, 3 * 1000 * 1000};
		while (nanosleep(&left, &left) != 0 && errno == EINTR)
			(*sleeps_cut_short)++;
	}
	s_sink += value;
}

static bool Has(const std::vector<std::string>& lines, const char* text)
{
	for (const std::string& line : lines)
	{
		if (line.find(text) != std::string::npos)
			return true;
	}
	return false;
}

// The share (percent) on the first line that holds `name` after the first line that holds
// `heading` and before the next heading; -1 if there is none.
static double Share(const std::vector<std::string>& lines, const char* thread, const char* heading, const char* name)
{
	const std::string prefix = std::string("profile: ") + thread + ":";
	bool inside = false;
	for (const std::string& line : lines)
	{
		if (line.compare(0, prefix.size(), prefix) != 0)
			continue;
		const std::string rest = line.substr(prefix.size());
		if (rest.find(heading) != std::string::npos)
		{
			inside = true;
			continue;
		}
		if (!inside)
			continue;
		if (rest.find('%') == std::string::npos || rest.find("  [") == std::string::npos)
		{
			if (rest.find(':') != std::string::npos && rest.find('[') == std::string::npos)
				inside = false; // the next heading
			continue;
		}
		if (rest.find(name) != std::string::npos)
			return std::atof(rest.c_str());
	}
	return -1.0;
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <folder with this program's symbols.txt> [relaxed]\n", argv[0]);
		return 2;
	}
	s_title_dir = argv[1];
	const bool relaxed = (argc > 2 && std::strcmp(argv[2], "relaxed") == 0);
	unsigned failures = 0;
	const auto check = [&](bool condition, const char* what) {
		if (!condition)
		{
			failures++;
			std::fprintf(stderr, "FAILED: %s\n", what);
		}
	};

	// Named before the profiler runs: not a thread of its.
	ps5px_profile_thread_named("GS");

	ps5px_profile_start(1);
	check(Has(s_log, "profile: the graphics, emulation and VU threads every 2 ms"), "the profiler starts");
	ps5px_profile_start(1); // a second start changes nothing

	std::atomic<unsigned> sleeps_cut_short{0};
	std::thread graphics(test_graphics_thread);
	std::thread emulation(test_emulation_thread, &sleeps_cut_short);
	// A thread of the driver's, announced the way the driver announces its recording thread.
	std::thread recorder([]() {
		ps5_title_thread_named("RADV recorder");
		unsigned long long value = 3;
		while (!s_stop.load(std::memory_order_relaxed))
			value ^= test_side_leaf(value, 1000);
		s_sink += value;
	});

	// VU threads that come and go while the sampler signals: 150 of them in about 3 s.
	unsigned vu_threads = 0;
	const auto started = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(3200))
	{
		std::thread vu([]() {
			ps5px_install_crash_stack();
			ps5px_profile_thread_named("MTVU");
			s_sink += test_side_leaf(4, 200000 + static_cast<unsigned>(s_sink.load() % 400000));
		});
		vu.join();
		vu_threads++;
	}

	s_stop.store(true);
	graphics.join();
	emulation.join();
	recorder.join();
	// The sampler goes on with nothing to sample: one more window, with every thread gone.
	std::this_thread::sleep_for(std::chrono::milliseconds(PS5PX_PROFILE_WINDOW_MS + 200));

	std::vector<std::string> lines;
	{
		const std::lock_guard<std::mutex> lock(s_log_mutex);
		lines = s_log;
	}
	for (const std::string& line : lines)
	{
		if (line.find("profile: graphics:") == 0 || line.find("joins") != std::string::npos)
			std::printf("    %s\n", line.c_str());
	}

	check(Has(lines, "profile: the graphics thread joins (its stack is known)"), "the graphics thread joined, its stack known");
	check(Has(lines, "profile: the emulation thread joins"), "the emulation thread joined");
	check(Has(lines, "profile: the VU thread joins"), "a VU thread joined");
	check(Has(lines, "profile: the recorder thread joins"), "the driver's thread joined");
	check(Has(lines, "driver thread started: RADV recorder"), "the driver's thread was announced");
	check(vu_threads >= 20, "VU threads came and went");

	check(Has(lines, "profile: graphics: where: the emulator"), "the graphics thread was written up");
	check(Has(lines, "profile: emulation:") || relaxed, "the emulation thread was written up");
	check(Has(lines, "profile: recorder: where:"), "the driver's thread was written up");
	if (!relaxed)
	{
		// The graphics thread: the hot leaf first (about 80% of its time, the side function
		// about 20%), the hot parent and the thread's own function on the stack of nearly
		// every sample, and all of it the program's own code.
		const double hot = Share(lines, "graphics", "running in", "test_hot_leaf");
		const double side = Share(lines, "graphics", "running in", "test_side_leaf");
		const double parent = Share(lines, "graphics", "on the stack", "test_hot_parent");
		const double thread = Share(lines, "graphics", "on the stack", "test_graphics_thread");
		std::printf("graphics thread: hot leaf %.1f%%, side leaf %.1f%%; on the stack: hot parent %.1f%%, the thread's function %.1f%%\n",
			hot, side, parent, thread);
		check(hot >= 60.0 && hot <= 95.0, "the hot leaf is where the graphics thread runs most");
		check(side >= 5.0 && side <= 40.0, "the side leaf has its share");
		check(hot > side, "the hot leaf is before the side leaf");
		check(parent >= 60.0, "the hot leaf's caller is on the stack");
		check(thread >= 90.0, "the thread's own function is on the stack of nearly every sample");
		check(parent >= hot - 3.0, "the caller is on the stack whenever the leaf runs");

		// The emulation thread sleeps most of the time: its sleeps were interrupted, and went on.
		check(sleeps_cut_short.load() > 50, "the sampled thread's sleeps were interrupted");
		const double waiting = Share(lines, "emulation", "outside the program", "test_emulation_thread");
		std::printf("emulation thread: %u sleeps cut short and gone on with; outside the program from its own function: %.1f%%\n",
			sleeps_cut_short.load(), waiting);
	}

	std::printf("profiler: %u VU threads, %zu log lines, %u failed\n", vu_threads, lines.size(), failures);
	return (failures == 0) ? 0 : 1;
}
