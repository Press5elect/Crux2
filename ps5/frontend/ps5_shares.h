// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 v1.0.19 - the network shares as Settings > Folders edits them (from OGBox 0.0.3's
// ogbox_shares, Press5elect/OGBox). network.cfg stays the file it was (path, user, password, uid,
// gid lines); each share is one path line, with its own account in it when it has one
// ("user:password@server/share/folder", percent-encoded) and its name in a "#@ name = ..." line just
// above (a comment to anything else). Crux2 mounts them itself, each at /smb/<its name>, so a share
// added or changed in the settings is there at once. A share's name is its last folder unless it is
// given one: the mounts of a file written by hand are named as before.
#pragma once

#include <string>
#include <vector>

namespace PS5Shares {

struct Share {
	std::string name;       // as shown; its mount is /smb/<name, made safe>
	bool nfs{ false };
	std::string host;       // the server: an address or a name
	std::string path;       // SMB: share/folder; NFS: /export/folder (with ?options)
	std::string user, password;   // SMB; no user: the file's (guest)
	std::string mount;      // where it is mounted ("" not mounted)
};

struct ShareFile {
	std::vector<Share> shares;
	std::string user{ "guest" }, password;   // the file's account, for SMB shares without their own
	std::string uid, gid;                     // the file's, for NFS
};

/* Reads a network.cfg; false (and why) when it cannot be read. */
bool readShares(const std::string &file, ShareFile &out, std::string *error = nullptr);
/* Writes it whole (a short header, the file's account, each share); false and why. */
bool writeShares(const std::string &file, const ShareFile &in, std::string *error = nullptr);

/* "smb://192.168.1.10/games/xbox", "nfs://nas/volume1/xbox": the address, without the account. */
std::string shareAddress(const Share &s);
/* The mount's name for a share's name: letters, digits, '.', '_' and '-', at most 28, not taken. */
std::string shareMountName(const std::string &name, const std::vector<std::string> &taken);
/* Mounts it (at /smb/<name>), the file's account or uid/gid for what it lacks; false and why. */
bool mountShare(Share &s, const ShareFile &file, const std::vector<std::string> &taken, std::string *error = nullptr);
/* Its mount taken away (a read under way still ends). */
void unmountShare(Share &s);
/* The share's last folder (its name when it has none). */
std::string shareDefaultName(const Share &s);

std::string percentEncode(const std::string &s);
/* An account for an address: percent-encoded, a "DOMAIN;user"'s ';' kept. */
std::string accountEncode(const std::string &user);
/* The SMB servers on the local network (its /24 around this console's address): those that
 * take a connection on port 445, each with its NetBIOS name when it answers a node status query.
 * Blocks for up to about two seconds (the settings ask on a thread of their own). */
struct FoundServer {
	std::string ip, name;
};
std::vector<FoundServer> findServers();
std::string percentDecode(const std::string &s);

/* Crux2's shares as they are now: the file read at start (LoadAndMount), as Settings > Folders
 * changes them. Used on one thread at a time (the start, then the interface's). */
ShareFile &Current();
const std::string &CurrentFile();
/* network.cfg read and each share mounted; the number mounted, or -1 (and why) when the file
 * cannot be read. */
int LoadAndMount(const std::string &file, std::string *error = nullptr);
/* The mount points Current()'s shares have, but for `except`. */
std::vector<std::string> MountsTaken(const Share *except = nullptr);
/* Current() written back to its file; false and why. */
bool Save(std::string *error = nullptr);

} // namespace PS5Shares
