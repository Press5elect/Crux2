// Crux2 - the profiler's folding (frontend/ps5_profile_fold.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The sampling itself needs the console (a signal to a thread, the interrupted registers). What
// is done with the samples does not: reading symbols.txt, finding the function an address is in,
// and counting. This checks those against cases worked out by hand, and against a slow count of
// random samples.

#include "ps5_profile_fold.h"

#include <cinttypes>
#include <cstring>
#include <map>
#include <random>

static unsigned s_checks = 0, s_failures = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static const char SYMBOLS[] =
	"1000 p _start\n"
	"1100 E GSDeviceVK::SendHWDraw\n" // a capital letter: the function begins by setting up a frame
	"1400 E GSRendererHW::Draw\n"
	"2000 e GSState::FlushPrim\n"
	"3000 d radv_CmdDrawIndexed\n"
	"3200 d radv_emit_cache_flush\n"
	"3300 D radv_CmdPipelineBarrier2\n"
	"4000 l ZSTD_decompress\n"
	"5000 - end\n";

static constexpr uint64_t BASE = 0x400000;

static PS5ProfileSample Sample(uint64_t offset, std::initializer_list<uint64_t> chain_offsets, uint64_t cpu_us = 0,
	bool absolute = false)
{
	PS5ProfileSample sample{};
	sample.rip = absolute ? offset : BASE + offset;
	sample.cpu_us = cpu_us;
	unsigned depth = 0;
	for (const uint64_t value : chain_offsets)
		sample.chain[depth++] = (value >= 0x100000000ull) ? value : BASE + value;
	return sample;
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

static int LineOf(const std::vector<std::string>& lines, const char* text)
{
	for (size_t i = 0; i < lines.size(); i++)
	{
		if (lines[i].find(text) != std::string::npos)
			return static_cast<int>(i);
	}
	return -1;
}

static void TestSymbols()
{
	PS5ProfileSymbols symbols;
	CHECK(symbols.Load(SYMBOLS));
	CHECK(symbols.Count() == 8);
	CHECK(symbols.End() == 0x5000);

	CHECK(symbols.Find(0) == -1);
	CHECK(symbols.Find(0xfff) == -1);
	CHECK(symbols.Find(0x1000) == 0);
	CHECK(symbols.Find(0x10ff) == 0);
	CHECK(symbols.Find(0x1100) == 1);
	CHECK(symbols.Find(0x13ff) == 1);
	CHECK(symbols.Find(0x1400) == 2);
	CHECK(symbols.Find(0x4000) == 7);
	CHECK(symbols.Find(0x4fff) == 7);
	CHECK(symbols.Find(0x5000) == -1);
	CHECK(symbols.Find(UINT64_MAX) == -1);
	CHECK(std::strcmp(symbols.Name(1), "GSDeviceVK::SendHWDraw") == 0);
	CHECK(symbols.Tag(1) == 'e' && symbols.Tag(4) == 'd' && symbols.Tag(0) == 'p' && symbols.Tag(7) == 'l');
	// The letter's case says whether the function has a frame; the tag is the small letter either way.
	CHECK(symbols.Framed(1) && symbols.Framed(2) && !symbols.Framed(3) && !symbols.Framed(4) && !symbols.Framed(0));
	CHECK(symbols.Tag(2) == 'e' && symbols.Tag(3) == 'e' && symbols.Tag(6) == 'd' && symbols.Framed(6));

	// Windows line ends and empty lines are taken; a name may hold spaces.
	CHECK(symbols.Load("10 e a b c\r\n\r\n20 d x\r\n30 - end\r\n"));
	CHECK(symbols.Count() == 2 && std::strcmp(symbols.Name(0), "a b c") == 0);

	// What a file cut short, or not this file at all, looks like.
	CHECK(!symbols.Load(""));
	CHECK(!symbols.Load("1000 e f\n"));                      // no end line
	CHECK(!symbols.Load("1000 - end\n"));                    // no function
	CHECK(!symbols.Load("1000 e f\n900 e g\n2000 - end\n")); // not in order
	CHECK(!symbols.Load("1000 e f\n1000 e g\n2000 - end\n")); // the same address twice
	CHECK(!symbols.Load("1000 e f\n1000 - end\n"));          // the end not past the last function
	CHECK(!symbols.Load("1000 e f\n2000 - end\n3000 e g\n")); // something after the end
	CHECK(!symbols.Load("xyz e f\n2000 - end\n"));
	CHECK(!symbols.Load("1000 e\n2000 - end\n"));
	CHECK(!symbols.Load("1000\n2000 - end\n"));
	CHECK(!symbols.Load("100000000 e f\n200000000 - end\n")); // past 32 bits
	CHECK(symbols.Load("1000 e f\n2000 - end")); // the last line without a line end
	CHECK(symbols.Load(SYMBOLS)); // and it loads again after a failure
	CHECK(symbols.Count() == 8);
}

static void TestFold()
{
	PS5ProfileSymbols symbols;
	CHECK(symbols.Load(SYMBOLS));

	// 10 samples over 10 s, the thread's clock going from 1 s to 9.5 s: 85% busy.
	//   4 in the driver's flush, called from its draw, called from SendHWDraw, from Draw
	//   2 in SendHWDraw itself, called from Draw, from FlushPrim
	//   1 in FlushPrim, calling itself (once on the stack, not twice)
	//   1 in a library
	//   1 outside the program (a system library), reached from radv_CmdDrawIndexed
	//   1 outside the program with nothing of the program on its stack
	std::vector<PS5ProfileSample> samples;
	uint64_t cpu = 1000000;
	const auto next_cpu = [&]() {
		const uint64_t value = cpu;
		cpu += 8500000 / 9;
		return value;
	};
	for (int i = 0; i < 4; i++)
		samples.push_back(Sample(0x3210, {0x3040, 0x1180, 0x1500}, next_cpu()));
	for (int i = 0; i < 2; i++)
		samples.push_back(Sample(0x1104, {0x1410, 0x2020}, next_cpu()));
	samples.push_back(Sample(0x2100, {0x2200, 0x2300}, next_cpu()));
	samples.push_back(Sample(0x4abc, {}, next_cpu()));
	samples.push_back(Sample(0x800012340ull, {0x800012000ull, 0x3050, 0x1180}, next_cpu(), true));
	samples.push_back(Sample(0x800012340ull, {0x800099990ull}, next_cpu(), true));

	std::vector<std::string> lines;
	PS5ProfileFold(samples.data(), samples.size(), symbols, BASE, 10.0, "profile: graphics:", lines);
	for (const std::string& line : lines)
		std::printf("    %s\n", line.c_str());

	CHECK(Has(lines, "profile: graphics: 10.0 s, 10 samples, on a processor 85% of the time"));
	CHECK(Has(lines, "the emulator 30.0%, the graphics driver 40.0%, the platform layer 0.0%, other libraries 10.0%, "
					 "outside the program 20.0%"));

	// Running in: the flush 40%, SendHWDraw 20%, FlushPrim 10%, the library 10%, in that order
	// (equal counts by address).
	const int self_at = LineOf(lines, "running in");
	CHECK(self_at >= 0);
	CHECK(lines[self_at + 1].find(" 40.0%  [d] radv_emit_cache_flush") != std::string::npos);
	CHECK(lines[self_at + 2].find(" 20.0%  [e] GSDeviceVK::SendHWDraw") != std::string::npos);
	CHECK(lines[self_at + 3].find(" 10.0%  [e] GSState::FlushPrim") != std::string::npos);
	CHECK(lines[self_at + 4].find(" 10.0%  [l] ZSTD_decompress") != std::string::npos);

	// On the stack: SendHWDraw in 4 + 2 + 1 = 7 samples, Draw in 4 + 2 = 6, radv_CmdDrawIndexed
	// in 4 + 1 = 5, the flush in 4, FlushPrim in 2 + 1 = 3 (once for the sample it is in twice).
	const int stack_at = LineOf(lines, "on the stack");
	CHECK(stack_at > self_at);
	CHECK(lines[stack_at + 1].find(" 70.0%  [e] GSDeviceVK::SendHWDraw") != std::string::npos);
	CHECK(lines[stack_at + 2].find(" 60.0%  [e] GSRendererHW::Draw") != std::string::npos);
	CHECK(lines[stack_at + 3].find(" 50.0%  [d] radv_CmdDrawIndexed") != std::string::npos);
	CHECK(lines[stack_at + 4].find(" 40.0%  [d] radv_emit_cache_flush") != std::string::npos);
	CHECK(lines[stack_at + 5].find(" 30.0%  [e] GSState::FlushPrim") != std::string::npos);

	// Outside the program: one sample reached from the driver's draw, one from nowhere known.
	const int outside_at = LineOf(lines, "outside the program (system libraries");
	CHECK(outside_at > stack_at);
	CHECK(lines[outside_at + 1].find(" 10.0%  [d] radv_CmdDrawIndexed") != std::string::npos);
	CHECK(lines[outside_at + 2].find(" 10.0%  (no function of the program on the stack)") != std::string::npos);

	// The instructions interrupted most, for the functions the thread ran in: the flush was
	// interrupted four times at the same instruction (0x3210 is 0x10 into it), SendHWDraw twice
	// at +0x4; the two functions sampled once have no instruction interrupted twice.
	const int spots_at = LineOf(lines, "the instructions interrupted most");
	CHECK(spots_at == outside_at + 3);
	CHECK(lines[spots_at + 1] == "profile: graphics:   radv_emit_cache_flush: +0x10 40.0%");
	CHECK(lines[spots_at + 2] == "profile: graphics:   GSDeviceVK::SendHWDraw: +0x4 20.0%");
	CHECK(static_cast<int>(lines.size()) == spots_at + 3);

	// Nothing sampled, and a thread whose clock the console did not give.
	lines.clear();
	PS5ProfileFold(nullptr, 0, symbols, BASE, 10.0, "p:", lines);
	CHECK(lines.size() == 1 && lines[0] == "p: no samples in 10.0 s");
	lines.clear();
	const PS5ProfileSample one = Sample(0x1100, {});
	PS5ProfileFold(&one, 1, symbols, BASE, 10.0, "p:", lines);
	CHECK(Has(lines, "p: 10.0 s, 1 samples") && !Has(lines, "on a processor"));
	CHECK(Has(lines, "100.0%  [e] GSDeviceVK::SendHWDraw"));
	CHECK(!Has(lines, "outside the program (system"));

	// The lists are cut at their limits.
	lines.clear();
	PS5ProfileFold(samples.data(), samples.size(), symbols, BASE, 10.0, "p:", lines, 0, 0, 2, 1, 0, 0);
	const int cut_self = LineOf(lines, "running in"), cut_stack = LineOf(lines, "on the stack");
	CHECK(cut_stack - cut_self == 3);
	CHECK(LineOf(lines, "outside the program (system") == cut_stack + 2);
	CHECK(LineOf(lines, "the instructions interrupted most") < 0);

	// Words found by looking through the stack: none count for a function with a frame of its
	// own, whose frame holds only its own variables; the first counts for one without.
	{
		// A function without a frame of its own (a leaf): the first word found is where it
		// returns to, the rest are its caller's variables.
		PS5ProfileSample in_leaf =
			Sample(0x2100, {0x1410 /* found first: its caller */, 0x3040 /* found next */, 0x1180 /* a frame's */});
		in_leaf.scanned = 2;
		lines.clear();
		PS5ProfileFold(&in_leaf, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "100.0%  [e] GSRendererHW::Draw") && Has(lines, "100.0%  [e] GSDeviceVK::SendHWDraw"));
		CHECK(Has(lines, "100.0%  [e] GSState::FlushPrim") && !Has(lines, "radv_CmdDrawIndexed"));
		// ... and nothing is taken from a count that is larger than the sample is.
		in_leaf.scanned = 1000;
		lines.clear();
		PS5ProfileFold(&in_leaf, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "100.0%  [e] GSRendererHW::Draw") && !Has(lines, "GSDeviceVK::SendHWDraw") &&
			  !Has(lines, "radv_CmdDrawIndexed"));

		// The driver's functions are read the same way: one with a frame by its frame ...
		PS5ProfileSample in_framed_driver = Sample(0x3310, {0x3040 /* found by looking */, 0x1410 /* a frame's */});
		in_framed_driver.scanned = 1;
		lines.clear();
		PS5ProfileFold(&in_framed_driver, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(!Has(lines, "radv_CmdDrawIndexed") && Has(lines, "100.0%  [e] GSRendererHW::Draw"));
		// ... and code outside the program, of which nothing is known, by every word found.
		PS5ProfileSample outside_program = Sample(0x800012340ull, {0x3040, 0x2010, 0x1410 /* a frame's */}, 0, true);
		outside_program.scanned = 2;
		lines.clear();
		PS5ProfileFold(&outside_program, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "100.0%  [d] radv_CmdDrawIndexed") && Has(lines, "100.0%  [e] GSState::FlushPrim") &&
			  Has(lines, "100.0%  [e] GSRendererHW::Draw"));
	}
	{
		PS5ProfileSample in_emulator = Sample(0x1104, {0x3040 /* found by looking */, 0x1410 /* a frame's */});
		in_emulator.scanned = 1;
		lines.clear();
		PS5ProfileFold(&in_emulator, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "100.0%  [e] GSRendererHW::Draw") && !Has(lines, "radv_CmdDrawIndexed"));

		PS5ProfileSample in_driver = Sample(0x3210, {0x3040 /* found by looking */, 0x1410 /* a frame's */});
		in_driver.scanned = 1;
		lines.clear();
		PS5ProfileFold(&in_driver, 1, symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "100.0%  [e] GSRendererHW::Draw") && Has(lines, "100.0%  [d] radv_CmdDrawIndexed"));
	}

	// Several instructions of one function, the most interrupted first; a word from the stack
	// that is a round number (a size, a flag: the program is loaded low) is no return address.
	{
		std::vector<PS5ProfileSample> spread;
		for (int i = 0; i < 5; i++)
			spread.push_back(Sample(0x1120, {0x1000 /* round: left out */, 0x2010}));
		for (int i = 0; i < 3; i++)
			spread.push_back(Sample(0x1108, {}));
		for (int i = 0; i < 2; i++)
			spread.push_back(Sample(0x1300, {}));
		spread.push_back(Sample(0x1101, {})); // once: not listed
		lines.clear();
		PS5ProfileFold(spread.data(), spread.size(), symbols, BASE, 10.0, "p:", lines);
		CHECK(Has(lines, "p:   GSDeviceVK::SendHWDraw: +0x20 45.5%, +0x8 27.3%, +0x200 18.2%"));
		CHECK(Has(lines, " 45.5%  [e] GSState::FlushPrim"));
		CHECK(!Has(lines, "_start"));
	}

	// How many interruptions the samples are of, and the processor time counted from the end of
	// the window before (0.5 s on the clock then: 9.5 s - 0.5 s over 10 s is 90%).
	lines.clear();
	PS5ProfileFold(samples.data(), samples.size(), symbols, BASE, 10.0, "p:", lines, 12, 500000);
	CHECK(Has(lines, "p: 10.0 s, 10 samples of 12 interruptions, on a processor 90% of the time"));

	// An address from the stack is a return address: one that is a function's first byte
	// belongs to the call that ended the function before it.
	lines.clear();
	const PS5ProfileSample at_start = Sample(0x4abc, {0x3200, 0x1400});
	PS5ProfileFold(&at_start, 1, symbols, BASE, 10.0, "p:", lines);
	const int start_stack = LineOf(lines, "on the stack");
	CHECK(start_stack >= 0);
	CHECK(Has(lines, "100.0%  [d] radv_CmdDrawIndexed") && !Has(lines, "radv_emit_cache_flush"));
	CHECK(Has(lines, "100.0%  [e] GSDeviceVK::SendHWDraw") && !Has(lines, "GSRendererHW::Draw"));
	// The interrupted instruction itself is not moved.
	lines.clear();
	const PS5ProfileSample leaf_start = Sample(0x3200, {});
	PS5ProfileFold(&leaf_start, 1, symbols, BASE, 10.0, "p:", lines);
	CHECK(Has(lines, "100.0%  [d] radv_emit_cache_flush"));
}

// Random samples, counted here the slow way, against what the lines say.
static void TestRandom()
{
	std::mt19937_64 random(20261003);
	for (int round = 0; round < 200; round++)
	{
		// A table of 2 to 300 functions.
		std::string text;
		std::vector<uint64_t> starts;
		std::vector<char> tags;
		uint64_t offset = 0x100 + random() % 0x1000;
		const unsigned count = 2 + static_cast<unsigned>(random() % 299);
		for (unsigned i = 0; i < count; i++)
		{
			char line[64];
			const char tag = "edpl"[random() % 4];
			std::snprintf(line, sizeof(line), "%" PRIx64 " %c f%u\n", offset, tag, i);
			text += line;
			starts.push_back(offset);
			tags.push_back(tag);
			offset += 1 + random() % 0x400;
		}
		const uint64_t end = offset;
		char line[64];
		std::snprintf(line, sizeof(line), "%" PRIx64 " - end\n", end);
		text += line;

		PS5ProfileSymbols symbols;
		CHECK(symbols.Load(text));
		CHECK(symbols.Count() == count && symbols.End() == end);

		const auto slow_find = [&](uint64_t address) -> int {
			if (address < BASE)
				return -1;
			const uint64_t at = address - BASE;
			if (at >= end)
				return -1;
			int found = -1;
			for (unsigned i = 0; i < count; i++)
			{
				if (starts[i] <= at)
					found = static_cast<int>(i);
			}
			return found;
		};
		const auto random_address = [&]() -> uint64_t {
			switch (random() % 8)
			{
				case 0: return 0x800000000ull + random() % 0x100000; // a system library
				case 1: return BASE + random() % 0x100;              // before the first function
				case 2: return BASE + end + random() % 0x100;        // past the end
				case 3: return 1 + random() % (BASE - 1);            // below the program
				default: return BASE + starts[random() % count] + random() % 2;
			}
		};

		const size_t sample_count = 1 + random() % 400;
		std::vector<PS5ProfileSample> samples(sample_count);
		std::map<int, unsigned> self, on_stack;
		unsigned by_tag[4] = {0, 0, 0, 0}, outside = 0;
		for (PS5ProfileSample& sample : samples)
		{
			sample = {};
			sample.rip = random_address();
			const unsigned depth = static_cast<unsigned>(random() % (PS5ProfileSample::CHAIN + 1));
			for (unsigned i = 0; i < depth; i++)
				sample.chain[i] = random_address();

			std::map<int, bool> seen;
			const int leaf = slow_find(sample.rip);
			CHECK(leaf == ((sample.rip >= BASE) ? symbols.Find(sample.rip - BASE) : -1));
			if (leaf >= 0)
			{
				self[leaf]++;
				by_tag[std::strchr("edpl", tags[static_cast<size_t>(leaf)]) - "edpl"]++;
				seen[leaf] = true;
			}
			else if (sample.rip >= BASE && sample.rip - BASE < end)
			{
				by_tag[3]++; // the program's, unnamed: with the other libraries
			}
			else
			{
				outside++;
			}
			for (unsigned i = 0; i < depth; i++)
			{
				// (A return address: the byte before it is the caller's. A round number is none.)
				const int function = (((sample.chain[i] - BASE) & 0xfff) != 0) ? slow_find(sample.chain[i] - 1) : -1;
				if (function >= 0)
					seen[function] = true;
			}
			for (const auto& entry : seen)
				on_stack[entry.first]++;
		}

		std::vector<std::string> lines;
		PS5ProfileFold(samples.data(), samples.size(), symbols, BASE, 10.0, "p:", lines, 0, 0, 100000, 100000, 100000, 0);

		const double total = static_cast<double>(sample_count);
		char expected[320];
		std::snprintf(expected, sizeof(expected),
			"where: the emulator %.1f%%, the graphics driver %.1f%%, the platform layer %.1f%%, other libraries %.1f%%, "
			"outside the program %.1f%%",
			100.0 * by_tag[0] / total, 100.0 * by_tag[1] / total, 100.0 * by_tag[2] / total, 100.0 * by_tag[3] / total,
			100.0 * outside / total);
		CHECK(Has(lines, expected));

		// Every function with samples is listed with its share, and no other is.
		const int self_at = LineOf(lines, "running in"), stack_at = LineOf(lines, "on the stack");
		const int outside_at = LineOf(lines, "outside the program (system");
		CHECK(self.empty() == (self_at < 0));
		CHECK(on_stack.empty() == (stack_at < 0));
		if (self_at >= 0 && stack_at >= 0)
		{
			CHECK(static_cast<size_t>(stack_at - self_at - 1) == self.size());
			const int stack_end = (outside_at >= 0) ? outside_at : static_cast<int>(lines.size());
			CHECK(static_cast<size_t>(stack_end - stack_at - 1) == on_stack.size());
			for (const auto& entry : self)
			{
				std::snprintf(expected, sizeof(expected), "p:  %5.1f%%  [%c] f%d", 100.0 * entry.second / total,
					tags[static_cast<size_t>(entry.first)], entry.first);
				bool found = false;
				for (int i = self_at + 1; i < stack_at; i++)
					found |= (lines[static_cast<size_t>(i)] == expected);
				CHECK(found);
			}
			for (const auto& entry : on_stack)
			{
				std::snprintf(expected, sizeof(expected), "p:  %5.1f%%  [%c] f%d", 100.0 * entry.second / total,
					tags[static_cast<size_t>(entry.first)], entry.first);
				bool found = false;
				for (int i = stack_at + 1; i < stack_end; i++)
					found |= (lines[static_cast<size_t>(i)] == expected);
				CHECK(found);
			}
			// Largest first.
			for (int i = self_at + 2; i < stack_at; i++)
				CHECK(std::atof(lines[static_cast<size_t>(i - 1)].c_str() + 3) >= std::atof(lines[static_cast<size_t>(i)].c_str() + 3));
		}
	}
}

int main()
{
	TestSymbols();
	TestFold();
	TestRandom();
	std::printf("profile folding: %u checks, %u failed\n", s_checks, s_failures);
	return (s_failures == 0) ? 0 : 1;
}
