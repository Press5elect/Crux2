// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the console network module on a PC: the questions about the host answered by
// stand-ins (as the console may answer them, or not at all), and the hard drive image made as a
// file with a size and no content.
//   net_test <work folder>
#include "ps5_net.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void ps5px_log(const char* format, ...)
{
	(void)format;
}

static int g_failed = 0, g_checks = 0;
#define EXPECT(c, what) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAILED: %s (line %d)\n", what, __LINE__); } } while (0)

using namespace PS5Net;

static bool s_list_works = true, s_route_works = true, s_gateway_works = true;
static int Interfaces(std::vector<Testing::Interface>* list)
{
	if (!s_list_works)
		return ENOSYS;
	Testing::Interface lo;
	lo.name = "lo0";
	lo.up = lo.loopback = lo.has_address = true;
	const std::uint8_t local[4] = {127, 0, 0, 1}, mask8[4] = {255, 0, 0, 0};
	std::memcpy(lo.address, local, 4);
	std::memcpy(lo.mask, mask8, 4);
	list->push_back(lo);
	Testing::Interface eth;
	eth.name = "eth0";
	eth.up = eth.has_address = eth.has_mac = true;
	const std::uint8_t address[4] = {192, 0, 2, 77}, mask[4] = {255, 255, 255, 128}, mac[6] = {2, 4, 6, 8, 10, 12};
	std::memcpy(eth.address, address, 4);
	std::memcpy(eth.mask, mask, 4);
	std::memcpy(eth.mac, mac, 6);
	list->push_back(eth);
	return 0;
}
static int Gateway(std::uint8_t gateway[4])
{
	if (!s_gateway_works)
		return ENOENT;
	const std::uint8_t g[4] = {192, 0, 2, 1};
	std::memcpy(gateway, g, 4);
	return 0;
}
static int Route(const std::uint8_t*, std::uint8_t local[4])
{
	if (!s_route_works)
		return ENETUNREACH;
	// (The interface's own address when there is a list to find it in; else it is all there is.)
	const std::uint8_t a[4] = {192, 0, 2, static_cast<std::uint8_t>(s_list_works ? 77 : 78)};
	std::memcpy(local, a, 4);
	return 0;
}
static int s_resolves = 0;
static int Resolve4(const char* name, std::uint8_t address[4])
{
	s_resolves++;
	if (std::strcmp(name, "nowhere.invalid") == 0)
		return 8; // EAI_NONAME on the BSDs
	const std::uint8_t a[4] = {198, 51, 100, 9};
	std::memcpy(address, a, 4);
	return 0;
}
static int FreeSpace(const char*, std::uint64_t*)
{
	return ENOSYS; // as on the console
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	const std::string work = argv[1];
	Testing::Probes probes;
	probes.interfaces = &Interfaces;
	probes.default_gateway = &Gateway;
	probes.route_address = &Route;
	probes.resolve = &Resolve4;
	probes.free_space = &FreeSpace;
	Testing::SetProbes(probes);

	// Everything answers: the interface list's address, mask and hardware address.
	HostInfo info = LookupHost(true);
	EXPECT(info.have_ip && info.ip[3] == 77 && info.have_mask && info.mask[3] == 128 && info.have_mac && info.mac[5] == 12 && info.have_gateway && info.gateway[3] == 1,
		"the interface list and the routing table are believed when they answer");
	EXPECT(!info.how.empty() && std::strcmp(info.interface_name, "eth0") == 0, "and it says where from");
	// No interface list (as the console may be): the route's own address, a /24 assumed.
	s_list_works = false;
	Testing::Reset();
	info = LookupHost(true);
	EXPECT(info.have_ip && info.ip[3] == 78 && !info.have_mask && info.mask[2] == 255 && info.mask[3] == 0 && !info.have_mac, "without the list, the address a connection would leave from");
	// Nothing answers: no address, and nothing breaks.
	s_route_works = s_gateway_works = false;
	Testing::Reset();
	info = LookupHost(true);
	EXPECT(!info.have_ip && !info.have_gateway && !info.how.empty(), "with no answer at all there is no address, and it says so");
	Status status = CurrentStatus();
	EXPECT(!status.adapter_open && status.console_ip.empty(), "the status with no network");
	s_list_works = s_route_works = s_gateway_works = true;
	Testing::Reset();

	// The adapter's life, as vendor DEV9 tells it.
	info = LookupHost(false);
	const std::uint8_t mac[6] = {0, 4, 31, 1, 2, 3};
	AdapterStarted(info, mac);
	const std::uint8_t ps2[4] = {192, 0, 2, 100}, mask[4] = {255, 255, 255, 0}, gw[4] = {192, 0, 2, 1}, none[4] = {0, 0, 0, 0};
	ReportPs2Config(ps2, mask, gw, gw, none, "the built-in server");
	Note(Event::DhcpAnswer, "DHCP answered");
	std::uint8_t found[4] = {};
	EXPECT(Resolve("play.example", found) == 0 && found[0] == 198, "a name is resolved");
	EXPECT(Resolve("play.example", found) == 0 && s_resolves == 1, "and asked of the resolver once: the answer is kept");
	EXPECT(Resolve("nowhere.invalid", found) != 0, "a name nobody has is an error, not an address");
	status = CurrentStatus();
	EXPECT(status.adapter_open && status.console_ip == "192.0.2.77" && status.gateway == "192.0.2.1" && status.ps2_ip == "192.0.2.100" && status.dhcp_answers == 1,
		"the status while a game is online");
	AdapterStopped();
	EXPECT(!CurrentStatus().adapter_open, "and after it");

	// The hard drive: a file with a size and no content.
	const std::string image = work + "/DEV9hdd.raw";
	unlink(image.c_str());
	std::string error;
	std::uint64_t size = 0, allocated = 0;
	EXPECT(!DescribeHddImage(image, &size, &allocated), "no image yet");
	const bool made = CreateHddImage(image, 40, &error);
	EXPECT(made, "an image of 40 GB is made");
	if (!made)
		std::printf("  (%s)\n", error.c_str());
	EXPECT(DescribeHddImage(image, &size, &allocated) && size == (40ull << 30) && allocated < (64ull << 20), "of that size, taking next to no room");
	std::printf("  40 GB image: %llu bytes, %llu allocated\n", static_cast<unsigned long long>(size), static_cast<unsigned long long>(allocated));
	if (FILE* f = std::fopen(image.c_str(), "rb"))
	{
		unsigned char sector[512];
		std::memset(sector, 1, sizeof(sector));
		fseeko(f, 20ll << 30, SEEK_SET);
		const size_t got = std::fread(sector, 1, sizeof(sector), f);
		bool zero = got == sizeof(sector);
		for (const unsigned char c : sector)
			zero = zero && c == 0;
		std::fclose(f);
		EXPECT(zero, "what was never written reads as zeros");
	}
	EXPECT(!CreateHddImage(image, 40, &error) && !error.empty(), "an image that is there is not replaced");
	EXPECT(!CreateHddImage(work + "/no-such-folder/x.raw", 40, &error), "a folder that is not there: no image, and no crash");
	EXPECT(!CreateHddImage(work + "/zero.raw", 0, &error) && !CreateHddImage(work + "/huge.raw", 5000, &error), "sizes out of range are refused");
	unlink(image.c_str());

	std::printf("net: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
