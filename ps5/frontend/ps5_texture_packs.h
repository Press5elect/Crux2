// Crux2 - HD texture packs from the Internet Archive, downloaded and unpacked on the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The packs are other people's work, in the collection archive.org/details/pcsx2-hd-texture-packs
// (one RAR archive a game). Nothing of them comes with Crux2: a pack is fetched only when you ask
// for it, for a game of yours, from that collection. The list of what it holds (its metadata) is
// asked for once a day at most and kept in cache/texture-packs.json.
//
// A pack is unpacked as it downloads (ps5_texture_packs_extract.h), into the textures folder of
// the place you choose: the console (textures/ in the title folder) or a USB drive
// (<drive>/Crux2/textures), where PS5Extras finds packs already. When it is whole, the game's own
// "Load texture packs" is switched on, and a game that runs gets its textures at once.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5TexturePacks
{
inline constexpr const char* COLLECTION = "pcsx2-hd-texture-packs";

struct Pack
{
	std::string file;  // the archive's name in the collection
	std::string title; // the name without ".rar"
	uint64_t size = 0; // of the archive
	std::vector<std::string> serials;
};

/// A textures folder a pack can be unpacked into.
struct Place
{
	std::string root;  // ".../textures"
	std::string name;  // "This console", "USB drive 1"
	// (No free space: no call a title can make reports a drive's. A pack that does not fit stops
	// with the drive full, and what was unpacked whole stays.)
};
std::vector<Place> Places();

/// The packs of a game, from the collection's list (fetched when the copy is older than a day,
/// or `again`). On a thread of its own; `then` is called on the graphics thread, with an empty
/// list and a reason when the list could not be had.
void FindFor(const std::string& serial, bool again, std::function<void(std::vector<Pack>, std::string error)> then);

/// Downloads and unpacks a pack into a place, on a thread of its own; one at a time. `done` is
/// called on the graphics thread when it ends, with whether it is whole and a sentence. False
/// when one runs already.
bool Start(const Pack& pack, const Place& place, std::function<void(bool ok, std::string sentence)> done);
/// Stops the one that runs (what was unpacked whole stays; a file half written does not).
void Stop();
bool Busy();
/// "Black (USA) [SLUS-21376] HD Remaster: 1.2 of 1.8 GB (66%)"; empty when none runs.
std::string Doing();
} // namespace PS5TexturePacks
