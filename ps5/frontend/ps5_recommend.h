// Crux2 - recommended settings per game, tested on a PS5, downloaded from the repository.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 35e1dab and 1880404 (src/ui_recommend.hpp, data/recommended.ini; GPL-3.0-or-later):
// the repository's ps5/recommended.ini (its format is told at its top) is fetched from
// raw.githubusercontent.com/<[PS5] UpdateRepository>/main/ps5/recommended.ini at most once a day
// and kept in the data folder as recommended.ini; the time of the last try is in
// recommended.checked beside it. A game that has an entry shows "Recommended settings" at the top
// of its own options, with what each changes, and one press writes them as the game's own
// settings (bigpicture/bp_pcsx2.cpp). Only PCSX2's emulation settings are taken from the file.
//
// The download and the safe write are game fixes' (ps5_fixes_fetch.h). Built for the PC by
// ps5/tests/run-host-tests.sh with CRUX2_RECOMMEND_TEST, without the parts that start it.

#pragma once

#include "ps5_fixes_fetch.h"

#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace PS5Recommend
{
/// The file's first line.
inline constexpr const char* HEADER = "# Crux2 recommended settings 1";
/// How long a download is kept before it is asked for again.
inline constexpr time_t KEPT_SECONDS = 24 * 60 * 60;

struct Value
{
	std::string section, key, value; // "EmuCore/GS", "upscale_multiplier", "3"
};

struct Pick
{
	std::string note;          // why, from the file
	std::vector<Value> values; // in the file's order
};

/// The games of a file, by serial (upper case). False when it is not such a file (its first line
/// is not HEADER). Lines that are not a setting Crux2 takes are left out, counted in `refused`.
bool Parse(const std::string& text, std::map<std::string, Pick>* picks, unsigned* refused = nullptr);

/// Whether a setting may come from the file: PCSX2's emulation settings only (the sections
/// EmuCore, EmuCore/<...> such as EmuCore/CPU/Recompiler, and Framerate), with keys and section parts of
/// letters, digits and '_'.
bool SafeSetting(const std::string& section, const std::string& key);

/// Asks for the file again when the last try was KEPT_SECONDS ago or more (or never was, or its time
/// is ahead of the clock): the download is checked (HEADER, size) and written over `cache`, safely.
/// `checked` keeps the time of the try, whatever came of it. True when `cache` was changed.
bool Refresh(const std::string& url, const std::string& cache, const std::string& checked, time_t now,
	const PS5Fixes::Getter& get, std::string* why = nullptr);

/// A game's entry in `cache` (read again whenever the file changed). False when it has none.
bool For(const std::string& cache, const std::string& serial, Pick* pick);

/// The console's: Refresh() on a thread of its own once a run, when [PS5] FixesAtStart (Settings >
/// PlayStation 5, "Game fixes from GitHub at start") is on; and the game's entry from the data
/// folder's copy.
void Start();
bool ForGame(const std::string& serial, Pick* pick);
} // namespace PS5Recommend
