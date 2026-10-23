// Crux2 - the profiling build's counts: the runtime of ps5_pgo.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pgo.h"
#include "ps5_platform.h"

#include "common/Timer.h"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

#ifdef PS5PX_PGO_GENERATE
// LLVM's profile runtime (compiler-rt/lib/profile, built by ps5/tools/build-profile-runtime.sh).
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char* name);
#endif

namespace PS5Pgo
{
namespace
{
std::mutex s_lock;
std::string s_folder, s_file;
unsigned s_writes = 0;
} // namespace

bool Active()
{
#ifdef PS5PX_PGO_GENERATE
	return true;
#else
	return false;
#endif
}

void Start(const std::string& data_root)
{
#ifdef PS5PX_PGO_GENERATE
	const std::lock_guard<std::mutex> lock(s_lock);
	if (!s_file.empty())
		return;
	s_folder = data_root + (data_root.empty() || data_root.back() == '/' ? "" : "/") + "pgo";
	mkdir(s_folder.c_str(), 0777);
	char stamp[32] = "unknown";
	const time_t now = time(nullptr);
	struct tm local;
	if (localtime_r(&now, &local))
		strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
	s_file = s_folder + "/crux2-" + stamp + ".profraw";
	// The runtime keeps the pointer: s_file lives as long as the program.
	__llvm_profile_set_filename(s_file.c_str());
	ps5px_log("pgo: a profiling build: what runs is counted, and written to %s when a game closes and when "
			  "Crux2 closes (slower than a normal build while it counts)",
		s_file.c_str());
#else
	(void)data_root;
#endif
}

void Write(const char* why)
{
#ifdef PS5PX_PGO_GENERATE
	const std::lock_guard<std::mutex> lock(s_lock);
	if (s_file.empty())
		return;
	Common::Timer timer;
	// The runtime empties the file once, at start, then appends each write (fopen "ab" when
	// profiles are not merged): a second write would put the counts in twice, the first
	// write's included. The counts in memory are the whole run's, so the file is emptied
	// first and holds only the latest write.
	if (FILE* const fp = std::fopen(s_file.c_str(), "wb"))
		std::fclose(fp);
	const int result = __llvm_profile_write_file();
	struct stat st;
	const double mb = stat(s_file.c_str(), &st) == 0 ? st.st_size / 1048576.0 : 0.0;
	s_writes++;
	if (result == 0)
		ps5px_log("pgo: the counts so far written (%s): %.1f MB in %.0f ms, write %u", why, mb, timer.GetTimeMilliseconds(), s_writes);
	else
		ps5px_log("pgo: the counts could not be written (%s): %d", why, result);
#else
	(void)why;
#endif
}

std::string Folder()
{
	const std::lock_guard<std::mutex> lock(s_lock);
	return s_folder;
}
} // namespace PS5Pgo
