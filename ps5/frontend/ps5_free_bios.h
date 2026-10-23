// Crux2 - the free BIOS: OpenPS2BIOS, used only when no BIOS of your own is found.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include <string>

/// The free BIOS's full path when "[PS5] FreeBios" is on (the default) and the file is there;
/// empty otherwise. PCSX2's BIOS search asks for it when the BIOS folder holds none.
std::string PS5FreeBiosPath();

namespace PS5FreeBios
{
	/// Where it is, under the title's resources/.
	inline constexpr const char* RELATIVE_PATH = "free-bios/OpenPS2BIOS-0.2.bin";

	/// The BIOS a game was started with is the free one.
	bool InUse(const std::string& bios_path);
} // namespace PS5FreeBios
