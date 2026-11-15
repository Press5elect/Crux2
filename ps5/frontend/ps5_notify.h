// Crux2 - the console's own notifications: plain ones, and rich ones that look and sound like a
// trophy (RetroAchievements' unlocks).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 1679c95 (src/porpoise_notify.{hpp,cpp}, GPL-3.0-or-later), after PS5SX2's
// ProsperoNotify.cpp (GPL-3.0-or-later). Callers only queue: a thread of its own sends, one at a
// time, spaced so the system shows each picture. A rich notification is posted through
// libSceNotification when the title may load it; else a copy of the small relay payload
// (notify-relay.elf in the title folder, ps5/elevation/notify-relay) carries it to the payload
// loader on 127.0.0.1:9021; without either, its text goes out as a plain notification.

#pragma once

#include <string>

namespace PS5Notify
{
enum class Sound
{
	Default,  // the system's usual notification sound
	Trophy,   // the trophy sound ("psfx_trophy_toast")
	Platinum, // the platinum trophy sound ("psfx_platinum_trophy_toast")
	Silent,
};

/// Two lines, `message` and `sub`. `icon`: an https address, or a PNG under /data (empty for
/// none; anything else is dropped). `trophy`: in the Trophies channel, so that it follows the
/// player's trophy notification settings, as a real trophy does.
void Rich(const std::string& message, const std::string& sub, const std::string& icon, Sound sound, bool trophy = false);
/// Text only (ps5px_notify), queued with the rest so it keeps its place.
void Plain(const std::string& text);
/// The notifications queued from now on wait `seconds` first: the system drops the picture of one
/// sent in a game's first seconds.
void Hold(int seconds);
/// Waits up to `max_ms` for the queue to be sent (before the title closes).
void Flush(int max_ms);
/// Whether rich notifications reach the screen: -1 not tried yet, 1 yes, 0 no (only plain ones).
int RichState();

// ---- RetroAchievements (pcsx2/Achievements.cpp calls these on the emulation thread)

/// Whether [Achievements] ConsoleNotifications is on (Settings > Achievements, "As the console's
/// trophy notifications"; on unless switched off).
bool ConsoleNotificationsOn();
/// An achievement unlocked: a trophy notification with the trophy sound and the badge (its
/// https address). True when it was queued as the console's, so that PCSX2 does not draw its own
/// over the game as well; false when the setting is off or the console showed none before.
bool AchievementUnlocked(const std::string& title, const std::string& description, const std::string& badge_url);
/// Every achievement of the game unlocked: the platinum trophy's notification. The same answer.
bool AchievementsMastered(const std::string& title, const std::string& text, const std::string& icon_url);
/// A game's achievements were loaded (its first seconds): the notifications wait a few seconds.
void AchievementsGameLoaded();

/// The JSON of a rich notification (for the tests): `created` "2026-11-15T10:00:00.000Z", `id` a
/// number as text.
std::string Payload(const std::string& message, const std::string& sub, const std::string& icon, Sound sound, bool trophy,
	const char* created, const char* id);
/// A copy of the relay payload with `json` written into its room (for the tests): empty when the
/// image has no room, or the JSON does not fit.
std::string RelayImage(const std::string& relay, const std::string& json);
} // namespace PS5Notify
