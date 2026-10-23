// Crux2 - USB game pads: the console's transport, through libSceUsbd.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// libSceUsbd is libusb 1.0 under Sony's names (sceUsbdInit, sceUsbdGetDeviceList, sceUsbdOpen,
// sceUsbdInterruptTransfer, ...), with libusb's structures. The title does not link against it:
// the payload SDK has no stub library for it, and a title that imports a function the console
// does not give it does not start at all. So the module is brought in while the title runs, as
// the microphones' libSceAudioIn is (ps5_mic.cpp): found if it is already in the process, else
// loaded from the system's libraries with sceKernelLoadStartModule, and its functions looked up by
// name, then by NID (derived from the names as ps5/tools/check-imports.py derives them).
//
// Not run on a console when it was written (needs proper testing): whether a fake-signed title may
// open a USB device, and whether the console's own drivers let go of a pad, are what its first run
// shows. Every step that is refused says so once in the boot log (usb pads: ...), and the transport
// then gives up for the session: the console's own controllers are read as always.
//
// The pads are found by their interfaces: an Xbox 360 interface (class 0xFF, 0x5D, 0x01), a HID
// interface whose report descriptor has a joystick or game pad in it, or a DualShock 3 (054c:0268).
// Each is opened once when it comes; the HID report descriptor is read then (GET_DESCRIPTOR on the
// default pipe), and kept while the device stays plugged in.

#include "ps5_usbpad.h"
#include "ps5_keyboard.h" // PS5SystemModules

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5UsbPads
{
	namespace
	{
		// ---- libusb's structures, as libSceUsbd has them

		struct DeviceDescriptor
		{
			uint8_t bLength, bDescriptorType;
			uint16_t bcdUSB;
			uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
			uint16_t idVendor, idProduct, bcdDevice;
			uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
		};
		static_assert(sizeof(DeviceDescriptor) == 18);

		struct EndpointDescriptor
		{
			uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
			uint16_t wMaxPacketSize;
			uint8_t bInterval, bRefresh, bSynchAddress;
			const unsigned char* extra;
			int extra_length;
		};

		struct InterfaceDescriptor
		{
			uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass,
				bInterfaceSubClass, bInterfaceProtocol, iInterface;
			const EndpointDescriptor* endpoint;
			const unsigned char* extra;
			int extra_length;
		};

		struct Interface
		{
			const InterfaceDescriptor* altsetting;
			int num_altsetting;
		};

		struct ConfigDescriptor
		{
			uint8_t bLength, bDescriptorType;
			uint16_t wTotalLength;
			uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
			const Interface* interface;
			const unsigned char* extra;
			int extra_length;
		};

		struct Device; // opaque
		struct DeviceHandle; // opaque

		constexpr int ERROR_TIMEOUT = -7; // LIBUSB_ERROR_TIMEOUT
		constexpr int ERROR_NO_DEVICE = -4; // LIBUSB_ERROR_NO_DEVICE

		struct Api
		{
			int (*init)(void) = nullptr;
			void (*exit)(void) = nullptr;
			intptr_t (*get_device_list)(Device*** list) = nullptr;
			void (*free_device_list)(Device** list, int unref) = nullptr;
			int (*get_device_descriptor)(Device* dev, DeviceDescriptor* desc) = nullptr;
			int (*get_active_config_descriptor)(Device* dev, ConfigDescriptor** config) = nullptr;
			void (*free_config_descriptor)(ConfigDescriptor* config) = nullptr;
			uint8_t (*get_bus_number)(Device* dev) = nullptr;
			uint8_t (*get_device_address)(Device* dev) = nullptr;
			int (*open)(Device* dev, DeviceHandle** handle) = nullptr;
			void (*close)(DeviceHandle* handle) = nullptr;
			int (*claim_interface)(DeviceHandle* handle, int iface) = nullptr;
			int (*release_interface)(DeviceHandle* handle, int iface) = nullptr;
			int (*kernel_driver_active)(DeviceHandle* handle, int iface) = nullptr; // optional
			int (*detach_kernel_driver)(DeviceHandle* handle, int iface) = nullptr; // optional
			int (*interrupt_transfer)(DeviceHandle* handle, unsigned char endpoint, unsigned char* data, int length, int* transferred,
				unsigned int timeout) = nullptr;
			int (*control_transfer)(DeviceHandle* handle, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
				unsigned char* data, uint16_t length, unsigned int timeout) = nullptr;
		};

		// Each function by name and NID (the NID: the first 8 bytes of SHA-1 of the name and the
		// console's suffix, in its base64; checked against three NIDs the console resolves).
		struct Wanted
		{
			const char* name;
			const char* nid;
			void** slot;
			bool optional;
		};

		bool FindAll(Api& api, int handle)
		{
			const Wanted wanted[] = {
				{"sceUsbdInit", "TOhg7P6kTH4", reinterpret_cast<void**>(&api.init), false},
				{"sceUsbdExit", "Fq6+0Fm55xU", reinterpret_cast<void**>(&api.exit), false},
				{"sceUsbdGetDeviceList", "8qB9Ar4P5nc", reinterpret_cast<void**>(&api.get_device_list), false},
				{"sceUsbdFreeDeviceList", "EQ6SCLMqzkM", reinterpret_cast<void**>(&api.free_device_list), false},
				{"sceUsbdGetDeviceDescriptor", "bhomgbiQgeo", reinterpret_cast<void**>(&api.get_device_descriptor), false},
				{"sceUsbdGetActiveConfigDescriptor", "S1o1C6yOt5g", reinterpret_cast<void**>(&api.get_active_config_descriptor), false},
				{"sceUsbdFreeConfigDescriptor", "Hvd3S--n25w", reinterpret_cast<void**>(&api.free_config_descriptor), false},
				{"sceUsbdGetBusNumber", "t7WE9mb1TB8", reinterpret_cast<void**>(&api.get_bus_number), false},
				{"sceUsbdGetDeviceAddress", "GjlCrU4GcIY", reinterpret_cast<void**>(&api.get_device_address), false},
				{"sceUsbdOpen", "VJ6oMq-Di2U", reinterpret_cast<void**>(&api.open), false},
				{"sceUsbdClose", "HarYYlaFGJY", reinterpret_cast<void**>(&api.close), false},
				{"sceUsbdClaimInterface", "AE+mHBHneyk", reinterpret_cast<void**>(&api.claim_interface), false},
				{"sceUsbdReleaseInterface", "REfUTmTchMw", reinterpret_cast<void**>(&api.release_interface), false},
				{"sceUsbdKernelDriverActive", "RLf56F-WjKQ", reinterpret_cast<void**>(&api.kernel_driver_active), true},
				{"sceUsbdDetachKernelDriver", "Y5go+ha6eDs", reinterpret_cast<void**>(&api.detach_kernel_driver), true},
				{"sceUsbdInterruptTransfer", "rxi1nCOKWc8", reinterpret_cast<void**>(&api.interrupt_transfer), false},
				{"sceUsbdControlTransfer", "RRKFcKQ1Ka4", reinterpret_cast<void**>(&api.control_transfer), false},
			};
			bool all = true;
			std::string missing;
			for (const Wanted& w : wanted)
			{
				if (*w.slot == nullptr)
				{
					*w.slot = (handle >= 0) ? PS5SystemModules::FindInHandle(handle, w.name, w.nid) :
											  PS5SystemModules::FindFunction("libSceUsbd", w.name, w.nid);
				}
				if (*w.slot == nullptr && !w.optional)
				{
					all = false;
					missing += missing.empty() ? w.name : (std::string(", ") + w.name);
				}
			}
			if (!all && handle >= 0)
				ps5px_log("usb pads: libSceUsbd lacks %s", missing.c_str());
			return all;
		}

		/// libSceUsbd's functions: in the process already, or loaded from the system's libraries.
		bool LoadApi(Api& api)
		{
			if (FindAll(api, -1))
			{
				ps5px_log("usb pads: libSceUsbd is in the process");
				return true;
			}
			using LoadFn = int (*)(const char*, size_t, const void*, uint32_t, void*, int*);
			const auto load = reinterpret_cast<LoadFn>(PS5SystemModules::FindFunction("libkernel", "sceKernelLoadStartModule", "wzvqT4UqKX8"));
			if (!load)
			{
				ps5px_log("usb pads: libSceUsbd is not in the process, and the module loader was not found");
				return false;
			}
			for (const char* path : {"/system/common/lib/libSceUsbd.sprx", "/system/priv/lib/libSceUsbd.sprx"})
			{
				int result = 0;
				const int handle = load(path, 0, nullptr, 0, nullptr, &result);
				ps5px_log("usb pads: %s loaded: %#x", path, static_cast<unsigned>(handle));
				if (handle >= 0 && FindAll(api, handle))
					return true;
				if (FindAll(api, -1))
					return true;
			}
			return false;
		}

		class UsbdTransport final : public Transport
		{
		public:
			bool Start() override
			{
				if (!LoadApi(m_api))
				{
					ps5px_log("usb pads: libSceUsbd could not be used: only the console's own controllers are read");
					return false;
				}
				const int r = m_api.init();
				ps5px_log("usb pads: sceUsbdInit: %d", r);
				if (r < 0)
					return false;
				m_started = true;
				return true;
			}

			void Stop() override
			{
				if (!m_started)
					return;
				for (auto& [h, o] : m_handles)
				{
					m_api.release_interface(o.handle, o.interface_number);
					m_api.close(o.handle);
				}
				m_handles.clear();
				m_api.exit();
				m_started = false;
			}

			bool Enumerate(std::vector<DeviceInfo>* devices) override
			{
				devices->clear();
				Device** list = nullptr;
				const intptr_t count = m_api.get_device_list(&list);
				if (count < 0)
				{
					if (!m_said_list)
						ps5px_log("usb pads: sceUsbdGetDeviceList: %ld", static_cast<long>(count));
					m_said_list = true;
					return false;
				}
				std::map<uint64_t, Known> seen;
				for (intptr_t i = 0; i < count; i++)
				{
					Device* dev = list[i];
					DeviceDescriptor dd = {};
					if (m_api.get_device_descriptor(dev, &dd) < 0)
						continue;
					const uint64_t id = (static_cast<uint64_t>(m_api.get_bus_number(dev)) << 8) | m_api.get_device_address(dev);
					// What was learnt about it before (it stays as long as it stays plugged in).
					if (const auto k = m_known.find(id); k != m_known.end() && k->second.vendor == dd.idVendor &&
														  k->second.product == dd.idProduct)
					{
						seen.emplace(id, k->second);
						if (k->second.pad)
							devices->push_back(k->second.info);
						continue;
					}
					Known k = Learn(dev, dd, id);
					if (k.pad)
						devices->push_back(k.info);
					seen.emplace(id, std::move(k));
				}
				m_api.free_device_list(list, 1);
				m_known = std::move(seen);
				return true;
			}

			int Open(const DeviceInfo& device) override
			{
				const auto k = m_known.find(device.id);
				if (k == m_known.end() || !k->second.dev_ref_valid)
					return -1;
				// The device is looked up again in a fresh list (the one Learn saw is freed).
				Device** list = nullptr;
				const intptr_t count = m_api.get_device_list(&list);
				if (count < 0)
					return -1;
				Device* dev = nullptr;
				for (intptr_t i = 0; i < count && !dev; i++)
				{
					const uint64_t id = (static_cast<uint64_t>(m_api.get_bus_number(list[i])) << 8) | m_api.get_device_address(list[i]);
					if (id == device.id)
						dev = list[i];
				}
				int result = -1;
				DeviceHandle* h = nullptr;
				if (dev)
				{
					const int r = m_api.open(dev, &h);
					if (r < 0 || !h)
						ps5px_log("usb pads: %04x:%04x: sceUsbdOpen: %d", device.vendor, device.product, r);
					else
						result = Claim(h, device, k->second);
				}
				m_api.free_device_list(list, 1);
				return result;
			}

			int Read(int handle, uint8_t* buffer, uint32_t size, uint32_t timeout_ms) override
			{
				const auto it = m_handles.find(handle);
				if (it == m_handles.end() || it->second.in_endpoint == 0)
					return -1;
				int transferred = 0;
				// (libusb's 0 is no time limit: a short wait instead.)
				const int r = m_api.interrupt_transfer(it->second.handle, it->second.in_endpoint, buffer, static_cast<int>(size),
					&transferred, timeout_ms == 0 ? 1u : timeout_ms);
				if (r == 0)
					return transferred;
				if (r == ERROR_TIMEOUT)
					return transferred > 0 ? transferred : 0;
				if (r == ERROR_NO_DEVICE)
					return -1;
				if (!it->second.said_read)
				{
					it->second.said_read = true;
					ps5px_log("usb pads: reading endpoint %#x: %d", it->second.in_endpoint, r);
				}
				return 0;
			}

			bool Write(int handle, const uint8_t* data, uint32_t size) override
			{
				const auto it = m_handles.find(handle);
				if (it == m_handles.end() || it->second.out_endpoint == 0)
					return false;
				std::vector<uint8_t> copy(data, data + size);
				int transferred = 0;
				return m_api.interrupt_transfer(it->second.handle, it->second.out_endpoint, copy.data(), static_cast<int>(size),
						   &transferred, 50) == 0;
			}

			int Control(int handle, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data,
				uint16_t length) override
			{
				const auto it = m_handles.find(handle);
				if (it == m_handles.end())
					return -1;
				return m_api.control_transfer(it->second.handle, request_type, request, value, index, data, length, 200);
			}

			void Close(int handle) override
			{
				const auto it = m_handles.find(handle);
				if (it == m_handles.end())
					return;
				m_api.release_interface(it->second.handle, it->second.interface_number);
				m_api.close(it->second.handle);
				m_handles.erase(it);
			}

		private:
			struct Known
			{
				uint16_t vendor = 0, product = 0;
				bool pad = false;
				bool dev_ref_valid = false;
				uint8_t in_endpoint = 0, out_endpoint = 0;
				DeviceInfo info;
			};

			struct OpenPad
			{
				DeviceHandle* handle = nullptr;
				uint8_t interface_number = 0;
				uint8_t in_endpoint = 0, out_endpoint = 0;
				bool said_read = false;
			};

			/// A device seen for the first time: whether it is a pad, which interface and
			/// endpoints, and for a HID interface its report descriptor.
			Known Learn(Device* dev, const DeviceDescriptor& dd, uint64_t id)
			{
				Known k;
				k.vendor = dd.idVendor;
				k.product = dd.idProduct;
				ConfigDescriptor* config = nullptr;
				if (m_api.get_active_config_descriptor(dev, &config) < 0 || !config)
					return k;
				for (int i = 0; i < config->bNumInterfaces && !k.pad; i++)
				{
					const Interface& itf = config->interface[i];
					if (itf.num_altsetting < 1)
						continue;
					const InterfaceDescriptor& alt = itf.altsetting[0];
					const bool xinput = PS5UsbPadLogic::IsXInputInterface(alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol);
					const bool hid = alt.bInterfaceClass == 0x03 &&
									 !(alt.bInterfaceSubClass == 0x01 && (alt.bInterfaceProtocol == 0x01 || alt.bInterfaceProtocol == 0x02));
					if (!xinput && !hid)
						continue;
					if (dd.idVendor == 0x054c && !PS5UsbPadLogic::IsDualShock3(dd.idVendor, dd.idProduct))
						continue; // the console's own controllers are its pad service's
					uint8_t in = 0, out = 0;
					for (int e = 0; e < alt.bNumEndpoints; e++)
					{
						const EndpointDescriptor& ep = alt.endpoint[e];
						if ((ep.bmAttributes & 0x03) != 0x03)
							continue; // interrupt endpoints only
						if (ep.bEndpointAddress & 0x80)
							in = in ? in : ep.bEndpointAddress;
						else
							out = out ? out : ep.bEndpointAddress;
					}
					if (in == 0)
						continue;
					k.in_endpoint = in;
					k.out_endpoint = out;
					k.info.id = id;
					k.info.vendor = dd.idVendor;
					k.info.product = dd.idProduct;
					k.info.interface_class = alt.bInterfaceClass;
					k.info.interface_subclass = alt.bInterfaceSubClass;
					k.info.interface_protocol = alt.bInterfaceProtocol;
					k.info.interface_number = alt.bInterfaceNumber;
					if (hid && !PS5UsbPadLogic::IsDualShock3(dd.idVendor, dd.idProduct))
					{
						k.info.report_descriptor = ReadReportDescriptor(dev, alt);
						if (k.info.report_descriptor.empty())
							continue;
						const PS5UsbPadLogic::HidLayout layout =
							PS5UsbPadLogic::ParseReportDescriptor(k.info.report_descriptor.data(), k.info.report_descriptor.size());
						if (!layout.is_gamepad)
							continue; // a keyboard, a mouse, a headset's buttons...
					}
					k.pad = true;
					k.dev_ref_valid = true;
					ps5px_log("usb pads: found %04x:%04x, interface %u (class %02x/%02x/%02x), endpoints in %#x out %#x", dd.idVendor,
						dd.idProduct, alt.bInterfaceNumber, alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol, in, out);
				}
				m_api.free_config_descriptor(config);
				return k;
			}

			/// The HID report descriptor's length is in the HID descriptor (type 0x21) among the
			/// interface's extra bytes; the descriptor itself is asked for on the default pipe.
			std::vector<uint8_t> ReadReportDescriptor(Device* dev, const InterfaceDescriptor& alt)
			{
				uint16_t length = 0;
				for (int at = 0; alt.extra && at + 9 <= alt.extra_length;)
				{
					const unsigned char* d = alt.extra + at;
					if (d[0] == 0)
						break;
					if (d[1] == 0x21 && d[0] >= 9 && d[6] == 0x22)
					{
						length = static_cast<uint16_t>(d[7] | (d[8] << 8));
						break;
					}
					at += d[0];
				}
				if (length == 0)
					length = 512;
				std::vector<uint8_t> desc(length);
				DeviceHandle* h = nullptr;
				if (m_api.open(dev, &h) < 0 || !h)
					return {};
				const int got = m_api.control_transfer(h, 0x81, 0x06, 0x2200, alt.bInterfaceNumber, desc.data(), length, 200);
				m_api.close(h);
				if (got <= 0)
				{
					ps5px_log("usb pads: the HID report descriptor of interface %u could not be read: %d", alt.bInterfaceNumber, got);
					return {};
				}
				desc.resize(static_cast<size_t>(got));
				return desc;
			}

			int Claim(DeviceHandle* h, const DeviceInfo& device, const Known& k)
			{
				const int iface = device.interface_number;
				if (m_api.kernel_driver_active && m_api.detach_kernel_driver && m_api.kernel_driver_active(h, iface) == 1)
				{
					const int d = m_api.detach_kernel_driver(h, iface);
					ps5px_log("usb pads: %04x:%04x: the console's driver had interface %d: let go %d", device.vendor, device.product, iface, d);
				}
				const int c = m_api.claim_interface(h, iface);
				if (c < 0)
				{
					ps5px_log("usb pads: %04x:%04x: sceUsbdClaimInterface(%d): %d", device.vendor, device.product, iface, c);
					m_api.close(h);
					return -1;
				}
				const int handle = m_next_handle++;
				OpenPad o;
				o.handle = h;
				o.interface_number = static_cast<uint8_t>(iface);
				o.in_endpoint = k.in_endpoint;
				o.out_endpoint = k.out_endpoint;
				m_handles.emplace(handle, o);
				return handle;
			}

			Api m_api;
			bool m_started = false;
			bool m_said_list = false;
			std::map<uint64_t, Known> m_known;
			std::map<int, OpenPad> m_handles;
			int m_next_handle = 1;
		};
	} // namespace

	std::unique_ptr<Transport> CreateConsoleTransport()
	{
		return std::make_unique<UsbdTransport>();
	}
} // namespace PS5UsbPads
