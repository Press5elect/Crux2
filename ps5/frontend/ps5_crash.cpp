// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the crash report.
//
// A title that faults is closed by the console with an error code and nothing else. This handler
// first writes what a developer needs into the boot log in the title folder (the signal, the
// faulting address, the registers, and the return addresses up the stack), then hands the signal
// back to the console, which closes the title as it would have.
//
// Addresses are also given relative to the start of the program, so that they can be looked up in
// the link's output (which keeps its symbols) whatever address the console loaded the title at:
//
//     llvm-addr2line -f -e build/ps5/link/llvm-pie.elf 0x<offset>

#include "ps5_platform.h"

#include "common/CrashHandler.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

// Refuses to build against an SDK whose ucontext_t is not the console's layout.
#include <ps5platform/context.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);
extern "C" int sceKernelUsleep(unsigned microseconds);

// The first byte of the program's image (the linker script places it).
extern "C" const char __executable_start[] __attribute__((weak));

namespace {

volatile sig_atomic_t s_in_crash = 0;
char s_dump_directory[192] = "";

const char* SignalName(int signal)
{
  switch (signal)
  {
    case SIGSEGV:
      return "SIGSEGV (invalid memory access)";
    case SIGBUS:
      return "SIGBUS (bus error)";
    case SIGILL:
      return "SIGILL (illegal instruction)";
    case SIGFPE:
      return "SIGFPE (arithmetic error)";
    case SIGABRT:
      return "SIGABRT (abort)";
    case SIGTRAP:
      return "SIGTRAP (trap)";
    case SIGSYS:
      return "SIGSYS (bad system call)";
    default:
      return "signal";
  }
}

// Formatting on the stack and write(): nothing here allocates or takes a lock.
void Line(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Line(const char* format, ...)
{
  char text[256];
  va_list args;
  va_start(args, format);
  int length = std::vsnprintf(text, sizeof(text) - 1, format, args);
  va_end(args);
  if (length < 0)
    return;
  if (length > static_cast<int>(sizeof(text)) - 2)
    length = static_cast<int>(sizeof(text)) - 2;
  text[length++] = '\n';

  const int fd = ps5px_log_fd();
  if (fd >= 0)
    (void)!write(fd, text, static_cast<size_t>(length));
  text[length] = '\0';
  (void)sceKernelDebugOutText(0, text);
}

void Address(const char* what, uintptr_t address)
{
  const uintptr_t base = reinterpret_cast<uintptr_t>(&__executable_start[0]);
  if (base != 0 && address >= base && address - base < 0x20000000u)
    Line("  %-10s 0x%016llx  (program + 0x%llx)", what, static_cast<unsigned long long>(address),
         static_cast<unsigned long long>(address - base));
  else
    Line("  %-10s 0x%016llx", what, static_cast<unsigned long long>(address));
}

void RestoreDefault(int signal)
{
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = SIG_DFL;
  (void)sigaction(signal, &sa, nullptr);
}

void InstallFor(int signal)
{
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sa.sa_sigaction = &CrashHandler::CrashSignalHandler;
  (void)sigaction(signal, &sa, nullptr);
}

// The threads that have a crash stack, so the watchdog can sample where each one is.
constexpr int MAX_THREADS = 64;
std::mutex s_threads_mutex;
pthread_t s_threads[MAX_THREADS];
bool s_thread_used[MAX_THREADS] = {};

// A thread's own stack for the signal handler, freed with the thread.
struct CrashStack
{
  static constexpr size_t SIZE = 128 * 1024;
  void* memory = nullptr;
  int slot = -1;
  ~CrashStack()
  {
    if (slot >= 0)
    {
      const std::lock_guard<std::mutex> lock(s_threads_mutex);
      s_thread_used[slot] = false;
    }
    if (memory)
    {
      stack_t none;
      std::memset(&none, 0, sizeof(none));
      none.ss_flags = SS_DISABLE;
      (void)sigaltstack(&none, nullptr);
      std::free(memory);
    }
  }
};

// SIGUSR1, sent by the watchdog: where this thread is, as program offsets. The return addresses
// are found by scanning the top of the stack (the driver is not built with frame pointers), and
// only to the end of the 16 KiB page the stack pointer is in: what follows a thread's stack may be
// unmapped, and reading on from a nearly empty stack faulted (the first console run to show the
// interface was ended by this handler).
void SampleSignalHandler(int, siginfo_t*, void* ctx)
{
  const ucontext_t* const context = static_cast<const ucontext_t*>(ctx);
  if (!context)
    return;
  const mcontext_t& mc = context->uc_mcontext;
  Line("  -- thread %p:", reinterpret_cast<void*>(pthread_self()));
  Address("rip", static_cast<uintptr_t>(mc.mc_rip));
  const uintptr_t base = reinterpret_cast<uintptr_t>(&__executable_start[0]);
  const uintptr_t stack = static_cast<uintptr_t>(mc.mc_rsp) & ~uintptr_t(7);
  const uintptr_t* words = reinterpret_cast<const uintptr_t*>(stack);
  const int count = static_cast<int>((((stack | uintptr_t(0x3fff)) + 1) - stack) / sizeof(uintptr_t));
  int found = 0;
  for (int i = 0; i < count && found < 28; i++)
  {
    const uintptr_t value = words[i];
    if (base != 0 && value >= base && value - base < 0x20000000u)
    {
      char label[16];
      std::snprintf(label, sizeof(label), "s+%d", i * 8);
      Address(label, value);
      found++;
    }
  }
}

// If the emulator's log stops growing, where are the threads? This is what turns a silent hang
// into something that can be looked up. Only with a file named watchdog.txt in the title folder: an
// idle interface logs nothing either. At most five samplings per run.
void* WatchdogMain(void*)
{
  off_t last_size = -1;
  int quiet_seconds = 0;
  int samplings = 0;
  for (;;)
  {
    sceKernelUsleep(1000000);
    char path[300];
    std::snprintf(path, sizeof(path), "%s/crux2-boot.log", ps5px_title_dir());
    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    if (st.st_size != last_size)
    {
      last_size = st.st_size;
      quiet_seconds = 0;
      continue;
    }
    if (++quiet_seconds < 6 || samplings >= 5)
      continue;
    quiet_seconds = 0;
    samplings++;
    Line("==== watchdog: crux2-boot.log has not grown for 6 s (sampling %d of 5) ====", samplings);
    Address("program", reinterpret_cast<uintptr_t>(&__executable_start[0]));
    {
      const std::lock_guard<std::mutex> lock(s_threads_mutex);
      for (int i = 0; i < MAX_THREADS; i++)
      {
        if (s_thread_used[i])
          (void)pthread_kill(s_threads[i], SIGUSR1);
      }
    }
    sceKernelUsleep(500000);
    Line("==== end of the sampling ====");
    if (const int fd = ps5px_log_fd(); fd >= 0)
      fsync(fd);
  }
  return nullptr;
}

} // namespace

extern "C" void ps5px_install_crash_stack(void)
{
  static thread_local CrashStack stack;
  if (stack.memory)
    return;
  void* const memory = std::malloc(CrashStack::SIZE);
  if (!memory)
    return;
  stack_t ss;
  std::memset(&ss, 0, sizeof(ss));
  ss.ss_sp = static_cast<char*>(memory);
  ss.ss_size = CrashStack::SIZE;
  if (sigaltstack(&ss, nullptr) != 0)
  {
    std::free(memory);
    return;
  }
  stack.memory = memory;

  const std::lock_guard<std::mutex> lock(s_threads_mutex);
  for (int i = 0; i < MAX_THREADS; i++)
  {
    if (!s_thread_used[i])
    {
      s_thread_used[i] = true;
      s_threads[i] = pthread_self();
      stack.slot = i;
      break;
    }
  }
}

// abort(), exit() and _exit() end a title without a signal, so without a report: the link routes the
// title's calls here first (--wrap in tools/link-title.sh), and the boot log says who called.
namespace {
void ReportEnding(const char* what, void* caller, const uintptr_t* frame)
{
  Line("==== %s was called ====", what);
  Address("program", reinterpret_cast<uintptr_t>(&__executable_start[0]));
  Address("caller", reinterpret_cast<uintptr_t>(caller));
  for (int depth = 0; depth < 24 && frame; depth++)
  {
    const uintptr_t next = frame[0];
    const uintptr_t return_address = frame[1];
    if (return_address == 0)
      break;
    char label[16];
    std::snprintf(label, sizeof(label), "#%d", depth);
    Address(label, return_address);
    if (next <= reinterpret_cast<uintptr_t>(frame) || next - reinterpret_cast<uintptr_t>(frame) > 0x100000 ||
        (next & 7) != 0)
    {
      break;
    }
    frame = reinterpret_cast<const uintptr_t*>(next);
  }
  Line("==== end of the report ====");
  if (const int fd = ps5px_log_fd(); fd >= 0)
    fsync(fd);
}
} // namespace

extern "C" {
void __real_abort(void) __attribute__((noreturn));
void __real_exit(int status) __attribute__((noreturn));
void __real__exit(int status) __attribute__((noreturn));
void __real__Exit(int status) __attribute__((noreturn));

__attribute__((noreturn, noinline)) void __wrap_abort(void)
{
  ReportEnding("abort()", __builtin_return_address(0), static_cast<const uintptr_t*>(__builtin_frame_address(0)));
  __real_abort();
}

__attribute__((noreturn, noinline)) void __wrap_exit(int status)
{
  Line("  exit status %d", status);
  ReportEnding("exit()", __builtin_return_address(0), static_cast<const uintptr_t*>(__builtin_frame_address(0)));
  __real_exit(status);
}

__attribute__((noreturn, noinline)) void __wrap__exit(int status)
{
  Line("  exit status %d", status);
  ReportEnding("_exit()", __builtin_return_address(0), static_cast<const uintptr_t*>(__builtin_frame_address(0)));
  __real__exit(status);
}

__attribute__((noreturn, noinline)) void __wrap__Exit(int status)
{
  Line("  exit status %d", status);
  ReportEnding("_Exit()", __builtin_return_address(0), static_cast<const uintptr_t*>(__builtin_frame_address(0)));
  __real__Exit(status);
}
}

bool CrashHandler::Install()
{
  ps5px_install_crash_stack();

  // SIGSEGV and SIGBUS are taken over by the emulator's page fault handler while a game runs
  // (fastmem, and the recompiler's code pages); what it does not handle it passes on to
  // CrashSignalHandler.
  for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS})
    InstallFor(signal);

  struct sigaction sample;
  std::memset(&sample, 0, sizeof(sample));
  sigemptyset(&sample.sa_mask);
  sample.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sample.sa_sigaction = &SampleSignalHandler;
  (void)sigaction(SIGUSR1, &sample, nullptr);

  if (PS5Platform::HasFlagFile("watchdog.txt"))
  {
    pthread_t watchdog;
    if (pthread_create(&watchdog, nullptr, &WatchdogMain, nullptr) == 0)
      pthread_detach(watchdog);
  }
  return true;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
  const size_t length = std::min(dump_directory.size(), sizeof(s_dump_directory) - 1);
  std::memcpy(s_dump_directory, dump_directory.data(), length);
  s_dump_directory[length] = '\0';
}

void CrashHandler::WriteDumpForCaller()
{
  Line("---- report requested by the emulator ----");

  // The return addresses of the caller's stack.
  const uintptr_t* frame = static_cast<const uintptr_t*>(__builtin_frame_address(0));
  for (int depth = 0; depth < 24 && frame; depth++)
  {
    const uintptr_t next = frame[0];
    const uintptr_t return_address = frame[1];
    if (return_address == 0)
      break;
    char label[16];
    std::snprintf(label, sizeof(label), "#%d", depth);
    Address(label, return_address);
    if (next <= reinterpret_cast<uintptr_t>(frame) || next - reinterpret_cast<uintptr_t>(frame) > 0x100000 ||
        (next & 7) != 0)
    {
      break;
    }
    frame = reinterpret_cast<const uintptr_t*>(next);
  }
  if (const int fd = ps5px_log_fd(); fd >= 0)
    fsync(fd);
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
  // A second fault (in another thread, or while this report walks a broken stack): the console
  // takes it from here.
  if (s_in_crash)
  {
    RestoreDefault(signal);
    return;
  }
  s_in_crash = 1;

  const ucontext_t* const context = static_cast<const ucontext_t*>(ctx);
  Line("==== Crux2 stopped: %s ====", SignalName(signal));
  Line("  signal %d, code %d, fault address 0x%016llx", signal, siginfo ? siginfo->si_code : 0,
       static_cast<unsigned long long>(siginfo ? reinterpret_cast<uintptr_t>(siginfo->si_addr) : 0));
  Address("program", reinterpret_cast<uintptr_t>(&__executable_start[0]));

  if (context)
  {
    const mcontext_t& mc = context->uc_mcontext;
    Address("rip", static_cast<uintptr_t>(mc.mc_rip));
    Line("  rsp 0x%016llx  rbp 0x%016llx  err 0x%llx", static_cast<unsigned long long>(mc.mc_rsp),
         static_cast<unsigned long long>(mc.mc_rbp), static_cast<unsigned long long>(mc.mc_err));
    Line("  rax 0x%016llx  rbx 0x%016llx  rcx 0x%016llx  rdx 0x%016llx", static_cast<unsigned long long>(mc.mc_rax),
         static_cast<unsigned long long>(mc.mc_rbx), static_cast<unsigned long long>(mc.mc_rcx),
         static_cast<unsigned long long>(mc.mc_rdx));
    Line("  rsi 0x%016llx  rdi 0x%016llx  r8  0x%016llx  r9  0x%016llx", static_cast<unsigned long long>(mc.mc_rsi),
         static_cast<unsigned long long>(mc.mc_rdi), static_cast<unsigned long long>(mc.mc_r8),
         static_cast<unsigned long long>(mc.mc_r9));
    Line("  r10 0x%016llx  r11 0x%016llx  r12 0x%016llx  r13 0x%016llx", static_cast<unsigned long long>(mc.mc_r10),
         static_cast<unsigned long long>(mc.mc_r11), static_cast<unsigned long long>(mc.mc_r12),
         static_cast<unsigned long long>(mc.mc_r13));
    Line("  r14 0x%016llx  r15 0x%016llx", static_cast<unsigned long long>(mc.mc_r14),
         static_cast<unsigned long long>(mc.mc_r15));
    if (const int fd = ps5px_log_fd(); fd >= 0)
      fsync(fd);

    // The return addresses, by frame pointer (the title is built with them). The registers above
    // are already on disk should a frame pointer lead nowhere: the generated code of a running
    // game keeps none, and a fault inside it ends the walk at once.
    Line("  stack:");
    uintptr_t frame = static_cast<uintptr_t>(mc.mc_rbp);
    const uintptr_t stack = static_cast<uintptr_t>(mc.mc_rsp);
    for (int depth = 0; depth < 32; depth++)
    {
      // A frame is above the stack pointer, within the 2 MiB of a thread's stack, and aligned.
      if (frame < stack || frame - stack > 0x200000 || (frame & 7) != 0)
        break;
      const uintptr_t* const words = reinterpret_cast<const uintptr_t*>(frame);
      const uintptr_t next = words[0];
      const uintptr_t return_address = words[1];
      if (return_address == 0)
        break;
      char label[16];
      std::snprintf(label, sizeof(label), "#%d", depth);
      Address(label, return_address);
      if (next <= frame)
        break;
      frame = next;
    }
  }
  Line("==== end of the report ====");
  if (const int fd = ps5px_log_fd(); fd >= 0)
    fsync(fd);

  // Back to the console: returning re-executes the faulting instruction with the default action
  // in place, and the console closes the title with its own error screen.
  RestoreDefault(signal);
}
