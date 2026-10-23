// Crux2 - USB game pads that are not PlayStation controllers (ps5_usbpad.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The pads are read on a thread of their own through a transport (the console's USB access),
// decoded by ps5_usbpad_logic.h, and handed to the input source as up to four more controllers,
// SDL-4 to SDL-7. The thread looks for pads that came or went once a second.
//
// The console's transport (ps5_usbpad_console.cpp) reads USB devices through the console's
// libSceUsbd (libusb's interface under Sony's names), which is found while the title runs: the
// payload SDK has no stub library for it, and a title that imports what the console does not give
// it does not start. Whether a homebrew title may open a USB device at all had not been tried on
// a console when this was written (needs proper testing): every step says in the boot log how it
// went (usb pads: ...), and when one is refused the transport gives up for the session and only
// the console's own controllers are read. Off by default ([PS5] USBPads; Settings > Controllers
// > "USB controllers (experimental)").

#pragma once

#include "ps5_usbpad_logic.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PS5UsbPads
{
	constexpr uint32_t MAX_PADS = 4;

	/// One pad interface found on the bus.
	struct DeviceInfo
	{
		uint64_t id = 0; // stable while the device stays plugged in (bus and address, say)
		uint16_t vendor = 0;
		uint16_t product = 0;
		uint8_t interface_class = 0;
		uint8_t interface_subclass = 0;
		uint8_t interface_protocol = 0;
		uint8_t interface_number = 0;
		std::vector<uint8_t> report_descriptor; // HID interfaces only
		std::string name; // the product string, or empty
	};

	/// The console's USB access, or a test's.
	class Transport
	{
	public:
		virtual ~Transport() = default;
		/// False when there is no USB access: the manager then gives up for the session.
		virtual bool Start() = 0;
		virtual void Stop() = 0;
		/// The interfaces on the bus now (pads or not: the manager decides).
		virtual bool Enumerate(std::vector<DeviceInfo>* devices) = 0;
		/// Opens an interface for reading; a handle, or a negative value.
		virtual int Open(const DeviceInfo& device) = 0;
		/// One input report, waiting at most timeout_ms: its size, 0 when none came, a negative
		/// value when the device is gone.
		virtual int Read(int handle, uint8_t* buffer, uint32_t size, uint32_t timeout_ms) = 0;
		/// An output report (the rumble); false when it was not sent.
		virtual bool Write(int handle, const uint8_t* data, uint32_t size) = 0;
		/// A control transfer on the default pipe (a HID feature or output report by
		/// GET/SET_REPORT): the bytes moved, or a negative value.
		virtual int Control(int /*handle*/, uint8_t /*request_type*/, uint8_t /*request*/, uint16_t /*value*/, uint16_t /*index*/,
			uint8_t* /*data*/, uint16_t /*length*/)
		{
			return -1;
		}
		virtual void Close(int handle) = 0;
	};

	/// The console's transport (libSceUsbd; see above). On the PC, one whose Start() says no.
	std::unique_ptr<Transport> CreateConsoleTransport();

	/// What the input source sees of one slot.
	struct Slot
	{
		bool connected = false;
		uint32_t generation = 0; // changes each time another pad takes the slot
		std::string name;
		PS5UsbPadLogic::Protocol protocol = PS5UsbPadLogic::Protocol::None;
		PS5UsbPadLogic::MappedPad state;
	};

	class Manager
	{
	public:
		Manager() = default;
		~Manager();

		Manager(const Manager&) = delete;
		Manager& operator=(const Manager&) = delete;

		/// Starts the reading thread over this transport; false (and logged once) when the
		/// transport has no USB access. With threaded false, nothing runs by itself: the caller
		/// calls PollOnce (the tests).
		bool Start(std::unique_ptr<Transport> transport, bool threaded = true);
		void Stop();
		bool Running() const { return m_running.load(std::memory_order_acquire); }

		/// One round of the thread's work: the bus looked at again when due (now_s is a clock in
		/// seconds), each open pad read once, rumble sent when it changed.
		void PollOnce(double now_s, uint32_t read_timeout_ms);

		/// A copy of a slot, from any thread.
		Slot Get(uint32_t slot) const;

		/// The motors of a slot's pad (0 to 255 each); sent by the reading thread.
		void SetRumble(uint32_t slot, uint8_t large, uint8_t small);

		static constexpr double RESCAN_SECONDS = 1.0;

	private:
		struct Open
		{
			bool used = false;
			uint64_t id = 0;
			int handle = -1;
			PS5UsbPadLogic::Protocol protocol = PS5UsbPadLogic::Protocol::None;
			PS5UsbPadLogic::HidLayout layout;
			PS5UsbPadLogic::HidState hid;
			std::array<uint8_t, 2> rumble_sent = {};
			uint8_t interface_number = 0;
			bool output_sent = false; // a DualShock 3's light set once
		};

		void Rescan();
		void Release(uint32_t slot);
		void ThreadMain();

		std::unique_ptr<Transport> m_transport;
		std::array<Open, MAX_PADS> m_open; // the reading thread's alone
		double m_next_scan = 0.0;

		mutable std::mutex m_mutex; // guards what follows
		std::array<Slot, MAX_PADS> m_slots;
		std::array<std::array<uint8_t, 2>, MAX_PADS> m_rumble = {};

		std::atomic<bool> m_running{false};
		std::atomic<bool> m_quit{false};
		std::thread m_thread;
	};
} // namespace PS5UsbPads
