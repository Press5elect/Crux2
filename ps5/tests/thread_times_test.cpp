// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - host test of the threads' self-reported processor times (common/PS5ThreadTimes.h).
#include "PS5ThreadTimes.h"

#include <cstdio>
#include <thread>
#include <vector>

static int checks, failures;
#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

int main()
{
	static PS5ThreadTimes times;

	// nothing noted: 0; no handle: nothing
	CHECK(times.Read(0x1000) == 0);
	times.Publish(0, 55);
	CHECK(times.Read(0) == 0);

	times.Publish(0x1000, 10);
	times.Publish(0x2000, 7);
	CHECK(times.Read(0x1000) == 10);
	CHECK(times.Read(0x2000) == 7);
	times.Publish(0x1000, 25);
	CHECK(times.Read(0x1000) == 25);
	CHECK(times.Read(0x3000) == 0);

	// a thread that ended: its handle starts from nothing for the thread that gets it next
	times.Forget(0x1000);
	CHECK(times.Read(0x1000) == 0);
	times.Publish(0x1000, 3);
	CHECK(times.Read(0x1000) == 3);
	CHECK(times.Read(0x2000) == 7);
	times.Forget(0x4000); // never noted: nothing happens
	times.Forget(0x1000);
	times.Forget(0x2000);

	// more threads than slots: the extra ones read 0, the others are right, nothing is overwritten
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS + 8; i++)
		times.Publish(i * 16, i);
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS; i++)
		CHECK(times.Read(i * 16) == i);
	for (uintptr_t i = PS5ThreadTimes::SLOTS + 1; i <= PS5ThreadTimes::SLOTS + 8; i++)
		CHECK(times.Read(i * 16) == 0);
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS; i++)
		times.Forget(i * 16);
	// and every slot is free again
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS; i++)
		times.Publish(i * 32, i + 100);
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS; i++)
		CHECK(times.Read(i * 32) == i + 100);
	for (uintptr_t i = 1; i <= PS5ThreadTimes::SLOTS; i++)
		times.Forget(i * 32);

	// threads noting their own rising times while another reads: a reading never goes back, and
	// the last reading is the last time noted
	static constexpr int THREADS = 8, STEPS = 20000;
	std::atomic<bool> go{false};
	std::vector<std::thread> writers;
	for (int t = 0; t < THREADS; t++)
	{
		writers.emplace_back([t, &go]() {
			while (!go.load())
				std::this_thread::yield();
			for (uint64_t step = 1; step <= STEPS; step++)
				times.Publish(static_cast<uintptr_t>(0x100000 + t * 64), step);
		});
	}
	bool went_back = false;
	std::thread reader([&go, &went_back]() {
		uint64_t last[THREADS] = {};
		go.store(true);
		for (int round = 0; round < 200000; round++)
		{
			for (int t = 0; t < THREADS; t++)
			{
				const uint64_t now = times.Read(static_cast<uintptr_t>(0x100000 + t * 64));
				if (now < last[t])
					went_back = true;
				last[t] = now;
			}
		}
	});
	for (std::thread& writer : writers)
		writer.join();
	reader.join();
	CHECK(!went_back);
	for (int t = 0; t < THREADS; t++)
		CHECK(times.Read(static_cast<uintptr_t>(0x100000 + t * 64)) == STEPS);

	std::printf("thread times: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
