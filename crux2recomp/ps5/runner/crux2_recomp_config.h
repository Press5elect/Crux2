// Crux2 Recomp - the settings PS2Recomp's runtime and every game module are compiled with.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Included first in the runtime's files here and in a game's module (made on the player's PC by
// recomp-runner-module.sh): both sides of the boundary must see the same classes, so both see the
// same switches.
#pragma once

#ifndef PS2_RUNTIME_LOGS
#define PS2_RUNTIME_LOGS 0
#endif
#ifndef AGRESSIVE_LOGS
#define AGRESSIVE_LOGS 0
#endif
// (PS2_FUNCTION_LOG_TRACKER stays undefined: the generated code tests whether it is defined.)
#undef PS2_FUNCTION_LOG_TRACKER
#ifndef PS2X_ENABLE_IOP_RPC_TRACE
#define PS2X_ENABLE_IOP_RPC_TRACE 0
#endif
#ifndef PS2X_HAS_FFMPEG
#define PS2X_HAS_FFMPEG 0
#endif
// The module's interface version: a module made for another one is refused by the title.
#define CRUX2_RECOMP_MODULE_ABI 2
