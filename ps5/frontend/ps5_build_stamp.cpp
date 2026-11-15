// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - which build this is (tools/build.sh writes both strings before each build),
// for the first lines of the boot log: a log then says which eboot.bin wrote it, and which ELF
// its crash addresses belong to.

#include "ps5_platform.h"
#include "ps5_update.h"

#include "ps5_build_stamp.inc"

// A build made before build.sh wrote the release switch: a developer build.
#ifndef PS5PX_BUILD_RELEASE
#define PS5PX_BUILD_RELEASE 0
#endif

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

extern "C" const char* ps5px_build_number(void)
{
#ifdef PS5PX_BUILD_NUMBER
  return PS5PX_BUILD_NUMBER;
#else
  return "0";
#endif
}

// Whether this build was made as a release (tools/build.sh: PS5PX_RELEASE=1 from a clean tree),
// and its version as shown: "v1.0.18", or "v1.0.18 - developer build" for any other build.
bool PS5Update::RunningIsRelease()
{
  return PS5PX_BUILD_RELEASE != 0;
}

std::string PS5Update::RunningVersionLabel()
{
  return VersionLabel(PS5PX_BUILD_VERSION, PS5PX_BUILD_RELEASE != 0);
}
