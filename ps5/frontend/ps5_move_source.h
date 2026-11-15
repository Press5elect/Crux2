// Crux2 - a PlayStation Move as player 1's GunCon 2, from Move Management (ps5_move_source.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The functions PS5InputSource calls, on its polling thread (all of them on that one thread), and
// the loopback socket they read through (here, with no console call in it, so ps5/tests/move runs
// it on the PC against a stand-in producer).

#pragma once

#include "ps5_move_logic.h"

#include <cstddef>
#include <cstdint>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace PS5Move
{
	/// Once a poll, after the controllers were read. Reads [PS5] MoveAim (once a second), opens or
	/// closes the loopback sockets, takes what arrived and works out player 1's gun. While a game
	/// runs and the Move is live and centred it moves InputManager's pointer 0 (what the GunCon 2
	/// aims at) and answers true: the controller's own motion aim is then left alone for player 1.
	bool Update(bool running);

	/// Player 1's SDL buttons the Move and the Nav hold now (bit n: SDL button n), while a game runs.
	uint32_t GameButtons();

	/// Player 1's right trigger as the Move's T gives it (0 or 255), while a game runs.
	uint8_t Trigger();

	/// The sockets closed and everything let go (the input source shuts down).
	void Shutdown();

	/// One non-blocking UDP socket bound to 127.0.0.1:port, opened once, read with a budget.
	class Socket
	{
	public:
		static constexpr unsigned DRAIN_BUDGET = 64; // datagrams a poll, as the reference client

		Socket() = default;
		~Socket() { Close(); }
		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;

		/// False if the socket cannot be had (another program has the port, no network stack).
		bool Open(uint16_t port)
		{
			Close();
			m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
			if (m_fd < 0)
				return false;
			const int flags = fcntl(m_fd, F_GETFL, 0);
			if (flags < 0 || fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) != 0)
			{
				Close();
				return false;
			}
			sockaddr_in at = {};
			SetLength(at, 0);
			at.sin_family = AF_INET;
			at.sin_port = htons(port);
			at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0)
			{
				Close();
				return false;
			}
			socklen_t size = sizeof(at); // the port it got (port 0: any free one, for the PC's test)
			m_port = (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&at), &size) == 0) ? ntohs(at.sin_port) : port;
			return true;
		}

		void Close()
		{
			if (m_fd >= 0)
				::close(m_fd);
			m_fd = -1;
			m_port = 0;
		}

		bool IsOpen() const { return m_fd >= 0; }
		uint16_t Port() const { return m_port; }

		/// Hands each waiting datagram from 127.0.0.1 to take(bytes, size), at most DRAIN_BUDGET of
		/// them (the rest wait for the next poll). The number read.
		template <typename Take>
		unsigned Drain(Take&& take)
		{
			unsigned n = 0;
			if (m_fd < 0)
				return 0;
			uint8_t bytes[512]; // one larger than any packet is refused by its size
			for (; n < DRAIN_BUDGET; n++)
			{
				sockaddr_in from = {};
				socklen_t from_size = sizeof(from);
				const ssize_t got = ::recvfrom(m_fd, bytes, sizeof(bytes), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &from_size);
				if (got < 0)
					break;
				if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
					continue; // bound to loopback; only this console's programs
				take(static_cast<const uint8_t*>(bytes), static_cast<size_t>(got));
			}
			return n;
		}

	private:
		// sin_len where the system has one (the console's FreeBSD-style headers), nothing elsewhere.
		template <typename A>
		static auto SetLength(A& a, int) -> decltype(a.sin_len = sizeof(A), void())
		{
			a.sin_len = sizeof(A);
		}
		template <typename A>
		static void SetLength(A&, long)
		{
		}

		int m_fd = -1;
		uint16_t m_port = 0;
	};
} // namespace PS5Move
