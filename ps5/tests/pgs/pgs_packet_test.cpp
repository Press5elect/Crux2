// Crux2 - host test of the GIF packet length handed to paraLLEl-GS from VU1 memory (frontend/ps5_pgs_packet.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pgs_packet.h"

#include <cstdio>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		s_checks++;                                                                \
		if (!(cond))                                                               \
		{                                                                          \
			s_failed++;                                                            \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                          \
	} while (0)

static uint64_t Tag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg)
{
	return static_cast<uint64_t>(nloop) | (eop ? 1ull << 15 : 0) | (static_cast<uint64_t>(flg) << 58) |
		   (static_cast<uint64_t>(nreg & 0xF) << 60);
}

int main()
{
	using PS5PGS::PacketQwords;
	std::vector<uint64_t> q(2 * 1024, 0xDEADBEEFDEADBEEFull); // 1024 quadwords of rubbish after the packet
	auto put = [&](uint32_t at, uint64_t tag) { q[at * 2] = tag; q[at * 2 + 1] = 0; };
	const auto* bytes = reinterpret_cast<const uint8_t*>(q.data());

	// PACKED, 3 loops of 2 registers, EOP: 1 + 6.
	put(0, Tag(3, true, 0, 2));
	CHECK(PacketQwords(bytes, 1024) == 7);
	// PACKED with NREG 0 (16 registers), 1 loop: 1 + 16.
	put(0, Tag(1, true, 0, 0));
	CHECK(PacketQwords(bytes, 1024) == 17);
	// REGLIST, 3 loops of 3 registers: 9 doublewords, 5 quadwords.
	put(0, Tag(3, true, 1, 3));
	CHECK(PacketQwords(bytes, 1024) == 6);
	// IMAGE, 10 quadwords.
	put(0, Tag(10, true, 2, 0));
	CHECK(PacketQwords(bytes, 1024) == 11);
	// Two tags, the second with EOP: 1 + 2, then 1 + 4.
	put(0, Tag(2, false, 0, 1));
	put(3, Tag(4, true, 2, 0));
	CHECK(PacketQwords(bytes, 1024) == 8);
	// A packet longer than what is left of the memory: what is left.
	put(0, Tag(0x7FFF, true, 2, 0));
	CHECK(PacketQwords(bytes, 100) == 100);
	// Empty tags without EOP go on to the next.
	put(0, Tag(0, false, 0, 1));
	put(1, Tag(1, true, 0, 1));
	CHECK(PacketQwords(bytes, 1024) == 3);
	// Nothing left: nothing.
	CHECK(PacketQwords(bytes, 0) == 0);
	std::printf("pgs packets: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
