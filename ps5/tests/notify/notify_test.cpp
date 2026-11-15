// Crux2 - the console's own notifications (frontend/ps5_notify.cpp) on the PC: the rich
// notification's JSON, the relay payload filled in, a stand-in payload loader that receives it,
// and the plain notification when nothing listens.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   notify_test <work folder>

#include "ps5_notify.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static std::mutex s_mutex;
static std::vector<std::string> s_plain;

extern "C" {
void ps5px_log(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	std::fprintf(stderr, "  log: ");
	std::vfprintf(stderr, format, args);
	std::fprintf(stderr, "\n");
	va_end(args);
}
void ps5px_notify(const char* message)
{
	const std::lock_guard lock(s_mutex);
	s_plain.emplace_back(message);
}
const char* ps5px_title_dir(void)
{
	return "/nowhere";
}
}

static int s_failures = 0;
#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static bool Has(const std::string& text, const char* part)
{
	return text.find(part) != std::string::npos;
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	const std::string work = argv[1];
	using PS5Notify::Sound;

	// The JSON: the channel, the sound, the picture, the two lines, quoted.
	const std::string trophy = PS5Notify::Payload("Got \"it\"", "Line\\two\nthree", "https://media.example/Badge/1.png", Sound::Trophy, true,
		"2026-11-15T10:00:00.000Z", "100000001");
	CHECK(Has(trophy, "\"channelType\":\"Trophies\""));
	CHECK(Has(trophy, "\"soundEffect\":\"psfx_trophy_toast\""));
	CHECK(Has(trophy, "\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"https://media.example/Badge/1.png\"}}"));
	CHECK(Has(trophy, "\"message\":{\"body\":\"Got \\\"it\\\"\"}"));
	CHECK(Has(trophy, "\"subMessage\":{\"body\":\"Line\\\\two\\nthree\"}"));
	CHECK(Has(trophy, "\"localNotificationId\":\"100000001\""));
	const std::string plainer = PS5Notify::Payload("A", "", "", Sound::Default, false, "x", "1");
	CHECK(Has(plainer, "\"channelType\":\"ServiceFeedback\""));
	CHECK(!Has(plainer, "soundEffect"));
	CHECK(!Has(plainer, "icon"));
	CHECK(!Has(plainer, "subMessage"));
	CHECK(Has(PS5Notify::Payload("A", "", "", Sound::Platinum, true, "x", "1"), "psfx_platinum_trophy_toast"));
	CHECK(Has(PS5Notify::Payload("A", "", "", Sound::Silent, true, "x", "1"), "\"soundEffect\":\"none\""));

	// The relay: the marker and its empty room filled in, the rest as it was.
	const std::string marker = "CRUX2-NOTIFY-RELAY-JSON-V1";
	std::string relay = std::string(100, 'E') + marker + std::string(16384 - marker.size(), '\0') + std::string(50, 'T');
	const std::string image = PS5Notify::RelayImage(relay, trophy);
	CHECK(image.size() == relay.size());
	CHECK(image.compare(100, trophy.size(), trophy) == 0);
	CHECK(image[100 + trophy.size()] == '\0');
	CHECK(image.compare(0, 100, relay, 0, 100) == 0);
	CHECK(image.compare(image.size() - 50, 50, relay, relay.size() - 50, 50) == 0);
	CHECK(PS5Notify::RelayImage(relay, std::string(16384, '{')).empty()); // too long
	CHECK(PS5Notify::RelayImage(std::string(20000, 'x'), trophy).empty()); // no room
	std::string filled = relay;
	filled[100 + marker.size() + 10] = 'x'; // the room not empty: not the room
	CHECK(PS5Notify::RelayImage(filled, trophy).empty());

	// A stand-in payload loader on a port of the PC's: it reads what is sent until the sender stops
	// writing, keeps it, and closes, as the loader does when the payload has ended.
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	socklen_t length = sizeof(address);
	CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
	CHECK(listen(listener, 4) == 0);
	std::vector<std::string> received;
	std::thread loader([&] {
		for (int i = 0; i < 2; i++)
		{
			const int client = accept(listener, nullptr, nullptr);
			if (client < 0)
				return;
			std::string data;
			char block[4096];
			ssize_t got;
			while ((got = recv(client, block, sizeof(block), 0)) > 0)
				data.append(block, static_cast<size_t>(got));
			close(client);
			const std::lock_guard lock(s_mutex);
			received.push_back(std::move(data));
		}
	});

	const std::string relay_path = work + "/notify-relay.elf";
	if (FILE* const fp = std::fopen(relay_path.c_str(), "wb"))
	{
		std::fwrite(relay.data(), 1, relay.size(), fp);
		std::fclose(fp);
	}
	setenv("CRUX2_NOTIFY_RELAY", relay_path.c_str(), 1);
	setenv("CRUX2_NOTIFY_PORT", std::to_string(ntohs(address.sin_port)).c_str(), 1);
	setenv("CRUX2_NOTIFY_GAP_MS", "50", 1);

	CHECK(PS5Notify::RichState() == -1);
	CHECK(PS5Notify::AchievementUnlocked("First steps", "Finish the first stage", "https://media.example/Badge/2.png"));
	CHECK(PS5Notify::AchievementsMastered("Mastered A Game", "10 achievements, 100 points", "ftp://dropped/icon.png"));
	PS5Notify::Flush(10000);
	loader.join();
	close(listener);
	CHECK(PS5Notify::RichState() == 1);
	CHECK(received.size() == 2);
	if (received.size() == 2)
	{
		CHECK(received[0].size() == relay.size());
		CHECK(Has(received[0], "\"body\":\"First steps\""));
		CHECK(Has(received[0], "psfx_trophy_toast"));
		CHECK(Has(received[0], "https://media.example/Badge/2.png"));
		CHECK(Has(received[1], "psfx_platinum_trophy_toast"));
		CHECK(!Has(received[1], "ftp://")); // an address of another kind is no picture
	}
	CHECK(s_plain.empty());

	// Nothing listens any more: the text goes out plain, and from then on PCSX2 draws its own.
	PS5Notify::Rich("Second", "Its line", "", Sound::Trophy, true);
	PS5Notify::Flush(10000);
	CHECK(PS5Notify::RichState() == 0);
	CHECK(s_plain.size() == 1 && s_plain[0] == "Second\nIts line");
	CHECK(!PS5Notify::AchievementUnlocked("Third", "", ""));

	// The setting off: never the console's.
	setenv("CRUX2_NOTIFY_OFF", "1", 1);
	CHECK(!PS5Notify::ConsoleNotificationsOn());

	if (s_failures == 0)
		std::printf("notifications: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
