// Crux2 - a texture pack fetched in pieces (frontend/ps5_texture_packs_pieces.h), on a PC: made-up
// connections that finish out of order, fail, are slow, and a reading that is stopped.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_texture_packs_pieces.h"

#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace PS5TexturePacksPieces;

namespace
{
int g_checks = 0, g_failed = 0;
void Expect(bool ok, const std::string& what)
{
	g_checks++;
	if (!ok)
	{
		g_failed++;
		std::fprintf(stderr, "FAILED: %s\n", what.c_str());
	}
}

// The byte at an offset of the made-up archive.
char ByteAt(uint64_t at)
{
	return static_cast<char>((at * 2654435761u) >> 13);
}

// Everything the reading gets, in reads of odd sizes; false when a read failed (-1).
bool ReadAll(Assembly& a, std::vector<char>* out, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<char> buffer(70000);
	for (;;)
	{
		const size_t want = 1 + rng() % buffer.size();
		const long n = a.Read(buffer.data(), want);
		if (n < 0)
			return false;
		if (n == 0)
			return true;
		out->insert(out->end(), buffer.data(), buffer.data() + n);
	}
}

bool Same(const std::vector<char>& got, uint64_t total)
{
	if (got.size() != total)
		return false;
	for (uint64_t i = 0; i < total; i++)
	{
		if (got[i] != ByteAt(i))
			return false;
	}
	return true;
}

// A connection that brings what is asked in pieces of a random size, after random waits (so the
// pieces finish out of order).
struct Honest
{
	Assembly* a;
	unsigned seed;
	std::atomic<unsigned>* attempts;
	AttemptResult operator()(size_t index, uint64_t from, uint64_t to, unsigned, bool)
	{
		std::mt19937 rng(seed + static_cast<unsigned>(index) * 7919u);
		attempts->fetch_add(1);
		std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
		std::vector<char> data;
		for (uint64_t at = from; at <= to;)
		{
			const uint64_t n = std::min<uint64_t>(to + 1 - at, 1 + rng() % 9000);
			data.resize(n);
			for (uint64_t k = 0; k < n; k++)
				data[k] = ByteAt(at + k);
			if (!a->Append(index, data.data(), n))
				return {Attempt::Failed, "ended"};
			at += n;
			if (rng() % 4 == 0)
				std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
		}
		return {Attempt::Brought, {}};
	}
};

void Run(Assembly& a, unsigned connections, const std::function<AttemptResult(size_t, uint64_t, uint64_t, unsigned, bool)>& fetch, const Rules& rules,
	Counters* counters, std::vector<char>* got, bool* read_ok, unsigned seed)
{
	std::vector<std::thread> threads;
	for (unsigned k = 0; k < connections; k++)
		threads.emplace_back([&] { Work(a, fetch, rules, counters); });
	*read_ok = ReadAll(a, got, seed);
	a.Gone();
	for (std::thread& t : threads)
		t.join();
}

void ContentRanges()
{
	const ContentRange r = ParseContentRange(" bytes 0-0/123456\r\n");
	Expect(r.ok && r.from == 0 && r.to == 0 && r.total == 123456, "a Content-Range read");
	Expect(ParseContentRange("bytes 100-199/1000").from == 100 && ParseContentRange("bytes 100-199/1000").to == 199, "a Content-Range of a part");
	Expect(ParseContentRange("bytes 0-0/*").ok && ParseContentRange("bytes 0-0/*").total == 0, "a size the server does not know");
	Expect(!ParseContentRange("bytes 5-4/10").ok && !ParseContentRange("bytes 0-10/10").ok && !ParseContentRange("items 0-0/10").ok &&
			   !ParseContentRange("bytes */10").ok && !ParseContentRange("").ok && !ParseContentRange("bytes 0-0/99999999999999999999999").ok,
		"Content-Ranges refused");
	Expect(RangedSize(206, "bytes 0-0/5000", 5000) == 5000, "a server that sends parts: in pieces");
	Expect(RangedSize(206, "bytes 0-0/5000", 0) == 5000, "the size from the server when the list has none");
	Expect(RangedSize(200, "", 5000) == 0, "a server that sends the whole file (200): one stream");
	Expect(RangedSize(206, "", 5000) == 0, "206 without a Content-Range: one stream");
	Expect(RangedSize(206, "bytes 0-0/*", 5000) == 0, "a size the server does not know: one stream");
	Expect(RangedSize(206, "bytes 0-0/4999", 5000) == 0, "a size that is not the list's: one stream");
	Expect(RangedSize(206, "bytes 1-1/5000", 5000) == 0, "not the first byte: one stream");
	Expect(WindowFor(10) == 12 && WindowFor(1) == 3 && WindowFor(99) == CONNECTIONS_MOST + 2 && WindowFor(0) == 3, "the window");
}

// Pieces that finish in any order come out in order, and no more than the window is held.
void InOrder()
{
	for (const unsigned connections : {1u, 3u, 10u, 16u})
	{
		for (const uint64_t total : {uint64_t(1), uint64_t(4096), uint64_t(4096 * 37 + 1234), uint64_t(1 << 20)})
		{
			const size_t window = WindowFor(connections);
			Assembly a(total, 4096, window, nullptr);
			std::atomic<unsigned> attempts{0};
			Honest h{&a, connections * 31 + static_cast<unsigned>(total), &attempts};
			Counters counters;
			std::vector<char> got;
			bool ok = false;
			Rules rules;
			Run(a, connections, h, rules, &counters, &got, &ok, 5);
			const std::string what = std::to_string(total) + " bytes over " + std::to_string(connections);
			Expect(ok && Same(got, total), what + ": every byte, in order");
			Expect(a.MostHeld() <= window + 1, what + ": at most window + 1 pieces held (" + std::to_string(a.MostHeld()) + ")");
			Expect(a.Held() == 0 && !a.Failed() && counters.pieces == a.Count() && attempts.load() == a.Count(), what + ": each piece once");
		}
	}
}

// No piece is taken past the window until the one being read is read.
void Window()
{
	const size_t window = 3;
	Assembly a(10 * 100, 100, window, nullptr);
	for (size_t i = 0; i <= window; i++)
		Expect(a.Claim() == std::optional<size_t>(i), "a piece in the window taken at once");
	std::atomic<long> late{-2};
	std::thread t([&] {
		const std::optional<size_t> i = a.Claim();
		late = i ? static_cast<long>(*i) : -1;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	Expect(late.load() == -2, "the next piece waits while the window is full");
	// Piece 1 whole first: still nothing (piece 0 is the one being read).
	std::vector<char> data(100, 'x');
	a.Append(1, data.data(), 100);
	a.Complete(1);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	Expect(late.load() == -2, "a later piece whole does not open the window");
	a.Append(0, data.data(), 60);
	char buffer[200];
	Expect(a.Read(buffer, sizeof(buffer)) == 60, "the first piece read as it comes");
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	Expect(late.load() == -2, "a piece half read keeps the window");
	a.Append(0, data.data(), 40);
	a.Complete(0);
	Expect(a.Read(buffer, sizeof(buffer)) == 40, "the rest of the first piece");
	t.join();
	Expect(late.load() == static_cast<long>(window + 1), "the window opened by the piece read");
	Expect(!a.Append(2, buffer, 101), "more than a piece holds is refused");
	a.Gone();
	Expect(!a.Claim().has_value() && !a.Append(2, buffer, 1), "nothing taken once the reading is gone");
}

// Connections that fail: the piece is asked for again from what it has.
void Retries()
{
	const uint64_t total = 4096 * 20 + 77;
	Assembly a(total, 4096, WindowFor(4), nullptr);
	std::mutex m;
	std::vector<std::string> asked;
	std::vector<int> tries(a.Count(), 0);
	bool resumed_right = true, failures_right = true;
	const auto fetch = [&](size_t index, uint64_t from, uint64_t to, unsigned failures, bool) -> AttemptResult {
		int attempt;
		{
			const std::lock_guard lock(m);
			attempt = tries[index]++;
			if (from != a.Offset(index) + a.Have(index))
				resumed_right = false;
		}
		std::vector<char> data;
		const auto give = [&](uint64_t n) {
			data.resize(n);
			for (uint64_t k = 0; k < n; k++)
				data[k] = ByteAt(from + k);
			a.Append(index, data.data(), n);
		};
		switch (index % 4)
		{
			case 0: // cut off halfway, the first time
				if (attempt == 0)
				{
					give((to + 1 - from) / 2);
					return {Attempt::Failed, "cut off"};
				}
				break;
			case 1: // nothing twice, with a pause before each again
				if (attempt < 2)
					return {Attempt::Failed, "refused"};
				if (failures != 2)
				{
					const std::lock_guard lock(m);
					failures_right = false;
				}
				break;
			case 2: // left for being slow, a third in
				if (attempt == 0)
				{
					give((to + 1 - from) / 3);
					return {Attempt::Slow, {}};
				}
				break;
			default: // ends early without an error
				if (attempt == 0)
				{
					give(10);
					return {Attempt::Brought, {}};
				}
				break;
		}
		give(to + 1 - from);
		return {Attempt::Brought, {}};
	};
	Rules rules;
	rules.pause_ms = 1;
	Counters counters;
	std::vector<char> got;
	bool ok = false;
	Run(a, 4, fetch, rules, &counters, &got, &ok, 9);
	Expect(ok && Same(got, total), "every byte, after the failures");
	Expect(resumed_right, "each attempt asks from the bytes the piece has");
	Expect(failures_right, "the attempts that brought nothing are counted");
	Expect(counters.slow == 5 && counters.pieces == a.Count(), "the slow pieces counted (" + std::to_string(counters.slow) + ")");
	Expect(counters.retries == 6 + 2 * 5 + 5, "the retries counted (" + std::to_string(counters.retries) + ")");
}

// A piece that cannot be had fails the download, with the reason; the reading gets -1.
void Fails()
{
	Assembly a(4096 * 8, 4096, WindowFor(3), nullptr);
	std::atomic<unsigned> calls{0};
	const auto fetch = [&](size_t index, uint64_t from, uint64_t to, unsigned, bool) -> AttemptResult {
		calls++;
		if (index == 5)
			return {Attempt::Failed, "HTTP 503"};
		std::vector<char> data(to + 1 - from);
		for (uint64_t k = 0; k < data.size(); k++)
			data[k] = ByteAt(from + k);
		a.Append(index, data.data(), data.size());
		return {Attempt::Brought, {}};
	};
	Rules rules;
	rules.pause_ms = 1;
	rules.retries = 3;
	Counters counters;
	std::vector<char> got;
	bool ok = true;
	Run(a, 3, fetch, rules, &counters, &got, &ok, 3);
	Expect(!ok && a.Failed() && a.Error() == "HTTP 503", "the download failed, with the reason");
	Expect(got.size() <= 5 * 4096 && Same(got, got.size()), "what came before is in order");
	Expect(counters.retries == 4, "a piece asked for 1 + retries times (" + std::to_string(counters.retries) + ")");
}

// What fails after the reading has gone (the archive's end was read) is no failure.
void FailsAfterTheEnd()
{
	Assembly a(1000, 100, 3, nullptr);
	a.Gone();
	a.Fail("cut off");
	Expect(!a.Failed() && a.Error().empty(), "a failure after the end of the reading is none");
}

// Stopped halfway: the reading gets -1, the connections end, nothing hangs.
void Stops()
{
	for (const unsigned connections : {1u, 10u})
	{
		std::atomic_bool stop{false};
		const uint64_t total = 4096 * 200;
		Assembly a(total, 4096, WindowFor(connections), [&stop] { return stop.load(); });
		std::atomic<unsigned> attempts{0};
		Honest h{&a, 77, &attempts};
		Counters counters;
		std::vector<std::thread> threads;
		for (unsigned k = 0; k < connections; k++)
			threads.emplace_back([&] { Work(a, h, Rules(), &counters); });
		std::vector<char> buffer(10000);
		uint64_t read = 0;
		long n = 0;
		while ((n = a.Read(buffer.data(), buffer.size())) > 0)
		{
			read += static_cast<uint64_t>(n);
			if (read >= total / 3)
				stop = true;
		}
		Expect(n == -1, "a stopped reading gets -1");
		for (std::thread& t : threads)
			t.join();
		Expect(a.Ended() && !a.Claim().has_value(), "no piece taken once stopped");
		Expect(read < total && counters.pieces < a.Count(), "stopped before the end (" + std::to_string(connections) + ")");
	}
}

void Speeds()
{
	SpeedMeter m(4.0);
	Expect(m.BytesPerSecond() == 0.0, "no speed before readings");
	for (int i = 0; i <= 40; i++)
		m.Add(i * 0.1, static_cast<uint64_t>(i) * 100000); // 1 MB/s
	Expect(std::abs(m.BytesPerSecond() - 1e6) < 1e3, "a steady speed");
	for (int i = 41; i <= 100; i++)
		m.Add(i * 0.1, 4000000 + static_cast<uint64_t>(i - 40) * 300000); // 3 MB/s from 4 s on
	Expect(std::abs(m.BytesPerSecond() - 3e6) < 0.1e6, "the speed of the last seconds (" + std::to_string(m.BytesPerSecond()) + ")");
	Expect(SecondsLeft(0, 3000000, 1e6) == 3.0 && SecondsLeft(5, 0, 1e6) < 0 && SecondsLeft(5, 10, 0) < 0, "seconds left");
	Expect(TimeLeftText(45) == "45 s" && TimeLeftText(600) == "10 min" && TimeLeftText(7500) == "2 h 05 min" && TimeLeftText(-1).empty(),
		"time left, written");

	RecentSpeeds r;
	r.Add(5);
	r.Add(1);
	Expect(r.Median() == 0.0, "no median of two");
	r.Add(9);
	r.Add(3);
	r.Add(7);
	Expect(r.Median() == 5.0, "the median");
	for (int i = 0; i < 20; i++)
		r.Add(100);
	Expect(r.Median() == 100.0, "the last sixteen");

	// A connection that starts fast and slows down to a tenth.
	AttemptSpeed a(0.0);
	uint64_t bytes = 0;
	double left_at = -1;
	for (double t = 0.1; t < 30 && left_at < 0; t += 0.1)
	{
		bytes += (t < 8.0) ? 300000 : 30000; // 3 MB/s, then 0.3 MB/s
		a.Sample(t, bytes);
		if (a.Slow(t, 3e6))
			left_at = t;
	}
	Expect(left_at > 8.0 + AttemptSpeed::BELOW && left_at < 8.0 + AttemptSpeed::BELOW + 3.5, "a connection slowed down is left (" + std::to_string(left_at) + " s)");
	AttemptSpeed b(0.0);
	bool any = false;
	bytes = 0;
	for (double t = 0.1; t < 30; t += 0.1)
	{
		bytes += 120000; // 1.2 MB/s: above a third of 3
		b.Sample(t, bytes);
		any = any || b.Slow(t, 3e6) || b.Slow(t, 0.0);
	}
	Expect(!any, "a connection at more than a third of the median, or with none known, is let run");
	AttemptSpeed c(0.0);
	any = false;
	for (double t = 0.1; t < AttemptSpeed::SETTLE; t += 0.1)
	{
		c.Sample(t, 0);
		any = any || c.Slow(t, 3e6);
	}
	Expect(!any, "a new connection is given its first seconds");
}
} // namespace

int main()
{
	ContentRanges();
	InOrder();
	Window();
	Retries();
	Fails();
	FailsAfterTheEnd();
	Stops();
	Speeds();
	if (g_failed)
	{
		std::printf("pieces: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("pieces: %d checks passed\n", g_checks);
	return 0;
}
