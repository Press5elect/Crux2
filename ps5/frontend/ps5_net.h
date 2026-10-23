// Crux2 - what the emulated network adapter and hard drive need to know about the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// PCSX2's "Sockets" network back end carries the PlayStation 2's TCP and UDP traffic over ordinary
// sockets of the machine it runs on, and before it starts it asks that machine which network
// interface it has, what its address, gateway and name servers are. On a PC those answers come
// from the interface list, the routing table and /etc/resolv.conf. A title on the console has no
// /etc, its if_nameindex() always answers "none" (so PCSX2's own search never finds a gateway, and
// with no gateway refuses to start the adapter), and whether the interface list and the routing
// table answer at all is not known until a console says so. So the questions are asked here, each
// in more than one way, and none of them has to be answered for the adapter to start:
//
//   - the console's address: the interface list, and the local address of a UDP socket connected
//     to a public address (connect() on a UDP socket sends nothing, it only picks the route);
//   - the gateway: the routing table. It is shown to the user and nothing else: the game is given
//     a network of its own (192.0.2.0/24, PCSX2's built-in DHCP), not the console's;
//   - names: asked of the console's resolver through getaddrinfo(), the title's own
//     (ps5_libc.c), one lookup at a time, with the answers kept for a minute.
//
// The hard drive is an image file. Here it is made as a file with a size and no content
// (ftruncate), because filling 40 GB with zeros is hours on the console's data partition.
//
// No PCSX2 in this file or in ps5_net.cpp: ps5/tests/net builds both for the PC.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace PS5Net
{
	// ---- for the interface -------------------------------------------------------------------

	/// What a "Network" settings page can show. Strings are empty when a value is not known.
	struct Status
	{
		bool adapter_open = false; ///< a game is running with the network adapter started
		std::string console_ip; ///< the console's own IPv4 address ("192.168.1.50")
		std::string gateway; ///< the console's gateway; empty when the routing table gave none
		std::string dns; ///< the name server(s) the game was told to use; empty while no adapter is open
		std::string how; ///< one plain sentence: where the values above came from

		// More than was asked for; a page may ignore them.
		std::string ps2_ip; ///< the address the game was given (192.0.2.100); empty while no adapter is open
		std::string ps2_mac; ///< the adapter's hardware address as the game sees it
		bool ping_refused = false; ///< the console refused a ping socket: the game's pings go unanswered
		unsigned dhcp_answers = 0; ///< since the adapter started: DHCP requests of the game answered
		unsigned dns_answers = 0, dns_failures = 0; ///< names resolved for the game, and not resolved
		unsigned tcp_connects = 0, tcp_failures = 0; ///< TCP connections made for the game, and refused or timed out
		unsigned udp_ports = 0; ///< UDP ports opened for the game
	};

	/// Safe from any thread, with or without a game running. While no adapter is open the console's
	/// address is looked up again when the last answer is older than five seconds (a few system
	/// calls; nothing is sent on the network and nothing is logged).
	Status CurrentStatus();

	/// Makes a hard drive image of size_gb GiB (1 to 2000) at path as a file with a size and no
	/// content, in a fraction of a second, and checks afterwards (fstat) that the file system did
	/// not allocate the space. Never writes zeros: when the file system cannot make such a file the
	/// image is not made, and error says so. Refuses to touch a file that already exists, and a
	/// drive that reports less than 64 MiB free (the console cannot be asked how much is free: there
	/// the check is skipped, and the log says so). The boot log gets the size asked for, the size
	/// made and the space allocated.
	/// An image larger than 127 GiB makes PCSX2 present a 48-bit LBA drive, which software written
	/// for the 40 GB drive may not accept.
	bool CreateHddImage(const std::string& path, int size_gb, std::string* error);

	/// The size of an image and how much of it the file system has allocated. False when there is
	/// no such file.
	bool DescribeHddImage(const std::string& path, std::uint64_t* size_bytes, std::uint64_t* allocated_bytes);

	// ---- for the adapter (vendor/pcsx2/pcsx2/DEV9, inside #ifdef __PROSPERO__) ----------------

	/// What was found about the console's network. Addresses are four bytes in network order.
	struct HostInfo
	{
		bool have_ip = false; ///< ip is the console's address
		bool have_mask = false; ///< mask came from the interface list (else 255.255.255.0 is assumed)
		bool have_gateway = false; ///< gateway came from the routing table
		bool have_mac = false; ///< mac is the interface's own (else made from the address)
		std::uint8_t ip[4] = {};
		std::uint8_t mask[4] = {255, 255, 255, 0};
		std::uint8_t gateway[4] = {};
		std::uint8_t mac[6] = {};
		char interface_name[16] = "ps5net0"; ///< the interface's name, or this stand-in
		std::string how; ///< the sentence for Status::how
	};

	/// Asks every way there is. With log set, each question, what it answered and the errno of
	/// each failure go to the boot log (the adapter does this once each time it starts).
	HostInfo LookupHost(bool log);

	/// The adapter started (info is what LookupHost gave it) or stopped. Starting clears the counts.
	void AdapterStarted(const HostInfo& info, const std::uint8_t ps2_mac[6]);
	void AdapterStopped();

	/// What the built-in DHCP server will tell the game. dns1/dns2 are zero when there is none;
	/// dns_how says where they came from ("the console's resolver", "set by hand").
	void ReportPs2Config(const std::uint8_t ip[4], const std::uint8_t mask[4], const std::uint8_t gateway[4],
		const std::uint8_t dns1[4], const std::uint8_t dns2[4], const char* dns_how);

	enum class Event
	{
		DhcpAnswer,
		DnsAnswer,
		DnsFailure,
		TcpAttempt,
		TcpConnect,
		TcpFailure,
		UdpPort,
		PingRefused,
		Count
	};

	/// Counts the event, and writes the line to the boot log for the first few of each kind after
	/// the adapter started (a game that opens a connection a second must not fill the log).
	void Note(Event event, const char* format, ...) __attribute__((format(printf, 2, 3)));

	/// Resolves a name to an IPv4 address through getaddrinfo(). One lookup at a time (the
	/// console's resolver is not known to take several at once), answers are kept for a minute,
	/// and a game's repeated question while the first is still being asked gets the same answer.
	/// Returns 0, or getaddrinfo's error code.
	int Resolve(const char* name, std::uint8_t address[4]);

	// ---- for the PC tests ---------------------------------------------------------------------

	namespace Testing
	{
		struct Interface
		{
			std::string name;
			bool up = false;
			bool loopback = false;
			bool has_address = false;
			std::uint8_t address[4] = {};
			std::uint8_t mask[4] = {};
			bool has_mac = false;
			std::uint8_t mac[6] = {};
		};

		/// The questions LookupHost, Resolve and CreateHddImage ask the system. Each returns 0 or an errno
		/// (resolve: a getaddrinfo code). A null entry leaves the real question in place.
		struct Probes
		{
			int (*interfaces)(std::vector<Interface>* list) = nullptr;
			int (*default_gateway)(std::uint8_t gateway[4]) = nullptr;
			int (*route_address)(const std::uint8_t target[4], std::uint8_t local[4]) = nullptr;
			int (*resolve)(const char* name, std::uint8_t address[4]) = nullptr;
			int (*free_space)(const char* folder, std::uint64_t* bytes) = nullptr; ///< ENOSYS: cannot be asked
		};

		void SetProbes(const Probes& probes);

		/// Forgets kept name answers and the status (between tests).
		void Reset();
	} // namespace Testing
} // namespace PS5Net
