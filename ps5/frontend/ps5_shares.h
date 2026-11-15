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
//
// Brought back after the revert to v1.0.18 (883c832), on top of what the virtual file layer does
// now: the start mounts network.cfg itself (ps5vfs_load_network_config, which reads the "#@ name"
// lines too), and Current() takes those mounts for its shares. Added from Porpoise 35c039c and
// bc835d2 (GPL-3.0-or-later): NFS servers found as well as SMB ones, a server's shares or exports
// listed, and a test that says what kind of thing went wrong.
#pragma once

#include "vfs/ps5_share_problem.h"

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
/* The file servers on the local network (its /24 around this console's address): those that
 * take a connection on port 445 (SMB) or 2049 (NFS), each with its NetBIOS name when it answers a
 * node status query. Blocks for up to about four seconds (the settings ask on a thread of their
 * own). */
struct FoundServer {
	std::string ip, name;
	bool smb{ false }, nfs{ false };
};
std::vector<FoundServer> findServers();
/* A NetBIOS node status answer's file server name (suffix 0x20, unique), else its first unique
 * name; "" for none. (For the tests.) */
std::string nbstatName(const unsigned char *answer, size_t length);

/* The address a share is mounted with, its account (or the file's) in it. */
std::string mountUrl(const Share &s, const ShareFile &file);
/* The share tried, not mounted (ps5vfs_test_share): the kind of failure, and the server's or the
 * library's words in detail. An NFS export the server does not list is NoSuchShare, the list in
 * detail. Blocks for up to about 20 s. */
PS5SharesProblem::Problem testShare(const Share &s, const ShareFile &file, std::string *detail = nullptr, size_t *entries = nullptr);
/* Whether an NFS path is one of the exports a server lists, or a folder inside one. */
bool exportListed(const std::string &path, const std::vector<std::string> &exports);
/* The shares of an SMB server, or the exports of an NFS one, for picking one (s.host, s.nfs, and
 * for SMB its account or the file's). Blocks for up to about 15 s. */
PS5SharesProblem::Problem listOnServer(const Share &s, const ShareFile &file, std::vector<std::string> &out, std::string *detail = nullptr);
std::string percentDecode(const std::string &s);

/* Crux2's shares as they are now: network.cfg as the start read and mounted it (the mounts the
 * virtual file layer made for it), as Settings > Folders changes them. Used on one thread at a
 * time (the interface's). */
ShareFile &Current();
const std::string &CurrentFile();
/* The mount points taken (every /smb mount the layer has), but for `except`'s. */
std::vector<std::string> MountsTaken(const Share *except = nullptr);
/* Current() written back to its file; false and why. */
bool Save(std::string *error = nullptr);

} // namespace PS5Shares
