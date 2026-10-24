// Crux2 - the on-screen display's figures as one panel: the runtime of ps5_osd.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// What the console tells a title is what is shown: the graphics chip's load and its time a frame,
// the emulator's threads' loads, the memory in use, and - when the console's kernel has the calls,
// found while the title runs - the processor's clock and temperature. The chip's clock, temperature
// and power are not told to a title, so they are not shown (nothing here is made up). The drawing
// is ps5_osd_draw.h, which the interface's PC preview pictures too.

#include "ps5_osd.h"
#include "ps5_osd_draw.h"
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

		PS5OsdDraw::Panel s_panel; // made again ten times a second, kept while the game is paused
		Common::Timer s_refresh(0.0);
		Common::Timer s_style_check(0.0);
		int s_style = 0;

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

		void Add(const char* name, std::string value, const char* unit, ImU32 name_colour, ImU32 value_colour)
		{
			Row& r = s_panel.rows.emplace_back();
			r.name = name;
			r.value = std::move(value);
			r.unit = unit;
			r.name_colour = name_colour;
			r.value_colour = value_colour;
		}

		void Section()
		{
			if (!s_panel.rows.empty())
				s_panel.sections.push_back(s_panel.rows.size());
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

		void Refresh()
		{
			s_panel.rows.clear();
			s_panel.sections.clear();
			LookUpKernel();
			const float speed = PerformanceMetrics::GetSpeed();

			// ---- the graphics chip (paraLLEl-GS drawing: its own device's time, from its timestamps,
			// against the frame's period; PCSX2's GPU figures see only PCSX2's device, which then does
			// little)
			const bool pgs = PS5PGS::Active();
			const PS5PGS::Figures pf = pgs ? PS5PGS::GetFigures() : PS5PGS::Figures{};
			if (GSConfig.OsdShowGPU)
			{
				Section();
				if (pgs)
				{
					const float target = VMManager::GetTargetSpeed();
					const float period_ms = target > 0.0f ? 1000.0f / (target * 60.0f) : 16.7f;
					const float percent = pf.gpu_ms > 0.0f ? std::min(100.0f, 100.0f * pf.gpu_ms / period_ms) : 0.0f;
					Add("GPU", Number(percent, 0), "%", GPU_NAME, HOT_VALUE);
					Add("time", Number(pf.gpu_ms, 1), "ms", GPU_NAME, HOT_VALUE);
					Add("shading", Number(pf.gpu_shading_ms, 1), "ms", GPU_NAME, HOT_VALUE);
					Add("samples", Number(pf.samples, 0), "", GPU_NAME, HOT_VALUE);
				}
				else
				{
					Add("GPU", Number(std::min(100.0f, PerformanceMetrics::GetGPUUsage()), 0), "%", GPU_NAME, HOT_VALUE);
					Add("time", Number(PerformanceMetrics::GetGPUAverageTime(), 1), "ms", GPU_NAME, HOT_VALUE);
				}
			}

			// ---- what the renderer did in a frame (Settings > On-screen display > Statistics): PCSX2's
			// own counters, averaged over its windows of 32 frames (GSRenderer::VSync). Refresh runs on
			// the GS thread (ImGuiManager::RenderOSD from GSRenderer::EndPresentFrame), whose state
			// g_perfmon is.
			if (GSConfig.OsdShowGSStats && GSGetCurrentRenderer() != GSRendererType::Null)
			{
				Section();
				GSPerfMon& pm = g_perfmon;
				const auto count = [](GSPerfMon::counter_t c) { return Number(std::ceil(g_perfmon.Get(c)), 0); };
				if (GSGetCurrentRenderer() == GSRendererType::SW)
				{
					Add("prims", Number(pm.Get(GSPerfMon::Prim), 0), "", GPU_NAME, COOL_VALUE);
					Add("draws", Number(pm.Get(GSPerfMon::Draw), 0), "", GPU_NAME, COOL_VALUE);
					Add("syncs", count(GSPerfMon::SyncPoint), "", GPU_NAME, COOL_VALUE);
				}
				else
				{
					Add("prims", Number(pm.Get(GSPerfMon::Prim), 0), "", GPU_NAME, COOL_VALUE);
					Add("draws", Number(pm.Get(GSPerfMon::Draw), 0), "", GPU_NAME, COOL_VALUE);
					Add("calls", count(GSPerfMon::DrawCalls), "", GPU_NAME, COOL_VALUE);
					Add("barriers", count(GSPerfMon::Barriers), "", GPU_NAME, COOL_VALUE);
					Add("passes", count(GSPerfMon::RenderPasses), "", GPU_NAME, COOL_VALUE);
					Add("copies", count(GSPerfMon::TextureCopies), "", GPU_NAME, COOL_VALUE);
					Add("uploads", count(GSPerfMon::TextureUploads), "", GPU_NAME, COOL_VALUE);
					Add("readbacks", count(GSPerfMon::Readbacks), "", GPU_NAME, pm.Get(GSPerfMon::Readbacks) > 0.0 ? HOT_VALUE : COOL_VALUE);
					// Crux2's own: passes ended for a barrier on sampling the target (0 here unless
					// framebuffer fetch is on), colour targets moved to the feedback loop layout (DCC
					// decompressed), primitive ID DATE setups, copies of a target the draw reads; then the
					// barriers left out because nothing wrote the target since the last one, and the copies
					// reused because their source was not written since (neither is in "barriers"/"RT copies").
					Add("pass splits", count(GSPerfMon::BarriersExternal), "", GPU_NAME,
						pm.Get(GSPerfMon::BarriersExternal) > 0.0 ? HOT_VALUE : COOL_VALUE);
					Add("decompress", count(GSPerfMon::FeedbackDecompress), "", GPU_NAME,
						pm.Get(GSPerfMon::FeedbackDecompress) > 0.0 ? HOT_VALUE : COOL_VALUE);
					Add("DATE setup", count(GSPerfMon::DatePrimID), "", GPU_NAME, COOL_VALUE);
					Add("RT copies", count(GSPerfMon::RTCopies), "", GPU_NAME, COOL_VALUE);
					Add("barriers skipped", count(GSPerfMon::BarriersSkipped), "", GPU_NAME, COOL_VALUE);
					Add("copies reused", count(GSPerfMon::RTCopiesReused), "", GPU_NAME, COOL_VALUE);
					// Render passes the ordered view's draws kept (PS5ROVKeepPass): not in "passes".
					Add("passes kept", count(GSPerfMon::ROVPassesKept), "", GPU_NAME, COOL_VALUE);
				}
			}

			// ---- the graphics chip's work (Settings > On-screen display > GPU statistics): its time a
			// frame by the kind of render pass (the latest window; the test log's sums are kept apart),
			// and the shader invocations PCSX2 counts.
			if (GSConfig.OsdShowGPUStats && g_gs_device && !pgs)
			{
				Section();
				GSDevice::PS5GpuSections w;
				if (g_gs_device->PS5GetGpuSections(w))
				{
					Add("GPU", Number(w.total_ms, 2), "ms", GPU_NAME, HOT_VALUE);
					Add("ordered", Number(w.ordered_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("barrier", Number(w.barrier_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("draw", Number(w.draw_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("convert", Number(w.conversion_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("present", Number(w.present_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("outside", Number(w.outside_ms, 2), "ms", GPU_NAME, COOL_VALUE);
					Add("timed", Number(w.passes, 0), "passes", GPU_NAME, COOL_VALUE);
				}
				const auto units = [](double v) {
					return v >= 1e6 ? fmt::format("{:.1f}M", v / 1e6) : v >= 1e3 ? fmt::format("{:.1f}K", v / 1e3) : Number(v, 0);
				};
				Add("VS", units(PerformanceMetrics::GetGPUAverageVSInvocations()), "", GPU_NAME, COOL_VALUE);
				Add("PS", units(PerformanceMetrics::GetGPUAveragePSInvocations()), "", GPU_NAME, COOL_VALUE);
			}

			// ---- the processor
			if (GSConfig.OsdShowCPU || GSConfig.OsdShowHardwareInfo)
			{
				Section();
				if (GSConfig.OsdShowCPU)
				{
					Add("EE", Number(std::min(100.0, PerformanceMetrics::GetCPUThreadUsage()), 0), "%", CPU_NAME, HOT_VALUE);
					Add("GS", Number(std::min(100.0f, PerformanceMetrics::GetGSThreadUsage()), 0), "%", CPU_NAME, HOT_VALUE);
					if (THREAD_VU1)
						Add("VU", Number(std::min(100.0f, PerformanceMetrics::GetVUThreadUsage()), 0), "%", CPU_NAME, HOT_VALUE);
					if (pgs)
					{
						Add("PGS", Number(std::min(100.0f, pf.thread_percent), 0), "%", CPU_NAME, HOT_VALUE);
						Add("wait", Number(pf.wait_ms, 1), "ms", CPU_NAME, HOT_VALUE); // the GS thread's, for the graphics card
						if (pf.readbacks > 0.0f) // the game reads its picture back: what that stall costs a frame
							Add("reads", Number(pf.readback_wait_ms, 1), "ms", CPU_NAME, HOT_VALUE);
					}
					const u32 sw = PerformanceMetrics::GetGSSWThreadCount();
					for (u32 t = 0; t < sw; t++)
					{
						Row& r = s_panel.rows.emplace_back();
						r.name = fmt::format("SW{}", t);
						r.value = Number(std::min(100.0, PerformanceMetrics::GetGSSWThreadUsage(t)), 0);
						r.unit = "%";
						r.name_colour = CPU_NAME;
						r.value_colour = HOT_VALUE;
					}
				}
				if (GSConfig.OsdShowHardwareInfo)
				{
					int temp = 0;
					if (s_cpu_temp && s_cpu_temp(&temp) == 0 && temp > 0 && temp < 130)
						Add("temp", Number(temp, 0), "\xc2\xb0" "C", CPU_NAME, HOT_VALUE);
					if (s_cpu_freq)
					{
						const uint64_t hz = s_cpu_freq();
						if (hz > 100000000ULL && hz < 10000000000ULL)
							Add("clock", Number(static_cast<double>(hz) / 1e6, 0), "MHz", CPU_NAME, HOT_VALUE);
					}
					// The direct memory the title has taken (its pool less the largest free run).
					const u64 total = GetPhysicalMemory(), free_run = GetAvailablePhysicalMemory();
					if (total > free_run)
						Add("RAM", Number(static_cast<double>((total - free_run) >> 20), 0), "MB", CPU_NAME, HOT_VALUE);
				}
			}

			// ---- the frames
			if (GSConfig.OsdShowFPS || GSConfig.OsdShowVPS || GSConfig.OsdShowSpeed || GSConfig.OsdShowFrameTimes ||
				GSConfig.OsdShowResolution || GSConfig.OsdShowVersion)
			{
				Section();
				const ImU32 speed_colour = (speed < 95.0f) ? SLOW_VALUE : COOL_VALUE;
				if (GSConfig.OsdShowFPS)
				{
					Add(GSConfig.Renderer == GSRendererType::SW ? "Software" : "Vulkan", Number(PerformanceMetrics::GetInternalFPS(), 0), "FPS",
						FRAME_NAME, speed_colour);
					if (const float shown = PS5Pacing::ShownPerSecond(); shown > PerformanceMetrics::GetInternalFPS() + 2.0f)
						Add("shown", Number(shown, 0), "FPS", FRAME_NAME, COOL_VALUE);
				}
				if (GSConfig.OsdShowVPS)
					Add("VPS", Number(PerformanceMetrics::GetFPS(), 0), "", FRAME_NAME, COOL_VALUE);
				if (GSConfig.OsdShowSpeed)
					Add("Speed", Number(speed, 0), "%", FRAME_NAME, speed_colour);
				if (GSConfig.OsdShowFrameTimes)
				{
					const FrameFigures f = Figures();
					if (f.any)
					{
						Add("Frametime", Number(f.last_ms, 1), "ms", FRAME_NAME, COOL_VALUE);
						Add("Max", Number(f.max_fps, 0), "FPS", FRAME_NAME, COOL_VALUE);
						Add("Average", Number(f.avg_fps, 0), "FPS", FRAME_NAME, COOL_VALUE);
						Add("Min", Number(f.min_fps, 0), "FPS", FRAME_NAME, COOL_VALUE);
						Add("1% Low", Number(f.low1_fps, 0), "FPS", FRAME_NAME, COOL_VALUE);
						Add("0.1% Low", Number(f.low01_fps, 0), "FPS", FRAME_NAME, COOL_VALUE);
					}
				}
				if (GSConfig.OsdShowResolution)
				{
					int w = 0, h = 0;
					GSgetInternalResolution(&w, &h);
					Add("Res", fmt::format("{}x{}", w, h), "", FRAME_NAME, COOL_VALUE);
				}
				if (GSConfig.OsdShowVersion)
					Add("Crux2", ps5px_build_version(), "", FRAME_NAME, COOL_VALUE);
			}
			s_panel.graph = GSConfig.OsdShowFrameTimes;
		}
	} // namespace

	bool DrawPanel(float& position_y, float scale, float margin)
	{
		// The style, looked at twice a second (a setting is not read every frame).
		if (s_style_check.GetTimeSeconds() >= 0.5)
		{
			s_style_check.Reset();
			s_style = Host::GetBaseIntSettingValue("PS5", "OsdStyle", 0);
		}
		if (s_style != 0)
			return false;

		if (VMManager::GetState() != VMState::Paused && s_refresh.GetTimeSeconds() >= 0.1)
		{
			s_refresh.Reset();
			Refresh();
		}
		const auto& history = PerformanceMetrics::GetFrameTimeHistory();
		s_panel.history = history.data();
		s_panel.history_size = static_cast<uint32_t>(history.size());
		s_panel.history_pos = PerformanceMetrics::GetFrameTimeHistoryPos();

		ImFont* const font = ps5px_interface_font(2) ? ps5px_interface_font(2) : ImGuiManager::GetOSDFont();
		const float size = std::round(ImGuiManager::GetFontSizeStandard() * 1.3f);
		const float bottom = PS5OsdDraw::Draw(ImGui::GetBackgroundDrawList(), font, size, s_panel, static_cast<int>(GSConfig.OsdPerformancePos),
			scale, margin, position_y, ImGuiManager::GetWindowWidth(), ImGuiManager::GetWindowHeight());
		position_y = bottom + std::round(margin * 0.5f);
		return true;
	}
} // namespace PS5Osd
