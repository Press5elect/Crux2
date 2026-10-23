// Crux2 - what a sampled profile is folded into: functions, by name.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The profiler (ps5_profiler.cpp) interrupts a thread every two milliseconds and notes where it
// was: the instruction it ran, and return addresses found on its stack. This file turns ten
// seconds of such notes into lines a person can read: which functions the thread spent its time
// in, and how much of it was the emulator's code, the graphics driver's, and neither.
//
// The names come from symbols.txt in the title folder, which the link writes from the same
// program the title is (ps5/tools/make-symbols.py): one line a function, its offset from the
// program's first byte, where it comes from, and its name:
//
//     2b7a80 e GSDeviceVK::SendHWDraw
//     19602c0 d radv_CmdDrawIndexed
//     1f9c000 - end
//
// e: the emulator and this port; d: the graphics driver (RADV, Mesa); p: the platform layer and
// the C runtime; l: every other library. A capital letter (E, D, P, L): the function begins by
// setting up a frame ("push rbp; mov rbp, rsp"), which says how its stack may be read (see
// PS5ProfileFold). The last line is where the program's code ends.
//
// No PCSX2 types and nothing of the console here, so the PC-side test
// (ps5/tests/profile_fold_test.cpp) builds it alone.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// One interruption of one thread.
struct PS5ProfileSample
{
	static constexpr unsigned CHAIN = 12;

	/// The interrupted instruction.
	uint64_t rip;
	/// The thread's processor time so far, in microseconds (0 if the console does not say).
	uint64_t cpu_us;
	/// Addresses from its stack, nearest caller first; 0 ends them. First the words between the
	/// stack pointer and the frame that look like addresses of the program, then the return
	/// address of each frame.
	uint64_t chain[CHAIN];
	/// How many of the first entries of `chain` were found by looking through the stack between
	/// the stack pointer and the frame (the rest are the frames' own return addresses).
	uint32_t scanned;
	uint32_t reserved;
};

class PS5ProfileSymbols
{
public:
	/// Reads symbols.txt's text. False when it holds no function or does not end with its "end"
	/// line (a file cut short would name the wrong functions).
	bool Load(std::string_view text)
	{
		m_offsets.clear();
		m_tags.clear();
		m_framed.clear();
		m_name_at.clear();
		m_names.clear();
		m_end = 0;

		bool ended = false;
		uint64_t last = 0;
		while (!text.empty())
		{
			const size_t newline = text.find('\n');
			std::string_view line = text.substr(0, newline);
			text = (newline == std::string_view::npos) ? std::string_view() : text.substr(newline + 1);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (line.empty())
				continue;
			if (ended)
				return false; // something after the end line

			// "<hex offset> <tag> <name>"
			uint64_t offset = 0;
			size_t at = 0;
			for (; at < line.size() && line[at] != ' '; at++)
			{
				const char c = line[at];
				const int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
				if (digit < 0 || offset > (UINT64_MAX >> 4))
					return false;
				offset = (offset << 4) | static_cast<unsigned>(digit);
			}
			if (at == 0 || at + 3 > line.size() || line[at + 2] != ' ')
				return false;
			char tag = line[at + 1];
			const bool framed = (tag >= 'A' && tag <= 'Z');
			if (framed)
				tag = static_cast<char>(tag - 'A' + 'a');
			const std::string_view name = line.substr(at + 3);
			if (name.empty() || offset > UINT32_MAX || (!m_offsets.empty() && offset <= last))
				return false;

			if (tag == '-')
			{
				m_end = offset;
				ended = true;
				continue;
			}
			last = offset;
			m_offsets.push_back(static_cast<uint32_t>(offset));
			m_tags.push_back(tag);
			m_framed.push_back(framed ? 1 : 0);
			m_name_at.push_back(static_cast<uint32_t>(m_names.size()));
			m_names.append(name);
			m_names.push_back('\0');
		}
		return ended && !m_offsets.empty();
	}

	size_t Count() const { return m_offsets.size(); }

	/// Where the program's code ends, as an offset from its first byte.
	uint64_t End() const { return m_end; }

	/// The function `offset` is in, or -1 when it is before the first one or past the end.
	int Find(uint64_t offset) const
	{
		if (offset >= m_end || m_offsets.empty() || offset < m_offsets.front())
			return -1;
		const auto next = std::upper_bound(m_offsets.begin(), m_offsets.end(), static_cast<uint32_t>(offset));
		return static_cast<int>(next - m_offsets.begin()) - 1;
	}

	const char* Name(int index) const { return m_names.c_str() + m_name_at[static_cast<size_t>(index)]; }
	/// Where the function starts, as an offset from the program's first byte.
	uint32_t Start(int index) const { return m_offsets[static_cast<size_t>(index)]; }
	/// Where it comes from: 'e', 'd', 'p' or 'l' (always the small letter).
	char Tag(int index) const { return m_tags[static_cast<size_t>(index)]; }
	/// Whether the function begins by setting up a frame.
	bool Framed(int index) const { return m_framed[static_cast<size_t>(index)] != 0; }

private:
	std::vector<uint32_t> m_offsets;
	std::vector<char> m_tags;
	std::vector<uint8_t> m_framed;
	std::vector<uint32_t> m_name_at;
	std::string m_names;
	uint64_t m_end = 0;
};

/// Folds one thread's samples of one window into lines (no line ends; each starts with `prefix`).
/// `base` is the address of the program's first byte, `seconds` the window's length, `sent` how
/// many times the thread was interrupted for these samples (0: not counted), and `cpu_before`
/// the thread's processor time at the start of the window (0: not known; the first sample's is
/// used, which leaves out the time before it).
/// `self_top`, `stack_top` and `caller_top` bound the three lists; for the `spot_functions`
/// functions the thread ran in most, the instructions it was interrupted at most are listed too.
inline void PS5ProfileFold(const PS5ProfileSample* samples, size_t count, const PS5ProfileSymbols& symbols, uint64_t base,
	double seconds, const char* prefix, std::vector<std::string>& lines, size_t sent = 0, uint64_t cpu_before = 0,
	unsigned self_top = 25, unsigned stack_top = 12, unsigned caller_top = 8, unsigned spot_functions = 8)
{
	char text[320];
	if (count == 0)
	{
		std::snprintf(text, sizeof(text), "%s no samples in %.1f s", prefix, seconds);
		lines.emplace_back(text);
		return;
	}

	const size_t functions = symbols.Count();
	// For each function: the samples it was running in, those it was on the stack of (itself
	// running included, counted once a sample), and those where the thread was outside the
	// program with this function the nearest of the program's on the stack.
	std::vector<uint32_t> self(functions, 0), on_stack(functions, 0), outside_from(functions, 0);
	uint32_t outside = 0, outside_unknown = 0, unnamed = 0;
	uint32_t by_tag[4] = {0, 0, 0, 0}; // e, d, p, l

	const auto function_of = [&](uint64_t address) -> int {
		return (address >= base) ? symbols.Find(address - base) : -1;
	};
	// An address from the stack is where a call returns to: the instruction after the call,
	// which for a call that ends its function is the next function's first. The byte before it
	// is in the caller.
	// A word that only looks like one is left out where that can be told: the program is loaded
	// low (at 4 MiB on the console), so sizes and flags on a stack fall inside it, and those are
	// mostly round numbers, which a return address almost never is.
	const auto caller_of = [&](uint64_t address) -> int {
		return (address > base && ((address - base) & 0xfff) != 0) ? symbols.Find(address - base - 1) : -1;
	};
	const auto in_program = [&](uint64_t address) { return address >= base && address - base < symbols.End(); };

	for (size_t i = 0; i < count; i++)
	{
		const PS5ProfileSample& sample = samples[i];
		int seen[PS5ProfileSample::CHAIN + 1];
		unsigned seen_count = 0;

		const int leaf = function_of(sample.rip);
		if (leaf >= 0)
		{
			self[static_cast<size_t>(leaf)]++;
			switch (symbols.Tag(leaf))
			{
				case 'e': by_tag[0]++; break;
				case 'd': by_tag[1]++; break;
				case 'p': by_tag[2]++; break;
				default: by_tag[3]++; break;
			}
			seen[seen_count++] = leaf;
		}
		else if (in_program(sample.rip))
		{
			unnamed++; // the program's code, before its first named function
		}
		else
		{
			outside++;
		}

		// The words found by looking through the stack (the first `scanned` entries), by what was
		// interrupted:
		//   a function of the program with a frame of its own: none of them. Between its stack
		//     pointer and its frame are only its own variables, and what an earlier, deeper call
		//     left in the ones it has not written yet. (On the console those put a function that
		//     had returned on a fifth of the stacks.)
		//   a function of the program without one (the compiler leaves the frame out of functions
		//     that call nothing, in the emulator and in the driver alike): the first, which is
		//     where it returns to; the frame is still its caller's, and the words after the first
		//     are that caller's variables.
		//   code outside the program (the system's libraries, generated code), of which nothing is
		//     known: all of them, which is the only way its caller is found.
		const unsigned scanned = std::min<unsigned>(sample.scanned, PS5ProfileSample::CHAIN);
		unsigned scanned_kept = scanned;
		if (leaf >= 0)
			scanned_kept = symbols.Framed(leaf) ? 0 : std::min(scanned, 1u);
		bool caller_noted = (leaf >= 0 || in_program(sample.rip));
		for (unsigned depth = 0; depth < PS5ProfileSample::CHAIN && sample.chain[depth] != 0; depth++)
		{
			if (depth >= scanned_kept && depth < scanned)
				continue;
			const int function = caller_of(sample.chain[depth]);
			if (function < 0)
				continue;
			if (!caller_noted)
			{
				outside_from[static_cast<size_t>(function)]++;
				caller_noted = true;
			}
			bool repeated = false;
			for (unsigned j = 0; j < seen_count; j++)
				repeated |= (seen[j] == function);
			if (!repeated)
				seen[seen_count++] = function;
		}
		if (!caller_noted)
			outside_unknown++;
		for (unsigned j = 0; j < seen_count; j++)
			on_stack[static_cast<size_t>(seen[j])]++;
	}

	const double total = static_cast<double>(count);
	const auto percent = [&](uint32_t n) { return 100.0 * static_cast<double>(n) / total; };

	// How many of the interruptions became samples (one that finds the thread deep in the kernel
	// waits, and several then become one), and how busy the thread was: its own processor time
	// across the window, when the console gave it.
	char taken[64] = "";
	if (sent != 0)
		std::snprintf(taken, sizeof(taken), " of %zu interruptions", sent);
	const uint64_t cpu_first = (cpu_before != 0) ? cpu_before : samples[0].cpu_us, cpu_last = samples[count - 1].cpu_us;
	if (cpu_first != 0 && cpu_last > cpu_first && seconds > 0.0)
	{
		std::snprintf(text, sizeof(text), "%s %.1f s, %zu samples%s, on a processor %.0f%% of the time", prefix, seconds, count,
			taken, std::min(100.0, static_cast<double>(cpu_last - cpu_first) / (seconds * 10000.0)));
	}
	else
	{
		std::snprintf(text, sizeof(text), "%s %.1f s, %zu samples%s", prefix, seconds, count, taken);
	}
	lines.emplace_back(text);

	std::snprintf(text, sizeof(text),
		"%s where: the emulator %.1f%%, the graphics driver %.1f%%, the platform layer %.1f%%, other libraries %.1f%%, "
		"outside the program %.1f%%",
		prefix, percent(by_tag[0]), percent(by_tag[1]), percent(by_tag[2]), percent(by_tag[3] + unnamed), percent(outside));
	lines.emplace_back(text);

	// The lists: the largest counts first, equal counts by address so that a list is the same
	// every time.
	const auto list = [&](const std::vector<uint32_t>& counts, unsigned top, const char* heading, bool heading_alone = false) {
		std::vector<uint32_t> order;
		for (size_t i = 0; i < functions; i++)
		{
			if (counts[i] != 0)
				order.push_back(static_cast<uint32_t>(i));
		}
		if (order.empty() && !heading_alone)
			return;
		std::sort(order.begin(), order.end(),
			[&](uint32_t a, uint32_t b) { return (counts[a] != counts[b]) ? counts[a] > counts[b] : a < b; });
		std::snprintf(text, sizeof(text), "%s %s", prefix, heading);
		lines.emplace_back(text);
		for (size_t i = 0; i < order.size() && i < top; i++)
		{
			const int function = static_cast<int>(order[i]);
			std::snprintf(text, sizeof(text), "%s  %5.1f%%  [%c] %s", prefix, percent(counts[order[i]]), symbols.Tag(function),
				symbols.Name(function));
			lines.emplace_back(text);
		}
	};

	list(self, self_top, "running in (the function itself):");
	list(on_stack, stack_top, "on the stack (the function and what it calls, where the stack could be read):");
	if (outside != 0)
	{
		// (The heading also when no caller is known: the line under it then says so.)
		list(outside_from, caller_top, "outside the program (system libraries, generated code, waiting), reached from:", true);
		if (outside_unknown != 0)
		{
			std::snprintf(text, sizeof(text), "%s  %5.1f%%  (no function of the program on the stack)", prefix,
				percent(outside_unknown));
			lines.emplace_back(text);
		}
	}

	// Where inside the busiest functions: the instructions the thread was interrupted at most,
	// as offsets into the function, to be read against the build's disassembly. A function that
	// is busy all over has no line here that stands out; one that waits on a single instruction
	// (a division, a store that has to wait, a load that misses the cache) has.
	if (spot_functions != 0)
	{
		constexpr unsigned SPOTS = 6;
		std::vector<uint32_t> busiest;
		for (size_t i = 0; i < functions; i++)
		{
			if (self[i] * 100ull >= count) // 1% of the samples or more
				busiest.push_back(static_cast<uint32_t>(i));
		}
		std::sort(busiest.begin(), busiest.end(),
			[&](uint32_t a, uint32_t b) { return (self[a] != self[b]) ? self[a] > self[b] : a < b; });
		if (busiest.size() > spot_functions)
			busiest.resize(spot_functions);

		bool heading = false;
		std::vector<std::pair<uint32_t, uint32_t>> spots; // offset into the function, samples
		for (const uint32_t function : busiest)
		{
			spots.clear();
			for (size_t i = 0; i < count; i++)
			{
				if (function_of(samples[i].rip) != static_cast<int>(function))
					continue;
				const uint32_t offset = static_cast<uint32_t>(samples[i].rip - base) - symbols.Start(static_cast<int>(function));
				bool found = false;
				for (auto& spot : spots)
				{
					if (spot.first == offset)
					{
						spot.second++;
						found = true;
						break;
					}
				}
				if (!found)
					spots.emplace_back(offset, 1u);
			}
			std::sort(spots.begin(), spots.end(),
				[](const auto& a, const auto& b) { return (a.second != b.second) ? a.second > b.second : a.first < b.first; });

			std::string line = std::string(prefix) + "   " + symbols.Name(static_cast<int>(function)) + ":";
			unsigned listed = 0;
			for (const auto& spot : spots)
			{
				if (listed == SPOTS || spot.second < 2)
					break;
				std::snprintf(text, sizeof(text), "%s +0x%x %.1f%%", listed ? "," : "", spot.first, percent(spot.second));
				line += text;
				listed++;
			}
			if (listed == 0)
				continue;
			if (!heading)
			{
				heading = true;
				std::snprintf(text, sizeof(text), "%s the instructions interrupted most (offset into the function, share of all samples):",
					prefix);
				lines.emplace_back(text);
			}
			lines.push_back(std::move(line));
		}
	}
}
