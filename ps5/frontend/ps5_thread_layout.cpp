// Crux2 - where the emulator's threads run ([PS5] ThreadLayout): the runtime of ps5_thread_layout.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_thread_layout.h"
#include "ps5_platform.h"

#include "Host.h"

#include <cerrno>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/cpuset.h>

namespace PS5ThreadLayout
{
	bool Wanted()
	{
		return Host::GetBaseBoolSettingValue("PS5", "ThreadLayout", true);
	}

	namespace
	{
		// The set at the size the console's kernel keeps (a set of sizeof(cpuset_t) can be refused
		// with ERANGE; the first 64 bits hold every processor there is). 0, or the error.
		int SetAffinity(cpuwhich_t which, uint64_t mask)
		{
			cpuset_t set;
			CPU_ZERO(&set);
			for (int c = 0; c < 64; c++)
				if ((mask >> c) & 1u)
					CPU_SET(c, &set);
			int error = ERANGE;
			for (const size_t size : {size_t{8}, size_t{16}, sizeof(set)})
			{
				error = (cpuset_setaffinity(CPU_LEVEL_WHICH, which, -1, size, &set) == 0) ? 0 : errno;
				if (error != ERANGE)
					break;
			}
			return error;
		}
	} // namespace

	bool PinCallingThread(uint64_t mask, const char* what)
	{
		mask &= kTitleMask;
		if (mask == 0)
			return false;
		const int error = SetAffinity(CPU_WHICH_TID, mask);
		if (error != 0)
			ps5px_log("threads: %s could not be kept to processors 0x%llx (errno %d)", what, static_cast<unsigned long long>(mask), error);
		else
			ps5px_log("threads: %s on processors 0x%llx", what, static_cast<unsigned long long>(mask));
		return error == 0;
	}

	bool RestrictProcess(uint64_t mask)
	{
		mask &= kTitleMask;
		if (mask == 0)
			return false;
		const int error = SetAffinity(CPU_WHICH_PID, mask);
		if (error != 0)
			ps5px_log("threads: the title could not be kept to processors 0x%llx (errno %d)", static_cast<unsigned long long>(mask), error);
		return error == 0;
	}
} // namespace PS5ThreadLayout
