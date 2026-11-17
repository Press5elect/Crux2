// Crux2 - covers from libretro and cheats from GitHub (see ps5_online.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_online.h"

#include "ps5_cheats_logic.h"
#include "ps5_covers_match.h"
#include "ps5_platform.h"
#include "ps5_ui.h"

#include "Config.h"
#include "GameList.h"
#include "Host.h"
#include "Patch.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsInterface.h"

#include "fmt/format.h"

#include <zip.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <functional>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <unistd.h>

namespace
{
	std::string ReadResource(const char* name)
	{
		const std::string path = Path::Combine(EmuFolders::Resources, name);
		std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
		if (!text.has_value())
			ps5px_log("online: %s is not in the title folder's resources", name);
		return text.value_or(std::string());
	}

	/// One request, waited for. status: 200, another answer of the server, or less than 0 when
	/// there was no answer at all (no network, no name, no TLS, a timeout).
	/// `progress` (may be null) is what can cancel it while it waits.
	s32 Get(HTTPDownloader* http, const std::string& url, std::vector<u8>* body, ProgressCallback* progress = nullptr)
	{
		s32 result = HTTPDownloader::HTTP_STATUS_ERROR;
		http->CreateRequest(url, [&result, body](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
			result = status;
			*body = std::move(data);
		}, progress);
		http->WaitForAllRequests();
		return result;
	}

	const char* StatusText(s32 status)
	{
		return status == HTTPDownloader::HTTP_STATUS_TIMEOUT ? "no answer in time" :
			   status == HTTPDownloader::HTTP_STATUS_CANCELLED ? "cancelled" :
			   status < 0 ? "no connection (the network, the name or the certificate)" : "the server's answer";
	}
} // namespace

namespace PS5Covers
{
namespace
{
	/// Pictures of libretro's collection asked for several at a time (one at a time, a list of a few
	/// hundred games took many minutes: each picture is a round trip or two). Each picture goes the
	/// way it went alone: from the address its first request went to, from the plain host when
	/// GitHub gives no answer at all (and every picture after it), and at most one link followed (a
	/// name that stands for another picture). What is done with a picture that came, or did not, is
	/// the caller's, on its own thread, one at a time.
	class Pool
	{
	public:
		static constexpr size_t IN_FLIGHT = 3;
		using Done = std::function<void(s32 status, std::vector<u8>& body)>;

		Pool(HTTPDownloader* http, ProgressCallback* progress, const char* raw, const char* plain, const char* what, bool log_first)
			: m_http(http), m_progress(progress), m_raw(raw), m_plain(plain), m_base(raw), m_what(what), m_log_first(log_first)
		{
			m_http->SetMaxActiveRequests(static_cast<u32>(IN_FLIGHT));
		}
		~Pool()
		{
			// (Normally nothing is left: the callers wait for every picture. The answers still to
			// come are written into what is here, so it goes after them.)
			if (!m_ones.empty())
				m_http->WaitForAllRequests();
		}

		/// `folder` is beside the name in the collection (a link is in the same folder).
		void Start(std::string folder, std::string name, Done done)
		{
			std::unique_ptr<One> one = std::make_unique<One>();
			one->folder = std::move(folder);
			one->name = std::move(name);
			one->base = m_base;
			one->done = std::move(done);
			Ask(one.get());
			m_ones.push_back(std::move(one));
		}
		size_t Active() const { return m_ones.size(); }
		bool Full() const { return m_ones.size() >= IN_FLIGHT; }
		bool FromPlain() const { return m_base == m_plain; }

		/// The answers that came: each picture's next request asked, or its `done` called.
		void Poll()
		{
			m_http->PollRequests();
			std::vector<std::unique_ptr<One>> arrived;
			for (auto it = m_ones.begin(); it != m_ones.end();)
			{
				if ((*it)->arrived)
				{
					arrived.push_back(std::move(*it));
					it = m_ones.erase(it);
				}
				else
				{
					++it;
				}
			}
			if (arrived.empty())
			{
				usleep(2000);
				return;
			}
			for (std::unique_ptr<One>& one : arrived)
			{
				if (Step(one.get()))
					m_ones.push_back(std::move(one));
			}
		}

		/// Until every picture asked for is done.
		void Drain()
		{
			while (!m_ones.empty())
				Poll();
		}

	private:
		struct One
		{
			std::string folder, name;
			const char* base = nullptr;
			int hop = 0;
			bool retried = false, arrived = false;
			s32 status = 0;
			std::vector<u8> body;
			Done done;
		};

		void Ask(One* one)
		{
			one->arrived = false;
			one->status = 0;
			one->body.clear();
			m_http->CreateRequest(std::string(one->base) + PS5Cheats::EncodePath(one->folder + one->name),
				[one](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
					one->status = status;
					one->body = std::move(data);
					one->arrived = true;
				},
				m_progress);
		}

		/// True when the picture asked again (it stays in the pool).
		bool Step(One* one)
		{
			const bool cancelled = m_progress->IsCancelled();
			// (A pool with one address, the collection by serial, has nowhere else to ask.)
			if (one->status < 0 && m_plain != m_raw && one->base == m_raw && !one->retried && one->status != HTTPDownloader::HTTP_STATUS_CANCELLED &&
				!cancelled)
			{
				// GitHub is not to be had: libretro's own server, for this and the rest.
				if (m_base == m_raw)
				{
					ps5px_log("%s: raw.githubusercontent.com: %s; trying thumbnails.libretro.com", m_what, StatusText(one->status));
					m_base = m_plain;
				}
				one->base = m_plain;
				one->retried = true;
				Ask(one);
				return true;
			}
			if (!cancelled && m_log_first)
			{
				m_log_first = false;
				ps5px_log("%s: first request: status %d, %zu bytes, from %s", m_what, one->status, one->body.size(),
					one->base == m_raw ? "raw.githubusercontent.com" : "thumbnails.libretro.com");
			}
			const std::string link = (!cancelled && one->status == 200 && one->hop == 0) ? LinkTarget(one->body.data(), one->body.size()) : std::string();
			if (!link.empty())
			{
				one->name = link;
				one->hop = 1;
				one->base = m_base;
				one->retried = false;
				Ask(one);
				return true;
			}
			one->done(one->status, one->body);
			return false;
		}

		HTTPDownloader* m_http;
		ProgressCallback* m_progress;
		const char* m_raw;
		const char* m_plain;
		const char* m_base;
		const char* m_what;
		bool m_log_first;
		std::list<std::unique_ptr<One>> m_ones;
	};
} // namespace
} // namespace PS5Covers

// ------------------------------------------------------------------------------------- covers

namespace PS5Covers
{
namespace
{
	constexpr const char* RAW_BASE = "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_2/master/Named_Boxarts/";
	// libretro's own server has the same pictures and also answers without TLS: tried when
	// GitHub cannot be reached at all.
	constexpr const char* PLAIN_BASE = "http://thumbnails.libretro.com/Sony%20-%20PlayStation%202/Named_Boxarts/";
	// A second collection, asked by serial for the games libretro's has no box art for (xlenore's
	// ps2-covers, the one PCSX2's users commonly name as their cover address): <serial>.jpg.
	constexpr const char* SERIAL_BASE = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/";

	std::mutex s_index_mutex;
	Index* s_index = nullptr;

	const Index* TheIndex()
	{
		const std::lock_guard<std::mutex> lock(s_index_mutex);
		if (s_index == nullptr)
		{
			s_index = new Index();
			s_index->Build(ParseNameList(ReadResource("ps5/boxarts.txt")));
			ps5px_log("covers: %zu box-art names in the list", s_index->Size());
		}
		return s_index;
	}

	struct Missing
	{
		std::string serial, title, title_en, region, stem;
	};

	// The downloads writing into the covers folder now.
	std::atomic<int> s_downloading{0};
	struct DownloadingNow
	{
		DownloadingNow() { s_downloading.fetch_add(1, std::memory_order_acq_rel); }
		~DownloadingNow() { s_downloading.fetch_sub(1, std::memory_order_acq_rel); }
	};
} // namespace

bool Downloading()
{
	return s_downloading.load(std::memory_order_acquire) != 0;
}

int DownloadMissing(ProgressCallback* progress, bool again, std::string* summary)
{
	const Index* const index = TheIndex();
	if (index->Size() == 0)
	{
		*summary = "The list of covers is missing from the title folder.";
		return 0;
	}

	// The games nothing was found for before: not asked for on every scan.
	const std::string none_path = Path::Combine(EmuFolders::Covers, "no-cover.txt");
	std::set<std::string> none;
	if (!again)
	{
		const std::string text = FileSystem::ReadFileToString(none_path.c_str()).value_or(std::string());
		PS5Cheats::ForEachLine(text, [&](std::string_view line) {
			if (!line.empty())
				none.emplace(line);
		});
	}

	std::vector<Missing> missing;
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry == nullptr || entry->serial.empty() || !GameList::GetCoverImagePathForEntry(entry).empty())
				continue;
			Missing m;
			m.serial = entry->serial;
			m.title = entry->title;
			m.title_en = entry->title_en;
			m.region = GameList::RegionToString(entry->region, false);
			m.stem = std::string(Path::GetFileTitle(entry->path));
			// (The discs of one game have one serial each; a serial is asked for once.)
			if (std::none_of(missing.begin(), missing.end(), [&](const Missing& other) { return other.serial == m.serial; }))
				missing.push_back(std::move(m));
		}
	}
	if (missing.empty())
	{
		*summary = "Every game in the list has a cover.";
		return 0;
	}

	// The title has never fetched anything over HTTPS on a console before this version. Should a
	// download ever end the title, the next start must not walk into the same thing by itself:
	// a note is left while one runs, and a note found from a run that did not come back keeps the
	// automatic download off until it is asked for by hand.
	const std::string running_path = Path::Combine(EmuFolders::Covers, "download-running.txt");
	if (!again && FileSystem::FileExists(running_path.c_str()))
	{
		// This start only: the note goes, and the user is told once (the next start tries again).
		ps5px_log("covers: the last download did not come back (%s is there): not tried by itself this time. "
				  "Settings > Interface > Download the missing covers now tries it.", running_path.c_str());
		FileSystem::DeleteFilePath(running_path.c_str());
		*summary = "Covers were not downloaded by themselves this time: the last download did not finish. \"Download the missing covers now\" tries again.";
		PS5UI::Toast(*summary);
		return 0;
	}

	// No connection a moment ago: the scans that follow do not each wait for it again (asked for
	// by hand, it is tried at once).
	static std::time_t s_unreachable_at = 0;
	if (!again && s_unreachable_at != 0 && std::time(nullptr) - s_unreachable_at < 15 * 60)
	{
		*summary = "The covers were not asked for: there was no connection a moment ago.";
		return 0;
	}

	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
	{
		ps5px_log("covers: the downloader could not be made");
		*summary = "The cover download could not start (crux2-boot.log has the reason).";
		return 0;
	}
	http->SetTimeout(12.0f);
	const DownloadingNow downloading;
	FileSystem::WriteStringToFile(running_path.c_str(), "A cover download is running. If this file is still here when Crux2 starts, the download ended the title.\n");

	// The collection by serial has a downloader (three connections) of its own: neither pool waits
	// for the other's.
	std::unique_ptr<HTTPDownloader> http_serial = HTTPDownloader::Create();
	if (!http_serial)
	{
		ps5px_log("covers: the downloader could not be made");
		*summary = "The cover download could not start (crux2-boot.log has the reason).";
		FileSystem::DeleteFilePath(running_path.c_str());
		return 0;
	}
	http_serial->SetTimeout(12.0f);

	int saved = 0, unmatched = 0, skipped = 0, failed = 0;
	// Each collection's own count of no answer, and its own stop.
	int unreachable = 0, unreachable_serial = 0;
	bool lost = false, lost_serial = false;
	std::string none_new;
	progress->SetProgressRange(static_cast<u32>(missing.size()));
	// Three games asked for at a time; what came for each is handled as it comes, as it was when
	// they were asked for one by one.
	Pool pool(http.get(), progress, RAW_BASE, PLAIN_BASE, "covers", true);
	// The games libretro's collection has nothing for are asked for by serial in the second one.
	Pool by_serial(http_serial.get(), progress, SERIAL_BASE, SERIAL_BASE, "covers by serial", true);
	std::deque<size_t> second;
	int saved_by_serial = 0;
	const auto lostConnection = [&](s32 status, int& count, bool& stopped, const char* what) {
		// No connection: three in a row, and the rest of that collection is left for the next time
		// (the games asked for already are let finish).
		failed++;
		if (++count >= 3 && !stopped)
		{
			ps5px_log("%s: stopping: %s, three times in a row", what, StatusText(status));
			if (&stopped == &lost)
				s_unreachable_at = std::time(nullptr);
			stopped = true;
		}
	};
	size_t next = 0;
	while (true)
	{
		while (!by_serial.Full() && !second.empty() && !progress->IsCancelled() && !lost_serial)
		{
			const size_t i = second.front();
			second.pop_front();
			const std::string none_line = NoSerialCoverLine(missing[i].serial);
			if (none.count(none_line) != 0)
			{
				skipped++;
				continue;
			}
			by_serial.Start(std::string(), missing[i].serial + ".jpg", [&, i, none_line](s32 status, std::vector<u8>& body) {
				const Missing& m = missing[i];
				if (progress->IsCancelled())
					return;
				if (status == 200 && IsJPEG(body.data(), body.size()))
				{
					const std::string path = Path::Combine(EmuFolders::Covers, m.serial + ".jpg");
					if (FileSystem::WriteBinaryFile(path.c_str(), body.data(), body.size()))
					{
						PS5Platform::MakePublic(path.c_str(), false);
						saved++;
						saved_by_serial++;
						unreachable_serial = 0;
					}
					else
					{
						ps5px_log("covers: %s could not be written", path.c_str());
						failed++;
					}
				}
				else if (status < 0)
				{
					lostConnection(status, unreachable_serial, lost_serial, "covers by serial");
				}
				else if (IsNotThere(status))
				{
					// Neither collection has it: not asked for again.
					unmatched++;
					none_new += none_line + "\n";
				}
				else
				{
					// (Refused, too many asked, the server's trouble, or not a picture: asked for again
					// the next time.)
					ps5px_log("covers by serial: %s: status %d, %zu bytes", m.serial.c_str(), status, body.size());
					failed++;
				}
			});
		}
		while (!pool.Full() && next < missing.size() && !progress->IsCancelled() && !lost)
		{
			const size_t i = next++;
			const Missing& m = missing[i];
			progress->SetProgressValue(static_cast<u32>(i));
			const std::string none_line = NoMatchLine(m.serial, m.stem);
			if (none.count(none_line) != 0)
			{
				// (Found missing from libretro's before: the second collection may have it.)
				second.push_back(i);
				continue;
			}
			Game game;
			game.file_stem = m.stem;
			game.title = m.title;
			game.title_en = m.title_en;
			game.region = m.region;
			const Match match = index->Find(game);
			if (match.index < 0)
			{
				none_new += none_line + "\n";
				second.push_back(i);
				continue;
			}
			pool.Start(std::string(), index->Name(match.index) + ".png", [&, i, none_line](s32 status, std::vector<u8>& body) {
				const Missing& m = missing[i];
				if (progress->IsCancelled())
					return;
				if (status == 200 && IsPNG(body.data(), body.size()))
				{
					const std::string path = Path::Combine(EmuFolders::Covers, m.serial + ".png");
					if (FileSystem::WriteBinaryFile(path.c_str(), body.data(), body.size()))
					{
						PS5Platform::MakePublic(path.c_str(), false);
						saved++;
						unreachable = 0;
					}
					else
					{
						ps5px_log("covers: %s could not be written", path.c_str());
						failed++;
					}
				}
				else if (status < 0)
				{
					lostConnection(status, unreachable, lost, "covers");
				}
				else if (IsNotThere(status))
				{
					// The list names a picture the server does not have (any more): not asked for
					// there again, and asked for by serial.
					ps5px_log("covers: %s: status %d, %zu bytes", m.serial.c_str(), status, body.size());
					none_new += none_line + "\n";
					second.push_back(i);
				}
				else
				{
					// (Refused, too many asked, the server's trouble, or not a picture: asked for again
					// the next time.)
					ps5px_log("covers: %s: status %d, %zu bytes", m.serial.c_str(), status, body.size());
					failed++;
				}
			});
		}
		const bool more = !second.empty() && !progress->IsCancelled() && !lost_serial;
		if (pool.Active() == 0 && by_serial.Active() == 0 && !more)
			break;
		if (pool.Active() != 0)
			pool.Poll();
		if (by_serial.Active() != 0)
			by_serial.Poll();
	}
	if (!none_new.empty() || again)
	{
		std::string text = again ? std::string() : FileSystem::ReadFileToString(none_path.c_str()).value_or(std::string());
		text += none_new;
		FileSystem::WriteStringToFile(none_path.c_str(), text);
		PS5Platform::MakePublic(none_path.c_str(), false);
	}
	FileSystem::DeleteFilePath(running_path.c_str());
	ps5px_log("covers: %zu game(s) without a cover: %d saved (%d of them by serial), %d in neither collection, %d not asked for again, %d failed%s",
		missing.size(), saved, saved_by_serial, unmatched, skipped, failed, progress->IsCancelled() ? " (stopped: a scan or a game took over)" : "");
	if (lost || lost_serial || (saved == 0 && failed > 0 && unmatched + skipped == 0))
		*summary = saved > 0 ? fmt::format("{} cover(s) downloaded; then the connection was lost.", saved) :
							   std::string("The covers could not be downloaded: no connection (crux2-boot.log has the reason).");
	else if (saved > 0)
		*summary = fmt::format("{} cover(s) downloaded{}.", saved, unmatched + skipped > 0 ? fmt::format("; {} game(s) have none in either collection", unmatched + skipped) : "");
	else
		*summary = fmt::format("No new covers: {} game(s) have none in either collection.", unmatched + skipped + failed);
	return saved;
}

// ---- title screens and pictures of play

std::string PicturesFolder()
{
	return Path::Combine(EmuFolders::DataRoot, "pictures");
}

std::string PicturePath(const std::string& serial, int which)
{
	return Path::Combine(PicturesFolder(), serial + (which == 0 ? ".title.png" : ".snap.png"));
}

int DownloadPictures(ProgressCallback* progress, const std::string& serial, bool again, std::string* summary)
{
	static constexpr const char* RAW = "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_2/master/";
	static constexpr const char* PLAIN = "http://thumbnails.libretro.com/Sony%20-%20PlayStation%202/";
	static constexpr const char* const kinds[2] = {"Named_Titles/", "Named_Snaps/"};

	// (The pictures go by the names the box arts go by: the same list finds a game's name.)
	const Index* const index = TheIndex();
	if (index->Size() == 0)
	{
		*summary = "The list of names is missing from the title folder.";
		return 0;
	}
	const std::string folder = PicturesFolder();
	if (!FileSystem::DirectoryExists(folder.c_str()))
	{
		FileSystem::CreateDirectoryPath(folder.c_str(), false);
		PS5Platform::MakePublic(folder.c_str(), true);
	}

	const std::string none_path = Path::Combine(folder, "no-picture.txt");
	std::set<std::string> none;
	if (!again)
	{
		const std::string text = FileSystem::ReadFileToString(none_path.c_str()).value_or(std::string());
		PS5Cheats::ForEachLine(text, [&](std::string_view line) {
			if (!line.empty())
				none.emplace(line);
		});
	}

	std::vector<Missing> missing;
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry == nullptr || entry->serial.empty() || entry->type != GameList::EntryType::PS2Disc)
				continue;
			if (!serial.empty() && entry->serial != serial)
				continue;
			// (One game asked for: what it has is fetched anew. All of them: only those with nothing.)
			if (serial.empty() && (FileSystem::FileExists(PicturePath(entry->serial, 0).c_str()) || FileSystem::FileExists(PicturePath(entry->serial, 1).c_str())))
				continue;
			Missing m;
			m.serial = entry->serial;
			m.title = entry->title;
			m.title_en = entry->title_en;
			m.region = GameList::RegionToString(entry->region, false);
			m.stem = std::string(Path::GetFileTitle(entry->path));
			if (std::none_of(missing.begin(), missing.end(), [&](const Missing& other) { return other.serial == m.serial; }))
				missing.push_back(std::move(m));
		}
	}
	if (missing.empty())
	{
		*summary = serial.empty() ? "Every PlayStation 2 game of the list has its pictures, or was looked for before." :
									"That game is not in the list any more.";
		return 0;
	}

	// (As with the covers: a download that ended the title is not walked into by itself again.)
	const std::string running_path = Path::Combine(folder, "download-running.txt");
	if (serial.empty() && !again && FileSystem::FileExists(running_path.c_str()))
	{
		// This start only: the note goes, and the user is told once (the next start tries again).
		ps5px_log("pictures: the last download did not come back (%s is there): not tried by itself this time", running_path.c_str());
		FileSystem::DeleteFilePath(running_path.c_str());
		*summary = "Pictures were not fetched by themselves this time: the last download did not finish. Settings > Library > \"Fetch the games' pictures\" tries again.";
		PS5UI::Toast(*summary);
		return 0;
	}

	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
	{
		ps5px_log("pictures: the downloader could not be made");
		*summary = "The download could not start (crux2-boot.log has the reason).";
		return 0;
	}
	http->SetTimeout(12.0f);
	FileSystem::WriteStringToFile(running_path.c_str(), "A download of pictures is running. If this file is still here when Crux2 starts, the download ended the title.\n");

	int saved = 0, games_with = 0, unmatched = 0, skipped = 0, unreachable = 0;
	std::string none_new;
	bool lost = false;
	progress->SetProgressRange(static_cast<u32>(missing.size()));
	// Three games asked for at a time, each game's title screen and then its picture of play.
	struct Asked
	{
		size_t game = 0;
		std::string name, none_line;
		int got = 0;
		bool answered = true;
	};
	Pool pool(http.get(), progress, RAW, PLAIN, "pictures", false);
	std::function<void(std::shared_ptr<Asked>, int)> ask;
	ask = [&](std::shared_ptr<Asked> asked, int which) {
		pool.Start(kinds[which], asked->name, [&, asked, which](s32 status, std::vector<u8>& body) {
			if (progress->IsCancelled())
				return;
			const Missing& m = missing[asked->game];
			if (status == 200 && IsPNG(body.data(), body.size()))
			{
				const std::string path = PicturePath(m.serial, which);
				if (FileSystem::WriteBinaryFile(path.c_str(), body.data(), body.size()))
				{
					PS5Platform::MakePublic(path.c_str(), false);
					saved++;
					asked->got++;
					unreachable = 0;
				}
				else
				{
					ps5px_log("pictures: %s could not be written", path.c_str());
				}
			}
			else if (status < 0)
			{
				asked->answered = false;
				if (++unreachable >= 3 && !lost)
				{
					ps5px_log("pictures: stopping: %s, three times in a row", StatusText(status));
					lost = true;
				}
			}
			else if (!IsNotThere(status))
			{
				// (Refused, too many asked, the server's trouble, or not a picture: not settled, asked
				// for again the next time. Only "not there" is remembered.)
				ps5px_log("pictures: %s: status %d, %zu bytes", m.serial.c_str(), status, body.size());
				asked->answered = false;
			}
			if (which == 0 && !lost)
			{
				ask(asked, 1);
				return;
			}
			if (which == 0)
				asked->answered = false; // its picture of play not asked for: not settled
			if (asked->got > 0)
				games_with++;
			else if (asked->answered)
				none_new += asked->none_line + "\n"; // the collection has neither: not asked for again
		});
	};
	size_t next = 0;
	while (true)
	{
		while (!pool.Full() && next < missing.size() && !progress->IsCancelled() && !lost)
		{
			const size_t i = next++;
			const Missing& m = missing[i];
			progress->SetProgressValue(static_cast<u32>(i));
			const std::string none_line = NoMatchLine(m.serial, m.stem);
			if (none.count(none_line) != 0)
			{
				skipped++;
				continue;
			}
			Game game;
			game.file_stem = m.stem;
			game.title = m.title;
			game.title_en = m.title_en;
			game.region = m.region;
			const Match match = index->Find(game);
			if (match.index < 0)
			{
				unmatched++;
				none_new += none_line + "\n";
				continue;
			}
			std::shared_ptr<Asked> asked = std::make_shared<Asked>();
			asked->game = i;
			asked->name = index->Name(match.index) + ".png";
			asked->none_line = none_line;
			ask(std::move(asked), 0);
		}
		if (pool.Active() == 0)
			break;
		pool.Poll();
	}
	if (!none_new.empty() || (again && serial.empty()))
	{
		std::string text = (again && serial.empty()) ? std::string() : FileSystem::ReadFileToString(none_path.c_str()).value_or(std::string());
		text += none_new;
		FileSystem::WriteStringToFile(none_path.c_str(), text);
		PS5Platform::MakePublic(none_path.c_str(), false);
	}
	FileSystem::DeleteFilePath(running_path.c_str());
	ps5px_log("pictures: %zu game(s) asked about: %d picture(s) saved for %d game(s), %d not in libretro's collection, %d not asked for again%s%s",
		missing.size(), saved, games_with, unmatched, skipped, lost ? " (the connection was lost)" : "",
		progress->IsCancelled() ? " (stopped: a scan or a game took over)" : "");
	if (lost && saved == 0)
		*summary = "The pictures could not be fetched: no connection (crux2-boot.log has the reason).";
	else if (!serial.empty())
		*summary = saved > 0 ? fmt::format("{} picture(s) fetched for this game.", saved) : std::string("libretro's collection has no pictures for this game.");
	else if (saved > 0)
		*summary = fmt::format("Pictures fetched for {} game(s){}.", games_with, lost ? "; then the connection was lost" : "");
	else
		*summary = "No new pictures: libretro's collection has none for the games that lack them.";
	return saved;
}

// ---- the disc in a drive: in no list, so its pictures are fetched for it alone

int FetchForDisc(const std::string& serial, const std::string& title, const std::string& title_en, const std::string& region)
{
	const Index* const index = TheIndex();
	if (serial.empty() || index->Size() == 0)
		return 0;
	Game game;
	game.title = title;
	game.title_en = title_en;
	game.region = region;
	const Match match = index->Find(game);
	if (match.index < 0)
	{
		ps5px_log("covers: the disc %s (%s) is not in libretro's collection", serial.c_str(), title.c_str());
		return 0;
	}
	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
		return 0;
	http->SetTimeout(12.0f);
	const DownloadingNow downloading;
	static constexpr const char* RAW = "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_2/master/";
	static constexpr const char* PLAIN = "http://thumbnails.libretro.com/Sony%20-%20PlayStation%202/";
	const char* base = RAW;
	if (!FileSystem::DirectoryExists(PicturesFolder().c_str()))
	{
		FileSystem::CreateDirectoryPath(PicturesFolder().c_str(), false);
		PS5Platform::MakePublic(PicturesFolder().c_str(), true);
	}
	// The box art, the title screen, the picture of play: each that is not there yet.
	const std::string targets[3] = {Path::Combine(EmuFolders::Covers, serial + ".png"), PicturePath(serial, 0), PicturePath(serial, 1)};
	static constexpr const char* const kinds[3] = {"Named_Boxarts/", "Named_Titles/", "Named_Snaps/"};
	int saved = 0;
	for (int which = 0; which < 3; which++)
	{
		if (FileSystem::FileExists(targets[which].c_str()))
			continue;
		if (which == 0)
		{
			bool any = false;
			for (const char* const extension : {".jpg", ".jpeg", ".webp"})
				any = any || FileSystem::FileExists(Path::Combine(EmuFolders::Covers, serial + extension).c_str());
			if (any)
				continue;
		}
		std::string name = std::string(kinds[which]) + index->Name(match.index) + ".png";
		std::vector<u8> body;
		s32 status = 0;
		for (int hop = 0; hop < 2; hop++)
		{
			status = Get(http.get(), std::string(base) + PS5Cheats::EncodePath(name), &body);
			if (status < 0 && base == RAW)
			{
				base = PLAIN;
				status = Get(http.get(), std::string(base) + PS5Cheats::EncodePath(name), &body);
			}
			const std::string link = (status == 200) ? LinkTarget(body.data(), body.size()) : std::string();
			if (link.empty())
				break;
			name = std::string(kinds[which]) + link;
		}
		if (status < 0)
		{
			ps5px_log("covers: the disc's pictures: %s", StatusText(status));
			break;
		}
		if (status == 200 && IsPNG(body.data(), body.size()) && FileSystem::WriteBinaryFile(targets[which].c_str(), body.data(), body.size()))
		{
			PS5Platform::MakePublic(targets[which].c_str(), false);
			saved++;
		}
	}
	ps5px_log("covers: the disc %s: %d picture(s) fetched", serial.c_str(), saved);
	return saved;
}
} // namespace PS5Covers

// ------------------------------------------------------------------------------------- cheats

namespace PS5Cheats
{
namespace
{
	std::mutex s_mutex; // one fetch at a time
	Index* s_index = nullptr;

	const Index& TheIndex()
	{
		if (s_index == nullptr)
		{
			s_index = new Index(ParseIndex(ReadResource("ps5/cheat-index.txt")));
			ps5px_log("cheats: %zu file(s) of %zu collection(s) in the list", s_index->files.size(), s_index->sources.size());
		}
		return *s_index;
	}

	std::vector<std::string> UserPatterns()
	{
		const std::string path = PS5Platform::UserPath("cheat-sources.txt");
		std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
		if (!text.has_value())
		{
			// The first time: the file, with what can be written in it.
			if (FileSystem::WriteStringToFile(path.c_str(), DefaultPatternsFile()))
				PS5Platform::MakePublic(path.c_str(), false);
			return {};
		}
		return ParsePatterns(text.value());
	}

	/// Returns files saved, or -1 when there is no connection at all.
	int FetchOne(HTTPDownloader* http, const std::string& serial, u32 crc, const std::vector<std::string>& patterns, int* asked)
	{
		std::vector<Wanted> wanted = FilesFor(TheIndex(), serial, crc);
		for (size_t i = 0; i < patterns.size(); i++)
		{
			Wanted w;
			w.url = ExpandPattern(patterns[i], serial, crc);
			w.save_name = SaveName(serial, crc, "github-own" + (i > 0 ? "-" + std::to_string(i + 1) : std::string()));
			w.from = w.url;
			wanted.push_back(std::move(w));
		}
		int saved = 0;
		for (const Wanted& w : wanted)
		{
			// (A collection's patches go where PCSX2 looks for patches: a game's Patches page.)
			const std::string path = Path::Combine(w.patch ? EmuFolders::Patches : EmuFolders::Cheats, w.save_name);
			if (FileSystem::FileExists(path.c_str()))
				continue;
			std::vector<u8> body;
			const s32 status = Get(http, w.url, &body);
			(*asked)++;
			if (status < 0)
			{
				ps5px_log("cheats: %s: %s", w.from.c_str(), StatusText(status));
				return saved > 0 ? saved : -1;
			}
			std::string why;
			const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
			if (status != 200 || !LooksLikePnach(text, &why))
			{
				ps5px_log("cheats: %s: status %d, %zu bytes%s%s", w.from.c_str(), status, body.size(), why.empty() ? "" : ": ", why.c_str());
				continue;
			}
			char day[32] = "";
			const std::time_t now = std::time(nullptr);
			struct tm local;
			if (localtime_r(&now, &local) != nullptr)
				std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
			std::string file = fmt::format("// From {} - fetched by Crux2 on {}. Delete this file to fetch it again.\n", w.from, day);
			int repaired = 0;
			file.append(RepairPnach(text, &repaired));
			file.push_back('\n');
			if (repaired > 0)
				ps5px_log("cheats: %s: %d cheat name(s) broken over lines joined", w.from.c_str(), repaired);
			if (FileSystem::WriteStringToFile(path.c_str(), file))
			{
				PS5Platform::MakePublic(path.c_str(), false);
				ps5px_log("cheats: saved %s%s (%zu bytes) from %s", w.patch ? "patches/" : "", w.save_name.c_str(), body.size(), w.from.c_str());
				saved++;
			}
			else
			{
				ps5px_log("cheats: %s could not be written", path.c_str());
			}
		}
		return saved;
	}
} // namespace

int FetchForGame(const std::string& serial, u32 crc, std::string* summary)
{
	const std::lock_guard<std::mutex> lock(s_mutex);
	if (crc == 0)
	{
		*summary = "This game's cheats cannot be looked for: it has not been identified yet (start it once).";
		return 0;
	}
	const std::vector<std::string> patterns = UserPatterns();
	if (FilesFor(TheIndex(), serial, crc).empty() && patterns.empty())
	{
		*summary = "None of the cheat collections has this game.";
		return 0;
	}
	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
	{
		*summary = "The download could not start (crux2-boot.log has the reason).";
		return 0;
	}
	http->SetTimeout(20.0f);
	int asked = 0;
	const int saved = FetchOne(http.get(), serial, crc, patterns, &asked);
	if (saved < 0)
		*summary = "The cheats could not be downloaded: no connection (crux2-boot.log has the reason).";
	else if (saved > 0)
		*summary = fmt::format("{} file(s) downloaded. Switch the cheats and patches you want on in the game's options.", saved);
	else if (asked == 0)
		*summary = "This game's cheats are downloaded already.";
	else
		*summary = "No cheats were found for this game.";
	return std::max(saved, 0);
}

int FetchForAllGames(const bool* cancel, std::string* summary)
{
	const std::lock_guard<std::mutex> lock(s_mutex);
	struct One
	{
		std::string serial;
		u32 crc;
	};
	std::vector<One> games;
	{
		const auto lock2 = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry != nullptr && entry->crc != 0 &&
				std::none_of(games.begin(), games.end(), [&](const One& other) { return other.crc == entry->crc && other.serial == entry->serial; }))
				games.push_back({entry->serial, entry->crc});
		}
	}
	const std::vector<std::string> patterns = UserPatterns();
	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
	{
		*summary = "The download could not start (crux2-boot.log has the reason).";
		return 0;
	}
	http->SetTimeout(20.0f);
	int files = 0, with = 0, asked = 0;
	bool lost = false;
	for (const One& game : games)
	{
		if (cancel != nullptr && *cancel)
			break;
		if (FilesFor(TheIndex(), game.serial, game.crc).empty() && patterns.empty())
			continue;
		const int before = asked;
		const int saved = FetchOne(http.get(), game.serial, game.crc, patterns, &asked);
		if (saved < 0)
		{
			lost = true;
			break;
		}
		files += saved;
		with += (saved > 0) ? 1 : 0;
		// (A pause between games that were asked for: this is someone's server.)
		if (asked != before)
			usleep(150 * 1000);
	}
	ps5px_log("cheats: all games: %zu game(s), %d request(s), %d file(s) saved for %d game(s)%s", games.size(), asked, files, with, lost ? ", then no connection" : "");
	if (lost && files == 0)
		*summary = "The cheats could not be downloaded: no connection (crux2-boot.log has the reason).";
	else if (files > 0)
		*summary = fmt::format("Cheats and patches for {} game(s) downloaded ({} file(s)). They are off until switched on in a game's options.", with, files);
	else
		*summary = "No new cheats: the collections have nothing more for these games.";
	return files;
}
} // namespace PS5Cheats

// ------------------------------------------------------------------------------------ patches

namespace PS5Patches
{
namespace
{
constexpr const char* URL = "https://github.com/PCSX2/pcsx2_patches/releases/latest/download/patches.zip";

std::string ZipPath()
{
	return Path::Combine(EmuFolders::Resources, "patches.zip");
}

// How many games' patch files a collection holds; less than 0 when it is no zip that opens.
s64 CountGames(const std::string& path)
{
	int error = 0;
	zip_t* const zip = zip_open(path.c_str(), ZIP_RDONLY, &error);
	if (!zip)
		return -1;
	s64 games = 0;
	const zip_int64_t entries = zip_get_num_entries(zip, 0);
	for (zip_int64_t i = 0; i < entries; i++)
	{
		const char* const name = zip_get_name(zip, static_cast<zip_uint64_t>(i), 0);
		const size_t length = name ? std::strlen(name) : 0;
		if (length > 6 && std::strcmp(name + length - 6, ".pnach") == 0)
			games++;
	}
	zip_close(zip);
	return games;
}
} // namespace

bool Download(std::string* summary)
{
	std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create();
	if (!http)
	{
		*summary = "The patches could not be downloaded: the downloader did not start.";
		return false;
	}
	http->SetTimeout(60.0f);

	std::vector<u8> body;
	const s32 status = Get(http.get(), URL, &body);
	if (status != HTTPDownloader::HTTP_STATUS_OK)
	{
		ps5px_log("patches: %s: %s (%d)", URL, StatusText(status), status);
		*summary = (status < 0) ? "The patches could not be downloaded: no connection to GitHub." :
		                           fmt::format("The patches could not be downloaded: GitHub answered {}.", status);
		return false;
	}
	if (body.size() < 4096 || body[0] != 'P' || body[1] != 'K')
	{
		ps5px_log("patches: what came is no zip file (%zu bytes)", body.size());
		*summary = "The patches could not be downloaded: what came is not the collection.";
		return false;
	}

	const std::string path = ZipPath(), part = path + ".part";
	if (!FileSystem::WriteBinaryFile(part.c_str(), body.data(), body.size()))
	{
		*summary = "The patches were downloaded, but could not be written to the title folder.";
		return false;
	}
	const s64 games = CountGames(part);
	if (games <= 0)
	{
		FileSystem::DeleteFilePath(part.c_str());
		ps5px_log("patches: the downloaded file does not open as a collection of patches");
		*summary = "The patches could not be downloaded: the file that came does not open.";
		return false;
	}

	// The emulator holds the old collection open: let go of it and put the new one in its place
	// on its own thread, so no game reads one through the other's index.
	bool placed = false, widescreen = false;
	Host::RunOnCPUThread(
		[&] {
			Patch::ClosePatchesZip();
			placed = FileSystem::RenamePath(part.c_str(), path.c_str());
			if (!placed)
				return;
			char date[32] = "";
			const time_t now = time(nullptr);
			struct tm local;
			if (localtime_r(&now, &local) != nullptr)
				strftime(date, sizeof(date), "%Y-%m-%d", &local);
			const auto lock = Host::GetSettingsLock();
			SettingsInterface* const si = Host::Internal::GetBaseSettingsLayer();
			si->SetIntValue("PS5", "PatchesGames", static_cast<int>(games));
			si->SetStringValue("PS5", "PatchesDate", date);
			// "Widescreen patches applied automatically": switched on with the first collection,
			// once. (Whether the switch was ever set by hand cannot be told from the settings: the
			// emulator writes every option's default into them. So it is done one time and noted;
			// switched off afterwards, it stays off through later downloads.)
			if (!si->GetBoolValue("PS5", "WidescreenOffered", false))
			{
				si->SetBoolValue("PS5", "WidescreenOffered", true);
				if (!si->GetBoolValue("EmuCore", "EnableWideScreenPatches", false))
				{
					si->SetBoolValue("EmuCore", "EnableWideScreenPatches", true);
					widescreen = true;
				}
			}
		},
		true);
	if (!placed)
	{
		FileSystem::DeleteFilePath(part.c_str());
		*summary = "The patches were downloaded, but could not be put in the title folder.";
		return false;
	}
	Host::RunOnCPUThread([widescreen] {
		Host::CommitBaseSettingChanges();
		if (widescreen)
			VMManager::ApplySettings();
	});

	ps5px_log("patches: PCSX2's collection for %lld games is in place (%zu bytes)%s", static_cast<long long>(games), body.size(),
		widescreen ? "; widescreen patches switched on" : "");
	*summary = fmt::format("Patches for {} games downloaded.{}", games,
		widescreen ? " Widescreen patches are now on for every game that has one (Settings > Picture)." : "");
	return true;
}

std::string Describe()
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(ZipPath().c_str(), &sd))
		return "Not downloaded";
	int games = 0;
	std::string date;
	{
		const auto lock = Host::GetSettingsLock();
		const SettingsInterface* const si = Host::Internal::GetBaseSettingsLayer();
		games = si->GetIntValue("PS5", "PatchesGames", 0);
		date = si->GetStringValue("PS5", "PatchesDate", "");
	}
	if (games > 0 && !date.empty())
		return fmt::format("For {} games, downloaded {}", games, date);
	return fmt::format("In place ({:.1f} MB, copied by hand)", static_cast<double>(sd.Size) / (1024.0 * 1024.0));
}

} // namespace PS5Patches
