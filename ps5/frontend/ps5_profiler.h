// Crux2 - the profiler's registration of generated code (ps5_profiler.cpp).
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/// Names a range of generated code, [lo, hi), for the profiler's report: the samples outside the program
/// that fall in it are counted under `name` ("EE recompiler", "IOP recompiler", "VU0", "VU1", "VIF unpack").
/// `name` must stay valid (a literal). Call once the code reserve is mapped, from any thread; the table
/// is read only when the sampler folds a window, never in the signal handler. Does nothing for a range
/// that is empty, or when the table (16 entries) is full; a name registered again replaces its range.
void ps5px_profile_register_code_range(const char* name, uint64_t lo, uint64_t hi);

#ifdef __cplusplus
}
#endif
