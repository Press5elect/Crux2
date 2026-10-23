// Crux2 - USB game pads' reports, read without the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A HID report descriptor parser, the Xbox 360 wired pad's fixed report, and both mapped onto the
// controller layout the input source speaks (SDL's buttons and axes, plus numbered buttons and
// axes for what has no standard name).
//
// Three kinds of pad:
//
//  * Xbox 360 wired pads (and the many third-party pads that speak their protocol): USB class
//    0xFF, subclass 0x5D, protocol 0x01. Their input report is always the same 20 bytes, so no
//    descriptor is read: 0x00, 0x14, two bytes of buttons, the two triggers (0 to 255), then the
//    four stick axes as signed 16-bit little-endian values with up positive. The rumble is an
//    8-byte output report: 0x00, 0x08, 0x00, large motor, small motor, 0, 0, 0.
//
//  * Sony's DualShock 3 and SIXAXIS (054c:0268), which the PS5 does not take as a controller of its
//    own: woken by a feature report (GET_REPORT 0xF2), then a 49-byte input report 0x01 with the
//    buttons, the sticks and how hard each of the twelve pressure-sensitive buttons is pressed
//    (d-pad, L1/R1/L2/R2 and the four face buttons, 0 to 255): the pressure the DualShock 2 had.
//    The lights and the rumble are an output report 0x01 (sixaxis_send_output_report in Linux's
//    hid-sony.c is the reference for the layout).
//
//  * Generic HID game pads and joysticks (class 0x03 with a Joystick or Game Pad application
//    collection in the report descriptor): what each report holds is read from the descriptor.
//    Buttons are the Button page's usages (Button 1 is "Button0" here); the hat switch becomes the
//    d-pad; Generic Desktop X and Y are the left stick; Z and Rz the right stick when both are
//    there (the DirectInput pads' habit), else Rx and Ry. Every axis is also "Axis<n>" in the
//    order of AXIS_USAGES, so a pad with another habit can be bound by hand.
//
// Everything here is pure data in, data out, for the PC tests (ps5/tests/input).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PS5UsbPadLogic
{
	// ---------------------------------------------------------------- the mapped state

	/// SDL's numbering, as ps5_input_source.cpp uses it.
	enum : uint32_t
	{
		SDL_SOUTH = 0,
		SDL_EAST = 1,
		SDL_WEST = 2,
		SDL_NORTH = 3,
		SDL_BACK = 4,
		SDL_GUIDE = 5,
		SDL_START = 6,
		SDL_LEFT_STICK = 7,
		SDL_RIGHT_STICK = 8,
		SDL_LEFT_SHOULDER = 9,
		SDL_RIGHT_SHOULDER = 10,
		SDL_DPAD_UP = 11,
		SDL_DPAD_DOWN = 12,
		SDL_DPAD_LEFT = 13,
		SDL_DPAD_RIGHT = 14,
	};

	enum : uint32_t
	{
		SDL_AXIS_LEFT_X,
		SDL_AXIS_LEFT_Y,
		SDL_AXIS_RIGHT_X,
		SDL_AXIS_RIGHT_Y,
		SDL_AXIS_LEFT_TRIGGER,
		SDL_AXIS_RIGHT_TRIGGER,
		SDL_AXIS_COUNT
	};

	constexpr uint32_t MAX_GENERIC_BUTTONS = 32;
	constexpr uint32_t MAX_GENERIC_AXES = 12;

	/// A pad's state in the input source's terms. Sticks are -1 to 1 with down and right positive;
	/// triggers 0 to 1.
	struct MappedPad
	{
		uint32_t buttons = 0; // bit n: SDL button n
		std::array<float, SDL_AXIS_COUNT> axes = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		uint32_t generic_buttons = 0; // bit n: "Button<n>"
		std::array<float, MAX_GENERIC_AXES> generic_axes = {}; // "Axis<n>", -1 to 1
		uint32_t generic_axes_present = 0; // bit n: the pad has Axis<n>
		// How hard each SDL button is pressed, 0 to 1, for a pad that says so (the DualShock 3);
		// has_pressure false: a pressed button is a full press.
		bool has_pressure = false;
		std::array<float, 15> pressure = {};

		bool operator==(const MappedPad&) const = default;
	};

	// ---------------------------------------------------------------- Xbox 360 wired pads

	inline bool IsXInputInterface(uint8_t interface_class, uint8_t subclass, uint8_t protocol)
	{
		return interface_class == 0xff && subclass == 0x5d && protocol == 0x01;
	}

	inline float NormalizeS16(int16_t v)
	{
		return (v >= 0) ? (static_cast<float>(v) / 32767.0f) : (static_cast<float>(v) / 32768.0f);
	}

	/// The 20-byte input report. False for anything else (the pad also sends short reports, such
	/// as the LED status 0x01 0x03 ..., which carry no input).
	inline bool DecodeXInputReport(const uint8_t* data, size_t size, MappedPad* out)
	{
		if (size < 14 || data[0] != 0x00 || data[1] < 0x14)
			return false;

		MappedPad pad;
		const uint8_t lo = data[2], hi = data[3];
		const auto set = [&pad](bool on, uint32_t button) {
			if (on)
				pad.buttons |= 1u << button;
		};
		set(lo & 0x01, SDL_DPAD_UP);
		set(lo & 0x02, SDL_DPAD_DOWN);
		set(lo & 0x04, SDL_DPAD_LEFT);
		set(lo & 0x08, SDL_DPAD_RIGHT);
		set(lo & 0x10, SDL_START);
		set(lo & 0x20, SDL_BACK);
		set(lo & 0x40, SDL_LEFT_STICK);
		set(lo & 0x80, SDL_RIGHT_STICK);
		set(hi & 0x01, SDL_LEFT_SHOULDER);
		set(hi & 0x02, SDL_RIGHT_SHOULDER);
		set(hi & 0x04, SDL_GUIDE);
		set(hi & 0x10, SDL_SOUTH);
		set(hi & 0x20, SDL_EAST);
		set(hi & 0x40, SDL_WEST);
		set(hi & 0x80, SDL_NORTH);

		const auto s16 = [data](size_t at) { return static_cast<int16_t>(data[at] | (data[at + 1] << 8)); };
		pad.axes[SDL_AXIS_LEFT_TRIGGER] = static_cast<float>(data[4]) / 255.0f;
		pad.axes[SDL_AXIS_RIGHT_TRIGGER] = static_cast<float>(data[5]) / 255.0f;
		pad.axes[SDL_AXIS_LEFT_X] = NormalizeS16(s16(6));
		pad.axes[SDL_AXIS_LEFT_Y] = -NormalizeS16(s16(8)); // the pad's up is positive
		pad.axes[SDL_AXIS_RIGHT_X] = NormalizeS16(s16(10));
		pad.axes[SDL_AXIS_RIGHT_Y] = -NormalizeS16(s16(12));
		for (float& a : pad.axes)
			a = std::clamp(a, -1.0f, 1.0f);
		*out = pad;
		return true;
	}

	/// The rumble report for both motors (0 to 255 each).
	inline std::array<uint8_t, 8> XInputRumbleReport(uint8_t large, uint8_t small)
	{
		return {0x00, 0x08, 0x00, large, small, 0x00, 0x00, 0x00};
	}

	// ---------------------------------------------------------------- DualShock 3 / SIXAXIS

	inline bool IsDualShock3(uint16_t vendor, uint16_t product)
	{
		return vendor == 0x054c && product == 0x0268;
	}

	/// The input report: 0x01, then 48 bytes (49 in all; a report read without its id byte is 48).
	/// False for anything else.
	inline bool DecodeDualShock3Report(const uint8_t* data, size_t size, MappedPad* out)
	{
		if (size >= 49 && data[0] == 0x01)
			; // with its id
		else if (size == 48)
			data--, size++; // without: every offset below is one less
		else
			return false;

		MappedPad pad;
		pad.has_pressure = true;
		const uint8_t b2 = data[2], b3 = data[3], b4 = data[4];
		const auto set = [&pad](bool on, uint32_t button) {
			if (on)
				pad.buttons |= 1u << button;
		};
		set(b2 & 0x01, SDL_BACK); // Select
		set(b2 & 0x02, SDL_LEFT_STICK);
		set(b2 & 0x04, SDL_RIGHT_STICK);
		set(b2 & 0x08, SDL_START);
		set(b2 & 0x10, SDL_DPAD_UP);
		set(b2 & 0x20, SDL_DPAD_RIGHT);
		set(b2 & 0x40, SDL_DPAD_DOWN);
		set(b2 & 0x80, SDL_DPAD_LEFT);
		set(b3 & 0x04, SDL_LEFT_SHOULDER);
		set(b3 & 0x08, SDL_RIGHT_SHOULDER);
		set(b3 & 0x10, SDL_NORTH); // Triangle
		set(b3 & 0x20, SDL_EAST); // Circle
		set(b3 & 0x40, SDL_SOUTH); // Cross
		set(b3 & 0x80, SDL_WEST); // Square
		set(b4 & 0x01, SDL_GUIDE); // PS

		const auto stick = [](uint8_t v) { return std::clamp((static_cast<float>(v) - 128.0f) / 127.0f, -1.0f, 1.0f); };
		pad.axes[SDL_AXIS_LEFT_X] = stick(data[6]);
		pad.axes[SDL_AXIS_LEFT_Y] = stick(data[7]); // down is positive, as SDL has it
		pad.axes[SDL_AXIS_RIGHT_X] = stick(data[8]);
		pad.axes[SDL_AXIS_RIGHT_Y] = stick(data[9]);
		pad.axes[SDL_AXIS_LEFT_TRIGGER] = static_cast<float>(data[18]) / 255.0f; // L2's pressure
		pad.axes[SDL_AXIS_RIGHT_TRIGGER] = static_cast<float>(data[19]) / 255.0f; // R2's

		// The pressures, each with its button. A button whose bit is set reads at least a light press
		// (the pressure byte lags the bit by a report now and then).
		const auto press = [&pad, data](uint32_t button, size_t at) {
			const float p = static_cast<float>(data[at]) / 255.0f;
			pad.pressure[button] = (pad.buttons & (1u << button)) ? std::max(p, 1.0f / 255.0f) : 0.0f;
		};
		press(SDL_DPAD_UP, 14);
		press(SDL_DPAD_RIGHT, 15);
		press(SDL_DPAD_DOWN, 16);
		press(SDL_DPAD_LEFT, 17);
		press(SDL_LEFT_SHOULDER, 20);
		press(SDL_RIGHT_SHOULDER, 21);
		press(SDL_NORTH, 22);
		press(SDL_EAST, 23);
		press(SDL_SOUTH, 24);
		press(SDL_WEST, 25);
		*out = pad;
		return true;
	}

	/// The output report: the rumble (the small motor on or off, the large one 0 to 255) and the
	/// player's light (1 to 4), 49 bytes with its id, as Linux's hid-sony.c sends it.
	inline std::array<uint8_t, 49> DualShock3OutputReport(uint8_t large, uint8_t small, uint32_t player)
	{
		std::array<uint8_t, 49> r = {};
		r[0] = 0x01;
		r[2] = small ? 0xff : 0x00; // the small motor's duration
		r[3] = small ? 1 : 0; // and on/off
		r[4] = large ? 0xff : 0x00;
		r[5] = large;
		r[10] = static_cast<uint8_t>(1u << (std::clamp<uint32_t>(player, 1, 4))); // LED 1 is bit 1
		// Each light's blink settings: on, not blinking.
		for (size_t led = 0; led < 4; led++)
		{
			uint8_t* l = &r[11 + led * 5];
			l[0] = 0xff;
			l[1] = 0x27;
			l[2] = 0x10;
			l[3] = 0x00;
			l[4] = 0x32;
		}
		return r;
	}

	// ---------------------------------------------------------------- HID report descriptors

	/// The Generic Desktop and Simulation usages taken as axes, in the order of "Axis<n>".
	inline constexpr std::array<uint32_t, MAX_GENERIC_AXES> AXIS_USAGES = {{
		0x010030, // X
		0x010031, // Y
		0x010032, // Z
		0x010033, // Rx
		0x010034, // Ry
		0x010035, // Rz
		0x010036, // Slider
		0x010037, // Dial
		0x010038, // Wheel
		0x0200c4, // Accelerator
		0x0200c5, // Brake
		0x0200ba, // Rudder
	}};

	struct HidField
	{
		enum class Kind : uint8_t
		{
			Button,
			Axis,
			Hat,
		};

		Kind kind;
		uint8_t report_id; // 0: the device uses no report ids
		uint8_t bit_size;
		uint32_t bit_offset; // within the report, after its id byte
		int32_t logical_min;
		int32_t logical_max;
		uint32_t index; // button number from 0, or the axis's place in AXIS_USAGES
	};

	struct HidLayout
	{
		bool is_gamepad = false; // a Joystick or Game Pad application collection was there
		bool uses_report_ids = false;
		std::vector<HidField> fields;
		uint32_t button_count = 0;
		uint32_t axes_present = 0; // bit n: Axis<n>
		bool has_hat = false;
	};

	namespace detail
	{
		inline int32_t SignExtend(uint32_t value, uint32_t bytes)
		{
			if (bytes == 1)
				return static_cast<int8_t>(value);
			if (bytes == 2)
				return static_cast<int16_t>(value);
			return static_cast<int32_t>(value);
		}

		inline int AxisSlot(uint32_t usage)
		{
			for (size_t i = 0; i < AXIS_USAGES.size(); i++)
			{
				if (AXIS_USAGES[i] == usage)
					return static_cast<int>(i);
			}
			return -1;
		}
	} // namespace detail

	/// Reads a report descriptor. Only Input items inside a Joystick or Game Pad application
	/// collection are kept (a combined keyboard-and-pad device keeps only the pad); anything
	/// malformed ends the parse with what was read so far.
	inline HidLayout ParseReportDescriptor(const uint8_t* desc, size_t size)
	{
		HidLayout layout;

		struct Globals
		{
			uint32_t usage_page = 0;
			int32_t logical_min = 0;
			int32_t logical_max = 0;
			uint32_t logical_max_raw = 0;
			uint32_t logical_max_bytes = 0;
			uint32_t report_size = 0;
			uint32_t report_count = 0;
			uint8_t report_id = 0;
		};
		Globals g;
		std::vector<Globals> stack;

		std::vector<uint32_t> usages; // full usages (page << 16 | id)
		uint32_t usage_min = 0, usage_max = 0;
		bool have_range = false;

		int depth = 0;
		int gamepad_depth = -1; // depth of the Joystick/Game Pad application collection we are in
		std::array<uint32_t, 256> bit_cursor = {}; // per report id

		const auto full_usage = [&g](uint32_t value, uint32_t bytes) {
			return (bytes == 4) ? value : ((g.usage_page << 16) | (value & 0xffff));
		};

		size_t pos = 0;
		while (pos < size)
		{
			const uint8_t prefix = desc[pos++];
			if (prefix == 0xfe) // a long item: skip it
			{
				if (pos + 2 > size)
					break;
				const size_t len = desc[pos];
				pos += 2 + len;
				continue;
			}
			const uint32_t bytes = (prefix & 3) == 3 ? 4 : (prefix & 3);
			if (pos + bytes > size)
				break;
			uint32_t value = 0;
			for (uint32_t i = 0; i < bytes; i++)
				value |= static_cast<uint32_t>(desc[pos + i]) << (8 * i);
			pos += bytes;

			const uint8_t type = (prefix >> 2) & 3;
			const uint8_t tag = (prefix >> 4) & 0xf;

			if (type == 1) // global
			{
				switch (tag)
				{
					case 0x0: g.usage_page = value & 0xffff; break;
					case 0x1: g.logical_min = detail::SignExtend(value, bytes); break;
					case 0x2:
						g.logical_max = detail::SignExtend(value, bytes);
						g.logical_max_raw = value;
						g.logical_max_bytes = bytes;
						break;
					case 0x7: g.report_size = value; break;
					case 0x8:
						g.report_id = static_cast<uint8_t>(value);
						layout.uses_report_ids = true;
						break;
					case 0x9: g.report_count = value; break;
					case 0xa: stack.push_back(g); break;
					case 0xb:
						if (!stack.empty())
						{
							g = stack.back();
							stack.pop_back();
						}
						break;
					default: break;
				}
			}
			else if (type == 2) // local
			{
				switch (tag)
				{
					case 0x0: usages.push_back(full_usage(value, bytes)); break;
					case 0x1:
						usage_min = full_usage(value, bytes);
						have_range = true;
						break;
					case 0x2:
						usage_max = full_usage(value, bytes);
						have_range = true;
						break;
					default: break;
				}
			}
			else if (type == 0) // main
			{
				if (tag == 0xa) // collection
				{
					const uint32_t usage = usages.empty() ? 0 : usages.front();
					if (value == 0x01 && gamepad_depth < 0 && (usage == 0x010004 || usage == 0x010005))
					{
						gamepad_depth = depth;
						layout.is_gamepad = true;
					}
					depth++;
				}
				else if (tag == 0xc) // end collection
				{
					depth = std::max(depth - 1, 0);
					if (depth == gamepad_depth)
						gamepad_depth = -1;
				}
				else if (tag == 0x8 || tag == 0x9 || tag == 0xb) // input, output, feature
				{
					const uint64_t total_bits = static_cast<uint64_t>(g.report_size) * g.report_count;
					if (g.report_size == 0 || g.report_size > 32 || total_bits > 4096)
						break; // malformed
					uint32_t& cursor = bit_cursor[g.report_id];
					if (tag == 0x8)
					{
						const bool constant = value & 0x01;
						const bool variable = value & 0x02;
						// A logical maximum written in fewer bytes than its value needs (0xFF
						// for 255 in one byte) with a non-negative minimum is meant unsigned.
						int32_t lmax = g.logical_max;
						if (g.logical_min >= 0 && lmax < g.logical_min && g.logical_max_bytes < 4)
							lmax = static_cast<int32_t>(g.logical_max_raw);

						for (uint32_t i = 0; i < g.report_count; i++)
						{
							const uint32_t offset = cursor + i * g.report_size;
							if (constant || !variable || gamepad_depth < 0)
								continue;
							uint32_t usage = 0;
							if (i < usages.size())
								usage = usages[i];
							else if (have_range && usage_min + (i - static_cast<uint32_t>(usages.size())) <= usage_max)
								usage = usage_min + (i - static_cast<uint32_t>(usages.size()));
							else if (!usages.empty())
								usage = usages.back();
							else
								continue;

							HidField f{};
							f.report_id = g.report_id;
							f.bit_size = static_cast<uint8_t>(g.report_size);
							f.bit_offset = offset;
							f.logical_min = g.logical_min;
							f.logical_max = lmax;
							if ((usage >> 16) == 0x09 && (usage & 0xffff) >= 1)
							{
								f.kind = HidField::Kind::Button;
								f.index = (usage & 0xffff) - 1;
								if (f.index >= MAX_GENERIC_BUTTONS)
									continue;
								layout.button_count = std::max(layout.button_count, f.index + 1);
							}
							else if (usage == 0x010039)
							{
								f.kind = HidField::Kind::Hat;
								f.index = 0;
								layout.has_hat = true;
							}
							else if (const int slot = detail::AxisSlot(usage); slot >= 0)
							{
								if (lmax <= g.logical_min)
									continue;
								f.kind = HidField::Kind::Axis;
								f.index = static_cast<uint32_t>(slot);
								layout.axes_present |= 1u << slot;
							}
							else
							{
								continue;
							}
							layout.fields.push_back(f);
						}
					}
					if (tag == 0x8)
						cursor += static_cast<uint32_t>(total_bits);
				}
				usages.clear();
				have_range = false;
				usage_min = usage_max = 0;
			}
		}
		return layout;
	}

	namespace detail
	{
		inline uint32_t ReadBits(const uint8_t* data, size_t size, uint32_t offset, uint32_t bits, bool* ok)
		{
			if (bits == 0 || bits > 32 || offset + bits > size * 8)
			{
				*ok = false;
				return 0;
			}
			uint64_t value = 0;
			for (uint32_t i = 0; i < bits; i++)
			{
				const uint32_t bit = offset + i;
				if (data[bit / 8] & (1u << (bit % 8)))
					value |= 1ull << i;
			}
			*ok = true;
			return static_cast<uint32_t>(value);
		}
	} // namespace detail

	/// The decoded state of a generic pad before mapping.
	struct HidState
	{
		uint32_t buttons = 0; // bit n: Button n+1 of the Button page
		int hat = -1; // -1 centred, else 0 (up) to 7 (up-left), clockwise
		std::array<float, MAX_GENERIC_AXES> axes = {}; // -1 to 1
	};

	/// Decodes one input report. A device with report ids starts each report with its id; a report
	/// of an id the pad's fields do not use (another collection's) changes nothing and is false.
	inline bool DecodeHidReport(const HidLayout& layout, const uint8_t* data, size_t size, HidState* state)
	{
		if (size == 0)
			return false;
		uint8_t id = 0;
		if (layout.uses_report_ids)
		{
			id = data[0];
			data++;
			size--;
		}

		bool any = false;
		HidState next = *state;
		// Buttons and the hat are whole in each report of their id: start from none held.
		for (const HidField& f : layout.fields)
		{
			if (f.report_id != id)
				continue;
			if (!any)
			{
				any = true;
				next.buttons = 0;
				next.hat = -1;
			}
			bool ok = false;
			const uint32_t raw = detail::ReadBits(data, size, f.bit_offset, f.bit_size, &ok);
			if (!ok)
				continue;
			int64_t v = raw;
			if (f.logical_min < 0 && f.bit_size < 32 && (raw & (1u << (f.bit_size - 1))))
				v = static_cast<int64_t>(raw) - (int64_t{1} << f.bit_size); // signed field

			switch (f.kind)
			{
				case HidField::Kind::Button:
					if (v != 0 && f.index < MAX_GENERIC_BUTTONS)
						next.buttons |= 1u << f.index;
					break;
				case HidField::Kind::Hat:
				{
					const int64_t range = static_cast<int64_t>(f.logical_max) - f.logical_min;
					const int64_t pos = v - f.logical_min;
					if (pos < 0 || pos > range)
						next.hat = -1; // the "null" value: centred
					else if (range == 7)
						next.hat = static_cast<int>(pos);
					else if (range == 3)
						next.hat = static_cast<int>(pos * 2); // four-way hats
					else
						next.hat = -1;
					break;
				}
				case HidField::Kind::Axis:
				{
					const double span = static_cast<double>(f.logical_max) - f.logical_min;
					const double t = (static_cast<double>(std::clamp<int64_t>(v, f.logical_min, f.logical_max)) - f.logical_min) / span;
					if (f.index < MAX_GENERIC_AXES)
						next.axes[f.index] = static_cast<float>(t * 2.0 - 1.0);
					break;
				}
			}
		}
		if (any)
			*state = next;
		return any;
	}

	/// A generic pad's state in the input source's terms. Triggers come from Accelerator/Brake
	/// (or, with the right stick on Z/Rz, from Rx/Ry when the pad also has those, which is how
	/// several pads report analog triggers).
	inline MappedPad MapHid(const HidLayout& layout, const HidState& s)
	{
		MappedPad pad;
		pad.generic_buttons = s.buttons;
		pad.generic_axes = s.axes;
		pad.generic_axes_present = layout.axes_present;

		static constexpr uint32_t hat_bits[8] = {
			(1u << SDL_DPAD_UP),
			(1u << SDL_DPAD_UP) | (1u << SDL_DPAD_RIGHT),
			(1u << SDL_DPAD_RIGHT),
			(1u << SDL_DPAD_DOWN) | (1u << SDL_DPAD_RIGHT),
			(1u << SDL_DPAD_DOWN),
			(1u << SDL_DPAD_DOWN) | (1u << SDL_DPAD_LEFT),
			(1u << SDL_DPAD_LEFT),
			(1u << SDL_DPAD_UP) | (1u << SDL_DPAD_LEFT),
		};
		if (s.hat >= 0 && s.hat < 8)
			pad.buttons |= hat_bits[s.hat];

		const auto has = [&layout](uint32_t slot) { return (layout.axes_present & (1u << slot)) != 0; };
		constexpr uint32_t X = 0, Y = 1, Z = 2, RX = 3, RY = 4, RZ = 5, ACCEL = 9, BRAKE = 10;
		if (has(X))
			pad.axes[SDL_AXIS_LEFT_X] = s.axes[X];
		if (has(Y))
			pad.axes[SDL_AXIS_LEFT_Y] = s.axes[Y];
		const bool z_rz = has(Z) && has(RZ);
		if (z_rz)
		{
			pad.axes[SDL_AXIS_RIGHT_X] = s.axes[Z];
			pad.axes[SDL_AXIS_RIGHT_Y] = s.axes[RZ];
		}
		else if (has(RX) && has(RY))
		{
			pad.axes[SDL_AXIS_RIGHT_X] = s.axes[RX];
			pad.axes[SDL_AXIS_RIGHT_Y] = s.axes[RY];
		}
		const auto trigger = [](float v) { return std::clamp((v + 1.0f) * 0.5f, 0.0f, 1.0f); };
		if (has(BRAKE))
			pad.axes[SDL_AXIS_LEFT_TRIGGER] = trigger(s.axes[BRAKE]);
		else if (z_rz && has(RX))
			pad.axes[SDL_AXIS_LEFT_TRIGGER] = trigger(s.axes[RX]);
		if (has(ACCEL))
			pad.axes[SDL_AXIS_RIGHT_TRIGGER] = trigger(s.axes[ACCEL]);
		else if (z_rz && has(RY))
			pad.axes[SDL_AXIS_RIGHT_TRIGGER] = trigger(s.axes[RY]);
		return pad;
	}

	/// What kind of pad an interface is, from its descriptors. Sony's controllers are left to the
	/// console's pad service (they are the scePad players already), but for the DualShock 3, which the
	/// console does not take (see ClassifyDevice).
	enum class Protocol : uint8_t
	{
		None,
		XInput,
		Hid,
		DualShock3,
	};

	inline Protocol Classify(uint16_t vendor, uint8_t interface_class, uint8_t subclass, uint8_t protocol,
		const HidLayout* layout_if_hid)
	{
		if (vendor == 0x054c)
			return Protocol::None;
		if (IsXInputInterface(interface_class, subclass, protocol))
			return Protocol::XInput;
		// HID boot keyboards and mice (subclass 1) are not pads; neither is a HID device whose
		// descriptor has no joystick or pad in it.
		if (interface_class == 0x03 && !(subclass == 0x01 && (protocol == 0x01 || protocol == 0x02)) && layout_if_hid &&
			layout_if_hid->is_gamepad && !layout_if_hid->fields.empty())
			return Protocol::Hid;
		return Protocol::None;
	}

	/// Classify with the product too: a DualShock 3 / SIXAXIS is one of the console's own vendor
	/// that is read here.
	inline Protocol ClassifyDevice(uint16_t vendor, uint16_t product, uint8_t interface_class, uint8_t subclass, uint8_t protocol,
		const HidLayout* layout_if_hid)
	{
		if (IsDualShock3(vendor, product) && interface_class == 0x03)
			return Protocol::DualShock3;
		return Classify(vendor, interface_class, subclass, protocol, layout_if_hid);
	}
} // namespace PS5UsbPadLogic
