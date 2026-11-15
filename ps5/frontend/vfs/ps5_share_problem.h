// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - what went wrong reaching a network share, in one of a few kinds, from what the layer
 * and libsmb2 or libnfs wrote down while it tried (ps5vfs_test_share). Header only: the PC tests
 * read it without the layer.
 *
 * From Porpoise 35c039c and bc835d2 (GPL-3.0-or-later): its netfs Problem and classify(), with
 * Crux2's own port-check words added.
 */
#pragma once

#include <string>

namespace PS5SharesProblem
{
enum class Problem
{
	None,
	NoServer,    // no server given
	NoShare,     // no share or export given
	UnknownName, // no server by that name
	Unreachable, // nothing answered: a wrong address, the server off, sharing off, a firewall
	Refused,     // the server is there but refuses the port: file sharing (or NFS) is off
	SignIn,      // the user or password was not accepted
	NoSuchShare, // no share (or export) by that name
	Denied,      // not allowed in
	NoFolder,    // the folder inside the share is not there
	NoExports,   // an NFS server that does not list its exports
	Other,       // the detail says what
};

// Porpoise's classify() (porpoise_netfs.cpp), the server's or library's words to a kind. The
// first match wins, in this order.
inline Problem Classify(const std::string& text)
{
	const auto has = [&](const char* k) { return text.find(k) != std::string::npos; };
	if (has("LOGON_FAILURE") || has("WRONG_PASSWORD") || has("ACCOUNT_") || has("PASSWORD_EXPIRED"))
		return Problem::SignIn;
	if (has("BAD_NETWORK_NAME"))
		return Problem::NoSuchShare;
	if (has("ACCESS_DENIED") || has("ERR_ACCES") || has("ERR_PERM") || has("Permission denied") || has("AUTH_ERROR") ||
		has("AUTH_TOOWEAK") || has("AUTH_BADCRED") || has("access denied"))
		return Problem::Denied;
	if (has("is not among its exports") || has("exports no folder"))
		return Problem::NoSuchShare;
	if (has("NOT_FOUND") || has("NOT_A_DIRECTORY") || has("PATH_INVALID") || has("ERR_NOENT") || has("ERR_NOTDIR") ||
		has("No such file"))
		return Problem::NoFolder;
	if (has("cannot be looked up") || has("no address for") || has("resolve") || has("Invalid address"))
		return Problem::UnknownName;
	if (has("refuses the") || has("Connection refused"))
		return Problem::Refused;
	if (has("nothing answers") || has("connect failed") || has("Timeout") || has("timed out") || has("TIMEOUT") ||
		has("POLLHUP") || has("socket") || has("connect()") || has("Failed to connect") || has("portmap") ||
		has("RPC ERROR") || has("No route") || has("unreachable") || has("no answer"))
		return Problem::Unreachable;
	return Problem::Other;
}

// What to tell the player, a sentence that says what to look at.
inline const char* Text(Problem problem)
{
	switch (problem)
	{
		case Problem::None: return "It works: Crux2 can read the share.";
		case Problem::NoServer: return "Give the share's server (its IP address is the safe choice).";
		case Problem::NoShare: return "Give the share's folder.";
		case Problem::UnknownName: return "No server by that name was found. Give its IP address instead.";
		case Problem::Unreachable:
			return "Nothing answered at that address: check it, that the PC or NAS is on and on this network, and that no "
			       "firewall blocks file sharing.";
		case Problem::Refused:
			return "The server is there but refuses the connection: turn file sharing (SMB) or NFS on, on the PC or NAS.";
		case Problem::SignIn: return "The user or password was not accepted.";
		case Problem::NoSuchShare: return "The server has no share (or export) of that name. Pick one from its list.";
		case Problem::Denied: return "The server does not let this account in. Check the share's permissions.";
		case Problem::NoFolder: return "The share is there, but not the folder inside it.";
		case Problem::NoExports: return "The NFS server did not list its exports. Type the export's path.";
		case Problem::Other: break;
	}
	return "The share could not be read; crux2-boot.log says what the server answered.";
}
} // namespace PS5SharesProblem
