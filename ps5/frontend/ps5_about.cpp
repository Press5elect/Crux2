// Crux2 - what a game is, from Wikipedia and Wikidata (see ps5_about.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_about.h"

#include "ps5_platform.h"
#include "ps5_priority.h"

#include "Config.h"
#include "Host.h"

#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace PS5About
{
	namespace
	{
		constexpr std::int64_t AGAIN_AFTER = 14 * 24 * 3600; // a game not found is looked for again after two weeks

		std::mutex s_mutex;
		std::map<std::string, Info> s_known; // by serial
		std::set<std::string> s_asked; // looked for in this run (found or not, or no answer)
		std::deque<std::pair<std::string, std::string>> s_queue; // serial, title
		bool s_running = false;

		std::string Folder()
		{
			return Path::Combine(EmuFolders::DataRoot, "about");
		}

		std::string FileOf(const std::string& serial)
		{
			return Path::Combine(Folder(), serial + ".txt");
		}

		s32 Get(HTTPDownloader* http, const std::string& url, std::string* body)
		{
			s32 result = HTTPDownloader::HTTP_STATUS_ERROR;
			http->CreateRequest(url, [&result, body](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				result = status;
				body->assign(data.begin(), data.end());
			});
			http->WaitForAllRequests();
			return result;
		}

		// One game: its article, its summary, its item's facts. `answered` false when the network
		// gave no answer at all (then nothing is kept, and the next run asks again).
		Info LookUp(HTTPDownloader* http, const std::string& title, bool* answered)
		{
			Info info;
			info.looked = static_cast<std::int64_t>(std::time(nullptr));
			*answered = true;
			const std::string search = SearchTitle(title);
			if (search.empty())
				return info;
			std::string body, article;
			for (const std::string& words : {search + " PlayStation 2 video game", search + " video game"})
			{
				const s32 status = Get(http, "https://en.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=8&srsearch=" + Encode(words), &body);
				if (status < 0)
				{
					*answered = false;
					ps5px_log("about: Wikipedia did not answer (%d)", status);
					return info;
				}
				if (status == 200)
					article = ChooseArticle(body, search);
				if (!article.empty())
					break;
			}
			if (article.empty())
			{
				ps5px_log("about: no article fits \"%s\"", search.c_str());
				return info;
			}
			std::string path = article;
			for (char& c : path)
			{
				if (c == ' ')
					c = '_';
			}
			std::string item;
			if (Get(http, "https://en.wikipedia.org/api/rest_v1/page/summary/" + Encode(path), &body) != 200 || !ReadSummary(body, &info.summary, &item))
			{
				ps5px_log("about: no summary of \"%s\"", article.c_str());
				return info;
			}
			info.article = article;
			if (!item.empty() && Get(http, "https://www.wikidata.org/w/api.php?action=wbgetentities&format=json&props=claims&ids=" + Encode(item), &body) == 200)
			{
				const Claims claims = ReadClaims(body, item);
				if (!claims.videoGame)
				{
					ps5px_log("about: \"%s\" is not a game's article", article.c_str());
					info.summary.clear();
					info.article.clear();
					return info;
				}
				info.released = claims.released;
				std::vector<std::string> ids;
				for (const std::vector<std::string>* list : {&claims.developers, &claims.publishers, &claims.genres, &claims.modes})
					ids.insert(ids.end(), list->begin(), list->end());
				if (!ids.empty())
				{
					std::string joined;
					for (const std::string& id : ids)
						joined += (joined.empty() ? "" : "|") + id;
					if (Get(http, "https://www.wikidata.org/w/api.php?action=wbgetentities&format=json&props=labels&languages=en&ids=" + Encode(joined), &body) ==
						200)
					{
						info.developer = ReadLabels(body, claims.developers);
						info.publisher = ReadLabels(body, claims.publishers);
						info.genre = ReadLabels(body, claims.genres);
						info.modes = ReadLabels(body, claims.modes);
					}
				}
			}
			info.found = true;
			ps5px_log("about: \"%s\" is \"%s\" (%s, %s)", title.c_str(), article.c_str(), info.developer.c_str(), info.released.c_str());
			return info;
		}

		void Run()
		{
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			std::unique_ptr<HTTPDownloader> http =
				HTTPDownloader::Create(std::string("Crux2/") + ps5px_build_version() + " (a PlayStation 2 emulator for the PS5, by Press5elect)");
			if (http)
				http->SetTimeout(10.0f);
			for (;;)
			{
				std::pair<std::string, std::string> next;
				{
					std::unique_lock lock(s_mutex);
					if (s_queue.empty())
					{
						s_running = false;
						return;
					}
					next = s_queue.front();
					s_queue.pop_front();
				}
				bool answered = false;
				const Info info = http ? LookUp(http.get(), next.second, &answered) : Info();
				if (answered)
				{
					if (!FileSystem::DirectoryExists(Folder().c_str()))
					{
						FileSystem::CreateDirectoryPath(Folder().c_str(), false);
						PS5Platform::MakePublic(Folder().c_str(), true);
					}
					FileSystem::WriteStringToFile(FileOf(next.first).c_str(), Write(info));
				}
				const std::lock_guard lock(s_mutex);
				s_known[next.first] = info;
			}
		}
	} // namespace

	Info Get(const std::string& serial, const std::string& title, bool* looking)
	{
		if (looking)
			*looking = false;
		if (serial.empty())
			return Info();
		std::unique_lock lock(s_mutex);
		if (const auto known = s_known.find(serial); known != s_known.end())
			return known->second;
		if (s_asked.count(serial) != 0)
		{
			if (looking)
				*looking = true;
			return Info();
		}
		s_asked.insert(serial);
		lock.unlock();
		// Kept from before: what was found, or that nothing was (for two weeks).
		if (const std::optional<std::string> kept = FileSystem::ReadFileToString(FileOf(serial).c_str()))
		{
			const Info info = Read(*kept);
			if (info.found || static_cast<std::int64_t>(std::time(nullptr)) - info.looked < AGAIN_AFTER)
			{
				lock.lock();
				s_known[serial] = info;
				return info;
			}
		}
		if (!Host::GetBaseBoolSettingValue("PS5", "FetchAbout", true))
		{
			lock.lock();
			s_known[serial] = Info();
			return Info();
		}
		lock.lock();
		s_queue.emplace_back(serial, title);
		if (!s_running)
		{
			s_running = true;
			std::thread(Run).detach();
		}
		if (looking)
			*looking = true;
		return Info();
	}

	void Forget()
	{
		{
			const std::lock_guard lock(s_mutex);
			s_known.clear();
			s_asked.clear();
		}
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(Folder().c_str(), "*.txt", FILESYSTEM_FIND_FILES, &files);
		for (const FILESYSTEM_FIND_DATA& f : files)
			FileSystem::DeleteFilePath(f.FileName.c_str());
	}
} // namespace PS5About
