// Crux2 - the startup video: Crux2's mark glows behind black glass, three blows crack it, the glass
// bulges and shatters, and the mark and the name are there (ps5/art/intro: its sources, and how it
// is made). It plays in the start-up animation's place when Crux2 starts (bigpicture/bp_splash.cpp;
// Settings > Interface > Startup video turns it off; any button skips it).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Made from OGBox's player (ps5/src/ogbox_intro.{h,cpp}, OGBox by Press5elect): VP9 in an IVF file
// (resources/intro/intro.ivf), decoded by libvpx on a thread of its own into RGBA frames, a few of
// them ahead before its clock starts; its sound (resources/intro/intro.ogg) through the interface's
// sound (ps5_ui_audio.h), in the start-up sound's place. The caller shows the frame for the time it
// asks (frame()), and the video goes on by the clock, not by the frames shown: a slow frame is
// skipped, not stretched. Without the files, or when they cannot be read, open() says no and the
// start is as it was.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace PS5Intro
{
/// Where the video and its sound are: the title folder's resources/intro (on the PC, ".").
std::string Folder();

class Player
{
public:
	Player();
	~Player();
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	/// Reads folder/intro.ivf (and folder/intro.ogg, its sound, when it is there), starts decoding
	/// ahead, and tells the interface's sound that the video's plays in the start-up sound's
	/// place. False (and a line in the boot log) when the video is missing or unreadable.
	bool open(const std::string& folder);
	/// Enough frames are decoded ahead for the clock to start: the caller then calls start().
	bool ready() const;
	/// The clock and the sound start.
	void start();
	bool started() const;
	/// The frame for the time since start(): its RGBA pixels (w x h x 4, valid until the next
	/// call), nullptr when none has been decoded yet. *serial changes when the frame does.
	const std::uint8_t* frame(std::uint32_t* w, std::uint32_t* h, std::uint64_t* serial);
	/// Seconds since start() (0 before it), and the video's length.
	double elapsed() const;
	double duration() const;
	/// Past its end, cut short, or its decoder gave up: true.
	bool finished() const;
	/// A button: the sound fades out (SkipStartup) and the video ends.
	void skip();
	/// Stops the decoder (also the destructor's). The sound is left to the interface's.
	void close();

private:
	struct Impl;
	std::unique_ptr<Impl> d;
};
} // namespace PS5Intro
