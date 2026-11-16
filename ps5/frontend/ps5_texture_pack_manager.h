// Crux2 - the HD texture pack manager's runtime: the places packs are on, the packs found there and
// measured, moves and deletes, the collection's list, and the downloads one after another.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The file work is ps5_texture_pack_store.cpp's (tested on a PC); this gives it the console's
// places (its textures folder, each USB drive, each share) and file calls (a share's through the
// virtual file layer), threads, the boot log's lines ("texture packs: ..."), and a queue for the
// downloader (ps5_texture_packs.h), which brings one pack at a time. The interface's side is
// bp_pcsx2.cpp (the TexturePackShelf) and bp_textures.cpp.
//
// Everything is asked for from the graphics thread and returns at once. What a thread found is
// kept under a lock and Generation() moves; what a move or a delete came to is handed to its
// `done` by Frame(), on the graphics thread.

#pragma once

#include "ps5_texture_pack_store.h"
#include "ps5_texture_packs.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5TexturePackManager
{
struct PlaceInfo
{
	PS5TexturePackStore::Place place;
	int kind = 0;           // 0 the console, 1 a USB drive, 2 a share
	bool downloads = true;  // a download can be unpacked there (the console, a drive the title reaches itself)
	int64_t freeBytes = -1; // not known on the console
};

/// Moves whenever anything below would answer otherwise.
unsigned Generation();
std::vector<PlaceInfo> Places();
std::vector<PS5TexturePackStore::Pack> Packs();

/// The places looked into again and the packs found there; `all`: every pack measured anew and
/// the collection's list fetched anew, else only the packs not measured before (and the list
/// when it was not fetched yet).
void LookAgain(bool all);
/// How many packs are waiting to be measured.
int Measuring();
/// The collection's list: 0 not asked for, 1 being fetched, 2 there, 3 not to be had.
int CollectionState();
std::string CollectionError();
std::vector<PS5TexturePacks::Pack> Collection();

/// A move to another place, or a delete, on a thread of its own: false when one runs already.
/// `done` gets whether it worked and a sentence, on the graphics thread (Frame).
bool Move(const PS5TexturePackStore::Pack& pack, size_t place, std::function<void(bool ok, std::string sentence)> done);
bool Delete(const PS5TexturePackStore::Pack& pack, std::function<void(bool ok, std::string sentence)> done);
bool JobBusy();
/// "Moving SLUS-21376 to USB drive 1: copying, 40%"; empty when none runs.
std::string JobLine();
void StopJob();
/// The move or delete, for the bar the downloads have (once a frame).
struct JobBar
{
	bool active = false;
	std::string title;
	uint64_t done = 0, total = 0;
	double speed = 0;
	double secondsLeft = -1;
};
JobBar JobNow();

/// A download put in the queue; it starts when the downloader is free (Frame).
void Queue(const PS5TexturePacks::Pack& pack, const PS5TexturePacks::Place& place);
std::vector<std::string> Queued();
void Unqueue(size_t index);
/// Whether a pack of the serial is being downloaded or waits to be.
bool Downloading(const std::string& serial);

/// Once a frame, on the graphics thread: what the threads came to handed over, and the next
/// download started with `start` when none runs (false from it: not started, said already).
void Frame(const std::function<bool(const PS5TexturePacks::Pack&, const PS5TexturePacks::Place&)>& start);
} // namespace PS5TexturePackManager
