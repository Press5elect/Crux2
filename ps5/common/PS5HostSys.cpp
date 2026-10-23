// Crux2 - memory, pages and page faults (common/HostSys.h), in place of
// common/Linux/LnxHostSys.cpp.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Guest memory, its fastmem views and the recompilers' code live in direct memory, through the
// payload SDK fork's platform layer (ps5platform/shm.h, exec.h). Plain mmap() draws from the
// title's small flexible budget, charges it for every view of a shared-memory object and refuses
// PROT_EXEC; one direct-memory object can be mapped any number of times and is charged once to
// the 12 GiB pool. Kernel pages are 16 KiB (OVERRIDE_HOST_PAGE_SIZE), so page protection and the
// fastmem window take the vtlb's coalescing path, as on Apple Silicon.

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ucontext.h>
#include <unistd.h>

// Refuses to build against an SDK whose ucontext_t is not the console's layout.
#include <ps5platform/context.h>
#include <ps5platform/exec.h>
#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

extern "C" void ps5px_log(const char* format, ...);

namespace
{
	// The Vulkan driver's GPU-visible window, which nothing else may be mapped into, and the
	// platform layer's first choice for an arena that needs no place in particular.
	constexpr uptr GPU_WINDOW_LOW = 0x200000000ull;
	constexpr uptr GPU_WINDOW_HIGH = 0x300000000ull;
	constexpr uptr ARENA_HINT = 0x1000000000ull;

	// How far the recompilers' area may be from the title's own code and data: a signed 32-bit
	// displacement, less room for the title's image (the platform layer's PS5P_NEAR_REACH).
	constexpr uptr NEAR_REACH = 0x7c000000ull;
	constexpr uptr NEAR_STEP = 0x4000000ull; // 64 MiB

	// The code part of the main area: one executable direct-memory region, mapped over a part of
	// the reservation (SharedMemoryMappingArea::Map with no file).
	std::mutex s_code_regions_mutex;
	struct CodeRegion
	{
		void* base;
		ps5_exec_region region;
	};
	CodeRegion s_code_regions[4];

	int KernelProtection(const PageProtectionMode& mode)
	{
		return (mode.CanRead() ? PS5_KERNEL_PROT_CPU_READ : 0) | (mode.CanWrite() ? PS5_KERNEL_PROT_CPU_WRITE : 0) |
			   (mode.CanExecute() ? (PS5_KERNEL_PROT_CPU_EXEC | PS5_KERNEL_PROT_CPU_READ) : 0);
	}

	int ShmProtection(const PageProtectionMode& mode)
	{
		return (mode.CanRead() ? PS5_SHM_READ : 0) | (mode.CanWrite() ? PS5_SHM_WRITE : 0) |
			   (mode.CanExecute() ? (PS5_SHM_EXEC | PS5_SHM_READ) : 0);
	}

	bool OutsideGPUWindow(uptr base, size_t size)
	{
		return base >= GPU_WINDOW_HIGH || base + size <= GPU_WINDOW_LOW;
	}

	bool NearEnough(uptr base, size_t size, uptr anchor)
	{
		if (base < 0x1000000ull || !OutsideGPUWindow(base, size))
			return false;
		const uptr low = (base < anchor) ? (anchor - base) : 0;
		const uptr high = (base + size > anchor) ? (base + size - anchor) : 0;
		return low <= NEAR_REACH && high <= NEAR_REACH;
	}

	// Address space for the main area within reach of the title's own code and data, so that the
	// recompilers' code reaches the emulator's functions and variables with 32-bit displacements.
	// The kernel takes an address as a hint, so each candidate is asked for exactly.
	u8* ReserveNear(size_t size, uptr anchor)
	{
		const uptr origin = anchor / NEAR_STEP * NEAR_STEP;
		for (uptr step = 1; step <= NEAR_REACH / NEAR_STEP; step++)
		{
			const uptr above = origin + step * NEAR_STEP;
			if (NearEnough(above, size, anchor) && ps5_vrange_reserve_at(reinterpret_cast<void*>(above), size) == 0)
				return reinterpret_cast<u8*>(above);

			const uptr distance = step * NEAR_STEP + Common::AlignUpPow2(size, NEAR_STEP);
			if (origin > distance)
			{
				const uptr below = origin - distance;
				if (NearEnough(below, size, anchor) && ps5_vrange_reserve_at(reinterpret_cast<void*>(below), size) == 0)
					return reinterpret_cast<u8*>(below);
			}
		}
		return nullptr;
	}
} // namespace

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

	const int result = sceKernelMprotect(baseaddr, size, KernelProtection(mode));
	if (result != 0)
	{
		// The first few are worth a line each; a game that keeps failing would fill the log.
		static std::atomic_uint s_failures{0};
		if (s_failures.fetch_add(1, std::memory_order_relaxed) < 8)
			ps5px_log("memory: sceKernelMprotect(%p, %zu) failed: 0x%08X", baseaddr, size, static_cast<unsigned>(result));
		pxFail("sceKernelMprotect() failed");
	}
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	// Objects have no names here, and nothing else on the console could open one.
	return prefix;
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	ps5_shm* const shm = new ps5_shm{};
	const int result = ps5_shm_create(size, shm);
	if (result != 0)
	{
		ps5px_log("memory: ps5_shm_create(%zu) for %s failed: 0x%08X", size, name, static_cast<unsigned>(result));
		delete shm;
		return nullptr;
	}

	return shm;
}

void HostSys::DestroySharedMemory(void* ptr)
{
	ps5_shm* const shm = static_cast<ps5_shm*>(ptr);
	ps5_shm_destroy(shm);
	delete shm;
}

size_t HostSys::GetRuntimePageSize()
{
	return PS5_KERNEL_PAGE_SIZE;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	// Zen 2.
	return 64;
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");

	// The reservation and every hole left in it.
	if (ps5_vrange_release(m_base_ptr, m_size) != 0)
		pxFailRel("Failed to release shared memory area");
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

	// Address space only: views of guest memory and the code region are mapped into it afterwards.
	u8* base = nullptr;
	if (jit)
	{
		const uptr anchor = reinterpret_cast<uptr>(&SharedMemoryMappingArea::Create);
		base = ReserveNear(size, anchor);
		ps5px_log("memory: %zu MiB for main memory and code %s the title's code (title at %p, area at %p)", size >> 20,
			base ? "within reach of" : "could not be placed near", reinterpret_cast<void*>(anchor), base);
	}

	if (!base)
	{
		void* placed = nullptr;
		const int result = ps5_vrange_reserve(size, reinterpret_cast<void*>(ARENA_HINT), PS5_KERNEL_DIRECT_ALIGNMENT, &placed);
		if (result != 0)
		{
			ps5px_log("memory: ps5_vrange_reserve(%zu) failed: 0x%08X", size, static_cast<unsigned>(result));
			return nullptr;
		}
		base = static_cast<u8*>(placed);
		ps5px_log("memory: %zu MiB of address space reserved at %p", size >> 20, base);
	}

	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(base, size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

	if (file_handle)
	{
		// A view of a shared-memory object, over a part of the reservation.
		void* view = nullptr;
		const int result = ps5_shm_map(static_cast<const ps5_shm*>(file_handle), file_offset, map_size, map_base,
			ShmProtection(mode), PS5_SHM_FIXED, &view);
		if (result != 0)
		{
			ps5px_log("memory: ps5_shm_map(%zu bytes at %p) failed: 0x%08X", map_size, map_base, static_cast<unsigned>(result));
			return nullptr;
		}
	}
	else
	{
		// Memory of its own: the recompilers' code area, read, write and execute at once.
		std::unique_lock lock(s_code_regions_mutex);
		CodeRegion* slot = nullptr;
		for (CodeRegion& it : s_code_regions)
		{
			if (!it.base)
			{
				slot = &it;
				break;
			}
		}
		if (!slot)
			return nullptr;

		ps5_exec_request request = {};
		request.bytes = map_size;
		request.address = reinterpret_cast<uintptr_t>(map_base);
		request.flags = PS5_EXEC_FIXED;
		const int result = ps5_exec_alloc(&request, &slot->region);
		if (result != 0)
		{
			ps5px_log("memory: ps5_exec_alloc(%zu bytes at %p) failed: 0x%08X", map_size, map_base, static_cast<unsigned>(result));
			return nullptr;
		}

		slot->base = map_base;
		if (!mode.CanExecute() || !mode.CanWrite())
			sceKernelMprotect(map_base, map_size, KernelProtection(mode));

		size_t flexible_free = 0;
		sceKernelAvailableFlexibleMemorySize(&flexible_free);
		ps5px_log("memory: %zu MiB of executable memory at %p, %zu KiB of flexible memory free", map_size >> 20, map_base,
			flexible_free / 1024);
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

	if (is_file)
	{
		// The hole stays reserved, so nothing else is mapped into the area.
		if (ps5_shm_unmap(map_base, map_size, PS5_SHM_KEEP_RESERVED) != 0)
			return false;
	}
	else
	{
		std::unique_lock lock(s_code_regions_mutex);
		CodeRegion* slot = nullptr;
		for (CodeRegion& it : s_code_regions)
		{
			if (it.base == map_base)
			{
				slot = &it;
				break;
			}
		}
		if (!slot)
			return false;

		ps5_exec_free(&slot->region);
		slot->base = nullptr;
	}

	m_num_mappings--;
	return true;
}

namespace PageFaultHandler
{
	static std::recursive_mutex s_exception_handler_mutex;
	static bool s_in_exception_handler = false;
	static bool s_installed = false;

	static void SignalHandler(int sig, siginfo_t* info, void* ctx);
} // namespace PageFaultHandler

void PageFaultHandler::SignalHandler(int sig, siginfo_t* info, void* ctx)
{
	ucontext_t* const context = static_cast<ucontext_t*>(ctx);
	void* const exception_address = reinterpret_cast<void*>(info->si_addr);
#ifdef __PROSPERO__
	// The payload SDK fork's ucontext_t is the console's layout, so mc_rip is the faulting instruction.
	void* const exception_pc = reinterpret_cast<void*>(context->uc_mcontext.mc_rip);
	const bool is_write = (context->uc_mcontext.mc_err & 2) != 0;
#else
	// The PC build of ps5/tests/hostsys_test.cpp, over a stand-in for the platform layer.
	void* const exception_pc = reinterpret_cast<void*>(context->uc_mcontext.gregs[REG_RIP]);
	const bool is_write = (context->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#endif

	// Executing the handler concurrently from multiple threads wouldn't go down well.
	s_exception_handler_mutex.lock();

	// Prevent recursive exception filtering.
	HandlerResult result = HandlerResult::ExecuteNextHandler;
	if (!s_in_exception_handler)
	{
		s_in_exception_handler = true;
		result = HandlePageFault(exception_pc, exception_address, is_write);
		s_in_exception_handler = false;
	}

	s_exception_handler_mutex.unlock();

	// Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
	if (result == HandlerResult::ContinueExecution)
		return;

	// We couldn't handle it. Pass it off to the crash report.
	CrashHandler::CrashSignalHandler(sig, info, ctx);
}

bool PageFaultHandler::Install(Error* error)
{
	std::unique_lock lock(s_exception_handler_mutex);
	pxAssertRel(!s_installed, "Page fault handler has already been installed.");

	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	// On the thread's crash stack (ps5px_install_crash_stack), so a stack overflow is reported too.
	sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	sa.sa_sigaction = SignalHandler;

	if (sigaction(SIGSEGV, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGSEGV failed: ", errno);
		return false;
	}

	// An access to a reserved but unmapped page may arrive as SIGBUS.
	if (sigaction(SIGBUS, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGBUS failed: ", errno);
		return false;
	}

	s_installed = true;
	return true;
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return true;
}
