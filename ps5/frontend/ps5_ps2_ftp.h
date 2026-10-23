// Crux2 - the memory cards of a real PlayStation 2, read over the network from the FTP server it
// runs (wLaunchELF's, which is ps2ftpd: "ps2ftpd ready", anonymous, Unix-style listings).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Plain sockets and nothing of the console, so the PC tests it against a local FTP server
// (ps5/tests/ps2ftp). ps2ftpd names a memory card "/mc/0" and "/mc/1" (a USB drive "/mass/0");
// its listing of "/" itself can fail in passive mode, so the cards are asked for by name, and a
// listing that fails in passive mode is asked again in active mode.
//
// What it reads: the saves on the cards (folders at a card's top, each with its files), whole.
// Nothing is ever written to the PlayStation 2.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace PS5Ps2Ftp
{
struct Entry
{
	std::string name;
	bool folder = false;
	uint64_t size = 0;
};

/// "drwxrwxrwx   1 ps2  ps2  0 Sep  8  2013 CBC" and the like; false for what is no entry.
bool ParseListLine(const std::string& line, Entry* entry);

class Client
{
public:
	~Client();

	/// Connects to `host` (an IPv4 address) and logs in as anonymous. False with `why`.
	bool Connect(const std::string& host, unsigned port, std::string* why);
	void Close();

	/// The entries of a folder ("/mc/0"). False when it cannot be listed.
	bool List(const std::string& folder, std::vector<Entry>* entries, std::string* why);
	/// A whole file, up to `limit` bytes. False when it cannot be read.
	bool Read(const std::string& path, std::string* data, uint64_t limit, std::string* why);

	/// Seconds a reply or a transfer may take before it is given up.
	void SetTimeout(int seconds) { m_timeout = seconds; }

private:
	int Command(const std::string& line);
	bool ReadReply(int* code);
	int OpenData(const std::string& command, std::string* why);
	int OpenPassive(std::string* why);
	int OpenActive(int* listener, std::string* why);

	int m_control = -1;
	std::string m_reply;
	std::string m_host;
	int m_timeout = 10;
	bool m_active = false; // passive failed once: active from then on
};

/// A save on a card of the PlayStation 2.
struct Save
{
	std::string card;   // "Memory card 1"
	std::string path;   // "/mc/0/BESLES-12345SAVE"
	std::string name;   // "BESLES-12345SAVE"
	uint64_t bytes = 0; // its files together
	int files = 0;
};

/// The saves on both cards (folders that hold an icon.sys). False with `why` when no card could be
/// listed at all (no server, or a server that is not a PlayStation 2's).
bool ListSaves(Client& client, std::vector<Save>* saves, std::string* why);

/// A save's files (name and bytes). False with `why`.
struct SaveFile
{
	std::string name;
	std::string data;
};
bool ReadSave(Client& client, const std::string& path, std::vector<SaveFile>* files, std::string* why);
} // namespace PS5Ps2Ftp
