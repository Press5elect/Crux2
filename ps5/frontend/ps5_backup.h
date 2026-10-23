// Crux2 - backups of the memory cards and the saved states, on a network share or a USB drive.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A backup is a folder of its own, named by the moment it was made:
//
//     <place>/Crux2-backup/2026-10-06_14-32-07/
//         memcards/...     every file of the memory cards folder (a folder card: its tree)
//         sstates/...      every saved state (not the ".backup" copies PCSX2 keeps of a slot)
//         backup.txt       written last: a backup without it was cut short, and is not restored
//
// Automatic ones (made when a game is closed) hold the memory cards only, end in "_auto", and
// only the newest few are kept. Nothing is ever taken away from the console: restoring copies a
// backup's files over the ones of the same name, and what they replace is moved to a folder
// beside them first.
//
// A place is a folder: under a mount of the virtual file layer (/smb/<share>, written through
// frontend/vfs) or a real one (a USB drive, once the title has left its sandbox). Everything here
// blocks, for as long as a network takes; StartBackup, StartRestore and StartList run it on a
// thread of their own, one job at a time.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace PS5Backup
{
/// The folder of a place that holds the backups.
inline constexpr const char* FOLDER = "Crux2-backup";
/// The folder the builds before v0.6.0 kept them in (the title was named PSPCSX2 then): what is in
/// it is still listed and can be restored; new backups go to FOLDER.
inline constexpr const char* FOLDER_BEFORE = "PSPCSX2-backup";
/// How many automatic backups are kept.
inline constexpr int AUTOMATIC_KEPT = 10;

/// Where the console keeps what is backed up, and where a restore puts what it replaces.
struct Folders
{
	std::string memcards;
	std::string sstates;
	std::string replaced; // "<title folder>/backup-before-restore"
};

struct Snapshot
{
	std::string name;     // "2026-10-06_14-32-07" or "2026-10-06_14-32-07_auto"
	std::string path;     // its folder
	time_t when = 0;      // from its name, as local time
	bool automatic = false;
	bool complete = false; // backup.txt is there
	int card_files = 0;
	int states = 0;
	uint64_t bytes = 0;
};

struct Result
{
	bool ok = false;
	int files = 0;
	uint64_t bytes = 0;
	std::string snapshot;  // the backup's name
	std::string message;   // a sentence for the user, whatever came of it
};

/// A job's steps as they go: `done` of `total` files, and whether to stop before the next.
struct Watch
{
	std::function<void(int done, int total)> step;
	std::function<bool()> stop;
};

/// "Each backup replaces the one before it": with this on, a backup that was made whole takes
/// away the older backups of its kind at the place: one made by hand those made by hand, an
/// automatic one the automatic ones (of which the newest AUTOMATIC_KEPT are kept otherwise). So a
/// place holds two backups at most, and does not grow. Off unless it is switched on.
void SetReplaceOlder(bool on);

/// Makes a backup at `place`. `states`: the saved states too. `automatic`: the cards only, named
/// "_auto", and the automatic backups beyond the newest AUTOMATIC_KEPT are taken away afterwards.
/// `version` is written into backup.txt ("v0.4.0").
Result Backup(const Folders& folders, const std::string& place, bool states, bool automatic, const std::string& version,
	const Watch& watch = {}, time_t when = 0 /* the moment it is named by; 0: now */);

/// The backups of a place, the newest first. False when the place cannot be listed (`error` gets
/// a sentence); a place with no backup folder yet is an empty list.
bool List(const std::string& place, std::vector<Snapshot>* snapshots, std::string* error);

/// Copies a backup's files back. Files the console has that the backup does not are left alone.
Result Restore(const Folders& folders, const Snapshot& snapshot, const Watch& watch = {});

/// The memory cards as they are now, in a few letters: another text when a file came, went, grew
/// or was written to. An automatic backup is made only when it differs from the last one's.
std::string CardsSignature(const std::string& memcards);

/// "2026-10-06_14-32-07" for a moment (local time), and back; 0 when the name is none.
std::string NameFor(time_t when, bool automatic);
time_t WhenOf(const std::string& name, bool* automatic);

// ------------------------------------------------------------------------------- whole files
//
// At a place of either kind: under a mount of the virtual file layer, or on the console's own file
// system. (The logs' copy to a drive, frontend/ps5_logs.cpp, uses them too.) False with errno.

bool ReadWholeFile(const std::string& path, std::string* data);
/// The file holds exactly these bytes afterwards, or the one that was there is as it was.
bool WriteWholeFile(const std::string& path, const std::string& data);
/// The folder and those above it.
bool MakeAllFolders(const std::string& path);
/// The files of a folder (not its folders), by name, each with its size. False when the folder
/// cannot be listed (there is none, or its place does not answer).
bool ListFiles(const std::string& folder, std::vector<std::pair<std::string, uint64_t>>* files);
/// Whether there is such a file or folder.
bool PathExists(const std::string& path);
/// Why a place did not take or give a file (errno), as a sentence the user can act on.
std::string WhyNot(int error, const std::string& place);

// ------------------------------------------------------------------ one job at a time, on its thread

struct State
{
	bool busy = false;
	std::string doing;                // "Backing up: 3 of 12 files"; empty when idle
	unsigned generation = 0;          // bumped whenever anything else here has changed
	bool listed = false;              // `snapshots` is the answer for `listed_place`
	std::string listed_place;
	std::string list_error;           // why it could not be listed; empty when it could
	std::vector<Snapshot> snapshots;
};
State CurrentState();
/// Cheap, for every frame: what the running job is doing (empty when none runs), and how many
/// times the list of backups was read (a page showing it is made again when that moves).
std::string Doing();
unsigned ListsRead();

/// Each returns false when a job is running already (nothing is started then). `done` is called
/// on the job's thread, after the state says it is over; the list is read again by then.
bool StartBackup(const Folders& folders, const std::string& place, bool states, bool automatic, const std::string& version,
	std::function<void(const Result&)> done);
bool StartRestore(const Folders& folders, const Snapshot& snapshot, std::function<void(const Result&)> done);
bool StartList(const std::string& place);
/// Asked between files by every job: true stops it (a game is being started).
void SetStopCheck(std::function<bool()> stop);
/// For tests: waits until no job runs.
void WaitUntilIdle();

} // namespace PS5Backup
