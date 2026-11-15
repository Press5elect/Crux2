// Crux2 - the console's own notifications (ps5_notify.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise 1679c95 (src/porpoise_notify.cpp, GPL-3.0-or-later). The rich notification's
// JSON, the sounds, the channels, the pacing and the relay through the payload loader follow
// PS5SX2 (ps5/coreorbis/orbis-shims/ProsperoNotify.cpp, GPL-3.0-or-later), whose notes record what
// was seen on a console: only notifications kept in the list show; "psfx_trophy_toast" and
// "psfx_platinum_trophy_toast" play the trophy sounds; one sent in a game's first seconds loses its
// picture; the icon may be an https address. The JSON's layout is the payload SDK's notify sample.
//
// libSceNotification is not imported (a title that imports what the console does not give it does
// not start): it is loaded while the title runs, with sceKernelLoadStartModule found in libkernel,
// as ps5_mic.cpp loads libSceAudioIn.

#include "ps5_notify.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#ifndef CRUX2_NOTIFY_TEST
#include "ps5_keyboard.h" // PS5SystemModules
#include "Host.h"
#endif

extern "C" {
void ps5px_log(const char* format, ...);
void ps5px_notify(const char* message);
const char* ps5px_title_dir(void);
}

namespace PS5Notify
{
namespace
{
struct Toast
{
	bool rich = false;
	std::string message, sub, icon;
	Sound sound = Sound::Default;
	bool trophy = false;
};

struct Shared
{
	std::mutex lock;
	std::condition_variable wake, idle;
	std::deque<Toast> queue;
	bool started = false, sending = false;
	std::chrono::steady_clock::time_point hold_until{};
};
Shared& S()
{
	static Shared* const s = new Shared(); // never freed: its thread waits on it for good
	return *s;
}
constexpr size_t MAX_QUEUED = 24;
std::atomic<int> s_rich_state{-1};

// Between two rich notifications, so that each is shown with its picture.
std::chrono::milliseconds Gap()
{
#ifdef CRUX2_NOTIFY_TEST
	if (const char* ms = std::getenv("CRUX2_NOTIFY_GAP_MS"))
		return std::chrono::milliseconds(std::atoi(ms));
#endif
	return std::chrono::milliseconds(6500);
}

constexpr char RELAY_MARKER[] = "CRUX2-NOTIFY-RELAY-JSON-V1";
constexpr size_t RELAY_ROOM = 16384; // g_payload's size in ps5/elevation/notify-relay/notify_relay.c

std::string Quote(const std::string& s)
{
	std::string out = "\"";
	for (const unsigned char c : s)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += static_cast<char>(c);
		}
		else if (c == '\n')
			out += "\\n";
		else if (c < 0x20)
		{
			char esc[8];
			std::snprintf(esc, sizeof(esc), "\\u%04x", c);
			out += esc;
		}
		else
			out += static_cast<char>(c);
	}
	return out + "\"";
}

// libSceNotification's sceNotificationSend in the title's own process, when the system lets it.
using SendFn = int (*)(int user, bool logged, const char* payload);
SendFn OwnSender()
{
#ifdef CRUX2_NOTIFY_TEST
	return nullptr;
#else
	static bool tried = false;
	static SendFn send = nullptr;
	if (tried)
		return send;
	tried = true;
	using LoadFn = int (*)(const char*, size_t, const void*, uint32_t, void*, int*);
	const auto load = reinterpret_cast<LoadFn>(PS5SystemModules::FindFunction("libkernel", "sceKernelLoadStartModule", "wzvqT4UqKX8"));
	if (!load)
	{
		ps5px_log("notify: the module loader was not found; rich notifications go through the payload loader");
		return nullptr;
	}
	for (const char* dir : {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/", "/system_ex/priv_ex/lib/"})
	{
		const std::string path = std::string(dir) + "libSceNotification.sprx";
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		int result = 0;
		const int handle = load(path.c_str(), 0, nullptr, 0, nullptr, &result);
		ps5px_log("notify: %s loaded: %#x", path.c_str(), static_cast<unsigned>(handle));
		if (handle < 0)
			continue;
		send = reinterpret_cast<SendFn>(PS5SystemModules::FindInHandle(handle, "sceNotificationSend", "jSWBvOPfSOk"));
		break;
	}
	if (!send)
		ps5px_log("notify: libSceNotification is not given to the title; rich notifications go through the payload loader");
	return send;
#endif
}

// The relay payload as built (notify-relay.elf in the title folder), read once.
const std::string& Relay()
{
	static const std::string relay = [] {
		std::string path;
#ifdef CRUX2_NOTIFY_TEST
		if (const char* given = std::getenv("CRUX2_NOTIFY_RELAY"))
			path = given;
#else
		path = std::string(ps5px_title_dir()) + "/notify-relay.elf";
#endif
		std::string data;
		if (FILE* const fp = std::fopen(path.c_str(), "rb"))
		{
			char block[16384];
			size_t got;
			while ((got = std::fread(block, 1, sizeof(block), fp)) > 0)
				data.append(block, got);
			std::fclose(fp);
		}
		if (data.empty())
			ps5px_log("notify: %s is not there: no rich notifications through the payload loader", path.c_str());
		return data;
	}();
	return relay;
}

int RelayPort()
{
#ifdef CRUX2_NOTIFY_TEST
	if (const char* port = std::getenv("CRUX2_NOTIFY_PORT"))
		return std::atoi(port);
#endif
	return 9021;
}

// The relay with this JSON to the payload loader; true once it ran.
bool SendRelay(const std::string& json)
{
	const std::string image = RelayImage(Relay(), json);
	if (image.empty())
		return false;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<uint16_t>(RelayPort()));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const int flags = fcntl(fd, F_GETFL, 0);
	bool ok = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
	if (ok && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS)
		ok = false; // nothing listening: no payload loader on this console
	if (ok)
	{
		pollfd p{fd, POLLOUT, 0};
		int error = 0;
		socklen_t length = sizeof(error);
		ok = poll(&p, 1, 500) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
	}
	if (ok)
	{
		fcntl(fd, F_SETFL, flags);
		for (size_t sent = 0; ok && sent < image.size();)
		{
			const ssize_t n = send(fd, image.data() + sent, image.size() - sent, 0);
			ok = n > 0;
			if (ok)
				sent += static_cast<size_t>(n);
		}
	}
	if (ok)
	{
		shutdown(fd, SHUT_WR);
		// The loader closes the connection when the payload has ended, which keeps the
		// notifications in order (5 s at most).
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;)
		{
			const int left = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			pollfd q{fd, POLLIN, 0};
			char buffer[256];
			if (left <= 0 || poll(&q, 1, left) != 1 || recv(fd, buffer, sizeof(buffer), 0) <= 0)
				break;
		}
	}
	close(fd);
	return ok;
}

void Send(const Toast& t)
{
	if (!t.rich)
	{
		ps5px_notify(t.message.c_str());
		return;
	}
	timespec ts{};
	clock_gettime(CLOCK_REALTIME, &ts);
	tm when{};
	gmtime_r(&ts.tv_sec, &when);
	char created[48];
	std::snprintf(created, sizeof(created), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", when.tm_year + 1900, when.tm_mon + 1,
		when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec, static_cast<int>(ts.tv_nsec / 1000000));
	static unsigned count = 0;
	char id[16];
	std::snprintf(id, sizeof(id), "%u", 100000000u + (static_cast<unsigned>(ts.tv_sec) % 800000u) * 1000u + (count++ % 1000u));
	const std::string json = Payload(t.message, t.sub, t.icon, t.sound, t.trophy, created, id);

	if (const SendFn own = OwnSender())
	{
		const int result = own(0xFE, true, json.c_str());
		if (result == 0)
		{
			if (s_rich_state.exchange(1) != 1)
				ps5px_log("notify: rich notifications through libSceNotification");
			return;
		}
		ps5px_log("notify: sceNotificationSend answered %#x", static_cast<unsigned>(result));
	}
	if (SendRelay(json))
	{
		if (s_rich_state.exchange(1) != 1)
			ps5px_log("notify: rich notifications through the payload loader (notify-relay.elf)");
		return;
	}
	if (s_rich_state.exchange(0) != 0)
		ps5px_log("notify: no rich notification reached the console (no libSceNotification, no payload loader on port %d): plain ones",
			RelayPort());
	ps5px_notify((t.message + (t.sub.empty() ? "" : "\n" + t.sub)).c_str());
}

void Worker()
{
	Shared& s = S();
	std::chrono::steady_clock::time_point last_rich{};
	for (;;)
	{
		Toast t;
		std::chrono::steady_clock::time_point hold{};
		{
			std::unique_lock lock(s.lock);
			s.sending = false;
			s.idle.notify_all();
			s.wake.wait(lock, [&s] { return !s.queue.empty(); });
			t = std::move(s.queue.front());
			s.queue.pop_front();
			s.sending = true;
			hold = s.hold_until;
		}
		if (t.rich)
		{
			auto when = hold;
			if (last_rich.time_since_epoch().count() != 0 && last_rich + Gap() > when)
				when = last_rich + Gap();
			if (when > std::chrono::steady_clock::now())
				std::this_thread::sleep_until(when);
		}
		Send(t);
		if (t.rich)
			last_rich = std::chrono::steady_clock::now();
	}
}

// Under s.lock: true when the thread runs.
bool StartWorker(Shared& s)
{
	if (!s.started)
	{
		// (Built without exceptions, as the rest of the title: a thread that cannot be made ends it,
		// as everywhere else.)
		std::thread(Worker).detach();
		s.started = true;
	}
	return true;
}

void Queue(Toast t)
{
	Shared& s = S();
	const std::lock_guard lock(s.lock);
	if (!StartWorker(s) || s.queue.size() >= MAX_QUEUED)
		return;
	s.queue.push_back(std::move(t));
	s.wake.notify_one();
}
} // namespace

std::string Payload(const std::string& message, const std::string& sub, const std::string& icon, Sound sound, bool trophy,
	const char* created, const char* id)
{
	const char* const effect = (sound == Sound::Trophy) ? "psfx_trophy_toast" :
	                           (sound == Sound::Platinum) ? "psfx_platinum_trophy_toast" :
	                           (sound == Sound::Silent) ? "none" :
	                                                      nullptr;
	std::string j = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":" +
	                Quote(trophy ? "Trophies" : "ServiceFeedback") + ",\"useCaseId\":\"IDC\",";
	if (effect)
		j += "\"soundEffect\":" + Quote(effect) + ",";
	j += "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,\"viewData\":{";
	if (!icon.empty())
		j += "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":" + Quote(icon) + "}},";
	j += "\"message\":{\"body\":" + Quote(message) + "}";
	if (!sub.empty())
		j += ",\"subMessage\":{\"body\":" + Quote(sub) + "}";
	j += "}},\"createdDateTime\":" + Quote(created) + ",\"localNotificationId\":" + Quote(id) + "}";
	return j;
}

std::string RelayImage(const std::string& relay, const std::string& json)
{
	// The marker followed by the whole room, empty.
	const size_t marker = sizeof(RELAY_MARKER) - 1;
	if (json.size() >= RELAY_ROOM || relay.size() < RELAY_ROOM)
		return {};
	for (size_t at = relay.find(RELAY_MARKER); at != std::string::npos; at = relay.find(RELAY_MARKER, at + 1))
	{
		if (at + RELAY_ROOM > relay.size())
			break;
		size_t j = at + marker;
		while (j < at + RELAY_ROOM && relay[j] == '\0')
			++j;
		if (j != at + RELAY_ROOM)
			continue;
		std::string image = relay;
		std::memcpy(image.data() + at, json.data(), json.size());
		std::memset(image.data() + at + json.size(), 0, RELAY_ROOM - json.size());
		return image;
	}
	return {};
}

void Rich(const std::string& message, const std::string& sub, const std::string& icon, Sound sound, bool trophy)
{
	Toast t;
	t.rich = true;
	t.message = message;
	t.sub = sub;
	t.icon = (icon.rfind("https://", 0) == 0 || icon.rfind("/data/", 0) == 0) ? icon : std::string();
	t.sound = sound;
	t.trophy = trophy;
	Queue(std::move(t));
}

void Plain(const std::string& text)
{
	Toast t;
	t.message = text;
	Queue(std::move(t));
}

void Hold(int seconds)
{
	Shared& s = S();
	const std::lock_guard lock(s.lock);
	StartWorker(s);
	s.hold_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

int RichState()
{
	return s_rich_state.load();
}

bool ConsoleNotificationsOn()
{
#ifdef CRUX2_NOTIFY_TEST
	return std::getenv("CRUX2_NOTIFY_OFF") == nullptr;
#else
	return Host::GetBaseBoolSettingValue("Achievements", "ConsoleNotifications", true);
#endif
}

bool AchievementUnlocked(const std::string& title, const std::string& description, const std::string& badge_url)
{
	if (!ConsoleNotificationsOn() || RichState() == 0)
		return false;
	Rich(title, description, badge_url, Sound::Trophy, true);
	return true;
}

bool AchievementsMastered(const std::string& title, const std::string& text, const std::string& icon_url)
{
	if (!ConsoleNotificationsOn() || RichState() == 0)
		return false;
	Rich(title, text, icon_url, Sound::Platinum, true);
	return true;
}

void AchievementsGameLoaded()
{
	if (ConsoleNotificationsOn())
		Hold(8);
}

void Flush(int max_ms)
{
	Shared& s = S();
	std::unique_lock lock(s.lock);
	s.idle.wait_for(lock, std::chrono::milliseconds(max_ms), [&s] { return s.queue.empty() && !s.sending; });
}
} // namespace PS5Notify
