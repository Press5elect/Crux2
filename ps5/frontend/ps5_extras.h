// Crux2 - comforts of v0.5.0 that reach into the emulator: a memory card for each game, texture
// packs from a USB drive or a share, a state saved by itself while playing. (ps5_extras.cpp)
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Included by the emulator's own files too (VMManager.cpp, GSTextureReplacements.cpp), where a
// few lines under __PROSPERO__ call these: so nothing of the port is included here.

#pragma once

#include <memory>
#include <string>

class INISettingsInterface;

namespace PS5Extras
{
// ------------------------------------------------------------- a memory card for each game
//
// With "A memory card for each game" on ([PS5] CardPerGame), slot 1 holds a card of the game's
// own: "<serial>.ps2" in the memory cards folder, 8 MB, made the first time the game starts.
// Games that read each other's saves (as PCSX2's game database lists them) share one. It is named
// in the game's settings as the emulator holds them in memory, never in the game's settings file:
// switching the option off puts every game back on the card of the Memory cards page. A game
// whose own settings name a card for slot 1 keeps that one.

/// From VMManager::UpdateGameSettingsLayer, on the emulation thread, before the layer is taken.
/// `layer` may hold nothing (the game has no settings of its own): one is made then.
void CardForGame(std::unique_ptr<INISettingsInterface>* layer, const std::string& serial);

/// The file name of the card a game would get ("SLUS-12345.ps2"); empty for a game PCSX2's
/// database does not know. Whatever the option says. Any thread.
std::string CardNameFor(const std::string& serial);

// -------------------------------------------------------------------------- texture packs
//
// A game's replacement textures are looked for in the title folder first
// (textures/<serial>/replacements, as PCSX2 has it), then on every USB drive that is in and then
// on every share, in Crux2/textures/<serial>/replacements, PS2/textures/... and textures/... .
// The first of them that holds anything is the one used.

/// From GSTextureReplacements::ReloadReplacementMap (the graphics thread, when a game starts with
/// "Load replacement textures" on). `own` is the title folder's one, returned when there is no
/// pack anywhere else either.
std::string TexturePackFolder(const std::string& serial, const std::string& own);
/// The same for a pack kept as one ZIP file (PCSX2 reads those since v2.9.110): `own` when it is
/// there, else the first <place>/Crux2/textures/<serial>.zip on a drive or a share, else `own`.
std::string TexturePackZip(const std::string& serial, const std::string& own);

/// For the interface: where the game's pack is. `shares`: also ask the shares (that may take as
/// long as a network takes; without it only the title folder and the drives are looked at).
/// Empty when there is none.
std::string TexturePackPlace(const std::string& serial, bool shares);

// ------------------------------------------------------------- a state saved by itself
//
// With "Save a state by itself" set to a number of minutes ([PS5] AutoSaveMinutes), the running
// game's state is saved each time it was played that long, to a file of its own beside the slots
// ("<serial> (<CRC>).auto.p2s"). Not in RetroAchievements' hardcore mode, and never while a
// memory card is being written to.

/// Once a frame of the running game, on the emulation thread (Host::PumpMessagesOnCPUThread).
void FrameTick();
/// A game is being started: the time played counts from nothing.
void GameStarting();
/// Sleep-safe saving ([PS5] SleepSafe, on by default): the note a state saved while playing leaves
/// (inis/interrupted.txt: "game=", "serial=", "state=" lines), and its end when the game is closed
/// in the title.
std::string InterruptedNotePath();
void GameClosed();
} // namespace PS5Extras
