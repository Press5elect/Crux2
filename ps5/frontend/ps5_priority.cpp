// Crux2 - which of the title's threads the console runs first: the runtime of ps5_priority.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_priority.h"
#include "ps5_keyboard.h" // PS5SystemModules

#include "Host.h"

#include <atomic>
#include <pthread.h>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5Priority
{
namespace
{
using GetFn = int (*)(pthread_t thread, int* priority);
using SetFn = int (*)(pthread_t thread, int priority);

struct Functions
{
	GetFn get = nullptr;
	SetFn set = nullptr;
};

const Functions& Find()
{
	static const Functions functions = [] {
		Functions f;
		f.get = reinterpret_cast<GetFn>(PS5SystemModules::FindFunction("libkernel", "scePthreadGetprio", "1tKyG7RlMJo"));
		f.set = reinterpret_cast<SetFn>(PS5SystemModules::FindFunction("libkernel", "scePthreadSetprio", "W0Hpm2X0uPE"));
		ps5px_log("priority: scePthreadSetprio %s", f.set ? "found" : "NOT found: every thread keeps the title's priority");
		return f;
	}();
	return functions;
}

int PriorityOf(Role role)
{
	switch (role)
	{
		case Role::Sound: return 690;
		case Role::Input: return 692;
		case Role::Interface: return 695;
		case Role::Emulation: return 700;
		case Role::Background: return 730;
	}
	return 700;
}

const char* NameOf(Role role)
{
	switch (role)
	{
		case Role::Sound: return "sound";
		case Role::Input: return "controllers";
		case Role::Interface: return "interface";
		case Role::Emulation: return "emulation";
		case Role::Background: return "background job";
	}
	return "?";
}

std::atomic<unsigned> s_said{0};
} // namespace

void SetCurrentThread(Role role)
{
	if (!Host::GetBaseBoolSettingValue("PS5", "ThreadPriorities", false))
		return;
	const Functions& f = Find();
	if (!f.set)
		return;
	const int wanted = PriorityOf(role);
	int before = -1;
	if (f.get)
		f.get(pthread_self(), &before);
	const int result = f.set(pthread_self(), wanted);
	const unsigned bit = 1u << static_cast<unsigned>(role);
	if ((s_said.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
	{
		int after = -1;
		if (f.get)
			f.get(pthread_self(), &after);
		ps5px_log("priority: a %s thread from %d to %d: %#x (now %d)", NameOf(role), before, wanted,
			static_cast<unsigned>(result), after);
	}
}
} // namespace PS5Priority
