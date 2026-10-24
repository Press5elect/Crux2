// Crux2 - paraLLEl-GS as a second GS renderer: the runtime of ps5_pgs.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pgs.h"
#include "ps5_pgs_packet.h"

#include "../pgs/gs_gpu.h"

#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "Host.h"
#include "SaveState.h"
#include "VMManager.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5PGS
{
	namespace
	{
		std::unique_ptr<GSGpuRenderer> s_gs;
		std::atomic<bool> s_active{false}; // (read on the EE thread too: MTGS's readbacks)
		GSGpuScanout s_scanout_sent;       // the display settings paraLLEl-GS has (SendScanout)
		bool s_scanout_any = false;
		// The GS thread's waits for the graphics card (VSync's waitPresentBacklog), summed over the last second.
		Common::Timer s_wait_window;
		double s_wait_seconds = 0.0;
		uint32_t s_wait_frames = 0;
		float s_wait_ms = 0.0f;
		uint32_t s_samples_now = 0;
		uint32_t s_samples_setting = 0; // Settings' samples and twice-the-size, as last seen (a change is taken live)
		bool s_high_res_setting = true;
		uint32_t s_status_seconds = 0;
		const uint8_t* s_regs = nullptr; // the PS2's GS privileged registers (PMODE at 0, 16 bytes apart)
		uint64_t s_tick = 0;
		uint64_t s_last_seq = 0;
		std::atomic<bool> s_stop{false};
		GSTexture* s_picture = nullptr; // a texture of the GS device, remade at a new size
		uint32_t s_picture_w = 0, s_picture_h = 0;
		std::vector<uint32_t> s_opaque; // the picture made opaque, where the texture cannot be mapped
		float s_picture_scale = 1.0f; // the picture's height over the game's (2 at twice the size)
		std::vector<uint8_t> s_state; // the blob of the size asked for, kept for the save that follows

		uint32_t ReadSamples()
		{
			const int v = Host::GetIntSettingValue("EmuCore/GS", "PS5ParallelGSSamples", 8);
			for (uint32_t r : {1u, 2u, 4u, 8u, 16u})
			{
				if (static_cast<uint32_t>(v) == r)
					return r;
			}
			return 8;
		}
	} // namespace

	bool Active()
	{
		return s_active.load(std::memory_order_acquire);
	}

	namespace
	{
		void Start()
		{
			if (s_gs || !s_regs)
				return;
			if (!Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGS", false))
			{
				ps5px_log("paraLLEl-GS: off for this game; PCSX2's renderer draws");
				return;
			}
			// The renderer's switches that it reads from its environment: 2-D (sprites, text) super-sampled like
			// 3-D, or kept to whole pixels; no 4x scanout (at 16 samples it would be 2560 wide, read back each
			// frame); no shared pictures (Windows only).
			setenv("VENDETTA_UPSCALE_SPRITES", Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSSharp2D", false) ? "1" : "0", 1);
			setenv("VENDETTA_HIRES4", "0", 1);
			unsetenv("VENDETTA_PRESENT");
			GSGpuOptions options;
			options.superSampling = ReadSamples();
			options.orderedGrid = true;
			// The samples kept where a game draws a picture into a texture and draws that to the screen (its
			// blurs, glows and other passes over the whole picture): without them such a picture is the PS2's
			// own resolution again, however many samples it was drawn with.
			options.superSampledTextures = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSTextures", false);
			// Room for a few frames of a heavy game's GS input (up to about 13 MB a frame measured), so the
			// GS thread hands it over without waiting while paraLLEl-GS's thread works on the frame before.
			options.ringBytes = size_t{64} << 20;
			// Each picture is handed over in the buffer it was read back into and made opaque here, on its way
			// into the texture: one copy fewer on paraLLEl-GS's thread.
			options.mappedFrames = true;
			options.gpuTimestamps = true; // the graphics card's time a frame, for the on-screen display and automatic quality
			options.highResolutionScanout = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSHighRes", true);
			options.fsr = false; // Crux2's own presentation enlarges the picture
			options.libraryLog = false;
			std::string why;
			s_stop.store(false);
			s_gs = makeGpuRenderer(options, why);
			if (!s_gs)
			{
				ps5px_log("paraLLEl-GS: could not start (%s); PCSX2's renderer draws", why.c_str());
				Host::AddOSDMessage(fmt::format("paraLLEl-GS could not start ({}); PCSX2's renderer draws this game.", why), 10.0f);
				return;
			}
			s_tick = 0;
			s_last_seq = 0;
			s_scanout_any = false;
			s_samples_now = options.superSampling;
			s_wait_seconds = 0.0;
			s_wait_frames = 0;
			s_wait_ms = 0.0f;
			s_status_seconds = 0;
			s_wait_window.Reset();
			s_active.store(true, std::memory_order_release);
			ps5px_log("paraLLEl-GS: draws this game on %s: %u samples a pixel%s, 2-D %s, pictures drawn into textures %s",
				s_gs->deviceName(), options.superSampling,
				options.highResolutionScanout && options.superSampling >= 4 ? ", the picture at twice the size" : "",
				Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSSharp2D", false) ? "super-sampled too" : "at whole pixels",
				options.superSampledTextures ? "keep their samples" : "at the PS2's resolution");
		}
	} // namespace

	void Open(const uint8_t* regs)
	{
		s_regs = regs;
		// The interface's own opening, before any game, makes nothing: no game's settings are read yet.
		if (VMManager::HasValidVM())
			Start();
	}

	void GameStarted()
	{
		Start();
	}

	void DeviceGone()
	{
		// (The device frees its own textures; the pointer is only forgotten.)
		s_picture = nullptr;
		s_picture_w = s_picture_h = 0;
	}

	void Close()
	{
		if (!s_gs)
			return;
		s_active.store(false, std::memory_order_release);
		s_stop.store(true);
		s_gs->wake();
		s_gs->shutdown();
		s_gs.reset();
		if (s_picture && g_gs_device)
			g_gs_device->Recycle(s_picture);
		DeviceGone();
		s_state.clear();
		s_state.shrink_to_fit();
		s_opaque = {};
		ps5px_log("paraLLEl-GS: closed");
	}

	void Gif(uint32_t path, const uint8_t* data, uint32_t qwc)
	{
		if (path == 0)
			qwc = PacketQwords(data, qwc);
		if (qwc != 0)
			s_gs->gif(static_cast<uint8_t>(path), data, qwc * 16u);
	}

	namespace
	{
		// Crux2's display settings, as far as paraLLEl-GS scans out: the same switches PCSX2's own
		// presentation reads, handed to the renderer when one changes (each a frame late at most).
		// Deinterlacing "Automatic" asks for a progressive picture where the game's mode allows it, as
		// PCSX2 shows such games; any other choice leaves the fields as the PS2's own scan-out blended
		// them (paraLLEl-GS deinterlaces its own way: PCSX2's weave, bob, blend and adaptive shaders are
		// not its). What has no equivalent (dithering, texture filtering, the resolution, the hardware
		// fixes) is greyed out in Settings while paraLLEl-GS is on.
		void SendScanout()
		{
			// The samples and the picture at twice the size, changed in Settings while the game runs: taken at once
			// (automatic quality then counts the new samples as yours).
			const uint32_t samples = ReadSamples();
			const bool high_res = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSHighRes", true);
			if (s_scanout_any && (samples != s_samples_setting || high_res != s_high_res_setting))
			{
				ps5px_log("paraLLEl-GS: %u samples a pixel now%s (Settings)", samples, high_res && samples >= 4 ? ", the picture at twice the size" : "");
				s_samples_now = 0;
				SetSamples(samples);
			}
			s_samples_setting = samples;
			s_high_res_setting = high_res;
			GSGpuScanout want;
			want.forceProgressive = GSConfig.InterlaceMode == GSInterlaceMode::Automatic;
			want.antiBlur = GSConfig.PCRTCAntiBlur;
			want.overscan = GSConfig.PCRTCOverscan;
			want.crtcOffsets = GSConfig.PCRTCOffsets;
			want.mipmaps = GSConfig.HWMipmap;
			want.sharp2D = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSSharp2D", false);
			// Dithering (Settings > Graphics): Off (0) and Force 32 bit (3) draw without the PS2's dot pattern; Force 32 bit
			// also shows the 32-bit picture a game blits into a 16-bit frame buffer at the end (paraLLEl-GS's backbuffer
			// promotion). Scaled and Unscaled: as the PS2 dithers.
			want.dither = GSConfig.Dithering == 1 || GSConfig.Dithering == 2;
			want.backbufferPromotion = GSConfig.Dithering == 3;
			want.eagerSubmit = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSEarlySubmit", false); // (experimental: off unless chosen)
			// Hardware download mode "Unsynchronised": a read of GS memory the graphics card has not finished
			// drawing answers zeros instead of waiting for the card (paraLLEl-GS's unsynced read-backs; the read
			// still comes in order). "Off": the game's read is answered with zeros before it reaches here
			// (MTGS::InitAndReadFIFO), and paraLLEl-GS takes the unsynced path too, so the transfer nobody reads
			// costs it no wait for the card. The other modes wait and are right.
			want.unsyncedReadbacks = GSConfig.HWDownloadMode >= GSHardwareDownloadMode::Unsynchronized;
			if (s_scanout_any && std::memcmp(&want, &s_scanout_sent, sizeof(want)) == 0)
				return;
			s_gs->setScanout(want);
			s_scanout_sent = want;
			if (s_scanout_any)
				ps5px_log("paraLLEl-GS: scan-out now %s%s%s%s%s%s%s%s%s%s", want.forceProgressive ? "progressive where possible" : "the PS2's own fields",
					want.antiBlur ? ", anti-blur" : "", want.overscan ? ", overscan shown" : "", want.crtcOffsets ? ", screen offsets" : "",
					want.mipmaps ? "" : ", no mipmaps", want.sharp2D ? ", sharp 2-D" : "", want.unsyncedReadbacks ? ", unsynchronised read-backs" : "",
					want.dither ? "" : ", no dithering", want.backbufferPromotion ? ", 32-bit pictures promoted" : "",
					want.eagerSubmit ? "" : ", work handed over a frame at a time");
			s_scanout_any = true;
		}
	} // namespace

	void VSync(uint32_t field, bool registers_written)
	{
		SendScanout();
		uint64_t regs[GSGpuRenderer::kPrivRegCount];
		for (uint32_t i = 0; i < GSGpuRenderer::kPrivRegCount; i++)
			std::memcpy(&regs[i], s_regs + i * 16u, sizeof(uint64_t));
		// (The phase of the field is what paraLLEl-GS takes from the tick's lowest bit.)
		s_tick = (s_tick & ~1ull) + 2 + (field & 1u);
		s_gs->present(regs, s_tick, registers_written);
		s_gs->kick();
		// No more than two frames ahead of the pictures made: what is shown stays close to the controls.
		// (The wait is measured: it is where the game's time goes when the graphics card is the limit.)
		const u64 wait_start = Common::Timer::GetCurrentValue();
		s_gs->waitPresentBacklog(2, s_stop);
		s_wait_seconds += Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - wait_start);
		s_wait_frames++;
		if (s_wait_window.GetTimeSeconds() >= 1.0)
		{
			s_wait_ms = s_wait_frames ? static_cast<float>(s_wait_seconds * 1000.0 / s_wait_frames) : 0.0f;
			s_wait_seconds = 0.0;
			s_wait_frames = 0;
			s_wait_window.Reset();
			// The figures in the boot log now and then (a profiled run has them every window as well).
			if (++s_status_seconds % 30 == 15)
				ps5px_log("%s", StatusLine().c_str());
		}
	}

	Figures GetFigures()
	{
		Figures f;
		if (!s_gs)
			return f;
		const GSGpuStatus st = s_gs->status();
		f.gpu_ms = st.gpuMsPerFrame;
		f.gpu_setup_ms = st.gpuSetupMs;
		f.gpu_shading_ms = st.gpuShadingMs;
		f.gpu_upload_ms = st.gpuUploadMs;
		f.gpu_scanout_ms = st.gpuScanoutMs;
		f.thread_percent = st.busyPercent;
		f.wait_ms = s_wait_ms;
		f.readbacks = st.readbacksPerFrame;
		f.readback_wait_ms = st.readbackWaitMs;
		f.ring_wait_ms = st.ringWaitMs;
		f.presents = st.presentsPerSecond;
		f.samples = s_samples_now ? s_samples_now : st.superSampling;
		return f;
	}

	void SetSamples(uint32_t samples)
	{
		if (!s_gs || samples == s_samples_now)
			return;
		s_samples_now = samples;
		s_gs->setSuperSampling(samples);
		s_gs->setHighResolution(Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSHighRes", true) && samples >= 4);
	}

	namespace
	{
		// The picture from the buffer paraLLEl-GS read it back into, made opaque on its way into the
		// texture (the GS leaves alpha as the game drew it; what is shown is opaque): one pass, on this
		// thread, instead of a copy on paraLLEl-GS's thread and another here.
		void Upload(const GSGpuFrame& frame)
		{
			const uint32_t w = frame.width, h = frame.height;
			const auto* src = reinterpret_cast<const uint32_t*>(frame.pixels);
			auto opaque_row = [w](uint32_t* dst, const uint32_t* row) {
				for (uint32_t x = 0; x < w; x++)
					dst[x] = row[x] | 0xFF000000u;
			};
			GSTexture::GSMap map;
			if (s_picture->Map(map))
			{
				for (uint32_t y = 0; y < h; y++)
					opaque_row(reinterpret_cast<uint32_t*>(map.bits + static_cast<size_t>(y) * map.pitch), src + static_cast<size_t>(y) * w);
				s_picture->Unmap();
				return;
			}
			s_opaque.resize(static_cast<size_t>(w) * h);
			for (uint32_t y = 0; y < h; y++)
				opaque_row(s_opaque.data() + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * w);
			s_picture->Update(GSVector4i(0, 0, static_cast<int>(w), static_cast<int>(h)), s_opaque.data(), static_cast<int>(w * 4));
		}
	} // namespace

	GSTexture* Picture()
	{
		if (const GSGpuFrame* frame = s_gs->takeFrame(s_last_seq); frame && g_gs_device)
		{
			if (frame->width == 0 || frame->height == 0 || (!frame->pixels && frame->rgba.empty()))
				return nullptr; // nothing displayed: black
			if (!s_picture || s_picture_w != frame->width || s_picture_h != frame->height)
			{
				if (s_picture)
					g_gs_device->Recycle(s_picture);
				s_picture = g_gs_device->CreateTexture(static_cast<int>(frame->width), static_cast<int>(frame->height), 1,
					GSTexture::Format::Color);
				s_picture_w = frame->width;
				s_picture_h = frame->height;
				s_picture_scale = frame->modeHeight ? static_cast<float>(frame->height) / static_cast<float>(frame->modeHeight) : 1.0f;
				ps5px_log("paraLLEl-GS: the picture is %ux%u (the game's %ux%u%s)", frame->width, frame->height, frame->modeWidth,
					frame->modeHeight, frame->highResolution ? ", at twice the size" : "");
			}
			if (s_picture && frame->pixels)
				Upload(*frame);
			else if (s_picture)
				s_picture->Update(GSVector4i(0, 0, static_cast<int>(frame->width), static_cast<int>(frame->height)),
					frame->rgba.data(), static_cast<int>(frame->width * 4));
		}
		return s_picture;
	}

	void ReadFIFO(uint8_t* mem, uint32_t qwc)
	{
		const uint32_t bytes = qwc * 16u;
		const uint32_t got = s_gs->readTransfer(mem, bytes);
		if (got < bytes)
			std::memset(mem + got, 0, bytes - got);
	}

	void SoftReset(uint32_t mask)
	{
		s_gs->softReset(mask);
	}

	void Reset()
	{
		s_gs->reset();
	}

	int Freeze(freezeData* fd, bool size_only)
	{
		if (size_only || s_state.empty())
		{
			if (!s_gs->saveState(s_state))
			{
				ps5px_log("paraLLEl-GS: the GS's state could not be saved");
				return -1;
			}
		}
		if (size_only)
		{
			fd->size = static_cast<int>(s_state.size());
			return 0;
		}
		if (!fd->data || fd->size < static_cast<int>(s_state.size()))
			return -1;
		std::memcpy(fd->data, s_state.data(), s_state.size());
		s_state.clear();
		return 0;
	}

	int Defrost(const freezeData* fd)
	{
		s_state.clear();
		if (!fd->data || fd->size <= 0 || !s_gs->loadState(fd->data, static_cast<size_t>(fd->size)))
		{
			ps5px_log("paraLLEl-GS: the state's GS part is not paraLLEl-GS's (saved with PCSX2's renderer?)");
			Host::AddOSDMessage("This state was saved with PCSX2's renderer; paraLLEl-GS cannot load it.", 10.0f);
			return -1;
		}
		return 0;
	}

	float PictureScale()
	{
		return s_picture_scale;
	}

	std::string StatusLine()
	{
		if (!s_gs)
			return std::string();
		const GSGpuStatus st = s_gs->status();
		return fmt::format("paraLLEl-GS: its thread {:.0f}% busy (GS input {:.0f}%, flushes {:.0f}%, scanout {:.0f}%, read back {:.0f}%), "
						   "{:.0f} presents a second, {:.1f} MB/s of GS input, {:.0f} render passes a second, picture {}x{}; the graphics "
						   "card {:.1f} ms a frame (setup {:.1f}, shading {:.1f}, uploads {:.1f}, scan-out {:.1f}) at {} samples; the GS "
						   "thread waited {:.1f} ms a frame for it, {:.1f} ms a frame for room in its ring, and {:.2f} reads of GS memory "
						   "a frame cost {:.1f} ms a frame ({:.1f} of it the card finishing what the read needs; {:.1f} early submissions "
						   "a frame){}",
			st.busyPercent, st.gifPercent, st.flushPercent, st.vsyncPercent, st.readbackPercent, st.presentsPerSecond,
			st.gifMegabytesPerSecond, st.renderPassesPerSecond, st.pictureWidth, st.pictureHeight, st.gpuMsPerFrame, st.gpuSetupMs,
			st.gpuShadingMs, st.gpuUploadMs, st.gpuScanoutMs, s_samples_now ? s_samples_now : st.superSampling, s_wait_ms, st.ringWaitMs,
			st.readbacksPerFrame, st.readbackWaitMs, st.gpuWaitMs, st.eagerSubmitsPerFrame,
			st.readbackWaitMs > 2.0f ? " (the game reads its picture back: Hardware download mode decides whether that waits)" : "");
	}
} // namespace PS5PGS
