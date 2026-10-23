// Crux2 - the port's memory code (ps5/common/PS5HostSys.cpp), run on the PC over a stand-in
// for the platform layer (mock_platform.cpp), in the order PCSX2 uses it:
//
//   - the main area: guest memory (a shared-memory object) at its start and the recompilers' code
//     area after it, within reach of the program's own code;
//   - the fastmem window: 16 KiB host pages of guest memory mapped and unmapped one at a time;
//   - page protection and the fault handler (a write to a protected page arrives, is let through
//     and is retried);
//   - the GS's wrapped memory: one object mapped four times back to back;
//   - everything released again, with nothing asked of the platform layer that it would refuse.
//
// It says nothing about the console's kernel: which addresses it grants is what a console run shows.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>

extern "C" {
extern unsigned g_mock_shm_maps, g_mock_shm_unmaps, g_mock_exec_allocs, g_mock_exec_frees, g_mock_reservations, g_mock_releases;
extern unsigned g_mock_bad_requests;
}

// ---- what PS5HostSys.cpp needs of the rest of PCSX2 ---------------------------------------------

void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	std::fprintf(stderr, "%s:%d: assertion failed in %s: %s\n", file, line, func, msg);
	std::abort();
}

LOGLEVEL Log::GetMaxLevel()
{
	return LOGLEVEL_INFO;
}

void Log::Writev(LOGLEVEL level, ConsoleColors color, const char* format, va_list ap)
{
	std::vprintf(format, ap);
	std::printf("\n");
}

void Log::Write(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	std::printf("%.*s\n", static_cast<int>(message.size()), message.data());
}

void Error::SetErrno(Error* errptr, std::string_view prefix, int err)
{
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
	std::fprintf(stderr, "unhandled fault at %p\n", siginfo->si_addr);
	std::_Exit(3);
}

// ---- the test ----------------------------------------------------------------------------------

static unsigned s_checks, s_failures;

#define CHECK(condition, ...) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::printf("FAILED (%s:%d): %s: ", __FILE__, __LINE__, #condition); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

// As PCSX2's memory map: main memory of this size, and 305 MiB of code after it.
static constexpr size_t MAIN_SIZE = 285 * 1024 * 1024; // HostMemoryMap::MainSize
static constexpr size_t CODE_SIZE = 0x13100000;
static constexpr size_t FASTMEM_SIZE = 0x100000000ull;

static u8* s_protected_page;
static unsigned s_faults;
static bool s_fault_was_write;
static void* s_fault_address;

PageFaultHandler::HandlerResult PageFaultHandler::HandlePageFault(void* exception_pc, void* fault_address, bool is_write)
{
	s_faults++;
	s_fault_was_write = is_write;
	s_fault_address = fault_address;
	if (!s_protected_page || fault_address < s_protected_page || fault_address >= s_protected_page + __pagesize)
		return HandlerResult::ExecuteNextHandler;

	// As the emulator does for a write to a page with recompiled code in it: writable again.
	HostSys::MemProtect(s_protected_page, __pagesize, PageAccess_ReadWrite());
	return HandlerResult::ContinueExecution;
}

int main()
{
	static_assert(__pagesize == 0x4000, "the test is of the 16 KiB path");
	CHECK(HostSys::GetRuntimePageSize() == __pagesize, "%zu", HostSys::GetRuntimePageSize());
	CHECK(HostSys::GetRuntimeCacheLineSize() == __cachelinesize, "%zu", HostSys::GetRuntimeCacheLineSize());

	// ---- the main area
	void* const shm = HostSys::CreateSharedMemory(HostSys::GetFileMappingName("pcsx2").c_str(), MAIN_SIZE);
	CHECK(shm != nullptr, "no shared memory");

	std::unique_ptr<SharedMemoryMappingArea> area = SharedMemoryMappingArea::Create(MAIN_SIZE + CODE_SIZE, true);
	CHECK(area != nullptr, "no main area");
	if (!shm || !area)
		return 1;

	const sptr distance = reinterpret_cast<sptr>(area->BasePointer()) - reinterpret_cast<sptr>(&main);
	const sptr far_end = distance + static_cast<sptr>(MAIN_SIZE + CODE_SIZE);
	CHECK(distance == static_cast<s32>(distance) && far_end == static_cast<s32>(far_end),
		"the area at %p is out of reach of the program at %p", area->BasePointer(), reinterpret_cast<void*>(&main));
	CHECK((reinterpret_cast<uptr>(area->BasePointer()) & 0xFFFF) == 0, "the area is not on a 64 KiB boundary");

	u8* const data = area->Map(shm, 0, area->BasePointer(), MAIN_SIZE, PageAccess_ReadWrite());
	CHECK(data == area->BasePointer(), "data at %p", data);
	u8* const code = area->Map(nullptr, 0, area->OffsetPointer(MAIN_SIZE), CODE_SIZE, PageAccess_Any());
	CHECK(code == area->OffsetPointer(MAIN_SIZE), "code at %p", code);
	if (!data || !code)
		return 1;

	std::memset(data, 0x5A, 0x10000);
	CHECK(data[0xFFFF] == 0x5A, "data is not writable");
	CHECK(code[0] == 0 && code[CODE_SIZE - 1] == 0, "the code area does not read zero");

	// mov eax, 42 ; ret
	static constexpr u8 function[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
	std::memcpy(code + 0x1234560, function, sizeof(function));
	CHECK(reinterpret_cast<int (*)()>(code + 0x1234560)() == 42, "generated code does not run");

	// ---- the fastmem window
	std::unique_ptr<SharedMemoryMappingArea> fastmem = SharedMemoryMappingArea::Create(FASTMEM_SIZE);
	CHECK(fastmem != nullptr, "no fastmem area");
	if (!fastmem)
		return 1;

	// The first 64 pages of guest memory at guest address 0, again at 0x2000_0000, and one page of
	// another offset at 0x7000_0000.
	bool mapped = true;
	for (u32 page = 0; page < 64; page++)
	{
		mapped &= fastmem->Map(shm, page * __pagesize, fastmem->PagePointer(page), __pagesize, PageAccess_ReadWrite()) != nullptr;
		mapped &= fastmem->Map(shm, page * __pagesize, fastmem->OffsetPointer(0x20000000 + page * __pagesize), __pagesize,
					  PageAccess_ReadWrite()) != nullptr;
	}
	mapped &= fastmem->Map(shm, 0x100000, fastmem->OffsetPointer(0x70000000), __pagesize, PageAccess_ReadOnly()) != nullptr;
	CHECK(mapped, "a fastmem page did not map");

	data[0x4321] = 0x77;
	CHECK(fastmem->BasePointer()[0x4321] == 0x77, "the view at 0 does not see guest memory");
	fastmem->OffsetPointer(0x20000000)[0x8001] = 0x33;
	CHECK(data[0x8001] == 0x33 && fastmem->BasePointer()[0x8001] == 0x33, "the mirror is not the same memory");
	data[0x100010] = 0x99;
	CHECK(fastmem->OffsetPointer(0x70000000)[0x10] == 0x99, "a view at an offset is of the wrong memory");

	// ---- protection and the fault handler
	CHECK(PageFaultHandler::Install(nullptr), "the fault handler was not installed");

	s_protected_page = data + 3 * __pagesize;
	HostSys::MemProtect(s_protected_page, __pagesize, PageAccess_ReadOnly());
	CHECK(s_protected_page[5] == 0x5A && s_faults == 0, "reading a read-only page faulted");
	volatile u8* const target = s_protected_page + 5;
	*target = 0x11;
	CHECK(s_faults == 1 && s_fault_was_write && s_fault_address == s_protected_page + 5, "faults %u, write %d, at %p", s_faults,
		s_fault_was_write, s_fault_address);
	CHECK(s_protected_page[5] == 0x11, "the write was not retried");

	// The same through a fastmem view: protection is per view.
	s_protected_page = fastmem->PagePointer(7);
	HostSys::MemProtect(s_protected_page, __pagesize, PageAccess_ReadOnly());
	data[7 * __pagesize] = 0x21;
	CHECK(s_faults == 1, "a write through the other view faulted");
	volatile u8* const view_target = s_protected_page;
	*view_target = 0x22;
	CHECK(s_faults == 2 && data[7 * __pagesize] == 0x22, "faults %u", s_faults);
	s_protected_page = nullptr;

	// ---- views unmapped and mapped again (a guest TLB change)
	bool unmapped = true;
	for (u32 page = 0; page < 64; page++)
		unmapped &= fastmem->Unmap(fastmem->OffsetPointer(0x20000000 + page * __pagesize), __pagesize);
	CHECK(unmapped, "a fastmem page did not unmap");
	CHECK(fastmem->Map(shm, 5 * __pagesize, fastmem->OffsetPointer(0x20000000), __pagesize, PageAccess_ReadWrite()) != nullptr,
		"a page did not map into a hole");
	data[5 * __pagesize + 9] = 0x66;
	CHECK(fastmem->OffsetPointer(0x20000000)[9] == 0x66, "the page mapped into a hole is of the wrong memory");
	CHECK(fastmem->Unmap(fastmem->OffsetPointer(0x20000000), __pagesize), "unmap");

	for (u32 page = 0; page < 64; page++)
		unmapped &= fastmem->Unmap(fastmem->PagePointer(page), __pagesize);
	unmapped &= fastmem->Unmap(fastmem->OffsetPointer(0x70000000), __pagesize);
	CHECK(unmapped, "a fastmem page did not unmap");
	fastmem.reset();

	// ---- the GS's wrapped memory: 4 MiB, four times back to back
	static constexpr size_t GS_SIZE = 4 * 1024 * 1024;
	void* const gs_shm = HostSys::CreateSharedMemory("GS.mem", GS_SIZE);
	std::unique_ptr<SharedMemoryMappingArea> gs_area = SharedMemoryMappingArea::Create(GS_SIZE * 4);
	CHECK(gs_shm && gs_area, "no GS memory");
	if (!gs_shm || !gs_area)
		return 1;
	for (u32 i = 0; i < 4; i++)
		CHECK(gs_area->Map(gs_shm, 0, gs_area->OffsetPointer(GS_SIZE * i), GS_SIZE, PageAccess_ReadWrite()) != nullptr, "repeat %u", i);
	// A write that runs over the end of the first copy lands at the start of all of them.
	std::memset(gs_area->OffsetPointer(GS_SIZE - 8), 0xAB, 16);
	CHECK(gs_area->BasePointer()[0] == 0xAB && gs_area->BasePointer()[7] == 0xAB && gs_area->OffsetPointer(GS_SIZE * 3)[7] == 0xAB &&
			  gs_area->OffsetPointer(GS_SIZE * 2 - 1)[0] == 0xAB,
		"the copies are not the same memory");
	for (u32 i = 0; i < 4; i++)
		CHECK(gs_area->Unmap(gs_area->OffsetPointer(GS_SIZE * i), GS_SIZE), "repeat %u", i);
	gs_area.reset();
	HostSys::DestroySharedMemory(gs_shm);

	// ---- the main area released, code first, as PCSX2 does
	CHECK(area->Unmap(code, CODE_SIZE, false), "the code area did not unmap");
	CHECK(area->Unmap(data, MAIN_SIZE, true), "main memory did not unmap");
	area.reset();
	HostSys::DestroySharedMemory(shm);

	CHECK(g_mock_bad_requests == 0, "%u request(s) the platform layer refuses", g_mock_bad_requests);
	CHECK(g_mock_exec_allocs == 1 && g_mock_exec_frees == 1, "code regions: %u made, %u freed", g_mock_exec_allocs, g_mock_exec_frees);
	CHECK(g_mock_reservations == g_mock_releases, "reservations: %u made, %u released", g_mock_reservations, g_mock_releases);
	CHECK(g_mock_shm_maps == g_mock_shm_unmaps, "views: %u mapped, %u unmapped", g_mock_shm_maps, g_mock_shm_unmaps);

	std::printf("host memory: %u checks, %u failed\n", s_checks, s_failures);
	return (s_failures == 0) ? 0 : 1;
}
