// Crux2 - a memory card file held in memory and written back behind the game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2 serves a PlayStation 2 memory card straight from its file: for every 528-byte page a game
// writes it seeks, reads the page back, seeks again and writes it, all on the emulation thread.
// On a PC those are a few microseconds each. On the console they are not: each seek throws away
// the stream's buffer, each read fills it again from the title folder, and a game's save, which is
// thousands of such pages, ran at about 5 frames a second on the first console it was tried on.
//
// So on the console the card's file is read once, when the card is opened, and the game reads
// and writes this copy. What changed is written to the file in whole 64 KiB pieces once the game
// has left the card alone for half a second, and every two seconds while it keeps writing
// (PS5CardImage::Tick, which the host calls once a frame), when the game is paused, and when the
// card is closed. So a title that is closed from the console's menu, or that stops on an error,
// loses at most the last two seconds of a save. A file larger than MAX_SIZE is not loaded, and
// the caller then serves it from the file as before.
//
// No PCSX2 types here, so the PC-side test (ps5/tests/card_image_test.cpp) builds it alone.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

class PS5CardImage
{
public:
	/// What is written back at a time. A save touches the card's allocation table and a run of
	/// clusters: a few pieces, each one write.
	static constexpr size_t CHUNK_SIZE = 64 * 1024;

	/// The largest file held in memory (PCSX2's own cards are 8 to 64 MB; converted ones can be more).
	static constexpr int64_t MAX_SIZE = 256ll * 1024 * 1024;

	/// Reads the whole file. False when it is not held in memory (too large, empty, or a read
	/// failed): nothing is loaded then.
	bool Load(std::FILE* fp, int64_t size)
	{
		Unload();
		if (!fp || size <= 0 || size > MAX_SIZE)
			return false;

		m_data.resize(static_cast<size_t>(size));
		if (fseeko(fp, 0, SEEK_SET) != 0 || fread(m_data.data(), m_data.size(), 1, fp) != 1)
		{
			Unload();
			return false;
		}

		m_dirty.assign((m_data.size() + CHUNK_SIZE - 1) / CHUNK_SIZE, 0);
		return true;
	}

	void Unload()
	{
		std::vector<uint8_t>().swap(m_data);
		std::vector<uint8_t>().swap(m_dirty);
		m_any_dirty = false;
		m_idle_frames = 0;
		m_dirty_frames = 0;
	}

	bool IsLoaded() const { return !m_data.empty(); }
	bool IsDirty() const { return m_any_dirty; }
	size_t GetSize() const { return m_data.size(); }

	/// Whether [address, address + size) is inside the card: what a read of the file would have
	/// to stay within.
	bool InRange(uint32_t address, size_t size) const
	{
		return (size <= m_data.size() && static_cast<size_t>(address) <= m_data.size() - size);
	}

	/// The card's bytes at address (InRange has been asked).
	const uint8_t* Data(uint32_t address) const { return m_data.data() + address; }

	void Read(uint8_t* destination, uint32_t address, size_t size) const
	{
		std::memcpy(destination, m_data.data() + address, size);
	}

	void Write(const uint8_t* source, uint32_t address, size_t size)
	{
		if (size == 0)
			return;
		std::memcpy(m_data.data() + address, source, size);
		MarkDirty(address, size);
	}

	void Fill(uint8_t value, uint32_t address, size_t size)
	{
		if (size == 0)
			return;
		std::memset(m_data.data() + address, value, size);
		MarkDirty(address, size);
	}

	/// Once a frame. True when it is time to Flush(): the card has changes, and either the game
	/// has not written to it for idle_frames frames, or the oldest change is max_dirty_frames
	/// frames old (a game that keeps touching its card must not keep it from its file).
	bool Tick(unsigned idle_frames, unsigned max_dirty_frames)
	{
		if (!m_any_dirty)
			return false;
		if (m_idle_frames < idle_frames)
			m_idle_frames++;
		if (m_dirty_frames < max_dirty_frames)
			m_dirty_frames++;
		return (m_idle_frames >= idle_frames || m_dirty_frames >= max_dirty_frames);
	}

	/// Writes every changed piece to the file, neighbouring pieces in one write, and has the
	/// stream hand them to the system. True when the file has everything. On a failure every piece
	/// stays marked (a write that was repeated does no harm), and the next Flush() tries again.
	bool Flush(std::FILE* fp)
	{
		if (!m_any_dirty)
			return true;

		// The counts start again either way, so a card whose file cannot be written is tried
		// again in half a second and not on every frame.
		m_idle_frames = 0;
		m_dirty_frames = 0;
		if (!fp)
			return false;

		for (size_t chunk = 0; chunk < m_dirty.size();)
		{
			if (!m_dirty[chunk])
			{
				chunk++;
				continue;
			}

			size_t end = chunk;
			while (end < m_dirty.size() && m_dirty[end])
				end++;

			const size_t offset = chunk * CHUNK_SIZE;
			const size_t length = ((end * CHUNK_SIZE < m_data.size()) ? end * CHUNK_SIZE : m_data.size()) - offset;
			if (fseeko(fp, static_cast<off_t>(offset), SEEK_SET) != 0 || fwrite(m_data.data() + offset, length, 1, fp) != 1)
				return false;
			chunk = end;
		}

		if (fflush(fp) != 0)
			return false;

		std::memset(m_dirty.data(), 0, m_dirty.size());
		m_any_dirty = false;
		return true;
	}

private:
	void MarkDirty(uint32_t address, size_t size)
	{
		const size_t first = address / CHUNK_SIZE;
		const size_t last = (static_cast<size_t>(address) + size - 1) / CHUNK_SIZE;
		for (size_t chunk = first; chunk <= last; chunk++)
			m_dirty[chunk] = 1;
		m_any_dirty = true;
		m_idle_frames = 0;
	}

	std::vector<uint8_t> m_data;
	std::vector<uint8_t> m_dirty; // one flag for each CHUNK_SIZE piece
	bool m_any_dirty = false;
	unsigned m_idle_frames = 0; // frames since the last write
	unsigned m_dirty_frames = 0; // frames since the first write the file does not have yet
};
