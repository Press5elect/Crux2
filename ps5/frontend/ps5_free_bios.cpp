// Crux2 - the free BIOS: OpenPS2BIOS, used only when no BIOS of your own is found.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// OpenPS2BIOS (Tamariba Devs, MIT, https://github.com/TamaribaEmu/OpenPS2BIOS) is an open-source
// replacement PlayStation 2 BIOS made for emulators, with no Sony code in it. The title carries
// its 0.2 release in resources/free-bios. PCSX2's search for a BIOS (ps2/BiosTools.cpp,
// FindBiosImage) asks for it only after the BIOS folder gave nothing, so a BIOS of your own is
// always used first. It is early work: the games it was tried with reach their menus, and many
// go no further.

#include "ps5_free_bios.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"

#include "common/FileSystem.h"
#include "common/Path.h"

std::string PS5FreeBiosPath()
{
	if (!Host::GetBaseBoolSettingValue("PS5", "FreeBios", true))
		return {};
	std::string path = Path::Combine(EmuFolders::Resources, PS5FreeBios::RELATIVE_PATH);
	if (!FileSystem::FileExists(path.c_str()))
		return {};
	return path;
}

bool PS5FreeBios::InUse(const std::string& bios_path)
{
	return !bios_path.empty() && Path::GetFileName(bios_path) == Path::GetFileName(PS5FreeBios::RELATIVE_PATH);
}
