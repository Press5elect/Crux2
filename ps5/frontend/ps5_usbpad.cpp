// Crux2 - USB game pads that are not PlayStation controllers: the reading thread.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// See ps5_usbpad.h for what is and is not there: the console's transport is a stub, so on the
// console Start() logs one line and returns false. The rest builds and runs on the PC as well,
// where the tests drive it with a made-up transport.

#include "ps5_usbpad.h"
#include "ps5_priority.h"

#include <chrono>
#include <cstdio>

#ifdef __PROSPERO__
extern "C" void ps5px_log(const char* format, ...);
#define USBPAD_LOG(...) ps5px_log(__VA_ARGS__)
#else
#define USBPAD_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace PS5UsbPads
{
	namespace
	{
		// The console's transport: no USB access yet (ps5_usbpad.h says why).
		class StubTransport final : public Transport
		{
		public:
			bool Start() override { return false; }
			void Stop() override {}
			bool Enumerate(std::vector<DeviceInfo>* devices) override
			{
				devices->clear();
				return false;
			}
			int Open(const DeviceInfo&) override { return -1; }
			int Read(int, uint8_t*, uint32_t, uint32_t) override { return -1; }
			bool Write(int, const uint8_t*, uint32_t) override { return false; }
			void Close(int) override {}
		};

		double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	} // namespace

	std::unique_ptr<Transport> CreateConsoleTransport()
	{
		return std::make_unique<StubTransport>();
	}

	Manager::~Manager()
	{
		Stop();
	}

	bool Manager::Start(std::unique_ptr<Transport> transport, bool threaded)
	{
		if (m_running.load(std::memory_order_acquire) || !transport)
			return m_running.load(std::memory_order_acquire);

		if (!transport->Start())
		{
			USBPAD_LOG("usb pads: no USB access in this build (the transport is a stub: libSceUsbd has no SDK stubs); "
					   "only the console's own controllers are read");
			return false;
		}
		m_transport = std::move(transport);
		m_next_scan = 0.0;
		m_quit.store(false, std::memory_order_release);
		m_running.store(true, std::memory_order_release);
		USBPAD_LOG("usb pads: looking for USB pads once a second");
		if (threaded)
			m_thread = std::thread([this]() { ThreadMain(); });
		return true;
	}

	void Manager::Stop()
	{
		if (!m_running.load(std::memory_order_acquire))
			return;
		m_quit.store(true, std::memory_order_release);
		if (m_thread.joinable())
			m_thread.join();
		for (uint32_t i = 0; i < MAX_PADS; i++)
			Release(i);
		m_transport->Stop();
		m_transport.reset();
		m_running.store(false, std::memory_order_release);
	}

	void Manager::ThreadMain()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Input);
		while (!m_quit.load(std::memory_order_acquire))
		{
			bool any = false;
			for (const Open& o : m_open)
				any = any || o.used;
			// With pads, each is read with a short wait (the reports come every few
			// milliseconds); without, the thread only looks at the bus now and then.
			PollOnce(NowSeconds(), 4);
			if (!any)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	void Manager::Release(uint32_t slot)
	{
		Open& o = m_open[slot];
		if (o.used && o.handle >= 0 && m_transport)
		{
			if (o.protocol == PS5UsbPadLogic::Protocol::XInput && (o.rumble_sent[0] || o.rumble_sent[1]))
			{
				const auto report = PS5UsbPadLogic::XInputRumbleReport(0, 0);
				m_transport->Write(o.handle, report.data(), static_cast<uint32_t>(report.size()));
			}
			m_transport->Close(o.handle);
		}
		if (o.used)
			USBPAD_LOG("usb pads: player %u's pad let go", slot + 5);
		o = Open();
		std::lock_guard lock(m_mutex);
		Slot& s = m_slots[slot];
		s.connected = false;
		s.state = {};
		m_rumble[slot] = {};
	}

	void Manager::Rescan()
	{
		std::vector<DeviceInfo> devices;
		if (!m_transport->Enumerate(&devices))
			return;

		// Pads that went away.
		for (uint32_t i = 0; i < MAX_PADS; i++)
		{
			if (!m_open[i].used)
				continue;
			bool present = false;
			for (const DeviceInfo& d : devices)
				present = present || d.id == m_open[i].id;
			if (!present)
				Release(i);
		}

		// Pads that came: each takes the first free slot.
		for (const DeviceInfo& d : devices)
		{
			bool known = false;
			for (const Open& o : m_open)
				known = known || (o.used && o.id == d.id);
			if (known)
				continue;

			PS5UsbPadLogic::HidLayout layout;
			if (d.interface_class == 0x03 && !d.report_descriptor.empty())
				layout = PS5UsbPadLogic::ParseReportDescriptor(d.report_descriptor.data(), d.report_descriptor.size());
			const PS5UsbPadLogic::Protocol protocol =
				PS5UsbPadLogic::Classify(d.vendor, d.interface_class, d.interface_subclass, d.interface_protocol, &layout);
			if (protocol == PS5UsbPadLogic::Protocol::None)
				continue;

			uint32_t slot = MAX_PADS;
			for (uint32_t i = 0; i < MAX_PADS && slot == MAX_PADS; i++)
			{
				if (!m_open[i].used)
					slot = i;
			}
			if (slot == MAX_PADS)
				break;

			const int handle = m_transport->Open(d);
			if (handle < 0)
			{
				USBPAD_LOG("usb pads: %04x:%04x could not be opened (%d)", d.vendor, d.product, handle);
				continue;
			}
			Open& o = m_open[slot];
			o.used = true;
			o.id = d.id;
			o.handle = handle;
			o.protocol = protocol;
			o.layout = std::move(layout);
			o.hid = {};
			USBPAD_LOG("usb pads: %04x:%04x (%s) is player %u, %s, %u buttons", d.vendor, d.product,
				d.name.empty() ? "no name" : d.name.c_str(), slot + 5,
				protocol == PS5UsbPadLogic::Protocol::XInput ? "Xbox 360 protocol" : "HID",
				protocol == PS5UsbPadLogic::Protocol::XInput ? 15u : o.layout.button_count);

			std::lock_guard lock(m_mutex);
			Slot& s = m_slots[slot];
			s.connected = true;
			s.generation++;
			s.protocol = protocol;
			s.name = d.name.empty() ? (protocol == PS5UsbPadLogic::Protocol::XInput ? "Xbox 360 Controller" : "USB Controller") : d.name;
			s.state = {};
		}
	}

	void Manager::PollOnce(double now_s, uint32_t read_timeout_ms)
	{
		if (!m_running.load(std::memory_order_acquire))
			return;
		if (now_s >= m_next_scan)
		{
			m_next_scan = now_s + RESCAN_SECONDS;
			Rescan();
		}

		for (uint32_t i = 0; i < MAX_PADS; i++)
		{
			Open& o = m_open[i];
			if (!o.used)
				continue;

			// Every report waiting, the last one wins; a read that fails means the pad is gone.
			uint8_t buffer[64];
			bool changed = false;
			PS5UsbPadLogic::MappedPad mapped;
			for (int n = 0; n < 8; n++)
			{
				const int got = m_transport->Read(o.handle, buffer, sizeof(buffer), n == 0 ? read_timeout_ms : 0);
				if (got < 0)
				{
					Release(i);
					break;
				}
				if (got == 0)
					break;
				if (o.protocol == PS5UsbPadLogic::Protocol::XInput)
				{
					changed = PS5UsbPadLogic::DecodeXInputReport(buffer, static_cast<size_t>(got), &mapped) || changed;
				}
				else if (PS5UsbPadLogic::DecodeHidReport(o.layout, buffer, static_cast<size_t>(got), &o.hid))
				{
					mapped = PS5UsbPadLogic::MapHid(o.layout, o.hid);
					changed = true;
				}
			}
			if (!o.used)
				continue;

			std::array<uint8_t, 2> rumble;
			{
				std::lock_guard lock(m_mutex);
				if (changed)
					m_slots[i].state = mapped;
				rumble = m_rumble[i];
			}
			if (o.protocol == PS5UsbPadLogic::Protocol::XInput && rumble != o.rumble_sent)
			{
				const auto report = PS5UsbPadLogic::XInputRumbleReport(rumble[0], rumble[1]);
				if (m_transport->Write(o.handle, report.data(), static_cast<uint32_t>(report.size())))
					o.rumble_sent = rumble;
			}
		}
	}

	Slot Manager::Get(uint32_t slot) const
	{
		if (slot >= MAX_PADS)
			return {};
		std::lock_guard lock(m_mutex);
		return m_slots[slot];
	}

	void Manager::SetRumble(uint32_t slot, uint8_t large, uint8_t small)
	{
		if (slot >= MAX_PADS)
			return;
		std::lock_guard lock(m_mutex);
		m_rumble[slot] = {large, small};
	}
} // namespace PS5UsbPads
