// Crux2 - the processor time of the emulator's threads, as each thread reports it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2's performance overlay shows how busy the emulation, graphics and VU threads are, by
// reading each thread's processor time from another thread. Whether a title on the console may
// read another thread's clock has not been measured, and a call the console lacks stops the
// title. What is certain is a thread reading its own clock. So each thread notes its own time
// here (the emulation thread once a frame, the VU thread each time it wakes, and any thread
// whenever it is asked for its own), and a reader gets the last time a thread noted.
//
// A handle's time never goes back, whoever reads it: a reader on the thread itself notes the
// live time before returning it, so a later reader on another thread sees at least that. (The
// overlay subtracts two readings made on different threads: a reading that went back would wrap.)
//
// No PCSX2 types here, so the PC-side test (ps5/tests/thread_times_test.cpp) builds it alone.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class PS5ThreadTimes
{
public:
	/// More than the emulator ever runs (emulation, graphics, VU, software renderer workers).
	static constexpr size_t SLOTS = 64;

	/// The thread `handle` notes its own time (microseconds). Called by that thread.
	void Publish(uintptr_t handle, uint64_t time)
	{
		if (Slot* const slot = Find(handle, true))
			slot->time.store(time, std::memory_order_release);
	}

	/// The last time the thread noted; 0 for a thread that never did.
	uint64_t Read(uintptr_t handle)
	{
		const Slot* const slot = Find(handle, false);
		return slot ? slot->time.load(std::memory_order_acquire) : 0;
	}

	/// The thread is ending: its handle may be given to a new thread, which starts from 0.
	void Forget(uintptr_t handle)
	{
		if (Slot* const slot = Find(handle, false))
		{
			slot->time.store(0, std::memory_order_release);
			slot->handle.store(0, std::memory_order_release);
		}
	}

private:
	struct Slot
	{
		std::atomic<uintptr_t> handle{0};
		std::atomic<uint64_t> time{0};
	};

	Slot* Find(uintptr_t handle, bool create)
	{
		if (handle == 0)
			return nullptr;

		for (Slot& slot : m_slots)
		{
			if (slot.handle.load(std::memory_order_acquire) == handle)
				return &slot;
		}
		if (!create)
			return nullptr;

		// A handle is only ever added by its own thread, so no two threads add the same one.
		for (Slot& slot : m_slots)
		{
			uintptr_t expected = 0;
			if (slot.handle.compare_exchange_strong(expected, handle, std::memory_order_acq_rel))
			{
				slot.time.store(0, std::memory_order_release);
				return &slot;
			}
		}
		return nullptr; // every slot taken: this thread's time reads 0
	}

	Slot m_slots[SLOTS];
};
