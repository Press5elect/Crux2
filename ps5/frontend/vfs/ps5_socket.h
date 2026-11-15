// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
/*
 * Crux2 - sockets that behave on the console: non-blocking when asked to be, no SIGPIPE, and a
 * TCP check of a port with a time limit, each step of it written down.
 *
 * On the console, O_NONBLOCK set with fcntl does not take on a socket: it stays blocking, and a
 * read that waits for more than the other end sent waits for ever. FIONBIO and the console's own
 * socket option SO_NBIO (0x1200) do take. Porpoise found this (its e6e0b64); MakeNonBlocking does
 * all three, so it is right on the console and on a PC alike.
 *
 * Header only: the PC tests build single files of the frontend (ps5_net.cpp, ps5_web.cpp) on
 * their own, and each of them uses this.
 */
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace PS5Socket
{
// The console's socket option for a non-blocking socket (FreeBSD's SOL_SOCKET space).
constexpr int SO_NBIO_PS5 = 0x1200;

// What MakeNonBlocking did: each call's result (0 or -1), for the log.
struct NonBlocking
{
	int fcntl_result = -1;
	int fionbio_result = -1;
	int nbio_result = -1; // -1 also off the console, where it is not asked
	bool ok() const { return fcntl_result == 0 || fionbio_result == 0 || nbio_result == 0; }
};

// From Porpoise e6e0b64 (GPL-3.0-or-later): fcntl, then FIONBIO, then (on the console) SO_NBIO.
inline NonBlocking MakeNonBlocking(int fd)
{
	NonBlocking done;
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		done.fcntl_result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int one = 1;
	done.fionbio_result = ioctl(fd, FIONBIO, &one);
#if defined(__PROSPERO__)
	done.nbio_result = setsockopt(fd, SOL_SOCKET, SO_NBIO_PS5, &one, sizeof(one));
#endif
	return done;
}

// The same undone: a socket that blocks again (with its time limits, if it has any).
inline void MakeBlocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	int zero = 0;
	(void)ioctl(fd, FIONBIO, &zero);
#if defined(__PROSPERO__)
	(void)setsockopt(fd, SOL_SOCKET, SO_NBIO_PS5, &zero, sizeof(zero));
#endif
}

// A write to a connection the other end closed fails with EPIPE rather than raising SIGPIPE
// (which ends the title) where the system has the option.
inline void NoSigPipe(int fd)
{
#if defined(SO_NOSIGPIPE)
	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)fd;
#endif
}

// A read or a write that blocks after all gives up after this long.
inline void SetTimeouts(int fd, int seconds)
{
	struct timeval limit;
	limit.tv_sec = seconds;
	limit.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

// SIGPIPE ignored for the whole title (a library's socket without SO_NOSIGPIPE). Once is enough;
// a second call does nothing.
inline void IgnoreSigPipe()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	(void)signal(SIGPIPE, SIG_IGN);
}

inline long long MillisecondsNow()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What a TCP check found.
struct Check
{
	bool open = false;
	int error = 0;        // the errno when not open: ETIMEDOUT (no answer), ECONNREFUSED, EHOSTUNREACH...
	bool looked_up = true; // false: the name could not be turned into an address
	long long ms = 0;     // how long it took
	std::string address;  // the address asked ("192.168.1.10")
	std::string steps;    // each step, "; " between them, for the log
};

// From Porpoise 6ee2ad2 (GPL-3.0-or-later): a plain TCP connection to host:port, with its own
// time limit, each step written into steps. Says what the network does (no answer, refused, no
// route) before a library tries, and fails within the limit when it cannot connect.
inline Check CheckPort(const std::string& host, int port, int limit_ms)
{
	Check check;
	const long long started = MillisecondsNow();
	const auto step = [&](const std::string& text) {
		if (!check.steps.empty())
			check.steps += "; ";
		check.steps += text;
	};
	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* found = nullptr;
	const std::string service = std::to_string(port);
	const int looked = getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
	if (looked != 0 || !found)
	{
		check.looked_up = false;
		check.error = EHOSTUNREACH;
		step("no address for " + host + " (lookup error " + std::to_string(looked) + ")");
		if (found)
			freeaddrinfo(found);
		check.ms = MillisecondsNow() - started;
		return check;
	}
	char text[INET_ADDRSTRLEN] = "?";
	if (found->ai_family == AF_INET)
		inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text));
	check.address = text;

	const int fd = socket(found->ai_family, SOCK_STREAM, 0);
	if (fd < 0)
	{
		check.error = errno ? errno : EIO;
		step(std::string("no socket: ") + std::strerror(check.error));
		freeaddrinfo(found);
		check.ms = MillisecondsNow() - started;
		return check;
	}
	NoSigPipe(fd);
	const NonBlocking nb = MakeNonBlocking(fd);
	const int rc = connect(fd, found->ai_addr, found->ai_addrlen);
	const int connect_errno = rc == 0 ? 0 : errno;
	freeaddrinfo(found);
	step(std::string(text) + ":" + service + ": non-blocking fcntl " + std::to_string(nb.fcntl_result) + ", FIONBIO " +
		std::to_string(nb.fionbio_result) + ", SO_NBIO " + std::to_string(nb.nbio_result) + "; connect " +
		(rc == 0 ? std::string("done") : std::string(std::strerror(connect_errno))) + " after " +
		std::to_string(MillisecondsNow() - started) + " ms");
	if (rc == 0)
	{
		check.open = true;
	}
	else if (connect_errno == EINPROGRESS || connect_errno == EWOULDBLOCK || connect_errno == EALREADY)
	{
		struct pollfd ready;
		ready.fd = fd;
		ready.events = POLLOUT;
		ready.revents = 0;
		const long long deadline = started + limit_ms;
		int polled = 0;
		for (;;)
		{
			const long long left = deadline - MillisecondsNow();
			if (left <= 0)
			{
				polled = 0;
				break;
			}
			polled = poll(&ready, 1, static_cast<int>(left));
			if (polled < 0 && errno == EINTR)
				continue;
			break;
		}
		int error = 0;
		socklen_t size = sizeof(error);
		const int asked = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
		if (polled > 0 && asked == 0 && error == 0 && (ready.revents & POLLOUT))
			check.open = true;
		else
			check.error = polled == 0 ? ETIMEDOUT : (error ? error : (polled < 0 ? errno : ECONNREFUSED));
		step("waited " + std::to_string(MillisecondsNow() - started) + " ms: poll " + std::to_string(polled) + ", events " +
			std::to_string(ready.revents) + ", SO_ERROR " + std::to_string(error) +
			(check.open ? std::string(": open") : std::string(": ") + std::strerror(check.error)));
	}
	else
	{
		check.error = connect_errno;
	}
	close(fd);
	check.ms = MillisecondsNow() - started;
	return check;
}
} // namespace PS5Socket
