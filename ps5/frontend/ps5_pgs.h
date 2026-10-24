// Crux2 - paraLLEl-GS as a second GS renderer.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// paraLLEl-GS (Arntzen Software, LGPL-3.0-or-later) draws the whole GS on the graphics card in compute
// shaders: no texture cache, no draw-by-draw work on the processor, no barriers and no ordered view. It
// runs on a thread of its own with its own Vulkan device (def-jam-vendetta-recomp's renderer for it,
// jcsix694, GPL-3.0, as Crux2 Recomp uses it): the GS thread hands it the GS's input as it comes and
// takes its pictures back, which go on through Crux2's own presentation (FSR, frame generation, the
// on-screen display) as PCSX2's would.
//
// It is chosen in Settings or per game ([EmuCore/GS] PS5ParallelGS) and takes effect when a game starts. Its picture
// is the PS2's own resolution, made sharper by super-sampling ([EmuCore/GS] PS5ParallelGSSamples, 8 by
// default) and scanned out at twice the size from the samples ([EmuCore/GS] PS5ParallelGSHighRes);
// PCSX2's upscaling setting does not apply to it. Its save states are its own: a state saved with one
// GS renderer is not loaded by the other.
//
// Everything here runs on the GS thread.

#pragma once

#include <cstdint>
#include <string>

class GSTexture;
struct freezeData;

namespace PS5PGS
{
	/// Whether the game is drawn by paraLLEl-GS.
	bool Active();

	/// The GS opened (GSopen): `regs` is the GS's privileged registers in the PS2's memory. Crux2's
	/// interface opens the GS once, before any game, and keeps it open; paraLLEl-GS is made here only
	/// when the GS is opened while a game runs (the device made again), else at GameStarted().
	void Open(const uint8_t* regs);
	/// A game started (Host::OnVMStarted, on the GS thread, before a state is loaded): paraLLEl-GS is
	/// made when the game's own settings, or Settings, ask for it. The boot log says either way.
	void GameStarted();
	/// The GS closed (GSclose), or the game ended (Host::OnVMDestroyed): paraLLEl-GS with it.
	void Close();
	/// The GS device is about to be destroyed: the picture's texture goes first.
	void DeviceGone();

	/// The GS's input: `qwc` quadwords of GIF path `path` (0-3 as PCSX2 numbers them). For path 1
	/// (from VU1 memory) `qwc` is how much is left of the memory: only the packet up to its end (EOP)
	/// is handed on.
	void Gif(uint32_t path, const uint8_t* data, uint32_t qwc);
	/// A vsync: the frame is handed to paraLLEl-GS to scan out.
	void VSync(uint32_t field, bool registers_written);
	/// The newest picture paraLLEl-GS made, in a texture of the GS device; null before the first.
	GSTexture* Picture();
	/// How many of the picture's pixels make one of the game's (2 at twice the size): for cropping,
	/// which is set in the game's pixels.
	float PictureScale();

	/// GS memory read back to the PS2 (a local-to-host transfer): `qwc` quadwords.
	void ReadFIFO(uint8_t* mem, uint32_t qwc);
	/// The GIF paths in `mask` start over (bit 0: path 1).
	void SoftReset(uint32_t mask);
	/// The GS reset (a hardware reset): the drawing registers too.
	void Reset();

	/// Save states: PCSX2's GSfreeze's contract (0 when done). A state of the other renderer is refused.
	int Freeze(freezeData* fd, bool size_only);
	int Defrost(const freezeData* fd);

	/// One line for the test windows of the boot log ("" when not active).
	std::string StatusLine();

	/// The figures of the last second: for the on-screen display and automatic quality.
	struct Figures
	{
		float gpu_ms = 0.0f;       // the graphics card's time a frame, from its timestamps (0: none yet)
		float gpu_setup_ms = 0.0f, gpu_shading_ms = 0.0f, gpu_upload_ms = 0.0f, gpu_scanout_ms = 0.0f;
		float thread_percent = 0.0f; // paraLLEl-GS's own thread, busy
		float wait_ms = 0.0f;      // what the GS thread waited a frame for the graphics card to catch up
		float readbacks = 0.0f;    // reads of GS memory by the game a frame (its picture read back)
		float readback_wait_ms = 0.0f; // what those reads cost the GS thread a frame (everything queued drained first)
		float ring_wait_ms = 0.0f; // what the GS thread waited a frame for room (paraLLEl-GS's thread behind)
		float presents = 0.0f;     // presents a second
		uint32_t samples = 0;      // samples a pixel now
	};
	Figures GetFigures();
	/// Automatic quality: the samples a pixel while the game runs (1, 2, 4, 8, 16); the picture at twice
	/// the size needs 4 or more. The setting is not changed.
	void SetSamples(uint32_t samples);
} // namespace PS5PGS
