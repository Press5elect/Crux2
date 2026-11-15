// Crux2 - USB pads through the console's ugen nodes (/dev/ugenB.A), for when libSceUsbd cannot be
// used: the transport itself, with no console in it (ps5/tests/usbpad checks it over a made-up
// device node). The console's system calls are ps5_ugen.cpp's.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The PS5 is FreeBSD underneath: every USB device has a ugen node with FreeBSD's USB_FS_* transfer
// ioctls and USB_DO_REQUEST (sys/dev/usb/usb_ioctl.h). Porpoise reads a DolphinBar through them from
// a title (its src/porpoise_usbhid.cpp and src/porpoise_ugen.hpp, Copyright (C) 2026 Ruben,
// GPL-3.0-or-later; docs/DOLPHINBAR.md "How it works"); this is the same way in, for the USB pads
// of ps5_usbpad.cpp, behind the same Transport as libSceUsbd's (ps5_usbpad_console.cpp), and tried
// only when that one cannot be loaded or a pad cannot be opened through it. The reports are decoded
// as they always are (ps5_usbpad_logic.h); this only moves bytes.
//
// One node per device, opened once (a ugen node is opened by one at a time), with a small transfer
// table: two interrupt IN reads kept armed (transfers 1 and 2) and the interrupt OUT (3) when the
// interface has one; output without one goes as a HID SET_REPORT. A console's own HID driver that
// holds the interface is detached first. What a reading thread does not take waits in a short queue.
//
// The teardown is bounded, as Porpoise's is: the kernel drains running transfers when a node's
// table is taken down or the node is closed, a pad's read only ends with a report, and a drain that
// never ends would leave the process impossible to close. So every transfer is stopped first and
// waited for at most half a second; one that still runs leaves its node open (said in the log)
// rather than hang. Stopping the whole transport waits at most a second and a half in all.
//
// Not run on a console when it was written: whether a title may open a ugen node, and whether the
// numbers below are the console's, are what its first run shows (the boot log: "usb pads: ugen ...").

#pragma once

#include "ps5_usbpad.h"
#include "ps5_usbpad_logic.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace PS5Ugen
{
	// From Porpoise pr37 src/porpoise_ugen.hpp (GPL-3.0-or-later): FreeBSD's ugen ioctls and
	// structures as the console has them, spelt out so the PC tests can build them.
	constexpr unsigned long IOC_OUT = 0x40000000UL, IOC_IN = 0x80000000UL, IOC_INOUT = IOC_IN | IOC_OUT;
	constexpr unsigned long Ioc(unsigned long inout, unsigned long num, unsigned long len)
	{
		return inout | ((len & 0x1fffUL) << 16) | (0x55UL /* 'U' */ << 8) | num;
	}

	struct DeviceRequest // usb_device_request
	{
		uint8_t bmRequestType, bRequest, wValue[2], wIndex[2], wLength[2];
	};
	struct alignas(8) CtlRequest // usb_ctl_request
	{
		void* ucr_data;
		uint16_t ucr_flags, ucr_actlen;
		uint8_t ucr_addr;
		DeviceRequest ucr_request;
	};
	struct alignas(8) FsEndpoint // usb_fs_endpoint
	{
		void** ppBuffer;
		uint32_t* pLength;
		uint32_t nFrames, aFrames;
		uint16_t flags, timeout, isoc_time_complete;
		int status;
	};
	struct alignas(8) FsInit // usb_fs_init
	{
		FsEndpoint* pEndpoints;
		uint8_t ep_index_max;
	};
	struct alignas(4) FsOpen // usb_fs_open
	{
		uint32_t max_bufsize, max_frames;
		uint16_t max_packet_length;
		uint8_t dev_index, ep_index, ep_no;
	};
	struct FsIndex // usb_fs_start, _stop, _complete, _close, _uninit
	{
		uint8_t ep_index;
	};
	struct alignas(8) GenDescriptor // usb_gen_descriptor
	{
		void* ugd_data;
		uint16_t ugd_lang_id, ugd_maxlen, ugd_actlen, ugd_offset;
		uint8_t ugd_config_index, ugd_string_index, ugd_iface_index, ugd_altif_index, ugd_endpt_index, ugd_report_type;
		uint8_t reserved[8];
	};
	/// usb_device_info: the fields read. The console's is 8 bytes longer than FreeBSD's 0x170 (BlueV
	/// reads it with 0xc1785504); that is tried first, then FreeBSD's USB_GET_DEVICEINFO.
	struct DeviceInfo
	{
		uint16_t productNo, vendorNo, releaseNo, power;
		uint8_t bus, addr, index, cls, subclass, protocol, config_no, config_index, speed, mode, nports, hubaddr, hubindex,
			hubport, power_mode, suspended;
		uint8_t reserved[16];
		char product[128], vendor[128], serial[64], release[8];
		uint8_t ps5_more[8];
	};
	static_assert(sizeof(DeviceRequest) == 8 && sizeof(CtlRequest) == 24 && sizeof(FsEndpoint) == 40 && sizeof(FsInit) == 16 &&
				  sizeof(FsOpen) == 16 && sizeof(GenDescriptor) == 32 && sizeof(DeviceInfo) == 0x178);

	constexpr unsigned long FS_START = Ioc(IOC_IN, 192, 1), FS_STOP = Ioc(IOC_IN, 193, 1), FS_COMPLETE = Ioc(IOC_OUT, 194, 1),
							FS_INIT = Ioc(IOC_IN, 195, sizeof(FsInit)), FS_UNINIT = Ioc(IOC_IN, 196, 1),
							FS_OPEN = Ioc(IOC_INOUT, 197, sizeof(FsOpen)), FS_CLOSE = Ioc(IOC_IN, 198, 1),
							DO_REQUEST = Ioc(IOC_INOUT, 111, sizeof(CtlRequest)), GET_FULL_DESC = Ioc(IOC_INOUT, 109, sizeof(GenDescriptor)),
							DEVICE_INFO_PS5 = Ioc(IOC_INOUT, 4, 0x178), DEVICE_INFO = Ioc(IOC_OUT, 112, 0x170),
							IFACE_DRIVER_ACTIVE = Ioc(IOC_IN, 124, sizeof(int)), IFACE_DRIVER_DETACH = Ioc(IOC_IN, 125, sizeof(int));
	static_assert(DEVICE_INFO_PS5 == 0xc1785504UL, "BlueV's number");

	constexpr uint16_t FLAG_SINGLE_SHORT_OK = 0x0001, FLAG_FORCE_SHORT = 0x0004, FLAG_CLEAR_STALL = 0x0008;
	constexpr int ERR_CANCELLED = 5; // a transfer's status once it was stopped

	/// The system calls, as the console has them (ps5_ugen.cpp) or a test's. Each answers 0 (or a
	/// file descriptor) or a negative errno.
	struct SysOps
	{
		int (*open)(const char* path, bool nonblocking) = nullptr;
		int (*close)(int fd) = nullptr;
		int (*ioctl)(int fd, unsigned long request, void* arg) = nullptr;
		void (*sleep_us)(unsigned us) = nullptr;
		uint64_t (*now_ms)() = nullptr;
	};

	/// The console's (ps5_ugen.cpp); off the console, none (every call says no).
	SysOps ConsoleSysOps();

	// ---------------------------------------------------------------- descriptors

	/// A pad's interface in a configuration descriptor: the first of an Xbox 360 or HID interface
	/// (not a boot keyboard or mouse) with an interrupt IN endpoint.
	struct PadInterface
	{
		bool found = false;
		bool hid = false;
		uint8_t number = 0, cls = 0, subclass = 0, protocol = 0;
		uint8_t in_ep = 0, out_ep = 0;
		uint16_t in_size = 0, out_size = 0;
		uint16_t report_length = 0; // the HID descriptor's report descriptor length (0: not said)
	};

	/// The first pad interface of a whole configuration descriptor (as GET_FULL_DESC or GET_DESCRIPTOR
	/// give it), with its first interrupt IN and OUT endpoints; want_number asks for one interface by
	/// its number (-1: the first pad there is).
	inline PadInterface ParseConfig(const uint8_t* d, size_t len, int want_number = -1)
	{
		PadInterface cur, best;
		bool in_candidate = false;
		auto finish = [&] {
			if (in_candidate && cur.in_ep != 0 && !best.found)
			{
				best = cur;
				best.found = true;
			}
			in_candidate = false;
		};
		for (size_t off = 0; d && off + 2 <= len;)
		{
			const uint8_t blen = d[off], type = d[off + 1];
			if (blen < 2 || off + blen > len)
				break;
			if (type == 4 && blen >= 9) // an interface
			{
				finish();
				cur = PadInterface();
				cur.number = d[off + 2];
				const uint8_t alt = d[off + 3];
				cur.cls = d[off + 5];
				cur.subclass = d[off + 6];
				cur.protocol = d[off + 7];
				const bool xinput = PS5UsbPadLogic::IsXInputInterface(cur.cls, cur.subclass, cur.protocol);
				cur.hid = cur.cls == 0x03 && !(cur.subclass == 0x01 && (cur.protocol == 0x01 || cur.protocol == 0x02));
				in_candidate = alt == 0 && (xinput || cur.hid) && (want_number < 0 || cur.number == want_number);
			}
			else if (type == 0x21 && blen >= 9 && in_candidate && d[off + 6] == 0x22) // HID: the report descriptor's length
			{
				cur.report_length = static_cast<uint16_t>(d[off + 7] | (d[off + 8] << 8));
			}
			else if (type == 5 && blen >= 7 && in_candidate) // an endpoint
			{
				const uint8_t addr = d[off + 2], attr = d[off + 3];
				const uint16_t size = static_cast<uint16_t>(d[off + 4] | (d[off + 5] << 8)) & 0x7ff;
				if ((attr & 3) == 3) // interrupt
				{
					if ((addr & 0x80) && !cur.in_ep)
					{
						cur.in_ep = addr;
						cur.in_size = size;
					}
					else if (!(addr & 0x80) && !cur.out_ep)
					{
						cur.out_ep = addr;
						cur.out_size = size;
					}
				}
			}
			off += blen;
		}
		finish();
		return best;
	}

	// ---------------------------------------------------------------- the transport

	class Transport final : public PS5UsbPads::Transport
	{
	public:
		static constexpr int BUSES = 4; // ugen0 .. ugen3
		static constexpr int ADDRESSES = 32; // .1 is each bus's root hub
		static constexpr int TRANSFERS = 4; // 0 unused, 1 and 2 the reads, 3 the write
		static constexpr uint32_t BUF = 64;
		static constexpr int QUEUE = 16;
		static constexpr uint64_t STOP_WAIT_MS = 500;
		static constexpr uint64_t STOP_ALL_WAIT_MS = 1500;
		static constexpr int HANDLE_BASE = 1;

		explicit Transport(SysOps ops, void (*log)(const char* line) = nullptr) : m_ops(ops), m_log(log) {}
		~Transport() override { Stop(); }

		/// Whether nodes can be opened at all: a bus's root hub (ugenB.1) is always there.
		bool Start() override
		{
			if (!m_ops.open || !m_ops.ioctl || !m_ops.close || !m_ops.now_ms || !m_ops.sleep_us)
				return false;
			int last = -ENOENT;
			for (int bus = 0; bus < BUSES; bus++)
			{
				const int fd = m_ops.open(NodePath(bus, 1).c_str(), true);
				if (fd >= 0)
				{
					m_ops.close(fd);
					Log("usb pads: ugen: /dev/ugen%d.1 opens: reading USB pads through ugen", bus);
					m_started = true;
					return true;
				}
				last = fd;
			}
			Log("usb pads: ugen: no ugen node could be opened (errno %d): no USB pads", -last);
			return false;
		}

		void Stop() override
		{
			if (!m_started)
				return;
			const uint64_t until = m_ops.now_ms() + STOP_ALL_WAIT_MS;
			for (auto& [h, dev] : m_devices)
			{
				const uint64_t now = m_ops.now_ms();
				CloseDevice(*dev, now < until ? std::min<uint64_t>(STOP_WAIT_MS, until - now) : 0);
				if (dev->fd >= 0)
					m_stuck.push_back(std::move(dev)); // its buffers may still be written: kept
			}
			m_devices.clear();
			m_started = false;
		}

		bool Enumerate(std::vector<PS5UsbPads::DeviceInfo>* devices) override
		{
			devices->clear();
			if (!m_started)
				return false;
			std::map<uint64_t, Known> seen;
			for (int bus = 0; bus < BUSES; bus++)
			{
				for (int addr = 2; addr < ADDRESSES; addr++)
				{
					if (bus == 0 && addr == 2)
						continue; // ugen0.2: the console's own Bluetooth chip, which its controllers use
					const uint64_t id = (static_cast<uint64_t>(bus) << 8) | static_cast<uint64_t>(addr);
					// One of ours is open: it cannot be opened again, and it is there.
					if (const Device* d = DeviceById(id))
					{
						if (const auto k = m_known.find(id); k != m_known.end())
						{
							seen.emplace(id, k->second);
							if (k->second.pad && !d->dead)
								devices->push_back(k->second.info);
						}
						continue;
					}
					const int fd = m_ops.open(NodePath(bus, addr).c_str(), true);
					if (fd < 0)
					{
						const auto k = m_known.find(id);
						if (fd == -EBUSY && k != m_known.end())
							seen.emplace(id, k->second); // someone else has it now: still plugged in
						continue;
					}
					DeviceInfo info{};
					if (m_ops.ioctl(fd, DEVICE_INFO_PS5, &info) != 0 && m_ops.ioctl(fd, DEVICE_INFO, &info) != 0)
					{
						m_ops.close(fd);
						continue;
					}
					if (const auto k = m_known.find(id);
						k != m_known.end() && k->second.vendor == info.vendorNo && k->second.product == info.productNo)
					{
						seen.emplace(id, k->second); // learnt before, and the same device: no bus traffic
						m_ops.close(fd);
						if (k->second.pad)
							devices->push_back(k->second.info);
						continue;
					}
					Known k = Learn(fd, id, info);
					m_ops.close(fd);
					if (k.pad)
						devices->push_back(k.info);
					seen.emplace(id, std::move(k));
				}
			}
			m_known = std::move(seen);
			return true;
		}

		int Open(const PS5UsbPads::DeviceInfo& device) override
		{
			if (!m_started && !Start())
				return -1;
			const int bus = static_cast<int>(device.id >> 8), addr = static_cast<int>(device.id & 0xff);
			if (DeviceById(device.id))
				return -1; // open already
			auto d = std::make_unique<Device>();
			d->id = device.id;
			d->node = NodePath(bus, addr);
			d->fd = m_ops.open(d->node.c_str(), false);
			if (d->fd < 0)
			{
				Log("usb pads: ugen: open %s: errno %d", d->node.c_str(), -d->fd);
				return -1;
			}
			// The interface and its endpoints, from the kernel's copy of the configuration.
			std::vector<uint8_t> config = ReadConfig(d->fd);
			d->itf = ParseConfig(config.data(), config.size(), device.interface_number);
			if (!d->itf.found)
			{
				Log("usb pads: ugen: %s: interface %u not found", d->node.c_str(), device.interface_number);
				m_ops.close(d->fd);
				return -1;
			}
			for (int t = 0; t < TRANSFERS; t++)
			{
				d->buf_ptr[t] = d->buf[t];
				d->buf_len[t] = BUF;
				d->eps[t].ppBuffer = &d->buf_ptr[t];
				d->eps[t].pLength = &d->buf_len[t];
				d->eps[t].nFrames = 1;
			}
			FsInit init{};
			init.pEndpoints = d->eps;
			init.ep_index_max = TRANSFERS;
			if (const int r = m_ops.ioctl(d->fd, FS_INIT, &init); r != 0)
			{
				Log("usb pads: ugen: %s: USB_FS_INIT errno %d", d->node.c_str(), -r);
				m_ops.close(d->fd);
				return -1;
			}
			// A console's driver that holds the interface lets go of it first.
			int number = d->itf.number;
			if (m_ops.ioctl(d->fd, IFACE_DRIVER_ACTIVE, &number) == 0)
			{
				number = d->itf.number;
				const int r = m_ops.ioctl(d->fd, IFACE_DRIVER_DETACH, &number);
				Log("usb pads: ugen: %s: the console's driver had interface %u: %s (errno %d)", d->node.c_str(), d->itf.number,
					r == 0 ? "detached" : "not detached", -r);
			}
			bool ok = OpenSlot(*d, 1, d->itf.in_ep, d->itf.in_size) && OpenSlot(*d, 2, d->itf.in_ep, d->itf.in_size);
			if (ok && d->itf.out_ep)
				ok = OpenSlot(*d, 3, d->itf.out_ep, d->itf.out_size);
			if (!ok)
			{
				FsIndex u{0};
				m_ops.ioctl(d->fd, FS_UNINIT, &u);
				m_ops.close(d->fd);
				return -1;
			}
			ArmReads(*d);
			Log("usb pads: ugen: %s opened (%04x:%04x, interface %u, in %#x out %#x)", d->node.c_str(), device.vendor, device.product,
				d->itf.number, d->itf.in_ep, d->itf.out_ep);
			const int handle = m_next_handle++;
			m_devices.emplace(handle, std::move(d));
			return handle;
		}

		int Read(int handle, uint8_t* buffer, uint32_t size, uint32_t timeout_ms) override
		{
			Device* d = Find(handle);
			if (!d)
				return -1;
			const uint64_t until = m_ops.now_ms() + timeout_ms;
			for (;;)
			{
				Reap(*d);
				ArmReads(*d);
				if (d->count > 0)
				{
					const Report& r = d->queue[d->head];
					const uint32_t n = std::min(size, r.len);
					std::memcpy(buffer, r.data, n);
					d->head = (d->head + 1) % QUEUE;
					d->count--;
					return static_cast<int>(n);
				}
				if (d->dead)
					return -1;
				if (m_ops.now_ms() >= until)
					return 0;
				m_ops.sleep_us(1000);
			}
		}

		bool Write(int handle, const uint8_t* data, uint32_t size) override
		{
			Device* d = Find(handle);
			if (!d || d->dead || !data || size == 0 || size > BUF)
				return false;
			if (!d->itf.out_ep)
			{
				// No interrupt OUT: a HID SET_REPORT (Output, the report's id) to the interface.
				uint8_t copy[BUF];
				std::memcpy(copy, data, size);
				return Request(d->fd, 0x21, 0x09, static_cast<uint16_t>(0x0200 | data[0]), d->itf.number, copy,
						   static_cast<uint16_t>(size)) >= 0;
			}
			constexpr int t = 3;
			const uint64_t until = m_ops.now_ms() + 100;
			while (d->pending[t] && !d->dead && m_ops.now_ms() < until)
			{
				Reap(*d);
				if (d->pending[t])
					m_ops.sleep_us(500);
			}
			if (d->pending[t] || d->dead)
				return false; // the last one has not gone: this one is dropped (the next rumble is sent again)
			std::memcpy(d->buf[t], data, size);
			if (!StartTransfer(*d, t, size, FLAG_FORCE_SHORT, 50))
				return false;
			// Waited for briefly: the result of a write that does not end in time is collected later.
			while (d->pending[t] && !d->dead && m_ops.now_ms() < until)
			{
				Reap(*d);
				if (d->pending[t])
					m_ops.sleep_us(500);
			}
			return !d->pending[t] && d->eps[t].status == 0;
		}

		int Control(int handle, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data,
			uint16_t length) override
		{
			Device* d = Find(handle);
			if (!d || d->dead)
				return -1;
			return Request(d->fd, request_type, request, value, index, data, length);
		}

		void Close(int handle) override
		{
			const auto it = m_devices.find(handle);
			if (it == m_devices.end())
				return;
			CloseDevice(*it->second, STOP_WAIT_MS);
			if (it->second->fd >= 0)
				m_stuck.push_back(std::move(it->second));
			m_devices.erase(it);
		}

		/// Nodes left open because their transfers would not stop (for the tests and the log).
		size_t Stuck() const { return m_stuck.size(); }

	private:
		struct Known
		{
			uint16_t vendor = 0, product = 0;
			bool pad = false;
			PS5UsbPads::DeviceInfo info;
		};
		struct Report
		{
			uint8_t data[BUF];
			uint32_t len;
		};
		/// What the kernel reads and writes: never moved while the node is open.
		struct Device
		{
			uint64_t id = 0;
			std::string node;
			int fd = -1;
			bool dead = false;
			PadInterface itf;
			FsEndpoint eps[TRANSFERS] = {};
			void* buf_ptr[TRANSFERS] = {};
			uint32_t buf_len[TRANSFERS] = {};
			uint8_t buf[TRANSFERS][BUF] = {};
			bool opened[TRANSFERS] = {};
			bool pending[TRANSFERS] = {};
			bool clear_stall = false;
			unsigned errors = 0;
			Report queue[QUEUE];
			int head = 0, count = 0;
		};

		static std::string NodePath(int bus, int addr)
		{
			char path[48];
			std::snprintf(path, sizeof(path), "/dev/ugen%d.%d", bus, addr);
			return path;
		}

		template <typename... Args>
		void Log(const char* format, Args... args)
		{
			if (!m_log)
				return;
			char line[256];
			std::snprintf(line, sizeof(line), format, args...);
			m_log(line);
		}

		Device* Find(int handle)
		{
			const auto it = m_devices.find(handle);
			return it == m_devices.end() ? nullptr : it->second.get();
		}

		const Device* DeviceById(uint64_t id) const
		{
			for (const auto& [h, d] : m_devices)
			{
				if (d->id == id)
					return d.get();
			}
			return nullptr;
		}

		/// A request on the default pipe: the bytes moved, or a negative errno.
		int Request(int fd, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length)
		{
			CtlRequest r{};
			r.ucr_data = data;
			r.ucr_flags = 0x0001; // USB_SHORT_XFER_OK
			r.ucr_request.bmRequestType = type;
			r.ucr_request.bRequest = request;
			r.ucr_request.wValue[0] = static_cast<uint8_t>(value);
			r.ucr_request.wValue[1] = static_cast<uint8_t>(value >> 8);
			r.ucr_request.wIndex[0] = static_cast<uint8_t>(index);
			r.ucr_request.wIndex[1] = static_cast<uint8_t>(index >> 8);
			r.ucr_request.wLength[0] = static_cast<uint8_t>(length);
			r.ucr_request.wLength[1] = static_cast<uint8_t>(length >> 8);
			const int rc = m_ops.ioctl(fd, DO_REQUEST, &r);
			return rc != 0 ? rc : r.ucr_actlen;
		}

		/// The configuration in use: the kernel's copy (no bus traffic), else GET_DESCRIPTOR.
		std::vector<uint8_t> ReadConfig(int fd)
		{
			std::vector<uint8_t> cfg(512);
			GenDescriptor g{};
			g.ugd_data = cfg.data();
			g.ugd_maxlen = static_cast<uint16_t>(cfg.size());
			g.ugd_config_index = 0xff; // USB_UNCONFIG_INDEX: the current configuration
			if (m_ops.ioctl(fd, GET_FULL_DESC, &g) == 0 && g.ugd_actlen >= 9)
			{
				cfg.resize(std::min<size_t>(g.ugd_actlen, cfg.size()));
				return cfg;
			}
			if (Request(fd, 0x80, 0x06, 0x0200, 0, cfg.data(), 9) >= 9)
			{
				const uint16_t total = std::min<uint16_t>(static_cast<uint16_t>(cfg[2] | (cfg[3] << 8)), 512);
				const int got = Request(fd, 0x80, 0x06, 0x0200, 0, cfg.data(), total);
				if (got >= 9)
				{
					cfg.resize(static_cast<size_t>(got));
					return cfg;
				}
			}
			return {};
		}

		/// A device seen for the first time: whether it is a pad, its interface, and for a HID one its
		/// report descriptor (GET_DESCRIPTOR on the default pipe), which must describe a pad.
		Known Learn(int fd, uint64_t id, const DeviceInfo& info)
		{
			Known k;
			k.vendor = info.vendorNo;
			k.product = info.productNo;
			if (info.cls == 0x09)
				return k; // a hub
			if (k.vendor == 0x054c && !PS5UsbPadLogic::IsDualShock3(k.vendor, k.product))
				return k; // the console's own controllers are its pad service's
			const std::vector<uint8_t> config = ReadConfig(fd);
			const PadInterface itf = ParseConfig(config.data(), config.size());
			if (!itf.found)
				return k;
			k.info.id = id;
			k.info.vendor = k.vendor;
			k.info.product = k.product;
			k.info.interface_class = itf.cls;
			k.info.interface_subclass = itf.subclass;
			k.info.interface_protocol = itf.protocol;
			k.info.interface_number = itf.number;
			char product[sizeof(info.product) + 1] = {};
			std::memcpy(product, info.product, sizeof(info.product));
			k.info.name = product;
			if (itf.hid && !PS5UsbPadLogic::IsDualShock3(k.vendor, k.product))
			{
				const uint16_t length = itf.report_length ? itf.report_length : 512;
				std::vector<uint8_t> desc(length);
				const int got = Request(fd, 0x81, 0x06, 0x2200, itf.number, desc.data(), length);
				if (got <= 0)
				{
					Log("usb pads: ugen: %04x:%04x: the HID report descriptor could not be read: %d", k.vendor, k.product, got);
					return k;
				}
				desc.resize(static_cast<size_t>(got));
				if (!PS5UsbPadLogic::ParseReportDescriptor(desc.data(), desc.size()).is_gamepad)
					return k; // a keyboard, a mouse, a headset's buttons...
				k.info.report_descriptor = std::move(desc);
			}
			k.pad = true;
			Log("usb pads: ugen: found %04x:%04x at ugen%u.%u, interface %u (class %02x/%02x/%02x)", k.vendor, k.product,
				static_cast<unsigned>(id >> 8), static_cast<unsigned>(id & 0xff), itf.number, itf.cls, itf.subclass, itf.protocol);
			return k;
		}

		bool OpenSlot(Device& d, int t, uint8_t ep, uint16_t size)
		{
			FsOpen o{};
			o.max_bufsize = std::min<uint32_t>(size ? size : BUF, BUF);
			o.max_frames = 1;
			o.ep_index = static_cast<uint8_t>(t);
			o.ep_no = ep;
			if (const int r = m_ops.ioctl(d.fd, FS_OPEN, &o); r != 0)
			{
				Log("usb pads: ugen: %s: USB_FS_OPEN endpoint %#x errno %d", d.node.c_str(), ep, -r);
				return false;
			}
			d.opened[t] = true;
			return true;
		}

		void MarkDead(Device& d, const char* what, int err)
		{
			if (!d.dead)
				Log("usb pads: ugen: %s: %s errno %d: the device is gone", d.node.c_str(), what, err);
			d.dead = true;
		}

		bool StartTransfer(Device& d, int t, uint32_t len, uint16_t flags, uint16_t timeout)
		{
			d.buf_len[t] = len;
			d.eps[t].nFrames = 1;
			d.eps[t].aFrames = 0;
			d.eps[t].status = 0;
			d.eps[t].flags = flags;
			d.eps[t].timeout = timeout;
			FsIndex s{static_cast<uint8_t>(t)};
			if (const int r = m_ops.ioctl(d.fd, FS_START, &s); r != 0)
			{
				if (r == -ENXIO || r == -ENODEV)
					MarkDead(d, "USB_FS_START", -r);
				else if (d.errors++ < 5)
					Log("usb pads: ugen: %s: USB_FS_START %d errno %d", d.node.c_str(), t, -r);
				return false;
			}
			d.pending[t] = true;
			return true;
		}

		void ArmReads(Device& d)
		{
			for (int t = 1; t <= 2 && !d.dead; t++)
			{
				if (!d.opened[t] || d.pending[t])
					continue;
				uint16_t flags = FLAG_SINGLE_SHORT_OK;
				if (d.clear_stall)
				{
					flags |= FLAG_CLEAR_STALL;
					d.clear_stall = false;
				}
				StartTransfer(d, t, std::min<uint32_t>(BUF, d.itf.in_size ? d.itf.in_size : BUF), flags, 0);
			}
		}

		/// Every finished transfer collected; reports to the queue (the oldest go when it is full).
		void Reap(Device& d)
		{
			for (int guard = 0; guard < 64 && !d.dead; guard++)
			{
				FsIndex c{};
				if (const int r = m_ops.ioctl(d.fd, FS_COMPLETE, &c); r != 0)
				{
					// EBUSY: nothing finished. Only a device that is gone ends it all.
					if (r == -ENXIO || r == -ENODEV)
						MarkDead(d, "USB_FS_COMPLETE", -r);
					else if (r != -EBUSY && d.errors++ < 5)
						Log("usb pads: ugen: %s: USB_FS_COMPLETE errno %d", d.node.c_str(), -r);
					return;
				}
				const int t = c.ep_index;
				if (t <= 0 || t >= TRANSFERS)
					continue;
				d.pending[t] = false;
				if (t == 3)
					continue; // the write's status is read by Write
				const int status = d.eps[t].status;
				if (status == 0 && d.eps[t].aFrames > 0 && d.buf_len[t] > 0)
				{
					if (d.count == QUEUE)
					{
						d.head = (d.head + 1) % QUEUE;
						d.count--;
					}
					Report& r = d.queue[(d.head + d.count) % QUEUE];
					r.len = std::min(d.buf_len[t], BUF);
					std::memcpy(r.data, d.buf[t], r.len);
					d.count++;
				}
				else if (status != 0 && status != ERR_CANCELLED)
				{
					if (d.errors++ < 5)
						Log("usb pads: ugen: %s: read status %d", d.node.c_str(), status);
					d.clear_stall = true;
				}
			}
		}

		/// Every transfer stopped and waited for, at most wait_ms; then the table taken down and the
		/// node closed. One that still runs leaves the node open (fd stays), and the log says so.
		void CloseDevice(Device& d, uint64_t wait_ms)
		{
			if (d.fd < 0)
				return;
			if (!d.dead)
			{
				for (int t = 1; t < TRANSFERS; t++)
				{
					if (d.opened[t] && d.pending[t])
					{
						FsIndex s{static_cast<uint8_t>(t)};
						m_ops.ioctl(d.fd, FS_STOP, &s);
					}
				}
				const uint64_t until = m_ops.now_ms() + wait_ms;
				auto running = [&] {
					for (int t = 1; t < TRANSFERS; t++)
					{
						if (d.pending[t])
							return true;
					}
					return false;
				};
				for (;;)
				{
					Reap(d);
					if (!running() || d.dead || m_ops.now_ms() >= until)
						break;
					m_ops.sleep_us(1000);
				}
				if (running() && !d.dead)
				{
					Log("usb pads: ugen: %s: transfers still running after being stopped: the node is left open", d.node.c_str());
					return;
				}
				if (!d.dead)
				{
					FsIndex u{0};
					m_ops.ioctl(d.fd, FS_UNINIT, &u);
				}
			}
			m_ops.close(d.fd);
			d.fd = -1;
		}

		SysOps m_ops;
		void (*m_log)(const char*) = nullptr;
		bool m_started = false;
		std::map<uint64_t, Known> m_known;
		std::map<int, std::unique_ptr<Device>> m_devices;
		std::vector<std::unique_ptr<Device>> m_stuck;
		int m_next_handle = HANDLE_BASE;
	};
} // namespace PS5Ugen
