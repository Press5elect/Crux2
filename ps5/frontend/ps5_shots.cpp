// Crux2 - its own screenshots (ps5_shots.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise src/porpoise_shots.cpp (GPL-3.0-or-later): the picture copied now, written on a
// thread of its own, one shot at a time.

#include "ps5_shots.h"

#include "ps5_online.h"
#include "ps5_platform.h"
#include "ps5_priority.h"
#include "ps5_ui.h"

#include "Config.h"
#include "GS/GS.h"
#include "MTGS.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <atomic>
#include <thread>

namespace PS5Shots
{
namespace
{
std::atomic_bool s_busy{false};

// Written beside its place, then put there: a picture is whole or not there.
bool Save(const RGBA8Image& image, const std::string& path)
{
	const std::string part = path.substr(0, path.size() - 4) + ".part.png";
	if (!image.SaveToFile(part.c_str()))
	{
		FileSystem::DeleteFilePath(part.c_str());
		return false;
	}
	if (!FileSystem::RenamePath(part.c_str(), path.c_str()))
	{
		FileSystem::DeleteFilePath(part.c_str());
		return false;
	}
	PS5Platform::MakePublic(path.c_str(), false);
	return true;
}

void Write(std::string serial, uint32_t width, uint32_t height, std::vector<uint32_t> pixels)
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	const std::string folder = Path::Combine(EmuFolders::Snapshots, FolderName(serial));
	if (!FileSystem::DirectoryExists(folder.c_str()) && FileSystem::CreateDirectoryPath(folder.c_str(), true))
		PS5Platform::MakePublic(folder.c_str(), true);
	const time_t now = time(nullptr);
	std::string path;
	for (int nth = 1; nth < 100; nth++)
	{
		path = Path::Combine(folder, Name(now, nth) + ".png");
		if (!FileSystem::FileExists(path.c_str()))
			break;
	}

	// The game's picture of play, when it has none yet: a copy at the size of the downloaded ones.
	std::string play;
	if (!serial.empty())
	{
		const std::string target = PS5Covers::PicturePath(serial, 1);
		if (!FileSystem::FileExists(target.c_str()))
		{
			uint32_t w = 0, h = 0;
			std::vector<uint32_t> small = Smaller(pixels, width, height, 640, &w, &h);
			const std::string pictures = PS5Covers::PicturesFolder();
			if (!FileSystem::DirectoryExists(pictures.c_str()) && FileSystem::CreateDirectoryPath(pictures.c_str(), true))
				PS5Platform::MakePublic(pictures.c_str(), true);
			RGBA8Image image;
			image.SetPixels(w, h, std::move(small));
			if (Save(image, target))
				play = target;
		}
	}

	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));
	const bool ok = Save(image, path);
	ps5px_log("screenshot: %ux%u %s %s%s%s", width, height, ok ? "written to" : "could NOT be written to", path.c_str(),
		play.empty() ? "" : "; it is the game's picture of play too: ", play.c_str());
	if (ok)
		PS5UI::Toast(play.empty() ? fmt::format("Screenshot saved: snaps/{}/{}", FolderName(serial), Path::GetFileName(path)) :
		                            fmt::format("Screenshot saved: snaps/{}/{}. It is the game's picture of play too.", FolderName(serial),
										Path::GetFileName(path)));
	else
		PS5UI::Toast("The screenshot could not be saved.");
	s_busy.store(false, std::memory_order_release);
}
} // namespace

void Take()
{
	if (!VMManager::HasValidVM())
		return;
	if (s_busy.exchange(true, std::memory_order_acq_rel))
		return; // (the one before is still being written)
	std::string serial = VMManager::GetDiscSerial();
	MTGS::RunOnGSThread([serial = std::move(serial)]() mutable {
		uint32_t width = 0, height = 0;
		std::vector<uint32_t> pixels;
		// The display as the game shows it (internal resolution, its shape on the screen, the black
		// borders cut away): no menu and no notice is in it.
		if (!GSSaveSnapshotToMemory(0, 0, true, true, &width, &height, &pixels) || width < 16 || height < 16)
		{
			ps5px_log("screenshot: the game's picture could not be read");
			PS5UI::Toast("There is no picture to take yet.");
			s_busy.store(false, std::memory_order_release);
			return;
		}
		std::thread(Write, std::move(serial), width, height, std::move(pixels)).detach();
	});
}
} // namespace PS5Shots
