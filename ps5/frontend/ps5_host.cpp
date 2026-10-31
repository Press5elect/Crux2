// Crux2 - the host: start-up, settings, the emulation thread's loop, and what PCSX2 asks
// of the application it runs in.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The title is the console's interface (bigpicture/) and nothing else: there is no desktop
// interface to return to. The main thread sets the title up and then waits; the emulation ("CPU") thread
// starts the interface, runs games, and between games polls the controllers while PCSX2's
// graphics thread draws the interface.

#include "ps5_extras.h"
#include "ps5_priority.h"
#include "../common/PS5Timing.h"
#include "ps5_platform.h"
#include "ps5_pgo.h"
#include "ps5_keyboard.h"
#include "ps5_services.h"
#include "ps5_pacing.h"
#include "ps5_boot_step.h"
#include "ps5_pgs.h"
#include "ps5_update.h"
#include "ps5_presets.h"

#include "platform.h"
#include "ps5_input_source.h"
#include "ps5_letter_jump.h"
#include "ps5_disc.h"
#include "ps5_native.h"
#include "ps5_auto_quality.h"
#include "ps5_jobs.h"
#include "ps5_online.h"
#include "ps5_ui.h"
#include "ps5_ui_audio.h"
#include "bigpicture/bp.h"
#include "ps5_vfs.h"

#include "Achievements.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GameList.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Sio.h"
#include "SIO/Pad/Pad.h"
#include "VMManager.h"
#include "svnrev.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include <ps5platform/kernel.h>
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "common/WindowInfo.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <thread>
#include <vector>

extern "C" int sceKernelUsleep(unsigned microseconds);
extern "C" void ps5px_publish_thread_cpu_time(void);
extern "C" void ps5px_profiler_set_window_extra(void (*extra)(void));

namespace PS5Host
{
	/// The display: what the console's Vulkan driver offers a title (the swap chain corrects this
	/// from the display mode it finds).
	constexpr u32 DISPLAY_WIDTH = 3840;
	constexpr u32 DISPLAY_HEIGHT = 2160;
	constexpr float DISPLAY_REFRESH_RATE = 59.94f;

	/// How long the idle emulation thread sleeps between two polls of the controllers.
	constexpr auto IDLE_POLL_INTERVAL = std::chrono::milliseconds(8);

	/// Settings are written this long after the last change, so a slider being dragged is one write.
	constexpr double SETTINGS_SAVE_DELAY = 1.0;

	/// The emulation thread's stack: the recompilers are entered from it, and microVU recurses.
	constexpr u32 CPU_THREAD_STACK_SIZE = 8 * 1024 * 1024;

	/// Written to [PS5] DefaultsVersion with the port's defaults: a settings file without it has
	/// never had them. (A later version that changes a default migrates that one key; it never
	/// applies the whole set again, which would undo the user's own choices.)
	/// 2 (v0.7.0): the first name server is PS2 Online's.
	/// 3 (v1.0.0): frames paced by the screen (SetPacingDefaults).
	/// 4 (v1.2.0): thread pinning on, one emulator thread per physical core.
	/// 5 (v1.2.1): thread pinning off again: fast forward lost speed with it on the console.
	/// 6 (v1.2.2): the graphics driver records on its own thread (40.4 -> 48.4 fps measured).
	/// 7 (v1.6.5): the rasterizer ordered view on: the higher blending accuracies without a barrier
	///    per draw (Black: 10,000-12,500 barriers a frame, the GPU at 100%).
	/// 8 (the first release): the release's defaults (SetReleaseDefaults), each only where the file has no
	///    value of its own for it.
	/// 9 (v1.0.2): the figures' place back at the top right where the first release left it "not
	///    shown" (no figure is on by default, so nothing appears; switching one on showed nothing).
	constexpr int PS5_DEFAULTS_VERSION = 9;

	/// A test run's measuring window, and how long the last frame is given to reach its screenshot.
	constexpr double TEST_WINDOW_SECONDS = 5.0;
	constexpr double TEST_SCREENSHOT_SECONDS = 2.0;

	static bool InitializeConfig();
	static bool LoadFonts();
	static void SetPS5Defaults(SettingsInterface& si);
	static void SetReleaseDefaults(SettingsInterface& si, bool only_missing);
	static void SetPS5InputDefaults(SettingsInterface& si);
	static void ApplyGameFolders();
	static void LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message);
	static void ProcessCPUThreadEvents();
	static void SaveSettingsIfDue(bool force);
	static void CPUThreadEntryPoint();
	static void CPUThreadMainLoop();
	static bool IsOnCPUThread();
	static void StartGameListRefresh(bool invalidate_cache);
	static void OnGameListRefreshDone(u32 generation);
	static void GameListRefreshThreadEntryPoint(bool invalidate_cache, u32 generation);
	static void ShowMessage(std::string title, std::string message, const char* icon, float osd_duration);
	static void ShowToast(std::string message);
	static void SetNameServerDefault(SettingsInterface& si);
	static void SetPacingDefaults(SettingsInterface& si);
	static void SetOrderedViewDefault(SettingsInterface& si);
	static void DownloadMissingCovers(ProgressCallback* progress);
	static void DownloadMissingPictures(ProgressCallback* progress);
	static void ReadTestRun();
	static void TestRunFrame();
	static void TestRunIdle();
	static void TestRunTick();
	static void TestRunSummary();

	namespace
	{
		/// The game list scan's cancel flag, and its messages into the boot log. Nothing is drawn
		/// while it runs: the interface's progress dialog is modal, and a scan of a share that does
		/// not answer would hold the whole interface for as long as it takes. The list fills in as
		/// the scan finds games.
		class GameListProgressCallback final : public BaseProgressCallback
		{
		public:
			bool IsCancelled() const override;

			void SetTitle(const char* title) override {}
			void SetStatusText(const char* text) override { BaseProgressCallback::SetStatusText(text); }
			void SetProgressRange(u32 range) override { BaseProgressCallback::SetProgressRange(range); }
			void SetProgressValue(u32 value) override { BaseProgressCallback::SetProgressValue(value); }

			void DisplayError(const char* message) override { ps5px_log("game list: error: %s", message); }
			void DisplayWarning(const char* message) override { ps5px_log("game list: warning: %s", message); }
			void DisplayInformation(const char* message) override {}
			void DisplayDebugMessage(const char* message) override {}

			void ModalError(const char* message) override { ps5px_log("game list: error: %s", message); }
			bool ModalConfirmation(const char* message) override { return false; }
			void ModalInformation(const char* message) override {}
		};

		struct State
		{
			std::unique_ptr<INISettingsInterface> base_settings;
			std::unique_ptr<INISettingsInterface> secrets_settings;

			std::atomic_bool quit_requested{false};
			bool save_state_on_shutdown = false;
			Threading::Thread cpu_thread;
			pthread_t cpu_pthread{};
			std::atomic_bool cpu_thread_running{false};

			// Functions queued for the emulation thread; the bool marks one its caller waits for.
			std::mutex events_mutex;
			std::condition_variable events_cv;
			std::condition_variable events_done_cv;
			std::deque<std::pair<std::function<void()>, bool>> events;
			u32 blocking_events_pending = 0;

			std::atomic_bool settings_dirty{false};
			Common::Timer::Value settings_dirty_time = 0;

			// The game list scan: started, replaced and joined by the emulation thread alone (requests
			// from other threads are queued to it), so none of this needs a lock. A request made
			// while a scan runs cancels it and is started when that scan's thread reports back.
			std::thread game_list_thread;
			std::atomic_bool game_list_cancel{false};
			// "Download Missing Covers Now": the next scan fetches covers whether the setting is on or not.
			std::atomic_bool force_cover_download{false};
			std::atomic_bool force_picture_download{false};
			bool game_list_active = false;
			u32 game_list_generation = 0;
			int game_list_pending = -1; // -1: none, 0: a scan, 1: a scan that forgets the cache

			// The font files, which ImGui reads from for as long as it has the fonts.
			std::vector<u8> font_latin;
			std::vector<u8> font_emoji;
		};

		/// A test run (ps5/tools/run.sh): the launch found test-run.txt in the title folder. Read by
		/// the main thread before the emulation thread exists, and used by that thread alone after.
		struct TestRun
		{
			enum class Phase
			{
				Start,
				Running,
				Screenshot,
				Stopping,
				Done,
			};

			bool active = false;
			std::string game;
			double seconds = 30.0;
			bool screenshot = false;
			bool failed = false;
			// Played by hand: the run measures until the title is closed from its interface.
			bool by_hand = false;
			// 0: no profiler; 1: the graphics thread; 2: the emulation and VU threads as well.
			int profile = 0;
			bool threaded_recording = false;
			bool interface_noted = false;
			// Whether a game is being emulated right now (not: none, or a paused one): set by the
			// first frame, cleared by the emulation thread's idle loop.
			bool game_running = false;

			Phase phase = Phase::Start;
			bool stop_requested = false;
			Common::Timer total;
			Common::Timer window;
			Common::Timer frame;
			Common::Timer screenshot_wait;
			u32 window_index = 0;

			// This window's emulated frames, and all of the run's after the first window (the boot).
			u32 frames = 0;
			double worst_ms = 0.0;
			u32 over_40ms = 0;
			unsigned long long audio_played = 0;
			unsigned long long audio_short = 0;
			u32 steady_frames = 0;
			double steady_seconds = 0.0;
			double steady_worst_ms = 0.0;
			u32 steady_over_40ms = 0;
		};

		TestRun s_test;

		State s_state;
	} // namespace
} // namespace PS5Host

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

void PS5Host::SetPS5Defaults(SettingsInterface& si)
{
	// The console has one renderer, and a GPU with room for more than the PlayStation 2's own
	// resolution: four times it (2560x1792 for a 640x448 game). This build's speed at that has not
	// been measured on a console yet (needs proper testing): it is a cautious step below the six
	// times Mihawk's LRPS2 core held on the same driver. Settings > Graphics has the multiplier.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));

	// Nothing that trades accuracy for speed is switched on here: the speed hacks stay as PCSX2
	// ships them (multi-threaded VU1 off). They are the user's to turn on, by name, in Settings.

	// There is one output, and it is a television.
	si.SetBoolValue("UI", "StartFullscreen", true);
	si.SetBoolValue("UI", "ConfirmShutdown", true);
	si.SetBoolValue("UI", "PauseOnFocusLoss", false);
	si.SetBoolValue("UI", "InhibitScreensaver", true);

	// No keyboard to type a RetroAchievements password with, no Discord, no update check.
	si.SetBoolValue("AutoUpdater", "CheckAtStartup", false);
	si.SetBoolValue("EmuCore", "EnableDiscordPresence", false);

	// The emulator's log in the title folder's logs/, which is the file a user is asked for.
	si.SetBoolValue("Logging", "EnableFileLogging", true);
	si.SetBoolValue("Logging", "EnableSystemConsole", false);

	si.SetBoolValue("PS5", "USBDrives", true);
	si.SetBoolValue("PS5", "StartupSound", true);
	si.SetIntValue("PS5", "StartupVolume", 80);
	si.SetBoolValue("PS5", "BackgroundSound", true);
	si.SetIntValue("PS5", "BackgroundVolume", 40);
	si.SetBoolValue("PS5", "DownloadCovers", false);
	SetNameServerDefault(si);
	SetPacingDefaults(si);
	// Thread pinning stays off (as PCSX2 ships it): with it on (v1.2.0), fast forward lost speed on
	// the console. Only PCSX2's three threads are pinned, and every other thread may still land on
	// the other hardware thread of their cores, where a pinned thread cannot move away from it.
	si.SetBoolValue("EmuCore", "EnableThreadPinning", false);
	// The graphics driver records the renderer's commands on a thread of its own: in a scene where
	// the graphics thread was the limit (measured on the console, 2026-10-03), 40.4 frames a second
	// became 48.4.
	si.SetBoolValue("PS5", "ThreadedRecording", true);
	SetOrderedViewDefault(si);
	SetReleaseDefaults(si, false);
	si.SetIntValue("PS5", "DefaultsVersion", PS5_DEFAULTS_VERSION);
}

// The settings a release starts with (as I chose them for the first release): the picture at 6x,
// enlarged by FSR 1 from 4x, the resolution lowered by itself where the graphics chip falls short,
// the widescreen and no-interlacing patches, the largest output at 60 Hz paced by the screen with no
// frame queued, no on-screen display, a memory card for each game, the main threads on cores of
// their own, and texture packs loaded and held in memory. With `only_missing` (a settings file of
// an earlier version) a key the file already has keeps its value.
void PS5Host::SetReleaseDefaults(SettingsInterface& si, bool only_missing)
{
	const auto b = [&](const char* section, const char* key, bool value) {
		if (!only_missing || !si.ContainsValue(section, key))
			si.SetBoolValue(section, key, value);
	};
	const auto i = [&](const char* section, const char* key, int value) {
		if (!only_missing || !si.ContainsValue(section, key))
			si.SetIntValue(section, key, value);
	};
	const auto f = [&](const char* section, const char* key, float value) {
		if (!only_missing || !si.ContainsValue(section, key))
			si.SetFloatValue(section, key, value);
	};
	constexpr const char* GS = "EmuCore/GS";
	// Graphics.
	f(GS, "upscale_multiplier", 6.0f);
	i(GS, "CASMode", 5); // FSR 1 from 4x (GSCASMode: 5)
	b(GS, "PS5AutoQuality", true);
	b(GS, "pcrtc_antiblur", true);
	b(GS, "disable_interlace_offset", true);
	b(GS, "HWROV", true);
	i(GS, "FrameGeneration", 0);
	b(GS, "LoadTextureReplacements", true);
	b(GS, "LoadTextureReplacementsAsync", true);
	b(GS, "PrecacheTextureReplacements", true);
	b("EmuCore", "EnableWideScreenPatches", true);
	b("EmuCore", "EnableNoInterlacingPatches", true);
	// The output: as large as the console makes it, at 60 Hz, paced by the screen, nothing queued.
	i("PS5", "OutputHeight", 2160);
	i("PS5", "OutputRefresh", 60);
	b(GS, "VsyncEnable", true);
	b(GS, "UseVSyncForTiming", true);
	b(GS, "SyncToHostRefreshRate", true);
	i(GS, "VsyncQueueSize", 0);
	// No on-screen display: neither messages nor figures nor indicators.
	i(GS, "OsdMessagesPos", 0);
	i(GS, "OsdPerformancePos", 3); // (the figures' place; none of them is switched on)
	b(GS, "OsdShowIndicators", false);
	b(GS, "OsdShowTextureReplacements", false);
	// The emulator.
	b("EmuCore", "EnableFastBoot", true);
	b("EmuCore", "EnableGameFixes", true);
	b("EmuCore", "EnablePatches", true);
	b("PS5", "GameDbRecommended", true);
	b("EmuCore", "EnableThreadPinning", false);
	b("PS5", "ThreadLayout", true); // Thread pinning: cores of their own (Crux2)
	// Memory cards and saving.
	b("PS5", "CardPerGame", true);
	b("PS5", "SleepSafe", true);
	// The console.
	b("PS5", "ThreadPriorities", false);
	b("PS5", "FastPipelines", true);
	b("PS5", "ThreadedRecording", true);
	b("PS5", "UpdateAtStart", true);
	// Care for the screen (the interface's own section).
	i("PS5UI", "IdleDim", 3);
	b("PS5UI", "ScreenSaver", true);
	b("PS5UI", "HintFade", true);
}

void PS5Host::SetOrderedViewDefault(SettingsInterface& si)
{
	// The console's GPU orders overlapping pixels in hardware (RADV's primitive-ordered pixel
	// shading, VK_EXT_fragment_shader_interlock): with the rasterizer ordered view a draw that reads
	// what is under it needs no barrier. PCSX2 uses it only where it saves barriers. Without it, a
	// game with the higher blending accuracies stops the GPU before nearly every draw (Black at Full:
	// 10,000-12,500 barriers a frame, the GPU at 100%, 56-71% speed on the console, 2026-10-21).
	si.SetBoolValue("EmuCore/GS", "HWROV", true);
}

void PS5Host::SetPacingDefaults(SettingsInterface& si)
{
	// A game's frames are paced by the screen: the emulator waits for the display's refresh
	// rather than for a clock, so each frame stays on the screen for the same number of
	// refreshes (ps5_pacing.cpp; at 119.88 Hz each is held for two). Settings > Picture >
	// "Frame pacing" goes back to the clock, which a game at another rate than the screen's (a
	// 50 Hz one) uses by itself. Repeated frames are shown, not skipped: a skipped present would
	// let the emulator run ahead of the screen.
	si.SetBoolValue("EmuCore/GS", "VsyncEnable", true);
	si.SetBoolValue("EmuCore/GS", "SkipDuplicateFrames", false);
	si.SetBoolValue("EmuCore/GS", "SyncToHostRefreshRate", true);
	si.SetBoolValue("EmuCore/GS", "UseVSyncForTiming", true);
}

void PS5Host::SetNameServerDefault(SettingsInterface& si)
{
	// The PlayStation 2's first name server: the one of the service that keeps the online play of
	// the most games going (ps5_presets.h). It is asked only by a game that uses the network
	// adapter, which is off until switched on; Settings > Network chooses another, or the
	// console's own.
	si.SetStringValue("DEV9/Eth", "ModeDNS1", "Manual");
	si.SetStringValue("DEV9/Eth", "DNS1", PS5Presets::NAME_SERVERS[0].address);
}

void PS5Host::SetPS5InputDefaults(SettingsInterface& si)
{
	// The console's controllers in place of the keyboard, and the Options button for the pause
	// menu: Pad.cpp's defaults on this platform, the same ones "Reset Settings" on the interface's
	// controller page applies.
	Pad::SetDefaultControllerConfig(si);
	Pad::SetDefaultHotkeyConfig(si);
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	si.SetBoolValue("UI", "InhibitScreensaver", true);
	si.SetBoolValue("UI", "ConfirmShutdown", true);
	si.SetBoolValue("UI", "StartPaused", false);
	si.SetBoolValue("UI", "PauseOnFocusLoss", false);
	si.SetBoolValue("UI", "StartFullscreen", true);
	si.SetBoolValue("UI", "PreferEnglishGameList", false);
}

void Host::CommitBaseSettingChanges()
{
	PS5Host::s_state.settings_dirty_time = Common::Timer::GetCurrentValue();
	PS5Host::s_state.settings_dirty.store(true, std::memory_order_release);
}

void PS5Host::SaveSettingsIfDue(bool force)
{
	if (!s_state.settings_dirty.load(std::memory_order_acquire))
		return;
	if (!force && Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_state.settings_dirty_time) < SETTINGS_SAVE_DELAY)
		return;

	s_state.settings_dirty.store(false, std::memory_order_release);

	Error error;
	const auto lock = Host::GetSettingsLock();
	if (!s_state.base_settings->Save(&error))
		ps5px_log("settings: cannot save %s: %s", s_state.base_settings->GetFileName().c_str(), error.GetDescription().c_str());
	else
		PS5Platform::MakePublic(s_state.base_settings->GetFileName().c_str(), false);
}

bool PS5Host::LoadFonts()
{
	const auto read = [](const char* name, std::vector<u8>* data) {
		const std::string path = EmuFolders::GetOverridableResourcePath(fmt::format("fonts" FS_OSPATH_SEPARATOR_STR "{}", name));
		std::optional<std::vector<u8>> file = FileSystem::ReadBinaryFile(path.c_str());
		if (!file.has_value() || file->empty())
		{
			ps5px_log("fonts: cannot read %s", path.c_str());
			return false;
		}

		*data = std::move(file.value());
		return true;
	};

	if (!read("Roboto-Regular.ttf", &s_state.font_latin))
		return false;

	std::vector<ImGuiManager::FontInfo> fonts;
	ImGuiManager::FontInfo& latin = fonts.emplace_back();
	latin.data = s_state.font_latin;
	latin.exclude_ranges = {};
	latin.face_name = nullptr;
	latin.is_emoji_font = false;

	// The emoji of game titles and messages; the interface does without them if the file is gone.
	if (read("Twemoji.Mozilla.ttf", &s_state.font_emoji))
	{
		ImGuiManager::FontInfo& emoji = fonts.emplace_back();
		emoji.data = s_state.font_emoji;
		emoji.exclude_ranges = {};
		emoji.face_name = nullptr;
		emoji.is_emoji_font = true;
	}

	ImGuiManager::SetFonts(std::move(fonts));
	return true;
}

bool PS5Host::InitializeConfig()
{
	Error error;

	// Everything the title keeps is in its own folder: settings, memory cards, save states.
	EmuConfig.IsPortableMode = true;
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory())
	{
		ps5_title_fatal("The title folder has no resources folder. Copy the whole PPSA99249 folder to the console again.");
	}
	if (!EmuFolders::SetDataDirectory(&error))
	{
		ps5px_log("settings: no data folder at %s: %s", EmuFolders::DataRoot.c_str(), error.GetDescription().c_str());
		return false;
	}
	// Outside the title folder, when the folder for that is there (ps5_platform.h, OutsideFolder).
	if (const std::string& outside = PS5Platform::OutsideFolder(); !outside.empty())
	{
		const std::string inis = Path::Combine(outside, "inis");
		if (FileSystem::EnsureDirectoryExists(inis.c_str(), false, &error))
		{
			EmuFolders::DataRoot = outside;
			EmuFolders::Settings = inis;
			PS5Platform::MakePublic(inis.c_str(), true);
		}
		else
		{
			ps5px_log("data folder: %s could not be made (%s): the title folder is used", inis.c_str(), error.GetDescription().c_str());
		}
	}
	ps5px_log("data folder: %s", EmuFolders::DataRoot.c_str());
	PS5Pgo::Start(EmuFolders::DataRoot);
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	std::string path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
	const bool settings_exist = FileSystem::FileExists(path.c_str());
	ps5px_log("settings: %s%s", path.c_str(), settings_exist ? "" : " (first run)");

	s_state.base_settings = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_state.base_settings.get());

	SettingsInterface& si = *s_state.base_settings;
	const bool loaded = settings_exist && s_state.base_settings->Load() && VMManager::Internal::CheckSettingsVersion();
	if (!loaded)
	{
		if (settings_exist)
		{
			// A settings file that cannot be read (cut off by a power loss, say) or is of another
			// version: keep it for inspection and start from defaults, rather than refuse to start
			// on a console with no editor at hand.
			ps5px_log("settings: the file could not be used; starting from defaults (the old file is kept as PCSX2.ini.broken)");
			const std::string broken = s_state.base_settings->GetFileName() + ".broken";
			FileSystem::DeleteFilePath(broken.c_str());
			FileSystem::RenamePath(s_state.base_settings->GetFileName().c_str(), broken.c_str());
			s_state.base_settings->Clear();
		}

		VMManager::SetDefaultSettings(si, true, true, true, true, true);
		SetPS5Defaults(si);
		// The setup wizard is the desktop interface's: the BIOS and games folders are fixed here.
		si.SetBoolValue("UI", "SetupWizardIncomplete", false);

		if (!s_state.base_settings->Save(&error))
			ps5px_log("settings: cannot save: %s", error.GetDescription().c_str());
	}
	else if (const int version = si.GetIntValue("PS5", "DefaultsVersion", 0); version >= 1 && version < PS5_DEFAULTS_VERSION)
	{
		// The settings of an earlier build: only the defaults that changed since, one version
		// after another; everything else stays as chosen.
		if (version < 2)
		{
			// Before v0.7.0: a name server that was chosen by hand stays; "the console's own",
			// which is what every installation had without choosing, becomes PS2 Online's.
			const std::string mode = si.GetStringValue("DEV9/Eth", "ModeDNS1", "Auto");
			if (mode != "Manual")
			{
				SetNameServerDefault(si);
				ps5px_log("settings: the first name server is %s's now (%s); Settings > Network changes it",
					PS5Presets::NAME_SERVERS[0].name, PS5Presets::NAME_SERVERS[0].address);
			}
		}
		if (version < 3)
		{
			SetPacingDefaults(si);
			ps5px_log("settings: frames are paced by the screen now; Settings > Picture > Frame pacing changes it");
		}
		if (version < 6)
		{
			// On, unless it was switched in Settings (a value written there stays as it is).
			if (!si.ContainsValue("PS5", "ThreadedRecording"))
			{
				si.SetBoolValue("PS5", "ThreadedRecording", true);
				ps5px_log("settings: the graphics driver records on its own thread now; Settings > Graphics changes it");
			}
		}
		if (version < 7)
		{
			// On, unless it was switched in Settings (a value written there stays as it is).
			if (!si.ContainsValue("EmuCore/GS", "HWROV"))
			{
				SetOrderedViewDefault(si);
				ps5px_log("settings: the rasterizer ordered view is on now (fast high blending accuracy); Settings > Graphics changes it");
			}
		}
		if (version < 9 && si.GetIntValue("EmuCore/GS", "OsdPerformancePos", 3) == 0)
		{
			si.SetIntValue("EmuCore/GS", "OsdPerformancePos", 3);
			ps5px_log("settings: the on-screen figures' place is the top right again (the first release had it hidden)");
		}
		if (version < 8)
		{
			SetReleaseDefaults(si, true);
			ps5px_log("settings: the release's defaults for what was not set before");
		}
		if (version < 5)
		{
			// v1.2.0 switched it on for everyone; off again, as it was.
			si.SetBoolValue("EmuCore", "EnableThreadPinning", false);
			ps5px_log("settings: thread pinning is off again (fast forward lost speed with it); Settings > Emulation has it");
		}
		si.SetIntValue("PS5", "DefaultsVersion", PS5_DEFAULTS_VERSION);
		if (!s_state.base_settings->Save(&error))
			ps5px_log("settings: cannot save: %s", error.GetDescription().c_str());
	}
	else if (si.GetIntValue("PS5", "DefaultsVersion", 0) == 0)
	{
		// A settings file brought over from a PC (it has no PS5 section): the port's defaults over
		// it, and the console's controllers in place of its keyboard.
		ps5px_log("settings: no PS5 section, applying the PS5 defaults");
		SetPS5Defaults(si);
		SetPS5InputDefaults(si);
		if (!s_state.base_settings->Save(&error))
			ps5px_log("settings: cannot save: %s", error.GetDescription().c_str());
	}
	// No hardcore mode: Crux2 has no RetroAchievements client registration for it, so it is never on
	// (a file brought over from a PC may have it). Cheats, saved states and slow motion stay usable.
	if (si.GetBoolValue("Achievements", "ChallengeMode", false))
	{
		si.SetBoolValue("Achievements", "ChallengeMode", false);
		if (!s_state.base_settings->Save(&error))
			ps5px_log("settings: cannot save: %s", error.GetDescription().c_str());
		ps5px_log("settings: hardcore mode switched off (Crux2 does not offer it)");
	}
	PS5Platform::MakePublic(s_state.base_settings->GetFileName().c_str(), false);

	// The secrets (a RetroAchievements token) in a file of their own, as on the other platforms.
	std::string secrets_path = Path::Combine(EmuFolders::Settings, "secrets.ini");
	const bool secrets_exist = FileSystem::FileExists(secrets_path.c_str());
	s_state.secrets_settings = std::make_unique<INISettingsInterface>(std::move(secrets_path));
	Host::Internal::SetSecretsSettingsLayer(s_state.secrets_settings.get());
	if (secrets_exist)
		s_state.secrets_settings->Load();

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void PS5Host::ApplyGameFolders()
{
	// The folders this port adds on its own are remembered, so that one that is gone (a USB drive
	// not plugged in, a share removed from network.cfg) leaves the list again, and those the user
	// added by hand in inis/PCSX2.ini stay.
	const std::vector<std::string> folders = PS5Platform::FindGameFolders();

	const auto lock = Host::GetSettingsLock();
	SettingsInterface& si = *s_state.base_settings;
	const std::vector<std::string> previous = si.GetStringList("PS5", "AutoGamePaths");
	bool changed = (previous != folders);

	for (const std::string& path : previous)
	{
		if (std::find(folders.begin(), folders.end(), path) == folders.end())
			changed |= si.RemoveFromStringList("GameList", "RecursivePaths", path.c_str());
	}
	for (const std::string& path : folders)
	{
		// A folder the user already lists as non-recursive stays as they set it.
		const std::vector<std::string> flat = si.GetStringList("GameList", "Paths");
		if (std::find(flat.begin(), flat.end(), path) != flat.end())
			continue;
		changed |= si.AddToStringList("GameList", "RecursivePaths", path.c_str());
		ps5px_log("game folder: %s", path.c_str());
	}

	if (changed)
	{
		si.SetStringList("PS5", "AutoGamePaths", folders);
		Host::CommitBaseSettingChanges();
	}
}

void PS5Host::LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	// Errors and warnings also go into the boot log, which is the file a user is asked for.
	if (level > LOGLEVEL_WARNING || message.empty())
		return;

	// Not more than a few of them a second, though: each line is a write to the title folder and a
	// call into the kernel's log, made on whichever thread warns, and a game that warns on every
	// frame (or on every page of a memory card) would be slowed by its own log. What is left out
	// is counted, and the emulator's own log (logs/emulog.txt) has every line.
	static constexpr u32 LINES_PER_SECOND = 20;
	static std::mutex s_mutex;
	static Common::Timer s_window;
	static u32 s_lines = 0;
	static u32 s_dropped = 0;

	u32 dropped_before = 0;
	{
		std::unique_lock lock(s_mutex);
		if (s_window.GetTimeSeconds() >= 1.0)
		{
			s_window.Reset();
			s_lines = 0;
			dropped_before = s_dropped;
			s_dropped = 0;
		}
		if (s_lines >= LINES_PER_SECOND)
		{
			s_dropped++;
			return;
		}
		s_lines++;
	}

	if (dropped_before > 0)
		ps5px_log("(%u more warnings in the second before this were not written here)", dropped_before);
	ps5px_log("%s: %.*s", (level == LOGLEVEL_ERROR) ? "error" : "warning", static_cast<int>(message.size()), message.data());
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

// The graphics driver's variable refresh (PS5_Mesa patch 0017): sceVideoOutVrrUnpegFromFixedRate is looked up
// while the title runs, by its NID, since not every firmware's libSceVideoOut exports it, and a title that
// imports what the console lacks does not start. -1 when the console has none: the driver then refuses VRR.
extern "C" int ps5_title_videoout_vrr_unpeg(int32_t handle)
{
	using UnpegFn = int (*)(int32_t);
	static const UnpegFn unpeg = [] {
		const UnpegFn found =
			reinterpret_cast<UnpegFn>(PS5SystemModules::FindFunction("libSceVideoOut", "sceVideoOutVrrUnpegFromFixedRate", "T4ucGB8CsnM"));
		ps5px_log("display: sceVideoOutVrrUnpegFromFixedRate %s", found ? "found" : "not on this console: no variable refresh");
		return found;
	}();
	return unpeg ? unpeg(handle) : -1;
}

// Crux2: settings the emulation thread uses but must not read itself (it can be waited for by a thread
// that holds the settings lock), handed over where settings are applied, with the lock released.
static void PS5HandOverEmulationSettings()
{
	MTGS::PS5SetAutoFrameQueue(Host::GetBoolSettingValue("EmuCore/GS", "PS5AutoFrameQueue", true));
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
	PS5HandOverEmulationSettings();
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	Host::RunOnCPUThread([folders, core, controllers, hotkeys, ui]() {
		{
			const auto lock = Host::GetSettingsLock();
			SettingsInterface& si = *PS5Host::s_state.base_settings;
			VMManager::SetDefaultSettings(si, folders, core, controllers, hotkeys, ui);
			if (core || ui)
				PS5Host::SetPS5Defaults(si);
		}
		Host::CommitBaseSettingChanges();
		VMManager::ApplySettings();
		if (folders)
			PS5Host::ApplyGameFolders();
	});
	return true;
}

//////////////////////////////////////////////////////////////////////////
// The emulation thread
//////////////////////////////////////////////////////////////////////////

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	using namespace PS5Host;

	// A blocking call from the emulation thread itself would wait for itself.
	if (block && s_state.cpu_thread_running.load(std::memory_order_acquire) && pthread_equal(pthread_self(), s_state.cpu_pthread))
	{
		function();
		return;
	}

	std::unique_lock lock(s_state.events_mutex);
	s_state.events.emplace_back(std::move(function), block);
	s_state.events_cv.notify_one();
	if (block)
	{
		s_state.blocking_events_pending++;
		s_state.events_done_cv.wait(lock, []() { return s_state.blocking_events_pending == 0; });
	}
}

void Host::RunOnGSThread(std::function<void()> function)
{
	RunOnCPUThread([fn = std::move(function)]() { MTGS::RunOnGSThread(std::move(fn)); });
}

void PS5Host::ProcessCPUThreadEvents()
{
	std::unique_lock lock(s_state.events_mutex);
	while (!s_state.events.empty())
	{
		auto event = std::move(s_state.events.front());
		s_state.events.pop_front();
		lock.unlock();
		event.first();
		lock.lock();

		if (event.second)
		{
			s_state.blocking_events_pending--;
			s_state.events_done_cv.notify_all();
		}
	}
}

void Host::PumpMessagesOnCPUThread()
{
	// Once per emulated frame while a game runs.
	PS5Host::ProcessCPUThreadEvents();
	PS5Host::SaveSettingsIfDue(false);
	// The memory cards held in memory: one that is due is written to its file.
	FileMcd_FrameTick();
	// "Save a state by itself": the time played is counted, and the state saved when it is due.
	PS5Extras::FrameTick();
	// This thread's processor time, for the performance overlay to read from the graphics thread.
	ps5px_publish_thread_cpu_time();
	if (PS5Host::s_test.active)
	{
		PS5Host::TestRunFrame();
		PS5Host::TestRunTick();
	}
}

void PS5Host::CPUThreadMainLoop()
{
	while (!s_state.quit_requested.load(std::memory_order_acquire))
	{
		ProcessCPUThreadEvents();
		SaveSettingsIfDue(false);

		switch (VMManager::GetState())
		{
			case VMState::Running:
				// Returns when the game is paused, reset or shut down.
				VMManager::Execute();
				continue;

			case VMState::Resetting:
				VMManager::Reset();
				continue;

			case VMState::Stopping:
				VMManager::Shutdown(s_state.save_state_on_shutdown);
				s_state.save_state_on_shutdown = false;
				continue;

			case VMState::Initializing:
			case VMState::Shutdown:
			case VMState::Paused:
			default:
				break;
		}

		// Nothing to emulate: the graphics thread draws the interface, this thread polls the
		// controllers for it.
		VMManager::IdlePollUpdate();
		if (s_test.active)
		{
			TestRunIdle();
			TestRunTick();
			if (s_state.quit_requested.load(std::memory_order_acquire) || VMManager::GetState() == VMState::Running)
				continue;
		}

		std::unique_lock lock(s_state.events_mutex);
		if (s_state.events.empty())
			s_state.events_cv.wait_for(lock, IDLE_POLL_INTERVAL);
	}
}

void PS5Host::CPUThreadEntryPoint()
{
	// The title's own priority again (the main thread, which starts this one, runs above it): the GS,
	// VU and software renderer threads started from here are at it too (ps5_priority.h).
	PS5Priority::SetCurrentThread(PS5Priority::Role::Emulation);
	s_state.cpu_pthread = pthread_self();
	s_state.cpu_thread_running.store(true, std::memory_order_release);

	if (!VMManager::Internal::CPUThreadInitialize())
		ps5_title_fatal("The emulator could not be set up (its memory or its recompilers). crux2-boot.log has the reason.");
	ps5px_log("emulation thread ready");

	ApplyGameFolders();
	// (On a thread of its own since v0.8.1: it held the first picture back by 8 s.)
	PS5Platform::MakeTitleFilesPublicInBackground();

	// Leaving a game by holding a combination of buttons (ps5_input_source.cpp): as the quick
	// menu's Leave does, with the state saved as when it is closed.
	PS5InputSource::SetLeaveGameHandler([](u32 player) {
		ps5px_log("game: left by the buttons held on controller %u", player + 1);
		if (MemcardBusy::IsBusy())
		{
			ShowToast("A memory card is being written to: leave in a moment.");
			return;
		}
		Host::RequestVMShutdown(false, true, true);
	});

	// The updater: what to ask GitHub, and how (ps5_update.cpp). It asks once now, unless [PS5]
	// UpdateAtStart is off; nothing is downloaded until the user says so.
	PS5Services::StartUpdater();

	// The interface's sound, before the interface: its files are read on a thread of their own
	// while the first frames are drawn.
	ps5px_apply_ui_sound_settings();
	PS5UIAudio::Start();
	// The console's disc drive is looked at from now on, every two seconds, on a thread of its
	// own ([PS5] DiscDrive = false leaves the drive alone).
	if (Host::GetBaseBoolSettingValue("PS5", "DiscDrive", true))
		PS5Disc::Start();
	else
		ps5px_log("disc: not looked for ([PS5] DiscDrive is off)");

	// The interface (bigpicture/), drawn by the emulator's graphics thread.
	ImGuiManager::InitializeFullscreenUI();
	if (!MTGS::WaitForOpen())
		ps5_title_fatal("The graphics device could not be created. crux2-boot.log and logs/emulog.txt have the reason.");
	ps5px_log("interface started");

	Host::RefreshGameListAsync(false);

	CPUThreadMainLoop();

	// How long each part of closing takes is written down: v0.8.0 took 15 s to close on a console
	// and its log could not say where.
	const auto exit_started = std::chrono::steady_clock::now();
	auto lap_started = exit_started;
	const auto lap = [&lap_started] {
		const auto now = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(now - lap_started).count();
		lap_started = now;
		return seconds;
	};
	ps5px_log("exit requested");
	PS5UIAudio::Stop();
	PS5Disc::Stop();
	// A run played by hand ends here, with the title closed from its interface.
	if (s_test.active && s_test.by_hand && s_test.phase == TestRun::Phase::Running)
	{
		TestRunSummary();
		ps5px_log("test: ok, the title was closed from its interface");
	}
	Host::CancelGameListRefresh();
	const double sound_and_disc = lap();
	// (Not HasValidVM(): that is false for a machine that is on its way down, in Stopping.)
	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(false);
	const double machine = lap();
	MTGS::WaitForClose();
	const double graphics = lap();
	ProcessCPUThreadEvents();
	Host::CancelGameListRefresh();
	SaveSettingsIfDue(true);
	const double settings = lap();
	PS5Platform::MakeSessionFilesPublic();
	const double files = lap();

	VMManager::Internal::CPUThreadShutdown();
	const double emulator = lap();
	ps5px_log("closing took %.1f s: sound, disc and game list %.1f, the machine %.1f, graphics %.1f, settings %.1f, files %.1f, "
	          "the emulator %.1f",
		std::chrono::duration<double>(std::chrono::steady_clock::now() - exit_started).count(), sound_and_disc, machine, graphics,
		settings, files, emulator);
	ps5px_log("shut down");
}

//////////////////////////////////////////////////////////////////////////
// Test runs
//////////////////////////////////////////////////////////////////////////

// A console run has nobody at the controller. A launch that finds test-run.txt in the title folder
// is a test run: it deletes the file (so the next launch by hand is an ordinary one), does what the
// file says, writes what it measured to the boot log and klog as "test: ..." lines, and ends
// through the same shutdown as Exit in the interface. ps5/tools/run.sh writes the file.
//
//   seconds 60        how long the run lasts once it has started (default 30)
//   game PATH         start this disc image (as the title sees it: /app0/games/..., or relative to
//                     the title folder); without it the run stays in the interface
//   screenshot        save the game's last frame as snaps/test-run.png
//   profile           sample the graphics thread and write where its time goes every ten seconds
//                     as "profile: ..." lines (ps5_profiler.cpp); "profile all" samples the
//                     emulation and VU threads as well
//   threaded-recording  have the graphics driver record commands on a thread of its own
//                     (RADV_THREADED_RECORDING=1) for this launch, whatever the setting says
//
// A file with "profile" or "threaded-recording" and neither "seconds" nor "game" is a run played
// by hand: somebody starts a game from the interface and plays, the windows are measured for as
// long as a game runs, and the run ends when the title is closed from its interface.
void PS5Host::ReadTestRun()
{
	const std::string path = Path::Combine(ps5px_title_dir(), "test-run.txt");
	const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	if (!text.has_value())
		return;
	if (!FileSystem::DeleteFilePath(path.c_str()))
		ps5px_log("test: test-run.txt could not be deleted: the next launch will be a test run as well");

	s_test.active = true;
	bool seconds_given = false;
	for (const std::string_view full_line : StringUtil::SplitString(text.value(), '\n', true))
	{
		const std::string_view line = StringUtil::StripWhitespace(full_line);
		const std::string_view::size_type space = line.find(' ');
		const std::string_view key = line.substr(0, space);
		const std::string_view value =
			(space == std::string_view::npos) ? std::string_view() : StringUtil::StripWhitespace(line.substr(space + 1));
		if (key == "seconds")
		{
			s_test.seconds = std::clamp(StringUtil::FromChars<double>(value).value_or(30.0), 1.0, 86400.0);
			seconds_given = true;
		}
		else if (key == "profile")
			s_test.profile = (value == "all") ? 2 : 1;
		else if (key == "threaded-recording")
			s_test.threaded_recording = true;
		else if (key == "game")
			s_test.game = Path::IsAbsolute(value) ? std::string(value) : Path::Combine(ps5px_title_dir(), value);
		else if (key == "screenshot")
			s_test.screenshot = true;
		else if (!key.empty() && key[0] != '#')
			ps5px_log("test: unknown line in test-run.txt: %.*s", static_cast<int>(line.size()), line.data());
	}

	s_test.by_hand = (s_test.profile != 0 || s_test.threaded_recording) && !seconds_given && s_test.game.empty();
	if (s_test.by_hand)
		ps5px_log("test: a run played by hand: measured while a game runs, until the title is closed");
	else
		ps5px_log("test: run of %.0f s%s%s%s", s_test.seconds, s_test.game.empty() ? ", in the interface" : ", game ",
			s_test.game.c_str(), s_test.screenshot ? ", a screenshot of the last frame" : "");

	// (What "threaded-recording" asks for is done by main, with the setting of the same effect.)
	if (s_test.profile != 0)
		ps5px_profile_start(s_test.profile == 2);
}

// Once per emulated frame: the gap since the frame before, which is what a stutter is.
void PS5Host::TestRunFrame()
{
	if (s_test.phase != TestRun::Phase::Running)
		return;

	if (!s_test.game_running)
	{
		// A game started, or went on after a pause: its windows begin with this frame, and the
		// time in the menu before it is no frame of the game's.
		s_test.game_running = true;
		ps5px_audio_counters(&s_test.audio_played, &s_test.audio_short);
		s_test.frames = 0;
		s_test.worst_ms = 0.0;
		s_test.over_40ms = 0;
		s_test.window.Reset();
		s_test.frame.Reset();
		return;
	}

	const double gap_ms = s_test.frame.GetTimeMillisecondsAndReset();
	s_test.frames++;
	s_test.worst_ms = std::max(s_test.worst_ms, gap_ms);
	if (gap_ms > 40.0)
		s_test.over_40ms++;
}

// From the emulation thread's idle loop: no game is being emulated (none is loaded, or it is
// paused). A window the game had begun is dropped: it would count the pause as slow frames.
void PS5Host::TestRunIdle()
{
	if (!s_test.game_running)
		return;

	s_test.game_running = false;
	s_test.frames = 0;
	s_test.worst_ms = 0.0;
	s_test.over_40ms = 0;
	s_test.window.Reset();
}

// What the windows after the first add up to (the first is the boot).
void PS5Host::TestRunSummary()
{
	if (s_test.steady_seconds > 0.0)
	{
		ps5px_log("test: after the first window: %.1f fps over %.0f s, worst frame %.1f ms, %u over 40 ms",
			static_cast<double>(s_test.steady_frames) / s_test.steady_seconds, s_test.steady_seconds, s_test.steady_worst_ms,
			s_test.steady_over_40ms);
	}
}

// From the emulation thread's loop, with a game running or not.
void PS5Host::TestRunTick()
{
	switch (s_test.phase)
	{
		case TestRun::Phase::Start:
		{
			if (!s_test.game.empty())
			{
				VMBootParameters params;
				params.filename = s_test.game;
				Error error;
				if (VMManager::Initialize(params, &error) == VMBootResult::StartupSuccess)
				{
					VMManager::SetState(VMState::Running);
				}
				else
				{
					ps5px_log("test: FAILED: the game could not be started: %s", error.GetDescription().c_str());
					s_test.failed = true;
					s_test.phase = TestRun::Phase::Stopping;
					return;
				}
			}

			ps5px_audio_counters(&s_test.audio_played, &s_test.audio_short);
			s_test.total.Reset();
			s_test.window.Reset();
			s_test.frame.Reset();
			s_test.phase = TestRun::Phase::Running;
			return;
		}

		case TestRun::Phase::Running:
		{
			const double window_seconds = s_test.window.GetTimeSeconds();
			if (window_seconds >= TEST_WINDOW_SECONDS)
			{
				unsigned long long played = 0, shortfall = 0;
				ps5px_audio_counters(&played, &shortfall);
				s_test.window_index++;
				if (s_test.game_running)
				{
					s_test.interface_noted = false;
					// "frames queued": what the EE may run ahead of the GS thread at this moment (the setting, or
					// PS5AutoFrameQueue's two frames while the game is behind).
					ps5px_log("test: window %u: %.1f fps, speed %.0f%%, game's own %.1f fps, worst frame %.1f ms, "
							  "%u over 40 ms, audio %llu writes of which %llu short, frames queued %d",
						s_test.window_index, static_cast<double>(s_test.frames) / window_seconds,
						static_cast<double>(PerformanceMetrics::GetSpeed()),
						static_cast<double>(PerformanceMetrics::GetInternalFPS()), s_test.worst_ms, s_test.over_40ms,
						played - s_test.audio_played, shortfall - s_test.audio_short,
						EmuConfig.GS.VsyncQueueSize > 0 ? EmuConfig.GS.VsyncQueueSize : MTGS::PS5FrameQueueDepth());
					if (const std::string stretch = AudioStream::PS5TakeStretchWindow(); !stretch.empty())
						ps5px_log("test: window %u: %s", s_test.window_index, stretch.c_str());

					// How busy the threads and the GPU are (the overlay's figures), and what the
					// graphics thread draws in a frame: the renderer's own counters, read on its
					// thread. PRIM primitives, DRW the game's draws, DRWC draw calls made of them,
					// BAR barriers (a draw that reads the picture it draws on waits for the GPU
					// first), RP render passes, RB readbacks, TC texture copies, TU uploads.
					const u32 window = s_test.window_index;
					const double ee = PerformanceMetrics::GetCPUThreadUsage(), gs = PerformanceMetrics::GetGSThreadUsage();
					const double vu = PerformanceMetrics::GetVUThreadUsage(), gpu = PerformanceMetrics::GetGPUUsage();
					const u32 window_frames = static_cast<u32>(s_test.frames);
					MTGS::RunOnGSThread([window, ee, gs, vu, gpu, window_frames]() {
						if (!g_gs_device || !MTGS::IsOpen())
							return;
						SmallString stats;
						GSgetStats(stats);
						ps5px_log("test: window %u: EE %.0f%%, GS %.0f%%, VU %.0f%%, GPU %.0f%%; a frame: %s", window, ee, gs, vu,
							gpu, stats.c_str());
						// Where the GPU's time went, by the kind of work in each render pass.
						if (const std::string gpu_line = g_gs_device->PS5TakeGpuWindow(); !gpu_line.empty())
							ps5px_log("test: window %u: %s", window, gpu_line.c_str());
						if (const std::string pgs_line = PS5PGS::StatusLine(); !pgs_line.empty())
							ps5px_log("test: window %u: %s", window, pgs_line.c_str());
						// The ordered view's draws: which needed it, which only continued it.
						if (!PS5PGS::Active() && GSConfig.HWROV)
							ps5px_log("test: window %u: %s", window, GSRendererHW::PS5TakeROVWindow(window_frames).c_str());
					});

					// The first window is the boot (the BIOS, shaders compiling for the first time).
					if (s_test.window_index > 1)
					{
						s_test.steady_frames += s_test.frames;
						s_test.steady_seconds += window_seconds;
						s_test.steady_worst_ms = std::max(s_test.steady_worst_ms, s_test.worst_ms);
						s_test.steady_over_40ms += s_test.over_40ms;
					}
				}
				else if (!s_test.by_hand || !s_test.interface_noted)
				{
					// (Once, in a run played by hand: nobody needs a line every five seconds of a menu.)
					s_test.interface_noted = true;
					ps5px_log("test: window %u: the interface, no game running%s", s_test.window_index,
						s_test.by_hand ? " (or a paused one; not said again until a game has run)" : "");
				}

				s_test.audio_played = played;
				s_test.audio_short = shortfall;
				s_test.frames = 0;
				s_test.worst_ms = 0.0;
				s_test.over_40ms = 0;
				s_test.window.Reset();
			}

			if (s_test.by_hand || s_test.total.GetTimeSeconds() < s_test.seconds)
				return;

			if (s_test.screenshot && VMManager::HasValidVM())
			{
				const std::string path = Path::Combine(EmuFolders::Snapshots, "test-run.png");
				FileSystem::DeleteFilePath(path.c_str());
				MTGS::RunOnGSThread([path]() { GSQueueSnapshot(path); });
				ps5px_log("test: screenshot: %s", path.c_str());
				s_test.screenshot_wait.Reset();
				s_test.phase = TestRun::Phase::Screenshot;
			}
			else
			{
				s_test.phase = TestRun::Phase::Stopping;
			}
			return;
		}

		case TestRun::Phase::Screenshot:
		{
			// The snapshot is taken with the next frame presented and written by a worker.
			if (s_test.screenshot_wait.GetTimeSeconds() >= TEST_SCREENSHOT_SECONDS)
				s_test.phase = TestRun::Phase::Stopping;
			return;
		}

		case TestRun::Phase::Stopping:
		{
			if (VMManager::HasValidVM())
			{
				// As "Close Game" does it; the loop shuts the machine down and comes back here.
				if (!s_test.stop_requested)
				{
					s_test.stop_requested = true;
					s_state.save_state_on_shutdown = false;
					VMManager::SetState(VMState::Stopping);
				}
				return;
			}

			TestRunSummary();
			ps5px_log("test: %s, shutting down", s_test.failed ? "FAILED" : "ok");
			s_test.phase = TestRun::Phase::Done;
			Host::RequestExitApplication(false);
			return;
		}

		case TestRun::Phase::Done:
		default:
			return;
	}
}

//////////////////////////////////////////////////////////////////////////
// The emulator's events
//////////////////////////////////////////////////////////////////////////

namespace
{
	// How long the game that runs has run (the session's summary).
	Common::Timer s_session_timer;
} // namespace

void Host::OnVMStarting()
{
	PS5HandOverEmulationSettings();
	// The console's main output takes one port of a title: the interface's sound gives its own
	// up before the game's stream opens (this returns when the port is closed).
	PS5UIAudio::Stop();
	// The time played, for the state saved by itself, counts from here.
	PS5Extras::GameStarting();
	// The session's figures (and automatic quality) start afresh.
	PS5AutoQuality::ResetSession();
	s_session_timer.Reset();
}

namespace
{
	/// The memory the title has left, for the boot log at a game's start and end: games played one
	/// after another should find the same each time (a game that keeps something takes it from the next).
	void LogMemory(const char* when)
	{
		size_t flexible = 0;
		sceKernelAvailableFlexibleMemorySize(&flexible);
		ps5px_log("memory %s: %llu MiB of direct memory in its largest free run, %zu MiB of flexible memory free; the graphics "
				  "device's texture pool %llu MiB",
			when, static_cast<unsigned long long>(GetAvailablePhysicalMemory() >> 20), flexible >> 20,
			static_cast<unsigned long long>(g_gs_device ? (g_gs_device->GetPoolMemoryUsage() >> 20) : 0));
	}
} // namespace

void Host::OnVMStarted()
{
	PS5BootStep::Set(PS5BootStep::Step::None);
	MTGS::RunOnGSThread([]() { LogMemory("at the game's start"); });
	// The pipelines this game needed last time are made ahead (GSDeviceVK_PS5.cpp).
	MTGS::RunOnGSThread([serial = VMManager::GetDiscSerial()]() {
		if (g_gs_device)
			g_gs_device->PipelinesForGame(serial);
		// paraLLEl-GS, when this game's settings ask for it: made now, before a state is loaded and
		// before the game sends the GS anything. (The GS itself was opened for the interface, before
		// any game's settings were read.)
		PS5PGS::GameStarted();
	});
}

void Host::OnVMDestroyed()
{
	PS5BootStep::Set(PS5BootStep::Step::None);
	// A profiling build: the counts of the game just played, kept even if the title is killed later.
	PS5Pgo::Write("a game closed");
	// The game's sound is closed (or its start failed before there was any): the interface's again.
	PS5UIAudio::Start();
	// What it needed of the graphics driver is kept for its next start.
	if (MTGS::IsOpen())
	{
		MTGS::RunOnGSThread([]() {
			if (g_gs_device)
				g_gs_device->PipelinesForGame(std::string());
			// paraLLEl-GS ends with its game; the next game decides for itself.
			PS5PGS::Close();
			// The game's render targets and textures went back to the device's pool when the GS was
			// reset (VMManager::Shutdown), and the pool is aged only by a game's frames: the interface
			// draws none. At 6x they are hundreds of MB, which the next game would start with on top of
			// its own. They go now. (Games played one after another slowed down and hitched; the memory
			// lines say whether this was it. needs proper testing)
			if (g_gs_device)
			{
				const u64 pooled = g_gs_device->GetPoolMemoryUsage();
				g_gs_device->PurgePool();
				ps5px_log("graphics: %llu MiB of the game's textures let go", static_cast<unsigned long long>(pooled >> 20));
			}
			LogMemory("after the game");
		});
	}
	// And the disc drive is the watcher's again, if the game had it.
	PS5Disc::SetInUse(false);
	// Its native module goes (with what it ran, for the log).
	PS5Native::VMDestroyed();
	// The session in a few lines, before the rest of what closing says.
	{
		const PS5AutoQuality::Figures f = PS5AutoQuality::SessionFigures();
		const double minutes = s_session_timer.GetTimeSeconds() / 60.0;
		ps5px_log("session: %.1f minutes; at normal speed %.0f%% on average, under 95%% for %.0f s in all (the longest stretch %.0f s)",
			minutes, f.average_speed, f.seconds_slow, f.longest_slow);
		if (f.steps_down || f.steps_up)
			ps5px_log("session: automatic quality stepped down %u time(s) and up %u, to %gx at the lowest", f.steps_down, f.steps_up, f.lowest);
	}
	// (For the notice after a crash: what the log says from here on is not while a game was loaded.)
	ps5px_log("game: closed");
	// The memory cards are whole on the disk again: backed up, when that is asked for and they changed.
	PS5Jobs::GameClosed();
	// Closed in the title: sleep-safe saving's note goes (nothing to go on with at the next start).
	PS5Extras::GameClosed();
	// The scan a game's start cancelled, and the list's "last played" and play time.
	Host::RefreshGameListAsync(false);
}

void Host::OnVMPaused()
{
	// A pause is when a title is most likely to be closed from the console's menu: what a game
	// has just saved to a memory card goes to its file now, not up to two seconds later.
	FileMcd_FlushPending();
}

void Host::OnVMResumed()
{
}

// PCSX2 v2.9.145: per-game settings were saved or removed. Crux2's interface reads a game's own
// settings when it shows them, so it has nothing to update here.
void Host::OnGameSettingsChanged(const std::string& path)
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	PS5HandOverEmulationSettings(); // a game's own settings are in effect now
	if (!disc_path.empty() || !title.empty())
	{
		ps5px_log("game: %s [%s, CRC %08X] %s", title.c_str(), disc_serial.c_str(), disc_crc, disc_path.c_str());
		// What its picture costs: the settings that decide most of the GPU's work, as this game has them
		// (its own options and the game database's fixes applied).
		static constexpr const char* blending[] = {"minimum", "basic", "medium", "high", "full", "maximum"};
		const Pcsx2Config::GSOptions& gs = EmuConfig.GS;
		const int level = std::clamp(static_cast<int>(gs.AccurateBlendingUnit), 0, 5);
		ps5px_log("graphics: %gx resolution, blending %s, auto flush %d, rasterizer ordered view %s, texture barriers per draw %s",
			gs.UpscaleMultiplier, blending[level], static_cast<int>(gs.UserHacks_AutoFlush), gs.HWROV ? "on" : "off",
			gs.HWROVBarriersVK ? "on" : "off");
	}
	// The game's native module, if one was made for its executable (on the emulation thread, as here).
	PS5Native::GameChanged();
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
	// Readable and replaceable by an FTP client, as the other files the title keeps.
	const std::string path(filename);
	PS5Platform::MakePublic(path.c_str(), false);
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	if (!VMManager::HasValidVM())
		return;

	// The confirmation, when there is one, is the interface's own (the quick menu's "Quit game");
	// what arrives here is the decision.
	Host::RunOnCPUThread([save_state = (allow_save_state && default_save_state)]() {
		const VMState state = VMManager::GetState();
		if (state != VMState::Running && state != VMState::Paused)
			return;

		PS5Host::s_state.save_state_on_shutdown = save_state;
		VMManager::SetState(VMState::Stopping);
	});
}

void Host::RequestExitApplication(bool allow_confirm)
{
	using namespace PS5Host;
	s_state.quit_requested.store(true, std::memory_order_release);

	// A running game holds the emulation thread inside VMManager::Execute(): have it stop, so the
	// thread comes back to its loop and sees the request.
	Host::RunOnCPUThread([]() {
		const VMState state = VMManager::GetState();
		if (state == VMState::Running || state == VMState::Paused)
			VMManager::SetState(VMState::Stopping);
	});
}

void Host::RequestExitBigPicture()
{
	// There is nothing behind the interface on the console: leaving it is leaving the title.
	RequestExitApplication(false);
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	// There is no desktop interface behind this one.
	return true;
}

//////////////////////////////////////////////////////////////////////////
// The display
//////////////////////////////////////////////////////////////////////////

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// No window system: a plane of the console's display, through VK_KHR_display.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Display;
	wi.surface_width = PS5Host::DISPLAY_WIDTH;
	wi.surface_height = PS5Host::DISPLAY_HEIGHT;
	wi.surface_scale = 1.0f;
	wi.surface_refresh_rate = PS5Host::DISPLAY_REFRESH_RATE;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return std::nullopt;
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

//////////////////////////////////////////////////////////////////////////
// Messages
//////////////////////////////////////////////////////////////////////////

// On the screen: over a game as an OSD line, over the interface as its message dialog (the OSD is
// not drawn while one of the interface's windows is up). Callable from any thread.
void PS5Host::ShowMessage(std::string title, std::string message, const char* icon, float osd_duration)
{
	Host::RunOnCPUThread([title = std::move(title), message = std::move(message), icon, osd_duration]() mutable {
		// Before the graphics thread has a device there is no screen to show it on: the boot log has it.
		if (!MTGS::IsOpen())
			return;

		MTGS::RunOnGSThread([title = std::move(title), message = std::move(message), icon, osd_duration]() mutable {
			if (FullscreenUI::IsInitialized() && FullscreenUI::HasActiveWindow())
			{
				bp::message(title.empty() ? std::string("Crux2") : std::move(title), std::move(message));
			}
			else
			{
				std::string text = (!title.empty() && !message.empty()) ? fmt::format("{}: {}", title, message) :
																		  std::move(title.empty() ? message : title);
				Host::AddIconOSDMessage(std::string(), icon, text, osd_duration);
			}
		});
	});
}

// A line that shows for a few seconds over the interface and goes by itself. From any thread.
void PS5Host::ShowToast(std::string message)
{
	Host::RunOnCPUThread([message = std::move(message)]() mutable {
		if (!MTGS::IsOpen())
			return;
		MTGS::RunOnGSThread([message = std::move(message)]() mutable {
			if (FullscreenUI::IsInitialized())
				bp::toast(message, 5.0f);
		});
	});
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	ps5px_log("info: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	if (!message.empty())
		PS5Host::ShowMessage(std::string(title), std::string(message), ICON_FA_CIRCLE_INFO, Host::OSD_INFO_DURATION);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	ps5px_log("error: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	if (!title.empty() || !message.empty())
	{
		PS5Host::ShowMessage(std::string(message.empty() ? std::string_view() : title),
			std::string(message.empty() ? title : message), ICON_FA_TRIANGLE_EXCLAMATION, Host::OSD_ERROR_DURATION);
	}
}

void Host::OpenURL(const std::string_view url)
{
	ps5px_log("not opened (no browser for a title): %.*s", static_cast<int>(url.size()), url.data());
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return {};
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	// Never asked for: the interface's own file selector is the one (ShouldPreferHostFileSelector).
	callback(std::string());
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	// Without regard to case, as the desktop interface's collation orders titles (ps5_letter_jump.h).
	return PS5CompareTitles(lhs, rhs);
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char* tbuf,
	size_t tbuf_space)
{
	// English only: the translations are the desktop interface's (Qt) files.
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	const TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		const std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, 2, count_str.view());
	}

	return ret;
}

namespace PS5Host
{
	namespace
	{
		/// The progress of something the emulator makes the user wait for: a disc image being read
		/// into memory before a game starts (Enable CDVD Precaching), which from a network share
		/// takes as long as copying the image does. Shown by the interface's loading screen, as a
		/// line of text and a bar. The work runs on the emulation thread while the graphics thread
		/// draws; what is shown crosses in ps5_ui.h.
		class LoadProgressCallback final : public BaseProgressCallback
		{
		public:
			~LoadProgressCallback() override
			{
				if (m_open)
					PS5UI::SetLoadProgress(std::string(), -1.0f);
			}

			void SetStatusText(const char* text) override
			{
				BaseProgressCallback::SetStatusText(text);
				ps5px_log("progress: %s", text);
				Redraw(true);
			}

			void SetProgressRange(u32 range) override
			{
				BaseProgressCallback::SetProgressRange(range);
				Redraw(false);
			}

			void SetProgressValue(u32 value) override
			{
				BaseProgressCallback::SetProgressValue(value);
				Redraw(false);
			}

			void SetTitle(const char* title) override {}
			void DisplayError(const char* message) override { ps5px_log("progress: error: %s", message); }
			void DisplayWarning(const char* message) override { ps5px_log("progress: warning: %s", message); }
			void DisplayInformation(const char* message) override {}
			void DisplayDebugMessage(const char* message) override {}
			void ModalError(const char* message) override { ps5px_log("progress: error: %s", message); }
			bool ModalConfirmation(const char* message) override { return false; }
			void ModalInformation(const char* message) override {}

		private:
			void Redraw(bool force)
			{
				// The reader reports every chunk: ten updates a second are plenty.
				if (!force && m_open && m_last_update.GetTimeSeconds() < 0.1)
					return;
				m_last_update.Reset();

				const u32 range = std::max<u32>(m_progress_range, 1);
				const u32 value = std::min<u32>(m_progress_value, m_progress_range);
				PS5UI::SetLoadProgress(m_status_text.empty() ? std::string("Loading") : std::string(m_status_text),
					(range > 1) ? static_cast<float>(value) / static_cast<float>(range) : -1.0f);
				m_open = true;
			}

			Common::Timer m_last_update;
			bool m_open = false;
		};
	} // namespace
} // namespace PS5Host

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return std::make_unique<PS5Host::LoadProgressCallback>();
}

//////////////////////////////////////////////////////////////////////////
// The game list
//////////////////////////////////////////////////////////////////////////

bool PS5Host::GameListProgressCallback::IsCancelled() const
{
	return s_state.game_list_cancel.load(std::memory_order_acquire);
}

void PS5Host::DownloadMissingCovers(ProgressCallback* progress)
{
	// Covers by serial, for the games that have none yet: after each scan when [PS5] DownloadCovers
	// is set, and once when Settings > Interface > "Download the missing covers now" asks for it. From
	// the address [PS5] CoverURL names (${serial}, ${title} and ${filetitle} stand for the game's).
	// No address is built in: cover pictures are other people's work, and where they are taken
	// from is the choice of whoever runs this, as it is in PCSX2 on a PC.
	const bool asked_for = s_state.force_cover_download.exchange(false, std::memory_order_acq_rel);
	std::string url = Host::GetBaseStringSettingValue("PS5", "CoverURL", "");
	if (url.empty() || !Host::GetBaseBoolSettingValue("PS5", "DownloadCovers", false))
	{
		// The usual way: libretro's box arts, looked up by the game's name (ps5_online.cpp), after
		// each scan unless Settings > Interface > "Download covers automatically" is off.
		if (!asked_for && !Host::GetBaseBoolSettingValue("PS5", "AutoCovers", true))
			return;
		progress->SetStatusText("Downloading covers...");
		PS5UI::SetBackgroundNote("Downloading covers");
		std::string summary;
		const int saved = PS5Covers::DownloadMissing(progress, asked_for, &summary);
		PS5UI::SetBackgroundNote(std::string());
		if (asked_for && progress->IsCancelled())
		{
			// The scan that replaces this one carries on with what was asked for.
			s_state.force_cover_download.store(true, std::memory_order_release);
			return;
		}
		if (asked_for || saved > 0)
			ShowToast(std::move(summary));
		if (saved > 0)
		{
			Host::RunOnCPUThread([]() {
				if (MTGS::IsOpen() && FullscreenUI::IsInitialized())
					FullscreenUI::InvalidateCoverCache();
			});
		}
		return;
	}

	u32 missing = 0;
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* const entry = GameList::GetEntryByIndex(i);
			if (entry && !entry->serial.empty() && GameList::GetCoverImagePathForEntry(entry).empty())
				missing++;
		}
	}
	if (missing == 0)
	{
		if (asked_for)
			ShowToast("Every game in the list has a cover.");
		return;
	}

	ps5px_log("covers: %u game(s) without a cover, asking %s", missing, url.c_str());
	if (asked_for)
		ShowToast(fmt::format("Downloading covers for {} game(s)...", missing));
	progress->SetStatusText("Downloading covers...");
	PS5UI::SetBackgroundNote(fmt::format("Downloading covers for {} game(s)", missing));
	const bool result = GameList::DownloadCovers({std::move(url)}, true, progress, [](const GameList::Entry* entry, std::string path) {
		PS5Platform::MakePublic(path.c_str(), false);
	});
	PS5UI::SetBackgroundNote(std::string());
	// (A cancelled download reports success: another scan was asked for, or a game is starting.)
	const bool cancelled = progress->IsCancelled();
	ps5px_log("covers: %s", cancelled ? "stopped, another scan takes over" : (result ? "done" : "the download did not complete"));
	if (asked_for && cancelled)
	{
		// The scan that replaces this one carries on with what was asked for.
		s_state.force_cover_download.store(true, std::memory_order_release);
		return;
	}
	if (asked_for)
		ShowToast(result ? "Cover download finished." : "The cover download did not complete (crux2-boot.log has the reason).");

	// The interface remembers which games had no cover: have it look again. (Through the emulation
	// thread: that is the thread that may queue work for the graphics thread.)
	Host::RunOnCPUThread([]() {
		if (MTGS::IsOpen() && FullscreenUI::IsInitialized())
			FullscreenUI::InvalidateCoverCache();
	});
}

// Settings > Interface > PlayStation 5 > Profile the next start: writes the test-run.txt that
// makes the next launch a profiled run played by hand ("Test runs" above), so nobody needs an
// FTP client to ask for one.
extern "C" int ps5px_arm_profiled_start(int threaded_recording)
{
	const std::string path = Path::Combine(ps5px_title_dir(), "test-run.txt");
	// Every thread the emulator works on (graphics, emulation, VU, paraLLEl-GS, the driver's): a slow
	// game's limit can be any of them.
	const std::string_view text = threaded_recording ? "profile all\nthreaded-recording\n" : "profile all\n";
	const bool written = FileSystem::WriteStringToFile(path.c_str(), text);
	ps5px_log("test: %s for the next start: %s", threaded_recording ? "a profiled run with threaded recording" : "a profiled run",
		written ? "test-run.txt written" : "test-run.txt could not be written");
	return written ? 1 : 0;
}

extern "C" void ps5px_apply_ui_sound_settings(void)
{
	PS5UIAudio::SetEnabled(Host::GetBaseBoolSettingValue("PS5", "StartupSound", true), Host::GetBaseBoolSettingValue("PS5", "BackgroundSound", true));
	PS5UIAudio::SetVolume(static_cast<float>(Host::GetBaseIntSettingValue("PS5", "StartupVolume", 80)) / 100.0f,
		static_cast<float>(Host::GetBaseIntSettingValue("PS5", "BackgroundVolume", 40)) / 100.0f);
	// The interface's sounds (the kit's sets) and the music's order.
	PS5UIAudio::SetSoundSet(static_cast<PS5UIAudio::SoundSet>(std::clamp(Host::GetBaseIntSettingValue("PS5UI", "SoundSet", 1), 0, 2)));
	PS5UIAudio::SetSoundVolume(static_cast<float>(std::clamp(Host::GetBaseIntSettingValue("PS5UI", "SoundVolume", 80), 0, 100)) / 100.0f);
	PS5UIAudio::SetMusic(Host::GetBaseIntSettingValue("PS5UI", "MusicOrder", 1) == 0 ? PS5UIAudio::MusicOrder::InOrder : PS5UIAudio::MusicOrder::Shuffle,
		Host::GetBaseBoolSettingValue("PS5UI", "KitMusic", false));
}

// Cheats from the collections on GitHub (ps5_online.cpp), on a thread of their own: for one game
// (its Cheats page asks) or for all of the list (Settings > Interface).
namespace
{
	struct CheatJob
	{
		bool all = false;
		std::string serial;
		u32 crc = 0;
	};
	std::atomic<bool> s_cheat_job_running{false};

	void* CheatJobMain(void* argument)
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
		std::unique_ptr<CheatJob> job(static_cast<CheatJob*>(argument));
		std::string summary;
		PS5UI::SetBackgroundNote(job->all ? "Downloading cheats" : "Looking for this game's cheats");
		const int saved = job->all ? PS5Cheats::FetchForAllGames(nullptr, &summary) : PS5Cheats::FetchForGame(job->serial, job->crc, &summary);
		PS5UI::SetBackgroundNote(std::string());
		PS5Host::ShowToast(std::move(summary));
		if (saved > 0)
			PS5UI::CheatsChanged();
		s_cheat_job_running.store(false, std::memory_order_release);
		return nullptr;
	}

	void StartCheatJob(CheatJob* job)
	{
		if (s_cheat_job_running.exchange(true, std::memory_order_acq_rel))
		{
			delete job;
			PS5Host::ShowToast("Cheats are being downloaded already.");
			return;
		}
		// (No stack size is asked for: the platform layer's default is 2 MiB, enough for TLS.)
		pthread_t thread;
		const int rc = pthread_create(&thread, nullptr, &CheatJobMain, job);
		if (rc != 0)
		{
			ps5px_log("cheats: the download's thread did not start: %d", rc);
			delete job;
			s_cheat_job_running.store(false, std::memory_order_release);
			return;
		}
		pthread_detach(thread);
	}
} // namespace

extern "C" void ps5px_download_cheats(const char* serial, unsigned crc)
{
	CheatJob* const job = new CheatJob();
	job->serial = serial ? serial : "";
	job->crc = crc;
	StartCheatJob(job);
}

extern "C" void ps5px_download_all_cheats(void)
{
	CheatJob* const job = new CheatJob();
	job->all = true;
	StartCheatJob(job);
}

void PS5Host::DownloadMissingPictures(ProgressCallback* progress)
{
	// A game's title screen and a picture of it being played, for the PlayStation 2 games that
	// have neither yet: after each scan unless Settings > Interface > "Fetch game pictures
	// automatically" is off, and when Settings > Library > "Fetch the games' pictures" asks (then
	// also for the games nothing was found for before). From libretro's collection, as the covers.
	const bool asked_for = s_state.force_picture_download.exchange(false, std::memory_order_acq_rel);
	if (progress->IsCancelled())
	{
		if (asked_for)
			s_state.force_picture_download.store(true, std::memory_order_release);
		return;
	}
	if (!asked_for && !Host::GetBaseBoolSettingValue("PS5", "AutoPictures", true))
		return;
	progress->SetStatusText("Fetching pictures...");
	PS5UI::SetBackgroundNote("Fetching the games' pictures");
	std::string summary;
	const int saved = PS5Covers::DownloadPictures(progress, std::string(), asked_for, &summary);
	PS5UI::SetBackgroundNote(std::string());
	if (saved > 0)
		PS5UI::PicturesChanged();
	if (asked_for && progress->IsCancelled())
	{
		// The scan that replaces this one carries on with what was asked for.
		s_state.force_picture_download.store(true, std::memory_order_release);
		return;
	}
	if (asked_for || saved > 0)
		ShowToast(std::move(summary));
}

extern "C" void ps5px_download_pictures_now(void)
{
	PS5Host::s_state.force_picture_download.store(true, std::memory_order_release);
	Host::RefreshGameListAsync(false);
}

extern "C" void ps5px_download_covers_now(void)
{
	PS5Host::s_state.force_cover_download.store(true, std::memory_order_release);
	Host::RefreshGameListAsync(false);
}

void PS5Host::GameListRefreshThreadEntryPoint(bool invalidate_cache, u32 generation)
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	ps5px_install_crash_stack();

	Common::Timer timer;
	{
		GameListProgressCallback progress;
		GameList::Refresh(invalidate_cache, false, &progress);
		if (!progress.IsCancelled())
		{
			u32 count = 0;
			{
				const auto lock = GameList::GetLock();
				count = GameList::GetEntryCount();
			}
			ps5px_log("game list: %u game(s), %.1f s", count, timer.GetTimeSeconds());
			// The list is whole: what follows is downloads, and the interface says so by their own
			// note, not as a scan that goes on.
			PS5UI::SetScanning(false);
			// The memory cards in sync with the backup place, once when the title starts (the
			// shares answer by now: the scan has read them). Again after each game: PS5Jobs::GameClosed.
			static bool s_cards_synced_once = false;
			if (!s_cards_synced_once && Host::GetBaseBoolSettingValue("PS5", "CardSync", false))
			{
				s_cards_synced_once = true;
				PS5Jobs::SyncCards(true);
			}
			DownloadMissingCovers(&progress);
			DownloadMissingPictures(&progress);
		}
	}

	// Back on the emulation thread: this thread is joined, and a scan asked for meanwhile starts.
	Host::RunOnCPUThread([generation]() { OnGameListRefreshDone(generation); });
}

bool PS5Host::IsOnCPUThread()
{
	return s_state.cpu_thread_running.load(std::memory_order_acquire) && pthread_equal(pthread_self(), s_state.cpu_pthread);
}

// On the emulation thread.
void PS5Host::StartGameListRefresh(bool invalidate_cache)
{
	if (s_state.quit_requested.load(std::memory_order_acquire))
		return;

	if (s_state.game_list_active)
	{
		// One is running: stop it at its next file, and start again when its thread reports back.
		// (Not joined here: this thread would stand still, a running game with it, for as long as a
		// share takes to answer.)
		s_state.game_list_cancel.store(true, std::memory_order_release);
		s_state.game_list_pending = std::max(s_state.game_list_pending, invalidate_cache ? 1 : 0);
		return;
	}

	if (s_state.game_list_thread.joinable())
		s_state.game_list_thread.join();

	s_state.game_list_cancel.store(false, std::memory_order_release);
	s_state.game_list_active = true;
	PS5UI::SetScanning(true);
	s_state.game_list_generation++;
	s_state.game_list_thread = std::thread(&GameListRefreshThreadEntryPoint, invalidate_cache, s_state.game_list_generation);
}

// On the emulation thread, queued by a scan's thread as its last act.
void PS5Host::OnGameListRefreshDone(u32 generation)
{
	// A scan that was cancelled and joined since (a game was started) has nothing left to do here.
	if (!s_state.game_list_active || generation != s_state.game_list_generation)
		return;

	if (s_state.game_list_thread.joinable())
		s_state.game_list_thread.join();
	s_state.game_list_active = false;
	PS5UI::SetScanning(false);

	const int pending = s_state.game_list_pending;
	s_state.game_list_pending = -1;
	if (pending >= 0)
		StartGameListRefresh(pending != 0);
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	// The interface asks for this on the graphics thread, holding the game list's lock or the
	// settings lock, both of which a running scan takes: joining that scan from here would wait
	// for a thread that waits for the caller. So the request goes to the emulation thread.
	Host::RunOnCPUThread([invalidate_cache]() { PS5Host::StartGameListRefresh(invalidate_cache); });
}

void Host::CancelGameListRefresh()
{
	using namespace PS5Host;

	// Asked for by the emulation thread before a game starts (the scan and the game would both
	// read the disc image), and by this file at shutdown. From any other thread the scan is only
	// told to stop: that thread may hold a lock the scan is waiting for.
	s_state.game_list_cancel.store(true, std::memory_order_release);
	if (!IsOnCPUThread())
		return;

	s_state.game_list_pending = -1;
	if (s_state.game_list_thread.joinable())
		s_state.game_list_thread.join();
	if (s_state.game_list_active)
		PS5UI::SetScanning(false);
	s_state.game_list_active = false;
}

//////////////////////////////////////////////////////////////////////////
// Controllers, achievements
//////////////////////////////////////////////////////////////////////////

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	ps5px_log("controller connected: %.*s (%.*s)", static_cast<int>(identifier.size()), identifier.data(),
		static_cast<int>(device_name.size()), device_name.data());
	PS5UI::ControllerChanged(+1);
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
	ps5px_log("controller disconnected: %.*s", static_cast<int>(identifier.size()), identifier.data());
	PS5UI::ControllerChanged(-1);
}

// RetroAchievements. (Each from the emulation thread; the Achievements page of the settings shows
// the account, so it is made again when the account is another one.)
void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// The key kept from the last sign-in is not taken any more.
	ps5px_log("achievements: RetroAchievements asks for a new sign-in");
	PS5UI::Toast("RetroAchievements asks you to sign in again: Settings, Achievements.");
	PS5UI::PagesChanged();
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	ps5px_log("achievements: signed in at RetroAchievements (%u points, %u softcore)", points, sc_points);
	PS5UI::PagesChanged();
}

void Host::OnAchievementsRefreshed()
{
	PS5UI::PagesChanged();
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	ps5px_log("achievements: hardcore mode %s", enabled ? "on" : "off");
}

// A game was marked as a favourite, or no longer (GameList::SaveFavoriteForPath): the interface
// reads its list again by itself, twice a second, when the list's signature is another one.
void Host::OnGameListFavoriteChanged(const std::string& path)
{
}

// A USB keyboard's keys, by the names of PCSX2's HID keyboard (ps5_keyboard.cpp).
std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return PS5Keyboard::NameToCode(str);
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return PS5Keyboard::CodeToName(code);
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

//////////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[])
{
	using namespace PS5Host;

	// Before anything else, while the process has one thread: the title folder, leaving the sandbox
	// for USB drives, klog and the boot log.
	PS5Platform::EarlyInit();
	CrashHandler::Install();
	ps5px_log("PCSX2 %s", GIT_REV);
	// Which build this is: a log then says whether the eboot.bin that ran is the one just copied,
	// and a crash is symbolised against the ELF of the build with this identity, no other.
	ps5px_log("build: %s", ps5px_build_stamp());
	ps5px_log("build identity: %s", ps5px_build_identity());
	ReadTestRun();

	// How late the console's sleeps wake, measured on a thread of its own while the rest starts
	// (PS5Timing.h): the frame limiter's margin. The result is one "timing:" line.
	PS5Timing::SetReport([](const char* line) { ps5px_log("%s", line); });
	PS5Timing::StartProbe();
	// A profiled run's windows also say how evenly the frames reached the screen.
	ps5px_profiler_set_window_extra([] {
		const std::string line = PS5Pacing::TakeWindow();
		if (!line.empty())
			ps5px_log("profile: frames: %s", line.c_str());
	});

	// An update that a power cut stopped half way is put back as it was, before anything reads the
	// title folder (ps5_update.cpp).
	if (PS5Update::Recover(std::string(ps5px_title_dir()) + "/", [](const char* line) { ps5px_log("%s", line); }))
		ps5px_log("update: an interrupted update was undone; the title is as it was before it");

	// Is the title folder whole? A copy that left files out otherwise fails later, somewhere unrelated.
	{
		std::string first_missing;
		if (const unsigned missing = PS5Platform::CheckInstall(&first_missing); missing > 0)
		{
			char message[512];
			std::snprintf(message, sizeof(message),
				"%u file(s) are missing from the title folder's resources (the first: resources/%s). Copy the "
				"whole PPSA99249 folder to the console again; crux2-boot.log lists them.",
				missing, first_missing.c_str());
			ps5_title_fatal(message);
		}
	}

	// Warnings and errors of the emulator into the boot log.
	Log::SetHostOutputLevel(LOGLEVEL_WARNING, &PS5Host::LogCallback);

	const char* hardware_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
		ps5_title_fatal(hardware_error ? hardware_error : "The hardware check failed.");

	// The shares of network.cfg, before the settings: a path in the settings may be on one.
	PS5Platform::MountShares();

	if (!InitializeConfig())
		ps5_title_fatal("The settings could not be loaded. crux2-boot.log has the reason.");
	if (!LoadFonts())
		ps5_title_fatal("The interface's fonts could not be read from the title folder's resources/fonts.");
	// The interface before the emulator's threads, background jobs after them (ps5_priority.h).
	PS5Priority::SetCurrentThread(PS5Priority::Role::Interface);

	// The graphics driver on its own thread: Settings > Interface > PlayStation 5, or a test run's
	// "threaded-recording" line. The driver reads this when the graphics device is made, which
	// the emulation thread does below, so a change of the setting takes effect at the next start.
	if (s_test.threaded_recording || Host::GetBaseBoolSettingValue("PS5", "ThreadedRecording", true))
	{
		if (setenv("RADV_THREADED_RECORDING", "1", 1) == 0)
			ps5px_log("the graphics driver records commands on its own thread (%s)",
				s_test.threaded_recording ? "this test run asks for it" : "the setting under PlayStation 5");
		else
			ps5px_log("RADV_THREADED_RECORDING could not be set: the driver records as usual");
	}

	// How the driver waits for the GPU (PS5 Mesa with PS5CEMU-HAR's patch 0009: Settings > Interface >
	// PlayStation 5): only when changed from the driver's own 1500 us of checking and 1000 us sleeps.
	{
		const int spin = Host::GetBaseIntSettingValue("PS5", "GpuWaitSpinUs", 1500);
		const int sleep = Host::GetBaseIntSettingValue("PS5", "GpuWaitSleepUs", 1000);
		if (spin != 1500 || sleep != 1000)
		{
			setenv("RADV_PS5_WAIT_SPIN_US", std::to_string(spin).c_str(), 1);
			setenv("RADV_PS5_WAIT_SLEEP_US", std::to_string(sleep).c_str(), 1);
			ps5px_log("graphics driver: a wait for the GPU checks for %d us, then sleeps %d us between checks", spin, sleep);
		}
	}

	// 120 Hz. The driver offers a 119.88 Hz mode to a title whose param.json declares it, and it
	// reads that file from /app0, which a title that has left its sandbox for USB drives does not
	// see: v0.8.0 was offered 59.94 Hz only for that reason (a console run, 2026-10-07). The driver
	// takes the file's place from PS5_VIDEOOUT_PARAM_JSON, and the title folder has the same file.
	// With [PS5] OutputRefresh = 60 it is given a file with nothing in it, so that the output is
	// not put into the fast mode at all (the driver does that when the display is opened, whatever
	// mode is taken afterwards).
	{
		// (L1 and R1 held while the title starts set 60 Hz for the next start: bp_pcsx2.cpp.)
		const bool fast = (Host::GetBaseIntSettingValue("PS5", "OutputRefresh", 60) > 60);
		// The driver's own switches for the same choice (PS5_Mesa patches 0015 and 0016, as PSFlyCast sets them):
		// 59.94 Hz whatever param.json declares, or, at 120 Hz, variable refresh ([PS5] VariableRefresh, off).
		if (!fast)
			setenv("PS5_VIDEOOUT_59HZ", "1", 1);
		else if (Host::GetBaseBoolSettingValue("PS5", "VariableRefresh", false))
		{
			setenv("PS5_VIDEOOUT_VRR", "1", 1);
			ps5px_log("display: variable refresh is asked for (Settings > Interface > PlayStation 5)");
		}
		const std::string declared = fast ? Path::Combine(ps5px_title_dir(), "sce_sys/param.json") : std::string("/dev/null");
		const bool there = !fast || FileSystem::FileExists(declared.c_str());
		if (there && setenv("PS5_VIDEOOUT_PARAM_JSON", declared.c_str(), 1) == 0)
		{
			if (fast)
				ps5px_log("display: 120 Hz is asked for (the driver reads the title's declaration from %s)", declared.c_str());
			else
				ps5px_log("display: 60 Hz, as set under PlayStation 5: 120 Hz is not asked for");
		}
		else
		{
			ps5px_log("display: 120 Hz cannot be asked for (%s): the driver looks in /app0 by itself",
				there ? "PS5_VIDEOOUT_PARAM_JSON could not be set" : "the title folder has no sce_sys/param.json");
		}
	}

	// The emulation thread, with the stack the recompilers want; this thread only waits for it.
	s_state.cpu_thread.SetStackSize(CPU_THREAD_STACK_SIZE);
	if (!s_state.cpu_thread.Start(&PS5Host::CPUThreadEntryPoint))
		ps5_title_fatal("The emulation thread could not be started.");
	s_state.cpu_thread.Join();
	PS5Pgo::Write("Crux2 closed");

	// The line a test run's capture waits for: everything is saved and closed by now.
	if (s_test.active)
		ps5px_log("test: end");

	// The CRT hands the return to catchReturnFromMain, which has the shell close the title.
	return 0;
}
