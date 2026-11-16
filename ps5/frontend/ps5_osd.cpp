// Crux2 - the performance overlay: the runtime of ps5_osd.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// What the console tells a title is what is shown: the graphics chip's load and its time a frame,
// the emulator's threads' loads, the memory in use, and - when the console's kernel has the calls,
// found while the title runs - the processor's clock and temperature. The chip's clock, temperature
// and power are not told to a title, so they are not shown (nothing here is made up). The drawing
// is ps5_osd_draw.h, which the interface's PC preview pictures too; the levels (white, amber, red)
// and the sizes are ps5_osd_logic.h's.

#include "ps5_osd.h"
#include "ps5_osd_draw.h"
#include "ps5_osd_logic.h"
#include "ps5_pacing.h"
#include "ps5_pgs.h"
#include "ps5_keyboard.h" // PS5SystemModules

#include "Config.h"
#include "GS/GS.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "Host.h"
#include "ImGui/ImGuiManager.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/HostSys.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

extern "C" const char* ps5px_build_version(void);
ImFont* ps5px_interface_font(int which);

namespace PS5Osd
{
	namespace
	{
		using namespace PS5OsdDraw;

		PS5OsdDraw::Overlay s_overlay; // made again ten times a second, kept while the game is paused
		Common::Timer s_refresh(0.0);
		Common::Timer s_style_check(0.0);
		int s_style = 0;
		int s_size = PS5OsdLogic::DEFAULT_SIZE; // [PS5] OsdSize

		// The kernel's own figures, when it has the calls (looked up once).
		using CpuTempFn = int (*)(int*);
		using CpuFreqFn = uint64_t (*)(void);
		CpuTempFn s_cpu_temp = nullptr;
		CpuFreqFn s_cpu_freq = nullptr;
		bool s_kernel_looked = false;

		void LookUpKernel()
		{
			if (s_kernel_looked)
				return;
			s_kernel_looked = true;
			s_cpu_temp = reinterpret_cast<CpuTempFn>(PS5SystemModules::FindFunction("libkernel", "sceKernelGetCpuTemperature", "qiL4fFObAxM"));
			s_cpu_freq = reinterpret_cast<CpuFreqFn>(PS5SystemModules::FindFunction("libkernel", "sceKernelGetCpuFrequency", "iUohlUDiGac"));
		}

		std::string Number(double v, int decimals)
		{
			return decimals <= 0 ? fmt::format("{:.0f}", v) : fmt::format("{:.{}f}", v, decimals);
		}

		/// The frames' figures from the frame time history (milliseconds, the last 150 frames).
		struct FrameFigures
		{
			bool any = false;
			double last_ms = 0, max_fps = 0, avg_fps = 0, min_fps = 0, low1_fps = 0, low01_fps = 0;
		};

		FrameFigures Figures()
		{
			FrameFigures f;
			const auto& history = PerformanceMetrics::GetFrameTimeHistory();
			const u32 pos = PerformanceMetrics::GetFrameTimeHistoryPos();
			std::vector<float> times;
			times.reserve(history.size());
			double sum = 0;
			for (float t : history)
			{
				if (t > 0.01f)
				{
					times.push_back(t);
					sum += t;
				}
			}
			if (times.empty())
				return f;
			f.any = true;
			const u32 n = static_cast<u32>(history.size());
			for (u32 k = 1; k <= n; k++)
			{
				const float t = history[(pos + n - k) % n];
				if (t > 0.01f)
				{
					f.last_ms = t;
					break;
				}
			}
			std::sort(times.begin(), times.end());
			const auto fps = [](double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; };
			f.max_fps = fps(times.front());
			f.min_fps = fps(times.back());
			f.avg_fps = fps(sum / static_cast<double>(times.size()));
			// The slowest 1% and 0.1% of the frames: the frame time that many from the top.
			const auto at = [&times](double share) {
				const size_t i = std::min(times.size() - 1, static_cast<size_t>(std::floor((1.0 - share) * static_cast<double>(times.size()))));
				return static_cast<double>(times[i]);
			};
			f.low1_fps = fps(at(0.01));
			f.low01_fps = fps(at(0.001));
			return f;
		}

		using PS5OsdLogic::Level;

		void Stat(const char* label, std::string value, const char* unit, Level level = Level::Fine)
		{
			s_overlay.stats.push_back({label, std::move(value), unit, level});
		}

		void Load(const char* label, float percent)
		{
			percent = std::clamp(percent, 0.0f, 100.0f);
			s_overlay.meters.push_back({label, percent, Number(percent, 0), "%", PS5OsdLogic::LoadLevel(percent)});
		}

		PS5OsdLogic::Switches SwitchesNow()
		{
			PS5OsdLogic::Switches s;
			s.fps = GSConfig.OsdShowFPS;
			s.gpu = GSConfig.OsdShowGPU;
			s.cpu = GSConfig.OsdShowCPU;
			s.speed = GSConfig.OsdShowSpeed;
			s.frame_times = GSConfig.OsdShowFrameTimes;
			s.resolution = GSConfig.OsdShowResolution;
			s.hardware = GSConfig.OsdShowHardwareInfo;
			s.vps = GSConfig.OsdShowVPS;
			s.gs_stats = GSConfig.OsdShowGSStats;
			s.gpu_stats = GSConfig.OsdShowGPUStats;
			s.version = GSConfig.OsdShowVersion;
			return s;
		}

		void Refresh()
		{
			const PS5OsdLogic::Switches on = SwitchesNow();
			const PS5OsdLogic::Size size = PS5OsdLogic::EffectiveSize(s_size, on);
			const bool custom = size == PS5OsdLogic::Custom;
			const bool card = size == PS5OsdLogic::Full || custom;
			Overlay& o = s_overlay;
			o = Overlay{};
			o.size = size;
			LookUpKernel();
			const float speed = PerformanceMetrics::GetSpeed();
			const float target = VMManager::GetTargetSpeed();
			o.budget_ms = PS5OsdLogic::FrameBudgetMs(VMManager::GetFrameRate(), target);

			// ---- the frames: the game's rate (and with frame generation running, the screen's),
			// coloured by the speed against the speed asked for.
			const Level speed_level = PS5OsdLogic::SpeedLevel(speed, target);
			if (on.fps)
			{
				o.fps = true;
				o.game_fps = PerformanceMetrics::GetInternalFPS();
				o.fps_level = speed_level;
				// Frame generation: the pictures a second the screen is given (counted where they are
				// presented, over the last half second).
				if (const float given = PS5Pacing::FrameGenShownPerSecond(); given > 0.5f)
				{
					o.screen_fps = given;
					if (custom)
						Stat("made", Number(PS5Pacing::FrameGenMadePerSecond(), 0), "FPS");
				}
				else if (const float shown = PS5Pacing::ShownPerSecond(); custom && shown > o.game_fps + 2.0f)
					Stat("shown", Number(shown, 0), "FPS");
			}
			if (card && on.speed)
			{
				o.speed = true;
				o.speed_percent = speed;
				o.speed_level = speed_level;
			}
			if (card && on.vps)
			{
				o.vps = true;
				o.vps_value = PerformanceMetrics::GetFPS();
			}
			if (card && on.frame_times)
			{
				const FrameFigures f = Figures();
				o.graph = true;
				if (f.any)
				{
					o.frame_ms = true;
					o.last_ms = static_cast<float>(f.last_ms);
					o.frame_level = PS5OsdLogic::FrameLevel(o.last_ms, o.budget_ms);
					o.low1_fps = static_cast<float>(f.low1_fps);
					o.low01_fps = static_cast<float>(f.low01_fps);
					if (custom)
					{
						Stat("max", Number(f.max_fps, 0), "FPS");
						Stat("average", Number(f.avg_fps, 0), "FPS");
						Stat("min", Number(f.min_fps, 0), "FPS");
					}
				}
			}

			// ---- the graphics chip (paraLLEl-GS drawing: its own device's time, from its timestamps,
			// against the frame's period; PCSX2's GPU figures see only PCSX2's device, which then does
			// little)
			const bool pgs = PS5PGS::Active();
			const PS5PGS::Figures pf = pgs ? PS5PGS::GetFigures() : PS5PGS::Figures{};
			if (on.gpu)
			{
				if (pgs)
				{
					// (Against the game's own rate: a PAL game's frame is 20 ms, not 16.7.)
					Load("GPU", pf.gpu_ms > 0.0f ? 100.0f * pf.gpu_ms / o.budget_ms : 0.0f);
					if (custom)
					{
						Stat("GPU time", Number(pf.gpu_ms, 1), "ms");
						Stat("shading", Number(pf.gpu_shading_ms, 1), "ms");
						Stat("samples", Number(pf.samples, 0), "");
					}
				}
				else
				{
					Load("GPU", PerformanceMetrics::GetGPUUsage());
					if (custom)
						Stat("GPU time", Number(PerformanceMetrics::GetGPUAverageTime(), 1), "ms");
				}
			}

			// ---- the processor: the emulator's threads
			if (on.cpu)
			{
				Load("EE", static_cast<float>(PerformanceMetrics::GetCPUThreadUsage()));
				Load("GS", PerformanceMetrics::GetGSThreadUsage());
				if (THREAD_VU1)
					Load("VU", PerformanceMetrics::GetVUThreadUsage());
				if (card && pgs)
				{
					Load("PGS", pf.thread_percent);
					if (custom)
					{
						Stat("GS wait", Number(pf.wait_ms, 1), "ms"); // the GS thread's, for the graphics card
						if (pf.readbacks > 0.0f) // the game reads its picture back: what that stall costs a frame
							Stat("reads", Number(pf.readback_wait_ms, 1), "ms", Level::Near);
					}
				}
				if (card)
				{
					const u32 sw = PerformanceMetrics::GetGSSWThreadCount();
					for (u32 t = 0; t < sw; t++)
					{
						const float percent = std::clamp(static_cast<float>(PerformanceMetrics::GetGSSWThreadUsage(t)), 0.0f, 100.0f);
						s_overlay.meters.push_back({fmt::format("SW{}", t), percent, Number(percent, 0), "%", PS5OsdLogic::LoadLevel(percent)});
					}
				}
			}

			// ---- what the renderer did in a frame (Settings > On-screen display > Statistics): PCSX2's
			// own counters, averaged over its windows of 32 frames (GSRenderer::VSync). Refresh runs on
			// the GS thread (ImGuiManager::RenderOSD from GSRenderer::EndPresentFrame), whose state
			// g_perfmon is.
			if (card && on.gs_stats && GSGetCurrentRenderer() != GSRendererType::Null)
			{
				GSPerfMon& pm = g_perfmon;
				const auto count = [](GSPerfMon::counter_t c) { return Number(std::ceil(g_perfmon.Get(c)), 0); };
				const auto flagged = [&pm](GSPerfMon::counter_t c) { return pm.Get(c) > 0.0 ? Level::Near : Level::Fine; };
				Stat("prims", Number(pm.Get(GSPerfMon::Prim), 0), "");
				Stat("draws", Number(pm.Get(GSPerfMon::Draw), 0), "");
				if (GSGetCurrentRenderer() == GSRendererType::SW)
					Stat("syncs", count(GSPerfMon::SyncPoint), "");
				else
				{
					Stat("calls", count(GSPerfMon::DrawCalls), "");
					Stat("barriers", count(GSPerfMon::Barriers), "");
					Stat("passes", count(GSPerfMon::RenderPasses), "");
					Stat("copies", count(GSPerfMon::TextureCopies), "");
					Stat("uploads", count(GSPerfMon::TextureUploads), "");
					Stat("readbacks", count(GSPerfMon::Readbacks), "", flagged(GSPerfMon::Readbacks));
					// Crux2's own: passes ended for a barrier on sampling the target (0 here unless
					// framebuffer fetch is on), colour targets moved to the feedback loop layout (DCC
					// decompressed), primitive ID DATE setups, copies of a target the draw reads; then the
					// barriers left out because nothing wrote the target since the last one, and the copies
					// reused because their source was not written since (neither is in "barriers"/"RT copies").
					Stat("pass splits", count(GSPerfMon::BarriersExternal), "", flagged(GSPerfMon::BarriersExternal));
					Stat("decompress", count(GSPerfMon::FeedbackDecompress), "", flagged(GSPerfMon::FeedbackDecompress));
					Stat("DATE setup", count(GSPerfMon::DatePrimID), "");
					Stat("RT copies", count(GSPerfMon::RTCopies), "");
					Stat("barr. skipped", count(GSPerfMon::BarriersSkipped), "");
					Stat("copies reused", count(GSPerfMon::RTCopiesReused), "");
					// Render passes the ordered view's draws kept (PS5ROVKeepPass): not in "passes".
					Stat("passes kept", count(GSPerfMon::ROVPassesKept), "");
				}
			}

			// ---- the graphics chip's work (Settings > On-screen display > GPU statistics): its time a
			// frame by the kind of render pass (the latest window; the test log's sums are kept apart),
			// and the shader invocations PCSX2 counts.
			if (card && on.gpu_stats && g_gs_device && !pgs)
			{
				GSDevice::PS5GpuSections w;
				if (g_gs_device->PS5GetGpuSections(w))
				{
					Stat("GPU total", Number(w.total_ms, 2), "ms");
					Stat("ordered", Number(w.ordered_ms, 2), "ms");
					Stat("barrier", Number(w.barrier_ms, 2), "ms");
					Stat("draw", Number(w.draw_ms, 2), "ms");
					Stat("convert", Number(w.conversion_ms, 2), "ms");
					Stat("present", Number(w.present_ms, 2), "ms");
					Stat("outside", Number(w.outside_ms, 2), "ms");
					Stat("timed", Number(w.passes, 0), "passes");
				}
				const auto units = [](double v) {
					return v >= 1e6 ? fmt::format("{:.1f}M", v / 1e6) : v >= 1e3 ? fmt::format("{:.1f}K", v / 1e3) : Number(v, 0);
				};
				Stat("VS", units(PerformanceMetrics::GetGPUAverageVSInvocations()), "");
				Stat("PS", units(PerformanceMetrics::GetGPUAveragePSInvocations()), "");
			}

			// ---- the footer: the internal resolution and its multiple; the processor's temperature
			// (and clock in Custom) and the memory the title has taken, when the kernel tells them.
			if (card && on.resolution)
			{
				int w = 0, h = 0;
				GSgetInternalResolution(&w, &h);
				o.footer_left = fmt::format("{}x{}", w, h);
				if (GSConfig.UseHardwareRenderer())
					o.footer_left += fmt::format("  \xc2\xb7  {:g}x", GSConfig.UpscaleMultiplier);
			}
			if (card && on.hardware)
			{
				std::string hw;
				int temp = 0;
				if (s_cpu_temp && s_cpu_temp(&temp) == 0 && temp > 0 && temp < 130)
					hw = fmt::format("{} \xc2\xb0" "C", temp);
				if (custom && s_cpu_freq)
				{
					const uint64_t hz = s_cpu_freq();
					if (hz > 100000000ULL && hz < 10000000000ULL)
						hw += fmt::format("{}{:.0f} MHz", hw.empty() ? "" : "  \xc2\xb7  ", static_cast<double>(hz) / 1e6);
				}
				// The direct memory the title has taken (its pool less the largest free run).
				const u64 total = GetPhysicalMemory(), free_run = GetAvailablePhysicalMemory();
				if (total > free_run)
					hw += fmt::format("{}{:.1f} GB", hw.empty() ? "" : "  \xc2\xb7  ", static_cast<double>(total - free_run) / (1024.0 * 1024.0 * 1024.0));
				o.footer_right = std::move(hw);
			}
			if (card && on.version)
				o.version = fmt::format("Crux2 {}", ps5px_build_version());
		}
	} // namespace

	bool DrawPanel(float& position_y, float scale, float margin)
	{
		// The style and the size, looked at twice a second (a setting is not read every frame).
		if (s_style_check.GetTimeSeconds() >= 0.5)
		{
			s_style_check.Reset();
			s_style = Host::GetBaseIntSettingValue("PS5", "OsdStyle", 0);
			s_size = Host::GetBaseIntSettingValue("PS5", "OsdSize", PS5OsdLogic::DEFAULT_SIZE);
		}
		if (s_style != 0)
			return false;

		if (VMManager::GetState() != VMState::Paused && s_refresh.GetTimeSeconds() >= 0.1)
		{
			s_refresh.Reset();
			Refresh();
		}
		const auto& history = PerformanceMetrics::GetFrameTimeHistory();
		s_overlay.history = history.data();
		s_overlay.history_size = static_cast<uint32_t>(history.size());
		s_overlay.history_pos = PerformanceMetrics::GetFrameTimeHistoryPos();

		// Roboto (the interface's text face and its bold), else PCSX2's OSD face. The design's pixels
		// are a 1080-line screen's, times the OSD scale.
		PS5OsdDraw::Fonts fonts;
		fonts.regular = ps5px_interface_font(0) ? ps5px_interface_font(0) : ImGuiManager::GetOSDFont();
		fonts.bold = GSConfig.OsdBoldText && ps5px_interface_font(1) ? ps5px_interface_font(1) : fonts.regular;
		const float screen_h = ImGuiManager::GetWindowHeight();
		const float u = std::max(0.5f, scale) * std::max(0.5f, screen_h / 1080.0f);
		const float bottom = PS5OsdDraw::Draw(ImGui::GetBackgroundDrawList(), fonts, u, s_overlay, static_cast<int>(GSConfig.OsdPerformancePos),
			margin, position_y, ImGuiManager::GetWindowWidth(), screen_h);
		position_y = bottom + std::round(margin * 0.5f);
		return true;
	}
} // namespace PS5Osd
