// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - which build this is (tools/build.sh writes both strings before each build),
// for the first lines of the boot log: a log then says which eboot.bin wrote it, and which ELF
// its crash addresses belong to.

#include "ps5_platform.h"

#include "ps5_build_stamp.inc"

extern "C" const char* ps5px_build_stamp(void)
{
  return PS5PX_BUILD_STAMP;
}

extern "C" const char* ps5px_build_identity(void)
{
  return PS5PX_BUILD_IDENTITY;
}

extern "C" const char* ps5px_build_version(void)
{
  return PS5PX_BUILD_VERSION;
}
