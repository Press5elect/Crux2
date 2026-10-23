// Crux2 - the web panel on a PC: the server on 127.0.0.1, asked over plain sockets.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The title is played by stand-in hooks that write down what they were asked and on which
// thread; this program's main thread is the title's, and calls PS5Web::tick() while the requests
// are made from other threads. Checked: the key (and only the key) opens the panel, the page and
// every endpoint, JSON that survives quotes, control characters and broken UTF-8, the hooks
// running on the main thread only, limits and malformed requests, many requests at once, a new
// key, a prompt shutdown, the QR code's structure, and a few thousand requests of random bytes.
//
//   web_test

#include "ps5_web.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define EXPECT(c, what) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAILED: %s (line %d)\n", what, __LINE__); } } while (0)

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// ---- the stand-in title -----------------------------------------------------------------------

static std::thread::id g_main;
static std::mutex g_record_mutex;
static std::atomic<int> g_off_main{0};
static std::atomic<int> g_list_calls{0}, g_status_calls{0};
static std::vector<std::string> g_started, g_typed, g_actions, g_keys, g_logs, g_saved_keys;
static bool g_action_answer = true, g_emu_log_exists = true;

static void OnMain()
{
	if (std::this_thread::get_id() != g_main)
		g_off_main++;
}

static void Record(std::vector<std::string>& list, const std::string& what)
{
	std::lock_guard lock(g_record_mutex);
	list.push_back(what);
}

static size_t Count(std::vector<std::string>& list)
{
	std::lock_guard lock(g_record_mutex);
	return list.size();
}

static const std::string kTrickyTitle = std::string("Ti\"tle \\ with\nnew line, caf\xc3\xa9, \xf0\x9f\x8e\xae, broken \xff byte, <b>&");
static const std::string kTrickyPath = "/mnt/usb0/PS2/Caf\xc3\xa9 \"Edition\" & more.iso";
static const std::vector<std::uint8_t> kCover = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3, 0, 255};
static const std::string kBootLog = "boot line one\nweb: listening on port 3311\n";

static PS5Web::Hooks MakeHooks()
{
	PS5Web::Hooks hooks;
	hooks.listGames = [] {
		OnMain();
		g_list_calls++;
		std::vector<PS5Web::Game> games;
		games.push_back({kTrickyTitle, "SLUS-20946", "NTSC-U", kTrickyPath, true});
		games.push_back({"Plain Game", "SCES-50361", "PAL", "/data/crux2/games/plain.chd", false});
		games.push_back({"", "", "", "x", false});
		return games;
	};
	hooks.startGame = [](const std::string& path) {
		OnMain();
		Record(g_started, path);
		return true;
	};
	hooks.status = [] {
		OnMain();
		g_status_calls++;
		PS5Web::Status status;
		status.running = true;
		status.paused = false;
		status.fast_forward = true;
		status.text_field_open = true;
		status.title = "Running \"One\"";
		status.serial = "SLUS-20946";
		status.speed_percent = 99.6f;
		status.fps = 59.94f;
		return status;
	};
	hooks.action = [](const std::string& name, int arg) {
		OnMain();
		Record(g_actions, name + ":" + std::to_string(arg));
		return g_action_answer;
	};
	hooks.typeText = [](const std::string& text) {
		OnMain();
		Record(g_typed, text);
		return true;
	};
	hooks.pressKey = [](PS5Web::Key key) {
		OnMain();
		Record(g_keys, key == PS5Web::Key::Enter ? "enter" : "backspace");
		return true;
	};
	hooks.readLog = [](PS5Web::Log which, std::string* text) {
		OnMain();
		if (which == PS5Web::Log::Boot)
		{
			*text = kBootLog;
			return true;
		}
		if (!g_emu_log_exists)
			return false;
		*text = "emulog \xe2\x9c\x93\n";
		return true;
	};
	hooks.cover = [](const std::string& serial, std::vector<std::uint8_t>* bytes, std::string* mime) {
		OnMain();
		if (serial != "SLUS-20946")
			return false;
		*bytes = kCover;
		*mime = "";
		return true;
	};
	hooks.localAddress = [] {
		OnMain();
		return std::string("127.0.0.1");
	};
	hooks.keyChanged = [](const std::string& key) { Record(g_saved_keys, key); };
	return hooks;
}

static void Logger(const char* line)
{
	Record(g_logs, line);
}

// ---- a client ---------------------------------------------------------------------------------

struct Reply
{
	int code = 0; ///< 0: no answer at all
	std::string head; ///< status line and headers, lowercase names kept as sent
	std::string body;
	std::string raw;
	std::string Header(const std::string& name) const
	{
		std::string lower_head = head;
		for (char& c : lower_head)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		std::string key = "\r\n" + name + ":";
		for (char& c : key)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const size_t at = lower_head.find(key);
		if (at == std::string::npos)
			return "";
		size_t start = at + key.size();
		while (start < head.size() && head[start] == ' ')
			start++;
		const size_t end = head.find("\r\n", start);
		return head.substr(start, end - start);
	}
};

static int Connect(int port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_port = htons(static_cast<std::uint16_t>(port));
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0)
	{
		close(fd);
		return -1;
	}
	timeval limit = {5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
	return fd;
}

static bool SendAll(int fd, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

static Reply ReadReply(int fd)
{
	Reply reply;
	char block[8192];
	for (;;)
	{
		const ssize_t n = recv(fd, block, sizeof(block), 0);
		if (n <= 0)
			break;
		reply.raw.append(block, static_cast<size_t>(n));
	}
	const size_t end = reply.raw.find("\r\n\r\n");
	if (reply.raw.compare(0, 9, "HTTP/1.1 ") != 0 || end == std::string::npos)
		return reply;
	reply.code = std::atoi(reply.raw.c_str() + 9);
	reply.head = reply.raw.substr(0, end);
	reply.body = reply.raw.substr(end + 4);
	const std::string length = reply.Header("Content-Length");
	if (length.empty() || std::strtoull(length.c_str(), nullptr, 10) != reply.body.size())
		reply.code = -reply.code; // a broken answer shows as a negative code
	return reply;
}

/// One exchange on a client thread while this (main) thread ticks.
static Reply Exchange(int port, const std::string& request, bool tick = true, bool half_close = false)
{
	Reply reply;
	std::atomic<bool> done{false};
	std::thread client([&] {
		const int fd = Connect(port);
		if (fd >= 0)
		{
			SendAll(fd, request);
			if (half_close)
				shutdown(fd, SHUT_WR);
			reply = ReadReply(fd);
			close(fd);
		}
		done = true;
	});
	while (!done)
	{
		if (tick)
			PS5Web::tick();
		std::this_thread::sleep_for(1ms);
	}
	client.join();
	return reply;
}

static std::string Get(const std::string& target, const std::string& extra = "")
{
	return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n";
}

static std::string Post(const std::string& target, const std::string& body, const std::string& extra = "")
{
	return "POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}

static void TickFor(std::chrono::milliseconds time)
{
	const auto end = Clock::now() + time;
	while (Clock::now() < end)
	{
		PS5Web::tick();
		std::this_thread::sleep_for(2ms);
	}
}

// ---- a small JSON reader, to check what the server wrote is JSON ------------------------------

struct Value
{
	enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
	bool boolean = false;
	double number = 0;
	std::string text;
	std::vector<Value> items;
	std::vector<std::string> names;
	std::vector<Value> fields;
	const Value* Field(const std::string& name) const
	{
		for (size_t i = 0; i < names.size(); i++)
			if (names[i] == name)
				return &fields[i];
		return nullptr;
	}
};

struct JsonReader
{
	const std::string& s;
	size_t at = 0;
	bool ok = true;

	void Space()
	{
		while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r' || s[at] == '\t'))
			at++;
	}
	static void Put(std::string& out, unsigned long c)
	{
		if (c < 0x80)
			out += static_cast<char>(c);
		else if (c < 0x800)
			out += static_cast<char>(0xc0 | (c >> 6)), out += static_cast<char>(0x80 | (c & 63));
		else if (c < 0x10000)
			out += static_cast<char>(0xe0 | (c >> 12)), out += static_cast<char>(0x80 | ((c >> 6) & 63)), out += static_cast<char>(0x80 | (c & 63));
		else
			out += static_cast<char>(0xf0 | (c >> 18)), out += static_cast<char>(0x80 | ((c >> 12) & 63)),
				out += static_cast<char>(0x80 | ((c >> 6) & 63)), out += static_cast<char>(0x80 | (c & 63));
	}
	std::string Str()
	{
		std::string out;
		if (at >= s.size() || s[at] != '"')
			return ok = false, out;
		at++;
		while (at < s.size() && s[at] != '"')
		{
			const unsigned char c = static_cast<unsigned char>(s[at]);
			if (c < 0x20)
				return ok = false, out;
			if (c != '\\')
			{
				out += static_cast<char>(c);
				at++;
				continue;
			}
			if (++at >= s.size())
				return ok = false, out;
			const char e = s[at++];
			if (e == 'u')
			{
				if (at + 4 > s.size())
					return ok = false, out;
				Put(out, std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
				at += 4;
			}
			else if (e == '"' || e == '\\' || e == '/')
				out += e;
			else if (e == 'n')
				out += '\n';
			else if (e == 't')
				out += '\t';
			else
				return ok = false, out;
		}
		if (at >= s.size())
			return ok = false, out;
		at++;
		return out;
	}
	Value Read()
	{
		Value v;
		Space();
		if (at >= s.size())
			return ok = false, v;
		if (s[at] == '"')
			v.kind = Value::String, v.text = Str();
		else if (s[at] == '[')
		{
			v.kind = Value::Array;
			at++;
			Space();
			if (at < s.size() && s[at] == ']')
				return at++, v;
			for (;;)
			{
				v.items.push_back(Read());
				Space();
				if (at < s.size() && s[at] == ',')
				{
					at++;
					continue;
				}
				if (at < s.size() && s[at] == ']')
					return at++, v;
				return ok = false, v;
			}
		}
		else if (s[at] == '{')
		{
			v.kind = Value::Object;
			at++;
			Space();
			if (at < s.size() && s[at] == '}')
				return at++, v;
			for (;;)
			{
				Space();
				std::string name = Str();
				Space();
				if (at >= s.size() || s[at] != ':')
					return ok = false, v;
				at++;
				v.names.push_back(name);
				v.fields.push_back(Read());
				Space();
				if (at < s.size() && s[at] == ',')
				{
					at++;
					continue;
				}
				if (at < s.size() && s[at] == '}')
					return at++, v;
				return ok = false, v;
			}
		}
		else if (s.compare(at, 4, "true") == 0)
			v.kind = Value::Bool, v.boolean = true, at += 4;
		else if (s.compare(at, 5, "false") == 0)
			v.kind = Value::Bool, at += 5;
		else if (s.compare(at, 4, "null") == 0)
			at += 4;
		else
		{
			char* end = nullptr;
			v.kind = Value::Number;
			v.number = std::strtod(s.c_str() + at, &end);
			if (end == s.c_str() + at)
				ok = false;
			at = static_cast<size_t>(end - s.c_str());
		}
		return v;
	}
};

static bool ParseJson(const std::string& text, Value* value)
{
	JsonReader reader{text};
	*value = reader.Read();
	reader.Space();
	return reader.ok && reader.at == text.size();
}

static bool ValidUtf8(const std::string& text)
{
	size_t i = 0;
	while (i < text.size())
	{
		const unsigned char c = static_cast<unsigned char>(text[i]);
		const int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (n < 0 || i + n >= text.size())
			return false;
		for (int k = 1; k <= n; k++)
			if ((static_cast<unsigned char>(text[i + k]) & 0xc0) != 0x80)
				return false;
		i += 1 + n;
	}
	return true;
}

static bool LooksLikeAddress(const std::string& line)
{
	int dots = 0, digits = 0;
	for (size_t i = 0; i < line.size(); i++)
	{
		const char c = line[i];
		if (c >= '0' && c <= '9')
			digits++;
		else if (c == '.' && digits > 0)
		{
			dots++;
			digits = 0;
			if (dots == 3 && i + 1 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '9')
				return true;
		}
		else
			dots = digits = 0;
	}
	return false;
}

// ---- the QR code's structure ------------------------------------------------------------------

static bool Finder(const PS5Web::QrCode& code, int left, int top)
{
	for (int dy = -1; dy <= 7; dy++)
		for (int dx = -1; dx <= 7; dx++)
		{
			const int x = left + dx, y = top + dy;
			if (x < 0 || y < 0 || x >= code.size || y >= code.size)
				continue;
			const int ring = std::max(std::abs(dx - 3), std::abs(dy - 3));
			const bool want_dark = ring != 2 && ring != 4; // 0, 1: centre; 2: light; 3: dark ring; 4: separator
			if (code.dark(x, y) != want_dark)
				return false;
		}
	return true;
}

static int FormatBch(int data)
{
	int rem = data;
	for (int i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	return (data << 10 | rem) ^ 0x5412;
}

static void QrChecks()
{
	const std::string url = "http://192.0.2.10:3311/?key=0123456789abcdef0123456789abcdef";
	const PS5Web::QrCode code = PS5Web::qr(url);
	EXPECT(code.size >= 21 && code.size <= 177 && (code.size - 17) % 4 == 0, "the QR code has a valid size");
	EXPECT(code.modules.size() == static_cast<size_t>(code.size) * code.size, "the QR code has size x size modules");
	EXPECT(Finder(code, 0, 0) && Finder(code, code.size - 7, 0) && Finder(code, 0, code.size - 7), "the three finder patterns are there");
	EXPECT(!Finder(code, code.size - 7, code.size - 7), "and not a fourth");
	bool timing = true;
	for (int i = 8; i < code.size - 8; i++)
		timing = timing && code.dark(i, 6) == (i % 2 == 0) && code.dark(6, i) == (i % 2 == 0);
	EXPECT(timing, "the timing patterns alternate");
	EXPECT(code.dark(8, code.size - 8), "the dark module is there");

	int first = 0, second = 0;
	for (int i = 0; i <= 5; i++)
		first |= code.dark(8, i) << i;
	first |= code.dark(8, 7) << 6;
	first |= code.dark(8, 8) << 7;
	first |= code.dark(7, 8) << 8;
	for (int i = 9; i < 15; i++)
		first |= code.dark(14 - i, 8) << i;
	for (int i = 0; i < 8; i++)
		second |= code.dark(code.size - 1 - i, 8) << i;
	for (int i = 8; i < 15; i++)
		second |= code.dark(8, code.size - 15 + i) << i;
	EXPECT(first == second, "both copies of the format information agree");
	int data = -1;
	for (int d = 0; d < 32; d++)
		if (FormatBch(d) == first)
			data = d;
	EXPECT(data >= 0, "the format information is a valid BCH code word");
	const int ecl_bits = data >> 3;
	EXPECT(data >= 0 && ecl_bits != 1, "the error correction is M or better (not L)");

	// The version for this length (ECC M, 70 bytes, mode bytes): version 4 is 33 modules.
	const int version = (code.size - 17) / 4;
	EXPECT(version >= 4 && version <= 6, "a URL of this length takes a small version");
	if (version >= 7)
		EXPECT(false, "version information is needed");

	EXPECT(PS5Web::qr(std::string(3000, 'x')).size == 0, "text that cannot fit gives size 0");
	EXPECT(PS5Web::qr("").size == 21, "empty text is the smallest code");
	const PS5Web::QrCode again = PS5Web::qr(url);
	EXPECT(again.modules == code.modules, "the same text gives the same code");

	// The picture, should a reader be installed: decoded back to the text.
	if (std::system("command -v zbarimg > /dev/null 2>&1") == 0)
	{
		const char* path = "/tmp/crux2-web-test-qr.pbm";
		if (FILE* f = std::fopen(path, "w"))
		{
			const int scale = 4, border = 4, side = (code.size + 2 * border) * scale;
			std::fprintf(f, "P1\n%d %d\n", side, side);
			for (int y = 0; y < side; y++)
			{
				for (int x = 0; x < side; x++)
					std::fputs(code.dark(x / scale - border, y / scale - border) ? "1 " : "0 ", f);
				std::fputc('\n', f);
			}
			std::fclose(f);
			const std::string command = std::string("zbarimg -q --raw ") + path;
			std::string read;
			if (FILE* p = popen(command.c_str(), "r"))
			{
				char block[512];
				size_t n;
				while ((n = std::fread(block, 1, sizeof(block), p)) > 0)
					read.append(block, n);
				pclose(p);
			}
			while (!read.empty() && (read.back() == '\n' || read.back() == '\r'))
				read.pop_back();
			EXPECT(read == url, "zbarimg reads the QR code back as the address");
			std::remove(path);
		}
	}
}

// ---- the tests --------------------------------------------------------------------------------

int main()
{
	g_main = std::this_thread::get_id();
	const auto test_start = Clock::now();

	QrChecks();

	PS5Web::Options options;
	options.bind_address = "127.0.0.1";
	options.port = 0;
	PS5Web::init(MakeHooks(), Logger, options);
	const std::string key = PS5Web::key();
	EXPECT(key.size() == 32 && key.find_first_not_of("0123456789abcdef") == std::string::npos, "the key is 32 hexadecimal digits");
	EXPECT(Count(g_saved_keys) == 1 && g_saved_keys[0] == key, "keyChanged was told the new key");
	EXPECT(!PS5Web::listening() && PS5Web::url().empty() && PS5Web::status() == "Off", "init does not start the server");

	EXPECT(PS5Web::apply(true), "apply(true) starts it");
	const int port = PS5Web::port();
	EXPECT(PS5Web::listening() && port > 0, "it listens on a port");
	EXPECT(PS5Web::url() == "http://127.0.0.1:" + std::to_string(port) + "/?key=" + key, "url() is the address with the key");
	EXPECT(PS5Web::status().find("127.0.0.1:" + std::to_string(port)) != std::string::npos, "status() names the address for the screen");
	EXPECT(PS5Web::apply(true) && PS5Web::port() == port, "apply(true) again changes nothing");

	// The key.
	Reply r = Exchange(port, Get("/"));
	EXPECT(r.code == 403 && r.body.find("<html") == std::string::npos, "the page without the key: 403");
	r = Exchange(port, Get("/api/games"));
	EXPECT(r.code == 403 && g_list_calls == 0, "the games without the key: 403, and nothing asked of the title");
	r = Exchange(port, Post("/api/start", "x"));
	EXPECT(r.code == 403 && Count(g_started) == 0, "starting a game without the key: 403, nothing started");
	std::string near = key;
	near[31] = near[31] == '0' ? '1' : '0';
	EXPECT(Exchange(port, Get("/?key=" + near)).code == 403, "a key one digit off: 403");
	EXPECT(Exchange(port, Get("/?key=" + key.substr(0, 31))).code == 403, "a key one digit short: 403");
	EXPECT(Exchange(port, Get("/?key=" + key + "0")).code == 403, "a key one digit long: 403");
	EXPECT(Exchange(port, Get("/?key=")).code == 403, "an empty key: 403");
	EXPECT(Exchange(port, Get("/", "Cookie: crux2_key=" + near + "\r\n")).code == 403, "a wrong cookie: 403");

	r = Exchange(port, Get("/?key=" + key));
	EXPECT(r.code == 200, "the page with the key: 200");
	EXPECT(r.Header("Content-Type").find("text/html") == 0, "it is HTML");
	EXPECT(r.body.find("<!doctype html>") == 0 && r.body.find("</html>") != std::string::npos, "the page is whole");
	EXPECT(r.body.find("http://") == std::string::npos && r.body.find("https://") == std::string::npos && r.body.find("//cdn") == std::string::npos,
		"the page fetches nothing from elsewhere");
	EXPECT(r.Header("Set-Cookie").find("crux2_key=" + key) == 0 && r.Header("Set-Cookie").find("HttpOnly") != std::string::npos &&
			r.Header("Set-Cookie").find("SameSite=Strict") != std::string::npos,
		"a keyed address sets the cookie");
	EXPECT(r.Header("Content-Security-Policy").find("default-src 'none'") != std::string::npos, "the page has a strict content policy");
	EXPECT(r.Header("Referrer-Policy") == "no-referrer" && r.Header("Cache-Control") == "no-store", "no referrer, nothing cached");
	EXPECT(r.body.find("logs/emulog.txt") != std::string::npos && r.body.find("publicly") != std::string::npos, "the page warns about posting logs");

	const std::string cookie = "Cookie: other=1; crux2_key=" + key + "\r\n";
	r = Exchange(port, Get("/api/status", cookie));
	EXPECT(r.code == 200 && r.Header("Set-Cookie").empty(), "the cookie alone opens it, and sets nothing");
	EXPECT(Exchange(port, Get("/api/status?key=" + key)).code == 200, "the key in the address alone opens it");

	// The status, taken by tick().
	TickFor(300ms);
	r = Exchange(port, Get("/api/status", cookie));
	Value v;
	EXPECT(r.code == 200 && ParseJson(r.body, &v) && v.kind == Value::Object, "the status is a JSON object");
	EXPECT(v.Field("title") && v.Field("title")->text == "Running \"One\"" && v.Field("serial") && v.Field("serial")->text == "SLUS-20946",
		"the status has the title and serial");
	EXPECT(v.Field("speed") && v.Field("speed")->number == 100 && v.Field("fps") && std::abs(v.Field("fps")->number - 59.9) < 0.01,
		"the status has speed and fps");
	EXPECT(v.Field("running") && v.Field("running")->boolean && v.Field("fast_forward") && v.Field("fast_forward")->boolean &&
			v.Field("paused") && !v.Field("paused")->boolean && v.Field("text_field") && v.Field("text_field")->boolean,
		"the status has its flags");
	EXPECT(g_status_calls > 0 && g_status_calls < 20, "status() is asked a few times a second, not every tick");

	// The library.
	r = Exchange(port, Get("/api/games", cookie));
	EXPECT(r.code == 200 && g_list_calls == 1, "the games: 200, listGames asked once");
	EXPECT(ValidUtf8(r.body), "the games JSON is valid UTF-8");
	EXPECT(ParseJson(r.body, &v) && v.kind == Value::Array && v.items.size() == 3, "the games are a JSON array of three");
	if (v.kind == Value::Array && v.items.size() == 3)
	{
		const Value& game = v.items[0];
		std::string expected = kTrickyTitle;
		expected.replace(expected.find('\xff'), 1, "\xef\xbf\xbd");
		EXPECT(game.Field("title") && game.Field("title")->text == expected, "a title with quotes, a new line, accents and emoji survives (a broken byte becomes U+FFFD)");
		EXPECT(game.Field("path") && game.Field("path")->text == kTrickyPath, "the path survives");
		EXPECT(game.Field("cover") && game.Field("cover")->boolean && v.items[1].Field("cover") && !v.items[1].Field("cover")->boolean, "has-cover flags");
		EXPECT(v.items[1].Field("region") && v.items[1].Field("region")->text == "PAL", "the region");
		EXPECT(v.items[2].Field("title") && v.items[2].Field("title")->text.empty(), "an empty title is an empty string");
	}

	// Starting a game: queued, and done by tick() on the main thread.
	{
		Reply started;
		std::thread client([&] {
			const int fd = Connect(port);
			SendAll(fd, Post("/api/start", kTrickyPath, cookie));
			started = ReadReply(fd);
			close(fd);
		});
		std::this_thread::sleep_for(150ms);
		EXPECT(Count(g_started) == 0, "nothing is started before tick()");
		TickFor(100ms);
		client.join();
		EXPECT(started.code == 200 && Count(g_started) == 1 && g_started[0] == kTrickyPath, "tick() started the game with the path as sent");
	}
	r = Exchange(port, Post("/api/start?path=%2Fdata%2Fa%20b.iso", "", cookie));
	EXPECT(r.code == 200 && Count(g_started) == 2 && g_started[1] == "/data/a b.iso", "the path may come in the address");
	EXPECT(Exchange(port, Post("/api/start", "", cookie)).code == 400, "no path: 400");
	EXPECT(Exchange(port, Get("/api/start", cookie)).code == 405, "GET /api/start: 405");

	// Actions.
	const char* simple[] = {"pause", "resume", "quick_menu", "leave"};
	for (const char* name : simple)
		EXPECT(Exchange(port, Post(std::string("/api/action?name=") + name, "", cookie)).code == 200, name);
	for (int slot = 1; slot <= 10; slot++)
		EXPECT(Exchange(port, Post("/api/action?name=save_state&arg=" + std::to_string(slot), "", cookie)).code == 200, "save_state slot");
	EXPECT(Exchange(port, Post("/api/action?name=load_state&arg=3", "", cookie)).code == 200, "load_state 3");
	EXPECT(Exchange(port, Post("/api/action?name=fast_forward&arg=1", "", cookie)).code == 200, "fast_forward on");
	EXPECT(Exchange(port, Post("/api/action?name=fast_forward&arg=0", "", cookie)).code == 200, "fast_forward off");
	{
		std::lock_guard lock(g_record_mutex);
		EXPECT(g_actions.size() == 17 && g_actions[0] == "pause:0" && g_actions[3] == "leave:0" && g_actions[4] == "save_state:1" &&
				g_actions[13] == "save_state:10" && g_actions[14] == "load_state:3" && g_actions[15] == "fast_forward:1" &&
				g_actions[16] == "fast_forward:0",
			"every action reached the title, in order, with its argument");
	}
	const char* refused[] = {"name=save_state&arg=0", "name=save_state&arg=11", "name=save_state&arg=abc", "name=save_state", "name=load_state&arg=-1",
		"name=fast_forward&arg=2", "name=screenshot", "name=format_memory_card", "name=", ""};
	for (const char* query : refused)
		EXPECT(Exchange(port, Post(std::string("/api/action?") + query, "", cookie)).code == 400, query);
	EXPECT(Count(g_actions) == 17, "refused actions never reach the title");
	EXPECT(Exchange(port, Get("/api/action?name=pause", cookie)).code == 405, "GET /api/action: 405");
	g_action_answer = false;
	r = Exchange(port, Post("/api/action?name=pause", "", cookie));
	EXPECT(r.code == 409 && r.body.find("\"ok\":false") != std::string::npos, "an action the title could not do: 409");
	g_action_answer = true;

	// Typing.
	const std::string typed = "h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93 \"quoted\" & <b>";
	EXPECT(Exchange(port, Post("/api/text", typed, cookie)).code == 200 && Count(g_typed) == 1 && g_typed[0] == typed, "typed text arrives as sent");
	EXPECT(Exchange(port, Post("/api/text", "a\nb", cookie)).code == 400, "a control character: 400");
	EXPECT(Exchange(port, Post("/api/text", "bad \xc3", cookie)).code == 400, "broken UTF-8: 400");
	EXPECT(Exchange(port, Post("/api/text", "\xed\xa0\x80", cookie)).code == 400, "a surrogate: 400");
	EXPECT(Exchange(port, Post("/api/text", "\xc0\xaf", cookie)).code == 400, "an overlong form: 400");
	EXPECT(Exchange(port, Post("/api/text", "", cookie)).code == 400, "nothing: 400");
	EXPECT(Exchange(port, Post("/api/text", std::string(5000, 'a'), cookie)).code == 413, "too much text: 413");
	EXPECT(Count(g_typed) == 1, "refused text never reaches the title");
	EXPECT(Exchange(port, Post("/api/key?name=enter", "", cookie)).code == 200, "Enter");
	EXPECT(Exchange(port, Post("/api/key?name=backspace", "", cookie)).code == 200, "Backspace");
	EXPECT(Exchange(port, Post("/api/key?name=escape", "", cookie)).code == 400, "another key: 400");
	EXPECT(Count(g_keys) == 2 && g_keys[0] == "enter" && g_keys[1] == "backspace", "the keys arrive");

	// Logs.
	r = Exchange(port, Get("/api/log?which=boot", cookie));
	EXPECT(r.code == 200 && r.body == kBootLog, "the boot log");
	EXPECT(r.Header("Content-Type").find("text/plain") == 0 && r.Header("Content-Disposition") == "attachment; filename=\"crux2-boot.log\"",
		"as a text attachment named crux2-boot.log");
	r = Exchange(port, Get("/api/log?which=emu", cookie));
	EXPECT(r.code == 200 && r.Header("Content-Disposition") == "attachment; filename=\"emulog.txt\"" && r.body == "emulog \xe2\x9c\x93\n", "the emulator's log");
	g_emu_log_exists = false;
	EXPECT(Exchange(port, Get("/api/log?which=emu", cookie)).code == 404, "no log: 404");
	EXPECT(Exchange(port, Get("/api/log?which=../../etc/passwd", cookie)).code == 400, "another log: 400");

	// Covers.
	r = Exchange(port, Get("/api/cover?serial=SLUS-20946", cookie));
	EXPECT(r.code == 200 && r.body == std::string(kCover.begin(), kCover.end()), "the cover's bytes, whole");
	EXPECT(r.Header("Content-Type") == "image/png" && r.Header("Cache-Control").find("max-age=") != std::string::npos, "as a PNG, cached by the browser");
	EXPECT(Exchange(port, Get("/api/cover?serial=SCES-50361", cookie)).code == 404, "no cover: 404");
	EXPECT(Exchange(port, Get("/api/cover?serial=..%2F..%2Fx", cookie)).code == 400, "a serial with a path in it: 400");
	EXPECT(Exchange(port, Get("/api/cover", cookie)).code == 400, "no serial: 400");
	EXPECT(Exchange(port, Get("/nothing", cookie)).code == 404, "nothing there: 404");
	EXPECT(Exchange(port, Get("/api/nothing", cookie)).code == 404, "no such endpoint: 404");

	// Limits.
	r = Exchange(port, Get("/api/status?key=" + key, "X-Padding: " + std::string(17000, 'a') + "\r\n"));
	EXPECT(r.code == 431, "headers over 16 KiB: 431");
	r = Exchange(port, Get("/api/status?key=" + key, "X-Padding: " + std::string(20000, 'a')), true, true);
	EXPECT(r.code == 431, "16 KiB of headers that never end: 431");
	r = Exchange(port, "POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 70000\r\n\r\n");
	EXPECT(r.code == 413, "a body over 64 KiB: 413 before it is sent");
	r = Exchange(port, "POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 70000\r\n\r\n" + std::string(70000, 'a'));
	EXPECT(r.code == 413, "a body over 64 KiB sent anyway: the 413 still arrives");
	r = Exchange(port, "POST /api/text HTTP/1.1\r\nContent-Length: 70000\r\n\r\n");
	EXPECT(r.code == 403, "an oversized body without the key: 403");
	r = Exchange(port, "POST /api/text HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n");
	EXPECT(r.code == 403, "an absurd length without the key: 403");
	r = Exchange(port, "POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n");
	EXPECT(r.code == 413, "an absurd length with the key: 413");
	r = Exchange(port, Post("/api/start", std::string(60000, 'p'), cookie));
	EXPECT(r.code == 200 && Count(g_started) == 3 && g_started[2].size() == 60000, "a body just under 64 KiB is taken whole");

	// Malformed requests.
	const std::string malformed[] = {
		"GARBAGE\r\n\r\n",
		"GET / HTTP/2.0\r\n\r\n",
		"GET noslash HTTP/1.1\r\n\r\n",
		"GET  / HTTP/1.1\r\n\r\n",
		"get / HTTP/1.1\r\n\r\n",
		"GET / HTTP/1.1\r\nNo colon here\r\n\r\n",
		"GET / HTTP/1.1\r\n folded: x\r\n\r\n",
		"GET / HTTP/1.1\r\n: empty name\r\n\r\n",
		"GET / HTTP/1.1\r\nBad Name: x\r\n\r\n",
		"POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
		"POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
		"GET /\x01 HTTP/1.1\r\n\r\n",
		"GET / HTTP/1.1\r\nX: a\x01b\r\n\r\n",
		std::string("GET /\0 HTTP/1.1\r\n\r\n", 20),
		"\r\n\r\n",
	};
	for (const std::string& request : malformed)
	{
		r = Exchange(port, request);
		EXPECT(r.code == 400, ("400 for a malformed request: " + request.substr(0, 24)).c_str());
	}
	r = Exchange(port, "POST /api/text?key=" + key + " HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
	EXPECT(r.code == 501, "a chunked body: 501");
	std::string many = "GET /api/status?key=" + key + " HTTP/1.1\r\n";
	for (int i = 0; i < 150; i++)
		many += "X-" + std::to_string(i) + ": 1\r\n";
	EXPECT(Exchange(port, many + "\r\n").code == 431, "too many headers: 431");
	r = Exchange(port, "GET /api/sta", true, true);
	EXPECT(r.code == 0, "half a request and a close: no answer, no harm");
	r = Exchange(port, "POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", true, true);
	EXPECT(r.code == 0 && Count(g_typed) == 1, "a body cut short: nothing typed");
	EXPECT(Exchange(port, Get("/api/status", cookie)).code == 200, "the server still answers after all of that");

	// Expect: 100-continue, as curl sends it.
	{
		Reply reply;
		std::string interim;
		std::atomic<bool> done{false};
		std::thread client([&] {
			const int fd = Connect(port);
			SendAll(fd, "POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n");
			char block[256];
			const ssize_t n = recv(fd, block, sizeof(block), 0);
			if (n > 0)
				interim.assign(block, static_cast<size_t>(n));
			SendAll(fd, "abcd");
			reply = ReadReply(fd);
			close(fd);
			done = true;
		});
		while (!done)
		{
			PS5Web::tick();
			std::this_thread::sleep_for(1ms);
		}
		client.join();
		EXPECT(interim == "HTTP/1.1 100 Continue\r\n\r\n", "Expect: 100-continue is answered 100 Continue");
		EXPECT(reply.code == 200 && Count(g_typed) == 2 && g_typed[1] == "abcd", "and the body is taken after it");
	}

	// A silent client holds nothing up; many at once are all answered.
	{
		const int silent = Connect(port);
		const auto start = Clock::now();
		EXPECT(Exchange(port, Get("/api/status", cookie)).code == 200 && Clock::now() - start < 1s, "a silent connection holds nothing up");
		std::atomic<int> answered{0}, games_ok{0};
		std::atomic<int> finished{0};
		std::vector<std::thread> clients;
		const int kClients = 12, kEach = 10;
		const int list_before = g_list_calls;
		for (int c = 0; c < kClients; c++)
			clients.emplace_back([&, c] {
				for (int i = 0; i < kEach; i++)
				{
					const int fd = Connect(port);
					if (fd < 0)
						continue;
					const bool games = (i + c) % 2 == 0;
					SendAll(fd, Get(games ? "/api/games" : "/api/status", cookie));
					const Reply reply = ReadReply(fd);
					close(fd);
					if (reply.code == 200)
					{
						answered++;
						Value value;
						if (games && ParseJson(reply.body, &value) && value.items.size() == 3)
							games_ok++;
					}
				}
				finished++;
			});
		while (finished < kClients)
		{
			PS5Web::tick();
			std::this_thread::sleep_for(1ms);
		}
		for (auto& client : clients)
			client.join();
		close(silent);
		EXPECT(answered == kClients * kEach, "120 requests from 12 clients at once: all answered 200");
		EXPECT(games_ok == kClients * kEach / 2 && g_list_calls - list_before == kClients * kEach / 2, "every games answer whole, one listGames each");
	}

	// Random bytes, and requests with bytes changed.
	{
		const int kFuzz = 3000, kThreads = 8;
		std::atomic<int> sent{0}, finished{0}, answered_200{0};
		std::vector<std::thread> fuzzers;
		const std::string valid[] = {
			Get("/api/status", cookie),
			Post("/api/text", "hello", cookie),
			Post("/api/action?name=save_state&arg=2", "", cookie),
			Get("/api/games?key=" + key),
			Get("/api/cover?serial=SLUS-20946", cookie),
			"POST /api/text?key=" + key + " HTTP/1.1\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\nabc",
		};
		for (int t = 0; t < kThreads; t++)
			fuzzers.emplace_back([&, t] {
				std::mt19937 random(1234 + t);
				for (int i = t; i < kFuzz; i += kThreads)
				{
					std::string data;
					if (i % 2 == 0)
					{
						const size_t length = random() % (i % 50 == 0 ? 20000 : 600);
						for (size_t k = 0; k < length; k++)
							data += static_cast<char>(random() % 256);
						if (i % 4 == 0)
							data = "GET /" + data + " HTTP/1.1\r\n\r\n";
					}
					else
					{
						data = valid[random() % std::size(valid)];
						const int changes = 1 + static_cast<int>(random() % 4);
						for (int k = 0; k < changes && !data.empty(); k++)
						{
							const size_t at = random() % data.size();
							switch (random() % 3)
							{
								case 0: data[at] = static_cast<char>(random() % 256); break;
								case 1: data.erase(at, 1 + random() % 8); break;
								default: data.insert(at, std::string(1 + random() % 8, static_cast<char>(random() % 256))); break;
							}
						}
					}
					const int fd = Connect(port);
					if (fd < 0)
						continue;
					SendAll(fd, data);
					shutdown(fd, SHUT_WR);
					const Reply reply = ReadReply(fd);
					close(fd);
					sent++;
					if (reply.code == 200)
						answered_200++;
				}
				finished++;
			});
		while (finished < kThreads)
		{
			PS5Web::tick();
			std::this_thread::sleep_for(1ms);
		}
		for (auto& fuzzer : fuzzers)
			fuzzer.join();
		EXPECT(sent == kFuzz, "3000 requests of random and changed bytes were all taken and closed");
		EXPECT(Exchange(port, Get("/api/status", cookie)).code == 200, "the server answers after the fuzzing");
		std::printf("web: fuzzing: %d requests, %d of them still valid enough for a 200\n", sent.load(), answered_200.load());
	}

	EXPECT(g_off_main == 0, "every hook ran on the main thread");

	// A new key.
	PS5Web::newKey();
	const std::string fresh = PS5Web::key();
	EXPECT(fresh != key && fresh.size() == 32, "newKey makes another key");
	EXPECT(Count(g_saved_keys) == 2 && g_saved_keys[1] == fresh, "keyChanged was told");
	EXPECT(Exchange(port, Get("/api/status?key=" + key)).code == 403, "the old key in the address: 403");
	EXPECT(Exchange(port, Get("/api/status", cookie)).code == 403, "the old key's cookie: 403");
	EXPECT(Exchange(port, Get("/api/status?key=" + fresh)).code == 200, "the new key: 200");
	EXPECT(PS5Web::url().find(fresh) != std::string::npos, "url() carries the new key");

	// The logs say nothing of addresses or keys.
	{
		std::lock_guard lock(g_record_mutex);
		bool clean = true;
		for (const std::string& line : g_logs)
			clean = clean && !LooksLikeAddress(line) && line.find(key) == std::string::npos && line.find(fresh) == std::string::npos;
		EXPECT(clean && !g_logs.empty(), "no address and no key in the log");
	}

	// Stopping and starting again; a prompt shutdown with work outstanding.
	EXPECT(!PS5Web::apply(false) && !PS5Web::listening() && PS5Web::status() == "Off" && PS5Web::url().empty(), "apply(false) stops it");
	EXPECT(Connect(port) < 0, "nothing listens after it");
	EXPECT(PS5Web::apply(true), "it starts again");
	const int port2 = PS5Web::port();
	EXPECT(Exchange(port2, Get("/api/status?key=" + fresh)).code == 200, "and answers");
	{
		const int silent = Connect(port2);
		const int waiting = Connect(port2);
		SendAll(waiting, Get("/api/games?key=" + fresh));
		std::this_thread::sleep_for(100ms);
		const auto start = Clock::now();
		PS5Web::shutdown();
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
		std::printf("web: shutdown took %lld ms\n", static_cast<long long>(took));
		EXPECT(took < 500, "shutdown with a silent connection and a job waiting takes less than 500 ms");
		EXPECT(!PS5Web::listening() && Connect(port2) < 0, "and nothing listens after it");
		const Reply orphan = ReadReply(waiting);
		EXPECT(orphan.code == 0, "the waiting request is closed, not answered");
		close(silent);
		close(waiting);
	}

	// No tick(): a request is answered 503 in time and its job is never run late.
	{
		PS5Web::Options quick = options;
		quick.main_thread_timeout_ms = 300;
		quick.key = fresh;
		const size_t saved_before = Count(g_saved_keys);
		PS5Web::init(MakeHooks(), Logger, quick);
		EXPECT(PS5Web::key() == fresh && Count(g_saved_keys) == saved_before, "a kept key is taken as it is");
		PS5Web::apply(true);
		const int port3 = PS5Web::port();
		const int list_before = g_list_calls;
		const auto start = Clock::now();
		r = Exchange(port3, Get("/api/games?key=" + fresh), false);
		const auto took = Clock::now() - start;
		EXPECT(r.code == 503 && took >= 250ms && took < 1500ms, "no tick(): 503 after the timeout");
		TickFor(50ms);
		EXPECT(g_list_calls == list_before, "and the job is not run afterwards");
		PS5Web::Options bad = options;
		bad.key = "not a key";
		PS5Web::init(MakeHooks(), Logger, bad);
		EXPECT(PS5Web::key() != "not a key" && PS5Web::key().size() == 32, "a kept key that is not one is replaced");
		PS5Web::shutdown();
	}

	// Without the address hook the module asks the system; whatever it finds, nothing breaks.
	{
		PS5Web::Hooks hooks = MakeHooks();
		hooks.localAddress = nullptr;
		hooks.listGames = nullptr;
		PS5Web::init(hooks, Logger, options);
		EXPECT(PS5Web::apply(true), "it starts without the address hook");
		const std::string url = PS5Web::url();
		EXPECT(url.empty() || url.compare(0, 7, "http://") == 0, "url() is empty or an address");
		EXPECT(PS5Web::status().compare(0, 12, "Listening on") == 0, "status() says it listens");
		EXPECT(Exchange(PS5Web::port(), Get("/api/games?key=" + PS5Web::key())).code == 501, "a hook not given: 501");
		PS5Web::shutdown();
	}

	PS5Web::Options taken = options;
	taken.bind_address = "not an address";
	PS5Web::init(MakeHooks(), Logger, taken);
	EXPECT(!PS5Web::apply(true) && PS5Web::status().find("not valid") != std::string::npos, "a bad bind address is refused, and said");

	std::printf("web: took %.1f s\n", std::chrono::duration<double>(Clock::now() - test_start).count());
	if (g_failed)
	{
		std::printf("web: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("web: %d checks passed\n", g_checks);
	return 0;
}
