// Crux2 - what the host (ps5_host.cpp) tells the interface (bigpicture/bp_pcsx2.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The interface is drawn by the graphics thread; the host's work goes on elsewhere (the emulation
// thread, the game list's scan). What the host has to say crosses here: these may be called from
// any thread, and the interface reads them when it draws.

#pragma once

#include <string>

namespace PS5UI
{
	/// What a game's start is waiting for ("Reading the disc image into memory"), and how far it
	/// is (0..1; less than 0 for no bar). An empty text takes it away.
	void SetLoadProgress(std::string text, float progress);

	/// What goes on in the background, shown quietly in the library; empty for nothing.
	void SetBackgroundNote(std::string note);

	/// Whether the game list is being scanned.
	void SetScanning(bool scanning);

	/// A controller came or went.
	void ControllerChanged(int difference);

	/// Cheat files were added to the cheats folder: an open game's Cheats page is made again.
	void CheatsChanged();

	/// What a page of the settings shows is another thing now (a backup was made, the list of
	/// backups was read, the patches came): the pages are made again.
	void PagesChanged();
	/// The saved states on the console are other ones (a backup was restored).
	void StatesChanged();
	/// Pictures of games came (pictures/ in the title folder): what was looked for there is looked
	/// for again.
	void PicturesChanged();
	/// A line that goes by itself, and a message to acknowledge, from any thread: shown on the
	/// next frame the interface draws.
	void Toast(std::string text);
	void Message(std::string title, std::string text);
} // namespace PS5UI
