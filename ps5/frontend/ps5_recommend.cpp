// Crux2 - recommended settings per game (see ps5_recommend.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 35e1dab and 1880404 (GPL-3.0-or-later). POSIX calls only, as ps5_fixes_fetch.cpp.

#include "ps5_recommend.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>

#if defined(CRUX2_RECOMMEND_TEST)
static void recommend_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void recommend_log(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	std::vfprintf(stderr, format, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}
#else
#include "ps5_platform.h"
#include "ps5_priority.h"

#include "Config.h"
#include "Host.h"

#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include <thread>
#define recommend_log ps5px_log
#endif

namespace PS5Recommend
{
namespace
{
constexpr size_t MOST_BYTES = 1u << 20;

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

std::string Upper(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

bool ReadSmall(const std::string& path, std::string* out)
{
	out->clear();
	FILE* const fp = std::fopen(path.c_str(), "rb");
	if (!fp)
		return false;
	char block[8192];
	size_t got;
	bool ok = true;
	while ((got = std::fread(block, 1, sizeof(block), fp)) > 0)
	{
		if (out->size() + got > MOST_BYTES)
		{
			ok = false;
			break;
		}
		out->append(block, got);
	}
	ok = ok && !std::ferror(fp);
	std::fclose(fp);
	return ok;
}

// The cache as last read, and what it was when read (its size and time).
struct Cache
{
	std::mutex lock;
	std::string path;
	off_t size = -1;
	time_t mtime = 0;
	std::map<std::string, Pick> picks;
};
Cache& TheCache()
{
	static Cache* const c = new Cache();
	return *c;
}
} // namespace

bool SafeSetting(const std::string& section, const std::string& key)
{
	if (key.empty() || section.empty())
		return false;
	for (const char c : key)
	{
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
			return false;
	}
	if (section == "Framerate" || section == "EmuCore")
		return true;
	if (section.compare(0, 8, "EmuCore/") != 0 || section.size() == 8)
		return false;
	// ("EmuCore/GS", "EmuCore/CPU/Recompiler": parts of letters, digits and '_', none empty.)
	for (size_t i = 8; i < section.size(); i++)
	{
		const char c = section[i];
		if (c == '/' && section[i - 1] != '/' && i + 1 < section.size())
			continue;
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
			return false;
	}
	return true;
}

bool Parse(const std::string& text, std::map<std::string, Pick>* picks, unsigned* refused)
{
	picks->clear();
	if (refused)
		*refused = 0;
	size_t at = 0;
	bool first = true;
	std::string game;
	while (at <= text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (first)
		{
			first = false;
			if (Trim(line) != HEADER)
				return false;
			continue;
		}
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line.front() == '[')
		{
			game = (line.back() == ']') ? Upper(Trim(line.substr(1, line.size() - 2))) : std::string();
			if (game.empty() && refused)
				(*refused)++;
			continue;
		}
		const size_t equals = line.find('=');
		if (game.empty() || equals == std::string::npos)
		{
			if (refused)
				(*refused)++;
			continue;
		}
		const std::string name = Trim(line.substr(0, equals)), value = Trim(line.substr(equals + 1));
		if (name == "note")
		{
			(*picks)[game].note = value;
			continue;
		}
		const size_t slash = name.rfind('/');
		const std::string section = (slash == std::string::npos) ? std::string() : name.substr(0, slash);
		const std::string key = (slash == std::string::npos) ? std::string() : name.substr(slash + 1);
		if (!SafeSetting(section, key) || value.size() > 256)
		{
			if (refused)
				(*refused)++;
			continue;
		}
		Pick& pick = (*picks)[game];
		bool replaced = false;
		for (Value& v : pick.values)
		{
			if (v.section == section && v.key == key)
			{
				v.value = value; // (the last of the same setting counts)
				replaced = true;
			}
		}
		if (!replaced)
			pick.values.push_back({section, key, value});
		if (at > text.size())
			break;
	}
	// A game with only a note changes nothing: it is no entry.
	for (auto it = picks->begin(); it != picks->end();)
		it = it->second.values.empty() ? picks->erase(it) : std::next(it);
	return true;
}

bool Refresh(const std::string& url, const std::string& cache, const std::string& checked, time_t now,
	const PS5Fixes::Getter& get, std::string* why)
{
	std::string said;
	if (ReadSmall(checked, &said))
	{
		const time_t last = static_cast<time_t>(std::strtoll(said.c_str(), nullptr, 10));
		struct stat st;
		if (last > 0 && last <= now && now - last < KEPT_SECONDS && stat(cache.c_str(), &st) == 0)
		{
			if (why)
				*why = "asked for less than a day ago";
			return false;
		}
	}
	{
		const std::string stamp = std::to_string(static_cast<long long>(now)) + "\n";
		PS5Fixes::WriteOverSafely(checked, stamp.data(), stamp.size());
	}
	std::vector<uint8_t> body;
	const int status = get(url, &body);
	if (status != 200)
	{
		if (why)
			*why = (status < 0) ? "no answer" : "the answer was " + std::to_string(status);
		return false;
	}
	const std::string text(body.begin(), body.end());
	std::map<std::string, Pick> picks;
	unsigned refused = 0;
	if (body.size() > MOST_BYTES || !Parse(text, &picks, &refused))
	{
		if (why)
			*why = "the download is not a list of recommended settings";
		return false;
	}
	std::string before;
	if (ReadSmall(cache, &before) &&
		PS5Fixes::Sha256Hex(before.data(), before.size()) == PS5Fixes::Sha256Hex(text.data(), text.size()))
	{
		if (why)
			*why = "the same as the copy kept";
		return false;
	}
	if (!PS5Fixes::WriteOverSafely(cache, text.data(), text.size()))
	{
		if (why)
			*why = "it could not be written to " + cache;
		return false;
	}
	recommend_log("recommended settings: %zu game(s) (%u line(s) left out), SHA-256 %s", picks.size(), refused,
		PS5Fixes::Sha256Hex(text.data(), text.size()).c_str());
	if (why)
		*why = "fetched";
	return true;
}

bool For(const std::string& cache, const std::string& serial, Pick* pick)
{
	Cache& c = TheCache();
	const std::lock_guard lock(c.lock);
	struct stat st;
	if (stat(cache.c_str(), &st) != 0)
		return false;
	if (c.path != cache || c.size != st.st_size || c.mtime != st.st_mtime)
	{
		c.path = cache;
		c.size = st.st_size;
		c.mtime = st.st_mtime;
		std::string text;
		if (!ReadSmall(cache, &text) || !Parse(text, &c.picks))
			c.picks.clear();
	}
	const auto it = c.picks.find(Upper(serial));
	if (it == c.picks.end())
		return false;
	*pick = it->second;
	return true;
}

#if !defined(CRUX2_RECOMMEND_TEST)
namespace
{
std::string CachePath()
{
	return Path::Combine(EmuFolders::DataRoot, "recommended.ini");
}
} // namespace

void Start()
{
	static std::once_flag s_once;
	std::call_once(s_once, [] {
		if (!Host::GetBaseBoolSettingValue("PS5", "FixesAtStart", true))
			return;
		const std::string repository = Host::GetBaseStringSettingValue("PS5", "UpdateRepository", "Press5elect/Crux2");
		std::thread([repository] {
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
			if (!http)
				return;
			http->SetTimeout(30.0f);
			const PS5Fixes::Getter get = [&http](const std::string& url, std::vector<uint8_t>* body) {
				s32 result = HTTPDownloader::HTTP_STATUS_ERROR;
				http->CreateRequest(url, [&result, body](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
					result = status;
					*body = std::move(data);
				});
				http->WaitForAllRequests();
				return static_cast<int>(result);
			};
			std::string why;
			Refresh("https://raw.githubusercontent.com/" + repository + "/main/ps5/recommended.ini", CachePath(),
				Path::Combine(EmuFolders::DataRoot, "recommended.checked"), time(nullptr), get, &why);
			ps5px_log("recommended settings: %s", why.c_str());
		}).detach();
	});
}

bool ForGame(const std::string& serial, Pick* pick)
{
	return !serial.empty() && For(CachePath(), serial, pick);
}
#endif
} // namespace PS5Recommend
