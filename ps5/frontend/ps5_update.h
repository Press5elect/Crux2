// Crux2 - the updater: a newer release, downloaded and put in place from inside the title.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Adapted from PSSwanStation's updater (ps5/src/update.cpp), by the same author, whose way of
// working was proven on a console. What differs here: versions are semantic ("1.0.0", tagged
// "v1.0.0"), the notes of every release newer than the running one are gathered, the ZIP is read
// with libzip and the releases page with rapidjson, and every rename is made safe for exFAT.
//
// A release of Crux2 on GitHub carries the title folder as one ZIP, "Crux2-v1.0.0.zip", whose
// entries all sit below one folder named after the title ("PPSA99249/"), and SHA256SUMS beside it
// (ps5/tools/make-release-zip.py). A "<ZIP's name>.sha256" asset is read as well, and GitHub's own
// digest of the asset when it gives one.
//
// The three steps, each asked for by the interface:
//
//   Check()     asks GitHub for the repository's releases (one request, no personal data), finds
//               the newest one and the notes of every release newer than this one.
//   Download()  fetches the ZIP into <title folder>/.update/, checks its size and checksum, looks
//               at every entry before anything is unpacked, and unpacks the program's own files
//               (IsManaged) into .update/stage/.
//   Install()   renames each old file into .update/backup/ and the new one into its place, with a
//               journal, then the title must close.
//
// Recover() at the next start puts the old files back after an install that a power cut stopped.
// The user's files (settings, memory cards, states, BIOS, games, covers, network.cfg...) are never
// in the set that is replaced, and a file the release does not have is left where it is.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PS5Update
{

enum class State
{
	Idle,
	Checking,
	UpToDate,    // the newest release is this version, or older (`reinstall` says whether it can be put in again)
	Available,   // a newer one: version, name, notes
	NoRelease,   // the repository has no usable release; `error` says why in a sentence
	Downloading, // `done` of `total` bytes
	Verifying,   // the checksum, the ZIP, unpacking
	Ready,       // unpacked beside the title: Install() puts it in place
	Installed,   // in place: the title must close now
	Failed,      // `error` says why; nothing in the title folder was changed
};

/// One release newer than the running version, for "What's new".
struct ReleaseNotes
{
	std::string version; // "1.1.0"
	std::string tag;     // "v1.1.0"
	std::string name;    // the release's title, one line
	std::string date;    // "2026-10-11", from published_at; empty when GitHub gave none
	std::string text;    // its notes, as plain text
	bool prerelease = false;
};

struct Status
{
	State state = State::Idle;
	std::string version;  // the newest release's version, "1.1.0"
	std::string tag;      // "v1.1.0"
	std::string name;     // its title, one line
	std::string notes;    // every newer release's notes, newest first, as one plain text
	std::vector<ReleaseNotes> newer; // the same, one by one, newest first
	std::string error;    // Failed, NoRelease: a sentence for the user
	uint64_t done = 0, total = 0;
	double speed = 0;     // bytes a second while downloading
	bool reinstall = false; // UpToDate: Reinstall() can put the newest release in place again
};

/// One HTTP(S) GET into memory. Returns the HTTP status code, or less than 0 when there was no
/// answer. `seconds`: how long to wait at most.
using HttpGet = std::function<int(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)>;
/// One HTTP(S) GET into a file (created or truncated), no larger than `limit` bytes, telling how
/// far it is; `progress` returning false stops it (it then returns less than 0). Returns the
/// status code or less than 0.
using HttpDownload = std::function<int(const std::string& url, const std::string& path, uint64_t limit,
	const std::function<bool(uint64_t done, uint64_t total)>& progress)>;
/// One line for the boot log ("update: ...").
using Log = std::function<void(const char* line)>;
/// The bytes free on the drive that holds `folder`; less than 0 when unknown.
using FreeSpace = std::function<int64_t(const std::string& folder)>;

struct Setup
{
	std::string app_dir;                       // the title's folder (a trailing '/' is added)
	std::string version;                       // the running version, "1.0.0" or "v1.0.0"
	std::string title_id = "PPSA99249";        // the ZIP's top folder, and param.json's titleId
	std::string repository = "Press5elect/Crux2"; // "owner/repository" on GitHub
	std::string asset_prefix = "Crux2-";       // the ZIP is "<prefix>v1.2.3.zip"
	std::string api_base = "https://api.github.com";
	HttpGet http_get;
	HttpDownload http_download;
	Log log;              // may be empty
	FreeSpace free_space; // may be empty: statvfs() is asked
};

/// Once at start-up, before anything else reads the title's files: an install that a power cut
/// interrupted is undone, and what an earlier update left in .update/ is removed. True when an
/// interrupted install was found (and undone, as far as it could be).
bool Recover(const std::string& app_dir, const Log& log = {});
/// Takes the set-up. Stops a check or a download of an earlier one first.
void Init(const Setup& setup);
/// Asks the releases page, on a thread of its own. Does nothing while a check, a download or an
/// install is going on, or while an unpacked update waits (Ready).
void Check();
/// Downloads, checks and unpacks the release Check() found (state Available), on a thread of its
/// own.
void Download();
/// Up to date (and `reinstall`): downloads the newest release all the same, to put it in place
/// again ("Install again").
void Reinstall();
/// Stops a download or the checks after it; the state goes back to Available.
void Cancel();
/// Puts the new files in place (state Ready). True: done, close the title. False: the old files
/// are back and the status's error says why.
bool Install();
/// A copy of the status, taken under a lock.
Status GetStatus();

// ----------------------------------------------------------------------------------- helpers

struct Version
{
	int major = 0, minor = 0, patch = 0;
	bool valid = false;
};

/// "1.2.3", "v1.2.3", "V1.2.3-test", "1.2" (patch 0): white space and one leading 'v' are
/// skipped, whatever follows the numbers is ignored. Not valid when it does not start so.
Version ParseVersion(const std::string& text);
/// Less than 0, 0 or more than 0 as `a` is older than, the same as or newer than `b`. An
/// invalid version is older than any valid one.
int CompareVersions(const Version& a, const Version& b);
/// The version in a tag or a file name: the first "[v]N.N[.N]" in it ("Crux2-v1.0.0.zip",
/// "Crux2-v1.2.3-part1of3.zip", "v0.9.0"). Not valid when there is none.
Version VersionIn(const std::string& text);
/// "1.2.3".
std::string ToString(const Version& version);

/// Whether a path below the title folder ("eboot.bin", "resources/fonts/x.ttf") is one of the
/// program's own files, which an update may replace. Everything else is the user's.
bool IsManaged(const std::string& path);
/// A release's Markdown as plain text for the screen.
std::string PlainText(const std::string& markdown);
/// The SHA-256 of some bytes, as 64 lower-case hex digits.
std::string Sha256Hex(const void* data, size_t size);

#if defined(CRUX2_UPDATE_TEST)
/// The PC test's switches (ps5/tests/update); not in the title's build.
namespace Test
{
extern int stop_after;      // Install(): stop as a power cut would, after this many renames
extern int undo_stop_after; // the undo: the same
extern int fail_at;         // Install(): this rename fails
extern int lie_at;          // Install(): this rename reports success and does nothing (exFAT's way)
extern uint64_t max_unpacked; // 0: the real limit
extern size_t max_entries;    // 0: the real limit
} // namespace Test
#endif

} // namespace PS5Update
