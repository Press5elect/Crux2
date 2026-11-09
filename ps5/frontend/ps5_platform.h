// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the console side of the port: where the title's folder is, the boot log,
// leaving the sandbox for USB drives, network shares, and how the title ends.

#pragma once

#ifdef __cplusplus
#include <string>
#include <vector>

extern "C" {
#endif

/// The title's folder: /app0 inside the sandbox; once the sandbox is left for USB drives, the
/// folder's place in the console's file system (/data/homebrew/<TITLE_ID>). No trailing slash.
const char* ps5px_title_dir(void);

/// The certificate bundle HTTPS downloads are verified with (<title folder>/resources/cacert.pem).
const char* ps5px_ca_bundle(void);

/// One line to the boot log in the title folder and to klog.
void ps5px_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// When this build was made ("2026-10-02 18:45 UTC").
const char* ps5px_build_stamp(void);

/// What this build was made from: one digest over every input that decides the binary, then the
/// revisions behind it (tools/build.sh). The boot log's second line; build/ps5/build-identity.txt
/// has the same string for the ELF a crash of this build is symbolised against.
const char* ps5px_build_identity(void);

/// Cheats from the collections on GitHub, for one game or for every game of the list: started on
/// a thread of their own; what came of it is said in a toast. (ps5_host.cpp)
void ps5px_download_cheats(const char* serial, unsigned crc);
void ps5px_download_all_cheats(void);

/// The interface's sound takes its switches and volumes from the settings ([PS5] StartupSound,
/// StartupVolume, BackgroundSound, BackgroundVolume). Cheap; from any thread. (ps5_host.cpp)
void ps5px_apply_ui_sound_settings(void);

/// The version this build was made as ("v0.2.1": PS5PX_VERSION given to tools/build.sh), or an
/// empty string for a build that was given none.
const char* ps5px_build_version(void);

/// The build number shown in the top bar (the repository's commit count when it was built).
const char* ps5px_build_number(void);

/// How many 256-frame writes the audio thread has made to the console's output since the title
/// started, and how many of them found less sound buffered than a write holds (and so carried
/// silence): a test run's measure of audio continuity.
void ps5px_audio_counters(unsigned long long* writes, unsigned long long* short_writes);

/// A test run's profiler (ps5_profiler.cpp): samples the graphics thread (and with all_threads
/// the emulation and VU threads) and writes where their time goes to the boot log. Called before
/// the emulator's threads exist; they join as they name themselves.
void ps5px_profile_start(int all_threads);
void ps5px_profile_thread_named(const char* name);

/// The boot log's descriptor (-1: none), for the crash report.
int ps5px_log_fd(void);

/// Gives the calling thread a stack of its own for the crash report, so that a stack overflow can
/// be reported too (ps5_crash.cpp). The emulator's threads do this as they start.
void ps5px_install_crash_stack(void);

/// A notification on the console's screen.
void ps5px_notify(const char* message);

/// Records the message, shows it, and has the shell close the title. A title never calls abort()
/// or exit(): the console reports those as a crash.
__attribute__((noreturn)) void ps5_title_fatal(const char* message);

#ifdef __cplusplus
}

namespace PS5Platform {

/// First thing in main(), while the process has one thread: finds the title folder, leaves the
/// sandbox when USB drives are wanted, then starts klog capture and the boot log.
void EarlyInit();

/// Whether a file of this name is in the title folder: a switch for when the interface cannot be
/// reached (usb-off.txt, late-vulkan.txt).
bool HasFlagFile(const char* name);

/// Compares the title folder's resources/ with what the build shipped, and writes what is missing
/// or of another size to the boot log. Returns the number of missing files; the first one's name
/// (relative to resources/) is put in first_missing.
unsigned CheckInstall(std::string* first_missing);

/// Whether the title left its sandbox (USB drives are readable).
bool IsElevated();

/// Whether USB drives are asked for ([PS5] USBDrives in inis/PCSX2.ini, on by default; a file named
/// usb-off.txt in the title folder turns them off too).
bool WantsUSB();

/// Writes the network.cfg template when there is none, and mounts the shares it names under /smb.
void MountShares();

/// The folders games are looked for in without being asked: <title folder>/games (and games in
/// the outside folder, when that is used and has one), each network share, and the PlayStation
/// folders at the top of each USB drive.
std::vector<std::string> FindGameFolders();

/// Crux2's files outside the title folder. When a folder /data/crux2 is there and can be written
/// to, the emulator keeps everything in it that it would keep in the title folder: the settings,
/// the memory cards, the saved states, the BIOS folder, covers, cheats, caches (ps5_host.cpp makes
/// it the data folder). The title folder can then be replaced by a new version without them.
/// Nothing is moved: whoever makes the folder copies into it what is to be kept.
/// The folder, or an empty text when it is not used; decided once, the first time it is asked.
const std::string& OutsideFolder();
/// A file or folder of the user's that is looked for by its name (network.cfg, cheat-sources.txt,
/// backgrounds): in the outside folder when that is used and has it, else in the title folder.
std::string UserPath(const char* name);

/// Whether the console's own menu is over the title (the PS button was pressed): 1 it is, 0 it is
/// not, -1 the console did not say. Asked of the system service; cheap enough for twice a second.
int SystemMenuOver();

/// Writes a whole file beside its place and puts it there, safely on an exFAT drive too.
bool WriteFileSafely(const char* path, const void* data, size_t size);

/// Makes a file or folder of the title readable and writable for an FTP client.
void MakePublic(const char* path, bool directory);

/// The same for everything the title keeps in its folder (settings, memory cards, save states,
/// caches, logs): not what the build shipped, and not the games. On a thread of its own, since a
/// library's covers are thousands of files; a file that is open already is left as it is.
void MakeTitleFilesPublicInBackground();
/// At the end of a run: stops that walk if it still goes, then opens up what a run writes
/// (settings, memory cards, saved states, logs), which is a few folders and quick.
void MakeSessionFilesPublic();

} // namespace PS5Platform
#endif
