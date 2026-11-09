// Crux2 - how long the EE thread waits for the GS and for the VU thread, for the profiler.
//
// Wall-clock time added up at the places where the EE blocks: Begin() when it starts waiting, End() when
// it is done, with the value Begin() gave. The profiler (ps5_profiler.cpp) writes the totals with each
// window as "EE blocked on GS x s, on VU y s". Nothing is counted until the profiler starts
// (ps5px_profile_start turns it on), and with PS5_SYNC_STATS defined as 0 the calls are empty.
//
// Where the calls go (vendor/pcsx2/pcsx2):
//   MTGS.cpp   MTGS::WaitGS, around the else branch's `s_sem_event.WaitForEmpty()` (not the weakWait && isMTVU
//              branch: that is the VU thread waiting, not the EE).
//   MTVU.cpp   VU_Thread::WaitVU, around the semaEvent.WaitForEmpty*() calls; VU_Thread::WaitOnSize, around
//              the loop's KickStart() and yield() (the whole of the wait, not each turn).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#ifndef PS5_SYNC_STATS
#define PS5_SYNC_STATS 1
#endif

namespace PS5SyncStats
{
	enum Kind : unsigned
	{
		GS = 0,
		VU = 1,
		KINDS = 2
	};

#if PS5_SYNC_STATS
	inline std::atomic<bool> g_on{false};
	inline std::atomic<uint64_t> g_ns[KINDS] = {{0}, {0}};

	inline uint64_t NowNs()
	{
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	inline void Enable() { g_on.store(true, std::memory_order_relaxed); }

	/// The start of a wait; 0 when nothing is counted.
	inline uint64_t Begin() { return g_on.load(std::memory_order_relaxed) ? NowNs() : 0; }

	inline void End(Kind kind, uint64_t begun)
	{
		if (begun != 0)
			g_ns[kind].fetch_add(NowNs() - begun, std::memory_order_relaxed);
	}

	/// Seconds waited so far.
	inline double Seconds(Kind kind) { return static_cast<double>(g_ns[kind].load(std::memory_order_relaxed)) / 1e9; }
#else
	inline void Enable() {}
	inline uint64_t Begin() { return 0; }
	inline void End(Kind, uint64_t) {}
	inline double Seconds(Kind) { return 0.0; }
#endif

	/// Begin() and End() around a scope.
	struct Scope
	{
		explicit Scope(Kind k) : kind(k), begun(Begin()) {}
		~Scope() { End(kind, begun); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
		Kind kind;
		uint64_t begun;
	};
} // namespace PS5SyncStats
