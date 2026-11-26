// Crux2 - HD texture packs from the Internet Archive: the runtime of ps5_texture_packs.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The download and the unpacking run at once: the download over one connection or several
// (ps5_texture_packs_fetch.h), libarchive taking the bytes in order as they come. Nothing of the
// archive is kept on the console but the files it holds.

#include "ps5_texture_packs.h"
#include "ps5_priority.h"
#include "ps5_texture_packs_extract.h"
#include "ps5_texture_packs_fetch.h"
#include "ps5_texture_packs_logic.h"
#include "ps5_texture_packs_pieces.h"
#include "ps5_platform.h"

#include "Config.h"
#include "Host.h"
#include "ps5_broker.h"
#include "MTGS.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"
#include "rapidjson/document.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5TexturePacks
{
namespace
{
using namespace PS5TexturePacksLogic;

constexpr time_t LIST_KEPT_SECONDS = 24 * 60 * 60;

std::atomic_bool s_busy{false};
std::atomic_bool s_stop{false};
std::atomic<uint64_t> s_received{0};
std::atomic<int> s_active{0};
std::mutex s_doing_mutex;
std::string s_title;
uint64_t s_total = 0;
// The speed the bar shows, measured on the graphics thread (Now()).
PS5TexturePacksPieces::SpeedMeter s_meter;

std::string ListPath()
{
	return Path::Combine(EmuFolders::Cache, "texture-packs.json");
}

// A thread of its own with room for libarchive's RAR window (the console gives a default thread
// 2 MiB; RAR5's dictionary is on the heap, but its decoder's tables are not small).
bool RunDetached(std::function<void()> work)
{
	auto* const job = new std::function<void()>(std::move(work));
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8u << 20);
	pthread_t thread;
	const int rc = pthread_create(
		&thread, &attr,
		[](void* argument) -> void* {
			auto* const todo = static_cast<std::function<void()>*>(argument);
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			(*todo)();
			delete todo;
			return nullptr;
		},
		job);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		delete job;
		ps5px_log("texture packs: a thread did not start: %d", rc);
		return false;
	}
	pthread_detach(thread);
	return true;
}

// The graphics thread draws the interface: what a job found is handed to it there, by the emulation
// thread (the graphics thread's queue has that one writer). From any thread.
void OnGraphicsThread(std::function<void()> work)
{
	Host::RunOnCPUThread([work = std::move(work)]() mutable {
		if (MTGS::IsOpen())
			MTGS::RunOnGSThread(std::move(work));
	});
}

void Prepare(CURL* curl, const std::string& url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Crux2 (PlayStation 5 homebrew)");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CAINFO, ps5px_ca_bundle());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	// A transfer that brings less than 1 KiB a second for a minute has stopped.
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
}

size_t ToString(char* data, size_t size, size_t count, void* user)
{
	static_cast<std::string*>(user)->append(data, size * count);
	return size * count;
}

// The collection's list of files, from the console's copy when it is fresh.
bool CollectionList(bool again, std::string* json, std::string* error)
{
	const std::string path = ListPath();
	struct stat st;
	if (!again && stat(path.c_str(), &st) == 0 && time(nullptr) - st.st_mtime < LIST_KEPT_SECONDS)
	{
		if (std::optional<std::string> kept = FileSystem::ReadFileToString(path.c_str()); kept.has_value() && !kept->empty())
		{
			*json = std::move(*kept);
			return true;
		}
	}
	CURL* const curl = curl_easy_init();
	if (!curl)
	{
		*error = "the download could not start";
		return false;
	}
	std::string body;
	Prepare(curl, fmt::format("https://archive.org/metadata/{}", COLLECTION));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	const CURLcode rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK || body.empty())
	{
		*error = fmt::format("the Internet Archive did not answer ({})", curl_easy_strerror(rc));
		// An old copy is better than none.
		if (std::optional<std::string> kept = FileSystem::ReadFileToString(path.c_str()); kept.has_value() && !kept->empty())
		{
			*json = std::move(*kept);
			return true;
		}
		return false;
	}
	FileSystem::CreateDirectoryPath(EmuFolders::Cache.c_str(), false);
	FileSystem::WriteStringToFile(path.c_str(), body);
	*json = std::move(body);
	return true;
}

std::vector<Pack> PacksIn(const std::string& json)
{
	std::vector<Pack> packs;
	rapidjson::Document doc;
	doc.Parse(json.c_str(), json.size());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("files") || !doc["files"].IsArray())
		return packs;
	for (const rapidjson::Value& file : doc["files"].GetArray())
	{
		if (!file.IsObject() || !file.HasMember("name") || !file["name"].IsString())
			continue;
		const std::string name = file["name"].GetString();
		if (!IsPack(name))
			continue;
		Pack pack;
		pack.file = name;
		pack.title = TitleOf(name);
		pack.serials = SerialsIn(name);
		if (file.HasMember("size"))
		{
			const rapidjson::Value& size = file["size"];
			if (size.IsString())
				pack.size = std::strtoull(size.GetString(), nullptr, 10);
			else if (size.IsUint64())
				pack.size = size.GetUint64();
		}
		packs.push_back(std::move(pack));
	}
	return packs;
}

} // namespace

std::vector<Place> Places()
{
	std::vector<Place> places;
	places.push_back({EmuFolders::Textures, "This console"});
	if (PS5Platform::IsElevated())
	{
		for (unsigned drive = 0; drive < 8; drive++)
		{
			const std::string root = fmt::format("/mnt/usb{}", drive);
			if (FileSystem::DirectoryExists(root.c_str()))
				places.push_back({root + "/Crux2/textures", fmt::format("USB drive {}", drive + 1)});
		}
	}
	return places;
}

void FindFor(const std::string& serial, bool again, std::function<void(std::vector<Pack>, std::string error)> then)
{
	const bool started = RunDetached([serial, again, then] {
		std::string json, error;
		std::vector<Pack> found;
		if (CollectionList(again, &json, &error))
		{
			for (Pack& pack : PacksIn(json))
			{
				for (const std::string& s : pack.serials)
				{
					if (IEquals(s, serial))
					{
						found.push_back(std::move(pack));
						break;
					}
				}
			}
			error.clear();
		}
		ps5px_log("texture packs: %zu for %s in the collection%s%s", found.size(), serial.c_str(), error.empty() ? "" : ": ", error.c_str());
		OnGraphicsThread([then, found = std::move(found), error]() { then(found, error); });
	});
	if (!started)
		then({}, "a thread could not be started");
}

void FindAll(bool again, std::function<void(std::vector<Pack>, std::string error)> then)
{
	const bool started = RunDetached([again, then] {
		std::string json, error;
		std::vector<Pack> found;
		if (CollectionList(again, &json, &error))
		{
			found = PacksIn(json);
			error.clear();
		}
		ps5px_log("texture packs: %zu packs in the collection%s%s", found.size(), error.empty() ? "" : ": ", error.c_str());
		then(std::move(found), error);
	});
	if (!started)
		then({}, "a thread could not be started");
}

bool Start(const Pack& pack, const Place& place, unsigned connections, std::function<void(bool ok, std::string sentence)> done)
{
	bool expected = false;
	if (!s_busy.compare_exchange_strong(expected, true))
		return false;
	s_stop = false;
	s_received = 0;
	{
		const std::lock_guard lock(s_doing_mutex);
		s_title = pack.title;
		s_total = pack.size;
		s_meter.Clear();
	}
	const bool started = RunDetached([pack, place, connections, done] {
		PS5TexturePacksFetch::Options options;
		options.url = fmt::format("https://archive.org/download/{}/{}", COLLECTION, EncodeForURL(pack.file));
		options.size = pack.size;
		options.connections = std::clamp(connections, 1u, PS5TexturePacksPieces::CONNECTIONS_MOST);
		options.ca_bundle = ps5px_ca_bundle();
		options.stop = [] { return s_stop.load(); };
		options.received = &s_received;
		options.active = &s_active;
		options.log = [](const std::string& line) { ps5px_log("%s", line.c_str()); };
		ps5px_log("texture packs: %s (%s) into %s, %u connections", pack.file.c_str(), SizeText(pack.size).c_str(), place.root.c_str(),
			options.connections);

		PS5TexturePacksExtract::MakeFolders(place.root);
		PS5TexturePacksExtract::Result result;
		const PS5TexturePacksFetch::Outcome fetched = PS5TexturePacksFetch::Run(options, [&](const PS5TexturePacksFetch::Read& read) {
			// (Unpacked onto the console's storage: through the write broker with the setting on.)
			PS5Broker::Session broker("texture pack download");
			result = PS5TexturePacksExtract::Extract(read, place.root, pack.serials, [] { return s_stop.load(); }, &broker);
		});

		std::string sentence;
		const bool stopped = s_stop.load();
		// (The archive read to its end is whole, even where the download was cut off after it: a few
		// bytes past the end of an archive are not read.)
		const bool whole = result.ok && fetched.complete;
		if (whole)
		{
			sentence = fmt::format("{}: {} textures ({}) on {}. The game uses them now.", pack.title, result.files, SizeText(result.bytes), place.name);
		}
		else if (stopped)
		{
			sentence = fmt::format("{}: stopped. {} textures were unpacked whole and stay on {}.", pack.title, result.files, place.name);
		}
		else
		{
			const std::string why = !fetched.complete ? fetched.error :
										(result.error.empty() ? std::string("the archive ended early") : result.error);
			sentence = fmt::format("{}: not complete ({}). {} textures were unpacked whole and stay on {}.", pack.title, why, result.files, place.name);
		}
		ps5px_log("texture packs: %s", sentence.c_str());
		const bool ok = whole;
		s_busy = false;
		OnGraphicsThread([done, ok, sentence]() { done(ok, sentence); });
	});
	if (!started)
	{
		s_busy = false;
		return false;
	}
	return true;
}

void Stop()
{
	s_stop = true;
}

bool Busy()
{
	return s_busy.load();
}

std::string Doing()
{
	if (!s_busy.load())
		return {};
	const std::lock_guard lock(s_doing_mutex);
	const uint64_t got = s_received.load(std::memory_order_relaxed);
	if (s_total == 0)
		return fmt::format("{}: {}", s_title, SizeText(got));
	return fmt::format("{}: {} of {} ({}%)", s_title, SizeText(got), SizeText(s_total), std::min<uint64_t>(got * 100 / s_total, 100));
}

Progress Now()
{
	Progress p;
	if (!s_busy.load())
		return p;
	const std::lock_guard lock(s_doing_mutex);
	p.active = true;
	p.title = s_title;
	p.got = s_received.load(std::memory_order_relaxed);
	p.total = s_total;
	p.connections = s_active.load(std::memory_order_relaxed);
	s_meter.Add(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), p.got);
	p.speed = s_meter.BytesPerSecond();
	p.seconds_left = PS5TexturePacksPieces::SecondsLeft(p.got, p.total, p.speed);
	return p;
}
} // namespace PS5TexturePacks
