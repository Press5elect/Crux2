// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 - the name server test and the public DNS asked when the console's name server answers
// 0.0.0.0 (frontend/ps5_net.cpp), against a made-up name server on 127.0.0.1.

#include "ps5_net.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

extern "C" void ps5px_log(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	std::fputs("  log: ", stdout);
	std::vprintf(format, arguments);
	std::fputs("\n", stdout);
	va_end(arguments);
}

namespace
{
int s_failures = 0;
int s_checks = 0;

void Check(bool ok, const char* what)
{
	s_checks++;
	if (!ok)
	{
		s_failures++;
		std::printf("FAILED: %s\n", what);
	}
}

// The made-up server: "game.example" is 203.0.113.7 behind a CNAME, "gone.example" no such name,
// and "silent.example" is never answered.
std::atomic<bool> s_stop{false};
std::atomic<int> s_questions{0};

std::string QuestionName(const unsigned char* q, size_t n)
{
	std::string name;
	size_t at = 12;
	while (at < n && q[at] != 0)
	{
		const size_t len = q[at];
		if (!name.empty())
			name += '.';
		name.append(reinterpret_cast<const char*>(q + at + 1), len);
		at += len + 1;
	}
	return name;
}

void Serve(int fd)
{
	while (!s_stop)
	{
		pollfd p{fd, POLLIN, 0};
		if (poll(&p, 1, 50) <= 0)
			continue;
		unsigned char q[512];
		sockaddr_in from{};
		socklen_t length = sizeof(from);
		const ssize_t got = recvfrom(fd, q, sizeof(q), 0, reinterpret_cast<sockaddr*>(&from), &length);
		if (got < 17)
			continue;
		s_questions++;
		const std::string name = QuestionName(q, static_cast<size_t>(got));
		if (name == "silent.example")
			continue;
		unsigned char r[512];
		std::memcpy(r, q, static_cast<size_t>(got));
		size_t n = static_cast<size_t>(got);
		r[2] = 0x81;
		r[3] = name == "gone.example" ? 0x83 : 0x80;
		if (name == "game.example")
		{
			r[7] = 2; // two answers: a CNAME, then the A record
			const unsigned char cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 12};
			std::memcpy(r + n, cname, sizeof(cname));
			n += sizeof(cname);
			const unsigned char a[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 203, 0, 113, 7};
			std::memcpy(r + n, a, sizeof(a));
			n += sizeof(a);
		}
		sendto(fd, r, n, 0, reinterpret_cast<sockaddr*>(&from), length);
	}
}

int s_console_answer_zero = 1;
int ConsoleResolve(const char* name, std::uint8_t address[4])
{
	if (std::strcmp(name, "blocked.example") == 0 || std::strcmp(name, "game.example") == 0)
	{
		std::memset(address, 0, 4); // what a filtering name server answers
		return s_console_answer_zero ? 0 : EAI_NONAME;
	}
	return EAI_NONAME;
}
} // namespace

int main()
{
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in here{};
	here.sin_family = AF_INET;
	here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&here), sizeof(here)) != 0)
	{
		std::printf("dns: no UDP socket on 127.0.0.1: skipped\n");
		return 0;
	}
	socklen_t length = sizeof(here);
	getsockname(fd, reinterpret_cast<sockaddr*>(&here), &length);
	std::thread server(Serve, fd);
	PS5Net::Testing::SetNameServerPort(ntohs(here.sin_port));

	{
		const PS5Net::NameServerTest test = PS5Net::TestNameServer("127.0.0.1", "game.example");
		Check(test.result == 1, "a name server that knows the name: an address");
		Check(test.address == "203.0.113.7", "the A record after the CNAME");
	}
	{
		const PS5Net::NameServerTest test = PS5Net::TestNameServer("127.0.0.1", "gone.example");
		Check(test.result == 0, "no such name: it answered, without an address");
		Check(test.why == "no such name", "the reason given");
	}
	{
		const int before = s_questions;
		const PS5Net::NameServerTest test = PS5Net::TestNameServer("127.0.0.1", "silent.example");
		Check(test.result == -1, "a name server that never answers: no answer");
		Check(s_questions - before == 9, "three tests of three tries each");
		Check(test.ms >= 4500 && test.ms < 8000, "0.6 s a try");
	}
	{
		const PS5Net::NameServerTest test = PS5Net::TestNameServer("not an address", "game.example");
		Check(test.result == -1, "not an address: no answer");
	}
	{
		const PS5Net::NameServerTest test = PS5Net::TestNameServer("127.0.0.1", std::string(70, 'a') + ".example");
		Check(test.result == 0, "a label longer than 63: not asked");
	}

	// The console's name server answers 0.0.0.0: the public one (here, the made-up one) is asked.
	PS5Net::Testing::Probes probes;
	probes.resolve = ConsoleResolve;
	PS5Net::Testing::SetProbes(probes);
	PS5Net::Testing::SetPublicNameServers({"127.0.0.1"});
	{
		std::uint8_t address[4] = {};
		Check(PS5Net::Resolve("game.example", address) == 0, "0.0.0.0 from the console: public DNS gave the name");
		Check(address[0] == 203 && address[1] == 0 && address[2] == 113 && address[3] == 7, "the public answer");
	}
	{
		std::uint8_t address[4] = {9, 9, 9, 9};
		Check(PS5Net::Resolve("blocked.example", address) == EAI_NONAME, "0.0.0.0 from both: not found");
		Check(address[0] == 9, "the address left alone");
	}
	{
		PS5Net::Testing::Reset();
		s_console_answer_zero = 0;
		const int before = s_questions;
		std::uint8_t address[4] = {};
		Check(PS5Net::Resolve("game.example", address) == EAI_NONAME, "not found by the console: not asked elsewhere");
		Check(s_questions == before, "no public question for a name the console does not know");
	}

	s_stop = true;
	server.join();
	close(fd);
	std::printf("dns: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures ? 1 : 0;
}
