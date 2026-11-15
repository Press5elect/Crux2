// Crux2 - the logs, for the person in front of the console: the end of a log as lines to read,
// whether the run before this one ended in a crash, and a copy of the logs on a drive or a share.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The title writes crux2-boot.log (this run; ps5_platform.cpp) and keeps the one before as
// crux2-boot.previous.log, with the crash report as its last lines when there was one
// (ps5_crash.cpp). PCSX2's own log is logs/emulog.txt. Nothing here knows the console: the title
// folder is a path, and the tests run on the PC.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace PS5Logs
{
/// The files, as the title folder has them.
struct Files
{
	std::string now;      // <title>/crux2-boot.log
	std::string previous; // <title>/crux2-boot.previous.log
	std::string emulator; // <data>/logs/emulog.txt
	std::vector<std::string> extra; // more files, copied under their own names (a profiling build's counts)
};

/// The last `most` lines of a file, oldest first, as they can be drawn: tabs made spaces, what is
/// not text dropped, a line cut at `width` characters with the rest on lines of its own (indented),
/// so nothing of a long line is lost. A file that is not there: no lines. Only the file's last
/// `bytes` are read, so a log of any size costs the same.
std::vector<std::string> Tail(const std::string& path, size_t most = 400, size_t width = 150, size_t bytes = 256 * 1024);
/// The same of a log's text (a boot log kept in memory).
std::vector<std::string> TailText(const std::string& text, size_t most = 400, size_t width = 150);

/// The boot log kept in memory (from OGBox 0.0.7; Settings > Debug > Log > Boot log, "Only when
/// something goes wrong", the default): the session's first HEAD bytes kept whole and its last
/// HISTORY bytes after them, written to the file only when something goes wrong (ps5px_log_keep:
/// a crash, a fatal error, a game that did not start, 15 s without a new frame, "Save the log").
/// Fixed storage, and WriteOut is safe in a signal handler (write() and snprintf on the stack), for
/// the crash report. No lock of its own: the caller has one.
template <size_t HEAD, size_t HISTORY>
class MemoryLog
{
public:
	void Put(const char* data, size_t n)
	{
		if (m_head_length < HEAD)
		{
			const size_t k = std::min(n, HEAD - m_head_length);
			std::memcpy(m_head + m_head_length, data, k);
			m_head_length += k;
			data += k;
			n -= k;
		}
		if (n > HISTORY)
		{
			m_lost += n - HISTORY;
			data += n - HISTORY;
			n = HISTORY;
		}
		if (m_length + n > HISTORY)
		{
			const size_t drop = m_length + n - HISTORY;
			m_first = (m_first + drop) % HISTORY;
			m_length -= drop;
			m_lost += drop;
		}
		size_t at = (m_first + m_length) % HISTORY;
		for (size_t done = 0; done < n;)
		{
			const size_t k = std::min(n - done, HISTORY - at);
			std::memcpy(m_history + at, data + done, k);
			done += k;
			at = (at + k) % HISTORY;
		}
		m_length += n;
	}

	/// What it holds, as the file would get it (the note of the lines let go included).
	std::string Text() const
	{
		std::string out(m_head, m_head_length);
		if (m_lost)
		{
			char note[128];
			const int k = std::snprintf(note, sizeof(note), "[log: %llu bytes of lines from the middle of the session not kept]\n", m_lost);
			if (k > 0)
				out.append(note, std::min(static_cast<size_t>(k), sizeof(note) - 1));
		}
		const size_t first = std::min(m_length, HISTORY - m_first);
		out.append(m_history + m_first, first);
		out.append(m_history, m_length - first);
		return out;
	}

	/// Into the file (`why`: said at its top), and emptied.
	void WriteOut(int fd, const char* why)
	{
		if (fd < 0)
			return;
		char note[256];
		int k = std::snprintf(note, sizeof(note), "[log: kept in memory until now, written because %s]\n", why ? why : "it was asked for");
		if (k > 0)
			(void)!write(fd, note, std::min(static_cast<size_t>(k), sizeof(note) - 1));
		(void)!write(fd, m_head, m_head_length);
		if (m_lost)
		{
			k = std::snprintf(note, sizeof(note), "[log: %llu bytes of lines from the middle of the session not kept]\n", m_lost);
			if (k > 0)
				(void)!write(fd, note, std::min(static_cast<size_t>(k), sizeof(note) - 1));
		}
		const size_t first = std::min(m_length, HISTORY - m_first);
		(void)!write(fd, m_history + m_first, first);
		(void)!write(fd, m_history, m_length - first);
		m_head_length = m_first = m_length = 0;
		m_lost = 0;
	}

private:
	char m_head[HEAD];
	size_t m_head_length = 0;
	char m_history[HISTORY];
	size_t m_first = 0, m_length = 0; // the oldest byte of the history, and how many
	unsigned long long m_lost = 0;    // bytes between the head and the history let go
};

/// How the run before this one ended, from its log.
struct Ending
{
	bool crashed = false;   // it ended in a crash report, or stopped itself with a reason
	std::string what;       // "a memory fault (SIGSEGV)", "Crux2 stopped itself: <its reason>"
	std::string game;       // the game that was loaded then; empty when none was
	std::string build;      // the build that crashed ("v0.3.0, built ..." as its log says), may be empty
};
Ending PreviousRun(const std::string& previous_log);

/// The sentence(s) shown at start-up after a crash.
std::string Notice(const Ending& ending);

/// Copies the logs that exist to <place>/Crux2-logs/<moment>/. Returns how many files were
/// written; `message` gets a sentence for the user either way.
int CopyTo(const Files& files, const std::string& place, std::string* message);

} // namespace PS5Logs
