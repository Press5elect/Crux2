// Crux2 - HD texture packs: an archive unpacked as it comes, into the textures folder.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// libarchive (BSD-2-Clause) reads RAR, RAR5, ZIP and 7z from a stream: the bytes are handed over
// as the download brings them, so a pack of 28 GB needs 28 GB of room, not 56. Nothing of PCSX2
// or the console here: ps5/tests/texturepacks runs it on a PC with libarchive's own test archives.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5TexturePacksExtract
{
/// The next bytes of the archive: up to `size` into `buffer`; 0 at its end; -1 when it failed or
/// was stopped.
using Read = std::function<long(void* buffer, size_t size)>;

struct Result
{
	bool ok = false;
	int files = 0;
	uint64_t bytes = 0;  // written
	int skipped = 0;     // entries not written (folders, unsafe paths)
	std::string error;   // why it stopped, when it did
};

/// Unpacks everything of the archive `read` gives into `root` (the textures folder), each file
/// where PS5TexturePacksLogic::EntryTarget puts it. A file is written beside its place and renamed
/// when whole, so a stopped unpack leaves no half file under its name. `stop` is asked between
/// files and between blocks.
Result Extract(const Read& read, const std::string& root, const std::vector<std::string>& serials, const std::function<bool()>& stop);

/// The folders of a path, made if missing ("a/b/c" makes a, a/b and a/b/c).
bool MakeFolders(const std::string& path);
} // namespace PS5TexturePacksExtract
