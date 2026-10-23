// Crux2 - opening up the title's files (ps5_open_up.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   openup_test <work folder>
//
// Makes a title folder as a console has one after some use (settings, memory cards, saved states,
// logs, covers for a library, what the build shipped, games), with the modes files get there, and
// checks what a walk changes: everything the title keeps and nothing else, nothing at all the
// second time, only what a run writes when the title closes, no link followed, and that a walk
// told to stop does. Then how long a walk over 6,000 covers takes that has nothing to change,
// against one that changes every file, as v0.8.0's did on every start.
#include "ps5_open_up.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <thread>
#include <unistd.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(what) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); \
		} \
	} while (0)

static void Folder(const std::string& path, mode_t mode)
{
	if (mkdir(path.c_str(), 0777) != 0 || chmod(path.c_str(), mode) != 0)
	{
		std::perror(path.c_str());
		std::exit(2);
	}
}

static void File(const std::string& path, mode_t mode)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0 || write(fd, "x", 1) != 1 || fchmod(fd, mode) != 0)
	{
		std::perror(path.c_str());
		std::exit(2);
	}
	close(fd);
}

static mode_t ModeOf(const std::string& path)
{
	struct stat st;
	return (lstat(path.c_str(), &st) == 0) ? (st.st_mode & 07777) : static_cast<mode_t>(0xffff);
}

static double Seconds(std::chrono::steady_clock::time_point then)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - then).count();
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: openup_test <work folder>\n");
		return 2;
	}
	const std::string work = argv[1];
	if (std::system(("rm -rf '" + work + "/title' '" + work + "/elsewhere' '" + work + "/many'").c_str()) != 0)
		return 2;
	umask(022); // what a file gets that nobody opened up

	// ---- a title folder
	const std::string t = work + "/title";
	Folder(t, 0755);
	for (const char* name : {"inis", "memcards", "sstates", "logs", "gamesettings", "covers", "cache", "textures", "resources", "games",
			 "sce_sys", "licenses"})
		Folder(t + "/" + name, 0755);
	Folder(t + "/sstates/deep", 0700);
	Folder(t + "/covers/more", 0755);
	Folder(t + "/resources/fonts", 0755);
	const std::map<std::string, mode_t> kept = {
		{"network.cfg", 0600}, {"crux2-boot.log", 0644}, {"inis/PCSX2.ini", 0644}, {"memcards/Mcd001.ps2", 0600},
		{"sstates/a.p2s", 0644}, {"sstates/deep/b.p2s", 0400}, {"logs/emulog.txt", 0644}, {"gamesettings/one.ini", 0640},
		{"covers/one.png", 0644}, {"covers/more/two.png", 0644}, {"cache/shaders.bin", 0644}, {"textures/pack.dds", 0644}};
	const std::map<std::string, mode_t> shipped = {{"eboot.bin", 0755},       {"README.md", 0644},        {"LEGAL.txt", 0644},
		{"symbols.txt", 0644},     {"resources/fonts/a.ttf", 0644}, {"games/a game.iso", 0444}, {"sce_sys/param.json", 0644},
		{"licenses/one.txt", 0644}};
	for (const auto& [name, mode] : kept)
		File(t + "/" + name, mode);
	for (const auto& [name, mode] : shipped)
		File(t + "/" + name, mode);
	// One that is open already, with more than it needs: left as it is.
	File(t + "/memcards/open.ps2", 0777);
	// A link out of the title folder: neither followed nor changed.
	Folder(work + "/elsewhere", 0700);
	File(work + "/elsewhere/private.txt", 0600);
	CHECK(symlink((work + "/elsewhere").c_str(), (t + "/sstates/link").c_str()) == 0);

	// ---- when the title closes: what a run writes, and only that
	{
		PS5OpenUp::Opened count;
		PS5OpenUp::In(t.c_str(), true, nullptr, &count);
		for (const char* name : {"network.cfg", "crux2-boot.log", "inis/PCSX2.ini", "memcards/Mcd001.ps2", "sstates/a.p2s",
				 "sstates/deep/b.p2s", "logs/emulog.txt", "gamesettings/one.ini"})
			CHECK(ModeOf(t + "/" + name) == 0666);
		for (const char* name : {"inis", "memcards", "sstates", "sstates/deep", "logs", "gamesettings"})
			CHECK(ModeOf(t + "/" + name) == 0777);
		for (const char* name : {"covers/one.png", "covers/more/two.png", "cache/shaders.bin", "textures/pack.dds"})
			CHECK(ModeOf(t + "/" + name) == 0644);
		for (const char* name : {"covers", "cache", "textures"})
			CHECK(ModeOf(t + "/" + name) == 0755);
		CHECK(ModeOf(t + "/memcards/open.ps2") == 0777);
		CHECK(count.failed == 0 && count.changed == 14); // 8 files and 6 folders
		CHECK(count.seen == 15);                         // and the card that was open
	}

	// ---- at the start: everything the title keeps
	{
		PS5OpenUp::Opened count;
		PS5OpenUp::In(t.c_str(), false, nullptr, &count);
		for (const auto& [name, mode] : kept)
			CHECK(ModeOf(t + "/" + name) == 0666);
		for (const char* name : {"covers", "covers/more", "cache", "textures"})
			CHECK(ModeOf(t + "/" + name) == 0777);
		CHECK(count.failed == 0 && count.changed == 8); // 4 files and 4 folders: the rest was done
		CHECK(count.seen == 15 + 8);
		// What the build shipped and the games: as they were.
		for (const auto& [name, mode] : shipped)
			CHECK(ModeOf(t + "/" + name) == mode);
		for (const char* name : {"resources", "resources/fonts", "games", "sce_sys", "licenses"})
			CHECK(ModeOf(t + "/" + name) == 0755);
		// The link, and where it points.
		CHECK(ModeOf(work + "/elsewhere") == 0700 && ModeOf(work + "/elsewhere/private.txt") == 0600);
	}

	// ---- again: nothing is written
	for (const bool session_only : {false, true})
	{
		PS5OpenUp::Opened count;
		PS5OpenUp::In(t.c_str(), session_only, nullptr, &count);
		CHECK(count.changed == 0 && count.failed == 0 && count.seen == (session_only ? 15u : 23u));
	}

	// ---- a folder that is not there, and one that is a file
	{
		PS5OpenUp::Opened count;
		PS5OpenUp::In((work + "/no such folder").c_str(), false, nullptr, &count);
		PS5OpenUp::In((t + "/network.cfg").c_str(), false, nullptr, &count);
		CHECK(count.seen == 0 && count.changed == 0 && count.failed == 0);
	}

	// ---- covers for a library: a walk that has to change everything, one that has nothing to
	// change, and one that is told to stop
	const std::string m = work + "/many";
	Folder(m, 0755);
	Folder(m + "/covers", 0755);
	Folder(m + "/inis", 0755);
	File(m + "/inis/PCSX2.ini", 0644);
	const unsigned covers = 6000;
	for (unsigned i = 0; i < covers; i++)
		File(m + "/covers/" + std::to_string(i) + ".png", 0644);
	{
		auto started = std::chrono::steady_clock::now();
		PS5OpenUp::Opened first;
		PS5OpenUp::In(m.c_str(), false, nullptr, &first);
		const double changing = Seconds(started);
		CHECK(first.changed == covers + 3 && first.seen == covers + 3 && first.failed == 0);

		started = std::chrono::steady_clock::now();
		PS5OpenUp::Opened second;
		PS5OpenUp::In(m.c_str(), false, nullptr, &second);
		const double looking = Seconds(started);
		CHECK(second.changed == 0 && second.seen == covers + 3);

		started = std::chrono::steady_clock::now();
		PS5OpenUp::Opened closing;
		PS5OpenUp::In(m.c_str(), true, nullptr, &closing);
		const double at_the_end = Seconds(started);
		CHECK(closing.seen == 2 && closing.changed == 0); // the settings folder and its file
		std::printf("  %u covers on this PC: %.1f ms to open them all up, %.1f ms when they are open already, %.2f ms when the title closes\n",
			covers, changing * 1e3, looking * 1e3, at_the_end * 1e3);

		// Told to stop before it begins: the top of the folder is looked at, nothing below it.
		for (unsigned i = 0; i < covers; i++)
			(void)chmod((m + "/covers/" + std::to_string(i) + ".png").c_str(), 0644);
		std::atomic_bool stop{true};
		PS5OpenUp::Opened stopped;
		PS5OpenUp::In(m.c_str(), false, &stop, &stopped);
		CHECK(stopped.seen == 2 && stopped.changed == 0);
		CHECK(ModeOf(m + "/covers/0.png") == 0644);

		// Told to stop while it goes, from another thread: it ends, and what it did not reach is
		// as it was, to be done by the next start.
		stop.store(false);
		PS5OpenUp::Opened partly;
		std::thread walker([&] { PS5OpenUp::In(m.c_str(), false, &stop, &partly); });
		stop.store(true);
		walker.join();
		CHECK(partly.changed <= covers && partly.failed == 0);
		PS5OpenUp::Opened rest;
		PS5OpenUp::In(m.c_str(), false, nullptr, &rest);
		CHECK(partly.changed + rest.changed == covers);
		CHECK(ModeOf(m + "/covers/5999.png") == 0666);
	}

	std::printf("opening up the title's files: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}
