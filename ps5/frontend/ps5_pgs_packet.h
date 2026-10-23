// Crux2 - the length of a GIF packet (ps5_pgs.cpp's path 1), header-only for ps5/tests/pgs.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include <cstdint>
#include <cstring>

namespace PS5PGS
{
	/// The quadwords of the GIF packet at `data`: its tags and their data, up to the end of the tag with
	/// EOP set, and no more than `max_qwc`. A tag: NLOOP bits 0-14, EOP bit 15, FLG bits 58-59 (0
	/// PACKED: NLOOP x NREG quadwords; 1 REGLIST: NLOOP x NREG doublewords, rounded up to quadwords; 2
	/// and 3 IMAGE: NLOOP quadwords), NREG bits 60-63 (0 is 16).
	inline uint32_t PacketQwords(const uint8_t* data, uint32_t max_qwc)
	{
		uint32_t pos = 0;
		while (pos < max_qwc)
		{
			uint64_t tag;
			std::memcpy(&tag, data + static_cast<size_t>(pos) * 16, sizeof(tag));
			const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
			const bool eop = ((tag >> 15) & 1u) != 0;
			const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
			uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
			if (nreg == 0)
				nreg = 16;
			uint64_t body;
			if (flg == 0)
				body = static_cast<uint64_t>(nloop) * nreg;
			else if (flg == 1)
				body = (static_cast<uint64_t>(nloop) * nreg + 1) / 2;
			else
				body = nloop;
			const uint64_t next = static_cast<uint64_t>(pos) + 1 + body;
			if (next >= max_qwc)
				return max_qwc;
			pos = static_cast<uint32_t>(next);
			if (eop)
				break;
		}
		return pos;
	}
} // namespace PS5PGS
