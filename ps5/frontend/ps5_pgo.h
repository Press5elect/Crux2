// Crux2 - the profiling build's counts (profile-guided optimisation).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A build made with PS5PX_PGO=generate (ps5/tools/build.sh) counts how often each branch and
// each function of the emulator runs, and which functions each indirect call reaches (LLVM's
// instrumentation). Those counts, from games played on the console, are what the next build
// (PS5PX_PGO=use) is compiled with: the compiler lays the hot paths out together, inlines the
// calls that are taken and turns the common target of an indirect call into a direct one.
//
// The counts live in the program's memory. The title never returns from main, so the runtime's
// own write at exit never happens: the counts so far are written here when a game closes and
// when Crux2 closes, to <data>/pgo/crux2-<when this run started>.profraw (each write replaces
// the run's file with the counts up to then). Settings > System > Log > copy takes them along
// with the logs. In any other build everything here does nothing.

#pragma once

#include <string>

namespace PS5Pgo
{
/// Whether this is a profiling build.
bool Active();
/// Names this run's file under <data_root>/pgo/ and makes the folder. Once, at start.
void Start(const std::string& data_root);
/// Writes the counts so far; `why` goes into the boot log line.
void Write(const char* why);
/// The folder the files go to ("" outside a profiling build).
std::string Folder();
} // namespace PS5Pgo
