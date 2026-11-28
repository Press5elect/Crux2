// Crux2 - a sampling profiler for the emulator's threads, for test runs.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2's overlay says how busy the emulation, graphics and VU threads are, not what with. On
// the first console this title ran on, a game's heavy scenes had the graphics thread at 95% with
// the GPU at 30%: the thread was the limit, and nothing said whether its time went into the
// emulator's own code or into the graphics driver's. This says: a thread of its own interrupts
// the profiled threads every two milliseconds with a signal, whose handler notes the interrupted
// instruction and return addresses from the stack; every ten seconds each thread's notes are
// folded into functions (ps5_profile_fold.h) and written to the boot log as "profile: ..." lines.
//
// Testing only: a "profile" line in test-run.txt starts it (ps5_host.cpp, "Test runs"), for that
// launch. "profile" samples the graphics thread, and the driver's recording thread when the
// driver records on one (a "threaded-recording" line); "profile all" the emulation and VU
// threads too.
//
// The mechanism is the one of PS5 RetroArch's sampler (src/sampler_ps5.cpp, Copyright (C) 2026
// Mihawk, GPL-3.0-or-later), which has run on a console: pthread_kill with SIGUSR2, the handler
// reading the interrupted context. What is different here: the threads are named ones of the
// emulator, each has its own ring of samples, and the folding has the program's symbols.

#include "ps5_platform.h"
#include "ps5_profile_fold.h"
#include "ps5_thread_layout.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <pthread.h>

// The EE's code at a guest address, for the busiest blocks' dumps (PCSX2's side, x86/ix86-32/iR5900.cpp on the
// console): up to `count` words, the number read (0 when the address is not memory).
extern "C" int ps5px_profile_guest_ee_words(uint32_t pc, uint32_t* out, int count) __attribute__((weak));
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__linux__)
// The PC-side test (ps5/tests/profiler_test.cpp) builds this file as it is: the same signals,
// handler, registration and sampler, with Linux's names for the interrupted registers and for
// a thread's stack.
#define PROFILE_RIP(mc) ((mc).gregs[REG_RIP])
#define PROFILE_RSP(mc) ((mc).gregs[REG_RSP])
#define PROFILE_RBP(mc) ((mc).gregs[REG_RBP])
#define PROFILE_THREAD_ATTRIBUTES(thread, attributes) pthread_getattr_np(thread, attributes)
#else
#include <pthread_np.h>
// Refuses to build against an SDK whose ucontext_t is not the console's layout.
#include <ps5platform/context.h>
#define PROFILE_RIP(mc) ((mc).mc_rip)
#define PROFILE_RSP(mc) ((mc).mc_rsp)
#define PROFILE_RBP(mc) ((mc).mc_rbp)
#define PROFILE_THREAD_ATTRIBUTES(thread, attributes) pthread_attr_get_np(thread, attributes)
#endif

// A test builds with a shorter window.
#ifndef PS5PX_PROFILE_WINDOW_MS
#define PS5PX_PROFILE_WINDOW_MS 10000
#endif

// The first byte of the program's image (the linker script places it).
extern "C" const char __executable_start[] __attribute__((weak));

namespace
{
	constexpr int SAMPLE_SIGNAL = SIGUSR2;
	constexpr long SAMPLE_INTERVAL_NS = 2 * 1000 * 1000;
	constexpr uint64_t WINDOW_NS = PS5PX_PROFILE_WINDOW_MS * 1000000ull;
	// A window's samples, with room: 5,000 at the interval above.
	constexpr uint32_t RING_SIZE = 8192;
	// A thread less busy than this over a window is not written up: the interface, a paused game.
	constexpr double QUIET_PERCENT = 15.0;
	// Windows written per thread before the profiler stops: ten minutes of a busy thread.
	constexpr unsigned MAX_WINDOWS = 60;
	constexpr unsigned MAX_THREADS = 7; // graphics, the split's drawing, two recorders (the GS device's and paraLLEl-GS's), paraLLEl-GS, emulation, VU

	struct ProfiledThread
	{
		// Set and cleared under s_mutex; read by the handler, which runs on the thread itself.
		std::atomic<bool> used{false};
		pthread_t handle{};
		char name[12] = "";
		// The thread's stack, [base, top); both 0 when the console did not say.
		uint64_t stack_base = 0;
		uint64_t stack_top = 0;
		PS5ProfileSample* ring = nullptr;
		// Samples written so far: stored after the sample, so a reader never sees half of one.
		std::atomic<uint32_t> head{0};
		// The sampler thread's own.
		uint32_t reported = 0;
		uint32_t sent = 0; // interruptions since the last report
		uint64_t cpu_reported = 0; // the thread's processor time at the last report
		unsigned windows = 0;
		bool quiet_noted = false;
	};

	std::atomic<bool> s_running{false};
	bool s_all_threads = false;
	std::mutex s_mutex; // registration against the sampler's signals; never taken by the handler
	ProfiledThread s_threads[MAX_THREADS];
	PS5ProfileSymbols s_symbols;
	uint64_t s_base = 0;
	uint64_t s_end = 0; // the end of the program's code, as an address

	// The generated code's ranges by name (ps5px_profile_code_region): the recompilers' code areas and
	// the stubs PCSX2 registers with Perf. Written under s_regions_lock by the recompilers' resets, read
	// by the sampler thread when it folds (never by the signal handler).
	constexpr size_t MAX_REGIONS = 48;
	struct CodeRegion
	{
		std::atomic<uint64_t> start{0}, end{0};
		std::atomic<const char*> name{nullptr};
	};
	CodeRegion s_regions[MAX_REGIONS];
	std::atomic<size_t> s_region_count{0};
	std::mutex s_regions_lock;

	std::vector<PS5ProfileRegion> CodeRegions()
	{
		std::vector<PS5ProfileRegion> out;
		const size_t n = std::min(s_region_count.load(std::memory_order_acquire), MAX_REGIONS);
		for (size_t i = 0; i < n; i++)
		{
			const uint64_t start = s_regions[i].start.load(std::memory_order_relaxed);
			const uint64_t end = s_regions[i].end.load(std::memory_order_relaxed);
			const char* name = s_regions[i].name.load(std::memory_order_relaxed);
			if (name && end > start)
				out.push_back({start, end, name});
		}
		return out;
	}

	// The recompilers' blocks (PCSX2's Perf::*.RegisterPC, common/Perf.cpp): where each block's code is,
	// and the guest address it was compiled from, so that a sample in generated code names the guest code
	// it ran. One table a recompiler, each written by the one thread that compiles for it (the EE thread
	// for the EE and the IOP, the VU thread for VU1), read by the sampler thread when it writes a window
	// up (never by the signal handler). A recompiler fills its code area from the bottom up between two
	// resets, so a table is in address order and searched by halves; a reset starts it again (a sample
	// taken across a reset can be put down to a block of the new code: rare, and only names).
	struct BlockEntry
	{
		uint64_t start;
		uint32_t size;
		uint32_t pc;
	};
	struct BlockTable
	{
		const char* name;
		uint32_t capacity;
		std::unique_ptr<BlockEntry[]> entries{};
		std::atomic<uint32_t> count{0};
		std::atomic<uint32_t> dropped{0}; // blocks past the capacity, not named
	};
	enum : int { BLOCKS_EE, BLOCKS_IOP, BLOCKS_VU0, BLOCKS_VU1, BLOCK_TABLES };
	BlockTable s_blocks[BLOCK_TABLES] = {{"EE", 262144, {}}, {"IOP", 65536, {}}, {"microVU0", 16384, {}}, {"microVU1", 65536, {}}};
	std::atomic<bool> s_blocks_on{false};
	int s_block_dumps_left = 12; // code dumps a run (the sampler thread's)

	struct BlockHit
	{
		int table;
		const BlockEntry* entry;
	};

	BlockHit FindBlock(uint64_t address)
	{
		for (int t = 0; t < BLOCK_TABLES; t++)
		{
			const BlockTable& table = s_blocks[t];
			const uint32_t n = std::min(table.count.load(std::memory_order_acquire), table.capacity);
			if (n == 0 || address < table.entries[0].start)
				continue;
			const BlockEntry* const begin = table.entries.get();
			// The last block that starts at or before the address.
			const BlockEntry* it = std::upper_bound(begin, begin + n, address, [](uint64_t a, const BlockEntry& e) { return a < e.start; });
			if (it == begin)
				continue;
			--it;
			if (address < it->start + it->size)
				return {t, it};
		}
		return {-1, nullptr};
	}

	// The window's samples by guest block: the busiest ones as a line, and, a few times a run, the code
	// of the busiest EE blocks as bytes (to be read against a disassembler: what the recompiler made).
	void BlockLines(const std::vector<PS5ProfileSample>& samples, const char* prefix)
	{
		if (!s_blocks_on.load(std::memory_order_acquire) || samples.empty())
			return;
		struct Count
		{
			int table;
			uint32_t pc;
			uint64_t start;
			uint32_t size;
			uint32_t n;
		};
		std::vector<Count> counts;
		uint32_t in_blocks = 0;
		for (const PS5ProfileSample& sample : samples)
		{
			const BlockHit hit = FindBlock(sample.rip);
			if (!hit.entry)
				continue;
			in_blocks++;
			bool found = false;
			for (Count& c : counts)
			{
				if (c.table == hit.table && c.start == hit.entry->start)
				{
					c.n++;
					found = true;
					break;
				}
			}
			if (!found && counts.size() < 4096)
				counts.push_back({hit.table, hit.entry->pc, hit.entry->start, hit.entry->size, 1});
		}
		if (in_blocks == 0)
			return;
		std::sort(counts.begin(), counts.end(), [](const Count& a, const Count& b) { return a.n != b.n ? a.n > b.n : a.start < b.start; });
		const double total = static_cast<double>(samples.size());
		std::string line = std::string(prefix) + " busiest guest blocks (share of all samples):";
		for (size_t i = 0; i < counts.size() && i < 10; i++)
		{
			char part[80];
			std::snprintf(part, sizeof(part), "%s %s %08X %.1f%%", i ? "," : "", s_blocks[counts[i].table].name, counts[i].pc,
				100.0 * counts[i].n / total);
			line += part;
		}
		ps5px_log("%s", line.c_str());

		// The code of the three busiest EE blocks, 64 bytes a line, at most 1 KiB a block.
		for (size_t i = 0; i < counts.size() && i < 3 && s_block_dumps_left > 0; i++)
		{
			const Count& c = counts[i];
			if (c.table != BLOCKS_EE || c.n * 250 < samples.size()) // under 0.4% of the window: not worth the lines
				continue;
			s_block_dumps_left--;
			const uint32_t size = std::min<uint32_t>(c.size, 1024);
			ps5px_log("%s code of EE block %08X (%u bytes%s, at 0x%llx):", prefix, c.pc, c.size, size < c.size ? ", the first 1024" : "",
				static_cast<unsigned long long>(c.start));
			// The guest instructions the block was compiled from (MIPS words from the block's address on),
			// to be read beside the x86: what the game's code does, and how the recompiler made it.
			if (ps5px_profile_guest_ee_words)
			{
				uint32_t words[32];
				const int n = ps5px_profile_guest_ee_words(c.pc, words, 32);
				for (int w = 0; w < n; w += 8)
				{
					char text[8 * 9 + 1] = "";
					for (int k = w; k < n && k < w + 8; k++)
						std::snprintf(text + (k - w) * 9, 10, "%08x ", words[k]);
					ps5px_log("%s   guest %08X %s", prefix, c.pc + w * 4, text);
				}
			}
			const uint8_t* const code = reinterpret_cast<const uint8_t*>(c.start);
			for (uint32_t at = 0; at < size; at += 64)
			{
				char hex[64 * 2 + 1];
				const uint32_t len = std::min<uint32_t>(64, size - at);
				for (uint32_t b = 0; b < len; b++)
					std::snprintf(hex + b * 2, 3, "%02x", code[at + b]);
				ps5px_log("%s   +%03x %s", prefix, at, hex);
			}
		}
	}

	bool InProgram(uint64_t address)
	{
		return address >= s_base && address < s_end;
	}

	uint64_t NowNs()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
	}

	// On the interrupted thread. Nothing here allocates, takes a lock or formats.
	// (It reads the interrupted stack word by word, other functions' frames with it: a
	// sanitizer build, which the PC-side test is, must not take those reads for errors.)
	__attribute__((no_sanitize("address", "thread", "undefined"))) void OnSample(int, siginfo_t*, void* context_pointer)
	{
		if (!context_pointer || !s_running.load(std::memory_order_acquire))
			return;

		const pthread_t self = pthread_self();
		ProfiledThread* thread = nullptr;
		for (ProfiledThread& candidate : s_threads)
		{
			if (candidate.used.load(std::memory_order_acquire) && pthread_equal(candidate.handle, self))
				thread = &candidate;
		}
		if (!thread)
			return;

		const int saved_errno = errno;
		const mcontext_t& registers = static_cast<const ucontext_t*>(context_pointer)->uc_mcontext;
		const uint32_t at = thread->head.load(std::memory_order_relaxed);
		PS5ProfileSample& sample = thread->ring[at & (RING_SIZE - 1)];

		sample.rip = static_cast<uint64_t>(PROFILE_RIP(registers));
		struct timespec cpu;
		sample.cpu_us = (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0) ?
							(static_cast<uint64_t>(cpu.tv_sec) * 1000000ull + static_cast<uint64_t>(cpu.tv_nsec) / 1000ull) :
							0;

		// The stack that may be read: from the interrupted stack pointer to the top of the
		// thread's stack when the pointer is on that stack, else (the stack is not known, or the
		// thread was interrupted on another: its signal stack, inside another handler) to the end
		// of the 16 KiB page the pointer is in. What follows a stack may not be mapped: the crash
		// report's sampler was ended by reading on, on the first console run to show the interface.
		const uint64_t rsp = static_cast<uint64_t>(PROFILE_RSP(registers));
		const uint64_t rbp = static_cast<uint64_t>(PROFILE_RBP(registers));
		unsigned depth = 0;
		unsigned scanned = 0; // how many of the entries come from looking through the stack
		if (rsp != 0 && (rsp & 7) == 0 && rsp < UINT64_MAX - 0x8000)
		{
			uint64_t limit = (rsp | 0x3fffull) + 1;
			if (rsp >= thread->stack_base && rsp < thread->stack_top)
				limit = thread->stack_top;

			// Is the frame pointer one? The program's functions that call others keep frame
			// pointers (the emulator's and the driver's: the compiler's default for the console);
			// those that call nothing do not, nor may the system's libraries, and then it is
			// either still the frame of the function that called them, or any other value. (No
			// "rbp + 16" in these tests: a value near the top of the address space would wrap and
			// pass.)
			const bool frame_valid = (rbp & 7) == 0 && rbp >= rsp && rbp < limit && limit - rbp >= 16;

			// First what lies between the stack pointer and that frame: the return address of
			// code without a frame (a function that calls nothing, the program's caller of a
			// system function), nearest first, among whatever else the stack holds there. Which
			// of these words count is decided when the samples are folded, by the function that
			// was interrupted (ps5_profile_fold.h).
			constexpr uint64_t SCAN_BYTES = 2048;
			constexpr unsigned SCAN_ENTRIES = PS5ProfileSample::CHAIN / 2;
			const uint64_t scan_end = std::min({frame_valid ? rbp : limit, rsp + SCAN_BYTES, limit});
			// (With a frame, half of the entries are kept for the frames above it.)
			const unsigned scan_entries = frame_valid ? SCAN_ENTRIES : PS5ProfileSample::CHAIN;
			for (uint64_t word = rsp; word + 8 <= scan_end && depth < scan_entries; word += 8)
			{
				const uint64_t value = *reinterpret_cast<const uint64_t*>(word);
				if (InProgram(value))
					sample.chain[depth++] = value;
			}

			scanned = depth;

			// Then the frames.
			uint64_t frame = rbp;
			while (frame_valid && depth < PS5ProfileSample::CHAIN)
			{
				if ((frame & 7) != 0 || frame < rsp || frame >= limit || limit - frame < 16)
					break;
				const uint64_t* const pair = reinterpret_cast<const uint64_t*>(frame);
				if (pair[1] == 0)
					break;
				sample.chain[depth++] = pair[1];
				if (pair[0] <= frame)
					break;
				frame = pair[0];
			}
		}
		sample.scanned = scanned;
		sample.reserved = 0;
		for (; depth < PS5ProfileSample::CHAIN; depth++)
			sample.chain[depth] = 0;

		thread->head.store(at + 1, std::memory_order_release);
		errno = saved_errno;
	}

	// The calling thread's stack, [base, top). False when the console does not say, or says
	// something this function's own frame does not lie in.
	bool OwnStack(uint64_t* base, uint64_t* top)
	{
		pthread_attr_t attributes;
		if (pthread_attr_init(&attributes) != 0)
			return false;
		void* stack = nullptr;
		size_t size = 0;
		const bool known = PROFILE_THREAD_ATTRIBUTES(pthread_self(), &attributes) == 0 &&
						   pthread_attr_getstack(&attributes, &stack, &size) == 0 && stack != nullptr && size != 0;
		pthread_attr_destroy(&attributes);
		if (!known)
			return false;

		const uint64_t low = reinterpret_cast<uint64_t>(stack), here = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
		if (size > 1024ull * 1024 * 1024 || low + size < low || here < low || here >= low + size)
			return false;
		*base = low;
		*top = low + size;
		return true;
	}

	// A thread that ends takes itself off the list first, so the sampler never signals a thread
	// that is gone.
	struct Registration
	{
		int slot = -1;
		~Registration()
		{
			if (slot < 0)
				return;
			// A signal already on its way stays pending until the thread is gone.
			sigset_t set;
			sigemptyset(&set);
			sigaddset(&set, SAMPLE_SIGNAL);
			pthread_sigmask(SIG_BLOCK, &set, nullptr);
			const std::lock_guard<std::mutex> lock(s_mutex);
			s_threads[slot].used.store(false, std::memory_order_release);
		}
	};

	void Report(ProfiledThread& thread, double seconds, std::vector<PS5ProfileSample>& copy, std::vector<std::string>& lines)
	{
		// The slot's ring and name are given under the lock (a thread joining); once given they stay.
		PS5ProfileSample* ring;
		char name[sizeof(thread.name)];
		{
			const std::lock_guard<std::mutex> lock(s_mutex);
			ring = thread.ring;
			std::memcpy(name, thread.name, sizeof(name));
		}
		if (!ring || thread.windows >= MAX_WINDOWS)
			return;

		const uint32_t head = thread.head.load(std::memory_order_acquire);
		uint32_t from = thread.reported;
		const uint32_t sent = thread.sent;
		thread.reported = head;
		thread.sent = 0;
		if (head - from > RING_SIZE)
			from = head - RING_SIZE;
		if (head == from)
			return;

		copy.clear();
		for (uint32_t at = from; at != head; at++)
			copy.push_back(ring[at & (RING_SIZE - 1)]);

		// The thread's processor time over the whole window: from the last sample of the window
		// before, when there was one (and the thread is the same one: its clock did not go back).
		const uint64_t cpu_last = copy.back().cpu_us;
		const uint64_t cpu_before = (thread.cpu_reported != 0 && thread.cpu_reported <= copy.front().cpu_us) ? thread.cpu_reported : 0;
		thread.cpu_reported = cpu_last;

		// A thread that mostly waited is not written up (the interface, a paused game).
		const uint64_t cpu_first = (cpu_before != 0) ? cpu_before : copy.front().cpu_us;
		if (cpu_first != 0 && cpu_last >= cpu_first &&
			static_cast<double>(cpu_last - cpu_first) / (seconds * 10000.0) < QUIET_PERCENT)
		{
			if (!thread.quiet_noted)
			{
				thread.quiet_noted = true;
				ps5px_log("profile: %s: under %.0f%% busy, not written up (nor are later windows like it)", name, QUIET_PERCENT);
			}
			return;
		}

		char prefix[32];
		std::snprintf(prefix, sizeof(prefix), "profile: %s:", name);
		lines.clear();
		const std::vector<PS5ProfileRegion> regions = CodeRegions();
		PS5ProfileFold(copy.data(), copy.size(), s_symbols, s_base, seconds, prefix, lines, sent, cpu_before, 25, 40, 8, 8,
			regions.data(), regions.size());
		for (const std::string& line : lines)
			ps5px_log("%s", line.c_str());
		BlockLines(copy, prefix);

		if (++thread.windows == MAX_WINDOWS)
			ps5px_log("profile: %s: %u windows written, no more are (the thread is left alone from here on)", name, MAX_WINDOWS);
	}

	std::atomic<void (*)()> s_window_extra{nullptr};



	void* SamplerMain(void*)
	{
		// This thread takes no samples itself, whatever thread the process's signals go to.
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SAMPLE_SIGNAL);
		pthread_sigmask(SIG_BLOCK, &set, nullptr);

		std::vector<PS5ProfileSample> copy;
		std::vector<std::string> lines;
		copy.reserve(RING_SIZE);
		uint64_t window_start = NowNs();
		for (;;)
		{
			const struct timespec interval = {0, SAMPLE_INTERVAL_NS};
			nanosleep(&interval, nullptr);

			{
				const std::lock_guard<std::mutex> lock(s_mutex);
				for (ProfiledThread& thread : s_threads)
				{
					// (A thread whose windows are all written is no longer interrupted.)
					if (thread.used.load(std::memory_order_acquire) && thread.windows < MAX_WINDOWS &&
						pthread_kill(thread.handle, SAMPLE_SIGNAL) == 0)
					{
						thread.sent++;
					}
				}
			}

			const uint64_t now = NowNs();
			if (now - window_start < WINDOW_NS)
				continue;
			const double seconds = static_cast<double>(now - window_start) / 1e9;
			window_start = now;
			for (ProfiledThread& thread : s_threads)
				Report(thread, seconds, copy, lines);
			// What else a window says (the host's: how evenly frames reached the screen).
			if (void (*extra)() = s_window_extra.load(std::memory_order_acquire))
				extra();
		}
		return nullptr;
	}

	bool ReadWholeFile(const char* path, std::string& text)
	{
		std::FILE* const file = std::fopen(path, "rb");
		if (!file)
			return false;
		char block[65536];
		size_t got;
		while ((got = std::fread(block, 1, sizeof(block), file)) > 0)
			text.append(block, got);
		const bool ok = !std::ferror(file);
		std::fclose(file);
		return ok;
	}
} // namespace

// From main, before the emulator's threads exist.
extern "C" void ps5px_profile_start(int all_threads)
{
	if (s_running.load(std::memory_order_acquire))
		return;

	char path[300];
	std::snprintf(path, sizeof(path), "%s/symbols.txt", ps5px_title_dir());
	std::string text;
	if (!ReadWholeFile(path, text) || !s_symbols.Load(text))
	{
		ps5px_log("profile: not started: symbols.txt in the title folder is missing or cut short (it comes with the build)");
		return;
	}
	s_base = reinterpret_cast<uint64_t>(&__executable_start[0]);
	s_end = s_base + s_symbols.End();
	// The recompilers' block tables, for a run that profiles the emulation threads (about 6 MiB).
	if (all_threads && !s_blocks_on.load(std::memory_order_relaxed))
	{
		for (BlockTable& table : s_blocks)
			table.entries.reset(new (std::nothrow) BlockEntry[table.capacity]);
		bool all = true;
		for (BlockTable& table : s_blocks)
			all &= table.entries != nullptr;
		s_blocks_on.store(all, std::memory_order_release);
	}
	if (s_base == 0)
	{
		ps5px_log("profile: not started: the program's own address is not known");
		return;
	}

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = &OnSample;
	// On the thread's signal stack (frontend/ps5_crash.cpp gives each thread one), so that the
	// stack of the recompilers' generated code is left alone. Interrupted system calls start
	// again, sleeps excepted: the emulator's go on for what is left (common/PS5Misc.cpp).
	action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
	if (sigaction(SAMPLE_SIGNAL, &action, nullptr) != 0)
	{
		ps5px_log("profile: not started: the signal could not be set up (errno %d)", errno);
		return;
	}

	s_all_threads = (all_threads != 0);
	s_running.store(true, std::memory_order_release);

	pthread_t sampler;
	if (pthread_create(&sampler, nullptr, &SamplerMain, nullptr) != 0)
	{
		s_running.store(false, std::memory_order_release);
		ps5px_log("profile: not started: its thread could not be created");
		return;
	}
	pthread_detach(sampler);
	ps5px_log("profile: %s every %ld ms, written up every %.0f s while busy; %zu functions known, the program is at %p",
		s_all_threads ? "the graphics, emulation and VU threads" : "the graphics thread", SAMPLE_INTERVAL_NS / 1000000,
		static_cast<double>(WINDOW_NS) / 1e9, s_symbols.Count(), static_cast<const void*>(&__executable_start[0]));
}

// A block a recompiler compiled (common/Perf.cpp's RegisterPC on the console): 0 EE, 1 IOP, 2 microVU0,
// 3 microVU1. From the thread that compiles for that recompiler only.
extern "C" void ps5px_profile_block(int table, const void* start, size_t size, uint32_t pc)
{
	if (table < 0 || table >= BLOCK_TABLES || !s_blocks_on.load(std::memory_order_acquire))
		return;
	BlockTable& t = s_blocks[table];
	const uint32_t n = t.count.load(std::memory_order_relaxed);
	if (n >= t.capacity)
	{
		t.dropped.store(t.dropped.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		return;
	}
	t.entries[n] = {reinterpret_cast<uint64_t>(start), static_cast<uint32_t>(size), pc};
	t.count.store(n + 1, std::memory_order_release);
}

// The recompiler's code was thrown away (a reset): its table starts again.
extern "C" void ps5px_profile_blocks_reset(int table)
{
	if (table >= 0 && table < BLOCK_TABLES)
		s_blocks[table].count.store(0, std::memory_order_release);
}

// A range of generated code by name (`name` must live for the whole run: PCSX2's Perf symbols and the
// recompilers' region names are string literals). The same name again moves its range (a reset).
extern "C" void ps5px_profile_code_region(const char* name, const void* start, size_t size)
{
	if (!name || !start || size == 0)
		return;
	const std::lock_guard<std::mutex> lock(s_regions_lock);
	const size_t n = s_region_count.load(std::memory_order_relaxed);
	size_t slot = n;
	for (size_t i = 0; i < n; i++)
	{
		const char* existing = s_regions[i].name.load(std::memory_order_relaxed);
		if (existing == name || (existing && std::strcmp(existing, name) == 0))
			slot = i;
	}
	if (slot >= MAX_REGIONS)
		return;
	s_regions[slot].start.store(reinterpret_cast<uint64_t>(start), std::memory_order_relaxed);
	s_regions[slot].end.store(reinterpret_cast<uint64_t>(start) + size, std::memory_order_relaxed);
	s_regions[slot].name.store(name, std::memory_order_relaxed);
	if (slot == n)
		s_region_count.store(n + 1, std::memory_order_release);
}

// From a thread of the emulator as it names itself (common/PS5Threads.cpp): the ones the
// profiler is for join its list.
extern "C" void ps5px_profile_thread_named(const char* name)
{
	if (!name || !s_running.load(std::memory_order_acquire))
		return;

	const char* label = nullptr;
	if (std::strcmp(name, "GS") == 0)
		label = "graphics";
	else if (std::strcmp(name, "GS draw") == 0)
		label = "drawing"; // GS Thread Split's DRAW side (GSSplit.cpp): the texture cache and the Vulkan device
	else if (std::strcmp(name, "RADV recorder") == 0)
		label = "recorder";
	else if (std::strcmp(name, "paraLLEl-GS") == 0)
		label = "paraLLEl-GS";
	else if (s_all_threads && std::strcmp(name, "CPU Thread") == 0)
		label = "emulation";
	else if (s_all_threads && std::strcmp(name, "MTVU") == 0)
		label = "VU";
	if (!label)
		return;

	static thread_local Registration registration;
	if (registration.slot >= 0)
		return;

	uint64_t stack_base = 0, stack_top = 0;
	const bool stack_known = OwnStack(&stack_base, &stack_top);
	const std::lock_guard<std::mutex> lock(s_mutex);
	for (unsigned slot = 0; slot < MAX_THREADS; slot++)
	{
		ProfiledThread& thread = s_threads[slot];
		if (thread.used.load(std::memory_order_acquire))
			continue;
		// A slot keeps its ring and its counts when a thread of the same name takes it again
		// (the VU thread, with each game).
		if (thread.ring && std::strcmp(thread.name, label) != 0)
			continue;
		if (!thread.ring)
		{
			thread.ring = static_cast<PS5ProfileSample*>(std::calloc(RING_SIZE, sizeof(PS5ProfileSample)));
			if (!thread.ring)
				return;
			std::snprintf(thread.name, sizeof(thread.name), "%s", label);
		}
		thread.handle = pthread_self();
		thread.stack_base = stack_base;
		thread.stack_top = stack_top;
		thread.used.store(true, std::memory_order_release);
		registration.slot = static_cast<int>(slot);
		ps5px_log("profile: the %s thread joins (its stack is %s)", label, stack_known ? "known" : "not known");
		return;
	}
}

// From the graphics driver's recording thread as it starts (threaded recording; the driver names
// this function weakly, ps5/patches/mesa/0002): it gets a crash stack as the emulator's threads
// do, and joins the profiler.
extern "C" void ps5_title_thread_named(const char* name)
{
	ps5px_install_crash_stack();
	const bool pgs = name && std::strcmp(name, "paraLLEl-GS") == 0;
	ps5px_log("%s thread started: %s", pgs ? "renderer" : "driver", name ? name : "(unnamed)");
	// Under Crux2's thread layout paraLLEl-GS's thread gets a core of its own, as the EE, VU and GS
	// threads do: started from the GS thread, it would otherwise share the GS's core.
	if (pgs && PS5ThreadLayout::Wanted())
		PS5ThreadLayout::PinCallingThread(PS5ThreadLayout::kParallelGsMask, "paraLLEl-GS's thread");
	// The driver's recording thread, started from the GS thread, would share the GS's core. It looks
	// for work between batches (a spin of up to 50 us, more than half its time in a lit scene), and a
	// Zen 2 core running two threads splits its queues between them, so the GS thread ran on half a
	// core's queues for most of the frame. [PS5] DriverOwnCore gives it core 3 instead (paraLLEl-GS's
	// core: under paraLLEl-GS it shares that core, as before).
	const bool recorder = name && std::strcmp(name, "RADV recorder") == 0;
	if (recorder && PS5ThreadLayout::Wanted() && PS5ThreadLayout::DriverOwnCore())
		PS5ThreadLayout::PinCallingThread(PS5ThreadLayout::kParallelGsMask, "the driver's recording thread");
	ps5px_profile_thread_named(name);
}

// A line or two more at the end of each window (ps5_host.cpp: the frame times of PS5Pacing).
extern "C" void ps5px_profiler_set_window_extra(void (*extra)(void))
{
	s_window_extra.store(extra, std::memory_order_release);
}
