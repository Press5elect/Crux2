// Crux2 - game fixes from GitHub at start (see ps5_fixes_fetch.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Written with POSIX calls only (no std::filesystem: the console's libc++ has none). Built for the
// PC by ps5/tests/fixes/run-fixes-tests.sh with CRUX2_FIXES_TEST, without the parts that start it.

#include "ps5_fixes_fetch.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#if defined(CRUX2_FIXES_TEST)
static void fixes_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void fixes_log(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	std::vfprintf(stderr, format, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}
static void MakePublic(const std::string&, bool) {}
#else
#include "ps5_platform.h"
#include "ps5_priority.h"
#include "ps5_ui.h"

#include "Config.h"
#include "GameDatabase.h"
#include "Host.h"

#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <mutex>
#include <thread>
#define fixes_log ps5px_log
static void MakePublic(const std::string& path, bool folder)
{
	PS5Platform::MakePublic(path.c_str(), folder);
}
#endif

namespace PS5Fixes
{
namespace
{
constexpr size_t MOST_FILES = 4000;
constexpr uint64_t MOST_FILE_BYTES = 16u << 20; // one file (GameIndex.yaml itself is under 3 MB)
constexpr size_t MOST_LIST_BYTES = 2u << 20;

struct Entry
{
	std::string sha, path;
	uint64_t bytes = 0;
};

// ---- SHA-256 (FIPS 180-4)

struct Sha256
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t block[64];
	size_t used = 0;
	uint64_t total = 0;

	static uint32_t Rotate(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void Compress(const uint8_t* p)
	{
		static constexpr uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
			0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
			0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
			0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
			0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
			0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
			0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
			0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = hh + (Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
	}

	void Add(const uint8_t* p, size_t n)
	{
		total += n;
		while (n > 0)
		{
			const size_t take = std::min(n, sizeof(block) - used);
			std::memcpy(block + used, p, take);
			used += take;
			p += take;
			n -= take;
			if (used == sizeof(block))
			{
				Compress(block);
				used = 0;
			}
		}
	}

	std::string Hex()
	{
		const uint64_t bits = total * 8;
		const uint8_t one = 0x80, zero = 0;
		Add(&one, 1);
		while (used != 56)
			Add(&zero, 1);
		uint8_t length[8];
		for (int i = 0; i < 8; i++)
			length[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
		Add(length, 8);
		static constexpr char digits[] = "0123456789abcdef";
		std::string out;
		for (uint32_t v : h)
			for (int i = 28; i >= 0; i -= 4)
				out.push_back(digits[(v >> i) & 15]);
		return out;
	}
};

// ---- files

bool IsHex64(const std::string& s)
{
	return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool EndsWith(const std::string& s, const char* end)
{
	const size_t n = std::strlen(end);
	return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

/// The whole file, if it is there and no larger than `most`.
bool ReadAll(const std::string& path, std::string* out, uint64_t most)
{
	out->clear();
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	char buffer[65536];
	for (;;)
	{
		const ssize_t n = read(fd, buffer, sizeof(buffer));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 || out->size() + static_cast<size_t>(n) > most)
		{
			close(fd);
			return false;
		}
		if (n == 0)
			break;
		out->append(buffer, static_cast<size_t>(n));
	}
	close(fd);
	return true;
}

bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

void MakeFolder(const std::string& path)
{
	if (mkdir(path.c_str(), 0777) == 0)
		MakePublic(path, true);
}

/// Written beside its place, then put there. Never renamed onto a file that is there (on an exFAT
/// drive such a rename was seen to report success and keep the old file): the old one is moved
/// aside first, and put back if the new one does not go in.
bool WriteOver(const std::string& path, const void* data, size_t size)
{
	const std::string part = path + ".part", old = path + ".old";
	unlink(part.c_str());
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	const char* p = static_cast<const char*>(data);
	size_t left = size;
	while (left > 0)
	{
		const ssize_t n = write(fd, p, left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			close(fd);
			unlink(part.c_str());
			return false;
		}
		p += n;
		left -= static_cast<size_t>(n);
	}
	fsync(fd);
	close(fd);
	const bool had = Exists(path);
	unlink(old.c_str());
	if (had && rename(path.c_str(), old.c_str()) != 0)
	{
		unlink(part.c_str());
		return false;
	}
	struct stat st;
	if (rename(part.c_str(), path.c_str()) != 0 || stat(path.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) != size)
	{
		unlink(part.c_str());
		if (had)
		{
			unlink(path.c_str());
			rename(old.c_str(), path.c_str());
		}
		return false;
	}
	unlink(old.c_str());
	MakePublic(path, false);
	return true;
}

std::vector<std::string> Lines(const std::string& text)
{
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		out.push_back(std::move(line));
		at = end + 1;
	}
	return out;
}

/// The games a fix file is for: a game database file's serials (its keys at the left edge), a
/// patch file's serial (its name before the CRC).
void GamesOf(const std::string& path, const std::string& text, std::vector<std::string>* games)
{
	const auto add = [games](std::string serial) {
		for (char& c : serial)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if (!serial.empty() && std::find(games->begin(), games->end(), serial) == games->end())
			games->push_back(std::move(serial));
	};
	const std::string name = path.substr(path.find('/') + 1);
	if (path.compare(0, 8, "patches/") == 0)
	{
		const size_t bar = name.find('_');
		add(bar == std::string::npos ? name.substr(0, name.size() - 6) : name.substr(0, bar));
		return;
	}
	for (const std::string& line : Lines(text))
	{
		if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#' || line[0] == '-')
			continue;
		const size_t colon = line.find(':');
		if (colon != std::string::npos && colon > 0)
			add(line.substr(0, colon));
	}
}
} // namespace

std::string Sha256Hex(const void* data, size_t size)
{
	Sha256 sha;
	sha.Add(static_cast<const uint8_t*>(data), size);
	return sha.Hex();
}

bool SafePath(const std::string& path)
{
	if (path.empty() || path.size() > 160)
		return false;
	const size_t slash = path.find('/');
	if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos)
		return false;
	const std::string folder = path.substr(0, slash), name = path.substr(slash + 1);
	if (name.empty() || name[0] == '.')
		return false;
	for (char c : name)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	if (folder == "gamedb")
		return name.size() > 5 && EndsWith(name, ".yaml");
	if (folder == "patches")
		return name.size() > 6 && EndsWith(name, ".pnach");
	return false;
}

bool Sync(const std::string& list_url, const std::string& files_url, const std::string& shipped, const std::string& fixes,
	const Getter& get, Report* report)
{
	*report = Report();
	std::vector<uint8_t> body;
	const int status = get(list_url, &body);
	if (status != 200)
	{
		report->error = (status < 0) ? "No connection to GitHub." : "GitHub did not give the list of game fixes.";
		fixes_log("game fixes: the list: status %d", status);
		return false;
	}
	if (body.size() > MOST_LIST_BYTES)
	{
		report->error = "The list of game fixes is too large.";
		fixes_log("game fixes: the list is %zu bytes: left alone", body.size());
		return false;
	}
	const std::vector<std::string> lines = Lines(std::string(body.begin(), body.end()));
	if (lines.empty() || lines[0] != LIST_HEADER)
	{
		report->error = "The list of game fixes is of a kind this version of Crux2 does not know.";
		fixes_log("game fixes: the list's first line is not \"%s\": left alone", LIST_HEADER);
		return false;
	}
	std::vector<Entry> entries;
	std::set<std::string> listed;
	for (size_t i = 1; i < lines.size(); i++)
	{
		const std::string& line = lines[i];
		if (line.empty() || line[0] == '#')
			continue;
		const size_t a = line.find(' '), b = (a == std::string::npos) ? a : line.find(' ', a + 1);
		Entry e;
		if (b != std::string::npos)
		{
			e.sha = line.substr(0, a);
			e.bytes = std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
			e.path = line.substr(b + 1);
		}
		if (b == std::string::npos || !IsHex64(e.sha) || e.bytes > MOST_FILE_BYTES || !SafePath(e.path) || !listed.insert(e.path).second)
		{
			fixes_log("game fixes: line %zu of the list refused: %s", i + 1, line.c_str());
			continue;
		}
		entries.push_back(std::move(e));
		if (entries.size() > MOST_FILES)
		{
			report->error = "The list of game fixes is too long.";
			return false;
		}
	}
	report->listed = static_cast<unsigned>(entries.size());

	const std::string record = fixes + "/fetched.txt";
	std::string recorded;
	std::set<std::string> before;
	if (ReadAll(record, &recorded, MOST_LIST_BYTES))
	{
		for (const std::string& line : Lines(recorded))
		{
			if (SafePath(line))
				before.insert(line);
		}
	}
	std::set<std::string> now = before;

	bool folders = false;
	for (const Entry& e : entries)
	{
		// The copy that is read: the one fetched before, else the title's own.
		const std::string place = fixes + "/" + e.path;
		const auto matches = [&e](const std::string& text) { return text.size() == e.bytes && Sha256Hex(text.data(), text.size()) == e.sha; };
		std::string have, own;
		const bool fetched_before = ReadAll(place, &have, MOST_FILE_BYTES);
		const bool shipped_matches = ReadAll(shipped + "/" + e.path, &own, MOST_FILE_BYTES) && matches(own);
		if (fetched_before && shipped_matches && now.count(e.path) != 0)
		{
			// The title's own copy is the listed one now (Crux2 was updated): the fetched one goes.
			if (unlink(place.c_str()) == 0)
			{
				now.erase(e.path);
				fixes_log("game fixes: %s: the title's own copy is current; the fetched one removed", e.path.c_str());
			}
			continue;
		}
		if (fetched_before ? matches(have) : shipped_matches)
			continue;
		body.clear();
		const int st = get(files_url + e.path, &body);
		if (st != 200 || body.size() != e.bytes || Sha256Hex(body.data(), body.size()) != e.sha)
		{
			fixes_log("game fixes: %s: not taken (status %d, %zu bytes, the list says %llu)", e.path.c_str(), st, body.size(),
				static_cast<unsigned long long>(e.bytes));
			report->failed++;
			if (st < 0)
			{
				// No connection any more: the rest is left for the next start.
				report->failed += static_cast<unsigned>(entries.size()) - static_cast<unsigned>(&e - entries.data()) - 1;
				break;
			}
			continue;
		}
		if (!folders)
		{
			MakeFolder(fixes);
			MakeFolder(fixes + "/gamedb");
			MakeFolder(fixes + "/patches");
			folders = true;
		}
		if (!WriteOver(place, body.data(), body.size()))
		{
			fixes_log("game fixes: %s could not be written (%s)", place.c_str(), std::strerror(errno));
			report->failed++;
			continue;
		}
		report->fetched++;
		now.insert(e.path);
		fixes_log("game fixes: %s %s (%llu bytes)", fetched_before ? "updated" : "added", e.path.c_str(), static_cast<unsigned long long>(e.bytes));
		GamesOf(e.path, std::string(body.begin(), body.end()), &report->games);
	}

	// A file fetched before that the list no longer has: its fix was withdrawn.
	for (const std::string& path : before)
	{
		if (listed.count(path) != 0)
			continue;
		const std::string place = fixes + "/" + path;
		std::string text;
		ReadAll(place, &text, MOST_FILE_BYTES);
		now.erase(path);
		if (unlink(place.c_str()) != 0)
			continue;
		report->removed++;
		fixes_log("game fixes: %s removed (no longer on GitHub)", path.c_str());
		GamesOf(path, text, &report->games);
	}

	if (now != before)
	{
		std::string text;
		for (const std::string& path : now)
			text += path + "\n";
		MakeFolder(fixes);
		if (!WriteOver(record, text.data(), text.size()))
			fixes_log("game fixes: the record %s could not be written", record.c_str());
	}
	report->ok = (report->failed == 0);
	if (!report->ok)
		report->error = std::to_string(report->failed) + " game fix file(s) could not be fetched.";
	fixes_log("game fixes: %zu file(s) listed; %u fetched, %u removed, %u failed", entries.size(), report->fetched, report->removed,
		report->failed);
	return report->ok;
}

#if !defined(CRUX2_FIXES_TEST)
void Start()
{
	static std::once_flag s_once;
	std::call_once(s_once, [] {
		if (!Host::GetBaseBoolSettingValue("PS5", "FixesAtStart", true))
		{
			ps5px_log("game fixes: not asked for ([PS5] FixesAtStart is off)");
			return;
		}
		const std::string repository = Host::GetBaseStringSettingValue("PS5", "UpdateRepository", "Press5elect/Crux2");
		std::thread([repository] {
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			const std::string raw = "https://raw.githubusercontent.com/" + repository + "/main/ps5/";
			const std::string shipped = Path::Combine(EmuFolders::Resources, "game-fixes");
			const std::string fixes = Path::Combine(EmuFolders::DataRoot, "game-fixes");
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
			if (!http)
			{
				ps5px_log("game fixes: the downloader could not be made");
				return;
			}
			http->SetTimeout(30.0f);
			const Getter get = [&http](const std::string& url, std::vector<uint8_t>* body) {
				s32 result = HTTPDownloader::HTTP_STATUS_ERROR;
				http->CreateRequest(url, [&result, body](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
					result = status;
					*body = std::move(data);
				});
				http->WaitForAllRequests();
				return static_cast<int>(result);
			};
			Report report;
			Sync(raw + "game-fixes.txt", raw + "package/resources/game-fixes/", shipped, fixes, get, &report);
			if (report.fetched + report.removed == 0)
				return; // (Nothing new, or no network: nothing is said.)
			// Read again for the games started from now on.
			GameDatabase::crux2ReloadFixes();
			std::string games;
			for (size_t i = 0; i < report.games.size() && i < 4; i++)
				games += (i > 0 ? ", " : "") + report.games[i];
			if (report.games.size() > 4)
				games += fmt::format(" and {} more", report.games.size() - 4);
			PS5UI::Toast(games.empty() ? std::string("Game fixes came from GitHub.") :
										 fmt::format("Game fixes came from GitHub for {}.", games));
		}).detach();
	});
}
#endif
} // namespace PS5Fixes
