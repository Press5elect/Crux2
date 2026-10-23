// Crux2 - the web panel: the title from a phone or a computer on the same network.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Adapted from PSSwanStation's web panel (ps5/src/web.cpp), by the same author. ps5_web.h says
// what it offers; this says how.
//
// One thread serves every connection: it waits in poll() on the listening socket, on the
// connections it has (sixteen at most; more wait in the listen queue) and on a pipe through
// which the main thread wakes it, either because a job it asked for is done or because the
// server is stopping. Every socket is non-blocking, and every connection has a deadline for each
// stage (ten seconds to send its request, a minute to take the answer), so one slow or silent
// client holds nothing up and a stop is prompt. Should the console refuse the pipe, the wait is
// cut to 20 ms slices instead and everything still works, a little later.
//
// A request is read whole before anything is done with it: the request line and headers (16 KiB
// at most, else 431) and the body its Content-Length gives (64 KiB at most, else 413; chunked
// bodies are not taken, 501). It is then checked for the key before anything else: a request
// without it is answered 403 and nothing more is looked at. Every answer closes the connection,
// after which what the client still sends is read and dropped for a second, so that a client
// whose request was refused before its body was read still gets to read the refusal.
//
// What needs the title becomes a job: a function the server thread puts into a queue, which
// tick() runs on the main thread, after which the answer it made is sent. A job tick() has not
// started within main_thread_timeout_ms is taken out of the queue again and answered 503, so it
// never runs late; one tick() has started is waited for.
//
// The console's address. The address a phone must open is the one the console's traffic leaves
// from. Without a hook giving it, it is asked of the system the way PSSwanStation does: a UDP
// socket is "connected" to an address beyond the local network (connect() on a UDP socket sends
// nothing; the system only picks the route) and getsockname() names the local address on that
// route. 192.0.2.1 is used for that, a documentation address no network has, which any network
// with a default route routes like any other. On a network without one, the interface list
// (getifaddrs) gives the first interface that is up, not the loopback, and has an IPv4 address.
// The calls are socket, connect, getsockname, close, getifaddrs and freeifaddrs, all of which the
// title already makes in ps5_net.cpp. The address is never written to the log.
//
// The key: 16 bytes from the platform layer's ps5_arc4random_buf on the console, mixed with
// /dev/urandom where it can be read (on the PC, only the latter), as 32 lowercase hexadecimal
// digits. It is compared in constant time, and never logged.

#include "ps5_web.h"
#include "ps5_priority.h"

#include "third_party/qrcodegen/qrcodegen.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__PROSPERO__)
// The platform layer's (ps5platform/libc.h): no system module exports arc4random_buf to a title.
extern "C" void ps5_arc4random_buf(void* buffer, size_t bytes);
#endif

namespace PS5Web
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

#if defined(MSG_NOSIGNAL)
		constexpr int kNoSignal = MSG_NOSIGNAL;
#else
		constexpr int kNoSignal = 0;
#endif

		constexpr size_t kMaxHead = 16 * 1024;
		constexpr size_t kMaxBody = 64 * 1024;
		constexpr size_t kMaxTypedText = 4096;
		constexpr size_t kMaxConnections = 16;
		constexpr int kMaxHeaders = 100;
		constexpr auto kReadTime = std::chrono::seconds(10);
		constexpr auto kWriteTime = std::chrono::seconds(60);
		constexpr auto kLingerTime = std::chrono::seconds(1);
		constexpr size_t kLingerBytes = 1024 * 1024;
		constexpr auto kStatusEvery = std::chrono::milliseconds(250);
		constexpr auto kAddressEvery = std::chrono::seconds(5);
		constexpr const char* kCookie = "crux2_key";

		extern const char* const kPage;

		// ---- what is kept, and who may touch it ------------------------------------------------

		/// An answer: the status code, the content type, the body, and any headers of its own
		/// (each ending in CRLF).
		struct Response
		{
			int code = 200;
			std::string type = "application/json; charset=utf-8";
			std::string body;
			std::string headers;
			bool cacheable = false; ///< else Cache-Control: no-store
		};

		/// What the server thread asks the main thread to do.
		struct Job
		{
			enum class State
			{
				Queued,
				Running,
				Done,
			};
			std::function<Response()> work;
			Response result;
			State state = State::Queued;
		};

		// Set by init() and only read while the server runs (init() stops it first).
		Hooks g_hooks;
		LogFunction g_log;
		Options g_options;

		// The main thread's own.
		std::thread g_thread;
		int g_listener = -1;
		int g_wake[2] = {-1, -1};
		Clock::time_point g_status_at, g_address_at;

		std::atomic<bool> g_stopping{false};
		std::atomic<bool> g_serving{false};
		std::atomic<int> g_port{0};

		// Between the threads, under g_mutex.
		std::mutex g_mutex;
		std::string g_key;
		std::string g_address;
		std::string g_status_text = "Off";
		std::string g_status_json = "{\"running\":false}";
		std::deque<std::shared_ptr<Job>> g_jobs;

		void Note(const char* format, ...) __attribute__((format(printf, 1, 2)));
		void Note(const char* format, ...)
		{
			if (!g_log)
				return;
			char line[256];
			va_list args;
			va_start(args, format);
			std::vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			g_log(line);
		}

		std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
		std::string Format(const char* format, ...)
		{
			char text[512];
			va_list args;
			va_start(args, format);
			std::vsnprintf(text, sizeof(text), format, args);
			va_end(args);
			return text;
		}

		// ---- the key ---------------------------------------------------------------------------

		/// 16 random bytes as 32 hexadecimal digits. False when no source of random numbers
		/// answered and the key was made from the clock alone.
		bool MakeKey(std::string* key)
		{
			std::uint8_t bytes[16] = {};
			bool strong = false;
#if defined(__PROSPERO__)
			ps5_arc4random_buf(bytes, sizeof(bytes));
			strong = true;
#endif
			const int fd = open("/dev/urandom", O_RDONLY);
			if (fd >= 0)
			{
				std::uint8_t more[sizeof(bytes)];
				size_t have = 0;
				while (have < sizeof(more))
				{
					const ssize_t got = read(fd, more + have, sizeof(more) - have);
					if (got < 0 && errno == EINTR)
						continue;
					if (got <= 0)
						break;
					have += static_cast<size_t>(got);
				}
				close(fd);
				if (have == sizeof(more))
				{
					for (size_t i = 0; i < sizeof(bytes); i++)
						bytes[i] ^= more[i];
					strong = true;
				}
			}
			if (!strong)
			{
				// Better than nothing, and said in the log: the clocks and where things are.
				std::uint64_t seed = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()) ^
					static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) * 0x9e3779b97f4a7c15ull ^
					static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&seed));
				for (size_t i = 0; i < sizeof(bytes); i++)
				{
					seed += 0x9e3779b97f4a7c15ull;
					std::uint64_t z = seed;
					z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
					z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
					bytes[i] = static_cast<std::uint8_t>(z ^ (z >> 31));
				}
			}
			static const char digits[] = "0123456789abcdef";
			key->clear();
			for (const std::uint8_t byte : bytes)
			{
				key->push_back(digits[byte >> 4]);
				key->push_back(digits[byte & 15]);
			}
			return strong;
		}

		bool IsKey(const std::string& text)
		{
			if (text.size() != 32)
				return false;
			for (const char c : text)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
					return false;
			return true;
		}

		/// In time that depends on the length of the real key only, not on how much of it was right.
		bool SameKey(const std::string& given, const std::string& real)
		{
			if (real.empty())
				return false;
			unsigned difference = given.size() == real.size() ? 0u : 1u;
			for (size_t i = 0; i < real.size(); i++)
			{
				const unsigned char a = i < given.size() ? static_cast<unsigned char>(given[i]) : 0;
				difference |= a ^ static_cast<unsigned char>(real[i]);
			}
			return difference == 0;
		}

		// ---- the console's address -------------------------------------------------------------

		std::string Dotted(const in_addr& address)
		{
			const std::uint32_t value = ntohl(address.s_addr);
			return Format("%u.%u.%u.%u", value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255);
		}

		bool Usable(const in_addr& address)
		{
			const std::uint32_t value = ntohl(address.s_addr);
			return value != 0 && (value >> 24) != 127 && value != 0xffffffffu;
		}

		std::string SystemAddress()
		{
			// The route's own address, towards a documentation address (see the top of the file).
			const int fd = socket(AF_INET, SOCK_DGRAM, 0);
			if (fd >= 0)
			{
				sockaddr_in to = {};
#if defined(__FreeBSD__) || defined(__PROSPERO__)
				to.sin_len = sizeof(to);
#endif
				to.sin_family = AF_INET;
				to.sin_port = htons(9);
				to.sin_addr.s_addr = htonl(0xc0000201u); // 192.0.2.1
				sockaddr_in own = {};
				socklen_t length = sizeof(own);
				const bool found = connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0 &&
					getsockname(fd, reinterpret_cast<sockaddr*>(&own), &length) == 0 && own.sin_family == AF_INET;
				close(fd);
				if (found && Usable(own.sin_addr))
					return Dotted(own.sin_addr);
			}

			// The interface list, for a network with no way out.
			ifaddrs* first = nullptr;
			if (getifaddrs(&first) != 0)
				return "";
			std::string found;
			for (const ifaddrs* entry = first; entry && found.empty(); entry = entry->ifa_next)
			{
				if (!entry->ifa_addr || !(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
					continue;
				sockaddr head;
				std::memcpy(&head, entry->ifa_addr, offsetof(sockaddr, sa_family) + sizeof(head.sa_family));
				if (head.sa_family != AF_INET)
					continue;
				sockaddr_in address;
				std::memcpy(&address, entry->ifa_addr, sizeof(address));
				if (Usable(address.sin_addr))
					found = Dotted(address.sin_addr);
			}
			freeifaddrs(first);
			return found;
		}

		/// On the main thread only (the hook is the title's).
		std::string LookUpAddress()
		{
			return g_hooks.localAddress ? g_hooks.localAddress() : SystemAddress();
		}

		/// Under g_mutex.
		void UpdateStatusText()
		{
			if (!g_serving)
				return;
			const int port = g_port;
			g_status_text = g_address.empty() ? Format("Listening on port %d (this console's address is not known yet)", port) :
			                                    Format("Listening on %s:%d", g_address.c_str(), port);
		}

		// ---- text ------------------------------------------------------------------------------

		/// The code point at `at` of UTF-8 text, moving past it; -1 for a byte that does not begin
		/// a valid sequence (overlong forms, surrogates and values past U+10FFFF included), which
		/// moves past that byte alone.
		long NextCodePoint(const std::string& text, size_t* at)
		{
			const unsigned char lead = static_cast<unsigned char>(text[*at]);
			int extra;
			long value;
			long lowest;
			if (lead < 0x80)
			{
				(*at)++;
				return lead;
			}
			else if ((lead & 0xe0) == 0xc0)
				extra = 1, value = lead & 0x1f, lowest = 0x80;
			else if ((lead & 0xf0) == 0xe0)
				extra = 2, value = lead & 0x0f, lowest = 0x800;
			else if ((lead & 0xf8) == 0xf0)
				extra = 3, value = lead & 0x07, lowest = 0x10000;
			else
			{
				(*at)++;
				return -1;
			}
			if (*at + extra >= text.size())
			{
				(*at)++;
				return -1;
			}
			for (int i = 1; i <= extra; i++)
			{
				const unsigned char next = static_cast<unsigned char>(text[*at + i]);
				if ((next & 0xc0) != 0x80)
				{
					(*at)++;
					return -1;
				}
				value = (value << 6) | (next & 0x3f);
			}
			if (value < lowest || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
			{
				(*at)++;
				return -1;
			}
			*at += 1 + extra;
			return value;
		}

		bool ValidUtf8(const std::string& text)
		{
			size_t at = 0;
			while (at < text.size())
				if (NextCodePoint(text, &at) < 0)
					return false;
			return true;
		}

		/// A JSON string of the text, which need not be valid UTF-8: what is not becomes U+FFFD.
		std::string Json(const std::string& text)
		{
			std::string out = "\"";
			size_t at = 0;
			while (at < text.size())
			{
				const size_t start = at;
				const long c = NextCodePoint(text, &at);
				if (c < 0)
					out += "\xef\xbf\xbd";
				else if (c == '"' || c == '\\')
				{
					out += '\\';
					out += static_cast<char>(c);
				}
				else if (c < 0x20 || c == 0x7f || c == 0x2028 || c == 0x2029)
					out += Format("\\u%04lx", c);
				else
					out.append(text, start, at - start);
			}
			return out + "\"";
		}

		int HexDigit(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		}

		std::string UrlDecode(const std::string& text)
		{
			std::string out;
			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] == '%' && i + 2 < text.size() && HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0)
				{
					out += static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2]));
					i += 2;
				}
				else if (text[i] == '+')
					out += ' ';
				else
					out += text[i];
			}
			return out;
		}

		/// The first value of `name` in a query string, decoded; `found` says whether it was there.
		std::string QueryValue(const std::string& query, const char* name, bool* found = nullptr)
		{
			const size_t name_length = std::strlen(name);
			size_t start = 0;
			while (start <= query.size())
			{
				size_t end = query.find('&', start);
				if (end == std::string::npos)
					end = query.size();
				if (end - start > name_length && query.compare(start, name_length, name) == 0 && query[start + name_length] == '=')
				{
					if (found)
						*found = true;
					return UrlDecode(query.substr(start + name_length + 1, end - start - name_length - 1));
				}
				start = end + 1;
			}
			if (found)
				*found = false;
			return "";
		}

		std::string CookieValue(const std::string& cookies, const char* name)
		{
			const size_t name_length = std::strlen(name);
			size_t start = 0;
			while (start < cookies.size())
			{
				size_t end = cookies.find(';', start);
				if (end == std::string::npos)
					end = cookies.size();
				size_t begin = start;
				while (begin < end && (cookies[begin] == ' ' || cookies[begin] == '\t'))
					begin++;
				if (end - begin > name_length && cookies.compare(begin, name_length, name) == 0 && cookies[begin + name_length] == '=')
					return cookies.substr(begin + name_length + 1, end - begin - name_length - 1);
				start = end + 1;
			}
			return "";
		}

		/// A whole decimal number in [low, high]; false for anything else.
		bool ParseInt(const std::string& text, long low, long high, long* value)
		{
			if (text.empty() || text.size() > 9)
				return false;
			long result = 0;
			size_t at = 0;
			const bool negative = text[0] == '-';
			if (negative)
				at = 1;
			if (at == text.size())
				return false;
			for (; at < text.size(); at++)
			{
				if (text[at] < '0' || text[at] > '9')
					return false;
				result = result * 10 + (text[at] - '0');
			}
			if (negative)
				result = -result;
			if (result < low || result > high)
				return false;
			*value = result;
			return true;
		}

		std::string Lowercase(std::string text)
		{
			for (char& c : text)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return text;
		}

		bool SerialLike(const std::string& text)
		{
			if (text.empty() || text.size() > 64)
				return false;
			for (const char c : text)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'))
					return false;
			return true;
		}

		// ---- answers ---------------------------------------------------------------------------

		const char* Reason(int code)
		{
			switch (code)
			{
				case 200: return "OK";
				case 400: return "Bad Request";
				case 403: return "Forbidden";
				case 404: return "Not Found";
				case 405: return "Method Not Allowed";
				case 408: return "Request Timeout";
				case 409: return "Conflict";
				case 413: return "Payload Too Large";
				case 431: return "Request Header Fields Too Large";
				case 501: return "Not Implemented";
				case 503: return "Service Unavailable";
				default: return "Error";
			}
		}

		Response JsonAnswer(std::string json)
		{
			Response response;
			response.body = std::move(json);
			return response;
		}

		Response Error(int code, const char* why)
		{
			Response response;
			response.code = code;
			response.body = "{\"ok\":false,\"error\":" + Json(why) + "}";
			return response;
		}

		Response Done(bool ok, const char* failure)
		{
			return ok ? JsonAnswer("{\"ok\":true}") : Error(409, failure);
		}

		Response Forbidden()
		{
			Response response;
			response.code = 403;
			response.type = "text/plain; charset=utf-8";
			response.body = "Forbidden. Open the address the TV shows (its QR code carries the key).\n";
			return response;
		}

		std::string ImageType(const std::string& given, const std::vector<std::uint8_t>& bytes)
		{
			const std::string type = Lowercase(given);
			if (type == "image/png" || type == "image/jpeg" || type == "image/webp" || type == "image/gif")
				return type;
			if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
				return "image/png";
			if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
				return "image/jpeg";
			if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
				return "image/webp";
			return "application/octet-stream";
		}

		// ---- what the main thread is asked for (these run in tick()) ---------------------------

		Response GamesJob()
		{
			const std::vector<Game> games = g_hooks.listGames();
			std::string json = "[";
			for (size_t i = 0; i < games.size(); i++)
			{
				const Game& game = games[i];
				json += (i ? ",{\"title\":" : "{\"title\":") + Json(game.title) + ",\"serial\":" + Json(game.serial) +
					",\"region\":" + Json(game.region) + ",\"path\":" + Json(game.path) + ",\"cover\":" + (game.has_cover ? "true}" : "false}");
			}
			return JsonAnswer(json + "]");
		}

		std::string StatusJson(const Status& status)
		{
			const auto number = [](float value) { return std::isfinite(value) ? static_cast<double>(value) : 0.0; };
			return std::string("{\"running\":") + (status.running ? "true" : "false") + ",\"paused\":" + (status.paused ? "true" : "false") +
				",\"fast_forward\":" + (status.fast_forward ? "true" : "false") + ",\"text_field\":" + (status.text_field_open ? "true" : "false") +
				",\"title\":" + Json(status.title) + ",\"serial\":" + Json(status.serial) +
				Format(",\"speed\":%.0f,\"fps\":%.1f}", number(status.speed_percent), number(status.fps));
		}

		// ---- the HTTP side (the server thread) -------------------------------------------------

		struct Request
		{
			std::string method;
			std::string path; ///< the target before '?', as sent
			std::string query; ///< after '?', still encoded
			size_t content_length = 0;
			bool chunked = false;
			bool expect_continue = false;
			std::string cookies;
		};

		bool TokenChar(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
		}

		/// The request line and headers (without the blank line that ends them). Returns 0, or the
		/// status code to refuse them with.
		int ParseHead(const std::string& head, Request* request)
		{
			size_t line_end = head.find("\r\n");
			if (line_end == std::string::npos)
				line_end = head.size();
			const std::string line = head.substr(0, line_end);
			const size_t first = line.find(' ');
			const size_t second = first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
			if (first == std::string::npos || second == std::string::npos || line.find(' ', second + 1) != std::string::npos)
				return 400;
			request->method = line.substr(0, first);
			const std::string target = line.substr(first + 1, second - first - 1);
			const std::string version = line.substr(second + 1);
			if (request->method.empty() || request->method.size() > 16)
				return 400;
			for (const char c : request->method)
				if (c < 'A' || c > 'Z')
					return 400;
			if (version != "HTTP/1.1" && version != "HTTP/1.0")
				return 400;
			if (target.empty() || target[0] != '/' || target.size() > 8192)
				return 400;
			for (const char c : target)
				if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f)
					return 400;
			const size_t question = target.find('?');
			request->path = target.substr(0, question);
			request->query = question == std::string::npos ? "" : target.substr(question + 1);

			bool have_length = false;
			int count = 0;
			size_t at = line_end == head.size() ? head.size() : line_end + 2;
			while (at < head.size())
			{
				size_t end = head.find("\r\n", at);
				if (end == std::string::npos)
					end = head.size();
				const std::string header = head.substr(at, end - at);
				at = end + 2;
				if (++count > kMaxHeaders)
					return 431;
				if (header.empty() || header[0] == ' ' || header[0] == '\t')
					return 400; // folded lines are refused, as RFC 9112 allows
				const size_t colon = header.find(':');
				if (colon == std::string::npos || colon == 0)
					return 400;
				for (size_t i = 0; i < colon; i++)
					if (!TokenChar(header[i]))
						return 400;
				for (size_t i = colon + 1; i < header.size(); i++)
				{
					const unsigned char c = static_cast<unsigned char>(header[i]);
					if ((c < 0x20 && c != '\t') || c == 0x7f)
						return 400;
				}
				size_t value_start = colon + 1, value_end = header.size();
				while (value_start < value_end && (header[value_start] == ' ' || header[value_start] == '\t'))
					value_start++;
				while (value_end > value_start && (header[value_end - 1] == ' ' || header[value_end - 1] == '\t'))
					value_end--;
				const std::string name = Lowercase(header.substr(0, colon));
				const std::string value = header.substr(value_start, value_end - value_start);
				if (name == "content-length")
				{
					if (value.empty())
						return 400;
					size_t length = 0;
					for (const char c : value)
					{
						if (c < '0' || c > '9')
							return 400;
						// Anything past the limit is only "too large", which is said after the key.
						length = std::min<size_t>(length * 10 + static_cast<size_t>(c - '0'), kMaxBody + 1);
					}
					if (have_length && length != request->content_length)
						return 400;
					have_length = true;
					request->content_length = length;
				}
				else if (name == "transfer-encoding")
					request->chunked = true;
				else if (name == "expect")
					request->expect_continue = Lowercase(value) == "100-continue";
				else if (name == "cookie")
				{
					if (!request->cookies.empty())
						request->cookies += "; ";
					request->cookies += value;
				}
			}
			return 0;
		}

		struct Connection
		{
			enum class Stage
			{
				Reading,
				Waiting, ///< for a job of the main thread
				Writing,
				Lingering, ///< answered; what the client still sends is read and dropped
				Closed,
			};

			int fd = -1;
			Stage stage = Stage::Reading;
			Clock::time_point deadline;
			std::string in;
			size_t head_length = 0; ///< with its blank line; 0 while it has not all arrived
			Request request;
			bool set_cookie = false;
			std::shared_ptr<Job> job;
			std::string out;
			size_t sent = 0;
			size_t dropped = 0;
		};

		void Wake()
		{
			if (g_wake[1] >= 0)
			{
				const char byte = 1;
				[[maybe_unused]] const ssize_t ignored = write(g_wake[1], &byte, 1);
			}
		}

		void SetNonBlocking(int fd)
		{
			const int flags = fcntl(fd, F_GETFL, 0);
			if (flags >= 0)
				fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}

		void CloseConnection(Connection& connection)
		{
			if (connection.fd >= 0)
				close(connection.fd);
			connection.fd = -1;
			connection.stage = Connection::Stage::Closed;
		}

		void Linger(Connection& connection)
		{
			::shutdown(connection.fd, SHUT_WR);
			connection.stage = Connection::Stage::Lingering;
			connection.deadline = Clock::now() + kLingerTime;
		}

		void SendSome(Connection& connection)
		{
			while (connection.sent < connection.out.size())
			{
				const ssize_t sent = send(connection.fd, connection.out.data() + connection.sent, connection.out.size() - connection.sent, kNoSignal);
				if (sent < 0 && errno == EINTR)
					continue;
				if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					return;
				if (sent <= 0)
				{
					CloseConnection(connection);
					return;
				}
				connection.sent += static_cast<size_t>(sent);
			}
			std::string().swap(connection.out);
			Linger(connection);
		}

		void Answer(Connection& connection, const Response& response)
		{
			std::string head = Format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n", response.code,
				Reason(response.code), response.type.c_str(), response.body.size());
			head += response.cacheable ? "Cache-Control: private, max-age=86400\r\n" : "Cache-Control: no-store\r\n";
			head += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n";
			if (response.code == 200 && response.type.compare(0, 9, "text/html") == 0)
				head += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; img-src 'self'; "
				        "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
			if (connection.set_cookie)
			{
				std::string key;
				{
					std::lock_guard lock(g_mutex);
					key = g_key;
				}
				head += Format("Set-Cookie: %s=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict\r\n", kCookie, key.c_str());
			}
			head += response.headers;
			head += "\r\n";
			connection.out = std::move(head);
			connection.out += response.body;
			connection.sent = 0;
			connection.job.reset();
			connection.stage = Connection::Stage::Writing;
			connection.deadline = Clock::now() + kWriteTime;
			SendSome(connection);
		}

		/// Hands work to the main thread; the answer is sent when tick() has done it.
		void Queue(Connection& connection, std::function<Response()> work)
		{
			auto job = std::make_shared<Job>();
			job->work = std::move(work);
			{
				std::lock_guard lock(g_mutex);
				g_jobs.push_back(job);
			}
			connection.job = std::move(job);
			connection.stage = Connection::Stage::Waiting;
			connection.deadline = Clock::now() + std::chrono::milliseconds(std::max(1, g_options.main_thread_timeout_ms));
		}

		void Route(Connection& connection, const std::string& body)
		{
			const Request& request = connection.request;
			const std::string& path = request.path;
			const bool get = request.method == "GET";
			const bool post = request.method == "POST";
			const auto wrong_method = [&]() {
				Response response = Error(405, "That is not asked for that way.");
				response.headers = get ? "Allow: POST\r\n" : "Allow: GET\r\n";
				return response;
			};

			if (path == "/" || path == "/index.html")
			{
				if (!get)
					return Answer(connection, wrong_method());
				Response response;
				response.type = "text/html; charset=utf-8";
				response.body = kPage;
				return Answer(connection, response);
			}
			if (path == "/api/status")
			{
				if (!get)
					return Answer(connection, wrong_method());
				std::string json;
				{
					std::lock_guard lock(g_mutex);
					json = g_status_json;
				}
				return Answer(connection, JsonAnswer(std::move(json)));
			}
			if (path == "/api/games")
			{
				if (!get)
					return Answer(connection, wrong_method());
				if (!g_hooks.listGames)
					return Answer(connection, Error(501, "The library is not available."));
				return Queue(connection, GamesJob);
			}
			if (path == "/api/start")
			{
				if (!post)
					return Answer(connection, wrong_method());
				bool in_query = false;
				std::string game = QueryValue(request.query, "path", &in_query);
				if (!in_query)
					game = body;
				if (game.empty() || game.find('\0') != std::string::npos)
					return Answer(connection, Error(400, "Which game?"));
				if (!g_hooks.startGame)
					return Answer(connection, Error(501, "Games cannot be started from here."));
				return Queue(connection, [game]() { return Done(g_hooks.startGame(game), "The game could not be started."); });
			}
			if (path == "/api/action")
			{
				if (!post)
					return Answer(connection, wrong_method());
				const std::string name = QueryValue(request.query, "name");
				const std::string arg_text = QueryValue(request.query, "arg");
				long arg = 0;
				if (name == "save_state" || name == "load_state")
				{
					if (!ParseInt(arg_text, 1, 10, &arg))
						return Answer(connection, Error(400, "The slot is 1 to 10."));
				}
				else if (name == "fast_forward")
				{
					if (!ParseInt(arg_text, 0, 1, &arg))
						return Answer(connection, Error(400, "Fast-forward is 1 (on) or 0 (off)."));
				}
				else if (name != "pause" && name != "resume" && name != "quick_menu" && name != "leave")
					return Answer(connection, Error(400, "No such action."));
				if (!g_hooks.action)
					return Answer(connection, Error(501, "That is not available."));
				const int value = static_cast<int>(arg);
				return Queue(connection, [name, value]() { return Done(g_hooks.action(name, value), "The title could not do that now."); });
			}
			if (path == "/api/text")
			{
				if (!post)
					return Answer(connection, wrong_method());
				if (body.size() > kMaxTypedText)
					return Answer(connection, Error(413, "That is too much text at once."));
				if (body.empty() || !ValidUtf8(body))
					return Answer(connection, Error(400, "The text is empty or not UTF-8."));
				for (const char c : body)
					if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
						return Answer(connection, Error(400, "Control characters are not typed; Enter and Backspace have buttons."));
				if (!g_hooks.typeText)
					return Answer(connection, Error(501, "Typing is not available."));
				return Queue(connection, [body]() { return Done(g_hooks.typeText(body), "No text field is open."); });
			}
			if (path == "/api/key")
			{
				if (!post)
					return Answer(connection, wrong_method());
				const std::string name = QueryValue(request.query, "name");
				Key key;
				if (name == "enter")
					key = Key::Enter;
				else if (name == "backspace")
					key = Key::Backspace;
				else
					return Answer(connection, Error(400, "The keys are enter and backspace."));
				if (!g_hooks.pressKey)
					return Answer(connection, Error(501, "Typing is not available."));
				return Queue(connection, [key]() { return Done(g_hooks.pressKey(key), "No text field is open."); });
			}
			if (path == "/api/log")
			{
				if (!get)
					return Answer(connection, wrong_method());
				const std::string which = QueryValue(request.query, "which");
				if (which != "boot" && which != "emu")
					return Answer(connection, Error(400, "The logs are boot and emu."));
				if (!g_hooks.readLog)
					return Answer(connection, Error(501, "The logs are not available."));
				const bool boot = which == "boot";
				return Queue(connection, [boot]() {
					Response response;
					if (!g_hooks.readLog(boot ? Log::Boot : Log::Emulator, &response.body))
						return Error(404, "There is no such log yet.");
					response.type = "text/plain; charset=utf-8";
					response.headers = Format("Content-Disposition: attachment; filename=\"%s\"\r\n", boot ? "crux2-boot.log" : "emulog.txt");
					return response;
				});
			}
			if (path == "/api/cover")
			{
				if (!get)
					return Answer(connection, wrong_method());
				const std::string serial = QueryValue(request.query, "serial");
				if (!SerialLike(serial))
					return Answer(connection, Error(400, "Which serial?"));
				if (!g_hooks.cover)
					return Answer(connection, Error(404, "No covers."));
				return Queue(connection, [serial]() {
					std::vector<std::uint8_t> bytes;
					std::string mime;
					if (!g_hooks.cover(serial, &bytes, &mime) || bytes.empty())
						return Error(404, "No cover.");
					Response response;
					response.type = ImageType(mime, bytes);
					response.body.assign(bytes.begin(), bytes.end());
					response.cacheable = true;
					return response;
				});
			}
			Answer(connection, Error(404, "Nothing is here."));
		}

		/// Looks at what has arrived: refuses it, waits for more, or acts on it.
		void Consider(Connection& connection)
		{
			if (connection.head_length == 0)
			{
				const size_t end = connection.in.find("\r\n\r\n");
				if (end == std::string::npos)
				{
					if (connection.in.size() > kMaxHead)
						Answer(connection, Error(431, "The request's headers are too long."));
					return;
				}
				if (end + 4 > kMaxHead)
					return Answer(connection, Error(431, "The request's headers are too long."));
				const int refusal = ParseHead(connection.in.substr(0, end), &connection.request);
				if (refusal != 0)
					return Answer(connection, Error(refusal, "The request was not understood."));
				connection.head_length = end + 4;

				// The key, before anything else is looked at.
				std::string key;
				{
					std::lock_guard lock(g_mutex);
					key = g_key;
				}
				const bool by_address = SameKey(QueryValue(connection.request.query, "key"), key);
				const bool by_cookie = SameKey(CookieValue(connection.request.cookies, kCookie), key);
				if (!by_address && !by_cookie)
					return Answer(connection, Forbidden());
				connection.set_cookie = by_address;

				if (connection.request.chunked)
					return Answer(connection, Error(501, "Chunked bodies are not taken; send a Content-Length."));
				if (connection.request.content_length > kMaxBody)
					return Answer(connection, Error(413, "The body is too large."));
				if (connection.request.expect_continue && connection.in.size() - connection.head_length < connection.request.content_length)
				{
					static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
					[[maybe_unused]] const ssize_t ignored = send(connection.fd, kContinue, sizeof(kContinue) - 1, kNoSignal);
				}
			}
			if (connection.in.size() - connection.head_length < connection.request.content_length)
				return;
			const std::string body = connection.in.substr(connection.head_length, connection.request.content_length);
			std::string().swap(connection.in);
			Route(connection, body);
		}

		void ReadSome(Connection& connection)
		{
			char block[8192];
			for (;;)
			{
				const ssize_t got = recv(connection.fd, block, sizeof(block), 0);
				if (got < 0 && errno == EINTR)
					continue;
				if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					return;
				if (got <= 0)
				{
					// The client is gone (or stopped sending): there is nobody to answer.
					CloseConnection(connection);
					return;
				}
				if (connection.stage == Connection::Stage::Lingering)
				{
					connection.dropped += static_cast<size_t>(got);
					if (connection.dropped > kLingerBytes)
					{
						CloseConnection(connection);
						return;
					}
					continue;
				}
				connection.in.append(block, static_cast<size_t>(got));
				Consider(connection);
				if (connection.stage != Connection::Stage::Reading)
					return;
			}
		}

		void CheckJob(Connection& connection, Clock::time_point now)
		{
			Response response;
			{
				std::lock_guard lock(g_mutex);
				Job& job = *connection.job;
				if (job.state == Job::State::Done)
					response = std::move(job.result);
				else if (job.state == Job::State::Queued && now >= connection.deadline)
				{
					// Not started in time: it is taken back, so it never runs late.
					g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), connection.job), g_jobs.end());
					response = Error(503, "The title did not answer in time; try again.");
				}
				else
					return;
			}
			Answer(connection, response);
		}

		void Serve(int listener, int wake)
		{
			PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
			std::vector<std::unique_ptr<Connection>> connections;
			std::vector<pollfd> waits;
			while (!g_stopping)
			{
				waits.clear();
				if (wake >= 0)
					waits.push_back({wake, POLLIN, 0});
				const bool taking = connections.size() < kMaxConnections;
				const size_t listener_index = waits.size();
				if (taking)
					waits.push_back({listener, POLLIN, 0});
				const size_t first_connection = waits.size();
				for (const auto& connection : connections)
				{
					short events = 0;
					if (connection->stage == Connection::Stage::Reading || connection->stage == Connection::Stage::Lingering)
						events = POLLIN;
					else if (connection->stage == Connection::Stage::Writing)
						events = POLLOUT;
					waits.push_back({connection->fd, events, 0});
				}
				bool any_waiting = false;
				for (const auto& connection : connections)
					any_waiting = any_waiting || connection->stage == Connection::Stage::Waiting;
				const int timeout = wake < 0 ? 20 : any_waiting ? 50 : 250;
				const int ready = poll(waits.data(), static_cast<nfds_t>(waits.size()), timeout);
				if (g_stopping)
					break;
				if (ready < 0 && errno != EINTR)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));

				if (wake >= 0 && ready > 0 && (waits[0].revents & POLLIN))
				{
					char drain[64];
					while (read(wake, drain, sizeof(drain)) > 0)
					{
					}
				}

				const Clock::time_point now = Clock::now();
				if (ready > 0)
					for (size_t i = 0; i < connections.size(); i++)
					{
						Connection& connection = *connections[i];
						const short revents = waits[first_connection + i].revents;
						if (revents == 0)
							continue;
						if (connection.stage == Connection::Stage::Writing)
						{
							if (revents & (POLLOUT | POLLERR | POLLHUP))
								SendSome(connection);
						}
						else if (connection.stage == Connection::Stage::Reading || connection.stage == Connection::Stage::Lingering)
						{
							if (revents & (POLLIN | POLLERR | POLLHUP))
								ReadSome(connection);
						}
					}

				for (auto& pointer : connections)
				{
					Connection& connection = *pointer;
					if (connection.stage == Connection::Stage::Waiting)
						CheckJob(connection, now);
					else if (connection.stage != Connection::Stage::Closed && now >= connection.deadline)
					{
						if (connection.stage == Connection::Stage::Reading && !connection.in.empty())
							Answer(connection, Error(408, "The request did not arrive in time."));
						else
							CloseConnection(connection);
					}
				}
				connections.erase(std::remove_if(connections.begin(), connections.end(),
				                      [](const std::unique_ptr<Connection>& connection) { return connection->stage == Connection::Stage::Closed; }),
					connections.end());

				if (taking && ready > 0 && (waits[listener_index].revents & POLLIN))
					while (connections.size() < kMaxConnections)
					{
						sockaddr_storage from;
						socklen_t length = sizeof(from);
						const int fd = accept(listener, reinterpret_cast<sockaddr*>(&from), &length);
						if (fd < 0)
							break;
						SetNonBlocking(fd);
#if defined(SO_NOSIGPIPE)
						const int one = 1;
						setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
						auto connection = std::make_unique<Connection>();
						connection->fd = fd;
						connection->deadline = Clock::now() + kReadTime;
						connections.push_back(std::move(connection));
					}
			}
			for (auto& connection : connections)
				CloseConnection(*connection);
		}

		// ---- starting and stopping (the main thread) -------------------------------------------

		bool ParseAddress(const std::string& text, in_addr* address)
		{
			unsigned parts[4];
			char tail;
			if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &tail) != 4)
				return false;
			for (const unsigned part : parts)
				if (part > 255)
					return false;
			address->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
			return true;
		}

		void Start()
		{
			const int wanted_port = g_options.port;
			in_addr bind_to;
			if (!ParseAddress(g_options.bind_address, &bind_to) || wanted_port < 0 || wanted_port > 65535)
			{
				std::lock_guard lock(g_mutex);
				g_status_text = "The web panel's address or port is not valid";
				Note("web: the address or port to listen on is not valid");
				return;
			}
			const int fd = socket(AF_INET, SOCK_STREAM, 0);
			sockaddr_in local = {};
#if defined(__FreeBSD__) || defined(__PROSPERO__)
			local.sin_len = sizeof(local);
#endif
			local.sin_family = AF_INET;
			local.sin_port = htons(static_cast<std::uint16_t>(wanted_port));
			local.sin_addr = bind_to;
			if (fd >= 0)
			{
				const int one = 1;
				setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
			}
			if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 || listen(fd, 16) != 0)
			{
				const int error = errno;
				if (fd >= 0)
					close(fd);
				std::lock_guard lock(g_mutex);
				g_status_text = Format("Port %d could not be opened (error %d)", wanted_port, error);
				Note("web: port %d could not be opened (errno %d: %s)", wanted_port, error, std::strerror(error));
				return;
			}
			SetNonBlocking(fd);
			sockaddr_in bound = {};
			socklen_t length = sizeof(bound);
			int port = wanted_port;
			if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) == 0)
				port = ntohs(bound.sin_port);

			if (pipe(g_wake) == 0)
			{
				SetNonBlocking(g_wake[0]);
				SetNonBlocking(g_wake[1]);
			}
			else
			{
				g_wake[0] = g_wake[1] = -1;
				Note("web: no pipe (errno %d); the server looks for work every 20 ms instead", errno);
			}

			g_listener = fd;
			g_port = port;
			g_stopping = false;
			g_serving = true;
			const std::string address = LookUpAddress();
			g_address_at = g_status_at = Clock::now();
			{
				std::lock_guard lock(g_mutex);
				g_address = address;
				g_status_json = "{\"running\":false}";
				UpdateStatusText();
			}
			Note("web: listening on port %d%s", port, address.empty() ? " (this console's address is not known yet)" : "");
			g_thread = std::thread(Serve, fd, g_wake[0]);
		}

		void Stop()
		{
			if (!g_serving)
				return;
			g_stopping = true;
			Wake();
			if (g_thread.joinable())
				g_thread.join();
			close(g_listener);
			g_listener = -1;
			for (int& end : g_wake)
			{
				if (end >= 0)
					close(end);
				end = -1;
			}
			g_serving = false;
			g_port = 0;
			{
				std::lock_guard lock(g_mutex);
				g_jobs.clear();
				g_status_text = "Off";
			}
			Note("web: stopped");
		}
	} // namespace

	void init(const Hooks& hooks, LogFunction log, const Options& options)
	{
		Stop();
		g_hooks = hooks;
		g_log = std::move(log);
		g_options = options;
		std::string key = Lowercase(options.key);
		const bool kept = IsKey(key);
		if (!kept && !MakeKey(&key))
			Note("web: no source of random numbers answered; the key was made from the clock");
		{
			std::lock_guard lock(g_mutex);
			g_key = key;
			g_address.clear();
			g_status_text = "Off";
		}
		if (!kept && g_hooks.keyChanged)
			g_hooks.keyChanged(key);
	}

	bool apply(bool enabled)
	{
		if (enabled && !g_serving)
		{
			bool have_key;
			{
				std::lock_guard lock(g_mutex);
				have_key = !g_key.empty();
			}
			if (!have_key)
			{
				Note("web: not started: init() was not called");
				return false;
			}
			Start();
		}
		else if (!enabled && g_serving)
			Stop();
		return g_serving;
	}

	void tick()
	{
		if (!g_serving)
			return;
		std::deque<std::shared_ptr<Job>> work;
		{
			std::lock_guard lock(g_mutex);
			work.swap(g_jobs);
			for (const auto& job : work)
				job->state = Job::State::Running;
		}
		for (const auto& job : work)
		{
			Response result = job->work();
			std::lock_guard lock(g_mutex);
			job->result = std::move(result);
			job->state = Job::State::Done;
		}
		if (!work.empty())
			Wake();

		const Clock::time_point now = Clock::now();
		if (g_hooks.status && now - g_status_at >= kStatusEvery)
		{
			g_status_at = now;
			std::string json = StatusJson(g_hooks.status());
			std::lock_guard lock(g_mutex);
			g_status_json = std::move(json);
		}
		if (now - g_address_at >= kAddressEvery)
		{
			g_address_at = now;
			const std::string address = LookUpAddress();
			std::lock_guard lock(g_mutex);
			if (address != g_address)
			{
				g_address = address;
				UpdateStatusText();
			}
		}
	}

	void shutdown()
	{
		Stop();
	}

	bool listening()
	{
		return g_serving;
	}

	int port()
	{
		return g_port;
	}

	std::string status()
	{
		std::lock_guard lock(g_mutex);
		return g_status_text;
	}

	std::string url()
	{
		std::lock_guard lock(g_mutex);
		if (!g_serving || g_address.empty() || g_key.empty())
			return "";
		return Format("http://%s:%d/?key=%s", g_address.c_str(), static_cast<int>(g_port), g_key.c_str());
	}

	std::string key()
	{
		std::lock_guard lock(g_mutex);
		return g_key;
	}

	void newKey()
	{
		std::string key;
		if (!MakeKey(&key))
			Note("web: no source of random numbers answered; the key was made from the clock");
		{
			std::lock_guard lock(g_mutex);
			g_key = key;
		}
		Note("web: a new key was made; the old one opens nothing any more");
		if (g_hooks.keyChanged)
			g_hooks.keyChanged(key);
	}

	QrCode qr(const std::string& text)
	{
		QrCode code;
		std::vector<std::uint8_t> temporary(qrcodegen_BUFFER_LEN_MAX), symbol(qrcodegen_BUFFER_LEN_MAX);
		if (text.find('\0') != std::string::npos ||
			!qrcodegen_encodeText(text.c_str(), temporary.data(), symbol.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
				qrcodegen_Mask_AUTO, true))
			return code;
		code.size = qrcodegen_getSize(symbol.data());
		code.modules.resize(static_cast<size_t>(code.size) * code.size);
		for (int y = 0; y < code.size; y++)
			for (int x = 0; x < code.size; x++)
				code.modules[static_cast<size_t>(y) * code.size + x] = qrcodegen_getModule(symbol.data(), x, y) ? 1 : 0;
		return code;
	}

	namespace
	{
		// The page. Dark, for a phone first, nothing from elsewhere. The key it was opened with is
		// taken out of the address bar at once (it stays in a cookie and in this page's memory) so
		// that it is not shown over a shoulder or kept in the history.
		const char* const kPage = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>Crux2</title>
<style>
:root{--bg:#0d1017;--panel:#171c27;--high:#252d3f;--line:#ffffff14;--text:#eef1f7;--dim:#9aa4b8;--accent:#5aa2ff;--on:#3ddc97;--bad:#ff6b6b}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{position:sticky;top:0;z-index:2;background:var(--panel);padding:12px 16px 0;border-bottom:1px solid var(--line)}
.top{display:flex;align-items:baseline;justify-content:space-between;gap:8px}h1{font-size:19px;margin:0;letter-spacing:.02em}
#state{color:var(--dim);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
nav{display:flex;gap:4px;margin-top:10px;overflow-x:auto}nav button{flex:1;border-radius:10px 10px 0 0;background:transparent;color:var(--dim)}
nav button.on{background:var(--bg);color:var(--text)}
main{padding:12px 16px 48px;max-width:760px;margin:0 auto}
button,select,input,textarea{font:inherit;color:var(--text);background:var(--high);border:0;border-radius:10px;padding:10px 14px;min-height:44px}
button{cursor:pointer}button:active{filter:brightness(1.25)}button.primary{background:var(--accent);color:#06101f}button.lit{background:var(--on);color:#04170e}
button.warn{background:#5b2630}button:disabled{opacity:.45}
.card{background:var(--panel);border-radius:14px;padding:14px;margin:0 0 12px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}
.row{display:flex;align-items:center;gap:12px;padding:8px 0;border-top:1px solid var(--line)}.row:first-child{border-top:0}
.grow{flex:1;min-width:0}.name{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.dim{color:var(--dim);font-size:14px}
.big{font-size:20px;font-weight:600}.stats{display:flex;gap:18px;margin-top:6px}.stats b{font-size:22px}
.cover{width:44px;height:62px;border-radius:6px;object-fit:cover;background:var(--high);flex:none}
input[type=search],#typed{width:100%}.tools{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.badge{display:inline-block;padding:2px 8px;border-radius:99px;background:var(--high);font-size:13px;color:var(--dim);margin-left:6px}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#000c;padding:10px 16px;border-radius:12px;opacity:0;transition:opacity .2s;pointer-events:none;max-width:90vw}
#toast.show{opacity:1}a{color:var(--accent)}label{display:flex;align-items:center;gap:8px}
</style></head><body>
<header><div class="top"><h1>Crux2</h1><span id="state">Connecting...</span></div>
<nav><button data-tab="play" class="on">Playing</button><button data-tab="library">Library</button><button data-tab="type">Type</button><button data-tab="logs">Logs</button></nav></header>
<main>
<section id="play"></section>
<section id="library" hidden><div class="card"><input id="filter" type="search" placeholder="Search by title or serial" autocomplete="off"></div><div id="games" class="card"><span class="dim">Loading...</span></div></section>
<section id="type" hidden><div class="card"><p class="dim" style="margin-top:0">What is sent here is typed into the text field the title has open (a RetroAchievements password, a cheat line). <span id="field"></span></p>
<input id="typed" type="text" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="Text to type">
<div class="tools" style="margin-top:10px"><button class="primary" id="send">Send text</button><button id="enter">Enter</button><button id="back">Backspace</button>
<label class="dim"><input type="checkbox" id="live" style="min-height:0"> Send as I type</label></div></div></section>
<section id="logs" hidden><div class="card"><div class="row"><div class="grow"><b>crux2-boot.log</b><div class="dim">What the title did since it started.</div></div><a id="bootlog" download="crux2-boot.log"><button>Download</button></a></div>
<div class="row"><div class="grow"><b>emulog.txt</b><div class="dim">The emulator's own log (logs/emulog.txt).</div></div><a id="emulog" download="emulog.txt"><button>Download</button></a></div></div>
<p class="dim">The logs carry no network addresses by design. Still, read them before posting them publicly, and never post the panel's address: it carries the key.</p></section>
</main><div id="toast"></div>
<script>
'use strict';
const K=new URLSearchParams(location.search).get('key')||'';
if(K)history.replaceState(null,'','/');
const withKey=p=>K?p+(p.includes('?')?'&':'?')+'key='+encodeURIComponent(K):p;
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let toastTimer;function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>e.classList.remove('show'),2200);}
async function api(path,opt){const r=await fetch(withKey(path),Object.assign({credentials:'same-origin',cache:'no-store'},opt||{}));
let j={};try{j=await r.json();}catch(e){}if(!r.ok)throw new Error(r.status===403?'The key is old: scan the QR code on the TV again.':(j.error||('Error '+r.status)));return j;}
const post=(path,body)=>api(path,{method:'POST',body:body===undefined?'':body,headers:{'Content-Type':'text/plain;charset=utf-8'}});
const act=(name,arg,ok)=>post('/api/action?name='+name+(arg!==undefined?'&arg='+arg:'')).then(()=>ok&&toast(ok)).catch(e=>toast(e.message));
let tab='play',st={running:false},games=null,slot=1;
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{tab=b.dataset.tab;
document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x===b));
document.querySelectorAll('main section').forEach(s=>s.hidden=s.id!==tab);if(tab==='library'&&!games)loadGames();if(tab==='play')drawPlay();});
function drawPlay(){const e=$('play');
if(!st.running){e.innerHTML='<div class="card"><div class="big">Nothing is running</div><p class="dim">Start a game from the Library.</p></div>';return;}
let opts='';for(let i=1;i<=10;i++)opts+=`<option value="${i}"${i===slot?' selected':''}>Slot ${i}</option>`;
e.innerHTML=`<div class="card"><div class="big">${esc(st.title||'A game')}${st.paused?'<span class="badge">Paused</span>':''}</div>
<div class="dim">${esc(st.serial)}</div><div class="stats"><div><b id="speed">${st.speed}</b> <span class="dim">% speed</span></div><div><b id="fps">${st.fps}</b> <span class="dim">fps</span></div></div></div>
<div class="card grid"><button class="primary" id="pp">${st.paused?'Resume':'Pause'}</button><button id="ff" class="${st.fast_forward?'lit':''}">Fast-forward ${st.fast_forward?'on':'off'}</button>
<button id="menu">Quick menu</button></div>
<div class="card"><div class="tools"><select id="slot">${opts}</select><button id="save">Save state</button><button id="load">Load state</button></div></div>
<div class="card"><button class="warn" id="leave" style="width:100%">Leave the game</button></div>`;
$('pp').onclick=()=>act(st.paused?'resume':'pause');$('ff').onclick=()=>act('fast_forward',st.fast_forward?0:1);
$('menu').onclick=()=>act('quick_menu');
$('slot').onchange=ev=>slot=+ev.target.value;
$('save').onclick=()=>act('save_state',slot,'Saved to slot '+slot);
$('load').onclick=()=>{if(confirm('Load the state in slot '+slot+'? What is not saved is lost.'))act('load_state',slot,'Loaded slot '+slot);};
$('leave').onclick=()=>{if(confirm('Leave '+(st.title||'the game')+'? What is not saved is lost.'))act('leave',undefined,'Left the game');};}
function shape(s){return [s.running,s.paused,s.fast_forward,s.title,s.serial].join('\u0001');}
async function poll(){try{const s=await api('/api/status');const redraw=shape(s)!==shape(st);st=s;
$('state').textContent=s.running?(s.paused?'Paused · ':'Playing · ')+(s.title||s.serial):'In the library';
$('field').textContent=s.text_field?'A text field is open now.':'No text field is open on the TV right now.';
if(tab==='play'){if(redraw)drawPlay();else if(s.running){$('speed').textContent=s.speed;$('fps').textContent=s.fps;}}}
catch(e){$('state').textContent=e.message;}}
async function loadGames(){try{games=await api('/api/games');drawGames();}catch(e){$('games').innerHTML='<span class="dim">'+esc(e.message)+'</span>';games=null;}}
function drawGames(){const f=$('filter').value.trim().toLowerCase();
const list=games.filter(g=>!f||g.title.toLowerCase().includes(f)||g.serial.toLowerCase().includes(f));
$('games').innerHTML=list.length?list.slice(0,400).map(g=>`<div class="row" data-i="${games.indexOf(g)}">${g.cover?`<img class="cover" loading="lazy" alt="" src="${esc(withKey('/api/cover?serial='+encodeURIComponent(g.serial)))}">`:'<div class="cover"></div>'}
<div class="grow"><div class="name">${esc(g.title)}</div><div class="dim">${esc(g.serial)}${g.region?' · '+esc(g.region):''}</div></div><button>Start</button></div>`).join('')
+(list.length>400?'<p class="dim">'+(list.length-400)+' more: search to narrow it down.</p>':''):'<span class="dim">'+(games.length?'No game matches.':'The library is empty.')+'</span>';
document.querySelectorAll('#games .row button').forEach(b=>b.onclick=()=>start(games[+b.parentNode.dataset.i]));}
function start(g){if(st.running&&!confirm('Leave '+(st.title||'the running game')+' and start '+g.title+'? What is not saved is lost.'))return;
post('/api/start',g.path).then(()=>{toast('Starting '+g.title);document.querySelector('nav [data-tab=play]').click();}).catch(e=>toast(e.message));}
$('filter').oninput=()=>games&&drawGames();
let chain=Promise.resolve();const queue=f=>chain=chain.then(f).catch(e=>toast(e.message));
const typeText=t=>t&&queue(()=>post('/api/text',t));const key=k=>queue(()=>post('/api/key?name='+k));
$('send').onclick=()=>{typeText($('typed').value);$('typed').value='';$('typed').focus();};
$('enter').onclick=()=>key('enter');$('back').onclick=()=>key('backspace');
$('typed').addEventListener('beforeinput',ev=>{if(!$('live').checked)return;
if(ev.inputType==='deleteContentBackward'){key('backspace');return;}
if(ev.inputType==='insertLineBreak'){ev.preventDefault();key('enter');return;}
if((ev.inputType==='insertText'||ev.inputType==='insertFromPaste')&&ev.data){typeText(ev.data);}});
$('typed').addEventListener('keydown',ev=>{if(ev.key==='Enter'){ev.preventDefault();if($('live').checked)key('enter');else $('send').click();}});
$('bootlog').href=withKey('/api/log?which=boot');$('emulog').href=withKey('/api/log?which=emu');
poll();setInterval(poll,1500);
</script></body></html>
)PAGE";
	} // namespace
} // namespace PS5Web
