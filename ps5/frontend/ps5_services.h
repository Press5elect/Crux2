// Crux2 - the title's services that are wired to PCSX2's own pieces: the updater over PCSX2's HTTP
// downloader.
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace PS5Services
{
	/// Gives the updater (ps5_update.h) the title folder, this build's version, the release
	/// repository ([PS5] UpdateRepository, "Press5elect/Crux2" unless set) and its HTTP. Nothing is
	/// asked of GitHub until the user does.
	void StartUpdater();
} // namespace PS5Services
