// Crux2 - what a game's start is doing: the runtime of ps5_boot_step.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_boot_step.h"
#include "ps5_platform.h"

#include "common/Timer.h"

#include <mutex>

namespace PS5BootStep
{
	namespace
	{
		std::mutex s_lock;
		Step s_step = Step::None;
		Common::Timer::Value s_since = 0;
		bool s_said_slow = false;
	} // namespace

	void Set(Step s)
	{
		const std::lock_guard lock(s_lock);
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (s_step != Step::None)
		{
			const double ms = Common::Timer::ConvertValueToMilliseconds(now - s_since);
			if (ms >= 500.0)
				ps5px_log("start: %s took %.1f s", Doing(s_step), ms / 1000.0);
		}
		s_step = s;
		s_since = now;
		s_said_slow = false;
	}

	Step Current(double* seconds)
	{
		const std::lock_guard lock(s_lock);
		if (s_step == Step::None)
		{
			*seconds = 0;
			return Step::None;
		}
		*seconds = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_since);
		if (*seconds >= SLOW_SECONDS && !s_said_slow)
		{
			s_said_slow = true;
			ps5px_log("start: still %s after %.0f s", Doing(s_step), *seconds);
		}
		return s_step;
	}
} // namespace PS5BootStep
