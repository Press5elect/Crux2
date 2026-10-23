// Crux2 - threads (common/Threading.h), in place of common/Linux/LnxThreads.cpp.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/HostSys.h"

#include "PS5ThreadTimes.h"

#include <memory>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <ps5platform/kernel.h>

// A stack of its own for the crash report, so that a stack overflow can be reported too
// (ps5/frontend/ps5_crash.cpp).
extern "C" void ps5px_install_crash_stack(void);

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

__forceinline void Threading::SpinWait()
{
	__asm__("pause");
}

__forceinline void Threading::EnableHiresScheduler()
{
}

__forceinline void Threading::DisableHiresScheduler()
{
}

// What the platform layer gives a thread that asks for no stack size.
static constexpr size_t PS5_DEFAULT_STACK_SIZE = 2 * 1024 * 1024;

// Unit of time of GetThreadCpuTime/GetCpuTime
u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

// The calling thread's own processor time, in microseconds (0 if the console does not say).
static u64 own_thread_time()
{
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return 0;

	return (u64)ts.tv_sec * (u64)1e6 + (u64)ts.tv_nsec / (u64)1e3;
}

// The threads' processor times as each thread notes its own (PS5ThreadTimes.h has the reason:
// only a thread's own clock is known to be readable on the console).
static PS5ThreadTimes s_thread_times;

// A thread notes its own time, for the performance overlay to read from another thread: the
// emulation thread once a frame (the host), the VU thread each time it wakes (MTVU.cpp).
extern "C" void ps5px_publish_thread_cpu_time(void)
{
	s_thread_times.Publish((uptr)pthread_self(), own_thread_time());
}

// A thread that is not one of Threading::Thread's (a std::thread: the software renderer's workers)
// gives its slot back when it ends, so that renderers started again and again never fill the table.
extern "C" void ps5px_forget_thread_cpu_time(void)
{
	s_thread_times.Forget((uptr)pthread_self());
}

u64 Threading::GetThreadCpuTime()
{
	return own_thread_time();
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = (void*)pthread_self();
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	if (!m_native_handle)
		return 0;

	// The thread itself: its live time, noted, so that no later reader sees less. Another
	// thread: the last time that thread noted.
	if (pthread_equal(pthread_self(), (pthread_t)m_native_handle))
	{
		const u64 time = own_thread_time();
		s_thread_times.Publish((uptr)m_native_handle, time);
		return time;
	}
	return s_thread_times.Read((uptr)m_native_handle);
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (!m_native_handle)
		return false;

	// A title's threads run on logical processors 0 to 12 (the platform layer's probe); no mask
	// means all of those.
	const u64 mask = (processor_mask != 0) ? (processor_mask & 0x1FFFu) : 0x1FFFu;
	return (mask != 0 && scePthreadSetaffinity((pthread_t)m_native_handle, mask) == 0);
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

void* Threading::Thread::ThreadProc(void* param)
{
	ps5px_install_crash_stack();
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	(*entry.get())();
	// This thread's handle may be given to the next thread that is created.
	s_thread_times.Forget((uptr)pthread_self());
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

	// A thread created with no stack size gets the platform layer's default: 2 MiB of direct memory
	// with a guard page (the console's own default is 64 KiB). One that asks for more is created as
	// asked, on a stack out of the title's small flexible memory; one that asks for 2 MiB or less
	// (the VU thread) is as well off with the default.
	pthread_attr_t attrs;
	bool has_attributes = false;
	if (m_stack_size > PS5_DEFAULT_STACK_SIZE)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
		pthread_attr_setstacksize(&attrs, m_stack_size);
	}

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
	if (has_attributes)
		pthread_attr_destroy(&attrs);
	if (res != 0)
		return false;

	// thread started, it'll release the memory
	m_native_handle = (void*)handle;
	func_clone.release();
	return true;
}

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach((pthread_t)m_native_handle);
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	void* retval;
	const int res = pthread_join((pthread_t)m_native_handle, &retval);
	if (res != 0)
		pxFailRel("pthread_join() for thread join failed");

	m_native_handle = nullptr;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

// A test run's profiler (frontend/ps5_profiler.cpp): the threads it is for join it by name.
extern "C" void ps5px_profile_thread_named(const char* name) __attribute__((weak));

void Threading::SetNameOfCurrentThread(const char* name)
{
	// No thread naming on the console. The name is what a profiled test run knows a thread by.
	if (ps5px_profile_thread_named)
		ps5px_profile_thread_named(name);
}

// A count and the waiters in atomics, a mutex and a condition variable only for sleeping: POSIX
// semaphores are not relied on for a title, and every call into the console's threading library is
// a system call. The VU thread posts a semaphore for each XGKICK packet it finishes (MTVU.cpp,
// semaXGkick), thousands a second in a heavy scene, and spent a third of its time in those posts
// while the GS thread was seldom waiting for one. Now a post touches the kernel only when a thread
// is asleep on the semaphore, and a wait looks for the count for 50 microseconds before it sleeps,
// as the work queues do (Semaphore.cpp), so that a post seldom finds one asleep.
//
// The race a post and a wait can run: the post raises the count and then reads the waiters, the
// wait raises the waiters and then reads the count, both sequentially consistent, so one of them
// sees the other's change: either the post signals, or the wait finds the count and does not sleep.

Threading::KernelSemaphore::KernelSemaphore()
{
	if (pthread_mutex_init(&m_mutex, nullptr) != 0 || pthread_cond_init(&m_cond, nullptr) != 0)
		pxFailRel("Semaphore creation failed");
}

Threading::KernelSemaphore::~KernelSemaphore()
{
	pthread_cond_destroy(&m_cond);
	pthread_mutex_destroy(&m_mutex);
}

void Threading::KernelSemaphore::Post()
{
	m_count.fetch_add(1, std::memory_order_seq_cst);
	if (m_waiters.load(std::memory_order_seq_cst) == 0)
		return;
	// Signalled with the mutex held: a waiter between its check of the count and its wait is not
	// missed, and the semaphore may be destroyed as soon as the waiter returns.
	pthread_mutex_lock(&m_mutex);
	pthread_cond_signal(&m_cond);
	pthread_mutex_unlock(&m_mutex);
}

bool Threading::KernelSemaphore::TryWait()
{
	s32 count = m_count.load(std::memory_order_relaxed);
	while (count > 0)
	{
		if (m_count.compare_exchange_weak(count, count - 1, std::memory_order_acquire, std::memory_order_relaxed))
			return true;
	}
	return false;
}

void Threading::KernelSemaphore::Wait()
{
	// A short look first: the post is usually on its way.
	u32 waited = 0;
	while (waited <= SPIN_TIME_NS)
	{
		if (TryWait())
			return;
		waited += ShortSpin();
	}
	pthread_mutex_lock(&m_mutex);
	m_waiters.fetch_add(1, std::memory_order_seq_cst);
	while (!TryWait())
		pthread_cond_wait(&m_cond, &m_mutex);
	m_waiters.fetch_sub(1, std::memory_order_seq_cst);
	pthread_mutex_unlock(&m_mutex);
}
