// Crux2 - HD texture packs: an archive downloaded over curl and handed to the unpacking as it comes.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of PCSX2 or the console here (curl and ps5_texture_packs_pieces.h): ps5_texture_packs.cpp
// is its one user, and ps5/tests/texturepacks/fetch_test.cpp runs it on a PC against a made-up
// server. (A header, so that the title's build and the test take it alike.)
//
// With one connection, the archive comes as one stream, as it always did: curl hands what it
// receives to a queue (32 MiB at most: when it is full, curl waits), and the unpacking takes it
// from there. With more, a first request asks for the first byte alone: the Internet Archive sends
// it to one of its data servers, and when that one answers with the byte and the size
// (206, Content-Range), the archive is fetched in pieces from that address over that many
// connections at once (ps5_texture_packs_pieces.h); a server that does not, one stream again.
// Each piece is asked for on a new connection, closed after it (the Archive slows a connection down
// the longer it runs), and every few seconds the log has a line of the speeds.

#pragma once

#include "ps5_texture_packs_pieces.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PS5TexturePacksFetch
{
/// The next bytes of the archive, as PS5TexturePacksExtract::Read: 0 at its end, -1 failed or stopped.
using Read = std::function<long(void* buffer, size_t size)>;

struct Options
{
	std::string url;                       // the collection's address of the archive
	uint64_t size = 0;                     // its size in the collection's list; 0 not known
	unsigned connections = PS5TexturePacksPieces::CONNECTIONS_DEFAULT; // 1: one stream
	uint64_t piece = PS5TexturePacksPieces::PIECE;
	const char* ca_bundle = nullptr;       // the certificates curl checks against (the console's)
	std::function<bool()> stop;
	std::atomic<uint64_t>* received = nullptr; // bytes received, counted up as they come
	std::atomic<int>* active = nullptr;        // connections in a transfer now
	std::function<void(const std::string&)> log;
	long low_speed_time = 60;              // seconds under 1 KiB a second that end a transfer
	PS5TexturePacksPieces::Rules rules;
	double log_every = 5.0;                // seconds between the lines of speeds
};

struct Outcome
{
	bool ranged = false;   // fetched in pieces
	bool complete = false; // the download did not fail before the unpacking stopped reading
	std::string error;     // why it failed, when it did
	unsigned connections = 1, pieces = 0, retries = 0, slow = 0;
	size_t most_held = 0;  // pieces in memory at once, at most
};

namespace detail
{
constexpr size_t QUEUE_LIMIT = 32u << 20;

inline double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void Log(const Options& o, const std::string& line)
{
	if (o.log)
		o.log(line);
}

inline bool Stopped(const Options& o)
{
	return o.stop && o.stop();
}

inline void Prepare(CURL* curl, const std::string& url, const Options& o)
{
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Crux2 (PlayStation 5 homebrew)");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	if (o.ca_bundle)
		curl_easy_setopt(curl, CURLOPT_CAINFO, o.ca_bundle);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	// A transfer that brings less than 1 KiB a second for a minute has stopped (each connection).
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, o.low_speed_time);
}

inline std::string Why(CURLcode rc, long code)
{
	if (rc == CURLE_HTTP_RETURNED_ERROR && code > 0)
		return std::string(curl_easy_strerror(rc)) + " (HTTP " + std::to_string(code) + ")";
	return curl_easy_strerror(rc);
}

// ---- one stream (one connection, or a server that sends no ranges)

// What curl receives, on its way to libarchive.
struct Queue
{
	const Options* options;
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<std::vector<char>> chunks;
	size_t front_at = 0;
	size_t queued = 0;
	bool ended = false;  // curl is done
	bool failed = false; // and the transfer did not complete
	bool gone = false;   // the unpacking stopped reading
};

inline size_t ToQueue(char* data, size_t size, size_t count, void* user)
{
	Queue* const q = static_cast<Queue*>(user);
	const size_t n = size * count;
	std::unique_lock lock(q->mutex);
	while (!(q->queued < QUEUE_LIMIT || q->gone || Stopped(*q->options)))
		q->changed.wait_for(lock, std::chrono::milliseconds(100));
	if (q->gone || Stopped(*q->options))
		return 0; // (curl ends the transfer)
	q->chunks.emplace_back(data, data + n);
	q->queued += n;
	if (q->options->received)
		q->options->received->fetch_add(n, std::memory_order_relaxed);
	q->changed.notify_all();
	return n;
}

inline long FromQueue(Queue* q, void* buffer, size_t size)
{
	std::unique_lock lock(q->mutex);
	while (!(!q->chunks.empty() || q->ended || Stopped(*q->options)))
		q->changed.wait_for(lock, std::chrono::milliseconds(100));
	if (Stopped(*q->options))
		return -1;
	if (q->chunks.empty())
		return q->failed ? -1 : 0;
	std::vector<char>& front = q->chunks.front();
	const size_t n = std::min(size, front.size() - q->front_at);
	std::memcpy(buffer, front.data() + q->front_at, n);
	q->front_at += n;
	q->queued -= n;
	if (q->front_at == front.size())
	{
		q->chunks.pop_front();
		q->front_at = 0;
	}
	q->changed.notify_all();
	return static_cast<long>(n);
}

inline Outcome OneStream(const Options& o, const std::function<void(const Read&)>& consume)
{
	auto queue = std::make_shared<Queue>();
	queue->options = &o;
	CURLcode curl_result = CURLE_OK;
	long code = 0;
	std::thread download([queue, &o, &curl_result, &code] {
		CURL* const curl = curl_easy_init();
		if (!curl)
		{
			curl_result = CURLE_FAILED_INIT;
		}
		else
		{
			Prepare(curl, o.url, o);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ToQueue);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, queue.get());
			curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);
			if (o.active)
				o.active->fetch_add(1);
			curl_result = curl_easy_perform(curl);
			if (o.active)
				o.active->fetch_sub(1);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
			curl_easy_cleanup(curl);
		}
		const std::lock_guard lock(queue->mutex);
		queue->ended = true;
		queue->failed = (curl_result != CURLE_OK);
		queue->changed.notify_all();
	});
	consume([queue](void* buffer, size_t size) { return FromQueue(queue.get(), buffer, size); });
	{
		const std::lock_guard lock(queue->mutex);
		queue->gone = true;
		queue->changed.notify_all();
	}
	download.join();
	Outcome out;
	// (The archive read to its end is whole, even where curl was cut off after it: a few bytes
	// past the end of an archive are not read.)
	out.complete = (curl_result == CURLE_OK || curl_result == CURLE_WRITE_ERROR);
	if (!out.complete)
		out.error = Why(curl_result, code);
	return out;
}

// ---- the first request: the first byte, and where it came from

inline size_t HeaderLine(char* data, size_t size, size_t count, void* user)
{
	std::string* const range = static_cast<std::string*>(user);
	const size_t n = size * count;
	const std::string_view line(data, n);
	if (line.size() >= 5 && line.substr(0, 5) == "HTTP/")
		range->clear(); // (a redirect's headers are not the answer's)
	static constexpr std::string_view NAME = "content-range:";
	if (line.size() > NAME.size())
	{
		bool same = true;
		for (size_t i = 0; i < NAME.size() && same; i++)
			same = (std::tolower(static_cast<unsigned char>(line[i])) == NAME[i]);
		if (same)
		{
			std::string_view value = line.substr(NAME.size());
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
				value.remove_prefix(1);
			while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
				value.remove_suffix(1);
			*range = std::string(value);
		}
	}
	return n;
}

struct Probe
{
	uint64_t size = 0; // 0: one stream
	std::string url;   // where the bytes came from (after the redirects)
	std::string why;   // why not in pieces
};

inline Probe AskFirstByte(const Options& o)
{
	Probe p;
	CURL* const curl = curl_easy_init();
	if (!curl)
	{
		p.why = "curl did not start";
		return p;
	}
	std::string range;
	uint64_t body = 0;
	Prepare(curl, o.url, o);
	curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HeaderLine);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &range);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t count, void* user) -> size_t {
		uint64_t* const got = static_cast<uint64_t*>(user);
		*got += size * count;
		return *got > 1 ? 0 : size * count; // (a whole file sent instead of a byte is not taken)
	});
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	const CURLcode rc = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	char* effective = nullptr;
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
	if (effective)
		p.url = effective;
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR)
		p.why = Why(rc, code);
	else if ((p.size = PS5TexturePacksPieces::RangedSize(code, range, o.size)) == 0)
		p.why = "the server sends no parts (HTTP " + std::to_string(code) + (range.empty() ? std::string() : ", Content-Range " + range) + ")";
	if (p.url.empty())
		p.url = o.url;
	return p;
}

// ---- in pieces

struct Slot
{
	std::atomic<long> piece{-1};
	std::atomic<double> speed{0.0}; // bytes a second over the last seconds
};

struct Shared
{
	const Options* options;
	PS5TexturePacksPieces::Assembly* assembly;
	PS5TexturePacksPieces::RecentSpeeds recent;
	PS5TexturePacksPieces::Counters counters;
	std::vector<std::unique_ptr<Slot>> slots;
	std::mutex log_mutex;
	double next_log = 0.0, last_log = 0.0;
	uint64_t last_received = 0;

	// The line of speeds, every few seconds: the whole, each connection's, the median of the
	// pieces that finished, and how far the reading is.
	void MaybeLog(double now)
	{
		std::unique_lock lock(log_mutex, std::try_to_lock);
		if (!lock.owns_lock() || now < next_log)
			return;
		const uint64_t received = options->received ? options->received->load(std::memory_order_relaxed) : 0;
		const double total = (now > last_log) ? static_cast<double>(received - last_received) / (now - last_log) : 0.0;
		char part[64];
		std::string each;
		int busy = 0;
		for (const std::unique_ptr<Slot>& s : slots)
		{
			const long piece = s->piece.load();
			if (piece < 0)
			{
				each += " -";
				continue;
			}
			busy++;
			std::snprintf(part, sizeof(part), " %.1f", s->speed.load() / 1e6);
			each += part;
		}
		unsigned retries, slow, pieces;
		{
			const std::lock_guard c(counters.mutex);
			retries = counters.retries;
			slow = counters.slow;
			pieces = counters.pieces;
		}
		char line[256];
		std::snprintf(line, sizeof(line), "texture packs: %.1f MB/s on %d, median piece %.1f MB/s; reading piece %zu of %zu, %zu held, %u done, %u slow left, %u retried; MB/s:",
			total / 1e6, busy, recent.Median() / 1e6, assembly->Head() + 1, assembly->Count(), assembly->Held(), pieces, slow, retries);
		Log(*options, line + each);
		last_log = now;
		last_received = received;
		next_log = now + options->log_every;
	}
};

struct Transfer
{
	Shared* shared;
	Slot* slot;
	size_t index;
	uint64_t from;
	std::string range;     // Content-Range of the answer
	bool checked = false;  // the answer is the part asked for
	bool refused = false;  // it is not
	long code = 0;
	uint64_t got = 0;
	bool watch = true;
	bool slow = false;
	PS5TexturePacksPieces::AttemptSpeed speed;
	CURL* curl;
};

inline size_t ToPiece(char* data, size_t size, size_t count, void* user)
{
	Transfer* const t = static_cast<Transfer*>(user);
	const size_t n = size * count;
	if (!t->checked)
	{
		curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &t->code);
		const PS5TexturePacksPieces::ContentRange r = PS5TexturePacksPieces::ParseContentRange(t->range);
		if (t->code != 206 || !r.ok || r.from != t->from)
		{
			t->refused = true;
			return 0;
		}
		t->checked = true;
	}
	if (!t->shared->assembly->Append(t->index, data, n))
		return 0;
	t->got += n;
	if (t->shared->options->received)
		t->shared->options->received->fetch_add(n, std::memory_order_relaxed);
	return n;
}

inline int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	Transfer* const t = static_cast<Transfer*>(user);
	if (t->shared->assembly->Ended())
		return 1;
	const double now = Now();
	t->speed.Sample(now, t->got);
	t->slot->speed.store(t->speed.Recent());
	if (t->watch && t->speed.Slow(now, t->shared->recent.Median()))
	{
		t->slow = true;
		return 1;
	}
	t->shared->MaybeLog(now);
	return 0;
}

inline void Connection(Shared* shared, Slot* slot, const std::string& final_url)
{
	const Options& o = *shared->options;
	CURL* const curl = curl_easy_init();
	if (!curl)
	{
		shared->assembly->Fail("curl did not start");
		return;
	}
	const auto fetch = [&](size_t index, uint64_t from, uint64_t to, unsigned failures, bool watch) {
		PS5TexturePacksPieces::AttemptResult r;
		const double start = Now();
		Transfer t{shared, slot, index, from, {}, false, false, 0, 0, watch, false, PS5TexturePacksPieces::AttemptSpeed(start), curl};
		// (After two attempts that brought nothing, the collection's address again: it may send
		// the piece to another data server.)
		Prepare(curl, failures >= 2 ? o.url : final_url, o);
		const std::string range = std::to_string(from) + "-" + std::to_string(to);
		curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
		// A new connection for each piece, closed after it.
		curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
		curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
		curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HeaderLine);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t.range);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ToPiece);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
		slot->piece.store(static_cast<long>(index));
		if (o.active)
			o.active->fetch_add(1);
		const CURLcode rc = curl_easy_perform(curl);
		if (o.active)
			o.active->fetch_sub(1);
		slot->piece.store(-1);
		slot->speed.store(0.0);
		long code = t.code;
		if (code == 0)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		const double took = Now() - start;
		if (t.slow)
		{
			r.what = PS5TexturePacksPieces::Attempt::Slow;
			char line[160];
			std::snprintf(line, sizeof(line), "texture packs: piece %zu left for being slow (%.2f MB/s, median %.2f, %.1f s in); again on a new connection",
				index + 1, t.speed.Recent() / 1e6, shared->recent.Median() / 1e6, took);
			Log(o, line);
		}
		else if (t.refused)
		{
			r.what = PS5TexturePacksPieces::Attempt::Failed;
			r.why = "the server did not send the part asked for (HTTP " + std::to_string(code) + ")";
		}
		else if (rc != CURLE_OK)
		{
			r.what = PS5TexturePacksPieces::Attempt::Failed;
			r.why = Why(rc, code);
		}
		else
		{
			r.what = PS5TexturePacksPieces::Attempt::Brought;
			// (What a piece took from its request to its end, a new connection's speed: of a part
			// large enough to say it.)
			if (from + t.got == to + 1 && t.got >= std::min<uint64_t>(1u << 20, o.piece / 2) && took > 0.0)
				shared->recent.Add(static_cast<double>(t.got) / took);
		}
		if (r.what == PS5TexturePacksPieces::Attempt::Failed && !shared->assembly->Ended())
			Log(o, "texture packs: piece " + std::to_string(index + 1) + ": " + r.why + " after " + std::to_string(t.got) + " bytes; asked for again");
		return r;
	};
	PS5TexturePacksPieces::Work(*shared->assembly, fetch, o.rules, &shared->counters);
	curl_easy_cleanup(curl);
}

inline Outcome InPieces(const Options& o, const Probe& probe, const std::function<void(const Read&)>& consume)
{
	const unsigned wanted = std::clamp(o.connections, 1u, PS5TexturePacksPieces::CONNECTIONS_MOST);
	PS5TexturePacksPieces::Assembly assembly(probe.size, o.piece, PS5TexturePacksPieces::WindowFor(wanted), o.stop);
	const unsigned n = static_cast<unsigned>(std::min<size_t>(wanted, std::max<size_t>(assembly.Count(), 1)));
	Shared shared;
	shared.options = &o;
	shared.assembly = &assembly;
	shared.last_log = Now();
	shared.next_log = shared.last_log + o.log_every;
	shared.last_received = o.received ? o.received->load() : 0;
	for (unsigned k = 0; k < n; k++)
		shared.slots.push_back(std::make_unique<Slot>());
	Log(o, "texture packs: " + std::to_string(probe.size) + " bytes from " + probe.url + " in " + std::to_string(assembly.Count()) + " pieces over " +
			   std::to_string(n) + " connections");
	const double start = Now();
	const uint64_t received_before = o.received ? o.received->load() : 0;
	std::vector<std::thread> connections;
	for (unsigned k = 0; k < n; k++)
		connections.emplace_back([&shared, &probe, slot = shared.slots[k].get()] { Connection(&shared, slot, probe.url); });
	consume([&assembly](void* buffer, size_t size) { return assembly.Read(buffer, size); });
	assembly.Gone();
	for (std::thread& t : connections)
		t.join();
	Outcome out;
	out.ranged = true;
	out.connections = n;
	out.complete = !assembly.Failed();
	out.error = assembly.Error();
	out.most_held = assembly.MostHeld();
	{
		const std::lock_guard lock(shared.counters.mutex);
		out.pieces = shared.counters.pieces;
		out.retries = shared.counters.retries;
		out.slow = shared.counters.slow;
	}
	const double took = Now() - start;
	const uint64_t got = (o.received ? o.received->load() : 0) - received_before;
	char line[200];
	std::snprintf(line, sizeof(line), "texture packs: %u pieces whole, %.1f MB in %.0f s (%.1f MB/s), %u left for being slow, %u retried, %zu held at most",
		out.pieces, static_cast<double>(got) / 1e6, took, took > 0 ? static_cast<double>(got) / 1e6 / took : 0.0, out.slow, out.retries, out.most_held);
	Log(o, line);
	return out;
}
} // namespace detail

/// Downloads the archive and runs `consume` (the unpacking) on this thread with the Read that
/// gives it in order; returns when `consume` has returned and the connections are closed.
inline Outcome Run(const Options& o, const std::function<void(const Read&)>& consume)
{
	if (o.connections <= 1)
		return detail::OneStream(o, consume);
	const detail::Probe probe = detail::AskFirstByte(o);
	if (probe.size == 0)
	{
		detail::Log(o, "texture packs: one stream: " + probe.why);
		return detail::OneStream(o, consume);
	}
	return detail::InPieces(o, probe, consume);
}
} // namespace PS5TexturePacksFetch
