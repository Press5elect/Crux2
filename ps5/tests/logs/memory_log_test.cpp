// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 - host test of the boot log kept in memory (PS5Logs::MemoryLog, ps5_logs.h): the head kept
// whole, the history wrapping, the note of what was let go, the text and the file written the same,
// emptied after; and TailText over such a text.
#include "ps5_logs.h"
#include "vfs/ps5_vfs_internal.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

static int s_failures = 0;
#define CHECK(x)                                                                 \
	do                                                                           \
	{                                                                            \
		if (!(x))                                                                \
		{                                                                        \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
			s_failures++;                                                        \
		}                                                                        \
	} while (0)

// ps5_logs.cpp copies the logs through the virtual file layer; its network backends are not in this test.
namespace ps5vfs {
std::unique_ptr<Backend> CreateSMBBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateNFSBackend(const char*, std::string* error) { *error = "not in this test"; return nullptr; }
std::unique_ptr<Backend> CreateFTPBackend(unsigned, const char*, std::string* error) { *error = "not in this test"; return nullptr; }
} // namespace ps5vfs

static std::string ReadFile(const std::string& path)
{
	std::string out;
	if (FILE* const f = std::fopen(path.c_str(), "rb"))
	{
		char b[4096];
		size_t n;
		while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
			out.append(b, n);
		std::fclose(f);
	}
	return out;
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : "/tmp";
	static PS5Logs::MemoryLog<16, 32> log;

	// Under the head: all of it.
	log.Put("0123456789", 10);
	CHECK(log.Text() == "0123456789");

	// The head fills, the rest goes into the history.
	log.Put("abcdefghij", 10);
	CHECK(log.Text() == "0123456789abcdefghij");

	// The history wraps: the oldest of it let go, and said.
	std::string more;
	for (int i = 0; i < 40; i++)
		more += static_cast<char>('A' + (i % 26));
	log.Put(more.data(), more.size());
	const std::string text = log.Text();
	CHECK(text.compare(0, 16, "0123456789abcdef") == 0);
	// head 16; history had "ghij" (4) + 40 = 44 > 32: 12 let go
	CHECK(text.find("[log: 12 bytes of lines from the middle of the session not kept]\n") == 16);
	const std::string tail = (std::string("ghij") + more).substr(12);
	CHECK(text.size() >= 32 && text.compare(text.size() - 32, 32, tail) == 0);

	// A piece larger than the whole history.
	static PS5Logs::MemoryLog<4, 8> small;
	small.Put("HEADxxxxxxxxxxxx12345678", 24);
	CHECK(small.Text() == "HEAD[log: 12 bytes of lines from the middle of the session not kept]\n12345678");

	// Into a file: the note of why, then the same text; emptied after.
	const std::string path = dir + "/memory_log_test.log";
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	log.WriteOut(fd, "a test asked");
	close(fd);
	const std::string file = ReadFile(path);
	CHECK(file == "[log: kept in memory until now, written because a test asked]\n" + text);
	CHECK(log.Text().empty());
	unlink(path.c_str());

	// TailText: the lines of such a text.
	const std::vector<std::string> lines = PS5Logs::TailText("[   0.001] one\n[   0.002] two\nthree", 2, 150);
	CHECK(lines.size() == 2);
	if (lines.size() == 2)
	{
		CHECK(lines[0].find("two") != std::string::npos);
		CHECK(lines[1] == "three");
	}

	if (s_failures)
	{
		std::fprintf(stderr, "memory log test: %d failed\n", s_failures);
		return 1;
	}
	std::printf("memory log test: ok\n");
	return 0;
}
