/*
 * Crux2 - Move probe: does the console's own PS Move library give a homebrew process Move data?
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 * A standalone payload (sent to the payload loader on port 9021), not part of the title. It loads
 * /system/common/lib/libSceMove.sprx in its own process the way Move Management's
 * tools/output-api-inventory.c did (GPL-3.0-or-later; that tool only checked the exports, on
 * firmware 13.60), resolves the functions by name and then by NID as Crux2's
 * frontend/ps5_keyboard.cpp does, and then, for the first time, calls them: Init, Open for the
 * initial user's two Move indices, 30 s of ReadStateLatest (and ReadStateRecent once a second),
 * GetDeviceInfo, SetLightSphere, SetVibration, Close, Term. Everything goes to
 * /data/crux2-move-probe.log, one fsync'd line at a time.
 *
 * The signatures are the PS4 ones (REMovePS4's scemove.h, shadPS4's move.h); the PS5's are not
 * known. Every output buffer is therefore much larger than the PS4 structure and filled with a
 * sentinel, so a larger PS5 structure shows up in the log instead of corrupting the stack.
 *
 * Imports: libkernel (module load and lookup, notifications) and libSceUserService only, as Move
 * Management's payloads do; libSceSysmodule is looked up at run time, only if the path load fails.
 *
 * What it never does: open /dev/ugen*, use sceUsbd, libSceBluetoothHid or any HCI command, touch
 * pairing, or patch another process. (Opening the USB device and reading the HCI event endpoint is
 * what broke DualSense reconnection in Move Management's tests.) A hard stop (alarm) ends it after
 * 45 s whatever happens.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- the console's functions (libkernel, libSceSysmodule, libSceUserService stubs) ---- */

int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* option,
	int* result);
int sceKernelDlsym(int32_t handle, const char* symbol, void** address);
int sceKernelGetModuleList(int32_t* handles, size_t capacity, size_t* count);
int sceKernelGetModuleInfo(int32_t handle, void* info);
int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user);
int sceUserServiceGetForegroundUser(int32_t* user);

/* ---- settings ---- */

#define LOG_PATH "/data/crux2-move-probe.log"
#define MOVE_PATH "/system/common/lib/libSceMove.sprx"
#define PAD_PATH "/system/common/lib/libScePad.sprx"
#define MOVE_SYSMODULE_PS4 0x008F /* the PS4's id; the PS5's is not known */
#define HARD_STOP_SECONDS 45
#define READ_SECONDS 30
#define READ_PERIOD_US 10000 /* ~100 Hz */
#define SUMMARY_PERIOD_US 500000
#define MOVE_DATA_SIZE 64 /* the PS4's SceMoveData */
#define STATE_ROOM 256 /* what ReadStateLatest is given */
#define GUARD_ROOM 4096 /* the sentinel area it sits in */
#define RECENT_ENTRIES 64 /* the PS4 keeps 32 */
#define SENTINEL 0xCD

/* ---- the log ---- */

static int g_log = -1;
static const char* volatile g_step = "start";
static struct timespec g_t0;

static int64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)(t.tv_sec - g_t0.tv_sec) * 1000000 + (t.tv_nsec - g_t0.tv_nsec) / 1000;
}

static void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* format, ...)
{
	char line[1024];
	const int64_t t = now_us();
	int n = snprintf(line, sizeof(line), "[%3lld.%03lld] ", (long long)(t / 1000000), (long long)(t / 1000 % 1000));
	va_list ap;
	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	if (g_log >= 0)
	{
		(void)write(g_log, line, (size_t)n);
		(void)fsync(g_log);
	}
}

static void notify(const char* text)
{
	/* The console's notification request: 45 reserved bytes, then the text (as Crux2's
	 * ps5px_notify and Move Management's notify do). */
	static struct
	{
		char reserved[45];
		char message[3075];
	} request;
	memset(&request, 0, sizeof(request));
	snprintf(request.message, sizeof(request.message), "Crux2 Move probe\n%s", text);
	(void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
	say("notification: %s", text);
}

/* The hard stop and crashes: one line (async-signal-safe writes only), then out. */
static void on_signal(int sig)
{
	char line[256];
	const char* what = sig == SIGALRM ? "HARD STOP (alarm) during step: " : "CRASH (signal) during step: ";
	size_t n = 0;
	for (const char* p = what; *p && n < sizeof(line) - 1; p++)
		line[n++] = *p;
	for (const char* p = g_step; p && *p && n < sizeof(line) - 16; p++)
		line[n++] = *p;
	if (sig != SIGALRM)
	{
		line[n++] = ' ';
		line[n++] = 's';
		line[n++] = 'i';
		line[n++] = 'g';
		line[n++] = (char)('0' + sig / 10 % 10);
		line[n++] = (char)('0' + sig % 10);
	}
	line[n++] = '\n';
	if (g_log >= 0)
	{
		(void)write(g_log, line, n);
		(void)fsync(g_log);
	}
	_exit(sig == SIGALRM ? 3 : 4);
}

/* ---- modules ---- */

#define MODULE_INFO_SIZE 0x160 /* the size first, the name at 8 (as ps5_keyboard.cpp reads it) */
#define MODULE_NAME_OFFSET 8
#define MODULE_NAME_MAX 256

static int module_name(int32_t handle, char* name, size_t room)
{
	_Alignas(8) uint8_t info[MODULE_INFO_SIZE];
	memset(info, 0, sizeof(info));
	const uint64_t size = sizeof(info);
	memcpy(info, &size, sizeof(size));
	name[0] = '\0';
	if (sceKernelGetModuleInfo(handle, info) != 0)
		return -1;
	size_t n = room - 1 < MODULE_NAME_MAX ? room - 1 : MODULE_NAME_MAX;
	if (n > MODULE_INFO_SIZE - MODULE_NAME_OFFSET)
		n = MODULE_INFO_SIZE - MODULE_NAME_OFFSET;
	memcpy(name, info + MODULE_NAME_OFFSET, n);
	name[n] = '\0';
	return 0;
}

/* The process's module named `wanted` ("libSceMove" matches libSceMove.sprx), or -1. */
static int32_t find_module(const char* wanted)
{
	int32_t handles[256];
	size_t count = 0;
	if (sceKernelGetModuleList(handles, 256, &count) != 0)
		return -1;
	if (count > 256)
		count = 256;
	const size_t len = strlen(wanted);
	for (size_t i = 0; i < count; i++)
	{
		char name[MODULE_NAME_MAX + 1];
		if (module_name(handles[i], name, sizeof(name)) == 0 && strncmp(name, wanted, len) == 0 &&
			(name[len] == '\0' || name[len] == '.'))
			return handles[i];
	}
	return -1;
}

static void log_modules(void)
{
	int32_t handles[256];
	size_t count = 0;
	const int rc = sceKernelGetModuleList(handles, 256, &count);
	say("module list: rc=%#x count=%zu", (unsigned)rc, count);
	if (rc != 0)
		return;
	if (count > 256)
		count = 256;
	for (size_t i = 0; i < count; i++)
	{
		char name[MODULE_NAME_MAX + 1];
		module_name(handles[i], name, sizeof(name));
		say("  module handle %#x: %s", (unsigned)handles[i], name[0] ? name : "(no info)");
	}
}

typedef struct
{
	const char* name;
	const char* nid;
	void* address;
	const char* how;
} Function;

/* By name, then by NID through the module handle; then the payload runtime's dlsym. */
static void resolve(int32_t handle, void* dl, Function* f)
{
	void* address = NULL;
	int rc = -1;
	f->address = NULL;
	f->how = "missing";
	if (handle >= 0)
	{
		rc = sceKernelDlsym(handle, f->name, &address);
		if (rc == 0 && address)
		{
			f->address = address;
			f->how = "sceKernelDlsym(name)";
			goto done;
		}
		const int by_name = rc;
		address = NULL;
		rc = sceKernelDlsym(handle, f->nid, &address);
		if (rc == 0 && address)
		{
			f->address = address;
			f->how = "sceKernelDlsym(nid)";
			goto done;
		}
		say("  %s: sceKernelDlsym name rc=%#x, nid %s rc=%#x", f->name, (unsigned)by_name, f->nid, (unsigned)rc);
	}
	if (dl)
	{
		address = dlsym(dl, f->name);
		if (address)
		{
			f->address = address;
			f->how = "dlsym(name)";
			goto done;
		}
		address = dlsym(dl, f->nid);
		if (address)
		{
			f->address = address;
			f->how = "dlsym(nid)";
			goto done;
		}
	}
done:
	say("resolve %-26s nid %s -> %p (%s)", f->name, f->nid, f->address, f->how);
}

/* ---- the Move library (PS4 signatures; the PS5's are unverified) ---- */

typedef int (*InitFn)(void);
typedef int (*TermFn)(void);
typedef int (*OpenFn)(int32_t user, int32_t type, int32_t index);
typedef int (*CloseFn)(int32_t handle);
typedef int (*ReadLatestFn)(int32_t handle, void* data);
typedef int (*ReadRecentFn)(int32_t handle, int64_t timestamp_us, void* data, int32_t* count);
typedef int (*GetDeviceInfoFn)(int32_t handle, void* info);
typedef int (*SetLightSphereFn)(int32_t handle, uint8_t r, uint8_t g, uint8_t b);
typedef int (*SetVibrationFn)(int32_t handle, uint8_t motor);
typedef int (*ResetLightSphereFn)(int32_t handle);

enum
{
	F_INIT,
	F_TERM,
	F_OPEN,
	F_CLOSE,
	F_LATEST,
	F_RECENT,
	F_INFO,
	F_LIGHT,
	F_VIBRATE,
	F_RESET_LIGHT,
	F_COUNT
};

static Function g_move[F_COUNT] = {
	{"sceMoveInit", "j1ITE-EoJmE", NULL, NULL},
	{"sceMoveTerm", "tsZi60H4ypY", NULL, NULL},
	{"sceMoveOpen", "HzC60MfjJxU", NULL, NULL},
	{"sceMoveClose", "XX6wlxpHyeo", NULL, NULL},
	{"sceMoveReadStateLatest", "ttU+JOhShl4", NULL, NULL},
	{"sceMoveReadStateRecent", "f2bcpK6kJfg", NULL, NULL},
	{"sceMoveGetDeviceInfo", "GWXTyxs4QbE", NULL, NULL},
	{"sceMoveSetLightSphere", "T8KYHPs1JE8", NULL, NULL},
	{"sceMoveSetVibration", "IFQwtT2CeY0", NULL, NULL},
	{"sceMoveResetLightSphere", "zuxWAg3HAac", NULL, NULL},
};

static const char* error_name(int rc)
{
	switch ((uint32_t)rc)
	{
		case 0: return "ok";
		case 1: return "no controller connected";
		case 0x80EE0001u: return "not initialised";
		case 0x80EE0002u: return "already initialised";
		case 0x80EE0003u: return "invalid argument";
		case 0x80EE0004u: return "invalid handle";
		case 0x80EE0005u: return "too many controllers";
		case 0x80EE0006u: return "invalid port";
		case 0x80EE0007u: return "already open";
		case 0x80EE00FFu: return "fatal";
		default: return rc >= 0 ? "non-negative (a handle?)" : "unknown error";
	}
}

/* The PS4's SceMoveData, read from the buffer without assuming it is that structure. */
typedef struct
{
	float accel[3];
	float gyro[3];
	uint16_t buttons;
	uint16_t analog_t;
	int64_t timestamp;
	int32_t counter;
	float temperature;
} MoveData;

static void decode(const uint8_t* p, MoveData* d)
{
	memcpy(d->accel, p + 0, 12);
	memcpy(d->gyro, p + 12, 12);
	memcpy(&d->buttons, p + 24, 2);
	memcpy(&d->analog_t, p + 26, 2);
	memcpy(&d->timestamp, p + 48, 8);
	memcpy(&d->counter, p + 56, 4);
	memcpy(&d->temperature, p + 60, 4);
}

static void buttons_text(uint16_t b, char* out, size_t room)
{
	static const struct
	{
		uint16_t bit;
		const char* name;
	} names[] = {{0x0001, "Select"}, {0x0002, "T"}, {0x0004, "Move"}, {0x0008, "Start"}, {0x0010, "Triangle"},
		{0x0020, "Circle"}, {0x0040, "Cross"}, {0x0080, "Square"}, {0x8000, "Intercepted"}};
	size_t n = 0;
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (b & names[i].bit)
			n += (size_t)snprintf(out + n, room > n ? room - n : 0, "%s%s", n ? "+" : "", names[i].name);
	const uint16_t other = (uint16_t)(b & ~0x80FFu);
	if (other)
		n += (size_t)snprintf(out + n, room > n ? room - n : 0, "%sunknown:%#x", n ? "+" : "", other);
	if (!n)
		snprintf(out, room, "none");
}

/* The highest offset (exclusive) in [from, size) that is no longer the sentinel, or 0. */
static size_t written_past(const uint8_t* buffer, size_t from, size_t size)
{
	for (size_t i = size; i > from; i--)
		if (buffer[i - 1] != SENTINEL)
			return i;
	return 0;
}

static void hex_line(const char* label, const uint8_t* p, size_t n)
{
	char text[3 * 128 + 1];
	size_t k = 0;
	for (size_t i = 0; i < n && i < 128; i++)
		k += (size_t)snprintf(text + k, sizeof(text) - k, "%02x%s", p[i], (i % 16 == 15) ? "|" : " ");
	say("%s %s", label, text);
}

/* ---- the probe ---- */

typedef struct
{
	int index;
	int32_t handle; /* < 0: not open */
	int last_rc;
	int32_t last_counter;
	int64_t last_timestamp;
	uint16_t last_buttons;
	unsigned reads, ok_reads, counter_changes, window_changes, logged_changes;
	int overrun_logged;
	int nonzero_seen;
} Port;

static _Alignas(16) uint8_t g_state[GUARD_ROOM];
static _Alignas(16) uint8_t g_recent[RECENT_ENTRIES * MOVE_DATA_SIZE + GUARD_ROOM];

static void read_latest(Port* port, ReadLatestFn read, int summary)
{
	memset(g_state, 0, STATE_ROOM);
	memset(g_state + MOVE_DATA_SIZE, SENTINEL, GUARD_ROOM - MOVE_DATA_SIZE);
	const int rc = read(port->handle, g_state);
	port->reads++;
	const int64_t t = now_us();

	if (port->reads == 1 || rc != port->last_rc)
	{
		say("port %d: ReadStateLatest rc=%#x (%s)%s", port->index, (unsigned)rc, error_name(rc),
			port->reads == 1 ? " [first read]" : " [rc changed]");
		port->last_rc = rc;
	}
	const size_t past = written_past(g_state, MOVE_DATA_SIZE, GUARD_ROOM);
	if (past && !port->overrun_logged)
	{
		port->overrun_logged = 1;
		say("port %d: ReadStateLatest wrote past byte 64: up to byte %zu%s", port->index, past,
			past > STATE_ROOM ? " (PAST THE 256 GIVEN)" : "");
		hex_line("  bytes 64..", g_state + MOVE_DATA_SIZE, past - MOVE_DATA_SIZE < 128 ? past - MOVE_DATA_SIZE : 128);
	}
	if (rc != 0)
		return;
	port->ok_reads++;

	MoveData d;
	decode(g_state, &d);
	if (!port->nonzero_seen)
	{
		for (size_t i = 0; i < MOVE_DATA_SIZE; i++)
			if (g_state[i])
			{
				port->nonzero_seen = 1;
				hex_line("  first non-zero state, bytes 0..63:", g_state, MOVE_DATA_SIZE);
				break;
			}
	}
	if (d.counter != port->last_counter)
	{
		port->counter_changes++;
		port->window_changes++;
		if (port->logged_changes < 5)
		{
			port->logged_changes++;
			say("port %d: counter %d -> %d, timestamp %lld (dt %lld us)", port->index, (int)port->last_counter,
				(int)d.counter, (long long)d.timestamp, (long long)(d.timestamp - port->last_timestamp));
		}
		port->last_counter = d.counter;
		port->last_timestamp = d.timestamp;
	}
	if (d.buttons != port->last_buttons)
	{
		char was[160], is[160];
		buttons_text(port->last_buttons, was, sizeof(was));
		buttons_text(d.buttons, is, sizeof(is));
		say("port %d: buttons %#06x (%s) -> %#06x (%s), T=%u", port->index, port->last_buttons, was, d.buttons, is,
			d.analog_t);
		port->last_buttons = d.buttons;
	}
	if (summary)
	{
		char b[160];
		buttons_text(d.buttons, b, sizeof(b));
		say("port %d: t=%.1fs counter=%d (+%u in 0.5 s) buttons=%#06x %s T=%u accel=(%.3f %.3f %.3f) "
			"gyro=(%.3f %.3f %.3f) temp=%.2f ts=%lld",
			port->index, (double)t / 1e6, (int)d.counter, port->window_changes, d.buttons, b, d.analog_t,
			(double)d.accel[0], (double)d.accel[1], (double)d.accel[2], (double)d.gyro[0], (double)d.gyro[1],
			(double)d.gyro[2], (double)d.temperature, (long long)d.timestamp);
		port->window_changes = 0;
	}
}

static void read_recent(Port* port, ReadRecentFn read)
{
	memset(g_recent, SENTINEL, sizeof(g_recent));
	int32_t count = -12345;
	const int64_t since = port->last_timestamp > 1000000 ? port->last_timestamp - 1000000 : 0;
	const int rc = read(port->handle, since, g_recent, &count);
	const size_t past = written_past(g_recent, 0, sizeof(g_recent));
	say("port %d: ReadStateRecent(since %lld) rc=%#x (%s) count=%d, bytes written up to %zu%s", port->index,
		(long long)since, (unsigned)rc, error_name(rc), (int)count, past,
		past > RECENT_ENTRIES * MOVE_DATA_SIZE ? " (PAST 64 ENTRIES)" : "");
	if (rc == 0 && count > 0 && count <= RECENT_ENTRIES)
	{
		MoveData first, last;
		decode(g_recent, &first);
		decode(g_recent + (size_t)(count - 1) * MOVE_DATA_SIZE, &last);
		say("port %d:   recent first counter=%d ts=%lld, last counter=%d ts=%lld", port->index, (int)first.counter,
			(long long)first.timestamp, (int)last.counter, (long long)last.timestamp);
	}
}

static void sleep_us(int64_t us)
{
	if (us <= 0)
		return;
	struct timespec t = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
	while (nanosleep(&t, &t) != 0 && errno == EINTR)
	{
	}
}

int main(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	signal(SIGALRM, on_signal);
	signal(SIGSEGV, on_signal);
	signal(SIGBUS, on_signal);
	signal(SIGILL, on_signal);
	signal(SIGFPE, on_signal);
	alarm(HARD_STOP_SECONDS);

	say("Crux2 Move probe: libSceMove in a payload process. Log %s (fd %d), pid %d, hard stop %d s", LOG_PATH, g_log,
		(int)getpid(), HARD_STOP_SECONDS);
	say("never opens /dev/ugen*, never uses sceUsbd, libSceBluetoothHid or HCI");
	notify("Started. Hold the Move; watch for prompts. Log: " LOG_PATH);

	/* 1. Load the library. */
	g_step = "load libSceMove";
	int start = 0;
	int32_t handle = sceKernelLoadStartModule(MOVE_PATH, 0, NULL, 0, NULL, &start);
	say("sceKernelLoadStartModule(%s) -> handle/rc %#x (%d), start result %#x", MOVE_PATH, (unsigned)handle,
		(int)handle, (unsigned)start);
	if (handle < 0)
	{
		/* Looked up only now, so a missing libSceSysmodule cannot stop the probe from starting. */
		void* sysmodule = dlopen("libSceSysmodule.sprx", RTLD_LAZY);
		int (*load)(uint16_t) = sysmodule ? (int (*)(uint16_t))dlsym(sysmodule, "sceSysmoduleLoadModule") : NULL;
		const int sys = load ? load(MOVE_SYSMODULE_PS4) : (int)0xFFFFFFFF;
		say("path load failed: sceSysmoduleLoadModule %s(%#x, the PS4's id) -> %#x", load ? "" : "NOT FOUND ",
			MOVE_SYSMODULE_PS4, (unsigned)sys);
		handle = find_module("libSceMove");
		say("libSceMove in the module list after that: handle %#x", (unsigned)handle);
	}
	const int32_t listed = find_module("libSceMove");
	say("libSceMove in the module list: handle %#x (load gave %#x)", (unsigned)listed, (unsigned)handle);
	if (handle < 0)
		handle = listed;
	void* dl = dlopen(MOVE_PATH, RTLD_LAZY);
	const char* dl_error = dl ? NULL : dlerror();
	say("dlopen(%s) -> %p%s%s", MOVE_PATH, dl, dl ? "" : ": ", dl ? "" : (dl_error ? dl_error : "?"));
	if (handle < 0 && !dl)
		log_modules();

	/* 2. Resolve. */
	g_step = "resolve";
	int missing = 0;
	for (int i = 0; i < F_COUNT; i++)
	{
		resolve(handle, dl, &g_move[i]);
		if (!g_move[i].address && i != F_RESET_LIGHT)
			missing++;
	}
	if (missing)
		log_modules();

	/* 7 (early, read-only). scePadIsMoveConnected: its signature is not known, so it is resolved
	 * and logged, never called. */
	g_step = "libScePad lookup";
	{
		int pad_start = 0;
		const int32_t pad = sceKernelLoadStartModule(PAD_PATH, 0, NULL, 0, NULL, &pad_start);
		say("sceKernelLoadStartModule(%s) -> %#x", PAD_PATH, (unsigned)pad);
		Function f = {"scePadIsMoveConnected", "4y9RNPSBsqg", NULL, NULL};
		resolve(pad >= 0 ? pad : find_module("libScePad"), NULL, &f);
		say("scePadIsMoveConnected: %s; NOT called (its signature is not known)", f.address ? "present" : "missing");
	}

	/* 3. The user. */
	g_step = "user service";
	const int us_init = sceUserServiceInitialize(NULL);
	int32_t user = -1;
	int us_rc = sceUserServiceGetInitialUser(&user);
	say("sceUserServiceInitialize -> %#x; GetInitialUser -> %#x, user %#x", (unsigned)us_init, (unsigned)us_rc,
		(unsigned)user);
	if (us_rc != 0 || user < 0)
	{
		int32_t fg = -1;
		const int fg_rc = sceUserServiceGetForegroundUser(&fg);
		say("GetForegroundUser -> %#x, user %#x", (unsigned)fg_rc, (unsigned)fg);
		if (fg_rc == 0 && fg >= 0)
			user = fg;
	}

	if (!g_move[F_INIT].address || !g_move[F_OPEN].address || !g_move[F_LATEST].address)
	{
		say("RESULT: Init/Open/ReadStateLatest not resolved: the route is closed or needs another loading route");
		notify("Ended: libSceMove functions not found. Send the log back.");
		say("complete");
		return 1;
	}

	/* 4. Init and open. */
	g_step = "sceMoveInit";
	const int init = ((InitFn)g_move[F_INIT].address)();
	say("sceMoveInit() -> %#x (%s)", (unsigned)init, error_name(init));

	Port ports[2];
	memset(ports, 0, sizeof(ports));
	int opened = 0;
	for (int i = 0; i < 2; i++)
	{
		g_step = i ? "sceMoveOpen index 1" : "sceMoveOpen index 0";
		ports[i].index = i;
		ports[i].handle = ((OpenFn)g_move[F_OPEN].address)(user, 0, i);
		say("sceMoveOpen(user %#x, type 0, index %d) -> %#x (%s)", (unsigned)user, i, (unsigned)ports[i].handle,
			ports[i].handle >= 0 ? "handle" : error_name(ports[i].handle));
		ports[i].last_counter = -1;
		if (ports[i].handle >= 0)
			opened++;
	}

	/* 5. Read for 30 s. */
	if (opened)
	{
		g_step = "reading";
		notify("0-10 s: press every button, one at a time (Move, Start, Select, Triangle, Circle, Cross, Square)");
		const int64_t begin = now_us();
		int64_t next_summary = begin + SUMMARY_PERIOD_US, next_recent = begin + 1000000;
		int stage = 0;
		for (int64_t t = begin; t - begin < (int64_t)READ_SECONDS * 1000000; t = now_us())
		{
			if (stage == 0 && t - begin >= 10000000)
			{
				stage = 1;
				notify("10-20 s: pull T slowly all the way and let go, a few times");
			}
			else if (stage == 1 && t - begin >= 20000000)
			{
				stage = 2;
				notify("20-30 s: tilt and turn the Move, then hold it still");
			}
			const int summary = t >= next_summary;
			const int recent = t >= next_recent;
			for (int i = 0; i < 2; i++)
			{
				if (ports[i].handle < 0)
					continue;
				read_latest(&ports[i], (ReadLatestFn)g_move[F_LATEST].address, summary);
				if (recent && g_move[F_RECENT].address)
				{
					g_step = "ReadStateRecent";
					read_recent(&ports[i], (ReadRecentFn)g_move[F_RECENT].address);
					g_step = "reading";
				}
			}
			if (summary)
				next_summary += SUMMARY_PERIOD_US;
			if (recent)
				next_recent += 1000000;
			sleep_us(READ_PERIOD_US - (now_us() - t));
		}
		for (int i = 0; i < 2; i++)
			if (ports[i].handle >= 0)
				say("port %d: %u reads, %u with rc 0, %u counter changes, last rc %#x (%s)", i, ports[i].reads,
					ports[i].ok_reads, ports[i].counter_changes, (unsigned)ports[i].last_rc, error_name(ports[i].last_rc));
	}
	else
	{
		say("no index opened: nothing to read");
	}

	/* 6. Device info, light, vibration, close. */
	for (int i = 0; i < 2; i++)
	{
		if (ports[i].handle < 0)
			continue;
		const int32_t h = ports[i].handle;
		if (g_move[F_INFO].address)
		{
			g_step = "GetDeviceInfo";
			memset(g_state, SENTINEL, GUARD_ROOM);
			const int rc = ((GetDeviceInfoFn)g_move[F_INFO].address)(h, g_state);
			const size_t past = written_past(g_state, 0, GUARD_ROOM);
			float f[4];
			memcpy(f, g_state, sizeof(f));
			say("port %d: GetDeviceInfo -> %#x (%s), bytes written up to %zu (PS4: 16); sphere radius %.3f, accel to "
				"sphere (%.3f %.3f %.3f)",
				i, (unsigned)rc, error_name(rc), past, (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
			if (past)
				hex_line("  device info bytes:", g_state, past < 64 ? past : 64);
		}
		if (g_move[F_LIGHT].address)
		{
			g_step = "SetLightSphere";
			if (i == 0)
				notify("Light: blue, then orange; then a short rumble");
			int rc = ((SetLightSphereFn)g_move[F_LIGHT].address)(h, 0, 0, 255);
			say("port %d: SetLightSphere(0,0,255) blue -> %#x (%s)", i, (unsigned)rc, error_name(rc));
			sleep_us(1000000);
			rc = ((SetLightSphereFn)g_move[F_LIGHT].address)(h, 255, 64, 0);
			say("port %d: SetLightSphere(255,64,0) orange -> %#x (%s)", i, (unsigned)rc, error_name(rc));
			sleep_us(1000000);
		}
		if (g_move[F_VIBRATE].address)
		{
			g_step = "SetVibration";
			int rc = ((SetVibrationFn)g_move[F_VIBRATE].address)(h, 128);
			say("port %d: SetVibration(128) -> %#x (%s)", i, (unsigned)rc, error_name(rc));
			sleep_us(300000);
			rc = ((SetVibrationFn)g_move[F_VIBRATE].address)(h, 0);
			say("port %d: SetVibration(0) -> %#x (%s)", i, (unsigned)rc, error_name(rc));
		}
		if (g_move[F_RESET_LIGHT].address)
		{
			g_step = "ResetLightSphere";
			const int rc = ((ResetLightSphereFn)g_move[F_RESET_LIGHT].address)(h);
			say("port %d: ResetLightSphere -> %#x (%s)", i, (unsigned)rc, error_name(rc));
		}
		if (g_move[F_CLOSE].address)
		{
			g_step = "sceMoveClose";
			const int rc = ((CloseFn)g_move[F_CLOSE].address)(h);
			say("port %d: sceMoveClose -> %#x (%s)", i, (unsigned)rc, error_name(rc));
		}
	}
	if (g_move[F_TERM].address)
	{
		g_step = "sceMoveTerm";
		const int rc = ((TermFn)g_move[F_TERM].address)();
		say("sceMoveTerm -> %#x (%s)", (unsigned)rc, error_name(rc));
	}

	/* The outcome, in one line. */
	unsigned ok = 0, changes = 0, none = 0;
	for (int i = 0; i < 2; i++)
	{
		ok += ports[i].ok_reads;
		changes += ports[i].counter_changes;
		none += ports[i].last_rc == 1;
	}
	const char* verdict = !opened ? "Open refused for both indices"
		: (ok && changes > 1) ? "Move data arrives (rc 0, counter changing)"
		: ok ? "rc 0 but the counter did not change"
		: none ? "rc +1: no Move bound to this user"
		: "reads failed";
	say("RESULT: %s (opened %d, rc-0 reads %u, counter changes %u)", verdict, opened, ok, changes);
	alarm(0);
	g_step = "end";
	notify(verdict);
	say("complete");
	if (g_log >= 0)
		close(g_log);
	return 0;
}
