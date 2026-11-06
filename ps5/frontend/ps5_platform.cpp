// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the console side of the port.

#include "ps5_platform.h"
#include "ps5_priority.h"

#include "ps5_open_up.h"

#include "platform.h"
#include "ps5_vfs.h"
#include "ps5_shares.h"

#include "../elevation/elevation.hpp"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
int sceSystemServiceLoadExec(const char* path, const char* const* argv);
int sceKernelUsleep(unsigned microseconds);
int sceKernelSendNotificationRequest(unsigned device, void* request, size_t size, int blocking);
int sceKernelDebugOutText(int channel, const char* text);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceGetStatus(void* status);
}

namespace {

constexpr char TITLE_ID[] = "PPSA99249";
constexpr char SANDBOX_DIR[] = "/app0";

// Fixed storage: these are read by the crash report, in a signal handler.
char s_title_dir[128] = "/app0";
char s_ca_bundle[192] = "/app0/resources/cacert.pem";
int s_log_fd = -1;
double s_start_time = 0.0;

bool s_wants_usb = true;
bool s_elevated = false;
elevation::Status s_elevation_status = elevation::Status::unavailable;
bool s_elevation_tried = false;

bool Exists(const char* path)
{
  struct stat st;
  return (stat(path, &st) == 0);
}

// [PS5] USBDrives in inis/PCSX2.ini, read by hand (ReadEarlySettings, below): the settings are
// loaded long after the one moment the sandbox can be left (before the process has a second
// thread). A file named usb-off.txt in the title folder says no as well, for when the interface
// cannot be reached to change the setting.
struct LogChoice
{
  bool on = true, boot = true, driver = true, klog = true;
  std::string place;
};

bool IniFalse(std::string_view value)
{
  return value == "false" || value == "False" || value == "0";
}

// The [PS5] keys of inis/PCSX2.ini the start needs before the settings are loaded.
void ReadEarlySettings(const char* title_dir, bool* wants_usb, LogChoice* logs)
{
  char path[256];
  std::snprintf(path, sizeof(path), "%s/inis/PCSX2.ini", title_dir);
  FILE* const fp = std::fopen(path, "r");
  if (!fp)
    return;
  bool in_section = false;
  char line[512];
  while (std::fgets(line, sizeof(line), fp))
  {
    const std::string_view text = StringUtil::StripWhitespace(std::string_view(line));
    if (text.empty())
      continue;
    if (text.front() == '[')
    {
      in_section = (text == "[PS5]");
      continue;
    }
    if (!in_section)
      continue;
    const size_t equals = text.find('=');
    if (equals == std::string_view::npos)
      continue;
    const std::string_view key = StringUtil::StripWhitespace(text.substr(0, equals));
    const std::string_view value = StringUtil::StripWhitespace(text.substr(equals + 1));
    if (key == "USBDrives" && wants_usb)
      *wants_usb = !IniFalse(value);
    else if (logs && key == "Logs")
      logs->on = !IniFalse(value);
    else if (logs && key == "LogBoot")
      logs->boot = !IniFalse(value);
    else if (logs && key == "LogDriver")
      logs->driver = !IniFalse(value);
    else if (logs && key == "LogKlog")
      logs->klog = !IniFalse(value);
    else if (logs && key == "LogPlace")
      logs->place = std::string(value);
  }
  std::fclose(fp);
}

bool ReadWantsUSB(const char* title_dir)
{
  char path[256];
  std::snprintf(path, sizeof(path), "%s/usb-off.txt", title_dir);
  if (Exists(path))
    return false;
  bool wanted = true;
  ReadEarlySettings(title_dir, &wanted, nullptr);
  return wanted;
}

const char* ElevationStatusText(elevation::Status status)
{
  if (!s_elevation_tried)
    return "not asked for";

  switch (status)
  {
    case elevation::Status::ok:
      return "granted";
    case elevation::Status::ok_unbalanced:
      return "granted the old way (the helper's root was not the console's: Crux2's exit releases two references to the console's root it never took)";
    case elevation::Status::invalid_request:
      return "the helper refused the request";
    case elevation::Status::unsupported_version:
      return "the helper is of another protocol version";
    case elevation::Status::unsupported_capability:
      return "the helper does not know the capability";
    case elevation::Status::target_mismatch:
      return "the helper was built for another title ID";
    case elevation::Status::unavailable:
      return "sandbox-elevator.elf is missing, or the kernel state is not available to the helper";
    case elevation::Status::prepare_failed:
      return "the credentials could not be prepared";
    case elevation::Status::apply_failed:
      return "the change failed and was undone";
    case elevation::Status::rollback_failed:
      return "the change failed and could not be undone";
    case elevation::Status::transport_error:
      return "no payload loader (elfldr) answered on port 9021";
    case elevation::Status::protocol_error:
      return "the helper's answer was not understood";
  }
  return "unknown";
}

bool IsTitleFolder(const char* directory)
{
  char probe[224];
  std::snprintf(probe, sizeof(probe), "%s/eboot.bin", directory);
  if (!Exists(probe))
    return false;
  std::snprintf(probe, sizeof(probe), "%s/sce_sys/param.json", directory);
  return Exists(probe);
}

// Where the title's folder is now. Inside the sandbox it is /app0. Outside it (the helper has
// replaced the process's root with the console's), /app0 is gone and the folder is wherever it was
// installed: /data/homebrew/<TITLE_ID> for a title started by ShadowMount, else the sandbox's own
// mount of it.
bool FindTitleFolder(char* out, size_t size)
{
  static constexpr const char* candidates[] = {
    "/app0",
    "/data/homebrew/PPSA99249",
    "/mnt/sandbox/PPSA99249_000/app0",
    "/user/app/PPSA99249",
    "/mnt/ext0/homebrew/PPSA99249",
    "/mnt/ext1/homebrew/PPSA99249",
    "/mnt/usb0/homebrew/PPSA99249",
    "/mnt/usb1/homebrew/PPSA99249",
  };
  for (const char* const candidate : candidates)
  {
    if (IsTitleFolder(candidate))
    {
      std::snprintf(out, size, "%s", candidate);
      return true;
    }
  }
  return false;
}

void VFSLog(const char* line)
{
  ps5px_log("%s", line);
}

constexpr char NETWORK_CFG_TEMPLATE[] =
  "# Crux2 - games on network shares (SMB or NFS).\n"
  "#\n"
  "# One \"path\" line for each folder of games: server/share/folder, with the server given by\n"
  "# its IP address (do not rely on a name being looked up on the console). Remove the # in\n"
  "# front of the example and put your own address there. Each folder then appears in the game\n"
  "# list, and in the file browser as /smb/<the folder's name>.\n"
  "#\n"
  "# For an open share leave user = guest and the password empty; otherwise give the account\n"
  "# on the NAS or PC. The share is only read. Restart Crux2 after changing this file.\n"
  "\n"
  "# path = 192.168.1.10/Games/PS2\n"
  "user = guest\n"
  "password =\n"
  "\n"
  "# NFS shares (a NAS or a Linux PC): an nfs:// line, export and folder after the address. uid and\n"
  "# gid are the account the NAS is asked as (1000 when not given).\n"
  "# path = nfs://192.168.1.10/volume1/games/ps2\n"
  "# uid = 1000\n"
  "# gid = 1000\n";

// ---- the logs (v1.0.19) ------------------------------------------------------------------------
//
// What is logged and where is the player's choice (Settings > Debug > Logs, read here by hand from
// inis/PCSX2.ini: the boot log starts long before the settings are loaded):
//   [PS5] Logs      false: no logs at all (no file, no copy in klog, the driver's messages dropped)
//   [PS5] LogBoot   false: no crux2-boot.log
//   [PS5] LogDriver false: the graphics driver's messages (standard error) dropped
//   [PS5] LogKlog   false: no copy of the lines in the console's kernel log
//   [PS5] LogPlace  the folder the logs go to ("": the title folder; a USB drive's Crux2-logs). A
//                   folder that is not there at the start (the drive is out) keeps the boot log in
//                   memory for the session, where Settings > Debug shows it.
//
// The lines go through memory to a writer thread of their own (from OGBox 0.0.3): a line written
// straight to the file waited for the file system on the thread that logged it (OGBox measured 600
// to 900 ms a line while a game's own writes were in flight, and Crux2 logs from the EE and GS
// threads while states and memory cards are written). The writer writes what is there every 100 ms
// at most, and the file is synced only at the moments that matter (a game starts or closes, a fatal
// error, the shutdown: ps5px_log_checkpoint): up to the first v1.0.19 builds it was synced every
// two seconds, which wrote the same last block of the console's drive again and again for a few
// hundred KB of text; the console writes the rest out by itself within about 30 s. Fixed storage:
// the crash report drains it, in a signal handler (ps5px_log_fd).
enum class LogMode
{
  Direct, // before the writer runs: straight into the file
  File,   // through the writer into the file
  Memory, // kept in memory only (the chosen folder was not there)
  Off,    // no boot log
};
LogChoice s_log_choice;
std::atomic<LogMode> s_log_mode{LogMode::Direct};
char s_log_path[256] = "";
char s_log_previous[256] = "";
char s_crash_path[256] = "";
int s_crash_fd = -1;

constexpr size_t LOG_RING = size_t(2) << 20;
char s_ring[LOG_RING];
size_t s_ring_head = 0, s_ring_length = 0; // the oldest byte, and how many
unsigned long long s_ring_dropped = 0;
pthread_mutex_t s_ring_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_ring_ready = PTHREAD_COND_INITIALIZER;
pthread_cond_t s_ring_empty = PTHREAD_COND_INITIALIZER;
std::atomic<bool> s_writer_up{false};
std::atomic<bool> s_sync_wanted{false}; // a checkpoint: the writer syncs the file after what it writes
bool s_writer_busy = false; // a chunk taken out, being written
int s_pipe_read = -1;       // standard error's pipe, read by a thread of its own

// Into the ring. Kept in memory only, the oldest lines make room for new ones; otherwise a full
// ring drops the new ones (and the file says how many) rather than wait for the file system.
void RingPut(const char* data, size_t length)
{
  if (length > LOG_RING)
  {
    data += length - LOG_RING;
    length = LOG_RING;
  }
  pthread_mutex_lock(&s_ring_lock);
  if (s_ring_length + length > LOG_RING)
  {
    if (s_log_mode.load(std::memory_order_relaxed) == LogMode::Memory)
    {
      const size_t room = s_ring_length + length - LOG_RING;
      s_ring_head = (s_ring_head + room) % LOG_RING;
      s_ring_length -= room;
    }
    else
    {
      s_ring_dropped += length;
      pthread_cond_signal(&s_ring_ready);
      pthread_mutex_unlock(&s_ring_lock);
      return;
    }
  }
  size_t at = (s_ring_head + s_ring_length) % LOG_RING;
  for (size_t done = 0; done < length;)
  {
    const size_t part = std::min(length - done, LOG_RING - at);
    std::memcpy(s_ring + at, data + done, part);
    done += part;
    at = (at + part) % LOG_RING;
  }
  s_ring_length += length;
  pthread_cond_signal(&s_ring_ready);
  pthread_mutex_unlock(&s_ring_lock);
}

// A line of the boot log, by the way the log goes now.
void LogWrite(const char* data, size_t length)
{
  switch (s_log_mode.load(std::memory_order_acquire))
  {
    case LogMode::Direct:
      if (s_log_fd >= 0)
        (void)!write(s_log_fd, data, length);
      break;
    case LogMode::File:
    case LogMode::Memory:
      RingPut(data, length);
      break;
    case LogMode::Off:
      break;
  }
}

// What the ring holds into the file. The caller holds the lock, which is let go while it writes.
void RingWriteOut()
{
  static char chunk[256 * 1024];
  while (s_ring_length)
  {
    const size_t length = std::min({s_ring_length, sizeof(chunk), LOG_RING - s_ring_head});
    std::memcpy(chunk, s_ring + s_ring_head, length);
    s_ring_head = (s_ring_head + length) % LOG_RING;
    s_ring_length -= length;
    s_writer_busy = true;
    pthread_mutex_unlock(&s_ring_lock);
    if (s_log_fd >= 0)
      (void)!write(s_log_fd, chunk, length);
    pthread_mutex_lock(&s_ring_lock);
    s_writer_busy = false;
  }
  if (s_ring_dropped)
  {
    char note[96];
    const int length =
      std::snprintf(note, sizeof(note), "[log: %llu bytes of lines dropped: the file system was slow]\n", s_ring_dropped);
    s_ring_dropped = 0;
    pthread_mutex_unlock(&s_ring_lock);
    if (s_log_fd >= 0 && length > 0)
      (void)!write(s_log_fd, note, static_cast<size_t>(length));
    pthread_mutex_lock(&s_ring_lock);
  }
  pthread_cond_broadcast(&s_ring_empty);
}

timespec Deadline(long milliseconds)
{
  timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += milliseconds * 1000000L;
  until.tv_sec += until.tv_nsec / 1000000000L;
  until.tv_nsec %= 1000000000L;
  return until;
}

void* LogWriterMain(void*)
{
  pthread_mutex_lock(&s_ring_lock);
  for (;;)
  {
    if (!s_ring_length || s_log_mode.load(std::memory_order_relaxed) != LogMode::File)
    {
      const timespec until = Deadline(100);
      pthread_cond_timedwait(&s_ring_ready, &s_ring_lock, &until);
    }
    if (s_log_mode.load(std::memory_order_relaxed) == LogMode::File)
    {
      RingWriteOut();
      if (s_sync_wanted.exchange(false, std::memory_order_acq_rel) && s_log_fd >= 0)
      {
        pthread_mutex_unlock(&s_ring_lock);
        fsync(s_log_fd);
        pthread_mutex_lock(&s_ring_lock);
      }
    }
  }
  return nullptr;
}

// Standard error's pipe into the ring: a line printed there never waits for the file either.
void* LogPipeMain(void*)
{
  char buffer[16384];
  for (;;)
  {
    const ssize_t got = read(s_pipe_read, buffer, sizeof(buffer));
    if (got > 0)
      RingPut(buffer, static_cast<size_t>(got));
    else if (got == 0 || (errno != EINTR && errno != EAGAIN))
      break;
  }
  return nullptr;
}


} // namespace

// Everything logged so far into the file and onto the disk, now: a fatal error, the shutdown.
// Waits up to 5 s for the log's writer.
extern "C" void ps5px_log_flush(void)
{
  const LogMode mode = s_log_mode.load(std::memory_order_acquire);
  if (mode == LogMode::File && s_writer_up.load(std::memory_order_acquire))
  {
    std::fflush(stderr);
    if (s_pipe_read >= 0)
      sceKernelUsleep(20000); // (the pipe's last lines into the ring)
    pthread_mutex_lock(&s_ring_lock);
    for (int i = 0; i < 100 && (s_ring_length || s_writer_busy); i++)
    {
      pthread_cond_signal(&s_ring_ready);
      const timespec until = Deadline(50);
      pthread_cond_timedwait(&s_ring_empty, &s_ring_lock, &until);
    }
    pthread_mutex_unlock(&s_ring_lock);
  }
  if ((mode == LogMode::File || mode == LogMode::Direct) && s_log_fd >= 0)
    fsync(s_log_fd);
}

// A moment that matters (a game starts or closes): the writer syncs the file after the lines it has,
// on its own thread; the caller does not wait.
extern "C" void ps5px_log_checkpoint(void)
{
  const LogMode mode = s_log_mode.load(std::memory_order_acquire);
  if (mode == LogMode::File && s_writer_up.load(std::memory_order_acquire))
  {
    s_sync_wanted.store(true, std::memory_order_release);
    pthread_mutex_lock(&s_ring_lock);
    pthread_cond_signal(&s_ring_ready);
    pthread_mutex_unlock(&s_ring_lock);
  }
  else if (mode == LogMode::Direct && s_log_fd >= 0)
  {
    fsync(s_log_fd);
  }
}

// The boot log as it is now, for Settings > Debug when it is kept in memory only: true and the
// text, or false when it has a file (or there is none).
bool PS5Platform::LogInMemory(std::string* text)
{
  if (s_log_mode.load(std::memory_order_acquire) != LogMode::Memory)
    return false;
  pthread_mutex_lock(&s_ring_lock);
  text->clear();
  text->reserve(s_ring_length);
  const size_t first = std::min(s_ring_length, LOG_RING - s_ring_head);
  text->append(s_ring + s_ring_head, first);
  text->append(s_ring, s_ring_length - first);
  pthread_mutex_unlock(&s_ring_lock);
  return true;
}

std::string PS5Platform::LogPath()
{
  return s_log_path;
}

std::string PS5Platform::LogPreviousPath()
{
  if (s_log_previous[0] != '\0')
    return s_log_previous;
  // No file of its own this run: a crash of the run before that kept its log in memory.
  char kept[256];
  std::snprintf(kept, sizeof(kept), "%s/crux2-crash.previous.log", s_title_dir);
  return Exists(kept) ? std::string(kept) : std::string();
}

// The crash report's descriptor of the log, which it writes its lines to itself (in a signal
// handler): what the ring still holds goes in first, unless another thread holds the ring (a
// signal handler can't wait for it). A boot log kept in memory goes into crux2-crash.log in the
// title folder with the report: a crash is the one time it is written to the console's drive.
extern "C" int ps5px_log_fd(void)
{
  const LogMode mode = s_log_mode.load(std::memory_order_acquire);
  if (mode == LogMode::Off)
    return -1;
  if (mode == LogMode::Memory)
  {
    if (s_crash_fd < 0 && s_crash_path[0] != '\0')
    {
      s_crash_fd = open(s_crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
      if (s_crash_fd >= 0 && pthread_mutex_trylock(&s_ring_lock) == 0)
      {
        const size_t first = std::min(s_ring_length, LOG_RING - s_ring_head);
        (void)!write(s_crash_fd, s_ring + s_ring_head, first);
        (void)!write(s_crash_fd, s_ring, s_ring_length - first);
        pthread_mutex_unlock(&s_ring_lock);
      }
    }
    return s_crash_fd;
  }
  if (s_log_fd >= 0 && s_writer_up.load(std::memory_order_acquire) && pthread_mutex_trylock(&s_ring_lock) == 0)
  {
    while (s_ring_length)
    {
      const size_t length = std::min(s_ring_length, LOG_RING - s_ring_head);
      (void)!write(s_log_fd, s_ring + s_ring_head, length);
      s_ring_head = (s_ring_head + length) % LOG_RING;
      s_ring_length -= length;
    }
    pthread_mutex_unlock(&s_ring_lock);
  }
  return s_log_fd;
}

extern "C" void ps5px_log(const char* format, ...)
{
  if (!s_log_choice.on)
    return;
  char text[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  char line[1100];
  int length;
  if (s_log_choice.klog)
  {
    length = std::snprintf(line, sizeof(line), "[Crux2] %s\n", text);
    if (length > 0)
      (void)sceKernelDebugOutText(0, line);
  }

  if (s_log_choice.boot)
  {
    length = std::snprintf(line, sizeof(line), "[%8.3f] %s\n", now_seconds() - s_start_time, text);
    if (length > 0)
      LogWrite(line, std::min<size_t>(static_cast<size_t>(length), sizeof(line) - 1));
  }
}

// Called by the graphics driver of a diagnostic build (ps5/patches/mesa) at each step of its
// start-up: written at once, so the last line names the step a silent stop happened in.
extern "C" void ps5_title_breadcrumb(const char* text)
{
  ps5px_log("radv: %s", text ? text : "");
}

extern "C" void ps5px_notify(const char* message)
{
  // The console's notification request: 45 reserved bytes, then the text.
  struct NotificationRequest
  {
    unsigned char reserved[45];
    char message[3075];
  };
  static NotificationRequest request;
  std::memset(&request, 0, sizeof(request));
  std::snprintf(request.message, sizeof(request.message), "Crux2\n%s", message);
  (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

extern "C" int ps5px_safety_state_write(void);

extern "C" void ps5_title_fatal(const char* message)
{
  static bool s_in_fatal = false;
  if (!s_in_fatal)
  {
    s_in_fatal = true;
    ps5px_log("FATAL: %s", message ? message : "(no message)");
    ps5px_notify(message ? message : "A fatal error stopped the emulator.");
    // v1.0.19: the game's safety state, kept in memory, onto the drive (ps5_extras.cpp).
    if (ps5px_safety_state_write())
      ps5px_log("(the game's safety state was written: the library offers to go on from it)");
    ps5px_log_flush();
  }

  // The shell closes the title asynchronously: wait for it, and never return.
  (void)sceSystemServiceLoadExec("exit", nullptr);
  for (;;)
    sceKernelUsleep(100000);
}

void PS5Platform::EarlyInit()
{
  s_start_time = now_seconds();

  // USB drives are under /mnt, which a title's sandbox hides. The bundled helper, sent to the
  // payload loader on port 9021, gives this process the whole file system. This has to happen now:
  // the helper changes the process's credentials, which is only safe with one thread. (Nothing has
  // been logged yet for the same reason: klog capture starts a thread.)
  s_wants_usb = ReadWantsUSB(SANDBOX_DIR);
  if (s_wants_usb)
  {
    s_elevation_tried = true;
    s_elevation_status = elevation::request(elevation::Capability::filesystem, "/app0/sandbox-elevator.elf");
    s_elevated = (s_elevation_status == elevation::Status::ok || s_elevation_status == elevation::Status::ok_unbalanced);
  }

  // The title's folder, under whichever name reaches it now.
  const bool found = FindTitleFolder(s_title_dir, sizeof(s_title_dir));
  std::snprintf(s_ca_bundle, sizeof(s_ca_bundle), "%s/resources/cacert.pem", s_title_dir);

  // The logs: what is logged and where (Settings > Debug > Logs; read by hand, as the settings are
  // loaded long after the boot log starts). Off: no file, no copy in klog, the driver's messages
  // dropped. The driver's messages go into the boot log, so without it they are dropped too.
  ReadEarlySettings(s_title_dir, nullptr, &s_log_choice);
  if (!s_log_choice.on)
    s_log_choice.boot = s_log_choice.driver = s_log_choice.klog = false;
  if (!s_log_choice.boot)
    s_log_choice.driver = false;
  std::snprintf(s_crash_path, sizeof(s_crash_path), "%s/crux2-crash.log", s_title_dir);
  {
    // A crash of a run whose boot log was kept in memory left crux2-crash.log: it becomes the run
    // before's (once), so that the library still tells of it.
    char kept[256];
    std::snprintf(kept, sizeof(kept), "%s/crux2-crash.previous.log", s_title_dir);
    if (access(s_crash_path, F_OK) == 0)
    {
      (void)unlink(kept);
      (void)rename(s_crash_path, kept);
    }
    else if (access(kept, F_OK) == 0)
    {
      (void)unlink(kept);
    }
  }
  bool place_missing = false;
  std::string folder = s_title_dir;
  if (!s_log_choice.place.empty())
  {
    // A folder of the player's choice (a USB drive's Crux2-logs): made when its drive is there.
    folder = s_log_choice.place;
    if (mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST)
      place_missing = true;
    struct stat st;
    if (stat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      place_missing = true;
  }
  char log_path[256], previous_path[256];
  std::snprintf(log_path, sizeof(log_path), "%s/crux2-boot.log", folder.c_str());
  std::snprintf(previous_path, sizeof(previous_path), "%s/crux2-boot.previous.log", folder.c_str());
  if (s_log_choice.boot && !place_missing)
  {
    if (s_log_choice.place.empty())
    {
      // The builds before v0.6.0 named them pspcsx2-boot.log: the last of those becomes this run's
      // "run before" (a crash it ended in is still told), the older one goes.
      char before[192];
      std::snprintf(before, sizeof(before), "%s/pspcsx2-boot.previous.log", s_title_dir);
      (void)unlink(before);
      std::snprintf(before, sizeof(before), "%s/pspcsx2-boot.log", s_title_dir);
      if (access(log_path, F_OK) != 0)
        (void)rename(before, log_path);
      else
        (void)unlink(before);
    }
    // The previous run's log is kept, since a crash report is the last thing in it. (It goes first:
    // a rename onto a file is not trusted on an exFAT drive.)
    (void)unlink(previous_path);
    (void)rename(log_path, previous_path);
    s_log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    MakePublic(log_path, false);
    MakePublic(previous_path, false);
  }
  if (!s_log_choice.boot)
  {
    s_log_mode.store(LogMode::Off, std::memory_order_release);
  }
  else if (s_log_fd >= 0)
  {
    std::snprintf(s_log_path, sizeof(s_log_path), "%s", log_path);
    std::snprintf(s_log_previous, sizeof(s_log_previous), "%s", previous_path);
    s_log_mode.store(LogMode::Direct, std::memory_order_release);
  }
  else
  {
    // The chosen folder is not there (its drive is out) or the file can't be made: the boot log is
    // kept in memory for this session, where Settings > Debug shows it.
    s_log_mode.store(LogMode::Memory, std::memory_order_release);
  }

  // The writer thread, when there is a file to write to.
  if (s_log_mode.load(std::memory_order_acquire) == LogMode::Direct)
  {
    pthread_t writer;
    if (pthread_create(&writer, nullptr, &LogWriterMain, nullptr) == 0)
    {
      pthread_detach(writer);
      s_writer_up.store(true, std::memory_order_release);
      s_log_mode.store(LogMode::File, std::memory_order_release);
    }
  }

  // Standard error (the graphics driver's messages) into the boot log, each line as it is printed:
  // what the driver says before a stop is then there. Through a pipe read by a thread of its own,
  // so that no line waits for the file system; without that thread, a second descriptor of the
  // file, as before v1.0.19. (The console refuses dup2 onto descriptor 2 to a title, so the stream
  // is moved to another descriptor.) The driver's messages not wanted: into /dev/null.
  {
    int second = -1;
    if (s_log_choice.driver)
    {
      const LogMode mode = s_log_mode.load(std::memory_order_acquire);
      int pipe_fds[2] = {-1, -1};
      if ((mode == LogMode::File || mode == LogMode::Memory) && pipe(pipe_fds) == 0)
      {
        s_pipe_read = pipe_fds[0];
        pthread_t reader;
        if (pthread_create(&reader, nullptr, &LogPipeMain, nullptr) == 0)
        {
          pthread_detach(reader);
          second = pipe_fds[1];
        }
        else
        {
          close(pipe_fds[0]);
          close(pipe_fds[1]);
          s_pipe_read = -1;
        }
      }
      if (second < 0 && mode == LogMode::Direct && s_log_fd >= 0)
        second = open(log_path, O_WRONLY | O_APPEND);
    }
    else
    {
      second = open("/dev/null", O_WRONLY);
    }
    FILE* const stream = (second >= 0) ? fdopen(second, "a") : nullptr;
    if (stream)
    {
      setvbuf(stream, nullptr, _IOLBF, 0);
      stderr = stream;
    }
    else if (second >= 0)
    {
      close(second);
    }
  }

  // The shell's splash covers the title's frames until it is dismissed.
  (void)sceSystemServiceHideSplashScreen();

  const std::time_t now = std::time(nullptr);
  char when[64] = "";
  if (const std::tm* const tm = std::gmtime(&now))
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", tm);
  ps5px_log("Crux2 (%s) starts, %s", TITLE_ID, when);
  ps5px_log("title folder: %s", s_title_dir);
  {
    const LogMode mode = s_log_mode.load(std::memory_order_acquire);
    ps5px_log("logs: this log %s; the graphics driver's messages %s; a copy in klog %s; synced to the drive when a game starts "
              "or closes and at the end",
              mode == LogMode::Memory ? (place_missing ? "kept in memory (its folder is not there: is the drive in?)" : "kept in memory (its file could not be made)")
                                      : s_log_path,
              s_log_choice.driver ? "in it" : "dropped", s_log_choice.klog ? "on" : "off");
  }
  if (!s_wants_usb)
    ps5px_log("USB drives: off ([PS5] USBDrives in inis/PCSX2.ini, or usb-off.txt)");
  else
    ps5px_log("USB drives: sandbox elevation %s (status %u)%s", ElevationStatusText(s_elevation_status),
              static_cast<unsigned>(s_elevation_status), s_elevated ? "" : "; continuing inside the sandbox");

  if (!found)
  {
    // Outside the sandbox, and installed somewhere this does not know to look: nothing can be
    // loaded. The way back is to stay inside the sandbox.
    ps5_title_fatal("The title folder was not found after leaving the sandbox for USB drives. Put a file named "
                    "usb-off.txt next to eboot.bin (or install the title in /data/homebrew/PPSA99249).");
  }

  // The clock the processor runs this title at. The kernel picks a clock profile when a title
  // starts, by the SDK version the title was built with (ProsperoEden measured about 3.75 GHz with
  // version 2.00, which this title has too, and a lower profile for current retail games). A chain
  // of dependent additions takes one cycle each, so their count over the time is the clock.
  {
    double best = 0;
    for (int round = 0; round < 3; round++)
    {
      constexpr uint64_t loops = 4000000; // 8 additions each: 32 million cycles, about 10 ms
      uint64_t x = 0;
      const auto started = std::chrono::steady_clock::now();
      for (uint64_t i = 0; i < loops; i++)
      {
#if defined(__x86_64__)
        __asm__ volatile("add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                         "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0"
                         : "+r"(x));
#else
        x += 8;
        __asm__ volatile("" : "+r"(x));
#endif
      }
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      if (seconds > 0)
        best = std::max(best, static_cast<double>(loops) * 8.0 / seconds / 1e9);
    }
    ps5px_log("cpu: about %.2f GHz (dependent additions, best of 3); built with SDK version 2.00, the kernel's high-frequency "
              "profile is expected (klog [BAPM] names it)", best);
  }

  // Mesa turns RADV's disk cache off in a process whose real and effective ids differ, and leaving
  // the sandbox leaves the effective group apart from the real one: no compiled shader was then
  // kept between sessions. The effective user may set its group, so the two are made the same
  // (as ProsperoEden found and does).
  {
    const unsigned uid = static_cast<unsigned>(getuid()), euid = static_cast<unsigned>(geteuid());
    const unsigned gid = static_cast<unsigned>(getgid()), egid = static_cast<unsigned>(getegid());
    const bool fixed = (egid != gid) && setegid(getgid()) == 0;
    ps5px_log("ids: uid %u, euid %u, gid %u, egid %u%s; the driver's shader cache is %s", uid, euid, gid, egid,
              egid == gid ? "" : fixed ? " (effective group set to the real one)" : " (the effective group could not be changed)",
              (geteuid() == getuid() && getegid() == getgid()) ? "on" : "off (the ids differ)");
  }

  // The graphics driver keeps its compiled shaders in the title folder, and the platform layer its
  // temporary files: both name /app0 unless told, which is not there outside the sandbox.
  char directory[192];
  std::snprintf(directory, sizeof(directory), "%s/radv-shader-cache", s_title_dir);
  setenv("MESA_SHADER_CACHE_DIR", directory, 1);
  std::snprintf(directory, sizeof(directory), "%s/tmp", s_title_dir);
  if (mkdir(directory, 0777) == 0 || errno == EEXIST)
  {
    MakePublic(directory, true);
    setenv("TMPDIR", directory, 1);
  }
}

bool PS5Platform::HasFlagFile(const char* name)
{
  char path[256];
  std::snprintf(path, sizeof(path), "%s/%s", s_title_dir, name);
  return Exists(path);
}

namespace {
struct ResourceFile
{
  const char* name;
  long long size;
};
constexpr ResourceFile RESOURCE_MANIFEST[] = {
#include "ps5_resource_manifest.inc"
};
} // namespace

unsigned PS5Platform::CheckInstall(std::string* first_missing)
{
  unsigned missing = 0, other_size = 0;
  for (const ResourceFile& file : RESOURCE_MANIFEST)
  {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/resources/%s", s_title_dir, file.name);
    struct stat st;
    if (stat(path, &st) != 0)
    {
      if (missing == 0 && first_missing)
        *first_missing = file.name;
      if (++missing <= 40)
        ps5px_log("install: missing: resources/%s", file.name);
    }
    else if (static_cast<long long>(st.st_size) != file.size)
    {
      if (++other_size <= 40)
        ps5px_log("install: resources/%s is %lld bytes, the build's is %lld", file.name,
                  static_cast<long long>(st.st_size), file.size);
    }
  }

  const unsigned total = static_cast<unsigned>(sizeof(RESOURCE_MANIFEST) / sizeof(RESOURCE_MANIFEST[0]));
  ps5px_log("install: %u of %u resource files are there%s", total - missing, total,
            other_size ? " (some of another size than the build's: see above)" : "");
  return missing;
}

bool PS5Platform::IsElevated()
{
  return s_elevated;
}

bool PS5Platform::WantsUSB()
{
  return s_wants_usb;
}

void PS5Platform::MakePublic(const char* path, bool directory)
{
  (void)chmod(path, directory ? 0777 : 0666);
}

const std::string& PS5Platform::OutsideFolder()
{
  // /data/crux2 on the console's own storage first; then, for a whole Crux2 on a USB drive, a
  // folder crux2-data at the top of one (when the title could leave its sandbox for drives).
  static const std::string folder = []() -> std::string {
    std::vector<std::string> places = {"/data/crux2"};
    if (IsElevated())
    {
      for (int i = 0; i < 8; i++)
        places.push_back("/mnt/usb" + std::to_string(i) + "/crux2-data");
    }
    for (const std::string& outside : places)
    {
      struct stat st;
      if (stat(outside.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        continue;
      // It has to take a file: a folder that cannot be written to would lose every save.
      const std::string probe = outside + "/.crux2-write-test";
      const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd < 0)
      {
        ps5px_log("data folder: %s is there but cannot be written to (%s)", outside.c_str(), std::strerror(errno));
        continue;
      }
      close(fd);
      unlink(probe.c_str());
      MakePublic(outside.c_str(), true);
      ps5px_log("data folder: %s is there: Crux2's files are kept in it, outside the title folder", outside.c_str());
      return outside;
    }
    return std::string();
  }();
  return folder;
}

std::string PS5Platform::UserPath(const char* name)
{
  const std::string& outside = OutsideFolder();
  if (!outside.empty())
  {
    std::string there = Path::Combine(outside, name);
    if (Exists(there.c_str()))
      return there;
  }
  return Path::Combine(s_title_dir, name);
}

void PS5Platform::MountShares()
{
  // A virtual stream is stood in for by a real FILE on a file that is always there.
  static char shadow[192];
  std::snprintf(shadow, sizeof(shadow), "%s/sce_sys/param.json", s_title_dir);
  ps5vfs_set_shadow_path(shadow);
  ps5vfs_set_log(VFSLog);

  const std::string config = UserPath("network.cfg");
  if (!Exists(config.c_str()))
  {
    if (FILE* const fp = std::fopen(config.c_str(), "w"))
    {
      std::fputs(NETWORK_CFG_TEMPLATE, fp);
      std::fclose(fp);
      ps5px_log("network shares: wrote the template %s", config.c_str());
    }
    else
    {
      ps5px_log("network shares: cannot write %s: %s", config.c_str(), std::strerror(errno));
    }
  }
  MakePublic(config.c_str(), false);

  // v1.0.19: read and mounted by PS5Shares, which Settings > Folders changes them through (a
  // share's own name and account; the mounts of a file written by hand are named as before).
  std::string why;
  const int mounted = PS5Shares::LoadAndMount(config, &why);
  if (mounted < 0)
    ps5px_log("network shares: %s cannot be read: %s", config.c_str(), why.c_str());
  else
    ps5px_log("network shares: %d in network.cfg", mounted);
}

std::vector<std::string> PS5Platform::FindGameFolders()
{
  std::vector<std::string> folders;

  // The title's own games folder.
  std::string games = Path::Combine(s_title_dir, "games");
  if (!FileSystem::DirectoryExists(games.c_str()))
    FileSystem::CreateDirectoryPath(games.c_str(), false);
  MakePublic(games.c_str(), true);
  folders.push_back(std::move(games));
  // And the one beside Crux2's files, when they are kept outside the title folder.
  if (!OutsideFolder().empty())
  {
    std::string outside = Path::Combine(OutsideFolder(), "games");
    if (FileSystem::DirectoryExists(outside.c_str()))
      folders.push_back(std::move(outside));
  }

  // Each network share.
  for (unsigned i = 0;; i++)
  {
    const char* const point = ps5vfs_mount_point(i);
    if (!point)
      break;
    if (std::strncmp(point, "/smb/", 5) == 0)
      folders.emplace_back(point);
  }

  // USB drives: the folders with one of these names at the top of the drive (not the whole drive,
  // which may hold a great many disc images that are not PlayStation 2 games).
  if (s_elevated)
  {
    static constexpr const char* names[] = {"crux2", "pspcsx2", "pcsx2", "ps2", "ps2iso", "playstation2"};
    for (unsigned drive = 0; drive < 8; drive++)
    {
      const std::string root = "/mnt/usb" + std::to_string(drive);
      if (!FileSystem::DirectoryExists(root.c_str()))
        continue;

      FileSystem::FindResultsArray entries;
      FileSystem::FindFiles(root.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_RELATIVE_PATHS, &entries);
      unsigned found = 0;
      for (const FILESYSTEM_FIND_DATA& entry : entries)
      {
        for (const char* const name : names)
        {
          if (StringUtil::compareNoCase(entry.FileName, name))
          {
            folders.push_back(Path::Combine(root, entry.FileName));
            found++;
            break;
          }
        }
      }
      ps5px_log("USB drive %s: %u game folder%s (%zu folders at its top)", root.c_str(), found, (found == 1) ? "" : "s",
                entries.size());
    }
  }

  return folders;
}

namespace {
// Outside the sandbox the title runs as another user than inside it, and an FTP server as yet
// another: a memory card or a settings file written by one must be writable by the others.
// Everything the title keeps is opened up; what the build shipped and the user's games are left.
//
// v0.8.0 did this for every file before the first picture and again while closing, with a write
// to the disk for each: with covers for a whole library that was 8 s of black screen at the start
// and as long at the end (measured on a console, 2026-10-07). Now a file is looked at first and
// changed only when it has to be, the walk at the start is on a thread of its own, and the one
// at the end goes through the few folders a session writes to.
std::thread s_public_thread;
std::atomic_bool s_public_stop{false};

double SecondsSince(std::chrono::steady_clock::time_point then)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - then).count();
}

void OpenUpEverything(bool session_only, const char* when)
{
  const auto started = std::chrono::steady_clock::now();
  PS5OpenUp::Opened count;
  PS5OpenUp::In(s_title_dir, session_only, &s_public_stop, &count);
  if (!PS5Platform::OutsideFolder().empty())
    PS5OpenUp::In(PS5Platform::OutsideFolder().c_str(), session_only, &s_public_stop, &count);
  ps5px_log("files: %u looked at, %u opened up for an FTP client%s (%.2f s, %s)%s", count.seen, count.changed,
            count.failed ? ", some could not be" : "", SecondsSince(started), when,
            s_public_stop.load(std::memory_order_relaxed) ? "; stopped before the end, the title is closing" : "");
}
} // namespace

void PS5Platform::MakeTitleFilesPublicInBackground()
{
  if (s_public_thread.joinable())
    return;
  s_public_stop.store(false, std::memory_order_relaxed);
  s_public_thread = std::thread([] { PS5Priority::SetCurrentThread(PS5Priority::Role::Background); OpenUpEverything(false, "in the background"); });
}

void PS5Platform::MakeSessionFilesPublic()
{
  // The walk of the start, should it still run: it stops at the next file.
  if (s_public_thread.joinable())
  {
    s_public_stop.store(true, std::memory_order_relaxed);
    s_public_thread.join();
  }
  s_public_stop.store(false, std::memory_order_relaxed);
  OpenUpEverything(true, "what this run wrote");
}

// The system service's status: a structure of 0x84 bytes (room is left for more), whose fifth byte
// is non-zero while the console's menu is over the title (PSSwanStation, by the same author,
// measured it on a console).
int PS5Platform::SystemMenuOver()
{
	alignas(8) unsigned char status[512] = {};
	const int result = sceSystemServiceGetStatus(status);
	static bool told = false;
	if (!told)
	{
		told = true;
		ps5px_log("system: the console's status answers %d (menu over the title: %d)", result, status[4]);
	}
	if (result != 0)
		return -1;
	return status[4] != 0 ? 1 : 0;
}

// A whole file written beside its place and then put there (FileSystem::RenamePath moves the old
// one aside first and checks the result: exFAT drives).
bool PS5Platform::WriteFileSafely(const char* path, const void* data, size_t size)
{
	const std::string part = std::string(path) + ".part";
	std::FILE* f = std::fopen(part.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool written = (size == 0 || std::fwrite(data, 1, size, f) == size);
	if (std::fclose(f) != 0 || !written)
	{
		unlink(part.c_str());
		return false;
	}
	if (!FileSystem::RenamePath(part.c_str(), path))
	{
		unlink(part.c_str());
		return false;
	}
	MakePublic(path, false);
	return true;
}
