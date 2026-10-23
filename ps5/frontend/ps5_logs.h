// Crux2 - the logs, for the person in front of the console: the end of a log as lines to read,
// whether the run before this one ended in a crash, and a copy of the logs on a drive or a share.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The title writes crux2-boot.log (this run; ps5_platform.cpp) and keeps the one before as
// crux2-boot.previous.log, with the crash report as its last lines when there was one
// (ps5_crash.cpp). PCSX2's own log is logs/emulog.txt. Nothing here knows the console: the title
// folder is a path, and the tests run on the PC.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace PS5Logs
{
/// The files, as the title folder has them.
struct Files
{
	std::string now;      // <title>/crux2-boot.log
	std::string previous; // <title>/crux2-boot.previous.log
	std::string emulator; // <data>/logs/emulog.txt
	std::vector<std::string> extra; // more files, copied under their own names (a profiling build's counts)
};

/// The last `most` lines of a file, oldest first, as they can be drawn: tabs made spaces, what is
/// not text dropped, a line cut at `width` characters with the rest on lines of its own (indented),
/// so nothing of a long line is lost. A file that is not there: no lines. Only the file's last
/// `bytes` are read, so a log of any size costs the same.
std::vector<std::string> Tail(const std::string& path, size_t most = 400, size_t width = 150, size_t bytes = 256 * 1024);

/// How the run before this one ended, from its log.
struct Ending
{
	bool crashed = false;   // it ended in a crash report, or stopped itself with a reason
	std::string what;       // "a memory fault (SIGSEGV)", "Crux2 stopped itself: <its reason>"
	std::string game;       // the game that was loaded then; empty when none was
	std::string build;      // the build that crashed ("v0.3.0, built ..." as its log says), may be empty
};
Ending PreviousRun(const std::string& previous_log);

/// The sentence(s) shown at start-up after a crash.
std::string Notice(const Ending& ending);

/// Copies the logs that exist to <place>/Crux2-logs/<moment>/. Returns how many files were
/// written; `message` gets a sentence for the user either way.
int CopyTo(const Files& files, const std::string& place, std::string* message);

} // namespace PS5Logs
