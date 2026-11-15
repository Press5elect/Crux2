// Crux2 - the elevation client asks the payload loader again for a few seconds (ps5/elevation/elevation.cpp),
// over a stand-in for the console's network and file calls.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A loader that is not listening is tried 16 times, a quarter of a second apart, and then the title
// stays in its sandbox; one that comes up late is reached on the try it answers; and once the helper
// has been sent, its answer is final (no second try).

#include "elevation.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <sys/types.h>
#include <unistd.h>

namespace
{
int s_failures = 0;
#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

int s_refusals = 0;     // connections refused before one is taken (-1: all of them)
int s_connects = 0;
int s_sockets_open = 0;
int s_sleeps = 0;
unsigned long s_slept_us = 0;
int s_helper_reads = 0;
bool s_answer_ok = true;
std::deque<elevation::wire::Message> s_replies;
elevation::wire::Message s_request{};

void Reset(int refusals, bool answer_ok)
{
	s_refusals = refusals;
	s_connects = s_sockets_open = s_sleeps = s_helper_reads = 0;
	s_slept_us = 0;
	s_answer_ok = answer_ok;
	s_replies.clear();
}
} // namespace

extern "C"
{
int sceKernelOpen(const char*, int, mode_t) { return 7; }
int sceKernelClose(int) { return 0; }
std::int64_t sceKernelRead(int, void* buffer, std::size_t length)
{
	// The helper: 64 bytes, read once.
	if (s_helper_reads++ > 0 || length < 64)
		return 0;
	std::memset(buffer, 0x7f, 64);
	return 64;
}
int sceKernelUsleep(unsigned int microseconds)
{
	s_sleeps++;
	s_slept_us += microseconds;
	return 0;
}
int sceNetSocket(const char*, int, int, int)
{
	s_sockets_open++;
	return 40 + s_sockets_open;
}
int sceNetSocketClose(int)
{
	s_sockets_open--;
	return 0;
}
int sceNetSetsockopt(int, int, int, const void*, std::uint32_t) { return 0; }
int sceNetConnect(int, const void*, std::uint32_t)
{
	s_connects++;
	if (s_refusals < 0 || s_connects <= s_refusals)
		return -1;
	return 0;
}
int sceNetSend(int, const void* data, std::size_t length, int)
{
	// The request (after the helper's bytes) gets the helper's "prepare"; the "prepared" its answer.
	if (length == sizeof(elevation::wire::Message))
	{
		elevation::wire::Message message;
		std::memcpy(&message, data, sizeof(message));
		if (message.kind == elevation::wire::Kind::request)
		{
			s_request = message;
			elevation::wire::Message reply = message;
			reply.kind = elevation::wire::Kind::prepare;
			s_replies.push_back(reply);
		}
		else if (message.kind == elevation::wire::Kind::prepared)
		{
			elevation::wire::Message reply = s_request;
			reply.kind = elevation::wire::Kind::response;
			reply.status = s_answer_ok ? elevation::Status::ok : elevation::Status::apply_failed;
			s_replies.push_back(reply);
		}
	}
	return static_cast<int>(length);
}
int sceNetRecv(int, void* data, std::size_t length, int)
{
	if (s_replies.empty() || length < sizeof(elevation::wire::Message))
		return -1;
	std::memcpy(data, &s_replies.front(), sizeof(elevation::wire::Message));
	s_replies.pop_front();
	return static_cast<int>(sizeof(elevation::wire::Message));
}
}

int main()
{
	using elevation::Status;

	// Nothing listens: 16 tries, 15 pauses of a quarter of a second, and the sandbox kept.
	Reset(-1, true);
	CHECK(elevation::request(elevation::Capability::filesystem, "/app0/helper.elf") == Status::transport_error);
	CHECK(s_connects == 16);
	CHECK(elevation::attempts() == 16);
	CHECK(s_sleeps == 15);
	CHECK(s_slept_us == 15u * 250000u);
	CHECK(s_sockets_open == 0);
	CHECK(s_helper_reads == 0);

	// The loader comes up on the fourth try: reached, the helper sent once, the answer taken.
	Reset(3, true);
	CHECK(elevation::request(elevation::Capability::filesystem, "/app0/helper.elf") == Status::ok);
	CHECK(s_connects == 4);
	CHECK(elevation::attempts() == 4);
	CHECK(s_sleeps == 3);
	CHECK(s_sockets_open == 0);
	CHECK(s_helper_reads == 2);

	// Listening at once: one try, no pause.
	Reset(0, true);
	CHECK(elevation::request(elevation::Capability::filesystem, "/app0/helper.elf") == Status::ok);
	CHECK(elevation::attempts() == 1);
	CHECK(s_sleeps == 0);

	// The helper refuses: its answer is final, not tried again.
	Reset(0, false);
	CHECK(elevation::request(elevation::Capability::filesystem, "/app0/helper.elf") == Status::apply_failed);
	CHECK(s_connects == 1);
	CHECK(s_sleeps == 0);

	if (s_failures == 0)
		std::printf("elevation retry: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
