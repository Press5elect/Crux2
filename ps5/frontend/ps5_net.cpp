// Crux2 - what the emulated network adapter and hard drive need to know about the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// ps5_net.h says what this is for. Every system call here is one the title already makes
// somewhere else (socket, connect, getsockname, getifaddrs, sysctl, open, ftruncate, fstat,
// pread, stat, unlink, close), or one the link binds to the title's own code (getaddrinfo to
// ps5_libc.c).

#include "ps5_net.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if !defined(__PROSPERO__)
#include <sys/statvfs.h>
#endif
#include <sys/types.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif

extern "C" void ps5px_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace PS5Net
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// ---- state ---------------------------------------------------------------------------

		std::mutex s_mutex; // s_status, s_lookup_time, s_probes
		Status s_status;
		Clock::time_point s_lookup_time;
		bool s_looked_up = false;
		Testing::Probes s_probes;

		// How many lines of each kind of event go to the log after an adapter start.
		constexpr unsigned EVENT_LOG_LINES = 8;
		std::atomic<unsigned> s_event_count[static_cast<size_t>(Event::Count)];
		const char* const s_event_names[static_cast<size_t>(Event::Count)] = {
			"DHCP answers", "names resolved", "names not found", "TCP attempts", "TCP connections", "TCP failures", "UDP ports",
			"refused pings"};

		// Names: one lookup at a time; answers kept for a minute, failures for five seconds (a game
		// repeats its question while the first is still being asked, and gets that one's answer).
		constexpr auto NAME_KEPT = std::chrono::seconds(60);
		constexpr auto FAILURE_KEPT = std::chrono::seconds(5);
		constexpr size_t NAME_CACHE_SIZE = 64;

		// A drive with less than this free is not given an image (where free space can be asked).
		constexpr std::uint64_t MINIMUM_FREE = 64ull << 20;
		struct NameAnswer
		{
			int error;
			std::uint8_t address[4];
			Clock::time_point until;
		};
		std::mutex s_name_mutex; // s_names
		std::map<std::string, NameAnswer> s_names;
		std::mutex s_lookup_mutex; // held across a lookup
		std::atomic<bool> s_cancel_lookups{false};

		// ---- small helpers -------------------------------------------------------------------

		std::string Dotted(const std::uint8_t a[4])
		{
			char text[16];
			std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
			return text;
		}

		std::string Colons(const std::uint8_t m[6])
		{
			char text[18];
			std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
			return text;
		}

		bool IsZero(const std::uint8_t a[4])
		{
			return (a[0] | a[1] | a[2] | a[3]) == 0;
		}

		long long MillisecondsSince(Clock::time_point start)
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
		}

		Testing::Probes CurrentProbes()
		{
			std::lock_guard lock(s_mutex);
			return s_probes;
		}

		// ---- the questions, as the system answers them ----------------------------------------

		int RealInterfaces(std::vector<Testing::Interface>* list)
		{
			ifaddrs* first = nullptr;
			if (getifaddrs(&first) != 0)
				return errno ? errno : EIO;

			for (const ifaddrs* entry = first; entry; entry = entry->ifa_next)
			{
				if (!entry->ifa_name)
					continue;

				auto found = std::find_if(list->begin(), list->end(),
					[&](const Testing::Interface& other) { return other.name == entry->ifa_name; });
				if (found == list->end())
				{
					list->emplace_back();
					found = list->end() - 1;
					found->name = entry->ifa_name;
				}
				found->up = found->up || (entry->ifa_flags & IFF_UP) != 0;
				found->loopback = found->loopback || (entry->ifa_flags & IFF_LOOPBACK) != 0;
				if (!entry->ifa_addr)
					continue;

				// (The family is read from a copy: the entry may be shorter than a sockaddr.)
				sockaddr head;
				std::memcpy(&head, entry->ifa_addr, offsetof(sockaddr, sa_family) + sizeof(head.sa_family));
				if (head.sa_family == AF_INET && !found->has_address)
				{
					sockaddr_in address;
					std::memcpy(&address, entry->ifa_addr, sizeof(address));
					std::memcpy(found->address, &address.sin_addr, 4);
					found->has_address = !IsZero(found->address);
					if (entry->ifa_netmask)
					{
						std::memcpy(&address, entry->ifa_netmask, sizeof(address));
						std::memcpy(found->mask, &address.sin_addr, 4);
					}
				}
#if defined(__FreeBSD__)
				else if (head.sa_family == AF_LINK)
				{
					sockaddr_dl link;
					std::memcpy(&link, entry->ifa_addr, std::min<size_t>(sizeof(link), static_cast<unsigned char>(entry->ifa_addr->sa_len)));
					if (link.sdl_alen == 6 && static_cast<size_t>(link.sdl_nlen) + 6 <= sizeof(link.sdl_data))
					{
						std::memcpy(found->mac, link.sdl_data + link.sdl_nlen, 6);
						found->has_mac = true;
					}
				}
#elif defined(__linux__)
				else if (head.sa_family == AF_PACKET)
				{
					sockaddr_ll link;
					std::memcpy(&link, entry->ifa_addr, sizeof(link));
					if (link.sll_halen == 6)
					{
						std::memcpy(found->mac, link.sll_addr, 6);
						found->has_mac = true;
					}
				}
#endif
			}
			freeifaddrs(first);
			return 0;
		}

		// The default route's gateway. detail gets a few words for the log.
		int RealDefaultGateway(std::uint8_t gateway[4], std::string* detail)
		{
#if defined(__FreeBSD__)
			int name[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0};
			size_t length = 0;
			if (sysctl(name, 6, nullptr, &length, nullptr, 0) != 0)
				return errno ? errno : EIO;
			if (length == 0)
			{
				*detail = "the table is empty";
				return ENOENT;
			}

			// (The table may grow between the two calls.)
			length += length / 2 + 1024;
			std::vector<char> table(length);
			if (sysctl(name, 6, table.data(), &length, nullptr, 0) != 0)
				return errno ? errno : EIO;

			// One message per route: a header, then the addresses rtm_addrs names, destination first
			// and gateway second, each rounded up to a long. Nothing here trusts a length it has not
			// checked: what the console's kernel writes is not known.
			unsigned routes = 0;
			size_t offset = 0;
			while (offset + sizeof(rt_msghdr) <= length)
			{
				rt_msghdr header;
				std::memcpy(&header, table.data() + offset, sizeof(header));
				if (header.rtm_msglen < sizeof(rt_msghdr) || offset + header.rtm_msglen > length)
					break;

				routes++;
				const char* at = table.data() + offset + sizeof(rt_msghdr);
				const char* const end = table.data() + offset + header.rtm_msglen;
				offset += header.rtm_msglen;
				if (!(header.rtm_flags & RTF_GATEWAY) || !(header.rtm_flags & RTF_UP) || !(header.rtm_addrs & RTA_DST) ||
					!(header.rtm_addrs & RTA_GATEWAY))
					continue;

				std::uint8_t found[2][4] = {};
				bool is_ipv4[2] = {};
				bool whole = true;
				for (int index = 0; index < 2; index++)
				{
					if (end - at < 2)
					{
						whole = false;
						break;
					}
					const size_t sa_len = static_cast<unsigned char>(at[0]);
					const size_t step = sa_len == 0 ? sizeof(long) : (sa_len + sizeof(long) - 1) / sizeof(long) * sizeof(long);
					if (static_cast<unsigned char>(at[1]) == AF_INET && sa_len >= 8 && end - at >= 8)
					{
						is_ipv4[index] = true;
						std::memcpy(found[index], at + 4, 4);
					}
					if (static_cast<size_t>(end - at) < step)
					{
						whole = index == 1; // the gateway may be the message's last address
						break;
					}
					at += step;
				}
				// The default route: destination 0.0.0.0 (or too short to hold an address at all).
				if (whole && IsZero(found[0]) && is_ipv4[1] && !IsZero(found[1]))
				{
					std::memcpy(gateway, found[1], 4);
					return 0;
				}
			}
			*detail = std::to_string(routes) + " routes, none of them a default route";
			return ENOENT;
#elif defined(__linux__)
			// (The PC tests only: the console is not Linux.)
			std::FILE* const table = std::fopen("/proc/net/route", "r");
			if (!table)
				return errno ? errno : EIO;
			char line[256];
			unsigned routes = 0;
			int result = ENOENT;
			while (std::fgets(line, sizeof(line), table))
			{
				char interface[32];
				unsigned destination, gate, flags;
				if (std::sscanf(line, "%31s %x %x %x", interface, &destination, &gate, &flags) != 4)
					continue;
				routes++;
				if (destination == 0 && (flags & 2) && gate != 0)
				{
					std::memcpy(gateway, &gate, 4); // already in network order
					result = 0;
					break;
				}
			}
			std::fclose(table);
			if (result != 0)
				*detail = std::to_string(routes) + " routes, none of them a default route";
			return result;
#else
			(void)gateway;
			*detail = "not asked on this system";
			return ENOSYS;
#endif
		}

		// The local address of a UDP socket connected to target: connect() sends nothing, it picks
		// the route, and getsockname() names the address on it.
		int RealRouteAddress(const std::uint8_t target[4], std::uint8_t local[4])
		{
			const int fd = socket(AF_INET, SOCK_DGRAM, 0);
			if (fd < 0)
				return errno ? errno : EIO;

			sockaddr_in to = {};
#if defined(__FreeBSD__)
			to.sin_len = sizeof(to);
#endif
			to.sin_family = AF_INET;
			to.sin_port = htons(53);
			std::memcpy(&to.sin_addr, target, 4);

			int result = 0;
			sockaddr_in from = {};
			socklen_t length = sizeof(from);
			if (connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0 ||
				getsockname(fd, reinterpret_cast<sockaddr*>(&from), &length) != 0)
				result = errno ? errno : EIO;
			else
				std::memcpy(local, &from.sin_addr, 4);
			close(fd);
			return result;
		}

		int RealResolve(const char* name, std::uint8_t address[4])
		{
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			addrinfo* list = nullptr;
			const int error = getaddrinfo(name, nullptr, &hints, &list);
			if (error != 0)
				return error;

			int result = EAI_NONAME;
			for (const addrinfo* entry = list; entry; entry = entry->ai_next)
			{
				if (entry->ai_family != AF_INET || !entry->ai_addr || entry->ai_addrlen < sizeof(sockaddr_in))
					continue;
				sockaddr_in found;
				std::memcpy(&found, entry->ai_addr, sizeof(found));
				std::memcpy(address, &found.sin_addr, 4);
				result = 0;
				break;
			}
			if (list)
				freeaddrinfo(list);
			return result;
		}

		// What is free in a folder's drive, for a program that is not root.
		int RealFreeSpace(const char* folder, std::uint64_t* bytes)
		{
#if defined(__PROSPERO__)
			// No call a title can make reports a drive's free space (statvfs is the platform
			// layer's stand-in and always answers 16 GiB), so nothing is asked and nothing claimed.
			(void)folder;
			(void)bytes;
			return ENOSYS;
#else
			struct statvfs volume;
			if (statvfs(folder, &volume) != 0)
				return errno ? errno : EIO;
			*bytes = static_cast<std::uint64_t>(volume.f_bavail) * volume.f_frsize;
			return 0;
#endif
		}

		// ---- the status ----------------------------------------------------------------------

		// (s_mutex held.)
		void ApplyHostInfo(const HostInfo& info)
		{
			s_status.console_ip = info.have_ip ? Dotted(info.ip) : std::string();
			s_status.gateway = info.have_gateway ? Dotted(info.gateway) : std::string();
			s_status.how = info.how;
			s_lookup_time = Clock::now();
			s_looked_up = true;
		}
	} // namespace

	// ---- the lookup --------------------------------------------------------------------------

	HostInfo LookupHost(bool log)
	{
		const Testing::Probes probes = CurrentProbes();
		HostInfo info;

		// 1. The interface list.
		std::vector<Testing::Interface> interfaces;
		const int list_error = probes.interfaces ? probes.interfaces(&interfaces) : RealInterfaces(&interfaces);
		if (list_error != 0)
			interfaces.clear();
		if (log)
		{
			if (list_error != 0)
				ps5px_log("net: interface list: failed (errno %d: %s)", list_error, std::strerror(list_error));
			else if (interfaces.empty())
				ps5px_log("net: interface list: empty");
			for (const Testing::Interface& entry : interfaces)
			{
				ps5px_log("net: interface %s:%s%s address %s mask %s mac %s", entry.name.c_str(), entry.up ? " up" : " down",
					entry.loopback ? " loopback" : "", entry.has_address ? Dotted(entry.address).c_str() : "none",
					entry.has_address ? Dotted(entry.mask).c_str() : "none", entry.has_mac ? Colons(entry.mac).c_str() : "none");
			}
		}

		// 2. The routing table, for the gateway.
		std::string detail;
		const int gateway_error = probes.default_gateway ? probes.default_gateway(info.gateway) : RealDefaultGateway(info.gateway, &detail);
		info.have_gateway = gateway_error == 0 && !IsZero(info.gateway);
		if (!info.have_gateway)
			std::memset(info.gateway, 0, 4);
		if (log)
		{
			if (info.have_gateway)
				ps5px_log("net: routing table: default gateway %s", Dotted(info.gateway).c_str());
			else
				ps5px_log("net: routing table: no gateway (errno %d: %s%s%s)", gateway_error, std::strerror(gateway_error),
					detail.empty() ? "" : "; ", detail.c_str());
		}

		// 3. The address traffic leaves from: a public address first (the route a game's
		// connections take), then the gateway, then the usual home routers, for a console on a
		// network with no way out (two consoles and a switch).
		std::uint8_t targets[6][4] = {{1, 1, 1, 1}, {}, {192, 168, 1, 1}, {192, 168, 0, 1}, {10, 0, 0, 1}, {172, 16, 0, 1}};
		std::memcpy(targets[1], info.gateway, 4);
		std::uint8_t routed[4] = {};
		bool have_routed = false;
		bool routed_public = false;
		for (size_t index = 0; index < std::size(targets) && !have_routed; index++)
		{
			if (IsZero(targets[index]))
				continue;
			std::uint8_t local[4] = {};
			const int error = probes.route_address ? probes.route_address(targets[index], local) : RealRouteAddress(targets[index], local);
			have_routed = error == 0 && !IsZero(local) && local[0] != 127;
			if (have_routed)
			{
				std::memcpy(routed, local, 4);
				routed_public = index == 0;
			}
			if (log)
			{
				if (have_routed)
					ps5px_log("net: a UDP socket connected to %s has the local address %s", Dotted(targets[index]).c_str(), Dotted(local).c_str());
				else if (error != 0)
					ps5px_log("net: a UDP socket connected to %s: failed (errno %d: %s)", Dotted(targets[index]).c_str(), error, std::strerror(error));
				else
					ps5px_log("net: a UDP socket connected to %s has the local address %s, which is no use", Dotted(targets[index]).c_str(),
						Dotted(local).c_str());
			}
		}

		// The address: the routed one, with the interface that holds it for its mask and hardware
		// address; else the first interface that is up, not the loopback, and has an address.
		const Testing::Interface* chosen = nullptr;
		if (have_routed)
		{
			info.have_ip = true;
			std::memcpy(info.ip, routed, 4);
			for (const Testing::Interface& entry : interfaces)
			{
				if (entry.has_address && std::memcmp(entry.address, routed, 4) == 0)
					chosen = &entry;
			}
			info.how = std::string("The console's address is the one its traffic to ") + (routed_public ? "the internet" : "the local network") +
					   " leaves from" + (chosen ? " (interface " + chosen->name + ")" : "");
		}
		else
		{
			for (const Testing::Interface& entry : interfaces)
			{
				if (!chosen && entry.up && !entry.loopback && entry.has_address)
					chosen = &entry;
			}
			if (chosen)
			{
				info.have_ip = true;
				std::memcpy(info.ip, chosen->address, 4);
				info.how = "The console's address is from its interface list (" + chosen->name + "); no route to the internet was found";
			}
			else
				info.how = "The console's address was not found: no interface with an address and no route";
		}

		if (chosen)
		{
			std::snprintf(info.interface_name, sizeof(info.interface_name), "%s", chosen->name.c_str());
			if (!IsZero(chosen->mask))
			{
				info.have_mask = true;
				std::memcpy(info.mask, chosen->mask, 4);
			}
			if (chosen->has_mac)
			{
				info.have_mac = true;
				std::memcpy(info.mac, chosen->mac, 6);
			}
		}
		if (!info.have_mac && info.have_ip)
		{
			// A stand-in that differs between two consoles on one network: locally administered,
			// then the address.
			const std::uint8_t made[6] = {0x02, 0x00, info.ip[0], info.ip[1], info.ip[2], info.ip[3]};
			std::memcpy(info.mac, made, 6);
		}
		info.how += info.have_gateway ? "; the gateway is from its routing table." : "; its gateway is not known (the game does not need it).";

		if (log)
		{
			ps5px_log("net: console address %s, mask %s%s, gateway %s, hardware address %s%s", info.have_ip ? Dotted(info.ip).c_str() : "unknown",
				Dotted(info.mask).c_str(), info.have_mask ? "" : " (assumed)", info.have_gateway ? Dotted(info.gateway).c_str() : "unknown",
				(info.have_mac || info.have_ip) ? Colons(info.mac).c_str() : "unknown", info.have_mac ? "" : " (made up)");
		}
		return info;
	}

	Status CurrentStatus()
	{
		std::unique_lock lock(s_mutex);
		if (!s_status.adapter_open && (!s_looked_up || Clock::now() - s_lookup_time > std::chrono::seconds(5)))
		{
			lock.unlock();
			const HostInfo info = LookupHost(false);
			lock.lock();
			if (!s_status.adapter_open)
				ApplyHostInfo(info);
		}

		Status status = s_status;
		lock.unlock();
		status.dhcp_answers = s_event_count[static_cast<size_t>(Event::DhcpAnswer)].load(std::memory_order_relaxed);
		status.dns_answers = s_event_count[static_cast<size_t>(Event::DnsAnswer)].load(std::memory_order_relaxed);
		status.dns_failures = s_event_count[static_cast<size_t>(Event::DnsFailure)].load(std::memory_order_relaxed);
		status.tcp_connects = s_event_count[static_cast<size_t>(Event::TcpConnect)].load(std::memory_order_relaxed);
		status.tcp_failures = s_event_count[static_cast<size_t>(Event::TcpFailure)].load(std::memory_order_relaxed);
		status.udp_ports = s_event_count[static_cast<size_t>(Event::UdpPort)].load(std::memory_order_relaxed);
		status.ping_refused = s_event_count[static_cast<size_t>(Event::PingRefused)].load(std::memory_order_relaxed) != 0;
		return status;
	}

	// ---- the adapter's reports ---------------------------------------------------------------

	void AdapterStarted(const HostInfo& info, const std::uint8_t ps2_mac[6])
	{
		for (std::atomic<unsigned>& count : s_event_count)
			count.store(0, std::memory_order_relaxed);
		s_cancel_lookups.store(false);

		std::lock_guard lock(s_mutex);
		ApplyHostInfo(info);
		s_status.adapter_open = true;
		s_status.ps2_mac = Colons(ps2_mac);
		ps5px_log("net: adapter started: the game's hardware address is %s", s_status.ps2_mac.c_str());
	}

	void AdapterStopped()
	{
		// Lookups still waiting their turn give up; the one being asked runs to its end.
		s_cancel_lookups.store(true);

		std::lock_guard lock(s_mutex);
		if (!s_status.adapter_open)
			return;
		s_status.adapter_open = false;
		s_status.dns.clear();
		s_status.ps2_ip.clear();
		s_status.ps2_mac.clear();
		ps5px_log("net: adapter stopped: %u DHCP answers, %u names resolved and %u not, %u TCP connections and %u failed, %u UDP ports",
			s_event_count[static_cast<size_t>(Event::DhcpAnswer)].load(), s_event_count[static_cast<size_t>(Event::DnsAnswer)].load(),
			s_event_count[static_cast<size_t>(Event::DnsFailure)].load(), s_event_count[static_cast<size_t>(Event::TcpConnect)].load(),
			s_event_count[static_cast<size_t>(Event::TcpFailure)].load(), s_event_count[static_cast<size_t>(Event::UdpPort)].load());
	}

	void ReportPs2Config(const std::uint8_t ip[4], const std::uint8_t mask[4], const std::uint8_t gateway[4], const std::uint8_t dns1[4],
		const std::uint8_t dns2[4], const char* dns_how)
	{
		std::string dns;
		if (!IsZero(dns1))
			dns = Dotted(dns1);
		if (!IsZero(dns2))
			dns += (dns.empty() ? "" : " and ") + Dotted(dns2);
		if (dns.empty())
			dns = "none";
		else if (dns_how && dns_how[0])
			dns += std::string(" (") + dns_how + ")";

		std::lock_guard lock(s_mutex);
		const std::string ps2_ip = Dotted(ip);
		// (Written again only when it changes: the adapter reports on every settings change.)
		if (s_status.dns != dns || s_status.ps2_ip != ps2_ip)
			ps5px_log("net: the game is given address %s, mask %s, gateway %s, name servers %s", ps2_ip.c_str(), Dotted(mask).c_str(),
				Dotted(gateway).c_str(), dns.c_str());
		s_status.dns = dns;
		s_status.ps2_ip = ps2_ip;
	}

	void Note(Event event, const char* format, ...)
	{
		const size_t index = static_cast<size_t>(event);
		if (index >= static_cast<size_t>(Event::Count))
			return;

		const unsigned before = s_event_count[index].fetch_add(1, std::memory_order_relaxed);
		if (before > EVENT_LOG_LINES)
			return;
		if (before == EVENT_LOG_LINES)
		{
			ps5px_log("net: (more %s are counted, not written here)", s_event_names[index]);
			return;
		}

		char line[256];
		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(line, sizeof(line), format, arguments);
		va_end(arguments);
		ps5px_log("net: %s", line);
	}

	int Resolve(const char* name, std::uint8_t address[4])
	{
		if (!name || !name[0])
			return EAI_NONAME;

		std::string key(name);
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });

		const auto kept = [&](int* error) {
			std::lock_guard lock(s_name_mutex);
			const auto found = s_names.find(key);
			if (found == s_names.end() || Clock::now() >= found->second.until)
				return false;
			*error = found->second.error;
			std::memcpy(address, found->second.address, 4);
			return true;
		};
		const auto count = [](int error) {
			s_event_count[static_cast<size_t>(error == 0 ? Event::DnsAnswer : Event::DnsFailure)].fetch_add(1, std::memory_order_relaxed);
		};

		int error = 0;
		if (kept(&error))
		{
			count(error);
			return error;
		}

		const Clock::time_point asked = Clock::now();
		std::lock_guard lookup(s_lookup_mutex);
		if (kept(&error))
		{
			count(error);
			return error;
		}
		if (s_cancel_lookups.load())
			return EAI_AGAIN;

		const Testing::Probes probes = CurrentProbes();
		const Clock::time_point started = Clock::now();
		std::uint8_t found[4] = {};
		error = probes.resolve ? probes.resolve(key.c_str(), found) : RealResolve(key.c_str(), found);
		if (error == 0 && IsZero(found))
			error = EAI_NONAME;

		{
			std::lock_guard lock(s_name_mutex);
			if (s_names.size() >= NAME_CACHE_SIZE)
				s_names.clear();
			NameAnswer& answer = s_names[key];
			answer.error = error;
			std::memcpy(answer.address, found, 4);
			answer.until = Clock::now() + (error == 0 ? std::chrono::duration_cast<Clock::duration>(NAME_KEPT) : std::chrono::duration_cast<Clock::duration>(FAILURE_KEPT));
		}

		if (error == 0)
		{
			std::memcpy(address, found, 4);
			Note(Event::DnsAnswer, "name %s is %s (asked for %lld ms, waited %lld ms before)", key.c_str(), Dotted(found).c_str(),
				MillisecondsSince(started), static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(started - asked).count()));
		}
		else
		{
			Note(Event::DnsFailure, "name %s was not found (getaddrinfo error %d, asked for %lld ms)", key.c_str(), error, MillisecondsSince(started));
		}
		return error;
	}

	// ---- the hard drive image ----------------------------------------------------------------

	bool DescribeHddImage(const std::string& path, std::uint64_t* size_bytes, std::uint64_t* allocated_bytes)
	{
		struct stat status;
		if (path.empty() || stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
			return false;
		if (size_bytes)
			*size_bytes = static_cast<std::uint64_t>(status.st_size);
		if (allocated_bytes)
			*allocated_bytes = static_cast<std::uint64_t>(status.st_blocks) * 512u;
		return true;
	}

	bool CreateHddImage(const std::string& path, int size_gb, std::string* error)
	{
		const auto fail = [&](const std::string& message) {
			ps5px_log("hdd image: %s: not made: %s", path.c_str(), message.c_str());
			if (error)
				*error = message;
			return false;
		};
		const auto with_errno = [](const char* what, int reason) {
			return std::string(what) + " failed (errno " + std::to_string(reason) + ": " + std::strerror(reason) + ")";
		};

		if (path.empty())
			return fail("no file name");
		if (size_gb < 1 || size_gb > 2000)
			return fail("the size must be 1 to 2000 GB");

		struct stat status;
		if (stat(path.c_str(), &status) == 0)
			return fail("there is already a file of that name; it was left as it is");

		const size_t slash = path.find_last_of('/');
		const std::string folder = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
		struct stat folder_status;
		if (stat(folder.c_str(), &folder_status) != 0 || !S_ISDIR(folder_status.st_mode))
			return fail("the folder " + folder + " does not exist");

		// Free space. An image with no content needs next to none when it is made; it takes space
		// as the game writes to it, so all that is refused here is a drive that is already full.
		const Testing::Probes probes = CurrentProbes();
		std::uint64_t free_bytes = 0;
		const int free_error = probes.free_space ? probes.free_space(folder.c_str(), &free_bytes) : RealFreeSpace(folder.c_str(), &free_bytes);
		if (free_error == ENOSYS)
			ps5px_log("hdd image: free space of %s cannot be asked on this system", folder.c_str());
		else if (free_error != 0)
			ps5px_log("hdd image: free space of %s is not known (errno %d: %s)", folder.c_str(), free_error, std::strerror(free_error));
		else
		{
			ps5px_log("hdd image: %s reports %llu MiB free", folder.c_str(), static_cast<unsigned long long>(free_bytes >> 20));
			if (free_bytes < MINIMUM_FREE)
				return fail("less than 64 MiB is free in " + folder);
		}

		const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
		if (fd < 0)
			return fail(with_errno("creating the file", errno));
		const auto give_up = [&](const std::string& message) {
			// (An emptied file first: removing a large allocated one can itself take long.)
			(void)ftruncate(fd, 0);
			close(fd);
			unlink(path.c_str());
			return fail(message);
		};

		// A small file first. A file system that cannot leave holes allocates (and may write) the
		// whole length inside ftruncate, with no way to stop it: 16 MiB of that is seconds, 40 GiB
		// would be hours. Only when the small one came out empty is the real size asked for.
		constexpr off_t PROBE = 16 << 20;
		const Clock::time_point started = Clock::now();
		if (ftruncate(fd, PROBE) != 0)
			return give_up(with_errno("giving the file a size", errno));
		if (fstat(fd, &status) != 0)
			return give_up(with_errno("reading the file's size", errno));
		const std::uint64_t probe_allocated = static_cast<std::uint64_t>(status.st_blocks) * 512u;
		if (status.st_size != PROBE || probe_allocated >= static_cast<std::uint64_t>(PROBE) / 2)
		{
			return give_up("this drive's file system gives a file all of its space at once (a 16 MiB file took " +
						   std::to_string(probe_allocated) + " bytes); an image cannot be made here without writing all of it");
		}

		const off_t wanted = static_cast<off_t>(size_gb) << 30;
		if (ftruncate(fd, wanted) != 0)
			return give_up(with_errno("giving the file its size", errno));
		if (fstat(fd, &status) != 0)
			return give_up(with_errno("reading the file's size", errno));
		if (status.st_size != wanted)
			return give_up("the file is " + std::to_string(static_cast<long long>(status.st_size)) + " bytes, not the " +
						   std::to_string(static_cast<long long>(wanted)) + " asked for");
		const std::uint64_t allocated = static_cast<std::uint64_t>(status.st_blocks) * 512u;

		// What the game reads from a place nothing was written to must be zeros: the first, a
		// middle and the last sector, read as the emulated drive reads them.
		bool zeros = true;
		const off_t places[3] = {0, (wanted / 2) & ~static_cast<off_t>(511), wanted - 512};
		for (const off_t place : places)
		{
			unsigned char sector[512];
			std::memset(sector, 0xa5, sizeof(sector));
			const ssize_t got = pread(fd, sector, sizeof(sector), place);
			if (got != static_cast<ssize_t>(sizeof(sector)))
				return give_up(got < 0 ? with_errno("reading the new file", errno) : std::string("the new file cannot be read to its end"));
			for (const unsigned char byte : sector)
				zeros = zeros && byte == 0;
		}
		if (!zeros)
			return give_up("the new file does not read as zeros");

		if (close(fd) != 0)
		{
			const int reason = errno;
			unlink(path.c_str());
			return fail(with_errno("closing the file", reason));
		}

		const bool sparse = allocated < static_cast<std::uint64_t>(wanted) / 2;
		ps5px_log("hdd image: %s: made in %lld ms: %d GiB asked for, size %lld bytes, %llu bytes allocated (%s), reads as zeros", path.c_str(),
			MillisecondsSince(started), size_gb, static_cast<long long>(status.st_size), static_cast<unsigned long long>(allocated),
			sparse ? "sparse" : "NOT sparse: the file system allocated it all");
		if (error)
			error->clear();
		return true;
	}

	// ---- for the PC tests --------------------------------------------------------------------

	void Testing::SetProbes(const Probes& probes)
	{
		std::lock_guard lock(s_mutex);
		s_probes = probes;
	}

	void Testing::Reset()
	{
		{
			std::lock_guard lock(s_name_mutex);
			s_names.clear();
		}
		for (std::atomic<unsigned>& count : s_event_count)
			count.store(0, std::memory_order_relaxed);
		s_cancel_lookups.store(false);

		std::lock_guard lock(s_mutex);
		s_status = Status();
		s_looked_up = false;
	}
} // namespace PS5Net
