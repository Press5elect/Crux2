// Crux2 - what the title fetches over the network when its user asks for it: cover pictures,
// title screens and pictures of play from libretro's collection, cheats from collections on
// GitHub, and PCSX2's own game patches.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Nothing of either is in the package: only lists of names (resources/ps5/boxarts.txt and
// cheat-index.txt, made by tools/make-online-lists.sh), in which a game is looked up before
// anything is asked of a server. All of it blocks, and is called from threads that may.

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class ProgressCallback;

namespace PS5Covers
{
/// The covers of the games that have none, from libretro's box arts, into the covers folder as
/// <serial>.png. A game for which nothing was found is remembered (covers/no-cover.txt) and not
/// asked for again until `again` says so. Stops when `progress` is cancelled. Returns how many
/// were saved; `summary` gets a sentence for the user.
int DownloadMissing(ProgressCallback* progress, bool again, std::string* summary);

/// A game's other pictures, from the same collection (its Named_Titles and Named_Snaps): its
/// title screen and a picture of it being played, into the folder "pictures" of the title folder
/// as <serial>.title.png and <serial>.snap.png. Of every PlayStation 2 game of the list that has
/// neither (`serial` empty), or of one game. A game nothing was found for is remembered
/// (pictures/no-picture.txt) and not asked for again until `again` says so. Stops when `progress`
/// is cancelled. Returns how many pictures were saved; `summary` gets a sentence for the user.
int DownloadPictures(ProgressCallback* progress, const std::string& serial, bool again, std::string* summary);
/// Where the pictures are kept, and a game's one: `which` 0 its title screen, 1 the picture of play.
std::string PicturesFolder();
std::string PicturePath(const std::string& serial, int which);
/// The disc in a drive (it is in no list): its box art, as covers/<serial>.png, and its two
/// pictures, those of the three that are not there yet. Returns how many were saved.
int FetchForDisc(const std::string& serial, const std::string& title, const std::string& title_en, const std::string& region);
} // namespace PS5Covers

namespace PS5Cheats
{
/// The cheat files of one game, from the collections of the list and the user's own addresses
/// (cheat-sources.txt in the title folder), into the cheats folder. Returns how many files were
/// saved; `summary` gets a sentence for the user.
int FetchForGame(const std::string& serial, u32 crc, std::string* summary);
/// The same for every game of the list that a collection has files for. `cancel` (may be null)
/// is looked at between games.
int FetchForAllGames(const bool* cancel, std::string* summary);
} // namespace PS5Cheats

namespace PS5Patches
{
/// PCSX2's collection of game patches (widescreen, no-interlacing, 60 fps and others): patches.zip
/// of the latest release of github.com/PCSX2/pcsx2_patches, the file PCSX2's own builds carry. It
/// is not in the package (the collection states no licence); the console fetches it for its user,
/// into the title folder's resources. Returns whether a new collection is in place; `summary` gets
/// a sentence for the user. The first time (and only then), widescreen patches are switched on for
/// every game that has one.
bool Download(std::string* summary);
/// What is there now: "Not downloaded", or how many games it knows and when it was fetched.
std::string Describe();
} // namespace PS5Patches
