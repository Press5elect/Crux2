// Crux2 - the PC tests of USB pads through ugen nodes (frontend/ps5_ugen.h), over a made-up FreeBSD
// USB stack: device nodes that open once at a time, FreeBSD's transfer table (USB_FS_INIT, _OPEN,
// _START, _COMPLETE, _STOP, _UNINIT) and control requests (USB_DO_REQUEST), a console's HID driver to
// detach, a device unplugged mid-read, and transfers that never stop after being told to.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_ugen.h"
#include "ps5_usbpad.h"

#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

static int s_checks = 0, s_failures = 0;
#define CHECK(x) \
	do \
	{ \
		s_checks++; \
		if (!(x)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
		} \
	} while (0)

using namespace PS5Ugen;

// ---------------------------------------------------------------- descriptors

// An Xbox 360 pad: one vendor interface (ff/5d/01), interrupt IN 0x81 and OUT 0x02 of 32 bytes.
static const uint8_t s_x360_config[] = {
	9, 2, 49, 0, 1, 1, 0, 0xa0, 0xfa, // configuration, 49 bytes in all
	9, 4, 0, 0, 2, 0xff, 0x5d, 0x01, 0, // interface 0
	17, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0, 0, 0, 0, 0x13, 0x02, 0x08, 0, 0, // its vendor descriptor
	7, 5, 0x81, 3, 32, 0, 4, // IN
	7, 5, 0x02, 3, 32, 0, 8, // OUT
};

// A boot keyboard (interface 0) and a generic HID pad (interface 1, IN 0x82 only).
static const uint8_t s_generic_pad[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x95, 0x0c, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x09, 0x30,
	0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x25, 0xff, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0xc0,
};
static const uint8_t s_keyboard_desc[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0};
static const uint8_t s_hid_config[] = {
	9, 2, 59, 0, 2, 1, 0, 0xa0, 0x32, //
	9, 4, 0, 0, 1, 0x03, 0x01, 0x01, 0, // a boot keyboard: not a pad
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(s_keyboard_desc), 0, //
	7, 5, 0x81, 3, 8, 0, 10, //
	9, 4, 1, 0, 1, 0x03, 0x00, 0x00, 0, // the pad
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(s_generic_pad), 0, //
	7, 5, 0x82, 3, 8, 0, 10, //
	9, 4, 1, 1, 0, 0x03, 0x00, 0x00, 0, // an alternate setting of it: not looked at
};

static void Config()
{
	PadInterface x = ParseConfig(s_x360_config, sizeof(s_x360_config));
	CHECK(x.found && !x.hid && x.number == 0 && x.in_ep == 0x81 && x.out_ep == 0x02 && x.in_size == 32 && x.out_size == 32);
	PadInterface h = ParseConfig(s_hid_config, sizeof(s_hid_config));
	CHECK(h.found && h.hid && h.number == 1 && h.in_ep == 0x82 && h.out_ep == 0 && h.report_length == sizeof(s_generic_pad));
	CHECK(!ParseConfig(s_hid_config, sizeof(s_hid_config), 0).found); // asked for the keyboard: not a pad
	CHECK(ParseConfig(s_hid_config, sizeof(s_hid_config), 1).found);
	CHECK(!ParseConfig(s_hid_config, 20).found); // cut short
	const uint8_t bad[] = {9, 2, 9, 0, 1, 1, 0, 0, 0, 0, 4}; // a zero length: stops, does not loop
	CHECK(!ParseConfig(bad, sizeof(bad)).found);
	CHECK(!ParseConfig(nullptr, 0).found);
}

// ---------------------------------------------------------------- a made-up USB stack

struct FakeDevice
{
	uint16_t vid = 0, pid = 0;
	uint8_t cls = 0;
	std::vector<uint8_t> config;
	std::vector<uint8_t> report_desc;
	std::string product;
	bool driver_active = false;
	bool full_desc = true; // the kernel's copy of the configuration can be read
	bool hang_on_stop = false; // a stopped read never completes (a drain that does not end)
	bool gone = false; // unplugged while open
	std::deque<std::vector<uint8_t>> reports; // what the pad sends
	// What was done to it.
	int open_fd = -1;
	int opens = 0, closes = 0, uninits = 0, detaches = 0, stops = 0;
	std::vector<std::vector<uint8_t>> set_reports, writes;
	// The transfer table.
	FsEndpoint* eps = nullptr;
	int ep_max = 0;
	bool opened[8] = {}, running[8] = {};
	std::deque<int> done;
};

static std::map<std::string, FakeDevice> s_nodes;
static std::map<int, std::string> s_fds;
static int s_next_fd = 3;
static uint64_t s_now_ms = 1000;

static FakeDevice* ByFd(int fd)
{
	const auto it = s_fds.find(fd);
	return it == s_fds.end() ? nullptr : &s_nodes[it->second];
}

static int FakeOpen(const char* path, bool)
{
	const auto it = s_nodes.find(path);
	if (it == s_nodes.end())
		return -ENOENT;
	if (it->second.open_fd >= 0)
		return -EBUSY;
	const int fd = s_next_fd++;
	it->second.open_fd = fd;
	it->second.opens++;
	s_fds[fd] = path;
	return fd;
}

static int FakeClose(int fd)
{
	FakeDevice* d = ByFd(fd);
	if (!d)
		return -EBADF;
	d->open_fd = -1;
	d->closes++;
	d->eps = nullptr;
	for (bool& r : d->running)
		r = false;
	d->done.clear();
	s_fds.erase(fd);
	return 0;
}

static void CompleteReads(FakeDevice& d)
{
	for (int t = 1; t <= 2 && !d.reports.empty(); t++)
	{
		if (!d.running[t])
			continue;
		const std::vector<uint8_t>& r = d.reports.front();
		void* buf = d.eps[t].ppBuffer[0];
		const uint32_t n = std::min<uint32_t>(*d.eps[t].pLength, static_cast<uint32_t>(r.size()));
		std::memcpy(buf, r.data(), n);
		*d.eps[t].pLength = n;
		d.eps[t].aFrames = 1;
		d.eps[t].status = 0;
		d.running[t] = false;
		d.done.push_back(t);
		d.reports.pop_front();
	}
}

static int FakeIoctl(int fd, unsigned long request, void* arg)
{
	FakeDevice* d = ByFd(fd);
	if (!d)
		return -EBADF;
	if (request == DEVICE_INFO_PS5)
	{
		DeviceInfo* info = static_cast<DeviceInfo*>(arg);
		info->vendorNo = d->vid;
		info->productNo = d->pid;
		info->cls = d->cls;
		std::snprintf(info->product, sizeof(info->product), "%s", d->product.c_str());
		return 0;
	}
	if (request == GET_FULL_DESC)
	{
		if (!d->full_desc)
			return -EINVAL;
		GenDescriptor* g = static_cast<GenDescriptor*>(arg);
		const size_t n = std::min<size_t>(g->ugd_maxlen, d->config.size());
		std::memcpy(g->ugd_data, d->config.data(), n);
		g->ugd_actlen = static_cast<uint16_t>(n);
		return 0;
	}
	if (request == DO_REQUEST)
	{
		CtlRequest* r = static_cast<CtlRequest*>(arg);
		const uint16_t value = static_cast<uint16_t>(r->ucr_request.wValue[0] | (r->ucr_request.wValue[1] << 8));
		const uint16_t length = static_cast<uint16_t>(r->ucr_request.wLength[0] | (r->ucr_request.wLength[1] << 8));
		const std::vector<uint8_t>* from = nullptr;
		if (r->ucr_request.bmRequestType == 0x81 && r->ucr_request.bRequest == 6 && value == 0x2200)
			from = &d->report_desc;
		else if (r->ucr_request.bmRequestType == 0x80 && r->ucr_request.bRequest == 6 && value == 0x0200)
			from = &d->config;
		else if (r->ucr_request.bmRequestType == 0x21 && r->ucr_request.bRequest == 9)
		{
			const uint8_t* p = static_cast<const uint8_t*>(r->ucr_data);
			d->set_reports.emplace_back(p, p + length);
			r->ucr_actlen = length;
			return 0;
		}
		if (!from)
			return -EPIPE;
		const size_t n = std::min<size_t>(length, from->size());
		std::memcpy(r->ucr_data, from->data(), n);
		r->ucr_actlen = static_cast<uint16_t>(n);
		return 0;
	}
	if (request == IFACE_DRIVER_ACTIVE)
		return d->driver_active ? 0 : -ENXIO;
	if (request == IFACE_DRIVER_DETACH)
	{
		d->driver_active = false;
		d->detaches++;
		return 0;
	}
	if (request == FS_INIT)
	{
		const FsInit* init = static_cast<const FsInit*>(arg);
		d->eps = init->pEndpoints;
		d->ep_max = init->ep_index_max;
		return 0;
	}
	if (request == FS_OPEN)
	{
		const FsOpen* o = static_cast<const FsOpen*>(arg);
		if (o->ep_index >= d->ep_max)
			return -EINVAL;
		d->opened[o->ep_index] = true;
		return 0;
	}
	if (request == FS_UNINIT)
	{
		d->uninits++;
		d->eps = nullptr;
		return 0;
	}
	const uint8_t t = static_cast<const FsIndex*>(arg)->ep_index;
	if (request == FS_START)
	{
		if (d->gone)
			return -ENXIO;
		if (!d->opened[t] || d->running[t])
			return -EINVAL;
		d->running[t] = true;
		if (t == 3)
		{
			// A write: taken at once.
			const uint8_t* p = static_cast<const uint8_t*>(d->eps[t].ppBuffer[0]);
			d->writes.emplace_back(p, p + *d->eps[t].pLength);
			d->eps[t].status = 0;
			d->eps[t].aFrames = 1;
			d->running[t] = false;
			d->done.push_back(t);
		}
		return 0;
	}
	if (request == FS_STOP)
	{
		d->stops++;
		if (d->running[t] && !d->hang_on_stop)
		{
			d->eps[t].status = ERR_CANCELLED;
			d->eps[t].aFrames = 0;
			d->running[t] = false;
			d->done.push_back(t);
		}
		return 0;
	}
	if (request == FS_COMPLETE)
	{
		if (d->gone)
			return -ENXIO;
		CompleteReads(*d);
		if (d->done.empty())
			return -EBUSY;
		static_cast<FsIndex*>(arg)->ep_index = static_cast<uint8_t>(d->done.front());
		d->done.pop_front();
		return 0;
	}
	return -ENOTTY;
}

static void FakeSleep(unsigned us) { s_now_ms += (us + 999) / 1000; }
static uint64_t FakeNow() { return s_now_ms; }

static SysOps FakeOps()
{
	SysOps ops;
	ops.open = FakeOpen;
	ops.close = FakeClose;
	ops.ioctl = FakeIoctl;
	ops.sleep_us = FakeSleep;
	ops.now_ms = FakeNow;
	return ops;
}

static std::vector<std::string> s_log;
static void Log(const char* line) { s_log.emplace_back(line); }

static void Reset()
{
	s_nodes.clear();
	s_fds.clear();
	s_log.clear();
	s_nodes["/dev/ugen0.1"].cls = 0x09; // the root hubs
	s_nodes["/dev/ugen1.1"].cls = 0x09;
}

static FakeDevice& AddX360(const char* node)
{
	FakeDevice& d = s_nodes[node];
	d.vid = 0x045e;
	d.pid = 0x028e;
	d.product = "Controller";
	d.config.assign(s_x360_config, s_x360_config + sizeof(s_x360_config));
	return d;
}

static FakeDevice& AddHid(const char* node)
{
	FakeDevice& d = s_nodes[node];
	d.vid = 0x0079;
	d.pid = 0x0006;
	d.product = "Generic USB Joystick";
	d.config.assign(s_hid_config, s_hid_config + sizeof(s_hid_config));
	d.report_desc.assign(s_generic_pad, s_generic_pad + sizeof(s_generic_pad));
	d.driver_active = true;
	return d;
}

// ---------------------------------------------------------------- the tests

static void StartAndFind()
{
	Reset();
	{
		Transport none(SysOps{}, Log);
		CHECK(!none.Start()); // no system calls: no ugen
	}
	s_nodes.clear();
	{
		Transport t(FakeOps(), Log);
		CHECK(!t.Start()); // no node opens
	}
	Reset();
	AddHid("/dev/ugen1.3");
	AddX360("/dev/ugen0.4");
	// The console's Bluetooth chip, and its own controller: never taken.
	AddX360("/dev/ugen0.2");
	FakeDevice& ds = AddHid("/dev/ugen0.5");
	ds.vid = 0x054c;
	ds.pid = 0x0ce6;
	// A keyboard alone.
	FakeDevice& kb = AddHid("/dev/ugen1.2");
	kb.config.resize(9 + 9 + 9 + 7);
	kb.config[2] = static_cast<uint8_t>(kb.config.size());
	kb.report_desc.assign(s_keyboard_desc, s_keyboard_desc + sizeof(s_keyboard_desc));

	Transport t(FakeOps(), Log);
	CHECK(t.Start());
	std::vector<PS5UsbPads::DeviceInfo> found;
	CHECK(t.Enumerate(&found));
	CHECK(found.size() == 2);
	bool hid = false, x360 = false;
	for (const auto& f : found)
	{
		if (f.id == ((1u << 8) | 3))
			hid = f.interface_class == 3 && f.interface_number == 1 && f.report_descriptor.size() == sizeof(s_generic_pad) &&
				  f.name == "Generic USB Joystick";
		if (f.id == 4)
			x360 = f.interface_class == 0xff && f.interface_subclass == 0x5d && f.report_descriptor.empty();
	}
	CHECK(hid && x360);
	CHECK(s_nodes["/dev/ugen0.2"].opens == 0);
	// Every node was closed again after being looked at.
	for (const auto& [path, d] : s_nodes)
		CHECK(d.open_fd < 0);
	// Looked at again: what was learnt is kept (no new report descriptor request needed).
	const int before = s_nodes["/dev/ugen1.3"].opens;
	CHECK(t.Enumerate(&found) && found.size() == 2);
	CHECK(s_nodes["/dev/ugen1.3"].opens == before + 1);
	// Unplugged: gone from the list.
	s_nodes.erase("/dev/ugen0.4");
	CHECK(t.Enumerate(&found) && found.size() == 1);
	// No configuration in the kernel's copy: GET_DESCRIPTOR on the bus instead.
	AddX360("/dev/ugen1.4").full_desc = false;
	CHECK(t.Enumerate(&found) && found.size() == 2);
}

static void ReadWriteClose()
{
	Reset();
	FakeDevice& pad = AddHid("/dev/ugen1.3");
	Transport t(FakeOps(), Log);
	CHECK(t.Start());
	std::vector<PS5UsbPads::DeviceInfo> found;
	CHECK(t.Enumerate(&found) && found.size() == 1);
	const int h = t.Open(found[0]);
	CHECK(h > 0);
	CHECK(pad.detaches == 1 && !pad.driver_active); // the console's driver let go first
	CHECK(pad.running[1] && pad.running[2] && !pad.opened[3]); // two reads armed, no OUT endpoint
	// It cannot be opened twice, and the list still has it while it is open.
	CHECK(t.Open(found[0]) < 0);
	CHECK(t.Enumerate(&found) && found.size() == 1);

	uint8_t buf[64];
	CHECK(t.Read(h, buf, sizeof(buf), 0) == 0); // nothing yet, and no wait
	const uint64_t t0 = s_now_ms;
	CHECK(t.Read(h, buf, sizeof(buf), 20) == 0);
	CHECK(s_now_ms - t0 >= 20 && s_now_ms - t0 < 25); // waited as long as asked, no longer
	pad.reports.push_back({1, 0x01, 0x0f, 10, 20, 30, 40});
	pad.reports.push_back({1, 0x02, 0x0f, 11, 21, 31, 41});
	pad.reports.push_back({1, 0x04, 0x0f, 12, 22, 32, 42});
	CHECK(t.Read(h, buf, sizeof(buf), 0) == 7 && buf[1] == 0x01);
	CHECK(t.Read(h, buf, sizeof(buf), 0) == 7 && buf[1] == 0x02);
	CHECK(t.Read(h, buf, sizeof(buf), 0) == 7 && buf[1] == 0x04);
	CHECK(t.Read(h, buf, sizeof(buf), 0) == 0);
	CHECK(pad.running[1] && pad.running[2]); // armed again each time

	// Output with no OUT endpoint: a SET_REPORT.
	const uint8_t out[] = {0x01, 0xaa};
	CHECK(t.Write(h, out, sizeof(out)) && pad.set_reports.size() == 1 && pad.set_reports[0][1] == 0xaa);
	uint8_t feature[4] = {};
	CHECK(t.Control(h, 0xa1, 0x01, 0x0300, 1, feature, 4) < 0); // what the pad refuses (a stall)

	// Closed: the reads stopped, the table taken down, the node closed.
	t.Close(h);
	CHECK(pad.stops == 2 && pad.uninits == 1 && pad.closes >= 1 && pad.open_fd < 0 && t.Stuck() == 0);
	CHECK(t.Read(h, buf, sizeof(buf), 0) < 0);

	// The Xbox 360 pad: its OUT endpoint takes the rumble.
	FakeDevice& x = AddX360("/dev/ugen0.4");
	CHECK(t.Enumerate(&found) && found.size() == 2);
	const PS5UsbPads::DeviceInfo* xi = nullptr;
	for (const auto& f : found)
		xi = f.id == 4 ? &f : xi;
	CHECK(xi != nullptr);
	const int hx = xi ? t.Open(*xi) : -1;
	CHECK(hx > 0 && x.opened[3] && x.detaches == 0);
	const uint8_t rumble[8] = {0x00, 0x08, 0x00, 0x80, 0x40, 0, 0, 0};
	CHECK(t.Write(hx, rumble, sizeof(rumble)) && x.writes.size() == 1 && x.writes[0].size() == 8 && x.writes[0][3] == 0x80);
	// Unplugged mid-read: what came first is still read, then the device is gone.
	x.reports.push_back(std::vector<uint8_t>(20, 0));
	CHECK(t.Read(hx, buf, sizeof(buf), 0) == 20);
	x.gone = true;
	CHECK(t.Read(hx, buf, sizeof(buf), 0) < 0);
	CHECK(t.Enumerate(&found)); // no longer listed as a pad once gone
	bool listed = false;
	for (const auto& f : found)
		listed = listed || f.id == 4;
	CHECK(!listed);
	t.Close(hx);
	CHECK(x.open_fd < 0 && x.uninits == 0); // a device that is gone is closed without draining
}

static void BoundedTeardown()
{
	Reset();
	FakeDevice& pad = AddHid("/dev/ugen1.3");
	pad.hang_on_stop = true;
	Transport t(FakeOps(), Log);
	CHECK(t.Start());
	std::vector<PS5UsbPads::DeviceInfo> found;
	CHECK(t.Enumerate(&found) && found.size() == 1);
	const int h = t.Open(found[0]);
	CHECK(h > 0);
	const uint64_t t0 = s_now_ms;
	t.Close(h);
	// Half a second, then the node is left open rather than drained.
	CHECK(s_now_ms - t0 >= Transport::STOP_WAIT_MS && s_now_ms - t0 < Transport::STOP_WAIT_MS + 10);
	CHECK(pad.open_fd >= 0 && pad.uninits == 0 && t.Stuck() == 1);
	bool said = false;
	for (const std::string& line : s_log)
		said = said || line.find("left open") != std::string::npos;
	CHECK(said);

	// Four that hang: stopping the transport takes at most a second and a half in all.
	Reset();
	Transport all(FakeOps(), Log);
	for (int i = 0; i < 4; i++)
	{
		char node[48];
		std::snprintf(node, sizeof(node), "/dev/ugen1.%d", 3 + i);
		AddHid(node).hang_on_stop = true;
	}
	CHECK(all.Start());
	CHECK(all.Enumerate(&found) && found.size() == 4);
	for (const auto& f : found)
		CHECK(all.Open(f) > 0);
	const uint64_t t1 = s_now_ms;
	all.Stop();
	CHECK(s_now_ms - t1 <= Transport::STOP_ALL_WAIT_MS + 10);
	CHECK(all.Stuck() == 4);
}

static void WithTheManager()
{
	Reset();
	FakeDevice& x = AddX360("/dev/ugen0.4");
	auto owned = std::make_unique<Transport>(FakeOps(), Log);
	PS5UsbPads::Manager m;
	CHECK(m.Start(std::move(owned), false));
	m.PollOnce(0.0, 0);
	PS5UsbPads::Slot s0 = m.Get(0);
	CHECK(s0.connected && s0.protocol == PS5UsbPadLogic::Protocol::XInput);
	std::vector<uint8_t> r(20, 0);
	r[1] = 0x14;
	r[3] = 0x10; // A
	x.reports.push_back(r);
	m.PollOnce(0.1, 0);
	CHECK(m.Get(0).state.buttons == (1u << PS5UsbPadLogic::SDL_SOUTH));
	// The rumble goes out through the OUT endpoint.
	m.SetRumble(0, 200, 100);
	m.PollOnce(0.2, 0);
	CHECK(!x.writes.empty() && x.writes.back().size() == 8 && x.writes.back()[3] == 200 && x.writes.back()[4] == 100);
	// Unplugged: the slot is let go.
	x.gone = true;
	m.PollOnce(0.3, 0);
	m.PollOnce(1.5, 0);
	CHECK(!m.Get(0).connected);
	m.Stop();
}

int main()
{
	Config();
	StartAndFind();
	ReadWriteClose();
	BoundedTeardown();
	WithTheManager();
	std::printf("ugen: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
