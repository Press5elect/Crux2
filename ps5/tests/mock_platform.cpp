// Crux2 - a stand-in for the payload SDK fork's platform layer, for the PC tests.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The same interface (ps5platform/shm.h, exec.h, kernel.h) over the PC's mmap, with the same
// argument rules the real layer enforces (16 KiB views, 64 KiB reservations and code regions), so
// that a caller that breaks one fails here as it would on the console. What it cannot stand in
// for is the console's kernel itself: which addresses it grants, and what it charges for them.

#include <ps5platform/exec.h>
#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace
{
	constexpr size_t PAGE = 0x4000;
	constexpr size_t UNIT = 0x10000;

	std::mutex s_mutex;
	std::map<int64_t, int> s_objects; // direct_start -> memfd
	int64_t s_next_start = UNIT;

	size_t RoundUp(size_t value, size_t unit)
	{
		return (value + unit - 1) / unit * unit;
	}

	int HostProtection(int protection)
	{
		return ((protection & PS5_KERNEL_PROT_CPU_READ) ? PROT_READ : 0) | ((protection & PS5_KERNEL_PROT_CPU_WRITE) ? PROT_WRITE : 0) |
			   ((protection & PS5_KERNEL_PROT_CPU_EXEC) ? PROT_EXEC : 0);
	}
} // namespace

extern "C" {

// What the mock was asked to do, for the tests to look at.
unsigned g_mock_shm_maps, g_mock_shm_unmaps, g_mock_exec_allocs, g_mock_exec_frees, g_mock_reservations, g_mock_releases;
unsigned g_mock_bad_requests;

void ps5px_log(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	std::printf("    log: ");
	std::vprintf(format, args);
	std::printf("\n");
	va_end(args);
}

int32_t sceKernelMprotect(const void* address, size_t length, int protection)
{
	if (reinterpret_cast<uintptr_t>(address) % PAGE != 0 || length % PAGE != 0)
	{
		g_mock_bad_requests++;
		return static_cast<int32_t>(0x80020016);
	}
	return (mprotect(const_cast<void*>(address), length, HostProtection(protection)) == 0) ? 0 : static_cast<int32_t>(0x8002000d);
}

int32_t sceKernelAvailableFlexibleMemorySize(size_t* available)
{
	*available = static_cast<size_t>(448) << 20;
	return 0;
}

int ps5_shm_create(size_t bytes, struct ps5_shm* shm)
{
	if (!shm || bytes == 0)
		return PS5_SHM_BAD_REQUEST;

	const size_t rounded = RoundUp(bytes, UNIT);
	const int fd = memfd_create("ps5_shm", 0);
	if (fd < 0 || ftruncate(fd, static_cast<off_t>(rounded)) != 0)
		return -errno;

	std::unique_lock lock(s_mutex);
	shm->direct_start = s_next_start;
	shm->bytes = rounded;
	s_objects[shm->direct_start] = fd;
	s_next_start += static_cast<int64_t>(rounded);
	return 0;
}

void ps5_shm_destroy(struct ps5_shm* shm)
{
	if (!shm || shm->bytes == 0)
		return;

	std::unique_lock lock(s_mutex);
	close(s_objects[shm->direct_start]);
	s_objects.erase(shm->direct_start);
	std::memset(shm, 0, sizeof(*shm));
}

int ps5_shm_map(const struct ps5_shm* shm, size_t offset, size_t bytes, void* address, int protection, unsigned flags, void** view)
{
	if (view)
		*view = nullptr;
	if (!shm || !view || bytes == 0 || offset % PAGE != 0 || bytes % PAGE != 0 || offset + bytes > shm->bytes ||
		((flags & PS5_SHM_FIXED) && reinterpret_cast<uintptr_t>(address) % PAGE != 0))
	{
		g_mock_bad_requests++;
		return PS5_SHM_BAD_REQUEST;
	}

	int fd;
	{
		std::unique_lock lock(s_mutex);
		fd = s_objects[shm->direct_start];
	}
	const int prot = ((protection & PS5_SHM_READ) ? PROT_READ : 0) | ((protection & PS5_SHM_WRITE) ? PROT_WRITE : 0) |
					 ((protection & PS5_SHM_EXEC) ? PROT_EXEC : 0);
	void* const mapped = mmap(address, bytes, prot, MAP_SHARED | ((flags & PS5_SHM_FIXED) ? MAP_FIXED : 0), fd, static_cast<off_t>(offset));
	if (mapped == MAP_FAILED)
		return -errno;

	*view = mapped;
	g_mock_shm_maps++;
	return 0;
}

int ps5_shm_unmap(void* view, size_t bytes, unsigned flags)
{
	if (!view || bytes == 0)
		return PS5_SHM_BAD_REQUEST;

	g_mock_shm_unmaps++;
	if (flags & PS5_SHM_KEEP_RESERVED)
		return (mmap(view, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == view) ? 0 : -errno;
	return (munmap(view, bytes) == 0) ? 0 : -errno;
}

int ps5_vrange_reserve(size_t bytes, void* hint, size_t alignment, void** base)
{
	if (!base || bytes == 0)
		return PS5_SHM_BAD_REQUEST;

	void* const mapped = mmap(hint, RoundUp(bytes, PAGE), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (mapped == MAP_FAILED)
		return -errno;
	if (reinterpret_cast<uintptr_t>(mapped) % (alignment ? alignment : UNIT) != 0)
	{
		munmap(mapped, RoundUp(bytes, PAGE));
		return PS5_SHM_NO_PLACE;
	}

	*base = mapped;
	g_mock_reservations++;
	return 0;
}

int ps5_vrange_reserve_at(void* address, size_t bytes)
{
	if (!address || bytes == 0 || reinterpret_cast<uintptr_t>(address) % UNIT != 0)
	{
		g_mock_bad_requests++;
		return PS5_SHM_BAD_REQUEST;
	}

	void* const mapped =
		mmap(address, RoundUp(bytes, PAGE), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (mapped == MAP_FAILED)
		return PS5_SHM_NO_PLACE;
	if (mapped != address)
	{
		munmap(mapped, RoundUp(bytes, PAGE));
		return PS5_SHM_NO_PLACE;
	}

	g_mock_reservations++;
	return 0;
}

int ps5_vrange_release(void* base, size_t bytes)
{
	if (!base || bytes == 0)
		return PS5_SHM_BAD_REQUEST;

	g_mock_releases++;
	return (munmap(base, RoundUp(bytes, PAGE)) == 0) ? 0 : -errno;
}

int ps5_exec_alloc(const struct ps5_exec_request* request, struct ps5_exec_region* region)
{
	if (region)
		std::memset(region, 0, sizeof(*region));
	if (!request || !region || request->bytes == 0 ||
		((request->flags & (PS5_EXEC_FIXED | PS5_EXEC_AT)) && (request->address == 0 || request->address % UNIT != 0)))
	{
		g_mock_bad_requests++;
		return PS5_EXEC_BAD_REQUEST;
	}

	const size_t bytes = RoundUp(request->bytes, UNIT);
	const int flags = MAP_PRIVATE | MAP_ANONYMOUS | ((request->flags & PS5_EXEC_FIXED) ? MAP_FIXED : 0) |
					  ((request->flags & PS5_EXEC_AT) ? MAP_FIXED_NOREPLACE : 0);
	void* const mapped = mmap(reinterpret_cast<void*>(request->address), bytes, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
	if (mapped == MAP_FAILED)
		return PS5_EXEC_NO_PLACE;

	region->base = mapped;
	region->write_view = mapped;
	region->bytes = bytes;
	region->direct_start = 1;
	region->flags = request->flags;
	g_mock_exec_allocs++;
	return 0;
}

void ps5_exec_free(struct ps5_exec_region* region)
{
	if (!region || region->bytes == 0)
		return;

	munmap(region->base, region->bytes);
	std::memset(region, 0, sizeof(*region));
	g_mock_exec_frees++;
}

} // extern "C"
