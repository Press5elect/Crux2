// Crux2 - the interface's Backend over PCSX2, and PCSX2's FullscreenUI functions.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Two things in one file, because they are two sides of the same door:
//
//   * PCSX2Backend: what the screens (bp_*.cpp) ask of the emulator. The games are PCSX2's game
//     list, the covers and pictures come from its texture cache, the settings are its settings
//     layers (all games: the base layer; one game: that game's .ini in gamesettings/), a game is
//     started, paused, saved and closed through VMManager on the emulation thread.
//
//   * namespace FullscreenUI: the functions the emulator calls of its Big Picture interface
//     (ImGui/FullscreenUI.h). PCSX2's own implementation (ImGui/FullscreenUI.cpp and
//     FullscreenUI_Settings.cpp) is not built for the console; these take its place and hand
//     each event to the screens.
//
// Kept of PCSX2's interface code: ImGui/ImGuiFullscreen.cpp, for its texture cache (covers load
// on its thread) and for the notifications other parts of the emulator post through it.
//
// Threads: the screens and everything in the Backend run on the graphics thread. What changes
// the machine goes to the emulation thread (Host::RunOnCPUThread); what comes back from it is
// queued to the graphics thread (MTGS::RunOnGSThread).

#include "bp.h"
#include "../ps5_priority.h"

#include "../ps5_platform.h"
#include "../ps5_pgs.h"
#include "../ps5_cards.h"
#include "../ps5_boot_step.h"
#include "../ps5_cheats_logic.h"
#include "../ps5_disc.h"
#include "../ps5_disc_image.h"
#include "../ps5_about.h"
#include "../vfs/ps5_vfs.h"
#include "../ps5_extras.h"
#include "../ps5_input_source.h"
#include "../ps5_jobs.h"
#include "../ps5_net.h"
#include "../ps5_online.h"
#include "../ps5_presets.h"
#include "../ps5_mic.h"
#include "../ps5_texture_packs.h"
#include "../ps5_texture_packs_logic.h"
#include "../ps5_ui.h"
#include "../ps5_ui_audio.h"
#include "../ps5_keyboard.h"
#include "../ps5_slang.h"
#include "../ps5_update.h"
#include "../ps5_web.h"

#include "Achievements.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GameDatabase.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ImGuiOverlays.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "Patch.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "USB/USB.h"
#include "SIO/Sio.h"
#include "SaveState.h"
#include "VMManager.h"
#include "ps2/BiosTools.h"
#include "svnrev.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// The host (ps5_host.cpp).
extern "C" void ps5px_download_covers_now(void);
extern "C" void ps5px_download_pictures_now(void);
extern "C" int ps5px_arm_profiled_start(int threaded_recording);
// The interface's faces, as the emulator's font atlas has them (ImGui/ImGuiManager.cpp): 0 the
// text face, 1 the bold one, 2 the one the title's name is lettered in.
ImFont* ps5px_interface_font(int which);

namespace
{

// ------------------------------------------------- what other threads tell the interface

std::mutex s_told_mutex;
std::string s_load_text;
float s_load_progress = -1.f;
std::string s_background_note;
// The game the machine holds, as the emulation thread last named it (FullscreenUI::GameChanged).
std::string s_game_title, s_game_serial, s_game_path;
u32 s_game_crc = 0;
std::atomic_bool s_scanning{false};
std::atomic_int s_controllers{0};
std::atomic_bool s_cheats_changed{false};
std::atomic_bool s_pages_changed{false};
std::atomic_bool s_states_changed{false};
std::atomic_bool s_pictures_changed{false};
// Lines and messages other threads have for the user, shown on the next frame (s_told_mutex).
std::vector<std::string> s_told_toasts;
std::vector<std::pair<std::string, std::string>> s_told_messages;
// Bumped by whoever knows the game list is another one now.
std::atomic_uint s_games_generation{1};
// The settings of every game were written to: they are to be saved, and (apply) the emulator
// is to take them. Set by the graphics thread and by the emulation thread (a button was bound);
// whoever sets both sets apply first.
std::atomic_bool s_shared_changed{false};
std::atomic_bool s_shared_apply{false};
// A start was asked for and the emulation thread has not said how it went. (A flag of its own,
// not an event queued to the graphics thread: the emulator empties that queue when it resets the
// machine, and makes the graphics device anew when a game's settings ask for another renderer.)
std::atomic_bool s_boot_pending{false};
std::atomic_bool s_boot_from_network{false}; // the game being started is on the network share
// When the game that is loaded was started: a state kept before a load ("Undo the last load") is
// this game's only when it is newer.
std::atomic<time_t> s_session_started{0};
// PCSX2's own list of achievements (1) or leaderboards (2) is up over the paused game; 0 for
// neither. Opened and drawn by the graphics thread; the emulation thread closes it when the
// game's achievements go (Achievements::ClearUIState).
std::atomic_int s_overlay{0};
// A sign-in at RetroAchievements is on its way (the emulation thread waits for the answer).
std::atomic_bool s_signing_in{false};

// --------------------------------------------------------------- waiting for a button

// Written by the graphics thread (listen, cancel) and by the emulation thread (the hook), both
// under the settings lock, as PCSX2's own interface did it.
struct Listen
{
	InputBindingInfo::Type type = InputBindingInfo::Type::Unknown;
	std::string section, key;
	std::vector<InputBindingKey> keys;
	std::vector<std::pair<InputBindingKey, std::pair<float, float>>> ranges;
	Common::Timer timer;
};
Listen s_listen;
std::atomic_bool s_listening{false};
constexpr double LISTEN_SECONDS = 5.0;

void StopListening()
{
	InputManager::RemoveHook();
	const auto lock = Host::GetSettingsLock();
	s_listen.type = InputBindingInfo::Type::Unknown;
	s_listen.keys.clear();
	s_listen.ranges.clear();
	s_listening.store(false, std::memory_order_release);
}

// ------------------------------------------------------------------------- small helpers

bp::Texture ToTexture(GSTexture* texture)
{
	if (texture == nullptr || texture == ImGuiFullscreen::GetPlaceholderTexture().get())
		return {};
	bp::Texture out;
	out.id = reinterpret_cast<ImTextureID>(texture->GetNativeHandle());
	out.width = static_cast<float>(texture->GetWidth());
	out.height = static_cast<float>(texture->GetHeight());
	return out;
}

// A folder of the title, as an FTP client reaches it. Inside the sandbox the title sees its own
// folder as /app0; everyone else sees it under /data/homebrew.
std::string Shown(std::string path)
{
	static constexpr std::string_view sandbox = "/app0";
	if (path.compare(0, sandbox.size(), sandbox) == 0)
		path = "/data/homebrew/PPSA99249" + path.substr(sandbox.size());
	if (path.empty() || path.back() != '/')
		path += '/';
	return path;
}

// How many values a game's settings file holds.
int CountValues(const std::string& path)
{
	const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	if (!text.has_value())
		return 0;
	int count = 0;
	for (const std::string_view line : StringUtil::SplitString(*text, '\n', true))
	{
		const std::string_view trimmed = StringUtil::StripWhitespace(line);
		if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[')
			continue;
		if (trimmed.find('=') != std::string_view::npos)
			count++;
	}
	return count;
}

bp::Option Heading(std::string label)
{
	bp::Option o;
	o.kind = bp::Option::Heading;
	o.label = std::move(label);
	return o;
}

bp::Option Line(std::string label, std::string value)
{
	bp::Option o;
	o.kind = bp::Option::Info;
	o.label = std::move(label);
	o.info = [value = std::move(value)] { return value; };
	return o;
}

bp::Option Action(std::string label, std::string desc, std::function<void()> action, unsigned flags = 0)
{
	bp::Option o;
	o.kind = bp::Option::Action;
	o.label = std::move(label);
	o.desc = std::move(desc);
	o.action = std::move(action);
	o.flags = flags;
	return o;
}

bp::Option Toggle(std::string label, std::string desc, const char* section, std::string key, bool def, unsigned flags = 0)
{
	bp::Option o;
	o.kind = bp::Option::Bool;
	o.label = std::move(label);
	o.desc = std::move(desc);
	o.section = section;
	o.key = std::move(key);
	o.defBool = def;
	o.flags = flags;
	return o;
}

bp::Option Range(std::string label, std::string desc, const char* section, std::string key, int def, int lo, int hi, int step,
	std::string unit, unsigned flags = 0)
{
	bp::Option o;
	o.kind = bp::Option::IntRange;
	o.label = std::move(label);
	o.desc = std::move(desc);
	o.section = section;
	o.key = std::move(key);
	o.defInt = def;
	o.lo = lo;
	o.hi = hi;
	o.step = step;
	o.unit = std::move(unit);
	o.flags = flags;
	return o;
}

// One of a controller type's own settings (PCSX2 describes them), as a row.
bool SettingRow(const char* section, const SettingInfo& si, bp::Option* out)
{
	bp::Option o;
	o.label = si.display_name != nullptr ? si.display_name : si.name;
	o.desc = si.description != nullptr ? si.description : "";
	o.section = section;
	o.key = si.name;
	o.flags = bp::Option::AllGames;
	switch (si.type)
	{
		case SettingInfo::Type::Boolean:
			o.kind = bp::Option::Bool;
			o.defBool = si.BooleanDefaultValue();
			break;
		case SettingInfo::Type::Integer:
			o.kind = bp::Option::IntRange;
			o.defInt = si.IntegerDefaultValue();
			o.lo = si.IntegerMinValue();
			o.hi = si.IntegerMaxValue();
			o.step = std::max(1, si.IntegerStepValue());
			break;
		case SettingInfo::Type::IntegerList:
			o.kind = bp::Option::IntList;
			o.defInt = si.IntegerDefaultValue();
			for (int i = 0; si.options != nullptr && si.options[i] != nullptr; i++)
			{
				o.names.emplace_back(si.options[i]);
				o.ints.push_back(si.IntegerMinValue() + i);
			}
			break;
		case SettingInfo::Type::Float:
		{
			o.kind = bp::Option::FloatRange;
			o.defFloat = si.FloatDefaultValue();
			o.lo = si.FloatMinValue();
			o.hi = si.FloatMaxValue();
			o.step = (si.FloatStepValue() > 0.f) ? si.FloatStepValue() : 0.01f;
			o.scale = (si.multiplier != 0.f) ? si.multiplier : 1.f;
			// "%.0f%%": how many decimals, and whether it is a percentage.
			const std::string_view format = si.format != nullptr ? si.format : "";
			o.decimals = 2;
			if (const size_t dot = format.find('.'); dot != std::string_view::npos && dot + 1 < format.size() && format[dot + 1] >= '0' &&
													 format[dot + 1] <= '9')
				o.decimals = format[dot + 1] - '0';
			if (format.find("%%") != std::string_view::npos)
				o.unit = "%";
			break;
		}
		case SettingInfo::Type::StringList:
			o.kind = bp::Option::StringList;
			o.defString = si.StringDefaultValue() != nullptr ? si.StringDefaultValue() : "";
			if (si.get_options != nullptr)
			{
				for (auto& [value, name] : si.get_options())
				{
					o.strings.push_back(std::move(value));
					o.names.push_back(std::move(name));
				}
			}
			else
			{
				for (int i = 0; si.options != nullptr && si.options[i] != nullptr; i++)
				{
					o.strings.emplace_back(si.options[i]);
					o.names.emplace_back(si.options[i]);
				}
			}
			break;
		default:
			// A path or free text: nothing a controller can enter.
			return false;
	}
	*out = std::move(o);
	return true;
}

// ---------------------------------------------------- pictures of one's own, behind the screens
//
// The pictures in the folder "backgrounds" of the title folder (PNG, JPEG or WebP), by name. One is
// read and made smaller on a thread of its own (a photograph of 4K is thirty megabytes as a
// texture: none is kept larger than 1920 by 1080), and two are held at a time: the one that shows
// and the one that follows it.
class Wallpapers
{
public:
	int Count(bool again)
	{
		if (!m_listed || again)
			List();
		return static_cast<int>(m_paths.size());
	}

	GSTexture* Get(int index)
	{
		if (index < 0 || index >= static_cast<int>(m_paths.size()) || g_gs_device == nullptr)
			return nullptr;
		m_clock++;
		for (Slot& slot : m_slots)
		{
			if (slot.index == index && slot.texture)
			{
				slot.used = m_clock;
				return slot.texture.get();
			}
		}
		if (m_bad.count(index) != 0)
			return nullptr;
		if (m_job)
		{
			if (!m_job->done.load(std::memory_order_acquire))
				return nullptr;
			// Read: into the place of the one used the longest ago.
			const std::shared_ptr<Job> job = std::move(m_job);
			m_job.reset();
			const int read = m_job_index;
			m_job_index = -1;
			Slot& slot = (m_slots[0].used <= m_slots[1].used) ? m_slots[0] : m_slots[1];
			bool made = false;
			if (job->ok && job->list == m_list)
			{
				std::unique_ptr<GSTexture> texture(g_gs_device->CreateTexture(job->image.GetWidth(), job->image.GetHeight(), 1, GSTexture::Format::Color));
				if (texture && texture->Update(GSVector4i(0, 0, job->image.GetWidth(), job->image.GetHeight()), job->image.GetPixels(), job->image.GetPitch()))
				{
					if (slot.texture)
						m_retired.push_back(std::move(slot.texture));
					slot.texture = std::move(texture);
					slot.index = read;
					slot.used = m_clock;
					made = true;
				}
				else if (texture)
				{
					m_retired.push_back(std::move(texture));
				}
			}
			if (!made && job->list == m_list)
			{
				ps5px_log("backgrounds: %s could not be %s", job->path.c_str(), job->ok ? "made a texture of" : "read as a picture");
				m_bad.insert(read);
			}
			if (made && read == index)
				return slot.texture.get();
			if (m_bad.count(index) != 0)
				return nullptr;
		}
		// Asked for, and not there: read now.
		m_job = std::make_shared<Job>();
		m_job->path = m_paths[static_cast<size_t>(index)];
		m_job->list = m_list;
		m_job_index = index;
		std::shared_ptr<Job>* const handed = new std::shared_ptr<Job>(m_job);
		pthread_t thread;
		if (pthread_create(&thread, nullptr, &ReadMain, handed) != 0)
		{
			delete handed;
			m_job.reset();
			m_job_index = -1;
			m_bad.insert(index);
			return nullptr;
		}
		pthread_detach(thread);
		return nullptr;
	}

	/// The textures that were replaced, for whoever lets go of them when it is safe to.
	void Retired(std::vector<std::unique_ptr<GSTexture>>* retired)
	{
		for (std::unique_ptr<GSTexture>& texture : m_retired)
			retired->push_back(std::move(texture));
		m_retired.clear();
	}

	/// The graphics device goes: all of them.
	void Release(std::vector<std::unique_ptr<GSTexture>>* retired)
	{
		for (Slot& slot : m_slots)
		{
			if (slot.texture)
				retired->push_back(std::move(slot.texture));
			slot = Slot();
		}
		Retired(retired);
	}

private:
	struct Job
	{
		std::string path;
		unsigned list = 0;
		RGBA8Image image;
		bool ok = false;
		std::atomic_bool done{false};
	};
	struct Slot
	{
		int index = -1;
		std::unique_ptr<GSTexture> texture;
		u64 used = 0;
	};

	void List()
	{
		m_listed = true;
		m_list++;
		m_bad.clear();
		std::vector<std::string> paths;
		const std::string folder = PS5Platform::UserPath("backgrounds");
		FileSystem::FindResultsArray found;
		FileSystem::FindFiles(folder.c_str(), "*", FILESYSTEM_FIND_FILES, &found);
		for (const FILESYSTEM_FIND_DATA& file : found)
		{
			std::string extension(Path::GetExtension(file.FileName));
			for (char& c : extension)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "webp")
				paths.push_back(file.FileName);
		}
		std::sort(paths.begin(), paths.end());
		if (paths.size() > 200)
			paths.resize(200);
		if (paths != m_paths)
		{
			// Other pictures than before: what is held is of the old ones.
			m_paths = std::move(paths);
			for (Slot& slot : m_slots)
			{
				if (slot.texture)
					m_retired.push_back(std::move(slot.texture));
				slot = Slot();
			}
		}
		ps5px_log("backgrounds: %zu picture(s) in %s", m_paths.size(), folder.c_str());
	}

	static void* ReadMain(void* argument)
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
		std::shared_ptr<Job>* const handed = static_cast<std::shared_ptr<Job>*>(argument);
		const std::shared_ptr<Job> job = *handed;
		delete handed;
		RGBA8Image image;
		if (image.LoadFromFile(job->path.c_str()) && image.GetWidth() > 0 && image.GetHeight() > 0)
		{
			// Smaller by a whole number, each pixel the mean of those it stands for.
			const u32 by = std::max((image.GetWidth() + 1919) / 1920, (image.GetHeight() + 1079) / 1080);
			if (by > 1)
			{
				const u32 width = image.GetWidth() / by, height = image.GetHeight() / by;
				std::vector<u32> small(static_cast<size_t>(width) * height);
				const u32* const from = image.GetPixels();
				const u32 pitch = image.GetWidth();
				for (u32 y = 0; y < height; y++)
				{
					for (u32 x = 0; x < width; x++)
					{
						u32 sum[4] = {};
						for (u32 dy = 0; dy < by; dy++)
						{
							const u32* row = from + static_cast<size_t>(y * by + dy) * pitch + x * by;
							for (u32 dx = 0; dx < by; dx++)
							{
								const u32 pixel = row[dx];
								sum[0] += pixel & 0xff;
								sum[1] += (pixel >> 8) & 0xff;
								sum[2] += (pixel >> 16) & 0xff;
								sum[3] += pixel >> 24;
							}
						}
						const u32 n = by * by;
						small[static_cast<size_t>(y) * width + x] = (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16) | ((sum[3] / n) << 24);
					}
				}
				image = RGBA8Image(width, height, std::move(small));
			}
			job->image = std::move(image);
			job->ok = true;
		}
		job->done.store(true, std::memory_order_release);
		return nullptr;
	}

	std::vector<std::string> m_paths;
	bool m_listed = false;
	unsigned m_list = 0; // counts the listings: a picture read for an older one is not this one's
	Slot m_slots[2];
	std::shared_ptr<Job> m_job;
	int m_job_index = -1;
	u64 m_clock = 0;
	std::set<int> m_bad;
	std::vector<std::unique_ptr<GSTexture>> m_retired;
};

// ---------------------------------------------------------------------------- the backend

// The console's additions (extraRows and the rest): an image of the disc being made; what the
// phone typed, for the interface's text field; an update asked for.
std::atomic<bool> s_disc_copying{false};
std::mutex s_web_typed_mutex;
std::string s_web_typed;
unsigned s_web_keys = 0;
std::atomic<bool> s_update_wanted{false};
std::atomic<bool> s_update_asked_by_hand{false};

class PCSX2Backend final : public bp::Backend, public bp::CardShelf
{
public:
	// ---- the library
	unsigned gamesGeneration() override { return s_games_generation.load(std::memory_order_acquire); }

	std::vector<bp::GameInfo> games() override
	{
		std::vector<bp::GameInfo> out;
		const bool english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		// The games not to be listed, by their path ([PS5] HiddenGames).
		std::vector<std::string> hidden;
		{
			const auto lock = Host::GetSettingsLock();
			hidden = Host::Internal::GetBaseSettingsLayer()->GetStringList("PS5", "HiddenGames");
		}
		std::sort(hidden.begin(), hidden.end());
		// The games that were given a title here ([PS5] RenamedGames, by their path).
		std::vector<std::string> renamed;
		{
			const auto lock = Host::GetSettingsLock();
			renamed = Host::Internal::GetBaseSettingsLayer()->GetStringList("PS5", "RenamedGames");
		}
		std::sort(renamed.begin(), renamed.end());
		// What there is for which game, by serial: one look into each folder.
		std::unordered_set<std::string> with_states, with_cheats, with_textures;
		{
			const auto serials = [](const std::string& folder, char end, std::unordered_set<std::string>* out) {
				FileSystem::FindResultsArray found;
				FileSystem::FindFiles(folder.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &found);
				for (const FILESYSTEM_FIND_DATA& file : found)
				{
					const size_t at = file.FileName.find(end);
					if (at != std::string::npos && at > 0)
						out->insert(file.FileName.substr(0, at));
				}
			};
			serials(EmuFolders::Savestates, ' ', &with_states); // "<serial> (<CRC>).<slot>.p2s"
			serials(EmuFolders::Cheats, '_', &with_cheats);     // "<serial>_<CRC>.pnach"
			FileSystem::FindResultsArray packs;
			FileSystem::FindFiles(EmuFolders::Textures.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS, &packs);
			for (const FILESYSTEM_FIND_DATA& pack : packs)
			{
				if (FileSystem::DirectoryExists(Path::Combine(Path::Combine(EmuFolders::Textures, pack.FileName), "replacements").c_str()))
					with_textures.insert(pack.FileName);
			}
		}
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		out.reserve(count);
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry == nullptr || entry->type == GameList::EntryType::Invalid || entry->type == GameList::EntryType::Count)
				continue;
			out.push_back(Describe(*entry, english));
			bp::GameInfo& game = out.back();
			game.hidden = std::binary_search(hidden.begin(), hidden.end(), entry->path);
			game.renamed = std::binary_search(renamed.begin(), renamed.end(), entry->path);
			// (A title given here is the title, also where the English one is preferred.)
			if (game.renamed)
				game.title = entry->title;
			if (!game.serial.empty())
			{
				game.hasStates = with_states.count(game.serial) != 0;
				game.hasCheats = with_cheats.count(game.serial) != 0;
				game.hasTextures = with_textures.count(game.serial) != 0;
			}
		}
		m_games_signature = Signature(&m_games_count);
		return out;
	}

	// A favourite is PCSX2's own mark (its game list keeps it, with the custom titles); a game not
	// to be listed is the port's, kept with the settings of every game.
	void setFavourite(const bp::GameInfo& game, bool favourite) override
	{
		if (game.path.empty() || game.source == 3)
			return;
		GameList::SaveFavoriteForPath(game.path, favourite);
		s_games_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	void setHidden(const bp::GameInfo& game, bool hidden) override
	{
		if (game.path.empty() || game.source == 3)
			return;
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface* const si = Host::Internal::GetBaseSettingsLayer();
			if (hidden)
				si->AddToStringList("PS5", "HiddenGames", game.path.c_str());
			else
				si->RemoveFromStringList("PS5", "HiddenGames", game.path.c_str());
		}
		s_shared_changed.store(true, std::memory_order_release);
		s_games_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	// Another title is PCSX2's own custom title (its game list keeps it and reads the game's file
	// again for it: on the emulation thread, which has nothing else to do with no game running).
	// Which games have one is kept with the settings too, to know them without asking PCSX2 game
	// by game.
	void setTitle(const bp::GameInfo& game, const std::string& title) override
	{
		if (game.path.empty() || game.source == 3)
			return;
		if (VMManager::GetState() != VMState::Shutdown)
		{
			bp::toast("A game is given another title when none is running.");
			return;
		}
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface* const si = Host::Internal::GetBaseSettingsLayer();
			if (!title.empty())
				si->AddToStringList("PS5", "RenamedGames", game.path.c_str());
			else
				si->RemoveFromStringList("PS5", "RenamedGames", game.path.c_str());
		}
		s_shared_changed.store(true, std::memory_order_release);
		Host::RunOnCPUThread([path = game.path, title]() {
			GameList::SaveCustomTitleForPath(path, title);
			s_games_generation.fetch_add(1, std::memory_order_acq_rel);
		});
	}

	// A game's title screen (0) and its picture of play (1): files in pictures/ of the title
	// folder, by its serial. Which games have which is found by one look into the folder.
	bp::Texture picture(const bp::GameInfo& game, int which) override
	{
		if (game.serial.empty() || which < 0 || which > 1)
			return {};
		if (!m_pictures_listed)
		{
			m_pictures_listed = true;
			m_pictures.clear();
			FileSystem::FindResultsArray found;
			FileSystem::FindFiles(PS5Covers::PicturesFolder().c_str(), "*.png", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &found);
			for (const FILESYSTEM_FIND_DATA& file : found)
			{
				const std::string_view name = file.FileName;
				if (name.size() > 10 && name.substr(name.size() - 10) == ".title.png")
					m_pictures[std::string(name.substr(0, name.size() - 10))] |= 1;
				else if (name.size() > 9 && name.substr(name.size() - 9) == ".snap.png")
					m_pictures[std::string(name.substr(0, name.size() - 9))] |= 2;
			}
		}
		const auto it = m_pictures.find(game.serial);
		if (it == m_pictures.end() || (it->second & (1 << which)) == 0)
			return {};
		return ToTexture(ImGuiFullscreen::GetCachedTextureAsync(PS5Covers::PicturePath(game.serial, which)));
	}

	void fetchPictures(const bp::GameInfo* game, bool again) override
	{
		if (game != nullptr)
		{
			bp::toast("Looking for this game's pictures...", 3.f);
			m_pictures_asked = game->serial;
			PS5Jobs::FetchPictures(game->serial);
			return;
		}
		if (VMManager::GetState() != VMState::Shutdown)
		{
			bp::toast("The games' pictures are fetched when no game is running.");
			return;
		}
		bp::toast("Looking for the games' pictures...", 3.f);
		ps5px_download_pictures_now();
	}

	int wallpapers(bool again) override { return m_wallpapers.Count(again); }
	bp::Texture wallpaper(int index) override { return ToTexture(m_wallpapers.Get(index)); }

	// Where the game's picture is on the screen: from what the renderer says of two points of the
	// screen's middle (it tells where in the picture a point of the window is).
	bool gameRect(float rect[4]) override
	{
		if (!VMManager::HasValidVM())
			return false;
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		const float cx = size.x / 2, cy = size.y / 2, step = 64;
		float u0, v0, u1, v1;
		GSTranslateWindowToDisplayCoordinates(cx, cy, &u0, &v0);
		GSTranslateWindowToDisplayCoordinates(cx + step, cy + step, &u1, &v1);
		if (u0 < 0 || v0 < 0 || u1 <= u0 || v1 <= v0)
			return false;
		const float width = step / (u1 - u0), height = step / (v1 - v0);
		rect[0] = std::round(cx - u0 * width);
		rect[1] = std::round(cy - v0 * height);
		rect[2] = std::round(rect[0] + width);
		rect[3] = std::round(rect[1] + height);
		return true;
	}

	// The emulator's figures: its renderer's own setting of where they are, changed for as long
	// as the game runs (BeginFrame puts it there again when the settings were applied anew).
	void overlayCorner(int corner) override { m_osd_corner = corner; }

	bp::Texture cover(const bp::GameInfo& game) override
	{
		auto it = m_cover_paths.find(game.path);
		if (it == m_cover_paths.end() && game.source == 3)
		{
			// The disc in the drive is in no list: its cover is the one a game of its serial has.
			std::string path;
			for (const char* const extension : {".png", ".jpg", ".jpeg", ".webp"})
			{
				const std::string candidate = Path::Combine(EmuFolders::Covers, game.serial + extension);
				if (!game.serial.empty() && FileSystem::FileExists(candidate.c_str()))
				{
					path = candidate;
					break;
				}
			}
			it = m_cover_paths.emplace(game.path, std::move(path)).first;
		}
		if (it == m_cover_paths.end())
		{
			// Looked for once: a game without a cover is not asked about on every frame.
			std::string path;
			{
				const auto lock = GameList::GetLock();
				const GameList::Entry* const entry = GameList::GetEntryForPath(game.path.c_str());
				// (Not in the list this moment: a scan is making it again. Asked again later.)
				if (entry == nullptr)
					return {};
				path = GameList::GetCoverImagePathForEntry(entry);
			}
			it = m_cover_paths.emplace(game.path, std::move(path)).first;
		}
		if (it->second.empty())
			return {};
		return ToTexture(ImGuiFullscreen::GetCachedTextureAsync(it->second));
	}

	void scan(bool forget) override
	{
		// Covers copied to the title since the last look are found too.
		m_cover_paths.clear();
		// (A scan reads disc images with the emulator's own disc reader: not while a game has it.)
		if (VMManager::GetState() != VMState::Shutdown)
			return;
		Host::RefreshGameListAsync(forget);
	}

	bool scanning() override { return s_scanning.load(std::memory_order_acquire); }

	std::string backgroundNote() override
	{
		const std::lock_guard lock(s_told_mutex);
		return s_background_note;
	}

	std::string shownFolder(const char* which) override
	{
		const std::string_view name = which;
		if (name == "games")
			return Shown(Path::Combine(ps5px_title_dir(), "games"));
		if (name == "bios")
			return Shown(EmuFolders::Bios);
		if (name == "covers")
			return Shown(EmuFolders::Covers);
		return Shown(ps5px_title_dir());
	}

	bool usbWanted() override { return PS5Platform::WantsUSB(); }
	bool usbAvailable() override { return PS5Platform::IsElevated(); }

	std::vector<std::string> networkFolders() override
	{
		std::vector<std::string> out;
		const auto lock = Host::GetSettingsLock();
		for (std::string& path : Host::Internal::GetBaseSettingsLayer()->GetStringList("PS5", "AutoGamePaths"))
		{
			if (path.compare(0, 5, "/smb/") == 0)
				out.push_back(std::move(path));
		}
		return out;
	}

	// ---- starting and the running game
	// The disc drives (the console's own and USB DVD drives), as their watcher (ps5_disc.cpp) last
	// saw them: made into what the screens want only when the watcher says something changed.
	bp::DiscState disc() override
	{
		const unsigned generation = PS5Disc::Generation();
		// (A game started from a disc tells its CRC, which the disc alone does not: with it the
		// disc's game has options of its own like any other.)
		u32 crc = 0;
		std::string serial;
		{
			const std::lock_guard lock(s_told_mutex);
			crc = s_game_crc;
			serial = s_game_serial;
		}
		if (crc != 0 && !serial.empty())
		{
			bool changed = false;
			for (bp::GameInfo& game : m_disc.games)
			{
				if (game.serial == serial && game.crc != crc)
				{
					m_disc_crcs[serial] = crc;
					game.crc = crc;
					changed = true;
				}
			}
			if (changed)
			{
				m_disc.game = m_disc.games.front();
				m_disc.generation++;
			}
		}
		if (generation == m_disc_seen)
			return m_disc;
		m_disc_seen = generation;

		bp::DiscState out;
		out.generation = m_disc.generation + 1;
		for (const PS5Disc::State& state : PS5Disc::Drives())
		{
			out.driveFound = out.driveFound || state.drive_found;
			out.present = out.present || state.present;
			// (Another disc, another cover.)
			m_cover_paths.erase(state.device);
			if (!(state.present && state.ps2))
			{
				if (state.present && out.note.empty())
					out.note = state.note;
				continue;
			}
			bp::GameInfo game;
			game.path = state.device;
			game.fileName = state.device;
			game.source = 3;
			game.cd = state.cd;
			game.serial = state.serial;
			game.kind = bp::GameInfo::PS2;
			game.size = state.sectors * 2048ull;
			game.title = !state.volume_label.empty() ? state.volume_label : state.serial;
			if (const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(state.serial))
			{
				const bool english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
				game.title = (english && !entry->name_en.empty()) ? entry->name_en : entry->name;
				game.region = entry->region;
				game.compatibility = std::clamp(static_cast<int>(entry->compat), 0, 6);
			}
			if (const auto known = m_disc_crcs.find(state.serial); known != m_disc_crcs.end())
				game.crc = known->second;
			out.games.push_back(std::move(game));
		}
		out.playable = !out.games.empty();
		if (out.playable)
			out.game = out.games.front();
		m_disc = std::move(out);
		return m_disc;
	}

	void start(const bp::GameInfo& game, int state, int boot) override
	{
		VMBootParameters params;
		params.filename = game.path;
		if (game.source == 3)
		{
			// From a disc drive: the watcher gets out of that drive first, and stays out until the
			// game is gone (Host::OnVMDestroyed gives it back). The other drives are still watched.
			params.source_type = CDVD_SourceType::Disc;
			PS5Disc::SetInUse(true, game.path);
			m_disc_lent = true;
			ps5px_log("game: starting from the disc in %s", game.path.c_str());
		}
		if (state != INT_MIN)
			params.state_index = state;
		if (boot == 1)
			params.fast_boot = true;
		else if (boot == 2)
			params.fast_boot = false;
		Boot(std::move(params));
	}

	void startStateFile(const bp::GameInfo& game, const std::string& statePath) override
	{
		VMBootParameters params;
		params.filename = game.path;
		params.save_state = statePath;
		Boot(std::move(params));
	}

	void startBios() override { Boot(VMBootParameters()); }

	bp::Running running() override
	{
		bp::Running r;
		const VMState state = VMManager::GetState();
		r.valid = (state == VMState::Running || state == VMState::Paused || state == VMState::Resetting);
		r.stopping = (state == VMState::Stopping);
		if (!r.valid)
			return r;
		r.paused = (state == VMState::Paused);
		{
			const std::lock_guard lock(s_told_mutex);
			r.title = s_game_title;
			r.serial = s_game_serial;
			r.path = s_game_path;
			r.crc = s_game_crc;
		}
		r.canSaveState = !r.serial.empty() && r.crc != 0;
		r.frameLimit = (VMManager::GetLimiterMode() != LimiterModeType::Unlimited);
		r.hardware = GSIsHardwareRenderer();
		r.sessionSeconds = static_cast<time_t>(VMManager::GetSessionPlayedTime());
		return r;
	}

	bool starting() override
	{
		return s_boot_pending.load(std::memory_order_acquire) || VMManager::GetState() == VMState::Initializing;
	}

	bp::LoadState loadState() override
	{
		bp::LoadState out;
		const std::lock_guard lock(s_told_mutex);
		out.text = s_load_text;
		out.progress = s_load_text.empty() ? -1.f : s_load_progress;
		// A step with a bar of its own (the disc read into memory) shows how far it is instead.
		if (out.progress < 0)
		{
			double seconds = 0;
			const PS5BootStep::Step step = PS5BootStep::Current(&seconds);
			out.slow = PS5BootStep::SlowLine(step, seconds, s_boot_from_network.load(std::memory_order_relaxed));
		}
		return out;
	}

	void setPaused(bool paused) override
	{
		Host::RunOnCPUThread([paused]() {
			if (VMManager::HasValidVM())
				VMManager::SetPaused(paused);
		});
	}

	void saveState(int slot) override
	{
		m_states_key.clear();
		Host::RunOnCPUThread([slot]() {
			if (!VMManager::HasValidVM())
				return;
			VMManager::SaveStateToSlot(slot, true, [slot](const std::string& error) { FullscreenUI::ReportStateSaveError(error, slot); });
		});
	}

	void loadStateFile(const bp::StateInfo& state) override
	{
		Host::RunOnCPUThread([path = state.path, slot = state.slot, backup = state.backup]() {
			if (!VMManager::HasValidVM())
				return;
			// (A state still being packed into its file, the one saved by itself above all, is
			// whole before it is read.)
			VMManager::WaitForSaveStateFlush();
			Error error;
			if (!VMManager::LoadState(path.c_str(), &error))
				FullscreenUI::ReportStateLoadError(error.GetDescription(), slot > 0 ? std::optional<s32>(slot) : std::nullopt, backup);
		});
		m_undo_age.ResetTo(0);
	}

	void reset() override
	{
		Host::RunOnCPUThread([]() {
			if (VMManager::HasValidVM())
				VMManager::Reset();
		});
	}

	void closeGame(bool saveState) override
	{
		m_states_key.clear();
		Host::RunOnCPUThread([saveState]() { Host::RequestVMShutdown(false, saveState, saveState); });
	}

	void changeDisc(const std::string& path) override
	{
		Host::RunOnCPUThread([path]() {
			if (VMManager::HasValidVM())
				VMManager::ChangeDisc(CDVD_SourceType::Iso, path);
		});
	}


	void setFrameLimit(bool on) override
	{
		Host::RunOnCPUThread([on]() {
			if (VMManager::HasValidVM())
				VMManager::SetLimiterMode(on ? LimiterModeType::Nominal : LimiterModeType::Unlimited);
		});
	}

	void toggleSoftwareRenderer() override
	{
		Host::RunOnCPUThread([]() {
			if (VMManager::HasValidVM())
				MTGS::ToggleSoftwareRendering();
		});
	}

	void recordGsDump() override
	{
		// On the GS thread (through the CPU thread, which alone hands it work), in order with the game's GS input.
		Host::RunOnCPUThread([]() {
			if (!VMManager::HasValidVM())
				return;
			MTGS::RunOnGSThread([]() {
				// [PS5] GsDumpSeconds (1, 2 or 5; 1 by default): how long the dump records, in vsyncs at 60 a second.
				const u32 presents = 60u * static_cast<u32>(std::clamp(Host::GetBaseIntSettingValue("PS5", "GsDumpSeconds", 1), 1, 5));
				if (PS5PGS::Active())
				{
					// paraLLEl-GS draws: PCSX2's GS is not fed, its dump would be empty. The renderer records it.
					if (!PS5PGS::RecordDump(PS5PGS::DumpPath(), presents))
						PS5UI::Toast("A GS dump cannot be recorded with this renderer.");
					return;
				}
				// PCSX2's renderer: its own dump (compressed as Settings say), named its way; it says (a toast) when it
				// starts and when it is saved.
				Error error;
				if (!FileSystem::EnsureDirectoryExists(EmuFolders::Snapshots.c_str(), false, &error))
					ps5px_log("GS dump: the folder %s could not be made (%s)", EmuFolders::Snapshots.c_str(), error.GetDescription().c_str());
				PS5Platform::MakePublic(EmuFolders::Snapshots.c_str(), true);
				GSQueueSnapshot(std::string(), presents);
			});
		});
	}

	bool memoryCardBusy() override { return MemcardBusy::IsBusy(); }

	// "Undo the last load": the emulator keeps the game as it was before a state is loaded into
	// it (vendor VMManager::DoLoadState), in a file of its own. It is this game's while it is newer
	// than the game's start. (Asked on every frame of the quick menu: looked at twice a second.)
	bool canUndoLoad() override
	{
		if (m_undo_age.GetTimeSeconds() < 0.5)
			return m_undo_can;
		m_undo_age.Reset();
		m_undo_can = false;
		std::string serial;
		u32 crc = 0;
		{
			const std::lock_guard lock(s_told_mutex);
			serial = s_game_serial;
			crc = s_game_crc;
		}
		if (serial.empty() || crc == 0 || !VMManager::HasValidVM() || Achievements::IsHardcoreModeActive())
			return false;
		const std::string path = VMManager::GetSpecialSaveStateFileName(serial.c_str(), crc, "undo");
		FILESYSTEM_STAT_DATA sd;
		m_undo_can = !path.empty() && FileSystem::StatFile(path.c_str(), &sd) &&
					 sd.ModificationTime + 2 >= s_session_started.load(std::memory_order_acquire);
		return m_undo_can;
	}

	void undoLoad() override
	{
		m_undo_can = false;
		m_undo_age.Reset();
		Host::RunOnCPUThread([]() {
			if (!VMManager::HasValidVM())
				return;
			const std::string path = VMManager::GetCurrentSpecialSaveStateFileName("undo");
			if (path.empty())
				return;
			// (It may still be on its way into its file.)
			VMManager::WaitForSaveStateFlush();
			Error error;
			if (!VMManager::LoadState(path.c_str(), &error))
				FullscreenUI::ReportStateLoadError(error.GetDescription(), std::nullopt, false);
		});
	}

	// What the running game has at RetroAchievements. (Asked on every frame of the quick menu.)
	int achievementsOf() override
	{
		if (m_cheevos_age.GetTimeSeconds() < 0.5)
			return m_cheevos_has;
		m_cheevos_age.Reset();
		m_cheevos_has = 0;
		if (VMManager::HasValidVM() && Achievements::IsActive())
		{
			const auto lock = Achievements::GetLock();
			if (Achievements::HasActiveGame())
				m_cheevos_has = (Achievements::HasAchievements() ? 1 : 0) | (Achievements::HasLeaderboards() ? 2 : 0);
		}
		return m_cheevos_has;
	}

	// PCSX2's own windows for them, drawn by FullscreenUI::Render in place of the screens until
	// they close themselves (FullscreenUI::ReturnToPreviousWindow).
	void openAchievements(int which) override
	{
		if (!VMManager::HasValidVM() || !Achievements::IsActive())
			return;
		const bool boards = (which == 2);
		if (boards ? !Achievements::HasLeaderboards() : !Achievements::HasAchievements())
		{
			bp::toast(boards ? "This game has no leaderboards." : "This game has no achievements.");
			return;
		}
		if (!(boards ? Achievements::PrepareLeaderboardsWindow() : Achievements::PrepareAchievementsWindow()))
		{
			bp::toast("The list could not be made (logs/emulog.txt has the reason).");
			return;
		}
		s_overlay.store(boards ? 2 : 1, std::memory_order_release);
		ImGuiFullscreen::QueueResetFocus(ImGuiFullscreen::FocusResetType::WindowChanged);
		ImGuiFullscreen::ForceKeyNavEnabled();
	}

	bool overlayOpen() override { return s_overlay.load(std::memory_order_acquire) != 0; }

	// ---- saved states
	std::vector<bp::StateInfo> states(const std::string& serial, uint32_t crc, bool withBackups) override
	{
		// The screens ask on every frame they show a game's states; the folder is looked at once
		// a second.
		const std::string key = fmt::format("{}|{:08X}|{}", serial, crc, withBackups ? 1 : 0);
		if (key == m_states_key && m_states_age.GetTimeSeconds() < 1.0)
			return m_states;

		std::vector<bp::StateInfo> out;
		if (!serial.empty())
		{
			const auto look = [&](int slot, bool backup) {
				const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot, backup);
				FILESYSTEM_STAT_DATA sd;
				if (path.empty() || !FileSystem::StatFile(path.c_str(), &sd))
					return;
				bp::StateInfo state;
				state.slot = slot;
				state.backup = backup;
				state.when = sd.ModificationTime;
				state.path = path;
				out.push_back(std::move(state));
			};
			look(-1, false);
			// The state saved by itself while the game was played ("Save a state by itself").
			if (const std::string path = VMManager::GetSpecialSaveStateFileName(serial.c_str(), crc, "auto"); !path.empty())
			{
				FILESYSTEM_STAT_DATA sd;
				if (FileSystem::StatFile(path.c_str(), &sd))
				{
					bp::StateInfo state;
					state.slot = -2;
					state.when = sd.ModificationTime;
					state.path = path;
					out.push_back(std::move(state));
				}
			}
			for (int slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++)
			{
				look(slot, false);
				if (withBackups)
					look(slot, true);
			}
		}
		m_states_key = key;
		m_states = out;
		m_states_age.Reset();
		return out;
	}

	bp::Texture statePicture(const bp::StateInfo& state) override
	{
		if (state.path.empty() || g_gs_device == nullptr)
			return {};
		auto it = m_state_pictures.find(state.path);
		if (it != m_state_pictures.end() && it->second.when != state.when)
		{
			// Saved to again since: its picture is another one.
			if (it->second.texture)
				m_retired.push_back(std::move(it->second.texture));
			m_state_pictures.erase(it);
			it = m_state_pictures.end();
		}
		if (it == m_state_pictures.end())
		{
			StatePicture picture;
			picture.when = state.when;
			u32 width = 0, height = 0;
			std::vector<u32> pixels;
			if (SaveState_ReadScreenshot(state.path, &width, &height, &pixels) && width > 0 && height > 0)
			{
				picture.texture.reset(g_gs_device->CreateTexture(width, height, 1, GSTexture::Format::Color));
				if (picture.texture && !picture.texture->Update(GSVector4i(0, 0, width, height), pixels.data(), sizeof(u32) * width))
					m_retired.push_back(std::move(picture.texture));
			}
			it = m_state_pictures.emplace(state.path, std::move(picture)).first;
		}
		it->second.used = m_frame;
		GSTexture* const texture = it->second.texture.get();
		if (texture == nullptr)
			return {};
		bp::Texture out;
		out.id = reinterpret_cast<ImTextureID>(texture->GetNativeHandle());
		out.width = static_cast<float>(texture->GetWidth());
		out.height = static_cast<float>(texture->GetHeight());
		return out;
	}

	bool deleteState(const bp::StateInfo& state) override
	{
		m_states_key.clear();
		return !state.path.empty() && FileSystem::DeleteFilePath(state.path.c_str());
	}

	void resetPlayTime(const bp::GameInfo& game) override
	{
		if (game.serial.empty())
			return;
		GameList::ClearPlayedTimeForSerial(game.serial);
		s_games_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	// ---- settings
	bool openGame(const bp::GameInfo& game) override
	{
		// (The emulator takes a game's own settings only for a game it has a CRC of.)
		if (game.crc == 0)
			return false;
		SaveOwn();
		m_game = game;
		m_game_serial = (game.kind != bp::GameInfo::Program) ? game.serial : std::string();
		m_game_si = std::make_unique<INISettingsInterface>(VMManager::GetGameSettingsPath(m_game_serial, game.crc));
		m_game_si->Load();
		m_own_count = CountValues(m_game_si->GetFileName());
		m_own_changed = false;
		LoadPatches();
		m_pages++;
		return true;
	}

	void closeGame() override
	{
		SaveOwn();
		m_game_si.reset();
		m_patches.clear();
		m_cheats.clear();
		m_pages++;
	}

	std::optional<bool> getBool(int layer, const char* section, const char* key) override
	{
		if (IsPatchSection(section))
		{
			// A game's patches and cheats are kept as a list of the ones that are on.
			if (layer != bp::Own || !m_game_si)
				return std::nullopt;
			const std::vector<std::string> on = m_game_si->GetStringList(section, Patch::PATCH_ENABLE_CONFIG_KEY);
			return (std::find(on.begin(), on.end(), key) != on.end()) ? std::optional<bool>(true) : std::nullopt;
		}
		bool value = false;
		const auto lock = Host::GetSettingsLock();
		SettingsInterface* const si = Layer(layer);
		return (si != nullptr && si->GetBoolValue(section, key, &value)) ? std::optional<bool>(value) : std::nullopt;
	}

	std::optional<int> getInt(int layer, const char* section, const char* key) override
	{
		if (std::strcmp(section, GAME_PATCH_SECTION) == 0)
		{
			// A game's widescreen or no-interlacing patch: 1 on, 2 off, nothing as in Settings.
			if (layer != bp::Own || !m_game_si)
				return std::nullopt;
			const auto lock = Host::GetSettingsLock();
			const auto has = [&](const char* list) {
				const std::vector<std::string> names = m_game_si->GetStringList(Patch::PATCHES_CONFIG_SECTION, list);
				return std::find(names.begin(), names.end(), key) != names.end();
			};
			if (has(Patch::PATCH_DISABLE_CONFIG_KEY))
				return 2;
			if (has(Patch::PATCH_ENABLE_CONFIG_KEY))
				return 1;
			return std::nullopt;
		}
		int value = 0;
		const auto lock = Host::GetSettingsLock();
		SettingsInterface* const si = Layer(layer);
		return (si != nullptr && si->GetIntValue(section, key, &value)) ? std::optional<int>(value) : std::nullopt;
	}

	std::optional<float> getFloat(int layer, const char* section, const char* key) override
	{
		float value = 0;
		const auto lock = Host::GetSettingsLock();
		SettingsInterface* const si = Layer(layer);
		return (si != nullptr && si->GetFloatValue(section, key, &value)) ? std::optional<float>(value) : std::nullopt;
	}

	std::optional<std::string> getString(int layer, const char* section, const char* key) override
	{
		std::string value;
		const auto lock = Host::GetSettingsLock();
		SettingsInterface* const si = Layer(layer);
		return (si != nullptr && si->GetStringValue(section, key, &value)) ? std::optional<std::string>(std::move(value)) : std::nullopt;
	}

	void setBool(int layer, const char* section, const char* key, bool value) override
	{
		if (IsPatchSection(section))
		{
			if (layer != bp::Own || !m_game_si)
				return;
			if (value)
				m_game_si->AddToStringList(section, Patch::PATCH_ENABLE_CONFIG_KEY, key);
			else
				m_game_si->RemoveFromStringList(section, Patch::PATCH_ENABLE_CONFIG_KEY, key);
			m_own_changed = true;
			return;
		}
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
				si->SetBoolValue(section, key, value);
		}
		Changed(layer, section, key);
	}

	void setInt(int layer, const char* section, const char* key, int value) override
	{
		if (std::strcmp(section, GAME_PATCH_SECTION) == 0)
		{
			if (layer != bp::Own || !m_game_si)
				return;
			{
				const auto lock = Host::GetSettingsLock();
				m_game_si->RemoveFromStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, key);
				m_game_si->RemoveFromStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_DISABLE_CONFIG_KEY, key);
				if (value == 1)
					m_game_si->AddToStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, key);
				else if (value == 2)
					m_game_si->AddToStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_DISABLE_CONFIG_KEY, key);
			}
			m_own_changed = true;
			m_pages++; // (the game's patch page shows the same switch)
			return;
		}
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
				si->SetIntValue(section, key, value);
		}
		Changed(layer, section, key);
	}

	void setFloat(int layer, const char* section, const char* key, float value) override
	{
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
				si->SetFloatValue(section, key, value);
		}
		Changed(layer, section, key);
	}

	void setString(int layer, const char* section, const char* key, const std::string& value) override
	{
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
				si->SetStringValue(section, key, value.c_str());
		}
		Changed(layer, section, key);
	}

	void erase(int layer, const char* section, const char* key) override
	{
		if (layer != bp::Own || !m_game_si)
			return;
		if (IsPatchSection(section))
		{
			m_game_si->RemoveFromStringList(section, Patch::PATCH_ENABLE_CONFIG_KEY, key);
		}
		else if (std::strcmp(section, GAME_PATCH_SECTION) == 0)
		{
			m_game_si->RemoveFromStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, key);
			m_game_si->RemoveFromStringList(Patch::PATCHES_CONFIG_SECTION, Patch::PATCH_DISABLE_CONFIG_KEY, key);
			m_own_changed = true;
			m_pages++;
			return;
		}
		else
			m_game_si->DeleteValue(section, key);
		Changed(layer, section, key);
	}

	void clearGame() override
	{
		if (!m_game_si)
			return;
		m_game_si->Clear();
		m_own_changed = true;
		LoadPatches();
		m_pages++;
	}

	int ownCount() override { return m_own_count; }

	// ---- pages the Backend makes
	unsigned pagesGeneration() override { return m_pages; }

	// ---- controller profiles (v1.5.0)

	// The microphone test: recorded and played back on a thread of its own, one at a time.
	static void TestMicrophone(int player)
	{
		static std::atomic_bool s_testing{false};
		if (VMManager::HasValidVM())
		{
			PS5UI::Toast("Close the game first: it has the microphones.");
			return;
		}
		if (s_testing.exchange(true))
		{
			PS5UI::Toast("A microphone test is running already.");
			return;
		}
		std::thread([player] {
			PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
			PS5UI::Toast(fmt::format("Player {}'s microphone: speak or sing now (5 seconds)...", player + 1));
			const PS5Mic::TestResult r = PS5Mic::Record(player, 5.0);
			if (!r.opened)
			{
				PS5UI::Toast(fmt::format("Player {}'s microphone did not open: is the controller on and the player signed in? "
										 "crux2-boot.log has the details.",
					player + 1));
				s_testing = false;
				return;
			}
			const double kept = 100.0 * static_cast<double>(r.samples.size()) / (PS5Mic::TEST_RATE * std::max(r.seconds, 0.1));
			if (r.peak < 300)
				PS5UI::Toast(fmt::format("Player {}'s microphone opened but heard nothing (peak {}): is it muted? Playing it back...",
					player + 1, r.peak));
			else
				PS5UI::Toast(fmt::format("Playing it back: {:.0f}% of the time heard, peak {} of 32767.", kept, r.peak));
			PS5Mic::PlayBack(r.samples);
			PS5UI::Toast(kept < 90.0 ? fmt::format("Done. Only {:.0f}% of the time came through: the gaps are the crackle. "
												   "crux2-boot.log's \"mic test\" lines say why.",
											kept) :
										 std::string("Done. If it sounded clear, SingStar will hear the same."));
			s_testing = false;
		}).detach();
	}

	void SaveControlsProfile()
	{
		bp::editText("The profile's name", "My controls", false, [this](const std::string& typed) {
			std::string name = typed;
			for (char& c : name)
			{
				if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos)
					c = '_';
			}
			if (name.empty())
				return;
			INISettingsInterface profile(VMManager::GetInputProfilePath(name));
			{
				const auto lock = Host::GetSettingsLock();
				SettingsInterface* const base = Host::Internal::GetBaseSettingsLayer();
				Pad::CopyConfiguration(&profile, *base, true, true, false);
				USB::CopyConfiguration(&profile, *base, true, true);
			}
			const bool saved = profile.Save();
			if (saved)
				PS5Platform::MakePublic(profile.GetFileName().c_str(), false);
			m_pages++;
			bp::toast(saved ? fmt::format("Profile \"{}\" saved. A game's options can choose it now.", name) :
			                  fmt::format("Profile \"{}\" could not be saved.", name));
		}, 40);
	}

	void LoadControlsProfile()
	{
		const std::vector<std::string> profiles = Pad::GetInputProfileNames();
		if (profiles.empty())
		{
			bp::toast("There is no profile yet: save the controls as one first.");
			return;
		}
		bp::pick("Load which profile?", profiles, -1, [this, profiles](int choice) {
			if (choice < 0 || choice >= static_cast<int>(profiles.size()))
				return;
			const std::string name = profiles[static_cast<size_t>(choice)];
			INISettingsInterface profile(VMManager::GetInputProfilePath(name));
			if (!profile.Load())
			{
				bp::toast(fmt::format("Profile \"{}\" could not be read.", name));
				return;
			}
			{
				const auto lock = Host::GetSettingsLock();
				SettingsInterface* const base = Host::Internal::GetBaseSettingsLayer();
				Pad::CopyConfiguration(base, profile, true, true, false);
				USB::CopyConfiguration(base, profile, true, true);
			}
			s_shared_apply.store(true, std::memory_order_release);
			s_shared_changed.store(true, std::memory_order_release);
			m_pages++;
			bp::toast(fmt::format("The controls are those of \"{}\" now.", name));
		});
	}

	void SwappedProfile()
	{
		const std::string name = "Cross and Circle swapped";
		INISettingsInterface profile(VMManager::GetInputProfilePath(name));
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface* const base = Host::Internal::GetBaseSettingsLayer();
			Pad::CopyConfiguration(&profile, *base, true, true, false);
			USB::CopyConfiguration(&profile, *base, true, true);
		}
		for (u32 port = 1; port <= 8; port++)
		{
			const std::string section = fmt::format("Pad{}", port);
			const std::vector<std::string> cross = profile.GetStringList(section.c_str(), "Cross");
			const std::vector<std::string> circle = profile.GetStringList(section.c_str(), "Circle");
			profile.SetStringList(section.c_str(), "Cross", circle);
			profile.SetStringList(section.c_str(), "Circle", cross);
		}
		const bool saved = profile.Save();
		if (saved)
			PS5Platform::MakePublic(profile.GetFileName().c_str(), false);
		m_pages++;
		bp::toast(saved ? fmt::format("Profile \"{}\" saved: choose it in a game's options.", name) : std::string("The profile could not be saved."));
	}

	void DeleteControlsProfile()
	{
		const std::vector<std::string> profiles = Pad::GetInputProfileNames();
		bp::pick("Delete which profile?", profiles, -1, [this, profiles](int choice) {
			if (choice < 0 || choice >= static_cast<int>(profiles.size()))
				return;
			const std::string name = profiles[static_cast<size_t>(choice)];
			bp::confirm(fmt::format("Delete \"{}\"?", name), "A game whose options chose it goes back to the controls of every game.",
				[this, name](bool yes) {
					if (!yes)
						return;
					const bool gone = FileSystem::DeleteFilePath(VMManager::GetInputProfilePath(name).c_str());
					m_pages++;
					bp::toast(gone ? fmt::format("Profile \"{}\" deleted.", name) : std::string("The profile could not be deleted."));
				},
				"Delete", "Keep");
		});
	}

	bp::Page controllersPage(int layer) override
	{
		bp::Page page{"\xef\x84\x9b", "Controllers", {}};
		const auto lock = Host::GetSettingsLock();
		SettingsInterface* const si = Host::Internal::GetBaseSettingsLayer();

		// Controller profiles (v1.5.0): the buttons of every port and the USB devices, kept by a
		// name in inputprofiles/ (PCSX2's own input profiles), and chosen for a game in its options.
		{
			const std::vector<std::string> profiles = Pad::GetInputProfileNames();
			page.rows.push_back(Heading("CONTROLLER PROFILES"));
			if (layer == bp::Own)
			{
				bp::Option use;
				use.kind = bp::Option::StringList;
				use.label = "This game's controls";
				use.desc = "Uses a saved controller profile for this game.";
				use.section = "EmuCore";
				use.key = "InputProfileName";
				use.defString = "";
				use.flags = bp::Option::OneGame | bp::Option::NextStart;
				use.names.emplace_back("As in Settings");
				use.strings.emplace_back("");
				for (const std::string& name : profiles)
				{
					use.names.push_back(name);
					use.strings.push_back(name);
				}
				page.rows.push_back(std::move(use));
			}
			page.rows.push_back(Action("Save the controls as a profile...",
				"Saves the current button mappings as a profile.",
				[this] { SaveControlsProfile(); }, bp::Option::AllGames));
			page.rows.push_back(Action("Load a profile into the controls...", "Loads a profile's button mappings, replacing the current ones.",
				[this] { LoadControlsProfile(); }, bp::Option::AllGames));
			page.rows.push_back(Action("Make a profile with Cross and Circle swapped",
				"Saves a profile with Cross and Circle swapped, for Japanese games.",
				[this] { SwappedProfile(); }, bp::Option::AllGames));
			if (!profiles.empty())
			{
				page.rows.push_back(Action("Delete a profile...", "", [this] { DeleteControlsProfile(); }, bp::Option::AllGames));
			}
		}

		page.rows.push_back(Heading("ALL PORTS"));
		page.rows.push_back(Action("Put the buttons back as they were",
			"Resets all controller ports and hotkeys to their default buttons.",
			[this] {
				bp::confirm("Put the buttons back?", "All controller ports and hotkeys will be reset to their default buttons.",
					[this](bool yes) {
						if (!yes)
							return;
						{
							const auto settings = Host::GetSettingsLock();
							SettingsInterface* const base = Host::Internal::GetBaseSettingsLayer();
							Pad::SetDefaultControllerConfig(*base);
							Pad::SetDefaultHotkeyConfig(*base);
						}
						s_shared_apply.store(true, std::memory_order_release);
						s_shared_changed.store(true, std::memory_order_release);
						m_pages++;
						bp::toast("The buttons are as they were.");
					},
					"Put them back", "Leave them");
			},
			bp::Option::AllGames));
		// SingStar (v1.5.0): the PlayStation 2's USB microphones are the console's (ps5_mic.h).
		{
			page.rows.push_back(Heading("SINGSTAR"));
			bp::Option mics;
			mics.kind = bp::Option::IntList;
			mics.label = "SingStar microphones";
			mics.desc = "Uses the controller's microphone or a headset as the PS2's USB microphones, for SingStar and other singing "
				"games.";
			mics.section = "PS5";
			mics.key = "SingStar";
			mics.defInt = 0;
			mics.names = {"None", "One: player 1's", "Two: players 1 and 2"};
			mics.ints = {0, 1, 2};
			mics.flags = bp::Option::NextStart;
			page.rows.push_back(std::move(mics));
			for (int player = 0; player < 2; player++)
			{
				page.rows.push_back(Action(fmt::format("Test player {}'s microphone", player + 1),
					"Speak or sing for five seconds; it is played back to you as a game would hear it, and the boot log's \"mic test\" "
					"lines say how the microphone did. Close the game first",
					[player] { TestMicrophone(player); }, bp::Option::AllGames));
			}
		}
		// Who plays on what: each signed-in user's controller is a player (the one who started the
		// title is the first), put on the PlayStation 2's ports in one of the three ways games expect.
		page.rows.push_back(Heading("PLAYERS"));
		page.rows.push_back(Action("One or two players", "Player 1 on port 1, player 2 on port 2, no multitap.",
			[this] { SetUpPlayers(0); }, bp::Option::AllGames));
		page.rows.push_back(Action("Up to four players, multitap in port 1",
			"Players 1 to 4 on a multitap in port 1, for most games with three or four players.", [this] { SetUpPlayers(1); },
			bp::Option::AllGames));
		page.rows.push_back(Action("Up to four players, multitap in port 2",
			"Player 1 on port 1, players 2 to 4 on a multitap in port 2, for games that need it there.",
			[this] { SetUpPlayers(2); }, bp::Option::AllGames));
		page.rows.push_back(Toggle("Light bar in the player's colour",
			"Sets each controller's light bar to its player colour: blue, red, green, pink.",
			"PS5", "LightBars", false, bp::Option::AllGames));
		{
			bp::Option now;
			now.kind = bp::Option::Info;
			now.label = "Controllers connected";
			now.info = [] { return std::to_string(std::max(0, s_controllers.load(std::memory_order_acquire))); };
			now.flags = bp::Option::AllGames;
			page.rows.push_back(std::move(now));
		}
		page.rows.push_back(Heading("BY HAND"));
		page.rows.push_back(Toggle("Multitap in port 1", "Enables an additional three controller slots. Not supported in all games.", "Pad",
			"MultitapPort1", false, bp::Option::AllGames));
		page.rows.push_back(Toggle("Multitap in port 2", "Enables an additional three controller slots. Not supported in all games.", "Pad",
			"MultitapPort2", false, bp::Option::AllGames));

		const bool multitap[2] = {si->GetBoolValue("Pad", "MultitapPort1", false), si->GetBoolValue("Pad", "MultitapPort2", false)};
		// The first port's four, then the second's (as PCSX2 lists them).
		static constexpr u32 order[Pad::NUM_CONTROLLER_PORTS] = {0, 2, 3, 4, 1, 5, 6, 7};
		static constexpr const char* sections[Pad::NUM_CONTROLLER_PORTS] = {"Pad1", "Pad2", "Pad3", "Pad4", "Pad5", "Pad6", "Pad7", "Pad8"};
		static constexpr char letters[4] = {'A', 'B', 'C', 'D'};
		for (const u32 global : order)
		{
			const auto [port, slot] = sioConvertPadToPortAndSlot(global);
			if (sioPadIsMultitapSlot(global) && !multitap[port])
				continue;
			const char* const section = sections[global];
			page.rows.push_back(Heading(multitap[port] ? fmt::format("CONTROLLER PORT {}{}", port + 1, letters[slot]) :
														fmt::format("CONTROLLER PORT {}", port + 1)));

			const Pad::ControllerInfo* const ci = Pad::GetConfigControllerType(*si, section, global);
			{
				bp::Option type;
				type.kind = bp::Option::StringList;
				type.label = "Controller";
				type.desc = "The controller plugged into this port.";
				type.section = section;
				type.key = "Type";
				type.defString = ci != nullptr ? ci->name : "";
				type.flags = bp::Option::AllGames;
				for (const auto& [name, display] : Pad::GetControllerTypeNames())
				{
					type.strings.emplace_back(name);
					type.names.emplace_back(display);
				}
				page.rows.push_back(std::move(type));
			}
			if (ci == nullptr || ci->bindings.empty())
				continue;

			page.rows.push_back(Action("Map it to a controller of the console",
				fmt::format("Every button of this port goes on the console's controller {} (the first one, if there are fewer)", global + 1),
				[this, global] { AutomaticMapping(global); }, bp::Option::AllGames));
			// (Read from si: this page holds the settings lock, which Host::GetBase*SettingValue takes again.)
			if (si->GetBoolValue("PS5", "USBPads", false))
			{
				page.rows.push_back(Action("Map it to a USB controller",
					"Maps this port to a controller plugged into the console's USB port.",
					[this, global] { AutomaticMapping(global, true); }, bp::Option::AllGames));
			}

			for (const InputBindingInfo& bi : ci->bindings)
			{
				if (bi.name == nullptr)
					continue;
				if (bi.bind_type == InputBindingInfo::Type::Motor)
				{
					// A motor is not pressed: it is chosen.
					bp::Option motor;
					motor.kind = bp::Option::StringList;
					motor.label = bi.display_name;
					motor.section = section;
					motor.key = bi.name;
					motor.flags = bp::Option::AllGames;
					motor.strings.emplace_back("");
					motor.names.emplace_back("None");
					for (int pad = 0; pad < 4; pad++)
					{
						motor.strings.push_back(fmt::format("SDL-{}/LargeMotor", pad));
						motor.names.push_back(fmt::format("Controller {}: the strong motor", pad + 1));
						motor.strings.push_back(fmt::format("SDL-{}/SmallMotor", pad));
						motor.names.push_back(fmt::format("Controller {}: the light motor", pad + 1));
					}
					page.rows.push_back(std::move(motor));
					continue;
				}
				if (bi.bind_type != InputBindingInfo::Type::Button && bi.bind_type != InputBindingInfo::Type::Axis &&
					bi.bind_type != InputBindingInfo::Type::HalfAxis)
					continue;
				bp::Option bind;
				bind.kind = bp::Option::Bind;
				bind.label = bi.display_name;
				bind.section = section;
				bind.key = bi.name;
				bind.bindType = static_cast<int>(bi.bind_type);
				bind.flags = bp::Option::AllGames;
				page.rows.push_back(std::move(bind));
			}
			for (const SettingInfo& setting : ci->settings)
			{
				bp::Option row;
				if (SettingRow(section, setting, &row))
					page.rows.push_back(std::move(row));
			}
		}
		return page;
	}

	bp::Page hotkeysPage() override
	{
		bp::Page page{"\xef\x84\x9c", "Hotkeys", {}};
		const HotkeyInfo* last = nullptr;
		for (const HotkeyInfo* hotkey : m_hotkeys)
		{
			// Screenshots and video capture are the console's own (the Create button).
			if (std::strcmp(hotkey->name, "Screenshot") == 0 || std::strcmp(hotkey->name, "ToggleVideoCapture") == 0)
				continue;
			if (last == nullptr || std::strcmp(hotkey->category, last->category) != 0)
			{
				std::string heading = hotkey->category;
				std::transform(heading.begin(), heading.end(), heading.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				page.rows.push_back(Heading(std::move(heading)));
				last = hotkey;
			}
			bp::Option bind;
			bind.kind = bp::Option::Bind;
			bind.label = hotkey->display_name;
			bind.section = "Hotkeys";
			bind.key = hotkey->name;
			bind.bindType = static_cast<int>(InputBindingInfo::Type::Button);
			bind.flags = bp::Option::AllGames;
			if (std::strcmp(hotkey->name, "GSDumpSingleFrame") == 0)
				bind.flags |= bp::Option::Advanced;
			page.rows.push_back(std::move(bind));
		}
		return page;
	}

	bp::Page memoryCardsPage(int layer) override
	{
		// v1.5.0: the cards first, each opening its menu (bp_cards.cpp); then what can be made or
		// brought in; then the slots; then the switches.
		bp::Page page{"\xef\x9f\x82", "Memory cards", {}};
		const bool own = (layer == bp::Own);
		const bool each = Host::GetBaseBoolSettingValue("PS5", "CardPerGame", true);
		if (own && each && m_game_si)
		{
			// "A memory card for each game": what slot 1 holds while this game runs.
			bool chosen = false;
			{
				const auto lock = Host::GetSettingsLock();
				chosen = m_game_si->ContainsValue("MemoryCards", "Slot1_Filename") || m_game_si->ContainsValue("MemoryCards", "Slot1_Enable");
			}
			const std::string name = (m_game.kind == bp::GameInfo::PS2) ? PS5Extras::CardNameFor(m_game_serial) : std::string();
			page.rows.push_back(Heading("A MEMORY CARD FOR EACH GAME"));
			page.rows.push_back(Line("This game's card",
				chosen ? std::string("The one chosen for slot 1 below") :
				name.empty() ? std::string("None: PCSX2's database does not know this game, and it uses the card of every game") :
				               fmt::format("{}, in slot 1 while this game runs", Path::GetFileTitle(name))));
		}
		if (!own)
		{
			// v1.6.0: the cards and their saves are the card manager's (bp_cards.cpp), a sheet of its own.
			page.rows.push_back(Heading("CARD MANAGER"));
			bp::Option manager = Action("Open the card manager",
				"Manage memory cards and saves: copy, delete, import and export saves, and create, format or rename cards.",
				[this] { bp::openCardManager(this); }, bp::Option::AllGames);
			manager.info = [] {
				const size_t n = FileMcd_GetAvailableCards(true).size();
				return n == 1 ? std::string("1 card") : fmt::format("{} cards", n);
			};
			page.rows.push_back(std::move(manager));
		}

		for (u32 slot = 0; slot < 2; slot++)
		{
			page.rows.push_back(Heading(fmt::format("SLOT {}", slot + 1)));
			page.rows.push_back(Toggle("A card in the slot", "Inserts a memory card into this slot. Off: the slot is empty.", "MemoryCards", fmt::format("Slot{}_Enable", slot + 1),
				true));

			bp::Option card;
			card.kind = bp::Option::StringList;
			card.label = "Card";
			card.desc = "The memory card file in this slot.";
			card.section = "MemoryCards";
			card.key = fmt::format("Slot{}_Filename", slot + 1);
			card.defString = FileMcd_GetDefaultName(slot);
			// The card a slot holds, as the settings have it (the game's own, else every game's).
			const auto held = [&](u32 of) {
				const std::string key = fmt::format("Slot{}_Filename", of + 1);
				std::string value;
				const auto lock = Host::GetSettingsLock();
				if (own && m_game_si && m_game_si->GetStringValue("MemoryCards", key.c_str(), &value))
					return value;
				if (Host::Internal::GetBaseSettingsLayer()->GetStringValue("MemoryCards", key.c_str(), &value))
					return value;
				return FileMcd_GetDefaultName(of);
			};
			const std::string current = held(slot), other = held(1 - slot);
			static constexpr const char* sizes[] = {"", "8 MB", "16 MB", "32 MB", "64 MB", "PlayStation 1"};
			bool listed = current.empty();
			for (AvailableMcdInfo& info : FileMcd_GetAvailableCards(true))
			{
				// The card the other slot holds is not offered: one file, one slot.
				if (!other.empty() && info.name == other && info.name != current)
					continue;
				const u32 type = static_cast<u32>(info.file_type);
				const std::string_view title = Path::GetFileTitle(info.name);
				if (info.type == MemoryCardType::Folder)
					card.names.push_back(fmt::format("{} (a folder)", title));
				else if (type > 0 && type < std::size(sizes))
					card.names.push_back(fmt::format("{} ({})", title, sizes[type]));
				else
					card.names.emplace_back(title);
				listed |= (info.name == current);
				card.strings.push_back(std::move(info.name));
			}
			if (!listed)
			{
				card.names.insert(card.names.begin(), std::string(Path::GetFileTitle(current)));
				card.strings.insert(card.strings.begin(), current);
			}
			card.names.emplace_back("No card");
			card.strings.emplace_back("");
			page.rows.push_back(std::move(card));
		}

		page.rows.push_back(Heading("OPTIONS"));
		if (!own)
		{
			page.rows.push_back(Toggle("A memory card for each game",
				"Gives every game its own memory card in slot 1, created when the game first starts.",
				"PS5", "CardPerGame", true, bp::Option::AllGames | bp::Option::NextStart));
		}
		page.rows.push_back(Toggle("A folder card shows only the running game's saves",
			"Simulates a larger memory card by filtering saves only to the current game.", "EmuCore", "McdFolderAutoManage", true));
		return page;
	}

	// ---- the saves on the cards: what the card's menu (bp_cards.cpp) asks for. The work is
	// PS5Jobs', on a thread of its own; renaming and deleting a card's file is done here.
	std::vector<std::string> cardNames() override
	{
		std::vector<std::string> names;
		for (AvailableMcdInfo& info : FileMcd_GetAvailableCards(true))
			names.push_back(std::move(info.name));
		return names;
	}

	// ---- the card manager's grouping: a game's own card is named by its serial (PS5Extras::CardNameFor)
	std::string cardSerial(const std::string& card) override
	{
		const std::string title(Path::GetFileTitle(card));
		// "SCUS-97580", and a copy of it ("SCUS-97580 copy") is still that game's.
		const std::string serial = title.substr(0, title.find(' '));
		return (GameDatabase::findGame(serial) != nullptr) ? serial : std::string();
	}

	std::string cardGame(const std::string& card) override
	{
		const std::string serial = cardSerial(card);
		const auto* entry = serial.empty() ? nullptr : GameDatabase::findGame(serial);
		if (entry == nullptr)
			return std::string();
		const std::string title(Path::GetFileTitle(card));
		const std::string name = entry->name.empty() ? serial : entry->name;
		return (title.size() > serial.size()) ? name + title.substr(serial.size()) : name;
	}

	bool systemCard(const std::string& card) override
	{
		return card == FileMcd_GetDefaultName(0) || card == FileMcd_GetDefaultName(1);
	}

	int cardMegabytes(const std::string& card) override
	{
		const std::optional<AvailableMcdInfo> file = FileMcd_GetCardInfo(card);
		if (!file.has_value())
			return 8;
		if (file->type == MemoryCardType::Folder)
			return -1;
		switch (file->file_type)
		{
			case MemoryCardFileType::PS2_8MB: return 8;
			case MemoryCardFileType::PS2_16MB: return 16;
			case MemoryCardFileType::PS2_32MB: return 32;
			case MemoryCardFileType::PS2_64MB: return 64;
			case MemoryCardFileType::PS1: return 0;
			default: return 8;
		}
	}

	bp::CardView lookAtCard(const std::string& card) override
	{
		bp::CardView view;
		const std::optional<AvailableMcdInfo> file = FileMcd_GetCardInfo(card);
		if (!file.has_value())
		{
			view.why = "The card is not there any more";
			return view;
		}
		if (file->type == MemoryCardType::Folder)
		{
			view.why = "A folder card: its saves are the folders inside it, to be copied as they are";
			return view;
		}
		if (file->file_type == MemoryCardFileType::PS1)
		{
			view.why = "A PlayStation 1 card: its saves are not listed here";
			return view;
		}
		const PS5Cards::CardInfo info = PS5Cards::Look(PS5Jobs::CardPath(card));
		view.state = (info.state == PS5Cards::CardInfo::Listed) ? bp::CardView::Listed :
		             (info.state == PS5Cards::CardInfo::New) ? bp::CardView::New : bp::CardView::Closed;
		view.why = info.why;
		view.freeBytes = info.free_bytes;
		view.totalBytes = info.total_bytes;
		view.undo = PS5Cards::HasUndo(PS5Jobs::CardPath(card), PS5Jobs::CardUndoFolder());
		for (const PS5Cards::SaveInfo& save : info.saves)
			view.saves.push_back({save.name, save.title, save.when, save.bytes});
		return view;
	}

	std::string cardsBusy() override
	{
		if (s_boot_pending.load(std::memory_order_acquire) || VMManager::GetState() != VMState::Shutdown)
			return "Close the game first: it has the memory cards, and would write over the change.";
		return std::string();
	}

	void findSaveFiles(std::function<void(std::vector<bp::SaveFileFound>)> then) override
	{
		PS5Jobs::FindSaveFiles([then = std::move(then)](std::vector<PS5Jobs::FoundSave> found) {
			std::vector<bp::SaveFileFound> files;
			for (PS5Jobs::FoundSave& one : found)
				files.push_back({std::move(one.path), std::move(one.name), std::move(one.where), one.size});
			then(std::move(files));
		});
	}

	std::string saveFilePlaces() override
	{
		return fmt::format("Save files (.psu, .max, .cbs, .sps, .xps, .psv) are looked for in a folder named saves on every USB drive that is in and "
						   "on every share of network.cfg, and in {} (which the console's FTP server reaches).",
			Shown(PS5Jobs::SavesFolder()));
	}

	std::vector<std::string> exportPlaces() override
	{
		std::vector<std::string> names;
		for (const PS5Jobs::Place& place : PS5Jobs::ExportPlaces())
			names.push_back(place.name);
		return names;
	}

	void importSave(const std::string& card, const bp::SaveFileFound& file) override { PS5Jobs::ImportSave(card, file.path); }

	void exportSave(const std::string& card, const std::string& save, int place) override
	{
		const std::vector<PS5Jobs::Place> places = PS5Jobs::ExportPlaces();
		if (place >= 0 && place < static_cast<int>(places.size()))
			PS5Jobs::ExportSave(card, save, places[static_cast<size_t>(place)]);
		else
			bp::toast("That place is not there any more.");
	}

	void copySave(const std::string& card, const std::string& save, const std::string& toCard) override { PS5Jobs::CopySave(card, save, toCard); }
	void deleteSave(const std::string& card, const std::string& save) override { PS5Jobs::DeleteSave(card, save); }
	void undoCard(const std::string& card) override { PS5Jobs::UndoCard(card); }
	void duplicateCard(const std::string& card) override
	{
		if (FileMcd_GetCardInfo(card).value_or(AvailableMcdInfo{}).type == MemoryCardType::Folder)
		{
			bp::toast("A folder card is copied as a folder, from a PC.");
			return;
		}
		PS5Jobs::DuplicateCard(card);
	}

	void renameCard(const std::string& card, const std::string& typed) override
	{
		const std::string title(StringUtil::StripWhitespace(typed));
		if (title.empty() || title[0] == '.' || title.find_first_of("/\\:*?\"<>|") != std::string::npos)
		{
			bp::toast("A card's name cannot be empty, begin with a dot or hold any of / \\ : * ? \" < > |");
			return;
		}
		const std::string_view extension = Path::GetExtension(card);
		const std::string name = extension.empty() ? title : fmt::format("{}.{}", title, extension);
		if (name == card)
			return;
		if (FileMcd_GetCardInfo(name).has_value() || FileSystem::FileExists(PS5Jobs::CardPath(name).c_str()) ||
			FileSystem::DirectoryExists(PS5Jobs::CardPath(name).c_str()))
		{
			bp::toast(fmt::format("There is a card named \"{}\" already.", title));
			return;
		}
		if (!FileMcd_RenameCard(card, name))
		{
			bp::toast("The card could not be renamed (logs/emulog.txt has the reason).");
			return;
		}
		// The slots that held it hold it still, under its new name; and what was kept of it
		// before its last change is kept under that name too.
		int slots = 0;
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface& si = *Host::Internal::GetBaseSettingsLayer();
			for (u32 slot = 0; slot < 8; slot++)
			{
				const std::string key = fmt::format("Slot{}_Filename", slot + 1);
				std::string held;
				if (si.GetStringValue("MemoryCards", key.c_str(), &held) ? (held == card) : (FileMcd_GetDefaultName(slot) == card))
				{
					si.SetStringValue("MemoryCards", key.c_str(), name.c_str());
					slots++;
				}
			}
		}
		if (slots > 0)
		{
			s_shared_apply.store(true, std::memory_order_release);
			s_shared_changed.store(true, std::memory_order_release);
		}
		FileSystem::RenamePath(Path::Combine(PS5Jobs::CardUndoFolder(), card).c_str(), Path::Combine(PS5Jobs::CardUndoFolder(), name).c_str());
		ps5px_log("cards: \"%s\" renamed to \"%s\" (%d slot(s) follow)", card.c_str(), name.c_str(), slots);
		m_pages++;
		bp::toast(fmt::format("The card is \"{}\" now.{}", title,
			(slots > 0) ? " The slot that held it holds it still." : " A game that had it as its own card gets a new one of the old name."), 6.f);
	}

	void deleteCard(const std::string& card) override
	{
		if (!FileMcd_DeleteCard(card))
		{
			bp::toast("The card could not be deleted (logs/emulog.txt has the reason).");
			return;
		}
		FileSystem::DeleteFilePath(Path::Combine(PS5Jobs::CardUndoFolder(), card).c_str());
		ps5px_log("cards: \"%s\" deleted", card.c_str());
		m_pages++;
		bp::toast(fmt::format("\"{}\" was deleted. A slot that held it gets a new, empty card of that name when a game next starts.", bp::cardTitle(card)), 6.f);
	}

	// ---- the card manager of v1.5.0 (bp_cards.cpp asks; PS5Jobs does it)

	std::vector<std::string> cardSources() override
	{
		std::vector<std::string> names;
		for (const PS5Jobs::CardSource& source : PS5Jobs::CardSources())
			names.push_back(source.name);
		return names;
	}

	std::string ps2Address() override { return Host::GetBaseStringSettingValue("PS5", "Ps2Address", ""); }

	void findIn(int source, const std::string& address, std::function<void(std::vector<bp::CardFound>, std::string why)> then) override
	{
		std::vector<PS5Jobs::CardSource> sources = PS5Jobs::CardSources();
		if (source < 0 || source >= static_cast<int>(sources.size()))
			return;
		PS5Jobs::CardSource chosen = sources[static_cast<size_t>(source)];
		if (chosen.ps2)
			chosen.path = address;
		PS5Jobs::FindIn(chosen, [then](std::vector<PS5Jobs::CardThing> things, std::string why) {
			std::vector<bp::CardFound> found;
			for (PS5Jobs::CardThing& t : things)
			{
				bp::CardFound f;
				f.kind = static_cast<bp::CardFound::Kind>(t.kind);
				f.path = std::move(t.path);
				f.name = std::move(t.name);
				f.where = std::move(t.where);
				f.bytes = t.size;
				f.ps2 = std::move(t.ps2);
				found.push_back(std::move(f));
			}
			then(std::move(found), std::move(why));
		});
	}

	static PS5Jobs::CardThing Thing(const bp::CardFound& f)
	{
		PS5Jobs::CardThing t;
		t.kind = static_cast<PS5Jobs::CardThing::Kind>(f.kind);
		t.path = f.path;
		t.name = f.name;
		t.where = f.where;
		t.size = f.bytes;
		t.ps2 = f.ps2;
		return t;
	}

	void bringIn(const std::string& card, const bp::CardFound& found) override { PS5Jobs::BringIn(card, Thing(found)); }
	void bringInCard(const bp::CardFound& image, const std::string& title) override { PS5Jobs::BringInCard(Thing(image), title); }

	bp::CardView lookAtImage(const bp::CardFound& image) override
	{
		bp::CardView view;
		const PS5Cards::CardInfo info = PS5Cards::Look(image.path);
		view.state = (info.state == PS5Cards::CardInfo::Listed) ? bp::CardView::Listed :
		             (info.state == PS5Cards::CardInfo::New) ? bp::CardView::New : bp::CardView::Closed;
		view.why = info.why;
		view.freeBytes = info.free_bytes;
		view.totalBytes = info.total_bytes;
		for (const PS5Cards::SaveInfo& save : info.saves)
			view.saves.push_back({save.name, save.title, save.when, save.bytes});
		return view;
	}

	void copyFromImage(const bp::CardFound& image, const std::string& save, const std::string& toCard) override
	{
		PS5Jobs::CopyFromImage(image.path, save, toCard);
	}

	void createCard(const std::string& title, int megabytes) override { PS5Jobs::CreateCard(title, megabytes); }
	void formatCard(const std::string& card) override { PS5Jobs::FormatCard(card); }

	void exportCard(const std::string& card, int place) override
	{
		// (exportPlaces(): the saves folder first, then the drives and shares in Places()'s order.)
		const std::vector<PS5Jobs::Place> places = PS5Jobs::Places();
		if (place >= 1 && place <= static_cast<int>(places.size()))
			PS5Jobs::ExportCard(card, places[static_cast<size_t>(place - 1)]);
	}

	int slotOf(const std::string& card) override
	{
		for (u32 slot = 0; slot < 2; slot++)
		{
			const std::string key = fmt::format("Slot{}_Filename", slot + 1);
			const std::string enable = fmt::format("Slot{}_Enable", slot + 1);
			if (Host::GetBaseBoolSettingValue("MemoryCards", enable.c_str(), true) &&
				Host::GetBaseStringSettingValue("MemoryCards", key.c_str(), FileMcd_GetDefaultName(slot).c_str()) == card)
				return static_cast<int>(slot) + 1;
		}
		return 0;
	}

	// The PlayStation 2's network adapter and hard drive. The adapter is PCSX2's "Sockets" one (the
	// only one a console can have: vendor DEV9/net.cpp): the game gets a network of its own, and
	// what it sends goes out through the console's connection.
	bp::Page networkPage(int layer) override
	{
		bp::Page page{"\xef\x9b\xbf", "Network", {}};
		page.rows.push_back(Heading("NETWORK ADAPTER"));
		page.rows.push_back(Toggle("Network adapter",
			"Enables the network adapter for online functionality and LAN play.",
			"DEV9/Eth", "EthEnable", false));
		{
			bp::Option now;
			now.kind = bp::Option::Info;
			now.label = "The console's connection";
			now.info = [] {
				const PS5Net::Status status = PS5Net::CurrentStatus();
				if (status.console_ip.empty())
					return std::string("No address found");
				return status.gateway.empty() ? status.console_ip : status.console_ip + ", through " + status.gateway;
			};
			page.rows.push_back(std::move(now));
			bp::Option game;
			game.kind = bp::Option::Info;
			game.label = "The game's connection";
			game.info = [] {
				const PS5Net::Status status = PS5Net::CurrentStatus();
				if (!status.adapter_open)
					return std::string("No game is using the adapter");
				return fmt::format("{}: {} name(s) found, {} not; {} connection(s) made, {} refused", status.ps2_ip.empty() ? "No address yet" : status.ps2_ip,
					status.dns_answers, status.dns_failures, status.tcp_connects, status.tcp_failures);
			};
			page.rows.push_back(std::move(game));
		}
		if (layer == bp::Shared)
		{
			// The first name server, chosen in one step: a service that keeps games' online play
			// going (its address comes with the title), the console's own, or an address typed in.
			bp::Option first = Action("First name server",
				"The DNS server games use to find their online servers. PS2 Online and PS Rewired keep many games online; check "
					"their websites to see which supports your game.",
				{}, bp::Option::AllGames);
			first.info = [this] {
				const std::string mode = getString(bp::Shared, "DEV9/Eth", "ModeDNS1").value_or(std::string("Auto"));
				if (mode != "Manual")
					return std::string("The console's own");
				const std::string now = getString(bp::Shared, "DEV9/Eth", "DNS1").value_or(std::string());
				for (const PS5Presets::NameServer& server : PS5Presets::NAME_SERVERS)
				{
					if (now == server.address)
						return fmt::format("{}  ({})", server.name, server.address);
				}
				int numbers[4];
				return (bp::parseAddress(now, numbers) && now != "0.0.0.0") ? now : std::string("Not set");
			};
			first.action = [this] {
				std::vector<std::string> names;
				const std::string mode = getString(bp::Shared, "DEV9/Eth", "ModeDNS1").value_or(std::string("Auto"));
				const std::string now = getString(bp::Shared, "DEV9/Eth", "DNS1").value_or(std::string());
				const int presets = static_cast<int>(std::size(PS5Presets::NAME_SERVERS));
				int current = presets + 1;
				for (int i = 0; i < presets; i++)
				{
					const PS5Presets::NameServer& server = PS5Presets::NAME_SERVERS[i];
					names.push_back(fmt::format("{}   \xc2\xb7   {}   \xc2\xb7   {}", server.name, server.address, server.site));
					if (mode == "Manual" && now == server.address)
						current = i;
				}
				names.emplace_back("The console's own name server");
				names.emplace_back("An address of your own...");
				if (mode != "Manual")
					current = presets;
				bp::pick("First name server", std::move(names), current, [this, now](int choice) {
					if (choice >= 0 && choice < presets)
					{
						setString(bp::Shared, "DEV9/Eth", "DNS1", PS5Presets::NAME_SERVERS[choice].address);
						setString(bp::Shared, "DEV9/Eth", "ModeDNS1", "Manual");
					}
					else if (choice == presets)
					{
						setString(bp::Shared, "DEV9/Eth", "ModeDNS1", "Auto");
					}
					else if (choice == presets + 1)
					{
						const std::string from = PS5Net::CurrentStatus().gateway;
						bp::editAddress("First name server's address", now == "0.0.0.0" ? std::string() : now,
							from.empty() ? std::string("192.168.1.1") : from, [this](const std::string& entered) {
								setString(bp::Shared, "DEV9/Eth", "DNS1", entered.empty() ? std::string("0.0.0.0") : entered);
								setString(bp::Shared, "DEV9/Eth", "ModeDNS1", entered.empty() ? "Auto" : "Manual");
							});
					}
				});
			};
			page.rows.push_back(std::move(first));
		}
		for (int i = (layer == bp::Shared) ? 2 : 1; i <= 2; i++)
		{
			const std::string mode_key = fmt::format("ModeDNS{}", i), address_key = fmt::format("DNS{}", i);
			bp::Option mode;
			mode.kind = bp::Option::StringList;
			mode.label = (i == 1) ? "First name server" : "Second name server";
			mode.desc = (i == 1) ? "The DNS server games use to find their online servers. Most revival servers ask you to set their address here." :
								   "Used when the first DNS server doesn't answer.";
			mode.section = "DEV9/Eth";
			mode.key = mode_key;
			mode.defString = "Auto";
			mode.names = {"The console's own", "The address below"};
			mode.strings = {"Auto", "Manual"};
			page.rows.push_back(std::move(mode));

			bp::Option address = Action((i == 1) ? "First name server's address" : "Second name server's address",
				"Changed number by number", {}, bp::Option::AllGames);
			address.info = [this, address_key] {
				const std::string now = getString(bp::Shared, "DEV9/Eth", address_key.c_str()).value_or(std::string());
				int numbers[4];
				return (bp::parseAddress(now, numbers) && now != "0.0.0.0") ? now : std::string("Not set");
			};
			address.action = [this, address_key, title = address.label] {
				const std::string now = getString(bp::Shared, "DEV9/Eth", address_key.c_str()).value_or(std::string());
				const std::string from = PS5Net::CurrentStatus().gateway;
				bp::editAddress(title, now == "0.0.0.0" ? std::string() : now, from.empty() ? std::string("192.168.1.1") : from,
					[this, address_key](const std::string& entered) {
						setString(bp::Shared, "DEV9/Eth", address_key.c_str(), entered.empty() ? std::string("0.0.0.0") : entered);
					});
			};
			address.enabled = [this, mode_key](int) { return getString(bp::Shared, "DEV9/Eth", mode_key.c_str()).value_or(std::string("Auto")) == "Manual"; };
			if (layer == bp::Shared)
				page.rows.push_back(std::move(address));
		}

		page.rows.push_back(Heading("HARD DRIVE"));
		page.rows.push_back(Toggle("Hard drive", "Enables the internal Hard Disk Drive for expanded storage.",
			"DEV9/Hdd", "HddEnable", false));
		if (layer == bp::Shared)
		{
			bp::Option file;
			file.kind = bp::Option::Info;
			file.label = "The drive's file";
			file.info = [] {
				u64 size = 0, allocated = 0;
				if (!PS5Net::DescribeHddImage(HddPath(), &size, &allocated))
					return std::string("None yet");
				return fmt::format("{} GB, {:.1f} GB of it written", size >> 30, static_cast<double>(allocated) / (1024.0 * 1024.0 * 1024.0));
			};
			page.rows.push_back(std::move(file));
			for (const int size : {40, 120})
			{
				page.rows.push_back(Action(fmt::format("Make a new hard drive of {} GB", size),
					size == 40 ? "The size of the drive Sony sold. An empty drive: the game, or the software that came with the drive, formats it" :
								 "For more than a few games. The file takes room only as it is written to",
					[size] {
						const std::string path = HddPath();
						if (FileSystem::FileExists(path.c_str()))
						{
							bp::message("A hard drive is there already",
								"Making a new one would lose what is on it. To start over, delete inis/DEV9hdd.raw in the title folder (over FTP) and come back here.");
							return;
						}
						std::string error;
						if (PS5Net::CreateHddImage(path, size, &error))
						{
							PS5Platform::MakePublic(path.c_str(), false);
							bp::toast(fmt::format("A hard drive of {} GB was made. Switch \"Hard drive\" on to use it.", size), 7.f);
						}
						else
						{
							bp::message("The hard drive could not be made", error.empty() ? std::string("crux2-boot.log has the reason.") : error);
						}
					},
					bp::Option::AllGames | bp::Option::NoGame));
			}
		}
		return page;
	}

	static std::string HddPath()
	{
		// (As vendor DEV9.cpp's GetHDDPath finds it.)
		std::string file = Host::GetBaseStringSettingValue("DEV9/Hdd", "HddFile", "DEV9hdd.raw");
		if (file.empty())
			file = "DEV9hdd.raw";
		return Path::IsAbsolute(file) ? file : Path::Combine(EmuFolders::Settings, file);
	}

	bp::Page biosPage(int) override
	{
		bp::Page page{"\xef\x8b\x9b", "BIOS", {}};
		bp::Option bios;
		bios.kind = bp::Option::StringList;
		bios.label = "BIOS";
		bios.desc = "The PS2 BIOS, which every game needs. Use a dump from your own console.";
		bios.section = "Filenames";
		bios.key = "BIOS";
		bios.flags = bp::Option::NextStart;
		bios.names.emplace_back("The first one found");
		bios.strings.emplace_back("");

		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
		for (const FILESYSTEM_FIND_DATA& file : files)
		{
			u32 version = 0, region = 0;
			std::string description, zone;
			if (!IsBIOS(file.FileName.c_str(), version, description, region, zone))
				continue;
			const std::string_view name = Path::GetFileName(file.FileName);
			bios.names.push_back(fmt::format("{} ({})", description, name));
			bios.strings.emplace_back(name);
		}
		const bool none = (bios.strings.size() == 1);
		page.rows.push_back(std::move(bios));
		if (none)
			page.rows.push_back(Line("None of your own found", "Copy yours to the folder below and open this page again"));
		page.rows.push_back(Line("Folder", Shown(EmuFolders::Bios)));
		// OpenPS2BIOS (ps5_free_bios.h): only ever when the folder above has no BIOS.
		page.rows.push_back(Toggle("Free BIOS when none of my own is found",
			"Uses OpenPS2BIOS, an open-source BIOS included with Crux2, when no BIOS of your own is found. Experimental: "
				"many games don't get past their menus.",
			"PS5", "FreeBios", true, bp::Option::AllGames | bp::Option::NextStart));
		return page;
	}

	// ---- HD texture packs (ps5_texture_packs.h)

	void findTexturePacks() override
	{
		if (m_game_serial.empty())
		{
			bp::toast("Only a PlayStation 2 game with a serial has texture packs.");
			return;
		}
		if (PS5TexturePacks::Busy())
		{
			bp::toast("A texture pack is being downloaded: wait for it, or stop it.");
			return;
		}
		bp::toast("Looking for this game's HD texture pack...", 3.f);
		const std::string serial = m_game_serial;
		const u32 crc = m_game.crc;
		PS5TexturePacks::FindFor(serial, false, [this, serial, crc](std::vector<PS5TexturePacks::Pack> packs, std::string error) {
			if (packs.empty())
			{
				bp::message("No HD texture pack", error.empty() ?
					fmt::format("The collection has no pack for {}.", serial) :
					fmt::format("The list of the collection could not be had: {}.", error));
				return;
			}
			std::vector<std::string> names;
			for (const PS5TexturePacks::Pack& p : packs)
				names.push_back(fmt::format("{}   \xc2\xb7   {}", p.title, PS5TexturePacksLogic::SizeText(p.size)));
			bp::pick("Which pack?", names, 0, [this, packs, serial, crc](int choice) {
				if (choice < 0 || choice >= static_cast<int>(packs.size()))
					return;
				const PS5TexturePacks::Pack pack = packs[static_cast<size_t>(choice)];
				const std::vector<PS5TexturePacks::Place> places = PS5TexturePacks::Places();
				std::vector<std::string> where;
				for (const PS5TexturePacks::Place& place : places)
					where.push_back(place.name);
				bp::pick(fmt::format("Where? ({})", PS5TexturePacksLogic::SizeText(pack.size)), where, 0, [this, pack, places, serial, crc](int at) {
					if (at < 0 || at >= static_cast<int>(places.size()))
						return;
					const PS5TexturePacks::Place place = places[static_cast<size_t>(at)];
					const bool started = PS5TexturePacks::Start(pack, place, [this, serial, crc](bool ok, std::string sentence) {
						if (ok)
							UseTexturePacks(serial, crc);
						PS5UI::SetBackgroundNote(std::string());
						bp::message(ok ? "The texture pack is ready" : "The texture pack is not complete", sentence);
					});
					if (!started)
					{
						bp::toast("A texture pack is being downloaded: wait for it, or stop it.");
						return;
					}
					PS5UI::SetBackgroundNote("Downloading a texture pack");
					bp::toast(fmt::format("Downloading {} to {}. It is unpacked as it comes; this page shows how far.", pack.title, place.name), 6.f);
				});
			});
		});
	}

	std::string texturePackDoing() override { return PS5TexturePacks::Doing(); }

	void stopTexturePack() override
	{
		if (PS5TexturePacks::Busy())
		{
			PS5TexturePacks::Stop();
			bp::toast("Stopping the download...", 3.f);
		}
	}

	// A pack that is whole: the game's own "Load texture packs" on, and a game that runs takes it.
	void UseTexturePacks(const std::string& serial, u32 crc)
	{
		if (m_game_si && m_game_serial == serial)
		{
			setBool(bp::Own, "EmuCore/GS", "LoadTextureReplacements", true);
			SaveOwn();
		}
		else
		{
			INISettingsInterface si(VMManager::GetGameSettingsPath(serial, crc));
			si.Load();
			si.SetBoolValue("EmuCore/GS", "LoadTextureReplacements", true);
			Error error;
			if (si.Save(&error))
				PS5Platform::MakePublic(si.GetFileName().c_str(), false);
		}
		if (VMManager::HasValidVM() && VMManager::GetDiscSerial() == serial)
		{
			Host::RunOnCPUThread([]() {
				VMManager::ReloadGameSettings();
				MTGS::RunOnGSThread([]() {
					if (!g_gs_renderer || !GSConfig.LoadTextureReplacements)
						return;
					GSTextureReplacements::ReloadReplacementMap();
					g_gs_renderer->PurgeTextureCache(true, false, true);
				});
			});
		}
	}

	bp::Page patchesPage(bool cheats) override
	{
		bp::Page page{cheats ? "\xef\x83\xa7" : "\xef\x91\xa2", cheats ? "Cheats" : "Patches", {}};
		const char* const section = cheats ? Patch::CHEATS_CONFIG_SECTION : Patch::PATCHES_CONFIG_SECTION;
		const std::vector<Patch::PatchInfo>& list = cheats ? m_cheats : m_patches;
		if (cheats)
			page.rows.push_back(Toggle("Use cheats", "Enables loading cheats from pnach files.", "EmuCore", "EnableCheats", false));
		page.rows.push_back(Toggle("Those of other versions of the game too",
			"Also lists cheats and patches made for other versions of the game. They may not work.", "EmuCore",
			cheats ? "ShowCheatsForAllCRCs" : "ShowPatchesForAllCRCs", false));

		if (cheats)
		{
			page.rows.push_back(Action("Download cheats for this game",
				"Downloads cheats for this game from the cheat collections on GitHub.",
				[this] {
					bp::toast("Looking for this game's cheats...");
					ps5px_download_cheats(m_game_serial.c_str(), m_game.crc);
				},
				bp::Option::OneGame));
			page.rows.push_back(Action("Type a cheat in",
				"Enter a raw cheat code: a name first, then one code line at a time. Encrypted codes won't work.",
				[this] { TypeCheat(); }, bp::Option::OneGame));
			if (!TypedCheats().empty())
			{
				page.rows.push_back(Action("Take a typed cheat out", "Removes a cheat you typed in.", [this] {
					const std::vector<std::string> names = TypedCheats();
					bp::pick("Take out which?", names, -1, [this, names](int choice) {
						if (choice < 0 || choice >= static_cast<int>(names.size()))
							return;
						const std::string path = TypedCheatsPath();
						const std::string text = FileSystem::ReadFileToString(path.c_str()).value_or(std::string());
						if (!FileSystem::WriteStringToFile(path.c_str(), PS5Cheats::WithoutCheat(text, names[static_cast<size_t>(choice)])))
						{
							bp::toast("The cheat file could not be written.");
							return;
						}
						bp::toast(fmt::format("\"{}\" was taken out.", names[static_cast<size_t>(choice)]), 3.f);
						PS5UI::CheatsChanged();
					});
				}, bp::Option::OneGame));
			}
		}
		if (list.empty())
		{
			page.rows.push_back(Line(cheats ? "No cheats are known for this game" : "No patches are known for this game", ""));
		}
		else
		{
			page.rows.push_back(Heading(cheats ? "CHEATS" : "PATCHES"));
			for (const Patch::PatchInfo& info : list)
			{
				bp::Option o = Toggle(info.name, info.description, section, info.name, false, bp::Option::OneGame);
				if (cheats)
					o.enabled = [this](int) { return Effective("EmuCore", "EnableCheats", false); };
				page.rows.push_back(std::move(o));
			}
		}
		if (cheats && m_unlabelled > 0)
			page.rows.push_back(Line(fmt::format("{} codes without a name", m_unlabelled), "applied whenever cheats are on"));
		if (!list.empty() || (cheats && m_unlabelled > 0))
			page.rows.push_back(Line("At your own risk", "they can break a game or its saves; the PCSX2 team gives no support with them on"));
		return page;
	}

	// ---- a cheat typed in on the keyboard (ps5_cheats_logic.h): its name, then its lines one by
	// one until a line with nothing typed; kept in a file of its own beside the downloaded ones.
	std::string TypedCheatsPath() const
	{
		return Path::Combine(EmuFolders::Cheats, PS5Cheats::TypedFileName(m_game_serial, m_game.crc));
	}

	std::vector<std::string> TypedCheats() const
	{
		if (m_game.crc == 0)
			return {};
		const std::string path = TypedCheatsPath();
		if (!FileSystem::FileExists(path.c_str()))
			return {};
		return PS5Cheats::CheatNames(FileSystem::ReadFileToString(path.c_str()).value_or(std::string()));
	}

	void TypeCheat()
	{
		if (m_game.crc == 0)
		{
			bp::toast("This game has no CRC to keep a cheat by.");
			return;
		}
		bp::editText("The cheat's name", std::string(), false, [this](const std::string& typed) {
			const std::string name = PS5Cheats::TypedCheatName(typed);
			if (name.empty())
			{
				bp::toast("A cheat needs a name: none was added.", 4.f);
				return;
			}
			m_typed_name = name;
			m_typed_lines.clear();
			TypeCheatLine();
		}, 60);
	}

	void TypeCheatLine()
	{
		bp::editText(fmt::format("Line {} of \"{}\"  (nothing typed: the code is complete)", m_typed_lines.size() + 1, m_typed_name), std::string(), false,
			[this](const std::string& typed) {
				if (PS5Cheats::TrimLine(typed).empty())
				{
					FinishTypedCheat();
					return;
				}
				const std::string line = PS5Cheats::TypedCodeLine(typed);
				if (line.empty())
					bp::toast("That is not a line of a code: two groups of eight digits and letters A to F. Type it again.", 5.f);
				else
					m_typed_lines.push_back(line);
				if (m_typed_lines.size() >= 200)
					FinishTypedCheat();
				else
					TypeCheatLine();
			}, 20);
	}

	void FinishTypedCheat()
	{
		if (m_typed_lines.empty())
		{
			bp::toast("No line was typed: no cheat was added.", 4.f);
			return;
		}
		const std::string path = TypedCheatsPath();
		const std::string text = FileSystem::ReadFileToString(path.c_str()).value_or(std::string());
		if (!FileSystem::WriteStringToFile(path.c_str(), PS5Cheats::WithTypedCheat(text, m_typed_name, m_typed_lines)))
		{
			bp::toast("The cheat could not be saved (crux2-boot.log may say why).", 5.f);
			ps5px_log("cheats: %s could not be written", path.c_str());
			return;
		}
		PS5Platform::MakePublic(path.c_str(), false);
		ps5px_log("cheats: \"%s\" typed in for %s, %zu line(s)", m_typed_name.c_str(), m_game_serial.c_str(), m_typed_lines.size());
		bp::toast(fmt::format("\"{}\" was added with {} line{}. Switch it on in the list, with Use cheats on.", m_typed_name, m_typed_lines.size(),
			m_typed_lines.size() == 1 ? "" : "s"), 6.f);
		m_typed_lines.clear();
		PS5UI::CheatsChanged();
	}

	// Backups of the memory cards and the saved states, on a USB drive or a share (ps5_backup.cpp).
	// Nothing here waits for a network: the list of backups is read on the jobs' thread, and the
	// page is made again when it has come.
	bp::Page backupPage() override
	{
		bp::Page page{"\xef\x83\xae", "Backup", {}};
		page.rows.push_back(Heading("MEMORY CARDS AND SAVED STATES"));
		const std::vector<PS5Jobs::Place> places = PS5Jobs::Places();
		if (places.empty())
		{
			page.rows.push_back(Line("Nowhere to back up to", "Put a USB drive in, or name a share in network.cfg"));
			if (!PS5Platform::IsElevated())
				page.rows.push_back(Line("USB drives", PS5Platform::WantsUSB() ? "Not reachable: the title could not leave its sandbox" : "Switched off (Interface)"));
			return page;
		}
		const PS5Jobs::Place place = PS5Jobs::BackupPlace();
		{
			bp::Option where;
			where.kind = bp::Option::StringList;
			where.label = "Backups go to";
			where.desc = "Where backups are saved. A network share must allow writing.";
			where.section = "PS5";
			where.key = "BackupPlace";
			where.defString = places.front().path;
			where.flags = bp::Option::AllGames;
			for (const PS5Jobs::Place& one : places)
			{
				where.names.push_back(one.name);
				where.strings.push_back(one.path);
			}
			page.rows.push_back(std::move(where));
		}
		page.rows.push_back(Toggle("Saved states too", "Includes save states in backups. Off: only memory cards are backed up, which is much faster.", "PS5", "BackupStates", true,
			bp::Option::AllGames));
		page.rows.push_back(Toggle("Back up the memory cards when a game is closed",
			"Backs up memory cards that changed when you close a game. The 10 newest automatic backups are kept.", "PS5", "AutoBackup", false, bp::Option::AllGames));
		page.rows.push_back(Toggle("Each backup replaces the one before it",
			"Deletes older backups when a new one is made, so the backup folder doesn't fill up.",
			"PS5", "BackupReplace", false, bp::Option::AllGames));
		{
			bp::Option now = Action("Back up now",
				"Backs up all memory cards and save states now.",
				[] { PS5Jobs::BackupNow(); }, bp::Option::AllGames | bp::Option::NoGame);
			now.info = [] { return PS5Backup::Doing(); };
			page.rows.push_back(std::move(now));
		}

		// Both ways, unlike the backups (ps5_card_sync.h).
		page.rows.push_back(Heading("MEMORY CARDS IN SYNC"));
		page.rows.push_back(Toggle("Keep the memory cards in sync",
			"Keeps memory cards in sync with the backup location each time Crux2 starts and after each game. If a card "
				"changed on both sides, the newer one is kept and the other is set aside. PCSX2 on PC and ARMSX2 on Android can "
				"use the same folder.",
			"PS5", "CardSync", false, bp::Option::AllGames));
		page.rows.push_back(Action("Sync the memory cards now", "Syncs the memory cards now.",
			[] { PS5Jobs::SyncCards(false); }, bp::Option::AllGames | bp::Option::NoGame));

		std::string upper = place.name;
		for (char& c : upper)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		page.rows.push_back(Heading("BACKUPS ON " + upper));
		const PS5Backup::State state = PS5Backup::CurrentState();
		if (!state.listed || state.listed_place != place.path)
		{
			// Asked for once for a place (and again by "Look again"): a share that does not answer
			// is not asked on every frame.
			if (m_backup_asked != place.path && !state.busy)
			{
				m_backup_asked = place.path;
				PS5Jobs::ListBackups();
			}
			page.rows.push_back(Line(m_backup_asked == place.path ? "Looking for backups there..." : "Not looked for yet", ""));
		}
		else if (!state.list_error.empty())
		{
			page.rows.push_back(Line("The place could not be read", state.list_error));
		}
		else if (state.snapshots.empty())
		{
			page.rows.push_back(Line("None yet", ""));
		}
		else
		{
			size_t shown = 0;
			for (const PS5Backup::Snapshot& snapshot : state.snapshots)
			{
				if (++shown > 40)
					break;
				char when[64] = "";
				struct tm local;
				if (localtime_r(&snapshot.when, &local) != nullptr)
					strftime(when, sizeof(when), "%e %B %Y, %H:%M", &local);
				const std::string date = (when[0] == ' ') ? std::string(when + 1) : std::string(when);
				if (!snapshot.complete)
				{
					page.rows.push_back(Line(date, "Not finished: it cannot be restored"));
					continue;
				}
				std::string holds = fmt::format("{} memory card file{}", snapshot.card_files, snapshot.card_files == 1 ? "" : "s");
				if (snapshot.states > 0)
					holds += fmt::format(", {} saved state{}", snapshot.states, snapshot.states == 1 ? "" : "s");
				holds += fmt::format(", {}", SizeText(snapshot.bytes));
				bp::Option row = Action(date,
					(snapshot.automatic ? "Made when a game was closed. Holds " : "Holds ") + holds + ". Cross copies it back to the console",
					[snapshot, date] {
						bp::confirm("Restore the backup of " + date + "?",
							"Its memory cards and saved states are copied back to the console, in place of the ones of the same name.\n\n"
							"What they replace is not lost: it is moved to the folder backup-before-restore in Crux2's folder. Cards and states "
							"the backup does not have are left alone.",
							[snapshot](bool yes) {
								if (yes)
									PS5Jobs::Restore(snapshot);
							},
							"Restore it", "Leave it");
					},
					bp::Option::AllGames | bp::Option::NoGame);
				// (On the row's right, where there is room for a few words: what kind it is, or its size.)
				row.info = [text = snapshot.automatic ? std::string("Automatic") : SizeText(snapshot.bytes)] { return text; };
				page.rows.push_back(std::move(row));
			}
			if (state.snapshots.size() > 40)
				page.rows.push_back(Line(fmt::format("And {} older ones", state.snapshots.size() - 40), "the place has them all"));
		}
		page.rows.push_back(Action("Look again", "Refreshes the list of backups.",
			[this] {
				m_backup_asked.clear();
				m_pages++;
				PS5Backup::StartList(PS5Jobs::BackupPlace().path);
				bp::toast("Looking for backups...", 3.f);
			},
			bp::Option::AllGames));
		return page;
	}

	// The logs, to read on the screen and to take along (ps5_logs.cpp).
	// RetroAchievements: the account, and what PCSX2 lets one choose. The name and the password
	// are typed on the interface's keyboard and handed to PCSX2's own sign-in, which keeps the
	// token RetroAchievements answers with (inis/secrets.ini), never the password.
	bp::Page achievementsPage() override
	{
		bp::Page page{"\xef\x82\x91", "Achievements", {}};
		const std::string user = Host::GetBaseStringSettingValue("Achievements", "Username", "");
		bool token = false;
		{
			const auto lock = Host::GetSecretsSettingsLock();
			std::string value;
			token = Host::Internal::GetSecretsSettingsLayer()->GetStringValue("Achievements", "Token", &value) && !value.empty();
		}
		const bool signed_in = !user.empty() && token;

		page.rows.push_back(Heading("RETROACHIEVEMENTS"));
		page.rows.push_back(Line("Account", signed_in ? user : std::string("Not signed in")));
		if (signed_in)
		{
			page.rows.push_back(Action("Sign out", "Signs out of RetroAchievements on this console.",
				[] {
					Host::RunOnCPUThread([]() {
						Achievements::Logout();
						ps5px_log("achievements: signed out");
						PS5UI::Toast("Signed out of RetroAchievements.");
						PS5UI::PagesChanged();
					});
				},
				bp::Option::AllGames | bp::Option::NoGame));
		}
		else
		{
			page.rows.push_back(Action("Sign in",
				"Sign in with your retroachievements.org username and password. Your password is not stored.",
				[] {
					if (s_signing_in.load(std::memory_order_acquire))
					{
						bp::toast("Still signing in...");
						return;
					}
					bp::editText("Your user name at RetroAchievements", "", false, [](const std::string& name) {
						if (name.empty())
							return;
						bp::editText("The password of " + name, "", true,
							[name](const std::string& password) {
								if (password.empty())
									return;
								SignIn(name, password);
							},
							96);
					});
				},
				bp::Option::AllGames | bp::Option::NoGame));
		}
		page.rows.push_back(Toggle("Achievements",
			"When enabled and logged in, PCSX2 will scan for achievements on startup.", "Achievements",
			"Enabled", false, bp::Option::AllGames | bp::Option::NoGame));

		page.rows.push_back(Heading("WHILE PLAYING"));
		page.rows.push_back(Toggle("Say when an achievement is earned", "Displays popup messages on events such as achievement unlocks and game completion.", "Achievements", "Notifications", true,
			bp::Option::AllGames));
		page.rows.push_back(Toggle("Say when a leaderboard attempt starts and ends", "Displays popup messages when starting, submitting, or failing a leaderboard challenge.", "Achievements", "LeaderboardNotifications", true,
			bp::Option::AllGames));
		page.rows.push_back(Toggle("Show achievements in progress",
			"Shows icons in the screen when a challenge/primed achievement is active.", "Achievements", "Overlays", true,
			bp::Option::AllGames));
		page.rows.push_back(Toggle("Show a leaderboard's time or score as it runs", "Shows icons in the screen when leaderboard tracking is active.", "Achievements", "LBOverlays", true, bp::Option::AllGames));
		{
			bp::Option o;
			o.kind = bp::Option::IntRange;
			o.label = "How long a notice stays";
			o.section = "Achievements";
			o.key = "NotificationsDuration";
			o.defInt = 5;
			o.lo = 3;
			o.hi = 30;
			o.step = 1;
			o.unit = " s";
			o.flags = bp::Option::AllGames;
			page.rows.push_back(o);
			o.label = "How long a leaderboard's notice stays";
			o.key = "LeaderboardsDuration";
			o.defInt = 10;
			page.rows.push_back(std::move(o));
		}
		{
			// (The emulator's numbers: a notice's place counts from 1 with "not shown" before it,
			// the pictures' place from 0.)
			static const std::vector<std::string> places = {"Top left", "Top centre", "Top right", "Centre left", "Centre", "Centre right",
				"Bottom left", "Bottom centre", "Bottom right"};
			bp::Option o;
			o.kind = bp::Option::IntList;
			o.label = "Where the notices are shown";
			o.desc = "Where achievement and leaderboard notifications appear on screen.";
			o.section = "Achievements";
			o.key = "NotificationPosition";
			o.names = places;
			o.ints = {1, 2, 3, 4, 5, 6, 7, 8, 9};
			o.defInt = 1;
			o.flags = bp::Option::AllGames;
			page.rows.push_back(o);
			o.label = "Where the small pictures are shown";
			o.desc = "Where achievement progress and leaderboard trackers appear on screen.";
			o.key = "OverlayPosition";
			o.ints = {0, 1, 2, 3, 4, 5, 6, 7, 8};
			o.defInt = 8;
			page.rows.push_back(std::move(o));
		}

		page.rows.push_back(Heading("MORE"));
		page.rows.push_back(Toggle("Encore mode", "When enabled, each session will behave as if no achievements have been unlocked.",
			"Achievements", "EncoreMode", false, bp::Option::AllGames | bp::Option::NoGame));
		page.rows.push_back(Toggle("Spectator mode", "When enabled, PCSX2 will assume all achievements are locked and not send any unlock notifications to the "
			"server.",
			"Achievements", "SpectatorMode", false, bp::Option::AllGames | bp::Option::NoGame));
		page.rows.push_back(Toggle("Unofficial achievements", "When enabled, PCSX2 will list achievements from unofficial sets. Please note that these achievements are not "
			"tracked by RetroAchievements, so they unlock every time.", "Achievements",
			"UnofficialTestMode", false, bp::Option::AllGames | bp::Option::NoGame));
		page.rows.push_back(Heading("SOUND EFFECTS"));
		page.rows.push_back(Toggle("Sound effects", "Plays sound effects for events such as achievement unlocks and leaderboard submissions.",
			"Achievements", "SoundEffects", true, bp::Option::AllGames));
		page.rows.push_back(Toggle("Achievement unlock sound", "", "Achievements", "UnlockSound", true, bp::Option::AllGames));
		page.rows.push_back(Toggle("Leaderboard submit sound", "", "Achievements", "LBSubmitSound", true, bp::Option::AllGames));
		page.rows.push_back(Toggle("Notification sound", "", "Achievements", "InfoSound", true, bp::Option::AllGames));
		return page;
	}

	bp::Page logPage() override
	{
		bp::Page page{"\xef\x85\x9c", "Log", {}};
		page.rows.push_back(Heading("WHAT CRUX2 WROTE DOWN"));
		const auto show = [](const char* title, std::string path) {
			return [title, path = std::move(path)] {
				std::vector<std::string> lines = PS5Logs::Tail(path, 800, 2000);
				if (lines.empty())
					bp::toast("There is no such log (yet).");
				else
					bp::showText(title, std::move(lines));
			};
		};
		const PS5Logs::Files files = PS5Jobs::LogFiles();
		page.rows.push_back(Action("This run",
			"Shows the log of this session.",
			show("The log of this run", files.now), bp::Option::AllGames));
		{
			bp::Option before = Action("The run before",
				"Shows the log of the previous session, including a crash report if it crashed.",
				show("The log of the run before", files.previous), bp::Option::AllGames);
			if (!m_crash_notice.empty())
				before.info = [] { return std::string("Ended in a crash"); };
			page.rows.push_back(std::move(before));
		}
		page.rows.push_back(Action("The emulator's own log", "Shows the newest lines of PCSX2's own log (logs/emulog.txt).",
			show("PCSX2's log of this run", files.emulator), bp::Option::AllGames));

		page.rows.push_back(Heading("TAKING THEM ALONG"));
		const std::vector<PS5Jobs::Place> places = PS5Jobs::Places();
		for (const PS5Jobs::Place& place : places)
		{
			page.rows.push_back(Action(fmt::format("Copy the logs to {}", place.name),
				"Puts the three logs in a folder named Crux2-logs there, to read on a computer or to pass on with a description of what went wrong",
				[place] {
					bp::toast("Copying the logs...", 3.f);
					PS5Jobs::CopyLogs(place);
				},
				bp::Option::AllGames));
		}
		if (places.empty())
			page.rows.push_back(Line("Copy the logs", "Put a USB drive in, or name a share in network.cfg; then open this page again"));
		page.rows.push_back(Line("On the console", Shown(ps5px_title_dir()) + "crux2-boot.log"));
		return page;
	}

	// What PCSX2's game database recommends for the open game and does not set by itself: the
	// blending accuracy first, accurate alpha test and AA1 (PCSX2 only says so on the screen when a
	// game starts), and its own fixes where they were switched off.
	std::vector<bp::Option> gameRows() override
	{
		std::vector<bp::Option> rows;
		if (!m_game_si || m_game_serial.empty())
			return rows;
		const Recommendation found = FindRecommended();
		bp::Option row = Action("Recommended settings",
			found.known ? "Sets for this game what PCSX2's game database recommends for it (the blending accuracy first), and says what it changed" :
			              "PCSX2's game database does not know this game",
			[this] { ApplyRecommended(); }, bp::Option::OneGame);
		std::string text = !found.known ? std::string("Not in the database") : found.changes.empty() ? std::string("Nothing to change") :
			fmt::format("{} change{}", found.changes.size(), found.changes.size() == 1 ? "" : "s");
		row.info = [text = std::move(text)] { return text; };
		rows.push_back(std::move(row));
		{
			// Where this game's texture pack is: the title folder and the USB drives are looked at
			// (a share is asked only when the game starts; what it was given then is known).
			const std::string place = PS5Extras::TexturePackPlace(m_game_serial, false);
			const bool on = Effective("EmuCore/GS", "LoadTextureReplacements", false);
			bp::Option pack;
			pack.kind = bp::Option::Info;
			pack.label = "Texture pack";
			pack.desc = fmt::format("Put replacement textures for this game in textures/{}/replacements, in the title folder or in a Crux2 or "
									"PS2 folder on a USB drive or network share. Turn on \"Load textures\" in Graphics to use them.",
				m_game_serial);
			std::string text = place.empty() ? std::string("None found (a share is looked at when the game starts)") :
			                   on            ? "Found: " + Shown(place) :
			                                   "Found, but \"Load replacement textures\" is off: " + Shown(place);
			pack.info = [text = std::move(text)] { return text; };
			pack.flags = bp::Option::OneGame;
			rows.push_back(std::move(pack));
		}
		return rows;
	}

	bp::Page aboutPage() override
	{
		bp::Page page{"\xef\x81\x9a", "About", {}};
		page.rows.push_back(Line("Crux2", *ps5px_build_version() != '\0' ?
			fmt::format("{}, a work in progress, built {}", ps5px_build_version(), ps5px_build_stamp()) :
			fmt::format("A work in progress, built {}", ps5px_build_stamp())));
		page.rows.push_back(Line("Made by", "Press5elect. Crux2 by Press5elect: Copyright (C) 2026 Press5elect"));
		page.rows.push_back(Line("Emulator", fmt::format("PCSX2 {}, by the PCSX2 Dev Team", GIT_REV)));
		page.rows.push_back(Line("Graphics", "Vulkan, by Mesa's RADV driver built into the title"));
		page.rows.push_back(Line("Interface", "Big Picture, from PSFlyCast by Press5elect"));
		page.rows.push_back(Line("Build", ps5px_build_identity()));
		page.rows.push_back(Line("Licence", "GPL-3.0 or later, with credit to Press5elect required (NOTICE.txt). LEGAL.txt and licenses/ in the title "
											 "folder name every part"));
		page.rows.push_back(Line("Log", Shown(ps5px_title_dir()) + "crux2-boot.log"));
		page.rows.push_back(Line("Crux2's files", PS5Platform::OutsideFolder().empty() ?
			fmt::format("{} (a folder /data/crux2 is used in its place when there is one)", Shown(EmuFolders::DataRoot)) :
			fmt::format("{}, outside the title folder", Shown(EmuFolders::DataRoot))));
		page.rows.push_back(Action("Quit Crux2", "Closes Crux2. A running game is closed first.", [this] { quit(); }));
		return page;
	}

	// ---- what the console adds (bp.h, Backend's last part)

	std::vector<bp::Option> portRows() override { return {}; }

	std::vector<bp::Option> extraRows(const char* where, int layer) override
	{
		const std::string_view w = where;
		std::vector<bp::Option> rows;
		const unsigned A = bp::Option::AllGames;
		if (w == "library")
		{
			rows.push_back(Heading("WHERE GAMES ARE"));
			rows.push_back(Toggle("USB drives",
				"Shows a USB tab for games in a folder named Crux2 or PS2 on a USB drive.", "PS5",
				"USBDrives", true, A | bp::Option::Restart));
			rows.push_back(Toggle("Disc drives",
				"Checks the console's disc drive and USB DVD drives for PS2 discs and shows them on their own tab. USB DVD "
					"drives can also read PS2 CDs.",
				"PS5", "DiscDrive", true, A | bp::Option::Restart));
			rows.push_back(Toggle("Read the disc into memory while playing",
				"Copies the whole disc into memory while you play, so the drive is no longer needed. Off: only recently read "
					"data is kept.",
				"PS5", "DiscToMemory", true, A | bp::Option::NextStart));
			rows.push_back(Toggle("Keep the disc turning",
				"Reads from the disc every 20 seconds so the drive doesn't spin down during play.",
				"PS5", "DiscKeepSpinning", true, A | bp::Option::NextStart));
			{
				bp::Option image = Action("Make an image of the disc",
					"Copies your disc to an ISO, CHD or CSO image on the console, a USB drive or a network share.",
					[this] { CopyDiscToImage(); }, A | bp::Option::NoGame);
				image.enabled = [](int) { return PS5Disc::Current().ps2 && !s_disc_copying.load(std::memory_order_acquire); };
				rows.push_back(std::move(image));
			}
			rows.push_back(Action("Eject a disc",
				"Unlocks and opens a disc drive's tray.",
				[this] { EjectDisc(); }, A | bp::Option::NoGame));
			rows.push_back(Action("Share the covers",
				"Syncs covers both ways with the backup location, so all your devices have them.",
				[] { PS5Jobs::ShareCovers(); }, A | bp::Option::NoGame));
			rows.push_back(Heading("FROM THE INTERNET"));
			rows.push_back(Toggle("Download covers automatically",
				"Downloads missing covers from libretro's collection after each scan.", "PS5", "AutoCovers",
				true, A));
			rows.push_back(Toggle("Fetch game pictures automatically",
				"Downloads title screens and screenshots for your games after each scan.", "PS5", "AutoPictures",
				true, A));
			rows.push_back(Toggle("Look up what each game is",
				"Shows a short description of the game in the drive, looked up on Wikipedia.",
				"PS5", "FetchAbout", true, A));
			rows.push_back(Action("Look the games up again", "Clears the stored game descriptions so they are looked up again.",
				[] {
					PS5About::Forget();
					bp::toast("Each game is looked up again when it is shown.");
				},
				A | bp::Option::NoGame));
			rows.push_back(Action("Download the missing covers now", "Tries again to download covers for every game without one.",
				[] {
					bp::toast("Looking for games without a cover...");
					ps5px_download_covers_now();
				},
				A | bp::Option::NoGame));
			rows.push_back(Action("Download cheats for all games",
				"Downloads cheats for all your games from the cheat collections on GitHub. Cheats stay off until you enable "
					"them for a game.",
				[] {
					bp::toast("Looking for your games' cheats...");
					ps5px_download_all_cheats();
				},
				A | bp::Option::NoGame));
			{
				bp::Option patches = Action("Download PCSX2's patches",
					"Downloads PCSX2's game patches (widescreen, no-interlacing, 60 fps and more) from GitHub.",
					[] {
						bp::toast("Downloading PCSX2's patches...");
						PS5Jobs::DownloadPatches();
					},
					A | bp::Option::NoGame);
				patches.info = [text = PS5Patches::Describe()] { return text; };
				rows.push_back(std::move(patches));
			}
		}
		else if (w == "interface")
		{
			rows.push_back(Heading("SOUND"));
			{
				bp::Option set;
				set.kind = bp::Option::IntList;
				set.label = "Interface sounds";
				set.desc = "Plays sounds when you move and select in the menus.";
				set.section = "PS5UI";
				set.key = "SoundSet";
				set.names = {"Off", "Glass", "Paper"};
				set.ints = {0, 1, 2};
				set.defInt = 1;
				set.flags = bp::Option::Front;
				rows.push_back(std::move(set));
			}
			{
				bp::Option v = Volume("Interface sounds' volume", "SoundVolume", 80);
				v.section = "PS5UI";
				v.flags = bp::Option::Front;
				rows.push_back(std::move(v));
			}
			rows.push_back(Toggle("Music",
				"Plays your own music in the menus. Put WAV, MP3, OGG or FLAC files in the \"music\" folder.",
				"PS5", "BackgroundSound", true, A));
			rows.push_back(Volume("Music's volume", "BackgroundVolume", 40));
			{
				bp::Option order;
				order.kind = bp::Option::IntList;
				order.label = "Order of the music";
				order.section = "PS5UI";
				order.key = "MusicOrder";
				order.names = {"As the files are named", "Shuffled"};
				order.ints = {0, 1};
				order.defInt = 1;
				order.flags = bp::Option::Front;
				rows.push_back(std::move(order));
			}
			rows.push_back(Action("Next track", "", [] { PS5UIAudio::NextTrack(); }, A));
			rows.push_back(Toggle("Start-up sound",
				"Plays a sound with the start-up animation. Put your own startup.wav in the title folder to replace it.", "PS5", "StartupSound",
				true, A));
			rows.push_back(Volume("Start-up sound's volume", "StartupVolume", 80));
		}
		else if (w == "motion")
		{
			bp::Option rate;
			rate.kind = bp::Option::IntList;
			rate.label = "Refresh rate";
			rate.desc = "Sends 120 Hz to the TV if it supports it. Needed for frame generation to 120 fps and black frame insertion. "
				"Hold L1 and R1 while Crux2 starts to use 60 Hz once.";
			rate.section = "PS5";
			rate.key = "OutputRefresh";
			rate.names = {"120 Hz when the television has it", "60 Hz"};
			rate.ints = {120, 60};
			rate.defInt = 60;
			rate.flags = A | bp::Option::Restart;
			rows.push_back(std::move(rate));
			{
				bp::Option vrr = Toggle("Variable refresh (VRR)",
					"With 120 Hz on a TV that has VRR, each frame is shown as soon as it is ready instead of at the next fixed "
					"refresh: smoother when a game drops below full speed. Takes effect the next time Crux2 starts. Experimental.",
					"PS5", "VariableRefresh", false, A | bp::Option::Restart);
				vrr.enabled = [](int) { return Host::GetBaseIntSettingValue("PS5", "OutputRefresh", 60) > 60; };
				rows.push_back(std::move(vrr));
			}
		}
		else if (w == "output")
		{
			{
				bp::Option size;
				size.kind = bp::Option::IntList;
				size.label = "Output resolution";
				size.desc = "The resolution sent to the TV. Lower it on a 1080p or 1440p TV to save GPU power.";
				size.section = "PS5";
				size.key = "OutputHeight";
				size.names = {"4K (3840 x 2160)", "1440p (2560 x 1440)", "1080p (1920 x 1080)"};
				size.ints = {2160, 1440, 1080};
				size.defInt = 2160;
				size.flags = A | bp::Option::Restart;
				rows.push_back(std::move(size));
			}
		}
		else if (w == "picture")
		{
			rows.push_back(Action("A look of your own",
				"Loads a libretro shader preset (.slangp) from a \"shaders\" folder on a USB drive or in the title folder.",
				[this, layer] { PickOwnLook(layer); }));
		}
		else if (w == "controllers")
		{
			rows.push_back(Heading("THE CONSOLE'S CONTROLLERS"));
			{
				bp::Option combo;
				combo.kind = bp::Option::StringList;
				combo.label = "Leave a game by holding";
				combo.desc = "Hold these buttons to leave the game.";
				combo.section = "PS5";
				combo.key = "LeaveGameCombo";
				combo.names = {"L3 and R3", "L1, R1, L3 and R3", "Nothing (off)"};
				combo.strings = {"L3R3", "L1R1L3R3", "Off"};
				combo.defString = "L3R3";
				combo.flags = A;
				rows.push_back(std::move(combo));
			}
			{
				bp::Option hold;
				hold.kind = bp::Option::FloatList;
				hold.label = "Held for";
				hold.section = "PS5";
				hold.key = "LeaveGameHoldSeconds";
				hold.names = {"1 second", "2 seconds", "3 seconds"};
				hold.floats = {1.f, 2.f, 3.f};
				hold.defFloat = 2.f;
				hold.flags = A;
				rows.push_back(std::move(hold));
			}
			rows.push_back(Toggle("Pressure from R2",
				"For games that use pressure-sensitive buttons: how far you pull R2 sets how hard the face buttons and D-pad "
					"are pressed.",
				"PS5", "PressureFromR2", false));
			rows.push_back(Toggle("Aim with motion",
				"Aims the GunCon 2 light gun by moving the controller. Press R3 to recentre.",
				"PS5", "MotionAim", false));
			{
				bp::Option sens;
				sens.kind = bp::Option::IntRange;
				sens.label = "Aiming: how far to turn to the edge";
				sens.desc = "How far you need to turn the controller to aim at the edge of the screen.";
				sens.section = "PS5";
				sens.key = "MotionAimSensitivity";
				sens.defInt = 20;
				sens.lo = 5;
				sens.hi = 60;
				sens.step = 1;
				sens.unit = "\xc2\xb0";
				sens.flags = A;
				rows.push_back(std::move(sens));
			}
			rows.push_back(Toggle("USB keyboard",
				"Lets a USB keyboard type in the menus, and act as a PS2 keyboard in games that support one.",
				"PS5", "USBKeyboard", true, A | bp::Option::Restart));
			rows.push_back(Toggle("USB controllers (experimental)",
				"Lets you use controllers plugged into the console's USB ports, such as a DualShock 3, Xbox 360 pads, guitars "
					"and drums. Experimental.",
				"PS5", "USBPads", false, A));
		}
		else if (w == "saves")
		{
			rows.push_back(Toggle("Sleep-safe saving",
				"Saves the game's state when you open the PS5 menu and every five minutes, so you can continue after the "
					"console goes to rest mode.",
				"PS5", "SleepSafe", true, A));
		}
		else if (w == "network")
		{
			rows.push_back(Heading("A PHONE OR A COMPUTER"));
			rows.push_back(Toggle("Control from a phone",
				"Opens a web page (port 3311) where you can start games, manage save states and type text from your phone or "
					"computer.",
				"PS5", "PhoneControl", false, A));
			{
				bp::Option code = Action("Show the code", "Shows the page's address as a QR code to scan with your phone.", [] {
					const std::string url = PS5Web::url();
					if (url.empty())
					{
						bp::message("Not on the network", PS5Web::listening() ? "The console's address is not known yet." :
																				  "Switch on \"Control from a phone\" first.");
						return;
					}
					const PS5Web::QrCode qr = PS5Web::qr(url);
					bp::showCode("Control Crux2 from a phone", "Open this on a phone or a computer on the same network:\n\n" + url +
						"\n\nWhoever has this address can control Crux2 while it runs. \"A new key\" locks the old address out.",
						qr.modules, qr.size);
				});
				code.enabled = [](int) { return PS5Web::listening(); };
				rows.push_back(std::move(code));
			}
			{
				bp::Option renew = Action("A new key", "Creates a new key. The old address stops working.", [] {
					PS5Web::newKey();
					bp::toast("A new key: show the code again for the new address.");
				});
				renew.enabled = [](int) { return PS5Web::listening(); };
				rows.push_back(std::move(renew));
			}
		}
		else if (w == "system")
		{
			rows.push_back(Heading("UPDATES"));
			{
				bp::Option look = Action("Look for a newer Crux2", "Checks GitHub for a newer version. Nothing is downloaded without asking.", [] {
					s_update_asked_by_hand.store(true, std::memory_order_release);
					PS5Update::Check();
					bp::toast("Looking for a newer Crux2...", 3.f);
				}, A);
				look.info = [] { return UpdateLine(); };
				rows.push_back(std::move(look));
			}
			rows.push_back(Toggle("Look for a newer Crux2 at start-up",
				"Checks GitHub for a newer version each time Crux2 starts. Nothing is downloaded without asking.", "PS5",
				"UpdateAtStart", true, A));
			{
				bp::Option news = Action("What's new", "Shows what changed in newer versions.", [] {
					const PS5Update::Status st = PS5Update::GetStatus();
					if (st.notes.empty())
					{
						bp::message("What's new", st.state == PS5Update::State::UpToDate ? "This is the newest Crux2." :
																						  "Look for a newer Crux2 first.");
						return;
					}
					std::vector<std::string> lines;
					size_t at = 0;
					while (at <= st.notes.size())
					{
						const size_t end = st.notes.find('\n', at);
						lines.push_back(st.notes.substr(at, end == std::string::npos ? std::string::npos : end - at));
						if (end == std::string::npos)
							break;
						at = end + 1;
					}
					bp::showText("What's new", std::move(lines));
				}, A);
				rows.push_back(std::move(news));
			}
			{
				bp::Option again = Action("Install this version again",
					"Reinstalls the newest version to repair missing or broken files. Your settings, saves and games are kept.",
					[] {
						bp::confirm("Install again?", "The newest version will be downloaded and installed, then Crux2 will close.",
							[](bool yes) {
								if (yes)
								{
									s_update_wanted.store(true, std::memory_order_release);
									PS5Update::Reinstall();
								}
							},
							"Install again", "Not now");
					},
					A | bp::Option::NoGame);
				again.enabled = [](int) {
					const PS5Update::Status st = PS5Update::GetStatus();
					return st.state == PS5Update::State::UpToDate && st.reinstall;
				};
				rows.push_back(std::move(again));
			}
			rows.push_back(Heading("THE CONSOLE"));
			rows.push_back(Toggle("Graphics driver on its own thread",
				"Runs the graphics driver on its own CPU thread. Usually faster in demanding games.", "PS5",
				"ThreadedRecording", true, A | bp::Option::Advanced | bp::Option::Restart));
			rows.push_back(Range("Wait for the GPU: checking for",
				"How long the driver keeps checking whether the GPU is done before it starts sleeping between checks.", "PS5", "GpuWaitSpinUs", 1500, 0, 5000, 250,
				" us", A | bp::Option::Advanced | bp::Option::Restart));
			rows.push_back(Range("Wait for the GPU: sleeps of",
				"How long the driver sleeps between checks while waiting for the GPU.", "PS5", "GpuWaitSleepUs", 1000, 50, 2000, 50, " us", A | bp::Option::Advanced | bp::Option::Restart));
			rows.push_back(Action("Profile the next start",
				"Records detailed performance data on the next start and writes it to crux2-boot.log. Useful for finding out "
					"why a game is slow.",
				[] {
					bp::toast(ps5px_arm_profiled_start(0) ? "The next start is a profiled one. Quit Crux2 and start it again." :
															"The file for it could not be written to the title folder.",
						8.f);
				},
				A | bp::Option::Advanced));
		}
		return rows;
	}

	void cue(int which) override
	{
		if (which >= 0 && which < static_cast<int>(PS5UIAudio::CueId::Count))
			PS5UIAudio::Cue(static_cast<PS5UIAudio::CueId>(which));
	}

	bool typed(std::string& text, unsigned& keys) override
	{
		text.clear();
		keys = 0;
		const PS5KeyboardLogic::Typed k = PS5Keyboard::Poll();
		text = k.text;
		keys |= k.enter ? 1u : 0u;
		keys |= k.backspace ? 2u : 0u;
		keys |= k.escape ? 4u : 0u;
		{
			const std::lock_guard lock(s_web_typed_mutex);
			text += s_web_typed;
			keys |= s_web_keys;
			s_web_typed.clear();
			s_web_keys = 0;
		}
		return !text.empty() || keys != 0;
	}

	void captureKeyboard(bool on) override { PS5Keyboard::SetTextCapture(on); }

	void frame() override
	{
		// L1 and R1 held as the title starts: a television that shows nothing at 120 Hz gets 60 Hz
		// from the next start (the screen is open by now: this start stays as it is).
		if (!m_safe_asked && m_frame < 150 && (pad().held & (bp::Pad::L1 | bp::Pad::R1)) == (bp::Pad::L1 | bp::Pad::R1))
		{
			m_safe_asked = true;
			if (Host::GetBaseIntSettingValue("PS5", "OutputRefresh", 60) != 60)
			{
				setInt(bp::Shared, "PS5", "OutputRefresh", 60);
				ps5px_log("display: L1 and R1 were held at start: 60 Hz from the next start");
				bp::message("60 Hz from the next start",
					"L1 and R1 were held while Crux2 started: the picture is 60 Hz from the next start. Settings > Motion > Refresh rate brings "
					"120 Hz back.");
			}
		}
		// The interface's sounds mix into the game's own output while a screen is over a game.
		PS5UIAudio::SetGameMixIn(bp::active() && VMManager::HasValidVM());
		// The track that plays, said once when it changes (with no game: the music is off then).
		const PS5UIAudio::TrackInfo track = PS5UIAudio::CurrentTrack();
		if (track.serial != m_track_said)
		{
			m_track_said = track.serial;
			if (!track.name.empty() && !VMManager::HasValidVM() && m_frame > 300)
				bp::toast(std::string(ICON_FA_MUSIC "  ") + track.name, 3.f);
		}
	}

	// Sleep-safe saving's note (PS5Extras): the console closed the title while a game was played.
	// The game is offered from where its state was last saved.
	void OfferToGoOn()
	{
		const std::string note_path = PS5Extras::InterruptedNotePath();
		const std::optional<std::string> note = FileSystem::ReadFileToString(note_path.c_str());
		if (!note.has_value())
			return;
		std::string game_path, state_path;
		for (const std::string_view line : StringUtil::SplitString(*note, '\n'))
		{
			if (line.starts_with("game="))
				game_path = std::string(line.substr(5));
			else if (line.starts_with("state="))
				state_path = std::string(line.substr(6));
		}
		FileSystem::DeleteFilePath(note_path.c_str());
		if (game_path.empty() || state_path.empty() || !FileSystem::FileExists(state_path.c_str()))
			return;
		bp::GameInfo game;
		bool listed = false;
		for (const bp::GameInfo& g : games())
		{
			if (g.path == game_path)
			{
				game = g;
				listed = true;
				break;
			}
		}
		if (!listed)
		{
			game.path = game_path;
			game.fileName = std::string(Path::GetFileName(game_path));
			game.title = std::string(Path::GetFileTitle(game_path));
		}
		ps5px_log("resume: the console closed the title while %s was played; going on is offered", game.title.c_str());
		bp::confirm("Go on where you were?",
			"Crux2 was closed by the console while " + game.title + " was played. Its state was saved just before: go on from there?",
			[this, game, state_path](bool yes) {
				if (yes)
					startStateFile(game, state_path);
			},
			"Go on", "Not now");
	}

	// The updater's state, a line for the row.
	static std::string UpdateLine()
	{
		const PS5Update::Status st = PS5Update::GetStatus();
		switch (st.state)
		{
			case PS5Update::State::Checking: return "Asking...";
			case PS5Update::State::UpToDate: return "This is the newest";
			case PS5Update::State::Available: return st.tag + " is out";
			case PS5Update::State::NoRelease: return "No release found";
			case PS5Update::State::Downloading:
				return st.total ? fmt::format("Downloading {}%", static_cast<int>(100.0 * st.done / st.total)) : std::string("Downloading");
			case PS5Update::State::Verifying: return "Checking the download";
			case PS5Update::State::Ready: return "Ready";
			case PS5Update::State::Installed: return "Installed";
			case PS5Update::State::Failed: return "Did not work";
			default: return std::string("v") + (ps5px_build_version()[0] == 'v' ? ps5px_build_version() + 1 : ps5px_build_version());
		}
	}

	// Once a frame (BeginFrame): what the updater came to, answered.
	void UpdateTick()
	{
		const PS5Update::Status st = PS5Update::GetStatus();
		if (st.state == m_update_state)
			return;
		m_update_state = st.state;
		switch (st.state)
		{
			case PS5Update::State::Available:
			{
				std::string body = st.notes;
				if (body.size() > 700)
					body = body.substr(0, 700) + "...\n(all of it under System > What's new)";
				bp::confirm(st.tag + " is out", body + "\n\nDownload and install it? Your settings, memory cards, states and games stay.",
					[](bool yes) {
						if (yes)
						{
							s_update_wanted.store(true, std::memory_order_release);
							PS5Update::Download();
						}
					},
					"Install", "Not now");
				break;
			}
			case PS5Update::State::UpToDate:
				if (s_update_asked_by_hand.exchange(false))
					bp::toast("This is the newest Crux2.");
				break;
			case PS5Update::State::Ready:
				if (s_update_wanted.load(std::memory_order_acquire) && VMManager::GetState() == VMState::Shutdown)
				{
					if (PS5Update::Install())
					{
						bp::message("Crux2 was updated", "The update is installed. Crux2 will now close; start it again.", [this] { quit(); });
					}
					else
					{
						bp::message("The update was not installed", PS5Update::GetStatus().error + "\nThe title is as it was.");
					}
				}
				break;
			case PS5Update::State::Failed:
				if (s_update_wanted.exchange(false))
					bp::message("The update did not work", st.error);
				break;
			default:
				break;
		}
	}

	// A look of the user's own: the presets in the places they are looked for, to pick from.
	void PickOwnLook(int layer)
	{
		std::vector<std::string> folders = {Path::Combine(ps5px_title_dir(), "shaders")};
		for (int i = 0; i < 8; i++)
		{
			for (const char* top : {"Crux2", "PS2", "crux2", "ps2"})
				folders.push_back(fmt::format("/mnt/usb{}/{}/shaders", i, top));
		}
		std::vector<std::string> found;
		for (const std::string& folder : folders)
		{
			if (!FileSystem::DirectoryExists(folder.c_str()))
				continue;
			for (std::string& preset : PS5Slang::FindPresets(folder, 4))
				found.push_back(std::move(preset));
			if (found.size() > 200)
				break;
		}
		if (found.empty())
		{
			bp::message("No preset found",
				"Put .slangp presets (with their .slang files, as libretro's slang-shaders has them) in a folder \"shaders\" inside a folder "
				"named Crux2 or PS2 at the top of a USB drive, or in the title folder.");
			return;
		}
		std::vector<std::string> names;
		for (const std::string& path : found)
			names.push_back(std::string(Path::GetFileTitle(path)) + "   (" + std::string(Path::GetFileName(Path::GetDirectory(path))) + ")");
		bp::pick("A look of your own", std::move(names), -1, [this, found, layer](int chosen) {
			if (chosen < 0 || chosen >= static_cast<int>(found.size()))
				return;
			setString(layer, "EmuCore/GS", "Look", found[static_cast<size_t>(chosen)]);
			bp::toast("The look is " + std::string(Path::GetFileTitle(found[static_cast<size_t>(chosen)])) + ". A look that cannot be run is said in the log.");
		});
	}

	// A disc in a drive as an image in the games folder, on a thread of its own. With games in more
	// than one drive, which one is asked.
	void CopyDiscToImage()
	{
		std::vector<PS5Disc::State> games;
		for (PS5Disc::State& state : PS5Disc::Drives())
		{
			if (state.present && state.ps2)
				games.push_back(std::move(state));
		}
		if (games.empty() || s_disc_copying.load(std::memory_order_acquire))
			return;
		if (games.size() == 1)
		{
			CopyDiscToImage(games.front());
			return;
		}
		std::vector<std::string> names;
		for (const PS5Disc::State& state : games)
		{
			std::string title = state.volume_label.empty() ? state.serial : state.volume_label;
			if (const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(state.serial))
				title = entry->name;
			names.push_back(title + "   (" + std::string(Path::GetFileName(state.device)) + (state.cd ? ", CD)" : ")"));
		}
		bp::pick("Which disc?", std::move(names), -1, [this, games](int chosen) {
			if (chosen >= 0 && chosen < static_cast<int>(games.size()))
				CopyDiscToImage(games[static_cast<size_t>(chosen)]);
		});
	}

	// A drive's tray opened; with several drives, which is asked.
	void EjectDisc()
	{
		const std::vector<PS5Disc::State> drives = PS5Disc::Drives();
		if (drives.empty())
		{
			bp::toast("No disc drive was found.");
			return;
		}
		const auto eject = [](const std::string& device) {
			std::string error;
			if (PS5Disc::Eject(device, &error))
				bp::toast("The tray of " + std::string(Path::GetFileName(device)) + " opens.");
			else
				bp::toast("Not ejected: " + error, 6.f);
		};
		if (drives.size() == 1)
		{
			eject(drives.front().device);
			return;
		}
		std::vector<std::string> names;
		for (const PS5Disc::State& state : drives)
		{
			std::string what = !state.present ? std::string("empty") : !state.serial.empty() ? state.serial : std::string("a disc");
			if (state.present && state.ps2)
			{
				if (const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(state.serial))
					what = entry->name;
			}
			names.push_back(std::string(Path::GetFileName(state.device)) + (state.device.ends_with("cd0") ? " (most likely the console's own)" : "") +
							"   " + what);
		}
		bp::pick("Eject which drive's disc?", std::move(names), -1, [drives, eject](int chosen) {
			if (chosen >= 0 && chosen < static_cast<int>(drives.size()))
				eject(drives[static_cast<size_t>(chosen)].device);
		});
	}

	// Where an image of a disc can go: the title's games folder, Crux2's own folder outside it, each
	// USB drive (its games folder, or a folder crux2 made at its top) and each network share, SMB
	// or NFS. (label, folder)
	std::vector<std::pair<std::string, std::string>> ImagePlaces()
	{
		std::vector<std::pair<std::string, std::string>> places;
		places.emplace_back("Internal storage (the title's games folder)", Path::Combine(ps5px_title_dir(), "games"));
		if (!PS5Platform::OutsideFolder().empty())
			places.emplace_back("Internal storage (Crux2's folder, games)", Path::Combine(PS5Platform::OutsideFolder(), "games"));
		// (USB drives are there outside the sandbox, or through the console's FTP server at /mnt.)
		if (PS5Platform::IsElevated() || ps5vfs_is_virtual("/mnt/usb0"))
		{
			static constexpr const char* names[] = {"crux2", "pspcsx2", "pcsx2", "ps2", "ps2iso", "playstation2"};
			for (unsigned drive = 0; drive < 8; drive++)
			{
				const std::string root = "/mnt/usb" + std::to_string(drive);
				if (!FileSystem::DirectoryExists(root.c_str()))
					continue;
				std::string folder;
				FileSystem::FindResultsArray entries;
				FileSystem::FindFiles(root.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS, &entries);
				for (const FILESYSTEM_FIND_DATA& entry : entries)
				{
					for (const char* const name : names)
					{
						if (folder.empty() && StringUtil::compareNoCase(entry.FileName, name))
							folder = Path::Combine(root, entry.FileName);
					}
				}
				const bool made = folder.empty();
				if (made)
					folder = Path::Combine(root, "crux2");
				places.emplace_back(fmt::format("USB drive {} ({}{})", drive + 1, Path::GetFileName(folder), made ? ", made for it" : ""), folder);
			}
		}
		for (unsigned i = 0;; i++)
		{
			const char* const point = ps5vfs_mount_point(i);
			if (!point)
				break;
			if (std::strncmp(point, "/smb/", 5) == 0)
				places.emplace_back(fmt::format("Network share {} (SMB or NFS)", point + 5), point);
		}
		return places;
	}

	void CopyDiscToImage(const PS5Disc::State& disc)
	{
		if (!disc.ps2 || s_disc_copying.load(std::memory_order_acquire))
			return;
		// First the format (the last one chosen comes first in the list's focus), then the place.
		std::vector<std::string> names;
		for (int f = 0; f < static_cast<int>(PS5DiscImage::Format::Count); f++)
		{
			const auto format = static_cast<PS5DiscImage::Format>(f);
			names.push_back(fmt::format("{}   ({}): {}", PS5DiscImage::Name(format), PS5DiscImage::Extension(format), PS5DiscImage::Description(format)));
		}
		const int last = std::clamp(Host::GetBaseIntSettingValue("PS5", "DiscImageFormat", 0), 0, static_cast<int>(PS5DiscImage::Format::Count) - 1);
		bp::pick("Which kind of image?", std::move(names), last, [this, disc](int chosen) {
			if (chosen < 0 || chosen >= static_cast<int>(PS5DiscImage::Format::Count))
				return;
			Host::SetBaseIntSettingValue("PS5", "DiscImageFormat", chosen);
			Host::CommitBaseSettingChanges();
			const auto format = static_cast<PS5DiscImage::Format>(chosen);
			std::vector<std::pair<std::string, std::string>> places = ImagePlaces();
			std::vector<std::string> where;
			for (const auto& place : places)
				where.push_back(place.first);
			bp::pick("Where does the image go?", std::move(where), -1, [this, disc, places, format](int at) {
				if (at >= 0 && at < static_cast<int>(places.size()))
					CopyDiscToImage(disc, places[static_cast<size_t>(at)].second, places[static_cast<size_t>(at)].first, format);
			});
		});
	}

	// "45%, about 6 minutes left" from what was done since the start.
	static std::string ImageProgress(const char* doing, std::uint64_t done, std::uint64_t total, std::chrono::steady_clock::time_point started)
	{
		const int percent = total ? static_cast<int>(100 * done / total) : 0;
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		if (done == 0 || seconds < 8 || total == 0)
			return fmt::format("{}: {}%", doing, percent);
		const double left = seconds * static_cast<double>(total - done) / static_cast<double>(done);
		const int minutes = static_cast<int>(left / 60 + 0.5);
		return minutes <= 1 ? fmt::format("{}: {}%, about a minute left", doing, percent) :
							  fmt::format("{}: {}%, about {} minutes left", doing, percent, minutes);
	}

	void CopyDiscToImage(const PS5Disc::State& disc, const std::string& folder, const std::string& place, PS5DiscImage::Format format)
	{
		if (!disc.ps2 || s_disc_copying.exchange(true))
			return;
		const bool remote = ps5vfs_is_virtual(folder.c_str()) != 0;
		if (remote ? ps5vfs_make_folders(folder.c_str()) != 0 : !FileSystem::CreateDirectoryPath(folder.c_str(), false))
		{
			PS5UI::Toast("No image was made: the folder " + folder + " could not be made.");
			s_disc_copying.store(false, std::memory_order_release);
			return;
		}
		// "Title (SLUS-12345).iso", as dumps are named; the disc's own label when the database does
		// not know it.
		std::string title = disc.volume_label.empty() ? std::string("disc") : disc.volume_label;
		if (const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(disc.serial))
			title = entry->name;
		std::string name = disc.serial.empty() ? title : title + " (" + disc.serial + ")";
		for (char& c : name)
		{
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
				c = '_';
		}
		name += PS5DiscImage::Extension(format);
		const std::string path = remote ? folder + "/" + name : Path::Combine(folder, name);
		bp::toast(fmt::format("Making an image of the disc ({}) on {}: the progress is at the bottom of the library.", PS5DiscImage::Name(format), place), 5.f);
		const std::string device = disc.device;
		std::thread([path, name, device, place, remote, format] {
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			const auto started = std::chrono::steady_clock::now();
			const std::string doing = fmt::format("Making an image of the disc ({})", PS5DiscImage::Name(format));
			const auto progress = [&](std::uint64_t done, std::uint64_t total) {
				PS5UI::SetBackgroundNote(ImageProgress(doing.c_str(), done, total, started));
				return true;
			};
			std::string error;
			bool ok;
			if (remote && format == PS5DiscImage::Format::Iso)
			{
				// A share (or a drive reached through the console's FTP server): written piece by
				// piece through the file layer, never whole in memory.
				ok = PS5Disc::SaveImageWith(device,
					[&path](std::uint64_t size, const PS5Disc::ImageSource& source, std::string* write_error) {
						if (ps5vfs_store_from(path.c_str(), size, source) == 0)
							return true;
						*write_error = std::string("The image could not be written to the share: ") + std::strerror(errno);
						return false;
					},
					progress, &error);
			}
			else if (remote)
			{
				// A compressed image's size is only known at its end, and its start is written last:
				// it is made on the internal storage first, then copied there and taken away here.
				const std::string temp_folder = Path::Combine(EmuFolders::DataRoot, "image-in-progress");
				FileSystem::CreateDirectoryPath(temp_folder.c_str(), false);
				const std::string temp = Path::Combine(temp_folder, name);
				ok = PS5Disc::SaveImageAs(device, temp, format, progress, &error);
				if (ok)
				{
					const int fd = open(temp.c_str(), O_RDONLY);
					struct stat st = {};
					ok = (fd >= 0 && fstat(fd, &st) == 0);
					const std::uint64_t size = ok ? static_cast<std::uint64_t>(st.st_size) : 0;
					const auto copied = std::chrono::steady_clock::now();
					const auto source = [&](void* buffer, std::uint32_t length, std::uint64_t offset) -> std::int64_t {
						std::uint32_t got = 0;
						while (got < length)
						{
							const ssize_t r = pread(fd, static_cast<std::uint8_t*>(buffer) + got, length - got, static_cast<off_t>(offset + got));
							if (r < 0 && errno == EINTR)
								continue;
							if (r <= 0)
								return -1;
							got += static_cast<std::uint32_t>(r);
						}
						PS5UI::SetBackgroundNote(ImageProgress("Copying the image to the share", offset + length, size, copied));
						return length;
					};
					if (ok && ps5vfs_store_from(path.c_str(), size, source) != 0)
					{
						error = std::string("The image could not be copied to the share: ") + std::strerror(errno);
						ok = false;
					}
					if (fd >= 0)
						close(fd);
				}
				FileSystem::DeleteFilePath(temp.c_str());
			}
			else
			{
				ok = PS5Disc::SaveImageAs(device, path, format, progress, &error);
				PS5Platform::MakePublic(path.c_str(), false);
			}
			PS5UI::SetBackgroundNote(std::string());
			const int minutes = static_cast<int>(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() / 60 + 0.5);
			PS5UI::Toast(ok ? fmt::format("The image of the disc is on {} ({} minute{}). Looking for games again...", place, minutes, minutes == 1 ? "" : "s") :
							  "No image was made: " + error);
			s_disc_copying.store(false, std::memory_order_release);
			if (ok)
				Host::RefreshGameListAsync(false);
		}).detach();
	}

	// The image of the disc in this drive (the Disc tab and its details ask for it).
	void makeImage(const bp::GameInfo& game) override
	{
		if (s_disc_copying.load(std::memory_order_acquire))
		{
			bp::toast("An image is being made already: its progress is at the bottom of the library.", 4.f);
			return;
		}
		const PS5Disc::State state = PS5Disc::Drive(game.path);
		if (!state.present || !state.ps2)
		{
			bp::toast("That disc is not in the drive any more.", 4.f);
			return;
		}
		CopyDiscToImage(state);
	}

	bool imageBusy() override { return s_disc_copying.load(std::memory_order_acquire); }

	bp::GameAbout about(const bp::GameInfo& game) override
	{
		bp::GameAbout out;
		if (game.serial.empty() || game.kind != bp::GameInfo::PS2)
			return out;
		// (Looked up by its English name when the database has one: the articles are English.)
		std::string title = game.title;
		if (const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(game.serial))
			title = !entry->name_en.empty() ? entry->name_en : entry->name;
		// The disc in a drive has no cover, title screen or picture of play until they are fetched
		// for it alone (it is in no list): once a run, with the covers' setting on.
		if (game.source == 3)
		{
			if (const unsigned done = m_disc_art_done.load(std::memory_order_acquire); done != m_disc_art_seen)
			{
				m_disc_art_seen = done;
				m_cover_paths.erase(game.path);
				m_pictures_listed = false;
			}
			if (m_disc_art_asked.insert(game.serial).second && Host::GetBaseBoolSettingValue("PS5", "AutoCovers", true) &&
				(!cover(game).valid() || !picture(game, 0).valid()))
			{
				const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(game.serial);
				const std::string name = entry ? entry->name : game.title, name_en = entry ? entry->name_en : std::string();
				const std::string serial = game.serial, region = game.region;
				std::thread([this, serial, name, name_en, region] {
					PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
					if (PS5Covers::FetchForDisc(serial, name, name_en, region) > 0)
						m_disc_art_done.fetch_add(1, std::memory_order_release);
				}).detach();
			}
		}
		bool looking = false;
		const PS5About::Info info = PS5About::Get(game.serial, title, &looking);
		out.looking = looking;
		out.found = info.found;
		out.summary = info.summary;
		out.developer = info.developer;
		out.publisher = info.publisher;
		out.released = info.released;
		out.genre = info.genre;
		out.modes = info.modes;
		if (info.found)
			out.source = "Wikipedia, \"" + info.article + "\" (CC BY-SA 4.0)";
		return out;
	}

	// The phone's control: the server's hooks, which it runs on this thread (PS5Web::tick).
	void StartPhoneControl()
	{
		PS5Web::Hooks hooks;
		hooks.listGames = [this] {
			std::vector<PS5Web::Game> list;
			for (const bp::GameInfo& g : games())
			{
				if (g.hidden)
					continue;
				PS5Web::Game game;
				game.title = g.title;
				game.serial = g.serial;
				game.region = g.region;
				game.path = g.path;
				game.has_cover = !g.serial.empty();
				list.push_back(std::move(game));
			}
			return list;
		};
		hooks.startGame = [this](const std::string& path) {
			if (VMManager::HasValidVM() || starting())
				return false;
			for (const bp::GameInfo& g : games())
			{
				if (g.path == path)
				{
					bp::toast("Started from the phone: " + g.title);
					start(g, INT_MIN, 0);
					return true;
				}
			}
			return false;
		};
		hooks.status = [this] {
			PS5Web::Status s;
			const bp::Running r = running();
			s.running = r.valid;
			s.paused = r.paused;
			s.fast_forward = !r.frameLimit;
			s.title = r.title;
			s.serial = r.serial;
			s.speed_percent = r.valid ? PerformanceMetrics::GetSpeed() : 0.f;
			s.fps = r.valid ? PerformanceMetrics::GetInternalFPS() : 0.f;
			return s;
		};
		hooks.action = [this](const std::string& name, int arg) {
			const bp::Running r = running();
			if (!r.valid)
				return false;
			if (name == "pause" || name == "resume")
				setPaused(name == "pause");
			else if (name == "save_state" && arg >= 1 && arg <= 10)
				saveState(arg);
			else if (name == "load_state" && arg >= 1 && arg <= 10)
			{
				for (const bp::StateInfo& st : states(r.serial, r.crc, false))
				{
					if (st.slot == arg && !st.backup)
					{
						loadStateFile(st);
						return true;
					}
				}
				return false;
			}
			else if (name == "fast_forward")
				setFrameLimit(arg == 0);
			else if (name == "quick_menu")
				bp::openQuickMenu();
			else if (name == "leave")
			{
				if (memoryCardBusy())
					return false;
				closeGame(true);
			}
			else
				return false;
			return true;
		};
		hooks.typeText = [](const std::string& text) {
			const std::lock_guard lock(s_web_typed_mutex);
			if (s_web_typed.size() < 4096)
				s_web_typed += text;
			return true;
		};
		hooks.pressKey = [](PS5Web::Key key) {
			const std::lock_guard lock(s_web_typed_mutex);
			s_web_keys |= (key == PS5Web::Key::Enter) ? 1u : 2u;
			return true;
		};
		hooks.readLog = [](PS5Web::Log which, std::string* out) {
			const std::string path = (which == PS5Web::Log::Boot) ? Path::Combine(ps5px_title_dir(), "crux2-boot.log") :
																	 Path::Combine(EmuFolders::Logs, "emulog.txt");
			std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
			if (!text.has_value())
				return false;
			*out = std::move(*text);
			return true;
		};
		hooks.cover = [](const std::string& serial, std::vector<uint8_t>* bytes, std::string* mime) {
			std::string path;
			{
				const auto lock = GameList::GetLock();
				for (u32 i = 0; i < GameList::GetEntryCount(); i++)
				{
					const GameList::Entry* e = GameList::GetEntryByIndex(i);
					if (e != nullptr && e->serial == serial)
					{
						path = GameList::GetCoverImagePathForEntry(e);
						break;
					}
				}
			}
			if (path.empty())
				return false;
			std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
			if (!data.has_value())
				return false;
			*bytes = std::move(*data);
			*mime = StringUtil::EndsWithNoCase(path, ".png") ? "image/png" : StringUtil::EndsWithNoCase(path, ".webp") ? "image/webp" : "image/jpeg";
			return true;
		};
		hooks.localAddress = [] { return PS5Net::CurrentStatus().console_ip; };
		hooks.keyChanged = [](const std::string& key) {
			Host::SetBaseStringSettingValue("PS5", "PhoneKey", key.c_str());
			Host::CommitBaseSettingChanges();
		};
		PS5Web::Options options;
		options.key = Host::GetBaseStringSettingValue("PS5", "PhoneKey", "");
		PS5Web::init(hooks, [](const char* line) { ps5px_log("phone: %s", line); }, options);
	}

	unsigned m_track_said = ~0u;
	PS5Update::State m_update_state = PS5Update::State::Idle;
	bool m_phone_started = false;
	bool m_phone_on = false;
	bool m_resume_asked = false;
	bool m_safe_asked = false;


	// ---- controller buttons
	std::string binding(int layer, const bp::Option& option) override
	{
		SmallString value;
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface* const si = Layer(layer);
			if (si == nullptr || !si->GetStringValue(option.section.c_str(), option.key.c_str(), &value))
				return {};
		}
		if (value.empty())
			return {};
		InputManager::PrettifyInputBinding(value, true);
		return std::string(value.view());
	}

	void listen(int, const bp::Option& option) override
	{
		if (s_listening.load(std::memory_order_acquire))
			StopListening();
		{
			const auto lock = Host::GetSettingsLock();
			s_listen.type = static_cast<InputBindingInfo::Type>(option.bindType);
			s_listen.section = option.section;
			s_listen.key = option.key;
			s_listen.keys.clear();
			s_listen.ranges.clear();
			s_listen.timer.Reset();
		}
		s_listening.store(true, std::memory_order_release);

		// On the emulation thread, for every event of a controller: the button (or the buttons
		// held together) is taken when it is let go, as PCSX2's own interface took it.
		InputManager::SetHook([](InputBindingKey key, float value) -> InputInterceptHook::CallbackResult {
			const auto lock = Host::GetSettingsLock();
			if (s_listen.type == InputBindingInfo::Type::Unknown)
				return InputInterceptHook::CallbackResult::StopProcessingEvent;

			float initial = value, lowest = value;
			const auto range = std::find_if(s_listen.ranges.begin(), s_listen.ranges.end(), [key](const auto& it) { return it.first.bits == key.bits; });
			if (range != s_listen.ranges.end())
			{
				initial = range->second.first;
				lowest = range->second.second = std::min(range->second.second, value);
			}
			else
			{
				s_listen.ranges.emplace_back(key, std::make_pair(initial, lowest));
			}

			const float magnitude = std::abs(value);
			// (A pedal rests at one end of its axis.)
			const bool reversed = (key.source_subtype == InputSubclass::ControllerAxis && initial > 0.5f);
			for (InputBindingKey& held : s_listen.keys)
			{
				if (held.MaskDirection() != key.MaskDirection())
					continue;
				// One of those taken, let go: that is the choice made.
				if (reversed ? ((initial - value) <= 0.25f) : (magnitude < 0.5f))
				{
					if (reversed && initial > 0.5f && lowest <= -0.5f)
						held.modifier = InputModifier::FullAxis;
					const std::string chosen =
						InputManager::ConvertInputBindingKeysToString(s_listen.type, s_listen.keys.data(), s_listen.keys.size());
					Host::Internal::GetBaseSettingsLayer()->SetStringValue(s_listen.section.c_str(), s_listen.key.c_str(), chosen.c_str());
					s_shared_apply.store(true, std::memory_order_release);
					s_shared_changed.store(true, std::memory_order_release);
					s_listen.type = InputBindingInfo::Type::Unknown;
					s_listen.keys.clear();
					s_listen.ranges.clear();
					s_listening.store(false, std::memory_order_release);
					return InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent;
				}
				return InputInterceptHook::CallbackResult::StopProcessingEvent;
			}

			// Pressed far enough: taken, and waited on until it is let go.
			if (reversed ? (magnitude < 0.5f) : (magnitude >= 0.5f))
			{
				InputBindingKey taken = key;
				taken.modifier = (value < 0.f && !reversed) ? InputModifier::Negate : InputModifier::None;
				taken.invert = reversed;
				s_listen.keys.push_back(taken);
				return InputInterceptHook::CallbackResult::StopProcessingEvent;
			}
			// Anything else goes its usual way: above all the release of the Cross that asked for
			// this, which the interface must see, or it would wait for that button for ever.
			return InputInterceptHook::CallbackResult::ContinueProcessingEvent;
		});
	}

	float listening() override
	{
		if (!s_listening.load(std::memory_order_acquire))
			return 0.f;
		const double left = LISTEN_SECONDS - s_listen.timer.GetTimeSeconds();
		if (left <= 0.0)
		{
			StopListening();
			return 0.f;
		}
		return static_cast<float>(left);
	}

	void cancelListen() override
	{
		if (s_listening.load(std::memory_order_acquire))
			StopListening();
	}

	void clearBinding(int layer, const bp::Option& option) override
	{
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
				si->DeleteValue(option.section.c_str(), option.key.c_str());
		}
		Changed(layer, option.section.c_str(), option.key.c_str());
	}

	// ---- the rest
	bp::Pad pad() override
	{
		bp::Pad pad;
		pad.connected = std::max(0, s_controllers.load(std::memory_order_acquire));
		// (The emulator's own map of a controller to ImGui's keys: the sticks are the d-pad too.)
		static constexpr std::pair<ImGuiKey, uint32_t> keys[] = {
			{ImGuiKey_GamepadDpadUp, bp::Pad::Up},       {ImGuiKey_GamepadDpadDown, bp::Pad::Down},   {ImGuiKey_GamepadDpadLeft, bp::Pad::Left},
			{ImGuiKey_GamepadDpadRight, bp::Pad::Right}, {ImGuiKey_GamepadFaceDown, bp::Pad::Cross},  {ImGuiKey_GamepadFaceRight, bp::Pad::Circle},
			{ImGuiKey_GamepadFaceLeft, bp::Pad::Square}, {ImGuiKey_GamepadFaceUp, bp::Pad::Triangle}, {ImGuiKey_GamepadL1, bp::Pad::L1},
			{ImGuiKey_GamepadR1, bp::Pad::R1},           {ImGuiKey_GamepadL2, bp::Pad::L2},           {ImGuiKey_GamepadR2, bp::Pad::R2},
			{ImGuiKey_GamepadStart, bp::Pad::Options},   {ImGuiKey_GamepadBack, bp::Pad::TouchPad},   {ImGuiKey_GamepadL3, bp::Pad::L3},
			{ImGuiKey_GamepadR3, bp::Pad::R3},
		};
		for (const auto& [key, bit] : keys)
		{
			if (ImGui::IsKeyDown(key))
				pad.held |= bit;
		}
		return pad;
	}

	void quit() override
	{
		Host::RunOnCPUThread([]() { Host::RequestExitApplication(false); });
	}

	void log(const std::string& line) override { ps5px_log("interface: %s", line.c_str()); }
	const char* appName() override { return "Crux2"; }

	void sound(bp::Sound what) override
	{
		// (Each of these only leaves a note for the sound's own thread.)
		switch (what)
		{
			case bp::Sound::StartupBegins:
				PS5UIAudio::PlayStartup();
				break;
			case bp::Sound::StartupSkipped:
				PS5UIAudio::SkipStartup();
				break;
			case bp::Sound::LibraryOpens:
				PS5UIAudio::StartBackground();
				// The library is on the screen for the first time: the moment to say that the run
				// before this one ended in a crash (its log is read once, here).
				if (!m_crash_looked)
				{
					m_crash_looked = true;
					m_crash_notice = PS5Jobs::CrashNotice();
					if (!m_crash_notice.empty())
						bp::message("Crux2 stopped unexpectedly last time", m_crash_notice);
				}
				if (!m_resume_asked)
				{
					m_resume_asked = true;
					OfferToGoOn();
				}
				break;
		}
	}

	// ------------------------------------------------- what the FullscreenUI functions use

	// Once a frame, before the screens are drawn.
	static bp::Option Volume(const char* label, const char* key, int def)
	{
		bp::Option o;
		o.kind = bp::Option::IntRange;
		o.label = label;
		o.section = "PS5";
		o.key = key;
		o.defInt = def;
		o.lo = 0;
		o.hi = 100;
		o.step = 5;
		o.unit = "%";
		o.flags = bp::Option::AllGames;
		return o;
	}

	void BeginFrame()
	{
		m_frame++;
		// The interface's sound follows its settings (asked twice a second: they are four lookups).
		if (m_frame % 30 == 1)
		{
			ps5px_apply_ui_sound_settings();
			// The phone's page, on or off as its switch says.
			const bool phone = Host::GetBaseBoolSettingValue("PS5", "PhoneControl", false);
			if (!m_phone_started)
			{
				m_phone_started = true;
				StartPhoneControl();
			}
			if (phone != m_phone_on)
			{
				m_phone_on = phone;
				PS5Web::apply(phone);
			}
		}
		PS5Web::tick();
		UpdateTick();
		// A start from the disc that came to nothing before there was a machine to be closed
		// (refused, or asked about and declined): the drive is the watcher's again.
		if (m_disc_lent && !s_boot_pending.load(std::memory_order_acquire) && VMManager::GetState() == VMState::Shutdown)
		{
			m_disc_lent = false;
			PS5Disc::SetInUse(false);
		}
		// Textures that were replaced: let go of once the frames that may have drawn them are shown
		// (what was put aside during the last frame waits for one more).
		for (std::unique_ptr<GSTexture>& texture : m_retired_before)
		{
			if (g_gs_device != nullptr)
				g_gs_device->Recycle(texture.release());
			else
				texture.release();
		}
		m_retired_before.clear();
		m_wallpapers.Retired(&m_retired);
		m_retired_before.swap(m_retired);
		// Pictures of games came: the folder is looked into again (and what was loaded of a file
		// that was written anew is not shown in its place).
		if (s_pictures_changed.exchange(false, std::memory_order_acq_rel))
		{
			m_pictures_listed = false;
			// (The pictures of the game that was asked about by itself may have been there before.)
			if (!m_pictures_asked.empty())
			{
				ImGuiFullscreen::InvalidateCachedTexture(PS5Covers::PicturePath(m_pictures_asked, 0));
				ImGuiFullscreen::InvalidateCachedTexture(PS5Covers::PicturePath(m_pictures_asked, 1));
				m_pictures_asked.clear();
			}
		}
		// The corner the emulator's figures were sent to (Settings > Screen care), kept there.
		{
			static constexpr OsdOverlayPos corners[4] = {OsdOverlayPos::TopLeft, OsdOverlayPos::TopRight, OsdOverlayPos::BottomRight, OsdOverlayPos::BottomLeft};
			if (m_osd_corner >= 0 && m_osd_corner < 4)
			{
				if (!m_osd_moved)
				{
					m_osd_moved = true;
					m_osd_own = GSConfig.OsdPerformancePos;
				}
				else if (m_osd_sent >= 0 && GSConfig.OsdPerformancePos != corners[m_osd_sent])
				{
					// (The settings were applied anew: what they say is the place of its own again.)
					m_osd_own = GSConfig.OsdPerformancePos;
				}
				if (m_osd_own != OsdOverlayPos::None)
					GSConfig.OsdPerformancePos = corners[m_osd_corner];
				m_osd_sent = m_osd_corner;
			}
			else if (m_osd_moved)
			{
				m_osd_moved = false;
				if (m_osd_sent >= 0 && GSConfig.OsdPerformancePos == corners[m_osd_sent])
					GSConfig.OsdPerformancePos = m_osd_own;
				m_osd_sent = -1;
			}
		}
		// Cheat files came from the network: the open game's list of them is read again.
		if (s_cheats_changed.exchange(false, std::memory_order_acq_rel) && m_game_si)
		{
			LoadPatches();
			m_pages++;
		}
		// A job on another thread is over, or has something to say.
		if (s_pages_changed.exchange(false, std::memory_order_acq_rel))
		{
			if (m_game_si)
				LoadPatches(); // (PCSX2's patches may have come)
			m_pages++;
		}
		if (s_states_changed.exchange(false, std::memory_order_acq_rel))
			m_states_key.clear();
		if (m_frame % 15 == 2)
		{
			if (const unsigned lists = PS5Backup::ListsRead(); lists != m_backup_lists)
			{
				m_backup_lists = lists;
				m_pages++;
			}
		}
		{
			std::vector<std::string> toasts;
			std::vector<std::pair<std::string, std::string>> messages;
			{
				const std::lock_guard lock(s_told_mutex);
				toasts.swap(s_told_toasts);
				messages.swap(s_told_messages);
			}
			for (const std::string& line : toasts)
				bp::toast(line, 7.f);
			for (const auto& [title, text] : messages)
				bp::message(title, text);
		}
		// Textures retired on the frame before were still in that frame's drawing; they go now.
		if (g_gs_device != nullptr)
		{
			for (std::unique_ptr<GSTexture>& texture : m_retired)
				g_gs_device->Recycle(texture.release());
		}
		else
		{
			for (std::unique_ptr<GSTexture>& texture : m_retired)
				texture.release();
		}
		m_retired.clear();
		// A handful of state pictures are kept: those not drawn for a while go.
		if (m_state_pictures.size() > 14)
		{
			for (auto it = m_state_pictures.begin(); it != m_state_pictures.end();)
			{
				if (it->second.used + 2 < m_frame)
				{
					if (it->second.texture)
						m_retired.push_back(std::move(it->second.texture));
					it = m_state_pictures.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
		// The game list fills in while a scan runs, and a game played has new times: looked at
		// twice a second. (A scan starts by emptying the list and fills it again: while one runs
		// the screens keep what they have until the list is at least as long.)
		if (m_frame % 30 == 0)
		{
			u32 count = 0;
			const u64 signature = Signature(&count);
			if (signature != m_games_signature && (!s_scanning.load(std::memory_order_acquire) || count >= m_games_count))
				s_games_generation.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	// Once a frame, after them: what was changed is saved and taken by the emulator.
	void EndFrame()
	{
		if (s_shared_changed.exchange(false, std::memory_order_acq_rel))
		{
			Host::CommitBaseSettingChanges();
			if (s_shared_apply.exchange(false, std::memory_order_acq_rel))
				Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
		}
		SaveOwn();
	}

	void ReleaseTextures()
	{
		for (auto& [path, picture] : m_state_pictures)
		{
			if (picture.texture)
				m_retired.push_back(std::move(picture.texture));
		}
		m_state_pictures.clear();
		m_wallpapers.Release(&m_retired);
		for (std::unique_ptr<GSTexture>& texture : m_retired_before)
			m_retired.push_back(std::move(texture));
		m_retired_before.clear();
		for (std::unique_ptr<GSTexture>& texture : m_retired)
		{
			if (g_gs_device != nullptr)
				g_gs_device->Recycle(texture.release());
			else
				texture.release();
		}
		m_retired.clear();
	}

	void Reset()
	{
		m_cover_paths.clear();
		m_pictures_listed = false;
		m_states_key.clear();
		m_game_si.reset();
	}

	void SetHotkeys(std::vector<const HotkeyInfo*> hotkeys) { m_hotkeys = std::move(hotkeys); }
	void ForgetCovers()
	{
		m_cover_paths.clear();
		m_pictures_listed = false;
	}
	void ForgetStates() { m_states_key.clear(); }
	void PagesChanged() { m_pages++; }


private:
	struct StatePicture
	{
		std::unique_ptr<GSTexture> texture;
		time_t when = 0;
		u64 used = 0;
	};

	// The rows of a game's own widescreen and no-interlacing patch (bp_options.cpp): the key is the
	// patch's name in PCSX2's lists.
	static constexpr const char* GAME_PATCH_SECTION = "PS5GamePatch";

	static bool IsPatchSection(const char* section)
	{
		return std::strcmp(section, Patch::PATCHES_CONFIG_SECTION) == 0 || std::strcmp(section, Patch::CHEATS_CONFIG_SECTION) == 0;
	}

	// (Under the settings lock.)
	SettingsInterface* Layer(int layer)
	{
		return (layer == bp::Own) ? static_cast<SettingsInterface*>(m_game_si.get()) : Host::Internal::GetBaseSettingsLayer();
	}

	// A switch as it is for the open game: its own, else every game's, else the emulator's.
	bool Effective(const char* section, const char* key, bool def)
	{
		bool value = def;
		const auto lock = Host::GetSettingsLock();
		if (m_game_si && m_game_si->GetBoolValue(section, key, &value))
			return value;
		Host::Internal::GetBaseSettingsLayer()->GetBoolValue(section, key, &value);
		return value;
	}

	void Changed(int layer, const char* section, const char* key)
	{
		const std::string_view where = section, what = key;
		// The SingStar choice sets USB port 2 up in the same layer (ps5_mic.h).
		if (where == "PS5" && what == "SingStar")
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr)
			{
				const PS5Mic::UsbSetup setup = PS5Mic::SetupFor(si->GetIntValue("PS5", "SingStar", 0));
				si->SetStringValue("USB2", "Type", setup.type);
				si->SetUIntValue("USB2", "singstar_subtype", 0);
				si->SetStringValue("USB2", "singstar_player1_device_name", setup.player1);
				si->SetStringValue("USB2", "singstar_player2_device_name", setup.player2);
			}
		}
		// A figure of the on-screen display switched on while the figures' place is "Not shown" (the
		// first release's default): they go to the top right, or switching them on would show nothing.
		if (where == "EmuCore/GS" && what.compare(0, 7, "OsdShow") == 0 && what != "OsdShowIndicators")
		{
			const auto lock = Host::GetSettingsLock();
			if (SettingsInterface* const si = Layer(layer); si != nullptr && si->GetBoolValue(section, key, false))
			{
				SettingsInterface* const base = Host::Internal::GetBaseSettingsLayer();
				if (base && base->GetIntValue("EmuCore/GS", "OsdPerformancePos", 3) == 0)
				{
					base->SetIntValue("EmuCore/GS", "OsdPerformancePos", 3);
					m_pages++;
				}
			}
		}
		// What decides the rows of a page.
		if ((where.compare(0, 3, "Pad") == 0 && (what == "Type" || what.compare(0, 8, "Multitap") == 0)))
			m_pages++;
		// (A card given to one slot is no longer offered for the other.)
		if (where == "MemoryCards")
			m_pages++;
		// Another place for the backups: its list is another one.
		if (where == "PS5" && what == "BackupPlace")
		{
			m_backup_asked.clear();
			m_pages++;
		}
		if (where == "EmuCore" && (what == "ShowPatchesForAllCRCs" || what == "ShowCheatsForAllCRCs"))
		{
			LoadPatches();
			m_pages++;
		}
		if (where == "UI" && what == "PreferEnglishGameList")
			s_games_generation.fetch_add(1, std::memory_order_acq_rel);
		if (layer == bp::Own)
		{
			m_own_changed = true;
			return;
		}
		// (Apply before changed: the frame's end reads them in the other order, and other threads
		// set them too.) The interface's own choices, its skin and its view, are nothing the
		// emulator takes.
		if (where != "PS5UI")
			s_shared_apply.store(true, std::memory_order_release);
		s_shared_changed.store(true, std::memory_order_release);
	}

	static std::string SizeText(u64 bytes)
	{
		if (bytes >= (1ull << 30))
			return fmt::format("{:.2f} GB", static_cast<double>(bytes) / (1ull << 30));
		if (bytes >= (1ull << 20))
			return fmt::format("{:.0f} MB", static_cast<double>(bytes) / (1ull << 20));
		return fmt::format("{:.0f} KB", static_cast<double>(bytes) / 1024.0);
	}

	// One thing the database recommends that the open game does not have yet.
	struct Change
	{
		const char* section;
		const char* key;
		bool is_int;
		int value;
		std::string line; // "Blending accuracy: Basic, now High"
	};
	struct Recommendation
	{
		bool known = false;          // the database has the game
		std::vector<Change> changes; // what the button would set
		std::vector<std::string> met; // what is recommended and is so already
		std::string compatibility;   // "Playable", or empty when the list does not say
	};

	// An option as it is for the open game: its own, else every game's, else `def`.
	int EffectiveInt(const char* section, const char* key, int def)
	{
		int value = def;
		const auto lock = Host::GetSettingsLock();
		if (m_game_si && m_game_si->GetIntValue(section, key, &value))
			return value;
		Host::Internal::GetBaseSettingsLayer()->GetIntValue(section, key, &value);
		return value;
	}

	Recommendation FindRecommended()
	{
		Recommendation out;
		GameDatabase::ensureLoaded();
		const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(m_game_serial);
		if (!entry)
			return out;
		out.known = true;
		static constexpr const char* compat[] = {"", "Nothing works", "Shows its intro", "Reaches its menus", "Goes in game", "Playable", "Perfect"};
		const int rating = static_cast<int>(entry->compat);
		if (rating > 0 && rating < static_cast<int>(std::size(compat)))
			out.compatibility = compat[rating];

		static constexpr const char* blending[] = {"Minimum", "Basic", "Medium", "High", "Full", "Maximum"};
		bool other_fixes = false;
		for (const auto& [id, value] : entry->gsHWFixes)
		{
			switch (id)
			{
				case GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel:
				{
					if (value < 0 || value >= static_cast<int>(std::size(blending)))
						break;
					const int now = std::clamp(EffectiveInt("EmuCore/GS", "accurate_blending_unit", 1), 0, static_cast<int>(std::size(blending)) - 1);
					if (now < value)
						out.changes.push_back({"EmuCore/GS", "accurate_blending_unit", true, value,
							fmt::format("Blending accuracy: {}, now {}", blending[now], blending[value])});
					else
						out.met.push_back(fmt::format("Blending accuracy {} is recommended; it is {}", blending[value], blending[now]));
					break;
				}
				case GameDatabaseSchema::GSHWFixId::RecommendedAccurateAlphaTest:
					if (value <= 0)
						break;
					if (!Effective("EmuCore/GS", "HWAccurateAlphaTest", false))
						out.changes.push_back({"EmuCore/GS", "HWAccurateAlphaTest", false, 1, "Accurate alpha test: now on"});
					else
						out.met.emplace_back("Accurate alpha test is recommended; it is on");
					break;
				case GameDatabaseSchema::GSHWFixId::RecommendedHWAA1:
					if (value <= 0)
						break;
					if (!Effective("EmuCore/GS", "HWAA1", false))
						out.changes.push_back({"EmuCore/GS", "HWAA1", false, 1, "AA1 (the console's edge smoothing): now on"});
					else
						out.met.emplace_back("AA1 is recommended; it is on");
					break;
				default:
					other_fixes = true;
					break;
			}
		}
		// The database's own fixes, where they were switched off for this game (or for all).
		if (other_fixes && Effective("EmuCore/GS", "UserHacks", false))
			out.changes.push_back({"EmuCore/GS", "UserHacks", false, 0,
				"Renderer fixes chosen by hand: now off, so the database's own fixes for this game apply"});
		const bool has_game_fixes = !entry->gameFixes.empty() || !entry->speedHacks.empty() ||
			entry->eeRoundMode != FPRoundMode::MaxCount || entry->eeDivRoundMode != FPRoundMode::MaxCount ||
			entry->vu0RoundMode != FPRoundMode::MaxCount || entry->vu1RoundMode != FPRoundMode::MaxCount ||
			entry->eeClampMode != GameDatabaseSchema::ClampMode::Undefined || entry->vu0ClampMode != GameDatabaseSchema::ClampMode::Undefined ||
			entry->vu1ClampMode != GameDatabaseSchema::ClampMode::Undefined;
		if (has_game_fixes && !Effective("EmuCore", "EnableGameFixes", true))
			out.changes.push_back({"EmuCore", "EnableGameFixes", false, 1, "Game fixes: now on, the database has some for this game"});
		if ((!entry->patches.empty() || !entry->dynaPatches.empty()) && !Effective("EmuCore", "EnablePatches", true))
			out.changes.push_back({"EmuCore", "EnablePatches", false, 1, "Compatibility patches: now on, the database has one for this game"});
		return out;
	}

	void ApplyRecommended()
	{
		const Recommendation found = FindRecommended();
		const std::string title = "Recommended for " + (m_game.title.empty() ? m_game_serial : m_game.title);
		if (!found.known)
		{
			bp::message(title, "PCSX2's game database does not know this game (" + m_game_serial + "): there is nothing it recommends.");
			return;
		}
		std::string text;
		if (found.changes.empty())
		{
			text = found.met.empty() ? "The database recommends nothing for this game beyond the fixes Crux2 applies by itself. Nothing was changed." :
			                           "This game has what the database recommends already. Nothing was changed.";
		}
		else
		{
			for (const Change& change : found.changes)
			{
				if (change.is_int)
					setInt(bp::Own, change.section, change.key, change.value);
				else
					setBool(bp::Own, change.section, change.key, change.value != 0);
			}
			text = "Set for this game only:";
			for (const Change& change : found.changes)
				text += "\n  -  " + change.line;
			if (running().valid)
				text += "\n\nThe running game takes them at once.";
		}
		for (const std::string& line : found.met)
			text += "\n" + line + ".";
		if (!found.compatibility.empty())
			text += "\n\nPCSX2's compatibility list says of this game: " + found.compatibility + ".";
		if (!found.changes.empty())
			text += "\n\nTo undo: \"Back to the Settings for everything\" on this page, or each option in Graphics.";
		m_pages++;
		bp::message(title, text);
	}

	// The open game's settings, to its file: gone with its last value.
	void SaveOwn()
	{
		if (!m_own_changed || !m_game_si)
			return;
		m_own_changed = false;
		Error error;
		m_game_si->RemoveEmptySections();
		const std::string& file = m_game_si->GetFileName();
		if (m_game_si->IsEmpty())
		{
			if (FileSystem::FileExists(file.c_str()) && !FileSystem::DeleteFilePath(file.c_str(), &error))
				bp::message("The game's options could not be removed", error.GetDescription());
		}
		else if (!m_game_si->Save(&error))
		{
			bp::message("The game's options could not be saved", error.GetDescription());
		}
		else
		{
			PS5Platform::MakePublic(file.c_str(), false);
		}
		m_own_count = CountValues(file);
		if (VMManager::HasValidVM())
			Host::RunOnCPUThread([]() { VMManager::ReloadGameSettings(); });
	}

	void LoadPatches()
	{
		m_patches.clear();
		m_cheats.clear();
		m_unlabelled = 0;
		if (!m_game_si)
			return;
		const auto sorted = [](std::vector<Patch::PatchInfo> list) {
			std::sort(list.begin(), list.end(), [](const Patch::PatchInfo& a, const Patch::PatchInfo& b) { return a.name < b.name; });
			return list;
		};
		m_patches = sorted(Patch::GetPatchInfo(m_game_serial, m_game.crc, false, Effective("EmuCore", "ShowPatchesForAllCRCs", false), nullptr));
		m_cheats = sorted(Patch::GetPatchInfo(m_game_serial, m_game.crc, true, Effective("EmuCore", "ShowCheatsForAllCRCs", false), &m_unlabelled));
	}

	// Every button of a port onto one of the console's controllers, as PCSX2 maps it by itself.
	// The players' controllers on the PlayStation 2's ports: 0 two players, 1 four through a
	// multitap in port 1, 2 four through one in port 2. Ports nobody is on are left empty, so that
	// one controller does not answer on two of them.
	void SetUpPlayers(int mode)
	{
		// The emulator's pads, in its numbering: 0 = 1A, 1 = 2A, 2..4 = 1B..1D, 5..7 = 2B..2D.
		static constexpr int players[3][Pad::NUM_CONTROLLER_PORTS] = {
			{0, 1, -1, -1, -1, -1, -1, -1},
			{0, -1, 1, 2, 3, -1, -1, -1},
			{0, 1, -1, -1, -1, 2, 3, -1},
		};
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface& si = *Host::Internal::GetBaseSettingsLayer();
			si.SetBoolValue("Pad", "MultitapPort1", mode == 1);
			si.SetBoolValue("Pad", "MultitapPort2", mode == 2);
			for (u32 pad = 0; pad < Pad::NUM_CONTROLLER_PORTS; pad++)
			{
				const int player = players[mode][pad];
				const std::string section = Pad::GetConfigSection(pad);
				if (player < 0)
				{
					si.SetStringValue(section.c_str(), "Type", Pad::GetControllerInfo(Pad::ControllerType::NotConnected)->name);
					continue;
				}
				si.SetStringValue(section.c_str(), "Type", Pad::GetControllerInfo(Pad::ControllerType::DualShock2)->name);
				InputManager::GenericInputBindingMapping mapping;
				PS5InputSource::AppendGenericBindingMapping(static_cast<u32>(player), &mapping);
				Pad::MapController(si, pad, mapping);
			}
		}
		ps5px_log("pads: set up for %s", mode == 0 ? "two players" : mode == 1 ? "four players, multitap in port 1" : "four players, multitap in port 2");
		s_shared_apply.store(true, std::memory_order_release);
		s_shared_changed.store(true, std::memory_order_release);
		m_pages++;
		bp::toast(mode == 0 ? "Player 1 is on port 1, player 2 on port 2." :
				  mode == 1 ? "Players 1 to 4 are on ports 1A to 1D. A running game sees the multitap the next time it starts." :
							  "Player 1 is on port 1, players 2 to 4 on ports 2A to 2C. A running game sees the multitap the next time it starts.",
			7.f);
	}

	void AutomaticMapping(u32 port, bool usb = false)
	{
		// (The controllers are the emulation thread's to list.)
		Host::RunOnCPUThread([this, port, usb]() {
			// (The emulator lists a keyboard and a mouse first, wherever it runs.)
			std::vector<std::pair<std::string, std::string>> devices = InputManager::EnumerateDevices();
			std::erase_if(devices, [](const auto& device) { return device.first.compare(0, 4, "SDL-") != 0; });
			// The USB controllers are SDL-4 to SDL-7 (ps5_input_source.cpp); the console's own, SDL-0 to SDL-3.
			std::erase_if(devices, [usb](const auto& device) {
				const int n = std::atoi(device.first.c_str() + 4);
				return usb ? (n < 4) : (n >= 4);
			});
			if (devices.empty())
			{
				MTGS::RunOnGSThread([usb]() {
					bp::toast(usb ? "No USB controller is connected (or it was not taken: the boot log says, under usb pads)." :
									"No controller is connected.");
				});
				return;
			}
			const std::string device = devices[std::min<size_t>(port, devices.size() - 1)].first;
			bool mapped = false;
			{
				const auto lock = Host::GetSettingsLock();
				mapped = Pad::MapController(*Host::Internal::GetBaseSettingsLayer(), port, InputManager::GetGenericBindingMapping(device));
			}
			s_shared_apply.store(true, std::memory_order_release);
			s_shared_changed.store(true, std::memory_order_release);
			MTGS::RunOnGSThread([this, mapped, port, usb]() {
				m_pages++;
				bp::toast(mapped ? fmt::format("Port {} is on the {} controller.", port + 1, usb ? "USB" : "console's") :
								   std::string("The port could not be mapped."));
			});
		});
	}

	void Boot(VMBootParameters params)
	{
		s_boot_pending.store(true, std::memory_order_release);
		s_boot_from_network.store(params.filename.compare(0, 5, "/smb/") == 0, std::memory_order_relaxed);
		PS5UI::SetLoadProgress(std::string(), -1.f);
		Host::RunOnCPUThread([params = std::move(params)]() {
			if (VMManager::GetState() != VMState::Shutdown)
			{
				s_boot_pending.store(false, std::memory_order_release);
				return;
			}
			auto hardcore = [](std::string reason, VMBootRestartCallback restart) {
				MTGS::RunOnGSThread([reason = std::move(reason), restart = std::move(restart)]() {
					bp::confirm(Achievements::GetHardcoreModeDisableTitle(), Achievements::GetHardcoreModeDisableText(reason.c_str()),
						[restart](bool yes) {
							if (yes)
								Host::RunOnCPUThread(restart);
							else
								s_boot_pending.store(false, std::memory_order_release);
						});
				});
			};
			// (A memory card that is being changed is whole on the drive before the emulator opens
			// it; no change is begun from here on: ps5_cards.h, SetWriteGuard.)
			PS5Cards::WaitForWrites();
			auto done = [](VMBootResult result, const Error& error) {
				if (result != VMBootResult::StartupSuccess)
				{
					ps5px_log("game: could not be started: %s", error.GetDescription().c_str());
					s_boot_pending.store(false, std::memory_order_release);
					if (MTGS::IsOpen())
						MTGS::RunOnGSThread([why = error.GetDescription()]() { bp::startFailed(why); });
					return;
				}
				// (Running before the start counts as over: the screens never see neither.)
				VMManager::SetState(VMState::Running);
				s_boot_pending.store(false, std::memory_order_release);
			};
			VMManager::InitializeAsync(params, std::move(hardcore), std::move(done));
		});
	}

	// Signs in at RetroAchievements on the emulation thread, which waits for the answer; what came
	// of it is said when it is there. (The password goes to PCSX2's sign-in and nowhere else.)
	static void SignIn(const std::string& name, const std::string& password)
	{
		if (s_signing_in.exchange(true, std::memory_order_acq_rel))
			return;
		bp::toast("Signing in at RetroAchievements...", 8.f);
		Host::RunOnCPUThread([name, password]() {
			Error error;
			const bool ok = Achievements::Login(name.c_str(), password.c_str(), &error);
			s_signing_in.store(false, std::memory_order_release);
			if (ok)
			{
				// Signed in to play with achievements: they are switched on with it.
				const bool were_on = Host::GetBaseBoolSettingValue("Achievements", "Enabled", false);
				if (!were_on)
				{
					Host::SetBaseBoolSettingValue("Achievements", "Enabled", true);
					Host::CommitBaseSettingChanges();
					VMManager::ApplySettings();
				}
				ps5px_log("achievements: signed in");
				PS5UI::Toast(fmt::format("Signed in as {}.{}", name, were_on ? "" : " Achievements are switched on."));
			}
			else
			{
				ps5px_log("achievements: signing in did not work: %s", error.GetDescription().c_str());
				PS5UI::Message("Signing in did not work",
					fmt::format("RetroAchievements said: {}\n\nThe name and the password are those of retroachievements.org, capitals as they "
								"are. With no answer at all, the console may have no way to the internet.",
						error.GetDescription()));
			}
			PS5UI::PagesChanged();
		});
	}

	static bp::GameInfo Describe(const GameList::Entry& entry, bool english)
	{
		bp::GameInfo game;
		game.path = entry.path;
		game.fileName = std::string(Path::GetFileName(entry.path));
		game.title = entry.GetTitle(english);
		game.source = (entry.path.compare(0, 5, "/smb/") == 0) ? 2 : (entry.path.compare(0, 8, "/mnt/usb") == 0) ? 1 : 0;
		game.serial = entry.serial;
		if (entry.region != GameList::Region::Other && entry.region != GameList::Region::Count)
			game.region = GameList::RegionToString(entry.region, false);
		game.crc = entry.crc;
		game.kind = (entry.type == GameList::EntryType::PS1Disc) ? bp::GameInfo::PS1 :
					(entry.type == GameList::EntryType::ELF)	 ? bp::GameInfo::Program :
																   bp::GameInfo::PS2;
		game.compatibility = std::clamp(static_cast<int>(entry.compatibility_rating), 0, 6);
		game.lastPlayed = entry.last_played_time;
		game.playedSeconds = entry.total_played_time;
		game.size = entry.total_size;
		game.favourite = entry.is_favorite;
		game.added = entry.last_modified_time;
		return game;
	}

	// A number that is another one when the list is: which games, their names and their times.
	static u64 Signature(u32* entries)
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		*entries = count;
		u64 sum = 0xcbf29ce484222325ull ^ count;
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry == nullptr)
				continue;
			u64 one = std::hash<std::string>()(entry->path);
			one ^= std::hash<std::string>()(entry->title) * 0x100000001b3ull;
			one ^= static_cast<u64>(entry->last_played_time) * 0x9e3779b97f4a7c15ull;
			one ^= static_cast<u64>(entry->total_played_time) << 20;
			one ^= static_cast<u64>(entry->crc) << 32;
			one ^= entry->is_favorite ? 0x2545f4914f6cdd1dull : 0;
			sum += one * 0x100000001b3ull;
		}
		return sum;
	}

	std::unordered_map<std::string, std::string> m_cover_paths;
	// The discs whose pictures were asked for in this run, and how many fetches have come back.
	std::set<std::string> m_disc_art_asked;
	std::atomic<unsigned> m_disc_art_done{0};
	unsigned m_disc_art_seen = 0;
	// Which games have which pictures (1 a title screen, 2 a picture of play), by serial.
	std::unordered_map<std::string, int> m_pictures;
	bool m_pictures_listed = false;
	std::string m_pictures_asked; // the game whose pictures were last asked for by itself
	// The cheat being typed in: its name, and the lines accepted so far.
	std::string m_typed_name;
	std::vector<std::string> m_typed_lines;
	Wallpapers m_wallpapers;
	// The corner the emulator's figures are sent to (-1: where the settings have them), the one
	// they were last put in, and where the settings had them.
	int m_osd_corner = -1, m_osd_sent = -1;
	bool m_osd_moved = false;
	OsdOverlayPos m_osd_own = OsdOverlayPos::None;
	u64 m_games_signature = 0;
	u32 m_games_count = 0;
	u64 m_frame = 0;

	// What is asked on every frame of the quick menu and answered from a look twice a second.
	Common::Timer m_undo_age, m_cheevos_age;
	bool m_undo_can = false;
	int m_cheevos_has = 0;

	std::string m_states_key;
	std::vector<bp::StateInfo> m_states;
	Common::Timer m_states_age;
	std::map<std::string, StatePicture> m_state_pictures;
	std::vector<std::unique_ptr<GSTexture>> m_retired;
	// What was put aside the frame before: let go of at the start of the next (BeginFrame).
	std::vector<std::unique_ptr<GSTexture>> m_retired_before;

	// The game whose own settings are open.
	std::unique_ptr<INISettingsInterface> m_game_si;
	bp::GameInfo m_game;
	std::string m_game_serial;
	bool m_own_changed = false;
	int m_own_count = 0;
	std::vector<Patch::PatchInfo> m_patches, m_cheats;
	u32 m_unlabelled = 0;

	unsigned m_pages = 1;
	// The backups: the place whose list was asked for, and how many lists had been read when the
	// pages were last made.
	std::string m_backup_asked;
	unsigned m_backup_lists = 0;
	// What the run before this one ended in, when it was a crash (read once, as the library opens).
	bool m_crash_looked = false;
	std::string m_crash_notice;
	std::vector<const HotkeyInfo*> m_hotkeys;
	bp::DiscState m_disc;
	bool m_disc_lent = false; // the drive was given to a start from the disc
	unsigned m_disc_seen = ~0u;
	std::map<std::string, u32> m_disc_crcs;
};

PCSX2Backend s_backend;
bool s_initialized = false;
bool s_tried_to_initialize = false;
bool s_front_started = false;

} // namespace

// ------------------------------------------------------------------------ from the host

void PS5UI::SetLoadProgress(std::string text, float progress)
{
	const std::lock_guard lock(s_told_mutex);
	s_load_text = std::move(text);
	s_load_progress = progress;
}

void PS5UI::SetBackgroundNote(std::string note)
{
	const std::lock_guard lock(s_told_mutex);
	s_background_note = std::move(note);
}

void PS5UI::SetScanning(bool scanning)
{
	s_scanning.store(scanning, std::memory_order_release);
	// A scan that ended has found what it found.
	if (!scanning)
		s_games_generation.fetch_add(1, std::memory_order_acq_rel);
}

void PS5UI::ControllerChanged(int difference)
{
	s_controllers.fetch_add(difference, std::memory_order_acq_rel);
}

void PS5UI::CheatsChanged()
{
	s_cheats_changed.store(true, std::memory_order_release);
}

void PS5UI::PagesChanged()
{
	s_pages_changed.store(true, std::memory_order_release);
}

void PS5UI::StatesChanged()
{
	s_states_changed.store(true, std::memory_order_release);
}

void PS5UI::PicturesChanged()
{
	s_pictures_changed.store(true, std::memory_order_release);
}

void PS5UI::Toast(std::string text)
{
	const std::lock_guard lock(s_told_mutex);
	if (s_told_toasts.size() < 8)
		s_told_toasts.push_back(std::move(text));
}

void PS5UI::Message(std::string title, std::string text)
{
	const std::lock_guard lock(s_told_mutex);
	if (s_told_messages.size() < 8)
		s_told_messages.emplace_back(std::move(title), std::move(text));
}

// ------------------------------------------------- PCSX2's FullscreenUI, as the console has it

bool FullscreenUI::Initialize()
{
	if (s_initialized)
		return true;
	if (s_tried_to_initialize)
		return false;

	ImGuiFullscreen::SetTheme("Dark");
	ImGuiFullscreen::UpdateLayoutScale();
	ImGuiFullscreen::UpdateFontScale();
	if (!ImGuiFullscreen::Initialize("fullscreenui/placeholder.svg"))
	{
		ImGuiFullscreen::Shutdown(true);
		s_tried_to_initialize = true;
		return false;
	}

	s_initialized = true;
	s_backend.SetHotkeys(InputManager::GetHotkeyList());
	// The graphics thread draws when no game gives it frames to.
	MTGS::SetRunIdle(true);

	bp::setFonts(ps5px_interface_font(0), ps5px_interface_font(1));
	bp::setBrandFont(ps5px_interface_font(2));
	if (!s_front_started)
	{
		bp::initialize(&s_backend);
		s_front_started = true;
		// No memory card is changed once a game is being started (ps5_cards.h).
		PS5Cards::SetWriteGuard([] { return !s_boot_pending.load(std::memory_order_acquire) && VMManager::GetState() == VMState::Shutdown; });
	}
	return true;
}

bool FullscreenUI::IsInitialized()
{
	return s_initialized;
}

void FullscreenUI::ReloadSvgResources()
{
	// The pictures made of drawings are made again, at their size, when next asked for.
}

bool FullscreenUI::HasActiveWindow()
{
	return s_initialized && (bp::active() || s_overlay.load(std::memory_order_acquire) != 0);
}

void FullscreenUI::CheckForConfigChanges(const Pcsx2Config& old_config)
{
}

// (These come from the emulation thread. Whether the interface is there is asked on the graphics
// thread, when the event arrives: a game whose settings ask for another renderer has the device,
// and the interface with it, made anew while it starts, and the event must not be dropped for
// arriving in between. The screens do not depend on any one of them: they also follow the
// machine's state as they find it on each frame.)
void FullscreenUI::OnVMStarted()
{
	s_session_started.store(std::time(nullptr), std::memory_order_release);
	PS5UI::SetLoadProgress(std::string(), -1.f);
	if (!MTGS::IsOpen())
		return;
	MTGS::RunOnGSThread([]() {
		if (IsInitialized())
			bp::gameStarted();
	});
}

void FullscreenUI::OnVMDestroyed()
{
	s_overlay.store(0, std::memory_order_release);
	PS5UI::SetLoadProgress(std::string(), -1.f);
	if (!MTGS::IsOpen())
		return;
	MTGS::RunOnGSThread([]() {
		if (!IsInitialized())
			return;
		s_backend.ForgetStates();
		bp::gameStopped();
	});
}

void FullscreenUI::OnVMResumed()
{
	if (!MTGS::IsOpen())
		return;
	MTGS::RunOnGSThread([]() {
		if (IsInitialized())
			bp::gameResumed();
	});
}

// (On the emulation thread: when a game starts, when its disc is changed, and with nothing in
// them when it is closed. Kept here and now, not queued to the graphics thread: the emulator
// empties that thread's queue when it resets the machine, which it does right after the first of
// these.)
void FullscreenUI::GameChanged(std::string path, std::string serial, std::string title, u32 disc_crc, u32 crc)
{
	const std::lock_guard lock(s_told_mutex);
	s_game_path = std::move(path);
	s_game_serial = std::move(serial);
	s_game_title = std::move(title);
	s_game_crc = disc_crc;
}

void FullscreenUI::OpenPauseMenu()
{
	if (!VMManager::HasValidVM())
		return;

	if (SaveStateSelectorUI::IsOpen())
	{
		SaveStateSelectorUI::Close();
		return;
	}

	MTGS::RunOnGSThread([]() {
		if (ImGuiManager::InitializeFullscreenUI())
			bp::openQuickMenu();
	});
}

// PCSX2's own lists of a game's achievements and leaderboards, over the paused game. From the
// quick menu they are opened by the Backend; these are for the emulator's hotkeys (the emulation
// thread), which open the quick menu under them first, so that the game is held and closing the
// list has somewhere to return to.
namespace
{
	bool OpenOverlayFromHotkey(int which)
	{
		if (!VMManager::HasValidVM() || !Achievements::IsActive() || !MTGS::IsOpen())
			return false;
		MTGS::RunOnGSThread([which]() {
			if (!ImGuiManager::InitializeFullscreenUI())
				return;
			if (!bp::active())
				bp::openQuickMenu();
			if (bp::active())
				s_backend.openAchievements(which);
		});
		return true;
	}
} // namespace

bool FullscreenUI::OpenAchievementsWindow()
{
	return OpenOverlayFromHotkey(1);
}

bool FullscreenUI::OpenLeaderboardsWindow()
{
	return OpenOverlayFromHotkey(2);
}

bool FullscreenUI::IsAchievementsWindowOpen()
{
	return s_overlay.load(std::memory_order_acquire) == 1;
}

bool FullscreenUI::IsLeaderboardsWindowOpen()
{
	return s_overlay.load(std::memory_order_acquire) == 2;
}

// (The list's own way out: Circle, or its close button. The quick menu is under it.)
void FullscreenUI::ReturnToPreviousWindow()
{
	s_overlay.store(0, std::memory_order_release);
}

// (From the emulation thread, when the game's achievements are taken away under the list.)
void FullscreenUI::ReturnToMainWindow()
{
	s_overlay.store(0, std::memory_order_release);
}

// The line at the foot of PCSX2's lists: which button does what.
void FullscreenUI::SetStandardSelectionFooterText(bool back_instead_of_cancel)
{
	ImGuiFullscreen::SetFullscreenFooterText(
		std::string_view(back_instead_of_cancel ? "Directions: move     Cross: open     Circle: back" : "Directions: move     Cross: choose     Circle: cancel"));
}

void FullscreenUI::LocaleChanged()
{
}

void FullscreenUI::GamepadLayoutChanged()
{
}

void FullscreenUI::PreferEnglishGameListChanged()
{
	s_games_generation.fetch_add(1, std::memory_order_acq_rel);
}

namespace
{
	// A state that could not be loaded or saved: said in a dialog, over the game held still.
	void ReportStateError(std::string title, std::string message)
	{
		std::function<void()> then;
		if (VMManager::GetState() == VMState::Running)
		{
			Host::RunOnCPUThread([]() { VMManager::SetPaused(true); });
			then = []() { Host::RunOnCPUThread([]() { VMManager::SetPaused(false); }); };
		}
		bp::message(std::move(title), std::move(message), std::move(then));
	}
} // namespace

// (Both may be called from the thread that packs a state's file. Only the emulation thread may
// queue work for the graphics thread, whose queue has one writer: so they go through it.)
void FullscreenUI::ReportStateLoadError(const std::string& message, std::optional<s32> slot, bool backup)
{
	Host::RunOnCPUThread([message, slot, backup]() {
		if (!MTGS::IsOpen())
			return;
		MTGS::RunOnGSThread([message, slot, backup]() {
			if (!ImGuiManager::InitializeFullscreenUI())
			{
				SaveState_ReportLoadErrorOSD(message, slot, backup);
				return;
			}
			ReportStateError(slot.has_value() ? fmt::format("The state of slot {}{} could not be loaded", *slot, backup ? " (the one before)" : "") :
												std::string("The state could not be loaded"),
				message);
		});
	});
}

void FullscreenUI::ReportStateSaveError(const std::string& message, std::optional<s32> slot)
{
	Host::RunOnCPUThread([message, slot]() {
		if (!MTGS::IsOpen())
			return;
		MTGS::RunOnGSThread([message, slot]() {
			if (!ImGuiManager::InitializeFullscreenUI())
			{
				SaveState_ReportSaveErrorOSD(message, slot);
				return;
			}
			ReportStateError(slot.has_value() ? fmt::format("The state could not be saved to slot {}", *slot) : std::string("The state could not be saved"),
				message);
		});
	});
}

void FullscreenUI::Shutdown(bool clear_state)
{
	if (s_listening.load(std::memory_order_acquire))
		StopListening();
	s_backend.ReleaseTextures();
	if (clear_state)
	{
		if (s_front_started)
		{
			s_backend.EndFrame();
			bp::shutdown();
			s_front_started = false;
		}
		s_backend.Reset();
	}
	ImGuiFullscreen::Shutdown(clear_state);
	s_initialized = false;
	s_tried_to_initialize = false;
}

void FullscreenUI::Render()
{
	if (!s_initialized)
		return;

	s_backend.BeginFrame();
	ImGuiFullscreen::UploadAsyncTextures();
	ImGuiFullscreen::BeginLayout();

	// (The emulator makes its fonts again when the display's scale changes.)
	bp::setFonts(ps5px_interface_font(0), ps5px_interface_font(1));
	bp::setBrandFont(ps5px_interface_font(2));

	// RetroAchievements while a game is played with no screen over it: the challenges under way
	// and a leaderboard's running time, in their corner, under whatever else is drawn.
	int overlay = s_overlay.load(std::memory_order_acquire);
	if (overlay != 0 && !VMManager::HasValidVM())
	{
		s_overlay.store(0, std::memory_order_release);
		overlay = 0;
	}
	if (overlay == 0 && !bp::active() && VMManager::HasValidVM())
		Achievements::DrawGameOverlays();

	// (While one of PCSX2's lists is up the screens draw nothing and read no button.)
	bp::render();
	if (overlay == 1)
		Achievements::DrawAchievementsWindow();
	else if (overlay == 2)
		Achievements::DrawLeaderboardsWindow();

	// What the emulator posts through its own toolkit (notifications: an achievement earned) is
	// drawn by it.
	ImGuiFullscreen::EndLayout();
	s_backend.EndFrame();
	ImGuiFullscreen::ResetCloseMenuIfNeeded();
}

void FullscreenUI::InvalidateCoverCache()
{
	if (!MTGS::IsOpen())
		return;
	MTGS::RunOnGSThread([]() { s_backend.ForgetCovers(); });
}

TinyString FullscreenUI::TimeToPrintableString(time_t t)
{
	struct tm lt = {};
	localtime_r(&t, &lt);

	TinyString ret;
	std::strftime(ret.data(), ret.buffer_size(), "%c", &lt);
	ret.update_size();
	return ret;
}

bool FullscreenUI::CreateHardDriveWithProgress(const std::string& filePath, int sizeInGB, bool use48BitLBA)
{
	// The PlayStation 2's hard drive is not offered on the console.
	return false;
}

void FullscreenUI::CancelAllHddOperations()
{
}

// The help lines of the toolkit's own dialogs (none of which the console's interface opens).
void ImGuiFullscreen::GetChoiceDialogHelpText(SmallStringBase& dest)
{
	dest.clear();
}

void ImGuiFullscreen::GetFileSelectorHelpText(SmallStringBase& dest)
{
	dest.clear();
}

void ImGuiFullscreen::GetInputDialogHelpText(SmallStringBase& dest)
{
	dest.clear();
}
