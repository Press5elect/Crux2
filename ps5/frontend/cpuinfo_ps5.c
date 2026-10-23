// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/* Crux2 - cpuinfo for the console: one fixed package of eight Zen 2 cores with two
 * threads each. The instruction-set flags come from CPUID, so the answers are the hardware's.
 * (The library's FreeBSD back end asks the kernel for a topology a title's sandbox does not give
 * out.) The header is PCSX2's own, 3rdparty/cpuinfo/include/cpuinfo.h. */
#include <cpuinfo.h>

#include <cpuid.h>
#include <string.h>

#define PS5_CORES 8u
#define PS5_THREADS 16u

struct cpuinfo_x86_isa cpuinfo_isa;

static struct cpuinfo_package s_package;
static struct cpuinfo_cluster s_cluster;
static struct cpuinfo_core s_cores[PS5_CORES];
static struct cpuinfo_processor s_processors[PS5_THREADS];
static struct cpuinfo_cache s_l1i[PS5_CORES], s_l1d[PS5_CORES], s_l2[PS5_CORES], s_l3[2];
static struct cpuinfo_uarch_info s_uarch;
static bool s_initialized;

bool cpuinfo_initialize(void)
{
	if (s_initialized)
		return true;

	unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
	unsigned family_model = 0;
	memset(&cpuinfo_isa, 0, sizeof(cpuinfo_isa));
	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
	{
		family_model = eax;
		cpuinfo_isa.sse3 = (ecx & bit_SSE3) != 0;
		cpuinfo_isa.ssse3 = (ecx & bit_SSSE3) != 0;
		cpuinfo_isa.sse4_1 = (ecx & bit_SSE4_1) != 0;
		cpuinfo_isa.sse4_2 = (ecx & bit_SSE4_2) != 0;
		cpuinfo_isa.popcnt = (ecx & bit_POPCNT) != 0;
		cpuinfo_isa.avx = (ecx & bit_AVX) != 0;
		cpuinfo_isa.fma3 = (ecx & bit_FMA) != 0;
		cpuinfo_isa.f16c = (ecx & bit_F16C) != 0;
		cpuinfo_isa.aes = (ecx & bit_AES) != 0;
		cpuinfo_isa.movbe = (ecx & bit_MOVBE) != 0;
	}
	if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
	{
		cpuinfo_isa.avx2 = cpuinfo_isa.avx && (ebx & bit_AVX2) != 0;
		cpuinfo_isa.bmi = (ebx & bit_BMI) != 0;
		cpuinfo_isa.bmi2 = (ebx & bit_BMI2) != 0;
		cpuinfo_isa.sha = (ebx & bit_SHA) != 0;
	}

	memset(&s_package, 0, sizeof(s_package));
	strncpy(s_package.name, "AMD Zen 2 (PlayStation 5)", sizeof(s_package.name) - 1);
	s_package.processor_count = PS5_THREADS;
	s_package.core_count = PS5_CORES;
	s_package.cluster_count = 1;

	memset(&s_cluster, 0, sizeof(s_cluster));
	s_cluster.processor_count = PS5_THREADS;
	s_cluster.core_count = PS5_CORES;
	s_cluster.package = &s_package;
	s_cluster.vendor = cpuinfo_vendor_amd;
	s_cluster.uarch = cpuinfo_uarch_zen2;
	s_cluster.cpuid = family_model;
	s_cluster.frequency = 3500000000ull;

	for (unsigned i = 0; i < 2; i++)
	{
		/* Two 4 MiB level 3 caches, one for each four cores. */
		s_l3[i] = (struct cpuinfo_cache){.size = 4u << 20, .associativity = 16, .sets = 4096, .partitions = 1,
			.line_size = 64, .flags = CPUINFO_CACHE_UNIFIED, .processor_start = i * 8, .processor_count = 8};
	}

	for (unsigned i = 0; i < PS5_CORES; i++)
	{
		s_cores[i] = (struct cpuinfo_core){.processor_start = i * 2, .processor_count = 2, .core_id = i,
			.cluster = &s_cluster, .package = &s_package, .vendor = cpuinfo_vendor_amd, .uarch = cpuinfo_uarch_zen2,
			.cpuid = family_model, .frequency = 3500000000ull};
		s_l1i[i] = (struct cpuinfo_cache){.size = 32768, .associativity = 8, .sets = 64, .partitions = 1,
			.line_size = 64, .flags = 0, .processor_start = i * 2, .processor_count = 2};
		s_l1d[i] = s_l1i[i];
		s_l2[i] = (struct cpuinfo_cache){.size = 524288, .associativity = 8, .sets = 1024, .partitions = 1,
			.line_size = 64, .flags = CPUINFO_CACHE_UNIFIED, .processor_start = i * 2, .processor_count = 2};
	}

	for (unsigned i = 0; i < PS5_THREADS; i++)
	{
		memset(&s_processors[i], 0, sizeof(s_processors[i]));
		s_processors[i].smt_id = i & 1u;
		s_processors[i].core = &s_cores[i / 2];
		s_processors[i].cluster = &s_cluster;
		s_processors[i].package = &s_package;
		s_processors[i].apic_id = i;
		s_processors[i].cache.l1i = &s_l1i[i / 2];
		s_processors[i].cache.l1d = &s_l1d[i / 2];
		s_processors[i].cache.l2 = &s_l2[i / 2];
		s_processors[i].cache.l3 = &s_l3[i / 8];
	}

	s_uarch = (struct cpuinfo_uarch_info){.uarch = cpuinfo_uarch_zen2, .cpuid = family_model,
		.processor_count = PS5_THREADS, .core_count = PS5_CORES};

	s_initialized = true;
	return true;
}

void cpuinfo_deinitialize(void)
{
}

const struct cpuinfo_processor* cpuinfo_get_processors(void) { return s_processors; }
const struct cpuinfo_core* cpuinfo_get_cores(void) { return s_cores; }
const struct cpuinfo_cluster* cpuinfo_get_clusters(void) { return &s_cluster; }
const struct cpuinfo_package* cpuinfo_get_packages(void) { return &s_package; }
const struct cpuinfo_uarch_info* cpuinfo_get_uarchs(void) { return &s_uarch; }

const struct cpuinfo_processor* cpuinfo_get_processor(uint32_t index) { return (index < PS5_THREADS) ? &s_processors[index] : NULL; }
const struct cpuinfo_core* cpuinfo_get_core(uint32_t index) { return (index < PS5_CORES) ? &s_cores[index] : NULL; }
const struct cpuinfo_cluster* cpuinfo_get_cluster(uint32_t index) { return (index == 0) ? &s_cluster : NULL; }
const struct cpuinfo_package* cpuinfo_get_package(uint32_t index) { return (index == 0) ? &s_package : NULL; }
const struct cpuinfo_uarch_info* cpuinfo_get_uarch(uint32_t index) { return (index == 0) ? &s_uarch : NULL; }

uint32_t cpuinfo_get_processors_count(void) { return PS5_THREADS; }
uint32_t cpuinfo_get_cores_count(void) { return PS5_CORES; }
uint32_t cpuinfo_get_clusters_count(void) { return 1; }
uint32_t cpuinfo_get_packages_count(void) { return 1; }
uint32_t cpuinfo_get_uarchs_count(void) { return 1; }

const struct cpuinfo_cache* cpuinfo_get_l1i_caches(void) { return s_l1i; }
const struct cpuinfo_cache* cpuinfo_get_l1d_caches(void) { return s_l1d; }
const struct cpuinfo_cache* cpuinfo_get_l2_caches(void) { return s_l2; }
const struct cpuinfo_cache* cpuinfo_get_l3_caches(void) { return s_l3; }
const struct cpuinfo_cache* cpuinfo_get_l1i_cache(uint32_t index) { return (index < PS5_CORES) ? &s_l1i[index] : NULL; }
const struct cpuinfo_cache* cpuinfo_get_l1d_cache(uint32_t index) { return (index < PS5_CORES) ? &s_l1d[index] : NULL; }
const struct cpuinfo_cache* cpuinfo_get_l2_cache(uint32_t index) { return (index < PS5_CORES) ? &s_l2[index] : NULL; }
const struct cpuinfo_cache* cpuinfo_get_l3_cache(uint32_t index) { return (index < 2) ? &s_l3[index] : NULL; }
uint32_t cpuinfo_get_l1i_caches_count(void) { return PS5_CORES; }
uint32_t cpuinfo_get_l1d_caches_count(void) { return PS5_CORES; }
uint32_t cpuinfo_get_l2_caches_count(void) { return PS5_CORES; }
uint32_t cpuinfo_get_l3_caches_count(void) { return 2; }
