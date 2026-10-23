// Crux2 - PS2Recomp's instruction macros, with memory through Crux2's runtime (ps2_runtime.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The arithmetic, MMI and FPU macros are PS2Recomp's own, unchanged
// (upstream/ps2_runtime_macros_upstream.h: ps2xRuntime/include/ps2_runtime_macros.h at the
// pinned revision, GPL-3.0, ran-j and contributors). Its memory macros read and write a flat
// 32 MiB RAM of PS2Recomp's runtime; here every access goes through PCSX2's map instead.

#pragma once

#include "ps2_runtime.h"
#include "upstream/ps2_runtime_macros_upstream.h"

// (Upstream's tracing of guest writes: nothing here.)
#ifndef ps2TraceGuestWrite
#define ps2TraceGuestWrite(...) ((void)0)
#endif

#undef FAST_READ8
#undef FAST_READ16
#undef FAST_READ32
#undef FAST_READ64
#undef FAST_READ128
#undef FAST_WRITE8
#undef FAST_WRITE16
#undef FAST_WRITE32
#undef FAST_WRITE64
#undef FAST_WRITE128
#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128

#define READ8(addr) crux2::Read8((uint32_t)(addr))
#define READ16(addr) crux2::Read16((uint32_t)(addr))
#define READ32(addr) crux2::Read32((uint32_t)(addr))
#define READ64(addr) crux2::Read64((uint32_t)(addr))
#define READ128(addr) crux2::Read128((uint32_t)(addr))
#define WRITE8(addr, val) crux2::Write8((uint32_t)(addr), (uint8_t)(val))
#define WRITE16(addr, val) crux2::Write16((uint32_t)(addr), (uint16_t)(val))
#define WRITE32(addr, val) crux2::Write32((uint32_t)(addr), (uint32_t)(val))
#define WRITE64(addr, val) crux2::Write64((uint32_t)(addr), (uint64_t)(val))
#define WRITE128(addr, val) crux2::Write128((uint32_t)(addr), (val))

// The converter's constant-address forms: the same (a constant address can be a register's).
#define FAST_READ8(addr) READ8(addr)
#define FAST_READ16(addr) READ16(addr)
#define FAST_READ32(addr) READ32(addr)
#define FAST_READ64(addr) READ64(addr)
#define FAST_READ128(addr) READ128(addr)
#define FAST_WRITE8(addr, val) WRITE8(addr, val)
#define FAST_WRITE16(addr, val) WRITE16(addr, val)
#define FAST_WRITE32(addr, val) WRITE32(addr, val)
#define FAST_WRITE64(addr, val) WRITE64(addr, val)
#define FAST_WRITE128(addr, val) WRITE128(addr, val)

// The MMI and FPU instructions: PCSX2's interpreter (CRUX2_EE, emitted by the converter's Crux2 mode).
#include "crux2_ee_interp.h"
