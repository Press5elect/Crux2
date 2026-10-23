// Crux2 - the title's services that are wired to PCSX2's own pieces (ps5_services.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_services.h"
#include "ps5_platform.h"
#include "ps5_update.h"

#include "Host.h"

#include "common/HTTPDownloader.h"
#include "common/ProgressCallback.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
	// What a download's progress is told through, and how it is cancelled: PCSX2's progress callback
	// with the updater's question behind it.
	class UpdateProgress final : public BaseProgressCallback
	{
	public:
		explicit UpdateProgress(const std::function<bool(uint64_t, uint64_t)>& tell) : m_tell(tell) {}
		void SetTitle(const char*) override {}
		void SetProgressRange(u32 range) override
		{
			BaseProgressCallback::SetProgressRange(range);
			Tell();
		}
		void SetProgressValue(u32 value) override
		{
			BaseProgressCallback::SetProgressValue(value);
			Tell();
		}
		void DisplayError(const char*) override {}
		void DisplayWarning(const char*) override {}
		void DisplayInformation(const char*) override {}
		void DisplayDebugMessage(const char*) override {}
		void ModalError(const char*) override {}
		bool ModalConfirmation(const char*) override { return false; }
		void ModalInformation(const char*) override {}

	private:
		void Tell()
		{
			if (m_tell && !m_tell(m_progress_value, m_progress_range > 1 ? m_progress_range : 0))
				m_cancelled = true;
		}
		std::function<bool(uint64_t, uint64_t)> m_tell;
	};

	std::string UserAgent()
	{
		const char* version = ps5px_build_version();
		return std::string("Crux2/") + ((version && *version) ? version : "dev") + " (PlayStation 5 homebrew)";
	}

	int HttpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
	{
		std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(UserAgent());
		if (!http)
			return -1;
		http->SetTimeout(static_cast<float>(seconds));
		int status = -1;
		http->CreateRequest(url, [&status, &out](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
			status = code;
			out = std::move(data);
		});
		http->WaitForAllRequests();
		return status;
	}

	int HttpDownload(const std::string& url, const std::string& path, uint64_t limit, const std::function<bool(uint64_t, uint64_t)>& progress)
	{
		std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(UserAgent());
		if (!http)
			return -1;
		http->SetTimeout(600.0f);
		UpdateProgress callback(progress);
		callback.SetCancellable(true);
		int status = -1;
		std::vector<u8> body;
		http->CreateRequest(url, [&status, &body](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
			status = code;
			body = std::move(data);
		}, &callback);
		http->WaitForAllRequests();
		if (callback.IsCancelled())
			return -1;
		if (status != 200)
			return status;
		if (limit != 0 && body.size() > limit)
			return -1;
		// (The body is held in memory by PCSX2's downloader: written out whole.)
		if (!PS5Platform::WriteFileSafely(path.c_str(), body.data(), body.size()))
			return -1;
		if (progress)
			progress(body.size(), body.size());
		return status;
	}
} // namespace

void PS5Services::StartUpdater()
{
	PS5Update::Setup setup;
	setup.app_dir = ps5px_title_dir();
	setup.version = ps5px_build_version();
	setup.repository = Host::GetBaseStringSettingValue("PS5", "UpdateRepository", "Press5elect/Crux2");
	setup.http_get = HttpGet;
	setup.http_download = HttpDownload;
	setup.log = [](const char* line) { ps5px_log("%s", line); };
	PS5Update::Init(setup);
	// Settings > PlayStation 5 > "Look for a newer Crux2 at start-up": the releases are asked once
	// now; a newer one is offered in a dialog, and nothing is said when this is the newest or the
	// network does not answer.
	if (Host::GetBaseBoolSettingValue("PS5", "UpdateAtStart", true))
		PS5Update::Check();
}
