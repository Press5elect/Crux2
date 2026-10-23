// Crux2 - the logs, for the person in front of the console (ps5_logs.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_logs.h"

#include "ps5_backup.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PS5Logs
{
namespace
{
// The file's last `bytes` (all of it when it is smaller), from the start of a line.
std::string ReadEnd(const std::string& path, size_t bytes)
{
	std::string data;
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return data;
	struct stat st;
	off_t from = 0;
	if (fstat(fd, &st) == 0 && static_cast<uint64_t>(st.st_size) > bytes)
		from = st.st_size - static_cast<off_t>(bytes);
	if (from > 0 && lseek(fd, from, SEEK_SET) != from)
		from = 0;
	char block[65536];
	for (;;)
	{
		const ssize_t got = read(fd, block, sizeof(block));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		data.append(block, static_cast<size_t>(got));
		if (data.size() > bytes + sizeof(block))
			break; // (a file that grows while it is read)
	}
	close(fd);
	if (from > 0)
	{
		// The first line is the end of one: left out.
		const size_t line = data.find('\n');
		data.erase(0, line == std::string::npos ? data.size() : line + 1);
	}
	return data;
}

// One line of the file as it can be drawn.
std::string Clean(const char* text, size_t length)
{
	std::string out;
	out.reserve(length);
	for (size_t i = 0; i < length; i++)
	{
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '\t')
			out += "    ";
		else if (c >= 0x20 && c != 0x7f)
			out.push_back(static_cast<char>(c)); // (UTF-8 passes: its bytes are all above 0x7f)
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

// Cuts at `width` characters, at a space when one is near, never inside a UTF-8 character.
void Wrap(const std::string& line, size_t width, std::vector<std::string>* out)
{
	static constexpr const char* INDENT = "        ";
	size_t at = 0;
	bool first = true;
	while (at < line.size())
	{
		const size_t room = first ? width : width - std::strlen(INDENT);
		size_t end = at, characters = 0, space = std::string::npos;
		while (end < line.size() && characters < room)
		{
			if (line[end] == ' ')
				space = end;
			end++;
			while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80)
				end++;
			characters++;
		}
		if (end < line.size() && space != std::string::npos && space > at + room / 2)
			end = space;
		std::string piece = line.substr(at, end - at);
		out->push_back(first ? std::move(piece) : INDENT + piece);
		at = end;
		while (at < line.size() && line[at] == ' ')
			at++;
		first = false;
	}
	if (first)
		out->emplace_back();
}

// The text of a log line after its time ("[  12.345] text").
std::string AfterTime(const std::string& line)
{
	if (!line.empty() && line[0] == '[')
	{
		const size_t close = line.find("] ");
		if (close != std::string::npos && close < 16)
			return line.substr(close + 2);
	}
	return line;
}

const char* SignalMeaning(const std::string& name)
{
	if (name == "SIGSEGV" || name == "SIGBUS")
		return "a memory fault";
	if (name == "SIGILL")
		return "an instruction the processor refused";
	if (name == "SIGFPE")
		return "an arithmetic fault";
	if (name == "SIGABRT")
		return "it gave up on itself";
	if (name == "SIGSYS" || name == "SIGTRAP")
		return "a call the console refused";
	return "a fault";
}
} // namespace

std::vector<std::string> Tail(const std::string& path, size_t most, size_t width, size_t bytes)
{
	std::vector<std::string> lines;
	const std::string data = ReadEnd(path, bytes);
	width = std::max<size_t>(width, 40);
	size_t at = 0;
	while (at < data.size())
	{
		size_t end = data.find('\n', at);
		if (end == std::string::npos)
			end = data.size();
		Wrap(Clean(data.data() + at, end - at), width, &lines);
		at = end + 1;
	}
	if (lines.size() > most)
		lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(most));
	return lines;
}

Ending PreviousRun(const std::string& previous_log)
{
	Ending ending;
	const std::string data = ReadEnd(previous_log, 2 * 1024 * 1024);
	bool shut_down = false;
	size_t at = 0;
	while (at < data.size())
	{
		size_t end = data.find('\n', at);
		if (end == std::string::npos)
			end = data.size();
		const std::string line = AfterTime(Clean(data.data() + at, end - at));
		at = end + 1;

		if (line.compare(0, 6, "game: ") == 0)
		{
			// "game: Title [SERIAL, CRC 0BADF00D] /path": the title. A line with no title: the BIOS.
			// (The serial's bracket is the one before ", CRC": a title or a path may have brackets too.)
			const size_t crc = line.find(", CRC ");
			const size_t bracket = (crc != std::string::npos) ? line.rfind(" [", crc) : std::string::npos;
			ending.game = (bracket != std::string::npos && bracket > 6) ? line.substr(6, bracket - 6) : std::string();
			continue;
		}
		if (line.compare(0, 7, "build: ") == 0)
		{
			ending.build = line.substr(7);
			continue;
		}
		if (line == "shut down" || line == "exit requested")
		{
			// The title was being closed: what an ending says after this is the closing's own.
			shut_down = true;
			continue;
		}
		if (shut_down || ending.crashed)
			continue;

		// (A build before v0.6.0 wrote the line with the name the title had then: the run before
		// the first one of this build may be one of those.)
		static constexpr const char STOPPED[] = "==== Crux2 stopped: ";
		static constexpr const char STOPPED_BEFORE[] = "==== PSPCSX2 stopped: ";
		const bool stopped = line.compare(0, sizeof(STOPPED) - 1, STOPPED) == 0;
		if (stopped || line.compare(0, sizeof(STOPPED_BEFORE) - 1, STOPPED_BEFORE) == 0)
		{
			std::string name = line.substr((stopped ? sizeof(STOPPED) : sizeof(STOPPED_BEFORE)) - 1);
			const size_t space = name.find(' ');
			if (space != std::string::npos)
				name.erase(space);
			ending.crashed = true;
			ending.what = std::string(SignalMeaning(name)) + " (" + name + ")";
			continue;
		}
		if (line.compare(0, 5, "==== ") == 0 && line.find(" was called ====") != std::string::npos)
		{
			ending.crashed = true;
			ending.what = "it ended itself without saying why (" + line.substr(5, line.find(" was called") - 5) + ")";
			continue;
		}
		if (line.compare(0, 7, "FATAL: ") == 0)
		{
			ending.crashed = true;
			ending.what = "Crux2 stopped itself: " + line.substr(7);
			continue;
		}
	}
	if (!ending.crashed)
		ending.game.clear();
	return ending;
}

std::string Notice(const Ending& ending)
{
	if (!ending.crashed)
		return std::string();
	std::string text = "The last time Crux2 ran, it stopped unexpectedly";
	if (!ending.game.empty())
		text += " while " + ending.game + " was loaded";
	text += ": " + ending.what + ".\n\n";
	text += "The log of that run is kept. Settings > Log shows it, and \"Copy the logs\" there puts it on a USB drive or a share.";
	return text;
}

int CopyTo(const Files& files, const std::string& place, std::string* message)
{
	char stamp[48] = "unknown";
	const time_t now = time(nullptr);
	struct tm local;
	if (localtime_r(&now, &local) != nullptr)
		strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
	const std::string folder = place + (place.empty() || place.back() == '/' ? "" : "/") + "Crux2-logs/" + stamp;
	if (!PS5Backup::MakeAllFolders(folder))
	{
		*message = "The folder for the logs could not be made. " + PS5Backup::WhyNot(errno, place);
		return 0;
	}

	const std::pair<const std::string*, const char*> all[] = {
		{&files.now, "crux2-boot.log"}, {&files.previous, "crux2-boot.previous.log"}, {&files.emulator, "emulog.txt"}};
	int written = 0;
	std::string data;
	for (const auto& [path, name] : all)
	{
		if (path->empty() || !PS5Backup::ReadWholeFile(*path, &data))
			continue;
		if (!PS5Backup::WriteWholeFile(folder + "/" + name, data))
		{
			*message = "The logs could not be written. " + PS5Backup::WhyNot(errno, place);
			return written;
		}
		written++;
	}
	for (const std::string& path : files.extra)
	{
		if (path.empty() || !PS5Backup::ReadWholeFile(path, &data))
			continue;
		const size_t slash = path.find_last_of('/');
		if (!PS5Backup::WriteWholeFile(folder + "/" + (slash == std::string::npos ? path : path.substr(slash + 1)), data))
		{
			*message = "The logs could not be written. " + PS5Backup::WhyNot(errno, place);
			return written;
		}
		written++;
	}
	if (written == 0)
		*message = "There is no log to copy yet.";
	else
		*message = "Copied " + std::to_string(written) + (written == 1 ? " log" : " logs") + " to the folder Crux2-logs/" + stamp + ".";
	return written;
}

} // namespace PS5Logs
