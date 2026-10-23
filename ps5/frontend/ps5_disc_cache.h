// Crux2 - a disc in the console's drive, held in memory as it is read.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2 reads a disc in a drive in blocks of 16 sectors, keeps 1024 of them, looks 16 blocks
// ahead of the game's last read, and when the game wants a block that is not there yet the
// emulation thread itself waits for the drive. From an image file that wait is nothing. From a
// DVD drive it is a seek: tenths of a second, a run of dropped frames each time. On a PC that is
// the same; nobody plays that way there. On the console it is what "play the disc" means.
//
// So, for the console (vendor CDVD/CDVDdiscThread.cpp uses these two classes):
//
//   Store    every block ever read stays, up to a budget: the whole disc when memory has room for
//            it, else the parts used last. Kept in pieces of 32 MiB that are made as they are
//            first written to.
//   Planner  what the reading thread does next: the block the game asked for; then far ahead of
//            it (8 MiB, where PCSX2 looks 512 KiB); then the rest of the disc, on from where the
//            reading ahead ended (so the drive's head does not move for it) and round to where
//            it began; and when there is nothing to read, a block now and then so that the drive
//            does not spin down and take seconds to come back.
//
// Neither knows a drive or a clock: the thread reads, and says what time it is. The PC tests
// (ps5/tests/disc/disc_cache_test.cpp) run them over a made-up drive.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace PS5DiscCache
{
constexpr uint32_t BLOCK_SECTORS = 16;
constexpr uint32_t CHUNK_BLOCKS = 1024;
constexpr uint32_t NONE = 0xFFFFFFFFu;

// The blocks of a disc that have been read. Any thread.
class Store
{
public:
	~Store() { Configure(0, 0, 0); }

	// A disc of `sectors`, each block `block_bytes` long, and at most `budget_bytes` kept
	// (at least one piece is). Everything held before goes.
	void Configure(uint32_t sectors, uint32_t block_bytes, uint64_t budget_bytes)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		for (Chunk& chunk : m_chunks)
			std::free(chunk.data);
		m_chunks.clear();
		m_sectors = sectors;
		m_block_bytes = block_bytes;
		m_blocks = (sectors + BLOCK_SECTORS - 1) / BLOCK_SECTORS;
		m_chunks.resize((m_blocks + CHUNK_BLOCKS - 1) / CHUNK_BLOCKS);
		m_chunk_bytes = static_cast<uint64_t>(block_bytes) * CHUNK_BLOCKS;
		m_budget_chunks = (m_chunk_bytes != 0) ? std::max<uint64_t>(budget_bytes / m_chunk_bytes, 1) : 0;
		m_held_chunks = 0;
		m_held_blocks = 0;
		m_bad_blocks = 0;
		m_evicted = 0;
		m_tick = 0;
	}

	uint32_t Blocks() const { return m_blocks; }
	uint32_t Sectors() const { return m_sectors; }
	uint32_t BlockBytes() const { return m_block_bytes; }

	// Whether the budget is the whole disc: nothing read is ever let go.
	bool HoldsAll() const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return m_budget_chunks >= m_chunks.size();
	}

	bool Has(uint32_t lsn) const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const uint32_t block = lsn / BLOCK_SECTORS;
		if (block >= m_blocks)
			return false;
		const Chunk& chunk = m_chunks[block / CHUNK_BLOCKS];
		return chunk.data != nullptr && Bit(chunk.present, block % CHUNK_BLOCKS);
	}

	// The block that begins at sector `lsn`, into `out`. False when it is not held.
	bool Fetch(uint32_t lsn, uint8_t* out)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const uint32_t block = lsn / BLOCK_SECTORS;
		if (block >= m_blocks)
			return false;
		Chunk& chunk = m_chunks[block / CHUNK_BLOCKS];
		const uint32_t index = block % CHUNK_BLOCKS;
		if (chunk.data == nullptr || !Bit(chunk.present, index))
			return false;
		chunk.used = ++m_tick;
		std::memcpy(out, chunk.data + static_cast<size_t>(index) * m_block_bytes, m_block_bytes);
		return true;
	}

	// Keeps a block. False when there is no memory for it (it is then simply not kept).
	bool Put(uint32_t lsn, const uint8_t* data)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const uint32_t block = lsn / BLOCK_SECTORS;
		if (block >= m_blocks)
			return false;
		const size_t which = block / CHUNK_BLOCKS;
		Chunk& chunk = m_chunks[which];
		if (chunk.data == nullptr)
		{
			if (m_held_chunks >= m_budget_chunks && !EvictOne(which))
				return false;
			chunk.data = static_cast<uint8_t*>(std::malloc(m_chunk_bytes));
			if (chunk.data == nullptr)
			{
				// Memory is shorter than the budget said: what is held now is what there is room
				// for, and the piece used longest ago makes room from here on.
				m_budget_chunks = std::max<uint64_t>(m_held_chunks, 1);
				if (!EvictOne(which))
					return false;
				chunk.data = static_cast<uint8_t*>(std::malloc(m_chunk_bytes));
				if (chunk.data == nullptr)
					return false;
			}
			std::memset(chunk.present, 0, sizeof(chunk.present));
			chunk.held = 0;
			m_held_chunks++;
		}
		const uint32_t index = block % CHUNK_BLOCKS;
		std::memcpy(chunk.data + static_cast<size_t>(index) * m_block_bytes, data, m_block_bytes);
		if (!Bit(chunk.present, index))
		{
			chunk.present[index / 64] |= (1ull << (index % 64));
			chunk.held++;
			m_held_blocks++;
		}
		if (Bit(chunk.bad, index))
		{
			chunk.bad[index / 64] &= ~(1ull << (index % 64));
			m_bad_blocks--;
		}
		chunk.used = ++m_tick;
		return true;
	}

	// A block the drive could not read: not asked for again by the filling of the disc.
	void MarkBad(uint32_t lsn)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const uint32_t block = lsn / BLOCK_SECTORS;
		if (block >= m_blocks)
			return;
		Chunk& chunk = m_chunks[block / CHUNK_BLOCKS];
		const uint32_t index = block % CHUNK_BLOCKS;
		if (!Bit(chunk.bad, index))
		{
			chunk.bad[index / 64] |= (1ull << (index % 64));
			m_bad_blocks++;
		}
	}

	// The first block at or after sector `from` that is neither held nor bad, looking on from the
	// disc's start when its end is reached. NONE when there is none.
	uint32_t NextMissing(uint32_t from) const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_blocks == 0)
			return NONE;
		const uint32_t start = std::min(from / BLOCK_SECTORS, m_blocks - 1);
		for (uint32_t step = 0; step < m_blocks;)
		{
			const uint32_t block = (start + step) % m_blocks;
			const Chunk& chunk = m_chunks[block / CHUNK_BLOCKS];
			const uint32_t index = block % CHUNK_BLOCKS;
			// A whole piece that has nothing left to read is stepped over at once.
			if (index == 0 && chunk.data != nullptr && step + CHUNK_BLOCKS <= m_blocks && Full(chunk, block))
			{
				step += CHUNK_BLOCKS;
				continue;
			}
			if (!(chunk.data != nullptr && Bit(chunk.present, index)) && !Bit(chunk.bad, index))
				return block * BLOCK_SECTORS;
			step++;
		}
		return NONE;
	}

	// How many of the blocks from sector `lsn` on are missing in a row, up to `most` (0: the first
	// one is held, bad, or past the disc's end).
	uint32_t MissingRun(uint32_t lsn, uint32_t most) const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		uint32_t block = lsn / BLOCK_SECTORS, run = 0;
		while (run < most && block < m_blocks)
		{
			const Chunk& chunk = m_chunks[block / CHUNK_BLOCKS];
			const uint32_t index = block % CHUNK_BLOCKS;
			if ((chunk.data != nullptr && Bit(chunk.present, index)) || Bit(chunk.bad, index))
				break;
			run++;
			block++;
		}
		return run;
	}

	struct Counts
	{
		uint32_t blocks = 0, held = 0, bad = 0;
		uint64_t bytes = 0;    // memory taken
		uint64_t evicted = 0;  // pieces let go to make room
	};
	Counts Count() const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return {m_blocks, m_held_blocks, m_bad_blocks, m_held_chunks * m_chunk_bytes, m_evicted};
	}

	// Every block is held (or could not be read): the drive is not needed any more.
	bool Complete() const
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return m_blocks != 0 && m_held_blocks + m_bad_blocks >= m_blocks;
	}

private:
	struct Chunk
	{
		uint8_t* data = nullptr;
		uint64_t present[CHUNK_BLOCKS / 64] = {};
		uint64_t bad[CHUNK_BLOCKS / 64] = {};
		uint32_t held = 0;
		uint64_t used = 0;
	};

	static bool Bit(const uint64_t* bits, uint32_t index) { return (bits[index / 64] >> (index % 64)) & 1; }

	// (The piece that begins at `block` and lies whole inside the disc.)
	static bool Full(const Chunk& chunk, uint32_t)
	{
		for (uint32_t word = 0; word < CHUNK_BLOCKS / 64; word++)
		{
			if ((chunk.present[word] | chunk.bad[word]) != ~0ull)
				return false;
		}
		return true;
	}

	// The piece used longest ago goes (never the one about to be written to). With m_mutex held.
	bool EvictOne(size_t keep)
	{
		size_t oldest = m_chunks.size();
		for (size_t i = 0; i < m_chunks.size(); i++)
		{
			if (i == keep || m_chunks[i].data == nullptr)
				continue;
			if (oldest == m_chunks.size() || m_chunks[i].used < m_chunks[oldest].used)
				oldest = i;
		}
		if (oldest == m_chunks.size())
			return false;
		Chunk& chunk = m_chunks[oldest];
		std::free(chunk.data);
		chunk.data = nullptr;
		m_held_blocks -= chunk.held;
		chunk.held = 0;
		std::memset(chunk.present, 0, sizeof(chunk.present));
		m_held_chunks--;
		m_evicted++;
		return true;
	}

	mutable std::mutex m_mutex;
	std::vector<Chunk> m_chunks;
	uint32_t m_sectors = 0, m_blocks = 0, m_block_bytes = 0;
	uint64_t m_chunk_bytes = 0, m_budget_chunks = 0, m_held_chunks = 0;
	uint32_t m_held_blocks = 0, m_bad_blocks = 0;
	uint64_t m_evicted = 0, m_tick = 0;
};

// What the reading thread does next. One thread.
class Planner
{
public:
	struct Options
	{
		uint32_t ahead_blocks = 256; // how far past the game's last read: 8 MiB of a DVD
		uint32_t ahead_piece = 4;    // blocks read at once ahead of the game: 128 KiB
		uint32_t fill_piece = 4;     // blocks read at once of the rest of the disc: 128 KiB (a read the game
		                             // asks for meanwhile waits for one such piece at most)
		bool fill = true;            // the whole disc, when the store holds all of it
		bool keep_spinning = true;
		double fill_after = 0.0;     // seconds the game has to be quiet before the filling goes on (0: it
		                             // goes on as soon as nothing is left to read ahead of the game: it
		                             // carries on from there, and a game that streams without a pause
		                             // would otherwise never have its disc read in)
		double keep_alive = 20.0;    // seconds without a read before one is made for the drive's sake
	};

	struct Step
	{
		enum Kind { Nothing, Ahead, Fill, KeepAlive } kind = Nothing;
		uint32_t lsn = 0;
		uint32_t blocks = 0;
	};

	void Reset(const Options& options)
	{
		m_options = options;
		m_ahead_next = 0;
		m_ahead_left = 0;
		m_fill_cursor = 0;
		m_last_lsn = NONE;
		m_last_request = -1e9;
		m_last_read = -1e9;
		m_read_anything = false;
	}

	// The game asked for the block at `lsn` (and it was read, or was there already).
	void Requested(uint32_t lsn, double now)
	{
		m_ahead_next = lsn + BLOCK_SECTORS;
		m_ahead_left = m_options.ahead_blocks;
		m_last_request = now;
		m_last_lsn = lsn;
		// The rest of the disc is read from where the game is, so the drive's head stays there.
		m_fill_cursor = lsn;
	}

	// The drive was read at `now` (by whoever: the game's own waits count too).
	void DriveRead(double now)
	{
		m_last_read = now;
		m_read_anything = true;
	}

	Step Next(const Store& store, double now)
	{
		// Ahead of the game: what is there already is stepped over, not read again.
		while (m_ahead_left > 0)
		{
			if (m_ahead_next >= store.Sectors())
			{
				m_ahead_left = 0;
				break;
			}
			const uint32_t run = store.MissingRun(m_ahead_next, std::min(m_ahead_left, m_options.ahead_piece));
			if (run != 0)
				return {Step::Ahead, m_ahead_next, run};
			m_ahead_next += BLOCK_SECTORS;
			m_ahead_left--;
		}

		if (m_options.fill && store.HoldsAll() && !store.Complete() && (now - m_last_request) >= m_options.fill_after)
		{
			const uint32_t lsn = store.NextMissing(m_fill_cursor);
			if (lsn != NONE)
				return {Step::Fill, lsn, std::max<uint32_t>(store.MissingRun(lsn, m_options.fill_piece), 1)};
		}

		// Nothing to read: now and then a block all the same, or the drive stops turning and the
		// next read waits for it to start again. Not once the whole disc is held.
		if (m_options.keep_spinning && m_read_anything && m_last_lsn != NONE && !store.Complete() && (now - m_last_read) >= m_options.keep_alive)
			return {Step::KeepAlive, m_last_lsn, 1};

		return {};
	}

	// A step was carried out (`ok`: the drive gave the blocks).
	void Done(const Step& step, bool ok, double now)
	{
		DriveRead(now);
		switch (step.kind)
		{
			case Step::Ahead:
				if (!ok)
				{
					m_ahead_left = 0; // (a stretch that does not read: not hammered on)
					break;
				}
				m_ahead_next = step.lsn + step.blocks * BLOCK_SECTORS;
				m_ahead_left -= std::min(m_ahead_left, step.blocks);
				break;
			case Step::Fill:
				m_fill_cursor = step.lsn + step.blocks * BLOCK_SECTORS;
				break;
			default:
				break;
		}
	}

private:
	Options m_options;
	uint32_t m_ahead_next = 0, m_ahead_left = 0, m_fill_cursor = 0, m_last_lsn = NONE;
	double m_last_request = -1e9, m_last_read = -1e9;
	bool m_read_anything = false;
};

// What the drive and the game did, for a line in the log now and then. Any thread adds; the
// reading thread takes.
struct Stats
{
	std::mutex mutex;
	uint64_t reads = 0, bytes = 0;
	double read_seconds = 0, slowest_read = 0;
	uint64_t waits = 0;             // times the game had to wait for the drive itself
	double wait_seconds = 0, longest_wait = 0;

	void Read(uint64_t n, double seconds)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		reads++;
		bytes += n;
		read_seconds += seconds;
		slowest_read = std::max(slowest_read, seconds);
	}
	void Waited(double seconds)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		waits++;
		wait_seconds += seconds;
		longest_wait = std::max(longest_wait, seconds);
	}
	struct Taken
	{
		uint64_t reads, bytes;
		double read_seconds, slowest_read;
		uint64_t waits;
		double wait_seconds, longest_wait;
	};
	Taken Take()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		const Taken out{reads, bytes, read_seconds, slowest_read, waits, wait_seconds, longest_wait};
		reads = bytes = waits = 0;
		read_seconds = slowest_read = wait_seconds = longest_wait = 0;
		return out;
	}
};

} // namespace PS5DiscCache
