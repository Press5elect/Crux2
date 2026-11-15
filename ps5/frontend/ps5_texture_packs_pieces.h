// Crux2 - HD texture packs: an archive fetched in pieces over several connections at once and
// handed to the unpacking in order.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of curl, PCSX2 or the console here: ps5/tests/texturepacks/pieces_test.cpp runs it on a
// PC with made-up connections, and ps5_texture_packs_fetch.h puts curl under it.
//
// The archive is cut into pieces (4 MiB), each asked for with an HTTP range. A connection takes the
// next piece nobody has (Assembly::Claim), fills it (Append) and gives it back (Complete); the
// unpacking reads the pieces in their order (Read), the first one as it comes, and a piece read to
// its end is let go. A piece that arrives before its turn waits in memory, and no piece is taken
// further than `window` pieces past the one being read: what the download holds is at most
// window + 1 pieces (14, 56 MiB, with 10 connections), whatever the connections do.
//
// A piece whose connection fails is asked for again from the bytes it has (Work: a few times, with
// a pause; an attempt that brought something is not counted against it). The Internet Archive
// starts a connection fast and slows it down as it goes on, so a piece goes on a connection of its
// own, and a piece much slower than the pieces before it (AttemptSpeed::Slow) is left and asked for
// again, from its bytes, on a new one.

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace PS5TexturePacksPieces
{
inline constexpr uint64_t PIECE = 4u << 20;
inline constexpr unsigned CONNECTIONS_DEFAULT = 10, CONNECTIONS_MOST = 16;

/// How many pieces past the one being read may be asked for, with `connections` at once.
inline size_t WindowFor(unsigned connections)
{
	return static_cast<size_t>(std::clamp(connections, 1u, CONNECTIONS_MOST)) + 2;
}

// ---- the first answer: does the server send ranges?

/// "bytes 0-0/1234" (Content-Range). total 0 when the server says "*" (it does not know).
struct ContentRange
{
	bool ok = false;
	uint64_t from = 0, to = 0, total = 0;
};

inline ContentRange ParseContentRange(std::string_view v)
{
	ContentRange r;
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
		v.remove_prefix(1);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
		v.remove_suffix(1);
	if (v.size() < 6 || v.substr(0, 6) != "bytes ")
		return r;
	v.remove_prefix(6);
	const auto number = [&v](uint64_t* out) {
		if (v.empty() || v.front() < '0' || v.front() > '9')
			return false;
		uint64_t n = 0;
		while (!v.empty() && v.front() >= '0' && v.front() <= '9')
		{
			if (n > (UINT64_MAX - 9) / 10)
				return false;
			n = n * 10 + static_cast<uint64_t>(v.front() - '0');
			v.remove_prefix(1);
		}
		*out = n;
		return true;
	};
	if (!number(&r.from) || v.empty() || v.front() != '-')
		return r;
	v.remove_prefix(1);
	if (!number(&r.to) || v.empty() || v.front() != '/' || r.to < r.from)
		return r;
	v.remove_prefix(1);
	if (v == "*")
		r.total = 0;
	else if (!number(&r.total) || !v.empty() || r.to >= r.total)
		return r;
	r.ok = true;
	return r;
}

/// The answer to the first request, a range of the first byte ("bytes=0-0"): the size to fetch in
/// pieces, or 0 for one stream as before (no 206, no Content-Range of the first byte, a size the
/// server does not know, or one that is not the collection's).
inline uint64_t RangedSize(long http_code, std::string_view content_range, uint64_t listed_size)
{
	if (http_code != 206)
		return 0;
	const ContentRange r = ParseContentRange(content_range);
	if (!r.ok || r.from != 0 || r.to != 0 || r.total == 0)
		return 0;
	if (listed_size != 0 && listed_size != r.total)
		return 0;
	return r.total;
}

// ---- the pieces and their order

class Assembly
{
public:
	/// `stop` is asked often (it may be called from any thread); nothing need wake the waits.
	Assembly(uint64_t total, uint64_t piece, size_t window, std::function<bool()> stop)
		: m_total(total), m_piece(std::max<uint64_t>(piece, 1)), m_window(std::max<size_t>(window, 1)), m_stop(std::move(stop))
	{
		m_pieces.resize(static_cast<size_t>((m_total + m_piece - 1) / m_piece));
	}

	size_t Count() const { return m_pieces.size(); }
	uint64_t Total() const { return m_total; }
	uint64_t Offset(size_t i) const { return static_cast<uint64_t>(i) * m_piece; }
	uint64_t Length(size_t i) const { return std::min(m_piece, m_total - Offset(i)); }

	/// The next piece nobody has: waits while it is more than `window` past the one being read.
	/// None when every piece is taken, or the download ended (stopped, failed, the reading gone).
	std::optional<size_t> Claim()
	{
		std::unique_lock lock(m_mutex);
		for (;;)
		{
			if (EndedLocked() || m_next >= m_pieces.size())
				return std::nullopt;
			if (m_next <= m_head + m_window)
				break;
			m_changed.wait_for(lock, std::chrono::milliseconds(100));
		}
		const size_t i = m_next++;
		m_pieces[i].data.resize(static_cast<size_t>(Length(i)));
		m_held++;
		m_most_held = std::max(m_most_held, m_held);
		return i;
	}

	uint64_t Have(size_t i) const
	{
		const std::lock_guard lock(m_mutex);
		return m_pieces[i].have;
	}

	/// The next bytes of piece i. False when the download ended, or more came than the piece holds
	/// (nothing is taken then).
	bool Append(size_t i, const void* data, size_t n)
	{
		const std::lock_guard lock(m_mutex);
		Piece& p = m_pieces[i];
		if (EndedLocked() || p.done || p.have + n > p.data.size())
			return false;
		std::memcpy(p.data.data() + p.have, data, n);
		p.have += n;
		if (i == m_head)
			m_changed.notify_all();
		return true;
	}

	/// Piece i is whole (what Append brought is all of it).
	void Complete(size_t i)
	{
		const std::lock_guard lock(m_mutex);
		m_pieces[i].done = true;
		m_changed.notify_all();
	}

	/// The download failed: the reading gets -1 (unless it is gone already: then what failed came
	/// after the end of what it wanted, and is no failure).
	void Fail(const std::string& why)
	{
		const std::lock_guard lock(m_mutex);
		if (!m_gone && !m_failed)
		{
			m_failed = true;
			m_error = why;
		}
		m_changed.notify_all();
	}

	/// The reading stopped reading (at the archive's end, or it failed itself).
	void Gone()
	{
		const std::lock_guard lock(m_mutex);
		m_gone = true;
		m_changed.notify_all();
	}

	bool Ended() const
	{
		const std::lock_guard lock(m_mutex);
		return EndedLocked();
	}
	bool Failed() const
	{
		const std::lock_guard lock(m_mutex);
		return m_failed;
	}
	std::string Error() const
	{
		const std::lock_guard lock(m_mutex);
		return m_error;
	}
	/// The piece being read (Count() when all were).
	size_t Head() const
	{
		const std::lock_guard lock(m_mutex);
		return m_head;
	}
	/// The pieces held in memory now, and the most there were at once.
	size_t Held() const
	{
		const std::lock_guard lock(m_mutex);
		return m_held;
	}
	size_t MostHeld() const
	{
		const std::lock_guard lock(m_mutex);
		return m_most_held;
	}

	/// The unpacking's read: the next bytes in order, up to `size`; 0 at the end; -1 stopped or failed.
	long Read(void* buffer, size_t size)
	{
		std::unique_lock lock(m_mutex);
		for (;;)
		{
			if (m_failed || m_stop_seen || (m_stop && m_stop()))
			{
				m_stop_seen = true;
				m_changed.notify_all();
				return -1;
			}
			if (m_head >= m_pieces.size())
				return 0;
			Piece& p = m_pieces[m_head];
			if (m_read_at < p.have)
			{
				const size_t n = static_cast<size_t>(std::min<uint64_t>(size, p.have - m_read_at));
				std::memcpy(buffer, p.data.data() + m_read_at, n);
				m_read_at += n;
				if (p.done && m_read_at == p.data.size())
					Advance();
				return static_cast<long>(n);
			}
			if (p.done && m_read_at == p.data.size())
			{
				Advance();
				continue;
			}
			m_changed.wait_for(lock, std::chrono::milliseconds(100));
		}
	}

private:
	struct Piece
	{
		std::vector<char> data;
		uint64_t have = 0;
		bool done = false;
	};

	bool EndedLocked() const
	{
		if (m_gone || m_failed || m_stop_seen)
			return true;
		if (m_stop && m_stop())
		{
			m_stop_seen = true;
			return true;
		}
		return false;
	}

	void Advance()
	{
		std::vector<char>().swap(m_pieces[m_head].data);
		m_held--;
		m_head++;
		m_read_at = 0;
		m_changed.notify_all();
	}

	const uint64_t m_total, m_piece;
	const size_t m_window;
	const std::function<bool()> m_stop;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::vector<Piece> m_pieces;
	size_t m_next = 0, m_head = 0, m_held = 0, m_most_held = 0;
	uint64_t m_read_at = 0;
	bool m_gone = false, m_failed = false;
	mutable bool m_stop_seen = false;
	std::string m_error;
};

// ---- speeds

/// The speed of what the last few seconds brought, from readings of a count of bytes.
class SpeedMeter
{
public:
	explicit SpeedMeter(double span = 4.0) : m_span(span) {}

	void Add(double seconds, uint64_t bytes)
	{
		if (!m_samples.empty() && seconds - m_samples.back().seconds < 0.25)
			return;
		m_samples.push_back({seconds, bytes});
		// (One reading older than the span is kept: the span is measured from it.)
		while (m_samples.size() > 2 && seconds - m_samples[1].seconds >= m_span)
			m_samples.pop_front();
	}

	/// Bytes a second; 0 until there are two readings half a second apart.
	double BytesPerSecond() const
	{
		if (m_samples.size() < 2)
			return 0.0;
		const double dt = m_samples.back().seconds - m_samples.front().seconds;
		if (dt < 0.5 || m_samples.back().bytes < m_samples.front().bytes)
			return 0.0;
		return static_cast<double>(m_samples.back().bytes - m_samples.front().bytes) / dt;
	}

	void Clear() { m_samples.clear(); }

private:
	struct Sample
	{
		double seconds;
		uint64_t bytes;
	};
	double m_span;
	std::deque<Sample> m_samples;
};

/// Seconds left at a speed; below 0 when it cannot be said.
inline double SecondsLeft(uint64_t got, uint64_t total, double bytes_per_second)
{
	if (total == 0 || bytes_per_second <= 1.0 || got > total)
		return -1.0;
	return static_cast<double>(total - got) / bytes_per_second;
}

/// "2 h 05 min", "12 min", "45 s"; empty when not known.
inline std::string TimeLeftText(double seconds)
{
	if (seconds < 0)
		return {};
	const long s = static_cast<long>(seconds + 0.5);
	char text[32];
	if (s >= 3600)
		std::snprintf(text, sizeof(text), "%ld h %02ld min", s / 3600, (s % 3600) / 60);
	else if (s >= 60)
		std::snprintf(text, sizeof(text), "%ld min", (s + 30) / 60);
	else
		std::snprintf(text, sizeof(text), "%ld s", s);
	return text;
}

/// The speeds the last pieces were fetched at (each attempt that finished its piece), and their
/// median: what a new connection gives now. Shared by the connections.
class RecentSpeeds
{
public:
	void Add(double bytes_per_second)
	{
		const std::lock_guard lock(m_mutex);
		m_speeds.push_back(bytes_per_second);
		if (m_speeds.size() > 16)
			m_speeds.pop_front();
	}
	/// 0 until three are known.
	double Median() const
	{
		const std::lock_guard lock(m_mutex);
		if (m_speeds.size() < 3)
			return 0.0;
		std::vector<double> s(m_speeds.begin(), m_speeds.end());
		std::nth_element(s.begin(), s.begin() + static_cast<long>(s.size() / 2), s.end());
		return s[s.size() / 2];
	}

private:
	mutable std::mutex m_mutex;
	std::deque<double> m_speeds;
};

/// One attempt at a piece, watched: its speed over the last seconds, and whether it has been far
/// slower than new connections are for long enough to leave it.
class AttemptSpeed
{
public:
	static constexpr double SETTLE = 4.0;   // seconds a new connection is given before it is judged
	static constexpr double BELOW = 3.0;    // seconds under a third of the median that leave it
	static constexpr double FRACTION = 1.0 / 3.0;

	explicit AttemptSpeed(double start) : m_start(start), m_meter(3.0) { m_meter.Add(start, 0); }

	void Sample(double seconds, uint64_t bytes) { m_meter.Add(seconds, bytes); }
	double Recent() const { return m_meter.BytesPerSecond(); }

	/// Called with each sample: true once the attempt has run SETTLE seconds and been under a third
	/// of `median` for BELOW seconds on end. A median of 0 (not known yet) judges nothing.
	bool Slow(double seconds, double median)
	{
		if (median <= 0.0 || seconds - m_start < SETTLE)
		{
			m_below_since = -1.0;
			return false;
		}
		if (Recent() >= median * FRACTION)
		{
			m_below_since = -1.0;
			return false;
		}
		if (m_below_since < 0)
			m_below_since = seconds;
		return seconds - m_below_since >= BELOW;
	}

private:
	double m_start;
	SpeedMeter m_meter;
	double m_below_since = -1.0;
};

// ---- a connection's work

enum class Attempt
{
	Brought, // the transfer ended (whole or not: Work looks at what the piece has)
	Failed,  // it failed (why says how)
	Slow,    // it was left for being slow
};

struct AttemptResult
{
	Attempt what = Attempt::Brought;
	std::string why;
};

struct Rules
{
	unsigned retries = 4;          // attempts after ones that brought nothing, before the download fails
	unsigned most_slow = 8;        // times a piece is left for being slow; then it is let run
	unsigned pause_ms = 1000;      // before a retry: 1, 2, 4, 8 s
};

struct Counters
{
	std::mutex mutex;
	unsigned retries = 0, slow = 0, pieces = 0;
};

/// A connection's work: pieces taken one after another and fetched until whole. `fetch(index,
/// from, to, failures, watch_speed)` asks for bytes from..to (the file's, both included) of piece
/// `index`, Appends what comes and returns how it ended; `failures` is how many attempts at the
/// piece brought nothing (the caller may go back to the first address after a few). Ends when no
/// piece is left or the download ended; a piece that cannot be had fails the download.
template <class Fetch>
void Work(Assembly& a, Fetch&& fetch, const Rules& rules, Counters* counters)
{
	while (const std::optional<size_t> taken = a.Claim())
	{
		const size_t i = *taken;
		unsigned failures = 0, slow = 0;
		for (;;)
		{
			if (a.Ended())
				return;
			const uint64_t had = a.Have(i), length = a.Length(i);
			if (had >= length)
				break;
			const AttemptResult r = fetch(i, a.Offset(i) + had, a.Offset(i) + length - 1, failures, slow < rules.most_slow);
			if (a.Ended())
				return;
			const uint64_t has = a.Have(i);
			if (has >= length)
				break;
			if (r.what == Attempt::Slow)
			{
				slow++;
				if (counters)
				{
					const std::lock_guard lock(counters->mutex);
					counters->slow++;
				}
				continue;
			}
			if (counters)
			{
				const std::lock_guard lock(counters->mutex);
				counters->retries++;
			}
			if (has > had)
				continue; // it brought something: again at once, from there
			if (++failures > rules.retries)
			{
				a.Fail(r.why.empty() ? std::string("the server ended a part early") : r.why);
				return;
			}
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<uint64_t>(rules.pause_ms) << (failures - 1));
			while (std::chrono::steady_clock::now() < until && !a.Ended())
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		a.Complete(i);
		if (counters)
		{
			const std::lock_guard lock(counters->mutex);
			counters->pieces++;
		}
	}
}
} // namespace PS5TexturePacksPieces
