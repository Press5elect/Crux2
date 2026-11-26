// Crux2 - the write broker on the PC (ps5/broker, frontend/ps5_broker.cpp): the payload's request
// handler on a thread at the end of a socketpair, the title's Session and File against it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   broker_test <work folder>
//
// - paths: the broker takes /data/, /mnt/usb<n>/ and /mnt/ext<n>/ only, no "." or ".." part;
//   Wanted only /data/ and only with the setting on;
// - Copy (COPY): sizes 0 to 70 MiB, byte for byte; a source that is shorter than said; a broker
//   that dies part-way (false: the caller copies it); one that can't be started;
// - File (WRITE): random piece sizes up to 3 MiB, 0 to 60 MiB, byte for byte, with the broker's
//   pwrites cut short; a broker that dies after 1 to 40 requests: the file is still whole (the
//   title writes the unconfirmed pieces and the rest); one that can't be started (null);
// - a Session not started when nothing wants it, and asked to quit at its end.

#include "ps5_broker.h"
#include "../../broker/server.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
int s_failures = 0;
#define CHECK(c)                                                                  \
	do                                                                            \
	{                                                                             \
		if (!(c))                                                                 \
		{                                                                         \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);   \
			s_failures++;                                                         \
		}                                                                         \
	} while (0)

std::string s_root;
std::atomic<unsigned> s_started{0}, s_quit{0};
unsigned s_die_after = 0;
size_t s_io_cap = 0;
bool s_refuse = false;
std::vector<std::thread> s_servers;

int Connect(std::string& why)
{
	if (s_refuse)
	{
		why = "refused by the test";
		return -1;
	}
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
	{
		why = "no socketpair";
		return -1;
	}
	s_started++;
	const unsigned die = s_die_after;
	const size_t cap = s_io_cap;
	s_servers.emplace_back([fd = fds[1], die, cap] {
		broker::ServerOptions o;
		o.root = s_root.c_str();
		o.die_after = die;
		o.io_cap = cap;
		o.idle_ms = 20000;
		if (broker::serve(fd, fd, o) == broker::End::quit)
			s_quit++;
		close(fd);
	});
	why = "a socketpair";
	return fds[0];
}

void Join()
{
	for (std::thread& t : s_servers)
		t.join();
	s_servers.clear();
}

uint64_t s_rng = 0x9e3779b97f4a7c15ull;
uint64_t Next()
{
	s_rng ^= s_rng >> 12;
	s_rng ^= s_rng << 25;
	s_rng ^= s_rng >> 27;
	return s_rng * 0x2545f4914f6cdd1dull;
}

std::vector<uint8_t> Bytes(size_t n, uint64_t salt)
{
	std::vector<uint8_t> v(n);
	broker::fill_pattern(v.data(), n, 0, salt);
	return v;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& v)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = v.empty() || std::fwrite(v.data(), 1, v.size(), f) == v.size();
	return std::fclose(f) == 0 && ok;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
	std::vector<uint8_t> v;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return v;
	std::fseek(f, 0, SEEK_END);
	v.resize(static_cast<size_t>(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (!v.empty() && std::fread(v.data(), 1, v.size(), f) != v.size())
		v.clear();
	std::fclose(f);
	return v;
}

void Paths()
{
	const char* good[] = {"/data/a", "/data/homebrew/PPSA99249/textures/x.png", "/mnt/usb0/a", "/mnt/ext12/b/c"};
	const char* bad[] = {"/data/", "/data", "/data/../etc", "/data/./a", "/data//a", "/mnt/usb/a", "/mnt/usb0", "/mnt/usb1234/a",
		"/system/a", "data/a", "/data/a/"};
	for (const char* p : good)
		CHECK(broker::path_allowed(p, std::strlen(p)));
	for (const char* p : bad)
		CHECK(!broker::path_allowed(p, std::strlen(p)));
	PS5Broker::SetForTest(Connect, s_root, true);
	CHECK(PS5Broker::Wanted(s_root + "/data/x"));
	CHECK(!PS5Broker::Wanted(s_root + "/mnt/usb0/x"));
	CHECK(!PS5Broker::Wanted("/elsewhere/data/x"));
	PS5Broker::SetForTest(Connect, s_root, false);
	CHECK(!PS5Broker::Wanted(s_root + "/data/x"));
	PS5Broker::SetForTest(Connect, s_root, true);
}

void Copies()
{
	const size_t sizes[] = {0, 1, 4095, 4096, 1u << 20, (32u << 20) - 1, 32u << 20, (32u << 20) + 7, 70u << 20};
	unsigned i = 0;
	for (const size_t n : sizes)
	{
		const std::string from = s_root + "/mnt/usb0/c" + std::to_string(i), to = s_root + "/data/c" + std::to_string(i);
		const std::vector<uint8_t> v = Bytes(n, i + 1);
		CHECK(WriteFile(from, v));
		uint64_t moved = 0;
		std::string why;
		{
			PS5Broker::Session s("copy test");
			CHECK(s.Copy(from, to, n, nullptr, [&](uint64_t m) { moved += m; }, &why));
		}
		Join();
		CHECK(why.empty());
		CHECK(moved == n);
		CHECK(ReadFile(to) == v);
		i++;
	}
	// a source shorter than said
	{
		const std::string from = s_root + "/mnt/usb0/short", to = s_root + "/data/short";
		CHECK(WriteFile(from, Bytes(1000, 9)));
		std::string why;
		{
			PS5Broker::Session s("copy test");
			CHECK(!s.Copy(from, to, 2000, nullptr, nullptr, &why));
		}
		Join();
		CHECK(!why.empty());
	}
	// a path the broker doesn't take: false, nothing started
	{
		const unsigned before = s_started;
		std::string why;
		{
			PS5Broker::Session s("copy test");
			CHECK(!s.Copy("/elsewhere/a", s_root + "/data/a", 1, nullptr, nullptr, &why));
		}
		CHECK(why.empty());
		CHECK(s_started == before);
	}
	// a broker that dies part-way: false (the caller copies it itself)
	for (unsigned die = 2; die <= 6; die++)
	{
		const std::string from = s_root + "/mnt/usb0/c8", to = s_root + "/data/dies";
		s_die_after = die;
		std::string why;
		bool ok;
		{
			PS5Broker::Session s("copy test");
			ok = s.Copy(from, to, 70u << 20, nullptr, nullptr, &why);
		}
		Join();
		s_die_after = 0;
		CHECK(!ok);
		CHECK(!why.empty());
	}
	// can't be started
	{
		s_refuse = true;
		std::string why;
		{
			PS5Broker::Session s("copy test");
			CHECK(!s.Copy(s_root + "/mnt/usb0/c1", s_root + "/data/r", 1, nullptr, nullptr, &why));
		}
		s_refuse = false;
	}
}

// Writes `total` bytes in random pieces through a File (or the title, when Create gave none).
bool WriteThrough(PS5Broker::Session& s, const std::string& path, const std::vector<uint8_t>& v, bool* brokered)
{
	std::string why;
	std::unique_ptr<PS5Broker::File> f = s.Create(path, &why);
	*brokered = f != nullptr;
	if (!f)
		return WriteFile(path, v);
	size_t at = 0;
	bool ok = true;
	while (ok && at < v.size())
	{
		const size_t n = std::min<size_t>(v.size() - at, 1 + Next() % (3u << 20));
		ok = f->Write(v.data() + at, n);
		at += n;
	}
	return f->Finish(&why) && ok;
}

void Files()
{
	const size_t sizes[] = {0, 1, 8u << 20, (8u << 20) + 1, 60u << 20};
	unsigned i = 0;
	for (const size_t cap : {size_t(0), size_t(65536 + 3)})
	{
		s_io_cap = cap;
		for (const size_t n : sizes)
		{
			const std::string path = s_root + "/data/w" + std::to_string(i);
			const std::vector<uint8_t> v = Bytes(n, 100 + i);
			bool brokered = false;
			{
				PS5Broker::Session s("write test");
				CHECK(WriteThrough(s, path, v, &brokered));
			}
			Join();
			CHECK(brokered);
			CHECK(ReadFile(path) == v);
			i++;
		}
	}
	s_io_cap = 0;
	// several files on one session
	{
		PS5Broker::Session s("write test");
		for (unsigned k = 0; k < 5; k++)
		{
			const std::string path = s_root + "/data/m" + std::to_string(k);
			const std::vector<uint8_t> v = Bytes(3u << 20, 300 + k);
			bool brokered = false;
			CHECK(WriteThrough(s, path, v, &brokered));
			CHECK(brokered);
			CHECK(ReadFile(path) == v);
		}
	}
	Join();
	CHECK(s_quit >= 1);
	// a broker that dies after `die` requests: the file is whole all the same
	for (unsigned die = 2; die <= 40; die += 3)
	{
		const std::string path = s_root + "/data/d" + std::to_string(die);
		const std::vector<uint8_t> v = Bytes(50u << 20, 500 + die);
		s_die_after = die;
		bool ok;
		{
			PS5Broker::Session s("write test");
			bool brokered = false;
			ok = WriteThrough(s, path, v, &brokered);
		}
		Join();
		s_die_after = 0;
		CHECK(ok);
		CHECK(ReadFile(path) == v);
	}
	// can't be started: no File
	{
		s_refuse = true;
		PS5Broker::Session s("write test");
		std::string why;
		CHECK(s.Create(s_root + "/data/none", &why) == nullptr);
		s_refuse = false;
	}
	// not on the console's storage: no File, nothing started
	{
		const unsigned before = s_started;
		PS5Broker::Session s("write test");
		std::string why;
		CHECK(s.Create(s_root + "/mnt/usb0/no", &why) == nullptr);
		CHECK(why.empty());
		CHECK(s_started == before);
	}
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: broker_test <work folder>\n");
		return 2;
	}
	s_root = argv[1];
	mkdir(s_root.c_str(), 0777);
	mkdir((s_root + "/data").c_str(), 0777);
	mkdir((s_root + "/mnt").c_str(), 0777);
	mkdir((s_root + "/mnt/usb0").c_str(), 0777);
	Paths();
	Copies();
	Files();
	PS5Broker::SetForTest(nullptr, "", false);
	Join();
	std::fprintf(stderr, "broker: %s (%u brokers started)\n", s_failures ? "FAILED" : "all passed", s_started.load());
	return s_failures ? 1 : 0;
}
