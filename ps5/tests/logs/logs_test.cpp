// Crux2 - the logs as the interface shows them (frontend/ps5_logs.cpp), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   logs_test <work folder>

#include "ps5_logs.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static void Put(const std::string& path, const std::string& text)
{
	FILE* const fp = fopen(path.c_str(), "wb");
	if (!fp)
		std::exit(2);
	fwrite(text.data(), 1, text.size(), fp);
	fclose(fp);
}

static std::string Get(const std::string& path)
{
	std::string data;
	if (FILE* const fp = fopen(path.c_str(), "rb"))
	{
		char block[4096];
		size_t got;
		while ((got = fread(block, 1, sizeof(block), fp)) > 0)
			data.append(block, got);
		fclose(fp);
	}
	return data;
}

int main(int argc, char* argv[])
{
	if (argc != 2)
		return 2;
	const std::string work = argv[1];
	using namespace PS5Logs;

	// ---- the end of a log, as lines
	CHECK(Tail(work + "/not there.log").empty());
	Put(work + "/empty.log", "");
	CHECK(Tail(work + "/empty.log").empty());
	{
		std::string text;
		for (int i = 1; i <= 1000; i++)
			text += "[" + std::to_string(i) + ".000] line " + std::to_string(i) + "\n";
		Put(work + "/long.log", text);
		const auto lines = Tail(work + "/long.log", 50);
		CHECK(lines.size() == 50);
		CHECK(lines.front() == "[951.000] line 951" && lines.back() == "[1000.000] line 1000");
		// Only the file's end is read: the first lines are not there, and no line is half of one.
		const auto few = Tail(work + "/long.log", 100000, 150, 4096);
		CHECK(!few.empty() && few.size() < 300 && few.back() == "[1000.000] line 1000");
		CHECK(few.front().compare(0, 1, "[") == 0 && few.front().find("] line ") != std::string::npos);
	}
	{
		// A long line is cut at a space, the rest indented; tabs and what is not text are handled;
		// a line with no end; UTF-8 is never cut inside a character.
		std::string longline = "[   1.000] warning:";
		for (int i = 0; i < 60; i++)
			longline += " word" + std::to_string(i);
		Put(work + "/wrap.log", longline + "\n\tindented\x01\x02 text  \r\n\n" + std::string(100, 'x') + "\xc3\xa9\xc3\xa9\xc3\xa9 caf\xc3\xa9\nno end");
		const auto lines = Tail(work + "/wrap.log", 400, 60);
		CHECK(lines.size() >= 8);
		size_t i = 0;
		CHECK(lines[0].compare(0, 19, "[   1.000] warning:") == 0 && lines[0].size() <= 60);
		std::string joined = lines[0];
		for (i = 1; i < lines.size() && lines[i].compare(0, 8, "        ") == 0; i++)
		{
			CHECK(lines[i].size() <= 60);
			joined += " " + lines[i].substr(8);
		}
		CHECK(joined == longline);
		CHECK(lines[i] == "    indented text");
		CHECK(lines[i + 1].empty());
		// 100 x and three two-byte letters: 60 characters, then the rest; every piece is whole UTF-8.
		CHECK(lines[i + 2] == std::string(60, 'x'));
		CHECK(lines[i + 3] == "        " + std::string(40, 'x') + "\xc3\xa9\xc3\xa9\xc3\xa9 caf\xc3\xa9");
		CHECK(lines.back() == "no end");
	}

	// ---- how the run before ended
	CHECK(!PreviousRun(work + "/not there.log").crashed);
	const std::string start = "[   0.000] PCSX2 v2.9.103\n[   0.001] build: v0.4.0, 2026-10-06 18:00 UTC\n[   2.000] interface started\n";
	Put(work + "/clean.log", start + "[  10.000] game: Some Game [SLES-00000, CRC 0BADF00D] /app0/games/some.iso\n[  99.000] exit requested\n[  99.500] shut down\n");
	{
		const Ending ending = PreviousRun(work + "/clean.log");
		CHECK(!ending.crashed && ending.game.empty() && Notice(ending).empty());
	}
	// Closed from the console's own menu: the log just stops. No crash.
	Put(work + "/closed.log", start + "[  10.000] game: Some Game [SLES-00000, CRC 0BADF00D] /app0/games/some.iso\n[  50.000] warning: something\n");
	CHECK(!PreviousRun(work + "/closed.log").crashed);
	Put(work + "/crash.log",
		start + "[  10.000] game: First Game [SLES-00000, CRC 0BADF00D] /app0/games/first.iso\n"
				"[  40.000] game: Second Game (Disc 1) [SLUS-11111, CRC 12345678] /smb/games/second [x].chd\n"
				"==== Crux2 stopped: SIGSEGV ====\n  signal 11, code 1, fault address 0x0000000000000010\n  program 0x0000000000400000\n"
				"==== end of the report ====\n");
	{
		const Ending ending = PreviousRun(work + "/crash.log");
		CHECK(ending.crashed);
		CHECK(ending.what == "a memory fault (SIGSEGV)");
		CHECK(ending.game == "Second Game (Disc 1)");
		CHECK(ending.build == "v0.4.0, 2026-10-06 18:00 UTC");
		const std::string notice = Notice(ending);
		CHECK(notice.find("while Second Game (Disc 1) was loaded") != std::string::npos);
		CHECK(notice.find("a memory fault (SIGSEGV)") != std::string::npos);
		std::fprintf(stderr, "  notice: %s\n", notice.c_str());
	}
	// The run before was one of a build before v0.6.0: its line is understood too.
	Put(work + "/crash-before.log", start + "==== PSPCSX2 stopped: SIGBUS ====\n");
	{
		const Ending ending = PreviousRun(work + "/crash-before.log");
		CHECK(ending.crashed && ending.what.find("(SIGBUS)") != std::string::npos);
	}
	Put(work + "/crash-menu.log", start + "==== Crux2 stopped: SIGILL ====\n");
	{
		const Ending ending = PreviousRun(work + "/crash-menu.log");
		CHECK(ending.crashed && ending.game.empty() && ending.what.find("SIGILL") != std::string::npos);
		CHECK(Notice(ending).find(" while ") == std::string::npos);
	}
	Put(work + "/fatal.log", start + "[   3.000] FATAL: The graphics device could not be created. crux2-boot.log and logs/emulog.txt have the reason.\n");
	{
		const Ending ending = PreviousRun(work + "/fatal.log");
		CHECK(ending.crashed && ending.what.compare(0, 22, "Crux2 stopped itself: ") == 0);
	}
	Put(work + "/abort.log", start + "[   5.000] game: A Game [SLES-00000, CRC 0BADF00D] /x.iso\n==== abort() was called ====\n  caller 0x1\n==== end of the report ====\n");
	{
		const Ending ending = PreviousRun(work + "/abort.log");
		CHECK(ending.crashed && ending.what.find("abort()") != std::string::npos && ending.game == "A Game");
	}
	// The shell's exit at the end of a clean shutdown goes through the same report: not a crash.
	Put(work + "/exit.log", start + "[  99.000] exit requested\n[  99.500] shut down\n==== exit() was called ====\n==== end of the report ====\n");
	CHECK(!PreviousRun(work + "/exit.log").crashed);

	// ---- the copy
	{
		const std::string title = work + "/title", place = work + "/usb";
		(void)!std::system(("rm -rf '" + title + "' '" + place + "' && mkdir -p '" + title + "/logs' '" + place + "'").c_str());
		Files files{title + "/crux2-boot.log", title + "/crux2-boot.previous.log", title + "/logs/emulog.txt"};
		std::string message;
		CHECK(CopyTo(files, place, &message) == 0 && message == "There is no log to copy yet.");
		Put(files.now, "now\n");
		Put(files.emulator, std::string(300000, 'e'));
		CHECK(CopyTo(files, place, &message) == 2);
		CHECK(message.compare(0, 41, "Copied 2 logs to the folder Crux2-logs/20") == 0);
		const std::string folder = place + "/" + message.substr(28, message.size() - 29);
		CHECK(Get(folder + "/crux2-boot.log") == "now\n");
		CHECK(Get(folder + "/emulog.txt") == std::string(300000, 'e'));
		struct stat st;
		CHECK(stat((folder + "/crux2-boot.previous.log").c_str(), &st) != 0);

		// A profiling build's counts go along under their own names; one that is gone is skipped.
		(void)!std::system(("mkdir -p '" + title + "/pgo'").c_str());
		Put(title + "/pgo/crux2-2026-10-11_22-30-00.profraw", std::string(70000, 'p'));
		files.extra = {title + "/pgo/crux2-2026-10-11_22-30-00.profraw", title + "/pgo/gone.profraw"};
		CHECK(CopyTo(files, place, &message) == 3);
		const std::string with_counts = place + "/" + message.substr(28, message.size() - 29);
		CHECK(Get(with_counts + "/crux2-2026-10-11_22-30-00.profraw") == std::string(70000, 'p'));
		CHECK(stat((with_counts + "/gone.profraw").c_str(), &st) != 0);
	}

	std::fprintf(stderr, "logs: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
