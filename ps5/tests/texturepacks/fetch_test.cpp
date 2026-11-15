// Crux2 - a texture pack downloaded over curl (frontend/ps5_texture_packs_fetch.h) from a made-up
// Internet Archive on 127.0.0.1 (range_server.py): in pieces over 10 connections, as one stream,
// from a server without ranges, with parts cut off, failing, stopped; and a .zip unpacked as it
// comes (ps5_texture_packs_extract.cpp, the PC's libarchive).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   fetch_test <server address, http://127.0.0.1:PORT> <folder of the files> <work folder>

#include "ps5_texture_packs_extract.h"
#include "ps5_texture_packs_fetch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

std::string g_server;
std::vector<char> g_data;
bool g_verbose = false;

std::vector<char> ReadFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

size_t ToString(char* data, size_t size, size_t count, void* user)
{
	static_cast<std::string*>(user)->append(data, size * count);
	return size * count;
}

std::map<std::string, long> Stats(bool reset)
{
	std::string body;
	CURL* const curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, (g_server + "/stats").c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_perform(curl);
	if (reset)
	{
		curl_easy_setopt(curl, CURLOPT_URL, (g_server + "/reset").c_str());
		curl_easy_perform(curl);
	}
	curl_easy_cleanup(curl);
	std::map<std::string, long> out;
	std::istringstream lines(body);
	std::string key;
	long value;
	while (lines >> key >> value)
		out[key] = value;
	return out;
}

struct Got
{
	PS5TexturePacksFetch::Outcome outcome;
	std::vector<char> bytes;
	bool read_failed = false;
	uint64_t received = 0;
	double seconds = 0;
};

PS5TexturePacksFetch::Options Options(const std::string& path, unsigned connections)
{
	PS5TexturePacksFetch::Options o;
	o.url = g_server + path;
	o.size = g_data.size();
	o.connections = connections;
	o.piece = 64 << 10;
	o.rules.pause_ms = 20;
	o.log_every = 0.5;
	o.log = [](const std::string& line) {
		if (g_verbose)
			std::fprintf(stderr, "%s\n", line.c_str());
	};
	return o;
}

// Everything the download hands over (`most` bytes at most: then the reading stops, as at the end
// of an archive), in reads of odd sizes.
Got Fetch(PS5TexturePacksFetch::Options o, size_t most = SIZE_MAX, std::atomic_bool* stop = nullptr, uint64_t stop_at = 0)
{
	Got g;
	std::atomic<uint64_t> received{0};
	std::atomic<int> active{0};
	o.received = &received;
	o.active = &active;
	if (stop)
		o.stop = [stop] { return stop->load(); };
	const auto start = std::chrono::steady_clock::now();
	g.outcome = PS5TexturePacksFetch::Run(o, [&](const PS5TexturePacksFetch::Read& read) {
		std::vector<char> buffer(50001);
		size_t want = 1;
		for (;;)
		{
			if (g.bytes.size() >= most)
				return;
			want = (want * 7 + 13) % buffer.size() + 1;
			const long n = read(buffer.data(), want);
			if (n < 0)
			{
				g.read_failed = true;
				return;
			}
			if (n == 0)
				return;
			g.bytes.insert(g.bytes.end(), buffer.data(), buffer.data() + n);
			if (stop && g.bytes.size() >= stop_at)
				stop->store(true);
		}
	});
	g.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	g.received = received.load();
	Expect(active.load() == 0, "no connection left counted as in a transfer");
	return g;
}

bool Prefix(const std::vector<char>& got)
{
	return got.size() <= g_data.size() && std::equal(got.begin(), got.end(), g_data.begin());
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 4)
		return 2;
	g_server = argv[1];
	const std::string files = argv[2], work = argv[3];
	g_verbose = std::getenv("FETCH_TEST_VERBOSE") != nullptr;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	g_data = ReadFile(files + "/data.bin");
	const size_t pieces = (g_data.size() + (64 << 10) - 1) / (64 << 10);
	Stats(true);
	const auto timed = [](const char* what, const Got& g) {
		if (g_verbose)
			std::fprintf(stderr, "%s: %.2f s\n", what, g.seconds);
	};

	// In pieces, over 10 connections, from where the redirect went.
	{
		const Got g = Fetch(Options("/download/data.bin", 10));
		timed("10 connections", g);
		const auto s = Stats(true);
		Expect(g.outcome.ranged && g.outcome.complete && !g.read_failed && g.bytes == g_data, "10 connections: every byte, in order");
		Expect(g.outcome.connections == 10 && g.outcome.pieces == pieces && g.outcome.retries == 0, "10 connections: each piece once");
		Expect(g.outcome.most_held <= PS5TexturePacksPieces::WindowFor(10) + 1, "10 connections: the window kept");
		Expect(g.received == g_data.size(), "the bytes received counted");
		Expect(s.count("/download") && s.at("/download") == 1, "the collection's address asked once (" + std::to_string(s.count("/download") ? s.at("/download") : 0) + ")");
		Expect(s.count("/data") && s.at("/data") == static_cast<long>(pieces) + 1, "every part asked of the data server");
		Expect(s.at("connections") >= static_cast<long>(pieces) + 1, "a new connection for each piece (" + std::to_string(s.at("connections")) + ")");
	}
	// One connection: one stream, as before; no ranges asked.
	{
		const Got g = Fetch(Options("/download/data.bin", 1));
		timed("1 connection", g);
		const auto s = Stats(true);
		Expect(!g.outcome.ranged && g.outcome.complete && g.bytes == g_data, "1 connection: one stream");
		Expect(s.at("ranged") == 0 && s.at("connections") <= 2, "1 connection: no range asked");
	}
	// A server without ranges: one stream.
	{
		const Got g = Fetch(Options("/norange/data.bin", 10));
		timed("no ranges", g);
		Stats(true);
		Expect(!g.outcome.ranged && g.outcome.complete && g.bytes == g_data, "a server without ranges: one stream, whole");
	}
	// A size that is not the list's: one stream.
	{
		PS5TexturePacksFetch::Options o = Options("/download/data.bin", 10);
		o.size = g_data.size() + 1;
		const Got g = Fetch(o);
		timed("size not the list", g);
		Stats(true);
		Expect(!g.outcome.ranged && g.outcome.complete && g.bytes == g_data, "a size not the list's: one stream");
	}
	// Parts cut off halfway: each asked for again from what it has.
	{
		const Got g = Fetch(Options("/flaky/data.bin", 10));
		timed("cut off", g);
		const auto s = Stats(true);
		Expect(g.outcome.ranged && g.outcome.complete && g.bytes == g_data, "parts cut off: every byte, in order");
		Expect(g.outcome.retries == pieces - 1, "parts cut off: each asked for again (" + std::to_string(g.outcome.retries) + ")");
		Expect(g.received == g_data.size(), "parts cut off: no byte received twice (" + std::to_string(g.received) + ")");
		Expect(s.at("/flaky") == static_cast<long>(2 * pieces), "parts cut off: one request more for each");
	}
	// A part that slows down to nothing: left after a few seconds, and asked for again from its bytes
	// on a new connection.
	{
		const Got g = Fetch(Options("/slowdown/data.bin", 10));
		timed("slowed down", g);
		Stats(true);
		Expect(g.outcome.ranged && g.outcome.complete && g.bytes == g_data, "a part slowed down: every byte, in order");
		Expect(g.outcome.slow == 1 && g.outcome.retries == 0, "a part slowed down: left once (" + std::to_string(g.outcome.slow) + ")");
		Expect(g.received == g_data.size(), "a part slowed down: no byte received twice");
		Expect(g.seconds < PS5TexturePacksPieces::AttemptSpeed::SETTLE + PS5TexturePacksPieces::AttemptSpeed::BELOW + 4, "a part slowed down: left in time");
	}
	// Parts the server refuses: the download fails, with the reason.
	{
		const Got g = Fetch(Options("/broken/data.bin", 4));
		timed("refused", g);
		Stats(true);
		Expect(g.outcome.ranged && !g.outcome.complete && g.read_failed, "parts refused: the download fails");
		Expect(g.outcome.error.find("503") != std::string::npos, "parts refused: the reason (" + g.outcome.error + ")");
		Expect(g.bytes.size() <= (64 << 10) && Prefix(g.bytes), "parts refused: the first part (the server sends it) and nothing after");
	}
	// Stopped a third of the way.
	{
		std::atomic_bool stop{false};
		const Got g = Fetch(Options("/download/data.bin", 10), SIZE_MAX, &stop, g_data.size() / 3);
		timed("stopped", g);
		Stats(true);
		Expect(g.read_failed && g.bytes.size() < g_data.size() && Prefix(g.bytes), "stopped: the reading gets -1 after what came in order");
		Expect(g.seconds < 10, "stopped: at once");
	}
	// The reading stops before the end (an archive's end): no failure.
	{
		const Got g = Fetch(Options("/download/data.bin", 10), 100000);
		timed("done early", g);
		Stats(true);
		Expect(g.outcome.complete && g.outcome.error.empty() && Prefix(g.bytes), "the reading done early: whole");
	}
	// A .zip unpacked as it comes, in pieces.
	{
		const std::vector<char> zip = ReadFile(files + "/pack.zip");
		PS5TexturePacksFetch::Options o = Options("/download/pack.zip", 10);
		o.size = zip.size();
		o.piece = 32 << 10;
		const std::string root = work + "/textures";
		std::filesystem::remove_all(root);
		PS5TexturePacksExtract::MakeFolders(root);
		PS5TexturePacksExtract::Result result;
		const PS5TexturePacksFetch::Outcome out = PS5TexturePacksFetch::Run(o, [&](const PS5TexturePacksFetch::Read& read) {
			result = PS5TexturePacksExtract::Extract(read, root, {"SLUS-00000"}, nullptr);
		});
		Stats(true);
		Expect(out.ranged && out.complete && result.ok, "a .zip in pieces: unpacked (" + result.error + ")");
		size_t same = 0, files_there = 0;
		for (const auto& e : std::filesystem::recursive_directory_iterator(files + "/pack"))
		{
			if (!e.is_regular_file())
				continue;
			files_there++;
			const std::string rel = std::filesystem::relative(e.path(), files + "/pack").string();
			if (ReadFile(e.path().string()) == ReadFile(root + "/" + rel))
				same++;
		}
		Expect(files_there > 10 && same == files_there && result.files == static_cast<int>(files_there),
			"a .zip in pieces: every file whole (" + std::to_string(same) + " of " + std::to_string(files_there) + ")");
	}
	curl_global_cleanup();
	if (g_failed)
	{
		std::printf("fetch: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("fetch: %d checks passed\n", g_checks);
	return 0;
}
