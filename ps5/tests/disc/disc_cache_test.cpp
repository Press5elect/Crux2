// Crux2 - the disc held in memory as it is read (frontend/ps5_disc_cache.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The store and the planner over a made-up drive and a made-up clock: what is kept is what was
// read, memory stays inside its budget, the game's reads come first, the rest of the disc follows
// from where the game is, and the drive is kept turning only while it may be needed. At the end,
// a model of a game on a slow drive, read PCSX2's way and this way: a model, not the console.

#include "ps5_disc_cache.h"

#include <cstdio>
#include <deque>
#include <random>
#include <vector>

using namespace PS5DiscCache;

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

static constexpr uint32_t BLOCK = 2048 * BLOCK_SECTORS;

// The block at `lsn` of the made-up disc: every byte follows from the sector it is in.
static void Fill(uint32_t lsn, uint8_t* out)
{
	for (uint32_t i = 0; i < BLOCK; i++)
		out[i] = static_cast<uint8_t>((lsn + i / 2048) * 131u + i * 7u);
}

static bool Same(uint32_t lsn, const uint8_t* data)
{
	std::vector<uint8_t> want(BLOCK);
	Fill(lsn, want.data());
	return std::memcmp(want.data(), data, BLOCK) == 0;
}

static void TestStore()
{
	Store store;
	std::vector<uint8_t> block(BLOCK), back(BLOCK);
	CHECK(!store.Has(0) && !store.Fetch(0, back.data()) && store.NextMissing(0) == NONE && !store.Complete());

	// A disc of 3 pieces and a bit, the last block a short one; room for all of it.
	const uint32_t sectors = (3 * CHUNK_BLOCKS + 100) * BLOCK_SECTORS + 5;
	store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 4);
	CHECK(store.Blocks() == 3 * CHUNK_BLOCKS + 101);
	CHECK(store.HoldsAll());
	CHECK(store.Count().bytes == 0 && store.Count().held == 0);
	CHECK(store.NextMissing(0) == 0);
	CHECK(store.MissingRun(0, 8) == 8);
	CHECK(store.MissingRun((store.Blocks() - 2) * BLOCK_SECTORS, 8) == 2);
	CHECK(store.MissingRun(store.Blocks() * BLOCK_SECTORS, 8) == 0);

	for (const uint32_t at : {0u, 16u, 32u, 1023u * 16u, 1024u * 16u, 2500u * 16u, (store.Blocks() - 1) * 16u})
	{
		Fill(at, block.data());
		CHECK(store.Put(at, block.data()));
		CHECK(store.Has(at) && store.Fetch(at, back.data()) && Same(at, back.data()));
	}
	CHECK(store.Count().held == 7);
	CHECK(store.Count().bytes == 4ull * BLOCK * CHUNK_BLOCKS); // four pieces were made
	CHECK(!store.Has(48) && !store.Fetch(48, back.data()));
	CHECK(!store.Put(store.Blocks() * BLOCK_SECTORS, block.data())); // past the end
	CHECK(store.NextMissing(0) == 48);
	CHECK(store.NextMissing(1023 * 16) == 1025 * 16);
	CHECK(store.MissingRun(0, 8) == 0 && store.MissingRun(48, 2000) == 1020);
	// From the last block on: round to the start.
	CHECK(store.NextMissing((store.Blocks() - 1) * 16) == 48);
	// A sector inside a block names its block.
	CHECK(store.Has(17) && store.Has(31) && !store.Has(48 + 3));

	// A block that does not read is stepped over, and comes back when it reads after all.
	store.MarkBad(48);
	CHECK(store.NextMissing(0) == 64 && store.Count().bad == 1 && store.MissingRun(48, 4) == 0);
	Fill(48, block.data());
	CHECK(store.Put(48, block.data()) && store.Count().bad == 0 && store.Has(48));

	// Every block: complete, and whole pieces are stepped over when looking for a missing one.
	for (uint32_t b = 0; b < store.Blocks(); b++)
	{
		Fill(b * 16, block.data());
		CHECK(store.Put(b * 16, block.data()));
	}
	CHECK(store.Complete() && store.NextMissing(0) == NONE && store.NextMissing(5000 * 16) == NONE);
	CHECK(store.Count().held == store.Blocks() && store.Count().evicted == 0);
	std::mt19937 random(1);
	for (int i = 0; i < 300; i++)
	{
		const uint32_t at = (random() % store.Blocks()) * 16;
		CHECK(store.Fetch(at, back.data()) && Same(at, back.data()));
	}

	// Room for two pieces only: the piece used longest ago goes, what is kept is still right.
	store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 2);
	CHECK(!store.HoldsAll() && store.Count().held == 0);
	const auto put = [&](uint32_t b) {
		Fill(b * 16, block.data());
		return store.Put(b * 16, block.data());
	};
	CHECK(put(5) && put(1024 + 5));        // pieces 0 and 1
	CHECK(store.Fetch(5 * 16, back.data())); // piece 0 used after piece 1
	CHECK(put(2048 + 5));                  // piece 2 comes: piece 1 goes
	CHECK(store.Has(5 * 16) && !store.Has((1024 + 5) * 16) && store.Has((2048 + 5) * 16));
	CHECK(store.Count().evicted == 1 && store.Count().held == 2);
	CHECK(store.Count().bytes == 2ull * BLOCK * CHUNK_BLOCKS);
	CHECK(put(3072 + 5));                  // piece 3: piece 0 goes (2 was used later)
	CHECK(!store.Has(5 * 16) && store.Has((2048 + 5) * 16) && store.Has((3072 + 5) * 16));
	CHECK(store.Fetch((3072 + 5) * 16, back.data()) && Same((3072 + 5) * 16, back.data()));
	CHECK(!store.Complete());

	// A budget of nothing still keeps one piece.
	store.Configure(sectors, BLOCK, 0);
	CHECK(put(7) && store.Has(7 * 16) && put(1024 + 7) && !store.Has(7 * 16) && store.Count().held == 1);

	store.Configure(0, 0, 0);
	CHECK(store.Count().bytes == 0 && store.Blocks() == 0);
}

// The reading thread's loop, over the made-up disc: carries out one step and says what it was.
struct Drive
{
	Store store;
	Planner planner;
	double now = 0;
	std::vector<Planner::Step> log;
	uint32_t bad_from = NONE, bad_to = 0; // sectors that do not read

	bool Reads(uint32_t lsn, uint32_t count) const
	{
		for (uint32_t i = 0; i < count; i++)
		{
			const uint32_t at = lsn + i * BLOCK_SECTORS;
			if (at >= bad_from && at < bad_to)
				return false;
		}
		return true;
	}

	// As the reading thread does it (vendor CDVD/CDVDdiscThread.cpp). `only_ahead`: a step of
	// another kind is said, and not carried out.
	Planner::Step Run(bool only_ahead = false)
	{
		const Planner::Step step = planner.Next(store, now);
		if (step.kind == Planner::Step::Nothing || (only_ahead && step.kind != Planner::Step::Ahead))
			return step;
		std::vector<uint8_t> block(BLOCK);
		Planner::Step done = step;
		bool ok = Reads(step.lsn, step.blocks);
		if (!ok && step.kind == Planner::Step::Fill)
		{
			// A piece of the rest of the disc that does not read: its first block alone, which is
			// kept or given up; the others are come back to.
			done.blocks = 1;
			ok = Reads(step.lsn, 1);
			if (!ok)
				store.MarkBad(step.lsn);
		}
		if (ok && step.kind != Planner::Step::KeepAlive)
		{
			for (uint32_t i = 0; i < done.blocks; i++)
			{
				Fill(step.lsn + i * BLOCK_SECTORS, block.data());
				store.Put(step.lsn + i * BLOCK_SECTORS, block.data());
			}
		}
		now += 0.02;
		planner.Done(done, ok, now);
		log.push_back(step);
		return step;
	}

	void Request(uint32_t lsn)
	{
		if (!store.Has(lsn))
		{
			std::vector<uint8_t> block(BLOCK);
			Fill(lsn, block.data());
			store.Put(lsn, block.data());
			planner.DriveRead(now);
		}
		planner.Requested(lsn, now);
	}
};

static void TestPlanner()
{
	const uint32_t blocks = 2 * CHUNK_BLOCKS + 300;
	const uint32_t sectors = blocks * BLOCK_SECTORS;

	// ---- the whole disc fits
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		d.planner.Reset({});
		// Before the game has read anything: nothing to do, the drive is left alone... except
		// that the rest of the disc may be read, from its start.
		CHECK(d.planner.Next(d.store, 0).kind == Planner::Step::Fill);

		d.now = 10;
		d.Request(1000 * 16);
		// Ahead of the game first: 256 blocks in pieces of 4, in order, nothing else in between.
		uint32_t next = 1001 * 16, ahead = 0;
		for (;;)
		{
			const Planner::Step step = d.Run(true);
			if (step.kind != Planner::Step::Ahead)
				break;
			CHECK(step.lsn == next && step.blocks >= 1 && step.blocks <= 4);
			next += step.blocks * 16;
			ahead += step.blocks;
		}
		CHECK(ahead == 256);
		for (uint32_t b = 1000; b <= 1256; b++)
			CHECK(d.store.Has(b * 16));
		CHECK(!d.store.Has(1257 * 16));

		// The game reads on, into what is there: only the new end of the window is read.
		d.log.clear();
		d.Request(1100 * 16);
		while (d.Run(true).kind == Planner::Step::Ahead)
		{
		}
		uint32_t read = 0;
		for (const Planner::Step& step : d.log)
		{
			if (step.kind == Planner::Step::Ahead)
			{
				CHECK(step.lsn >= 1257 * 16);
				read += step.blocks;
			}
		}
		CHECK(read == 100);

		// The rest of the disc: not before what is ahead of the game...
		d.log.clear();
		d.Request(1200 * 16);
		const double asked = d.now;
		CHECK(d.planner.Next(d.store, asked + 0.1).kind == Planner::Step::Ahead);
		// ...then on from there to the end, round to the start, until all is held.
		d.now = asked + 1.0;
		d.log.clear();
		int steps = 0;
		while (!d.store.Complete() && steps++ < 100000)
		{
			const Planner::Step step = d.Run();
			CHECK(step.kind == Planner::Step::Fill || step.kind == Planner::Step::Ahead);
			CHECK(step.blocks >= 1 && step.blocks <= 4);
		}
		CHECK(d.store.Complete());
		bool first_fill = true;
		for (const Planner::Step& step : d.log)
		{
			if (step.kind == Planner::Step::Fill && first_fill)
			{
				CHECK(step.lsn == 1457 * 16); // right after what was read ahead of the game: no move of the head
				first_fill = false;
			}
			else if (step.kind == Planner::Step::Ahead)
			{
				CHECK(first_fill); // and nothing ahead of the game after the filling began
			}
		}
		CHECK(!first_fill);
		// Every block is what the disc has there.
		std::vector<uint8_t> back(BLOCK);
		for (uint32_t b = 0; b < blocks; b += 7)
			CHECK(d.store.Fetch(b * 16, back.data()) && Same(b * 16, back.data()));
		// All held: the drive is left alone for good, however long nothing is read.
		d.now += 1000;
		CHECK(d.planner.Next(d.store, d.now).kind == Planner::Step::Nothing);
		d.Request(5 * 16);
		CHECK(d.planner.Next(d.store, d.now + 100).kind == Planner::Step::Nothing);
	}

	// ---- the game interrupts the filling: its block and what is ahead of it come first
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		d.planner.Reset({});
		d.now = 5;
		d.Request(0);
		while (d.Run(true).kind == Planner::Step::Ahead)
		{
		}
		for (int i = 0; i < 20; i++)
			CHECK(d.Run().kind == Planner::Step::Fill);
		d.Request(2000 * 16);
		const Planner::Step step = d.Run();
		CHECK(step.kind == Planner::Step::Ahead && step.lsn == 2001 * 16);
	}

	// ---- told to wait for the game to be quiet: no filling until it has been
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		Planner::Options options;
		options.fill_after = 0.4;
		d.planner.Reset(options);
		d.now = 5;
		d.Request(0);
		while (d.Run(true).kind == Planner::Step::Ahead)
		{
		}
		d.Request(0); // (again: nothing is left to read ahead of it, and the game was just here)
		CHECK(d.planner.Next(d.store, d.now + 0.1).kind == Planner::Step::Nothing);
		CHECK(d.planner.Next(d.store, d.now + 0.5).kind == Planner::Step::Fill);
	}

	// ---- a stretch of the disc that does not read: given up block by block, the rest is read
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		d.planner.Reset({});
		d.bad_from = 500 * 16;
		d.bad_to = 520 * 16;
		d.now = 1;
		d.Request(480 * 16);
		// (Ahead of the game the bad stretch stops the reading ahead: it is not hammered on.)
		int ahead = 0;
		while (d.Run(true).kind == Planner::Step::Ahead)
			ahead++;
		CHECK(ahead >= 4 && ahead <= 6);
		d.now += 1;
		int steps = 0;
		while (!d.store.Complete() && steps++ < 100000)
			d.Run();
		CHECK(d.store.Complete());
		CHECK(d.store.Count().bad == 20 && d.store.Count().held == blocks - 20);
		CHECK(!d.store.Has(505 * 16) && d.store.Has(499 * 16) && d.store.Has(520 * 16));
	}

	// ---- memory for a part of the disc only: no filling, and the drive is kept turning
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS);
		d.planner.Reset({});
		CHECK(d.planner.Next(d.store, 100).kind == Planner::Step::Nothing); // nothing read yet: the drive is not woken
		d.now = 200;
		d.Request(100 * 16);
		while (d.Run().kind == Planner::Step::Ahead)
		{
		}
		const double quiet = d.now;
		CHECK(d.planner.Next(d.store, quiet + 5).kind == Planner::Step::Nothing);
		CHECK(d.planner.Next(d.store, quiet + 19).kind == Planner::Step::Nothing);
		d.now = quiet + 21;
		const Planner::Step alive = d.Run();
		CHECK(alive.kind == Planner::Step::KeepAlive && alive.lsn == 100 * 16 && alive.blocks == 1);
		CHECK(d.planner.Next(d.store, d.now + 5).kind == Planner::Step::Nothing);
		d.now += 21;
		CHECK(d.Run().kind == Planner::Step::KeepAlive);

		// With that switched off, never.
		Planner::Options off;
		off.keep_spinning = false;
		d.planner.Reset(off);
		d.now += 1;
		d.Request(100 * 16);
		while (d.Run().kind == Planner::Step::Ahead)
		{
		}
		CHECK(d.planner.Next(d.store, d.now + 500).kind == Planner::Step::Nothing);
	}

	// ---- "Read the disc into memory" off: far ahead still, no filling
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		Planner::Options options;
		options.fill = false;
		d.planner.Reset(options);
		d.now = 1;
		d.Request(0);
		uint32_t ahead = 0;
		for (;;)
		{
			const Planner::Step step = d.Run();
			if (step.kind != Planner::Step::Ahead)
			{
				CHECK(step.kind == Planner::Step::Nothing);
				break;
			}
			ahead += step.blocks;
		}
		CHECK(ahead == 256);
		CHECK(d.planner.Next(d.store, d.now + 10).kind == Planner::Step::Nothing);
	}

	// ---- the end of the disc
	{
		Drive d;
		d.store.Configure(sectors, BLOCK, static_cast<uint64_t>(BLOCK) * CHUNK_BLOCKS * 3);
		Planner::Options options;
		options.fill = false;
		d.planner.Reset(options);
		d.now = 1;
		d.Request((blocks - 3) * 16);
		uint32_t ahead = 0;
		while (true)
		{
			const Planner::Step step = d.Run();
			if (step.kind != Planner::Step::Ahead)
				break;
			CHECK(step.lsn + step.blocks * 16 <= sectors);
			ahead += step.blocks;
		}
		CHECK(ahead == 2);
	}
}

// ------------------------------------------------------------------------------------ a model
//
// A drive that takes a while to move to another place and reads so many MiB a second, and a game
// that plays a stream (a block every 16 ms) while going to two other places for a file of 2 MiB
// each, three rounds. Counted: how long the game stands still because a block it needs is not in
// memory yet. Read PCSX2's way (16 blocks ahead of the last request) and this way, on a quick
// drive and on a slow one. It says what the planner is for; it is not the console's drive, whose
// numbers nobody has measured yet.
struct Model
{
	double SEEK = 0.120, PER_BLOCK = 32768.0 / (8.0 * 1024 * 1024);
	double drive_free_at = 0; // when the drive is done with what it is doing
	uint32_t head = NONE;     // the block after the last one read

	// The drive reads `blocks` from `block`, starting no earlier than `from`; returns when it is done.
	double Read(uint32_t block, uint32_t blocks, double from)
	{
		double t = std::max(from, drive_free_at);
		if (head != block)
			t += SEEK;
		t += blocks * PER_BLOCK;
		head = block + blocks;
		drive_free_at = t;
		return t;
	}
};

static void RunModel(bool ours, double seek, double mib_per_second, double* stalled, int* stalls)
{
	const uint32_t blocks = 140000; // a 4.3 GiB disc
	Store store;
	store.Configure(blocks * BLOCK_SECTORS, BLOCK, static_cast<uint64_t>(BLOCK) * (blocks + CHUNK_BLOCKS));
	Planner planner;
	planner.Reset({});
	Model model;
	model.SEEK = seek;
	model.PER_BLOCK = 32768.0 / (mib_per_second * 1024 * 1024);
	std::vector<uint8_t> data(BLOCK);
	std::vector<double> ready(blocks, 1e18); // when each block is in memory
	uint32_t old_next = 0, old_left = 0;     // PCSX2's way: the blocks after the last request
	double now = 0, planned_at = 0;
	*stalled = 0;
	*stalls = 0;

	// What the reading thread starts before the moment `until`.
	const auto background = [&](double until) {
		for (;;)
		{
			const double start = std::max(planned_at, model.drive_free_at);
			if (start >= until)
				return;
			uint32_t block, count;
			Planner::Step step;
			if (ours)
			{
				step = planner.Next(store, start);
				if (step.kind == Planner::Step::Nothing || step.kind == Planner::Step::KeepAlive)
					return;
				block = step.lsn / 16;
				count = step.blocks;
			}
			else
			{
				while (old_left > 0 && old_next < blocks && ready[old_next] < 1e17)
				{
					old_next++;
					old_left--;
				}
				if (old_left == 0 || old_next >= blocks)
					return;
				block = old_next;
				count = 1;
			}
			const double done = model.Read(block, count, start);
			for (uint32_t i = 0; i < count; i++)
			{
				ready[block + i] = done;
				store.Put((block + i) * 16, data.data());
			}
			if (ours)
				planner.Done(step, true, done);
		}
	};
	// The game needs a block now.
	const auto need = [&](uint32_t block) {
		background(now);
		if (ready[block] > 1e17)
		{
			// Not read and not on its way: the drive fetches it as soon as it is free.
			ready[block] = model.Read(block, 1, now);
			store.Put(block * 16, data.data());
		}
		if (ready[block] > now)
		{
			*stalled += ready[block] - now;
			(*stalls)++;
			now = ready[block];
		}
		planned_at = now;
		if (ours)
			planner.Requested(block * 16, now);
		else
		{
			old_next = block + 1;
			old_left = 16;
		}
	};

	const uint32_t stream = 20000, file_a = 90000, file_b = 60000;
	uint32_t at = stream;
	for (int round = 0; round < 3; round++)
	{
		for (int i = 0; i < 600; i++) // ten seconds of the stream
		{
			need(at++);
			now += 0.016;
		}
		for (const uint32_t file : {file_a, file_b})
		{
			for (uint32_t i = 0; i < 64; i++) // 2 MiB, as fast as the game takes it
			{
				need(file + i);
				now += 0.002;
			}
		}
	}
}

int main()
{
	TestStore();
	TestPlanner();

	struct Profile
	{
		const char* name;
		double seek, rate;
	};
	double worst_gain = 0;
	for (const Profile& profile : {Profile{"a quick drive (120 ms to move, 8 MiB/s)", 0.120, 8.0}, Profile{"a slow drive (350 ms to move, 4 MiB/s)", 0.350, 4.0},
			 Profile{"a drive slow to move (600 ms, 8 MiB/s)", 0.600, 8.0}})
	{
		double old_stalled, new_stalled;
		int old_stalls, new_stalls;
		RunModel(false, profile.seek, profile.rate, &old_stalled, &old_stalls);
		RunModel(true, profile.seek, profile.rate, &new_stalled, &new_stalls);
		std::fprintf(stderr, "  a model of 31 s of a game on %s:\n", profile.name);
		std::fprintf(stderr, "    read PCSX2's way: the game stands still %3d times, %.2f s in all\n", old_stalls, old_stalled);
		std::fprintf(stderr, "    read this way:    the game stands still %3d times, %.2f s in all\n", new_stalls, new_stalled);
		// Never much worse, and fewer stops.
		CHECK(new_stalled <= old_stalled + 0.1);
		CHECK(new_stalls <= old_stalls);
		worst_gain = std::max(worst_gain, old_stalled - new_stalled);
	}
	// And where the drive is slow to move, clearly better.
	CHECK(worst_gain > 0.5);

	std::fprintf(stderr, "disc cache: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
