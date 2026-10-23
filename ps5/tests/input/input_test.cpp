// Crux2 - the thinking behind the controller features, on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The leave-the-game hold and its buzz, pressure from R2, aiming with the motion sensor
// (ps5_input_source_logic.h), the USB keyboard's reports and key names (ps5_keyboard_logic.h),
// and USB pads: the HID report descriptor parser, the reports' decoding and the hot-plug
// bookkeeping over a made-up transport (ps5_usbpad_logic.h, ps5_usbpad.cpp).

#include "ps5_input_source_logic.h"
#include "ps5_keyboard_logic.h"
#include "ps5_usbpad.h"
#include "ps5_usbpad_logic.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <thread>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static bool Near(float a, float b, float eps = 1e-3f)
{
	return std::fabs(a - b) <= eps;
}

// ------------------------------------------------------------------------------ the hold

static void Combo()
{
	using namespace PS5InputLogic;
	constexpr uint32_t L3 = 1u << 7, R3 = 1u << 8, L1 = 1u << 9, R1 = 1u << 10, CROSS = 1u << 0;
	CHECK(LeaveComboMask("L3R3") == (L3 | R3));
	CHECK(LeaveComboMask("L1R1L3R3") == (L1 | R1 | L3 | R3));
	CHECK(LeaveComboMask("Off") == 0);
	CHECK(LeaveComboMask("") == 0);
	CHECK(LeaveComboMask("nonsense") == 0);
	CHECK(LeaveHoldMicroseconds(2.0f) == 2000000);
	CHECK(LeaveHoldMicroseconds(0.0f) == 500000);
	CHECK(LeaveHoldMicroseconds(99.0f) == 5000000);
	CHECK(LeaveHoldMicroseconds(std::nanf("")) == 2000000);

	HoldCombo c;
	c.Configure(L3 | R3, 2000000);
	uint64_t t = 1000000;
	// Nothing held, then one of the two: nothing.
	CHECK(!c.Update(0, t));
	CHECK(!c.Update(L3, t += 500000));
	CHECK(!c.Holding());
	// Both: the hold starts; it fires at 2 s and not before; extra buttons do not matter.
	const uint64_t start = t += 10000;
	CHECK(!c.Update(L3 | R3, start));
	CHECK(c.Holding());
	CHECK(!c.Update(L3 | R3 | CROSS, start + 1000000));
	CHECK(Near(c.Progress(start + 1000000), 0.5f));
	CHECK(!c.Update(L3 | R3, start + 1999999));
	CHECK(c.Update(L3 | R3, start + 2000000));
	CHECK(c.Progress(start + 2000000) == 0.0f);
	// Once per hold: holding on fires nothing more.
	CHECK(!c.Update(L3 | R3, start + 5000000));
	CHECK(!c.Update(L3 | R3, start + 9000000));
	// Let go and hold again: fires again after another 2 s.
	CHECK(!c.Update(R3, start + 9100000));
	CHECK(!c.Update(L3 | R3, start + 9200000));
	CHECK(c.Update(L3 | R3, start + 11200000));

	// Release cancels: a hold broken at 1.9 s and taken again starts from nothing.
	HoldCombo d;
	d.Configure(L3 | R3, 2000000);
	CHECK(!d.Update(0, 0));
	CHECK(!d.Update(L3 | R3, 100));
	CHECK(!d.Update(L3 | R3, 1900100));
	CHECK(!d.Update(L3, 1900200)); // R3 let go
	CHECK(!d.Holding());
	CHECK(!d.Update(L3 | R3, 1900300));
	CHECK(!d.Update(L3 | R3, 3900299));
	CHECK(d.Update(L3 | R3, 3900300));

	// A combination already held when configured (held through the menu into the game) does
	// nothing until it is let go.
	HoldCombo e;
	e.Configure(L3 | R3, 1000000);
	CHECK(!e.Update(L3 | R3, 0));
	CHECK(!e.Update(L3 | R3, 5000000));
	CHECK(!e.Holding());
	CHECK(!e.Update(0, 5000001));
	CHECK(!e.Update(L3 | R3, 5000002));
	CHECK(e.Update(L3 | R3, 6000002));

	// Cancel (the game stopped) forgets a running hold and wants a release first.
	HoldCombo f;
	f.Configure(L3 | R3, 1000000);
	CHECK(!f.Update(0, 0));
	CHECK(!f.Update(L3 | R3, 10));
	f.Cancel();
	CHECK(!f.Update(L3 | R3, 2000000));
	CHECK(!f.Holding());

	// Off: never.
	HoldCombo g;
	g.Configure(0, 1000000);
	CHECK(!g.Update(0, 0));
	CHECK(!g.Update(0xffffffffu, 1));
	CHECK(!g.Update(0xffffffffu, 9000000));

	// Configuring the same again changes nothing (it is called with every sample).
	HoldCombo h;
	h.Configure(L3 | R3, 1000000);
	CHECK(!h.Update(0, 0));
	CHECK(!h.Update(L3 | R3, 10));
	h.Configure(L3 | R3, 1000000);
	CHECK(h.Update(L3 | R3, 1000010));
}

static void Buzz()
{
	using namespace PS5InputLogic;
	DoubleBuzz b;
	uint8_t s = 99;
	CHECK(!b.Active());
	CHECK(!b.Sample(0, &s) && s == 0);
	b.Start(1000);
	CHECK(b.Active());
	CHECK(b.Sample(1000, &s) && s == DoubleBuzz::STRENGTH);
	CHECK(b.Sample(1000 + DoubleBuzz::PULSE_US - 1, &s) && s == DoubleBuzz::STRENGTH);
	CHECK(b.Sample(1000 + DoubleBuzz::PULSE_US, &s) && s == 0);
	CHECK(b.Sample(1000 + DoubleBuzz::PULSE_US + DoubleBuzz::GAP_US - 1, &s) && s == 0);
	CHECK(b.Sample(1000 + DoubleBuzz::PULSE_US + DoubleBuzz::GAP_US, &s) && s == DoubleBuzz::STRENGTH);
	// Two pulses with a gap: count the on/off edges over the whole buzz at 1 ms steps.
	DoubleBuzz c;
	c.Start(0);
	int rises = 0;
	bool was_on = false;
	uint64_t t = 0;
	while (c.Sample(t, &s))
	{
		const bool on = s != 0;
		rises += (on && !was_on);
		was_on = on;
		t += 1000;
	}
	CHECK(rises == 2);
	CHECK(t == 2 * DoubleBuzz::PULSE_US + DoubleBuzz::GAP_US);
	CHECK(!c.Active());
	CHECK(!c.Sample(t + 1000000, &s) && s == 0);
}

static void Pressure()
{
	using PS5InputLogic::TriggerPressure;
	CHECK(TriggerPressure(0) == 1.0f); // at rest: a full press, play unchanged
	CHECK(TriggerPressure(12) == 1.0f); // the rest's slack
	CHECK(Near(TriggerPressure(13), 0.051f, 0.002f)); // just pulled: light (above the floor)
	CHECK(Near(TriggerPressure(128), 128.0f / 255.0f));
	CHECK(TriggerPressure(255) == 1.0f);
	CHECK(TriggerPressure(13, 12, 0.2f) == 0.2f); // the floor
	// Deeper is never lighter once pulled.
	float last = 0.0f;
	for (int v = 13; v <= 255; v++)
	{
		const float p = TriggerPressure(static_cast<uint8_t>(v));
		CHECK(p >= last && p > 0.0f && p <= 1.0f);
		last = p;
	}
}

static void Gyro()
{
	using namespace PS5InputLogic;
	constexpr float DEG = 0.01745329252f;

	GyroAimSettings settings;
	settings.degrees_to_edge = 20.0f;
	settings.dead_zone_dps = 0.0f;
	settings.smoothing_s = 0.0f;
	settings.aspect = 0.5f;

	// Turning left (positive yaw) 10 degrees a second for a second: half way to the left edge.
	GyroAim aim;
	aim.SetSettings(settings);
	for (int i = 0; i < 250; i++)
		aim.Feed(0.0f, 10.0f * DEG, 1.0f / 250.0f);
	CHECK(Near(aim.X(), -0.5f, 0.002f));
	CHECK(Near(aim.Y(), 0.0f));
	// Turning back right the same: the middle again.
	for (int i = 0; i < 250; i++)
		aim.Feed(0.0f, -10.0f * DEG, 1.0f / 250.0f);
	CHECK(Near(aim.X(), 0.0f, 0.002f));
	// Tilting up 5 degrees: up and down use the same degrees per pixel, so with half the height
	// 10 degrees reach the top: half way up.
	for (int i = 0; i < 100; i++)
		aim.Feed(5.0f * DEG, 0.0f, 0.01f);
	CHECK(Near(aim.Y(), -0.5f, 0.002f));
	// The edge holds the pointer; turning back from past it moves at once (no wind-up).
	for (int i = 0; i < 100; i++)
		aim.Feed(0.0f, 100.0f * DEG, 0.01f);
	CHECK(aim.X() == -1.0f);
	aim.Feed(0.0f, -10.0f * DEG, 0.1f);
	CHECK(Near(aim.X(), -0.95f, 0.002f));
	// Recenter.
	aim.Recenter();
	CHECK(aim.X() == 0.0f && aim.Y() == 0.0f);

	// The dead zone: slower than it does nothing; faster moves by what is above it.
	settings.dead_zone_dps = 2.0f;
	aim.SetSettings(settings);
	for (int i = 0; i < 1000; i++)
		aim.Feed(1.9f * DEG, -1.9f * DEG, 0.004f);
	CHECK(aim.X() == 0.0f && aim.Y() == 0.0f);
	aim.Feed(0.0f, -12.0f * DEG, 1.0f / 10.0f); // 10 deg/s above it, for a tenth: 1 degree
	CHECK(Near(aim.X(), 1.0f / 20.0f, 0.001f));

	// Bad samples move nothing: no time, negative time, a long gap, a NaN.
	const float before = aim.X();
	aim.Feed(0.0f, 50.0f * DEG, 0.0f);
	aim.Feed(0.0f, 50.0f * DEG, -0.01f);
	aim.Feed(0.0f, 50.0f * DEG, 0.5f);
	aim.Feed(0.0f, std::nanf(""), 0.01f);
	aim.Feed(std::numeric_limits<float>::infinity(), 0.0f, 0.01f);
	CHECK(aim.X() == before);
	CHECK(std::isfinite(aim.Y()));

	// Smoothing: the pointer follows a step with the time constant, and arrives.
	settings.dead_zone_dps = 0.0f;
	settings.smoothing_s = 0.02f;
	GyroAim smooth;
	smooth.SetSettings(settings);
	smooth.Feed(0.0f, -100.0f * DEG, 0.04f); // raw jumps to 0.2 in one sample
	CHECK(smooth.X() > 0.0f && smooth.X() < 0.2f);
	CHECK(Near(smooth.X(), 0.2f * (1.0f - std::exp(-2.0f)), 0.001f));
	for (int i = 0; i < 100; i++)
		smooth.Feed(0.0f, 0.0f, 0.004f);
	CHECK(Near(smooth.X(), 0.2f, 0.001f));
	// Shaking at a high rate is calmed: the raw position swings 0.006 back and forth with each
	// sample; once settled, the smoothed one moves far less.
	GyroAim shaky;
	settings.smoothing_s = 0.03f;
	shaky.SetSettings(settings);
	float low = 1.0f, high = -1.0f;
	for (int i = 0; i < 400; i++)
	{
		shaky.Feed(0.0f, ((i & 1) ? 30.0f : -30.0f) * DEG, 0.004f);
		if (i >= 200)
		{
			low = std::min(low, shaky.X());
			high = std::max(high, shaky.X());
		}
	}
	CHECK(high - low < 0.002f);

	// Inverted axes.
	settings.smoothing_s = 0.0f;
	settings.invert_x = settings.invert_y = true;
	GyroAim inv;
	inv.SetSettings(settings);
	inv.Feed(2.0f * DEG, 2.0f * DEG, 0.1f);
	CHECK(inv.X() > 0.0f && inv.Y() > 0.0f);

	// The sample interval.
	CHECK(SampleInterval(0, 1000, 0.004f) == 0.004f);
	CHECK(Near(SampleInterval(1000, 5000, 0.001f), 0.004f));
	CHECK(SampleInterval(5000, 5000, 0.004f) == 0.004f);
	CHECK(SampleInterval(5000, 1000, 0.004f) == 0.004f);
	CHECK(SampleInterval(1000, 1000000, 0.004f) == 0.004f); // a gap
}

// ------------------------------------------------------------------------------ the keyboard

static std::array<uint8_t, 96> KeyboardState(bool connected, uint8_t modifiers, std::initializer_list<uint16_t> keys)
{
	std::array<uint8_t, 96> s = {};
	s[0] = 0x11; // a timestamp
	s[16] = connected ? 1 : 0;
	const uint32_t count = static_cast<uint32_t>(keys.size());
	std::memcpy(&s[20], &count, 4);
	const uint32_t mods = modifiers;
	std::memcpy(&s[28], &mods, 4);
	size_t i = 0;
	for (const uint16_t k : keys)
	{
		s[32 + 2 * i] = static_cast<uint8_t>(k);
		s[33 + 2 * i] = static_cast<uint8_t>(k >> 8);
		i++;
	}
	return s;
}

static void KeyboardNames()
{
	using namespace PS5KeyboardLogic;
	// Every name gives back its code, and no two codes share one.
	std::map<std::string, uint32_t> seen;
	for (const KeyName& k : KEY_NAMES)
	{
		CHECK(CodeOf(k.name) == k.usage);
		CHECK(NameOf(k.usage) != nullptr && std::strcmp(NameOf(k.usage), k.name) == 0);
		CHECK(seen.emplace(k.name, k.usage).second);
	}
	CHECK(CodeOf("Return") == 0x28u);
	CHECK(CodeOf("Enter") == 0x28u);
	CHECK(CodeOf("Shift_r") == 0xe5u);
	CHECK(CodeOf("Usage200") == 200u);
	CHECK(!CodeOf("Usage999").has_value());
	CHECK(!CodeOf("Usage3").has_value());
	CHECK(!CodeOf("Usage1x").has_value());
	CHECK(!CodeOf("").has_value());
	CHECK(!CodeOf("NoSuchKey").has_value());
	CHECK(NameOrUsage(0x04) == "A");
	CHECK(NameOrUsage(0xc8) == "Usage200");
	CHECK(CodeOf(NameOrUsage(0xc8)) == 0xc8u);

	CHECK(Character(0x04, false) == 'a');
	CHECK(Character(0x04, true) == 'A');
	CHECK(Character(0x1e, false) == '1');
	CHECK(Character(0x1e, true) == '!');
	CHECK(Character(0x27, true) == ')');
	CHECK(Character(0x2c, false) == ' ');
	CHECK(Character(0x38, true) == '?');
	CHECK(Character(0x34, true) == '"');
	CHECK(Character(0x32, false) == 0); // non-US #: not on a US keyboard
	CHECK(Character(0x59, false) == '1');
	CHECK(Character(0x62, true) == '0');
	CHECK(Character(0x28, false) == 0);
	CHECK(Character(0x3a, false) == 0); // F1
}

static void KeyboardReports()
{
	using namespace PS5KeyboardLogic;
	// The console's state.
	{
		auto s = KeyboardState(true, 0x02, {0x04, 0x05, 0xe0, 0x1234});
		const Report r = ParseState(s.data(), s.size());
		CHECK(r.connected);
		CHECK(r.modifiers == (0x02 | 0x01)); // Shift, and Ctrl listed as a key
		CHECK(r.keys[0] == 0x04 && r.keys[1] == 0x05 && r.keys[2] == 0);
		auto off = KeyboardState(false, 0x02, {0x04});
		CHECK(!ParseState(off.data(), off.size()).connected);
		CHECK(!ParseState(s.data(), 95).connected); // too short
		auto many = KeyboardState(true, 0, {});
		const int32_t bogus = 1000;
		std::memcpy(&many[20], &bogus, 4);
		const Report m = ParseState(many.data(), many.size()); // a count past 16 reads 16
		CHECK(m.connected);
		const int32_t negative = -5;
		std::memcpy(&many[20], &negative, 4);
		CHECK(ParseState(many.data(), many.size()).keys[0] == 0);
	}

	Tracker t;
	std::vector<KeyEvent> ev;
	Typed typed;
	const auto take = [&](std::initializer_list<uint8_t> keys, uint8_t mods, double now, bool connected = true) {
		Report r;
		r.connected = connected;
		r.modifiers = mods;
		size_t i = 0;
		for (const uint8_t k : keys)
			r.keys[i++] = k;
		ev.clear();
		typed = {};
		t.Take(r, now, &ev, &typed);
	};

	take({0x0b}, 0, 0.0); // h
	CHECK(ev.size() == 1 && ev[0].code == 0x0b && ev[0].down);
	CHECK(typed.text == "h");
	take({0x0b}, 0, 0.01); // still held: nothing new, a letter does not repeat
	CHECK(ev.empty() && typed.text.empty());
	take({0x0b}, 0, 2.0);
	CHECK(typed.text.empty());
	take({}, 0, 2.1);
	CHECK(ev.size() == 1 && ev[0].code == 0x0b && !ev[0].down);

	take({0x0c}, 0x20, 3.0); // right Shift + i
	CHECK(ev.size() == 2 && ev[0].code == 0xe5 && ev[0].down && ev[1].code == 0x0c);
	CHECK(typed.text == "I");
	take({}, 0, 3.1);
	CHECK(ev.size() == 2); // both up
	CHECK(ev[0].code == 0xe5 && !ev[0].down);

	// Caps Lock turns letters, not digits.
	take({0x39}, 0, 4.0);
	CHECK(t.CapsLock());
	take({}, 0, 4.1);
	take({0x04, 0x1e}, 0, 4.2);
	CHECK(typed.text == "A1");
	take({}, 0, 4.3);
	take({0x04}, 0x02, 4.4); // Shift with Caps Lock: lower case
	CHECK(typed.text == "a");
	take({}, 0, 4.5);
	take({0x39}, 0, 4.6);
	take({}, 0, 4.7);
	CHECK(!t.CapsLock());

	// Ctrl+C is not text, but is still a key event.
	take({0x06}, 0x01, 5.0);
	CHECK(typed.text.empty());
	CHECK(ev.size() == 2);
	take({}, 0, 5.1);

	// Special keys.
	take({0x28}, 0, 6.0);
	CHECK(typed.enter == 1);
	take({0x58, 0x29}, 0, 6.1); // keypad Enter (Return let go) and Escape
	CHECK(typed.enter == 1 && typed.escape == 1);
	take({}, 0, 6.2);
	take({0x2b, 0x4a, 0x4d, 0x4c}, 0, 6.3);
	CHECK(typed.tab == 1 && typed.home == 1 && typed.end == 1 && typed.del == 1);
	take({}, 0, 6.4);

	// Backspace repeats after the delay, then at the interval; letting go stops it.
	take({0x2a}, 0, 10.0);
	CHECK(typed.backspace == 1);
	take({0x2a}, 0, 10.0 + Tracker::REPEAT_DELAY - 0.01);
	CHECK(typed.backspace == 0);
	take({0x2a}, 0, 10.0 + Tracker::REPEAT_DELAY);
	CHECK(typed.backspace == 1);
	int repeats = 0;
	for (int i = 1; i <= 10; i++)
	{
		take({0x2a}, 0, 10.0 + Tracker::REPEAT_DELAY + i * Tracker::REPEAT_INTERVAL + 1e-6);
		repeats += typed.backspace;
	}
	CHECK(repeats == 10);
	take({}, 0, 12.0);
	take({}, 0, 13.0);
	CHECK(typed.backspace == 0);
	// The arrows repeat too.
	take({0x50}, 0, 20.0);
	take({0x50}, 0, 21.0);
	CHECK(typed.left == 1);
	take({}, 0, 21.1);

	// The report's error codes (1 to 3: too many keys) are not keys.
	take({0x01, 0x01, 0x01}, 0, 22.0);
	CHECK(ev.empty() && !typed.Any());

	// Unplugging lets go of everything held, modifiers included.
	take({0x04, 0x05}, 0x04, 23.0);
	CHECK(ev.size() == 3);
	take({0x04}, 0, 23.1, false);
	CHECK(ev.size() == 3);
	for (const KeyEvent& e : ev)
		CHECK(!e.down);

	// A key let go and pressed again: one event each time.
	take({0x07}, 0, 30.0);
	take({}, 0, 30.1);
	take({0x07}, 0, 30.2);
	CHECK(ev.size() == 1 && ev[0].down);

	// The queue the interface drains stays bounded.
	Typed queue;
	Typed chunk;
	chunk.text = std::string(100, 'x');
	chunk.backspace = 40;
	for (int i = 0; i < 10; i++)
		queue.Append(chunk);
	CHECK(queue.text.size() == 256);
	CHECK(queue.backspace == 64);
	Typed small;
	small.text = "ab";
	small.enter = 1;
	Typed q2;
	q2.Append(small);
	q2.Append(small);
	CHECK(q2.text == "abab" && q2.enter == 2);
}

// ------------------------------------------------------------------------------ USB pads

static void XInput()
{
	using namespace PS5UsbPadLogic;
	CHECK(IsXInputInterface(0xff, 0x5d, 0x01));
	CHECK(!IsXInputInterface(0xff, 0x5d, 0x03)); // the headset interface
	CHECK(!IsXInputInterface(0x03, 0x00, 0x00));

	uint8_t r[20] = {0x00, 0x14};
	r[2] = 0x01 | 0x10 | 0x80; // up, Start, right stick click
	r[3] = 0x10 | 0x80 | 0x04; // A, Y, Guide
	r[4] = 255; // left trigger
	r[5] = 0;
	const auto put = [&r](size_t at, int16_t v) {
		r[at] = static_cast<uint8_t>(v & 0xff);
		r[at + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
	};
	put(6, 32767);
	put(8, 32767); // up
	put(10, -32768);
	put(12, 0);
	MappedPad p;
	CHECK(DecodeXInputReport(r, sizeof(r), &p));
	CHECK(p.buttons == ((1u << SDL_DPAD_UP) | (1u << SDL_START) | (1u << SDL_RIGHT_STICK) | (1u << SDL_SOUTH) |
						   (1u << SDL_NORTH) | (1u << SDL_GUIDE)));
	CHECK(p.axes[SDL_AXIS_LEFT_TRIGGER] == 1.0f && p.axes[SDL_AXIS_RIGHT_TRIGGER] == 0.0f);
	CHECK(p.axes[SDL_AXIS_LEFT_X] == 1.0f);
	CHECK(p.axes[SDL_AXIS_LEFT_Y] == -1.0f); // up is negative here
	CHECK(p.axes[SDL_AXIS_RIGHT_X] == -1.0f);
	CHECK(p.axes[SDL_AXIS_RIGHT_Y] == 0.0f);
	CHECK(p.generic_buttons == 0);

	// Not input: an LED status report, a short one, another type.
	const uint8_t led[3] = {0x01, 0x03, 0x06};
	CHECK(!DecodeXInputReport(led, sizeof(led), &p));
	CHECK(!DecodeXInputReport(r, 13, &p));
	uint8_t other[20] = {0x08, 0x14};
	CHECK(!DecodeXInputReport(other, sizeof(other), &p));

	const auto rumble = XInputRumbleReport(0x80, 0x40);
	CHECK(rumble[0] == 0 && rumble[1] == 8 && rumble[3] == 0x80 && rumble[4] == 0x40);
}

// A common generic USB pad: report id 1, 12 buttons, a 4-bit hat with a null state, then X, Y, Z,
// Rz as bytes 0 to 255 (Logical Maximum written as 0xFF in one byte).
static const uint8_t s_generic_pad[] = {
	0x05, 0x01, // Usage Page (Generic Desktop)
	0x09, 0x05, // Usage (Game Pad)
	0xa1, 0x01, // Collection (Application)
	0x85, 0x01, //   Report ID (1)
	0x05, 0x09, //   Usage Page (Button)
	0x19, 0x01, //   Usage Minimum (1)
	0x29, 0x0c, //   Usage Maximum (12)
	0x15, 0x00, //   Logical Minimum (0)
	0x25, 0x01, //   Logical Maximum (1)
	0x75, 0x01, //   Report Size (1)
	0x95, 0x0c, //   Report Count (12)
	0x81, 0x02, //   Input (Data, Variable, Absolute)
	0x05, 0x01, //   Usage Page (Generic Desktop)
	0x09, 0x39, //   Usage (Hat switch)
	0x15, 0x00, //   Logical Minimum (0)
	0x25, 0x07, //   Logical Maximum (7)
	0x75, 0x04, //   Report Size (4)
	0x95, 0x01, //   Report Count (1)
	0x81, 0x42, //   Input (Data, Variable, Absolute, Null state)
	0x09, 0x30, //   Usage (X)
	0x09, 0x31, //   Usage (Y)
	0x09, 0x32, //   Usage (Z)
	0x09, 0x35, //   Usage (Rz)
	0x15, 0x00, //   Logical Minimum (0)
	0x25, 0xff, //   Logical Maximum (255, written in one byte)
	0x75, 0x08, //   Report Size (8)
	0x95, 0x04, //   Report Count (4)
	0x81, 0x02, //   Input (Data, Variable, Absolute)
	0x09, 0x01, //   Usage (vendor-ish): an output report, ignored
	0x91, 0x02, //   Output
	0xc0, // End Collection
};

// A flight-stick-like joystick without report ids: 16-bit signed X/Y/Rz, a slider, 8 buttons and a
// four-way hat 1 to 4.
static const uint8_t s_joystick[] = {
	0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, // Generic Desktop, Joystick, Collection (Application)
	0xa1, 0x00, //   Collection (Physical)
	0x09, 0x30, 0x09, 0x31, 0x09, 0x35, //   X, Y, Rz
	0x16, 0x00, 0x80, //   Logical Minimum (-32768)
	0x26, 0xff, 0x7f, //   Logical Maximum (32767)
	0x75, 0x10, 0x95, 0x03, 0x81, 0x02, //   16 bits x 3, Input
	0x09, 0x36, //   Slider
	0x15, 0x00, 0x26, 0xff, 0x00, //   0 to 255
	0x75, 0x08, 0x95, 0x01, 0x81, 0x02, //   8 bits, Input
	0xc0, //   End Collection
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, // 8 buttons
	0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x04, 0x75, 0x08, 0x95, 0x01, 0x81, 0x42, // hat 1..4
	0xc0,
};

// A keyboard and a pad in one device: the keyboard (report id 1) must not become buttons.
static const uint8_t s_combo_device[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, // Keyboard
	0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0,
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, // Game Pad
	0x85, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
	0x95, 0x04, 0x81, 0x03, // padding
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, // X, Y -127..127
	0xc0,
};

static void Hid()
{
	using namespace PS5UsbPadLogic;
	{
		const HidLayout l = ParseReportDescriptor(s_generic_pad, sizeof(s_generic_pad));
		CHECK(l.is_gamepad);
		CHECK(l.uses_report_ids);
		CHECK(l.button_count == 12);
		CHECK(l.has_hat);
		CHECK(l.axes_present == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5)));
		CHECK(l.fields.size() == 12 + 1 + 4);
		CHECK(Classify(0x0079, 0x03, 0x00, 0x00, &l) == Protocol::Hid);
		CHECK(Classify(0x054c, 0x03, 0x00, 0x00, &l) == Protocol::None); // Sony: scePad's
		CHECK(Classify(0x0079, 0x03, 0x01, 0x01, &l) == Protocol::None); // a boot keyboard

		// Buttons 1 and 12, hat right (2), X 0, Y 255, Z 128, Rz 64.
		const uint8_t report[] = {0x01, 0x01, 0x08 | 0x20, 0, 255, 128, 64};
		HidState s;
		CHECK(DecodeHidReport(l, report, sizeof(report), &s));
		CHECK(s.buttons == ((1u << 0) | (1u << 11)));
		// byte 2: bits 0-3 are buttons 9-12 (0x08 = button 12), bits 4-7 the hat (0x2 = right).
		CHECK(s.hat == 2);
		CHECK(s.axes[0] == -1.0f);
		CHECK(s.axes[1] == 1.0f);
		CHECK(Near(s.axes[2], 128.0f / 255.0f * 2.0f - 1.0f));
		const MappedPad m = MapHid(l, s);
		CHECK(m.buttons == (1u << SDL_DPAD_RIGHT));
		CHECK(m.generic_buttons == s.buttons);
		CHECK(m.axes[SDL_AXIS_LEFT_X] == -1.0f && m.axes[SDL_AXIS_LEFT_Y] == 1.0f);
		CHECK(Near(m.axes[SDL_AXIS_RIGHT_X], s.axes[2]) && Near(m.axes[SDL_AXIS_RIGHT_Y], s.axes[5]));
		CHECK(m.axes[SDL_AXIS_LEFT_TRIGGER] == 0.0f && m.axes[SDL_AXIS_RIGHT_TRIGGER] == 0.0f);

		// The hat's null state (15): centred; every diagonal maps to two directions.
		const uint8_t centred[] = {0x01, 0x00, 0xf0, 128, 128, 128, 128};
		CHECK(DecodeHidReport(l, centred, sizeof(centred), &s));
		CHECK(s.hat == -1 && s.buttons == 0);
		CHECK(MapHid(l, s).buttons == 0);
		const uint8_t up_left[] = {0x01, 0x00, 0x70, 128, 128, 128, 128};
		CHECK(DecodeHidReport(l, up_left, sizeof(up_left), &s));
		CHECK(MapHid(l, s).buttons == ((1u << SDL_DPAD_UP) | (1u << SDL_DPAD_LEFT)));

		// Another report id, an empty report, a truncated one: nothing changes, nothing crashes.
		const HidState keep = s;
		const uint8_t other_id[] = {0x05, 0xff, 0xff, 0xff};
		CHECK(!DecodeHidReport(l, other_id, sizeof(other_id), &s));
		CHECK(s.buttons == keep.buttons && s.hat == keep.hat);
		CHECK(!DecodeHidReport(l, report, 0, &s));
		const uint8_t truncated[] = {0x01, 0xff};
		CHECK(DecodeHidReport(l, truncated, sizeof(truncated), &s)); // the buttons it has are read
		CHECK(s.buttons == 0xff);
	}
	{
		const HidLayout l = ParseReportDescriptor(s_joystick, sizeof(s_joystick));
		CHECK(l.is_gamepad && !l.uses_report_ids);
		CHECK(l.button_count == 8);
		CHECK(l.axes_present == ((1u << 0) | (1u << 1) | (1u << 5) | (1u << 6)));
		// X -32768, Y 32767, Rz 0, slider 255, buttons 0x81, hat 3 (of 1..4: down)
		const uint8_t report[] = {0x00, 0x80, 0xff, 0x7f, 0x00, 0x00, 0xff, 0x81, 0x03};
		HidState s;
		CHECK(DecodeHidReport(l, report, sizeof(report), &s));
		CHECK(s.axes[0] == -1.0f && s.axes[1] == 1.0f);
		CHECK(Near(s.axes[5], 32768.0f / 65535.0f * 2.0f - 1.0f));
		CHECK(s.axes[6] == 1.0f);
		CHECK(s.buttons == 0x81);
		CHECK(s.hat == 4); // four-way: down
		const MappedPad m = MapHid(l, s);
		CHECK(m.buttons == (1u << SDL_DPAD_DOWN));
		CHECK(m.axes[SDL_AXIS_RIGHT_X] == 0.0f); // Rz alone is no right stick
		CHECK(m.generic_axes_present == l.axes_present);
		const uint8_t null_hat[] = {0, 0, 0, 0, 0, 0, 0, 0, 0x00};
		CHECK(DecodeHidReport(l, null_hat, sizeof(null_hat), &s));
		CHECK(s.hat == -1);
	}
	{
		const HidLayout l = ParseReportDescriptor(s_combo_device, sizeof(s_combo_device));
		CHECK(l.is_gamepad && l.uses_report_ids);
		CHECK(l.button_count == 4);
		for (const HidField& f : l.fields)
			CHECK(f.report_id == 2);
		const uint8_t keys[] = {0x01, 0xff, 0x04, 0x05, 0, 0, 0, 0};
		HidState s;
		CHECK(!DecodeHidReport(l, keys, sizeof(keys), &s));
		const uint8_t pad[] = {0x02, 0x05, 0x81, 0x7f};
		CHECK(DecodeHidReport(l, pad, sizeof(pad), &s));
		CHECK(s.buttons == 0x05);
		CHECK(s.axes[0] == -1.0f && s.axes[1] == 1.0f); // signed bytes
	}
	{
		// Not a pad: a keyboard alone, a mouse, nothing, garbage, a truncated descriptor.
		const HidLayout kb = ParseReportDescriptor(s_combo_device, 40);
		CHECK(!kb.is_gamepad && kb.fields.empty());
		CHECK(Classify(0x1234, 0x03, 0x00, 0x00, &kb) == Protocol::None);
		const HidLayout none = ParseReportDescriptor(nullptr, 0);
		CHECK(!none.is_gamepad);
		for (size_t cut = 0; cut < sizeof(s_generic_pad); cut++)
		{
			const HidLayout part = ParseReportDescriptor(s_generic_pad, cut);
			CHECK(part.fields.size() <= 17);
		}
		// A Report Size of 0 and a huge Report Count end the parse rather than run away.
		const uint8_t bad[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x75, 0x00, 0x95, 0x10, 0x81, 0x02,
			0x75, 0x08, 0x97, 0xff, 0xff, 0xff, 0x7f, 0x81, 0x02, 0xc0};
		const HidLayout b = ParseReportDescriptor(bad, sizeof(bad));
		CHECK(b.fields.empty());
		// Random bytes never crash it.
		uint32_t seed = 12345;
		for (int round = 0; round < 2000; round++)
		{
			uint8_t junk[64];
			for (uint8_t& j : junk)
			{
				seed = seed * 1103515245u + 12345u;
				j = static_cast<uint8_t>(seed >> 16);
			}
			const HidLayout r = ParseReportDescriptor(junk, sizeof(junk));
			HidState s;
			(void)DecodeHidReport(r, junk, sizeof(junk), &s);
			(void)MapHid(r, s);
		}
		CHECK(true);
	}
	{
		// Triggers: a pad with Z/Rz for the right stick and Rx/Ry as analog triggers.
		const uint8_t desc[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09,
			0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0xc0};
		const HidLayout l = ParseReportDescriptor(desc, sizeof(desc));
		const uint8_t report[] = {128, 128, 0, 255, 255, 0};
		HidState s;
		CHECK(DecodeHidReport(l, report, sizeof(report), &s));
		const MappedPad m = MapHid(l, s);
		CHECK(m.axes[SDL_AXIS_RIGHT_X] == -1.0f && m.axes[SDL_AXIS_RIGHT_Y] == 1.0f);
		CHECK(m.axes[SDL_AXIS_LEFT_TRIGGER] == 1.0f && m.axes[SDL_AXIS_RIGHT_TRIGGER] == 0.0f);
	}
}

// A transport made up for the tests: devices come and go, reports are queued.
class FakeTransport final : public PS5UsbPads::Transport
{
public:
	bool start_ok = true;
	std::vector<PS5UsbPads::DeviceInfo> devices;
	std::map<int, std::deque<std::vector<uint8_t>>> reports;
	std::map<int, std::vector<std::vector<uint8_t>>> written;
	std::map<int, uint64_t> handle_of;
	std::vector<int> closed;
	std::vector<uint64_t> gone; // reads on these fail
	int next = 10;

	bool Start() override { return start_ok; }
	void Stop() override {}
	bool Enumerate(std::vector<PS5UsbPads::DeviceInfo>* out) override
	{
		*out = devices;
		return true;
	}
	int Open(const PS5UsbPads::DeviceInfo& d) override
	{
		handle_of[next] = d.id;
		return next++;
	}
	int Read(int handle, uint8_t* buffer, uint32_t size, uint32_t) override
	{
		for (const uint64_t g : gone)
		{
			if (handle_of[handle] == g)
				return -1;
		}
		auto& q = reports[handle];
		if (q.empty())
			return 0;
		const std::vector<uint8_t> r = q.front();
		q.pop_front();
		const uint32_t n = std::min<uint32_t>(size, static_cast<uint32_t>(r.size()));
		std::memcpy(buffer, r.data(), n);
		return static_cast<int>(n);
	}
	bool Write(int handle, const uint8_t* data, uint32_t size) override
	{
		written[handle].emplace_back(data, data + size);
		return true;
	}
	struct ControlCall
	{
		int handle;
		uint8_t type, request;
		uint16_t value, index, length;
		std::vector<uint8_t> data;
	};
	std::vector<ControlCall> controls;
	int Control(int handle, uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length) override
	{
		controls.push_back({handle, type, request, value, index, length, std::vector<uint8_t>(data, data + length)});
		return length;
	}
	void Close(int handle) override { closed.push_back(handle); }
};

static void DualShock3()
{
	using namespace PS5UsbPadLogic;
	CHECK(IsDualShock3(0x054c, 0x0268));
	CHECK(!IsDualShock3(0x054c, 0x0ce6)); // a DualSense: the console's own
	CHECK(ClassifyDevice(0x054c, 0x0268, 0x03, 0x00, 0x00, nullptr) == Protocol::DualShock3);
	CHECK(ClassifyDevice(0x054c, 0x0ce6, 0x03, 0x00, 0x00, nullptr) == Protocol::None);

	// The 49-byte report: Cross pressed hard, Square lightly, up, L1, Start, the PS button;
	// left stick right, right stick up; L2 half.
	uint8_t r[49] = {0x01};
	r[2] = 0x08 | 0x10; // Start, up
	r[3] = 0x04 | 0x40 | 0x80; // L1, Cross, Square
	r[4] = 0x01; // PS
	r[6] = 255;
	r[7] = 128;
	r[8] = 128;
	r[9] = 0;
	r[14] = 200; // up's pressure
	r[18] = 128; // L2
	r[20] = 255; // L1
	r[24] = 255; // Cross
	r[25] = 40; // Square
	r[22] = 90; // Triangle's pressure byte, its button not pressed
	MappedPad p;
	CHECK(DecodeDualShock3Report(r, sizeof(r), &p));
	CHECK(p.has_pressure);
	CHECK(p.buttons == ((1u << SDL_START) | (1u << SDL_DPAD_UP) | (1u << SDL_LEFT_SHOULDER) | (1u << SDL_SOUTH) | (1u << SDL_WEST) |
						   (1u << SDL_GUIDE)));
	CHECK(Near(p.pressure[SDL_SOUTH], 1.0f));
	CHECK(Near(p.pressure[SDL_WEST], 40.0f / 255.0f));
	CHECK(Near(p.pressure[SDL_DPAD_UP], 200.0f / 255.0f));
	CHECK(p.pressure[SDL_NORTH] == 0.0f); // not pressed: no pressure, whatever the byte says
	CHECK(Near(p.axes[SDL_AXIS_LEFT_X], 1.0f) && Near(p.axes[SDL_AXIS_LEFT_Y], 0.0f));
	CHECK(p.axes[SDL_AXIS_RIGHT_Y] == -1.0f); // 0: all the way up
	CHECK(Near(p.axes[SDL_AXIS_LEFT_TRIGGER], 128.0f / 255.0f));

	// A pressed button whose pressure byte still reads 0 is a light press, not none.
	uint8_t lag[49] = {0x01};
	lag[3] = 0x20; // Circle
	CHECK(DecodeDualShock3Report(lag, sizeof(lag), &p));
	CHECK(p.pressure[SDL_EAST] > 0.0f && p.pressure[SDL_EAST] < 0.01f);

	// Without its id byte (48 bytes), the same.
	CHECK(DecodeDualShock3Report(r + 1, 48, &p));
	CHECK(p.buttons & (1u << SDL_SOUTH));
	// Not input: short, another id.
	CHECK(!DecodeDualShock3Report(r, 20, &p));
	uint8_t other[49] = {0x02};
	CHECK(!DecodeDualShock3Report(other, sizeof(other), &p));

	const auto out = DualShock3OutputReport(200, 1, 2);
	CHECK(out[0] == 0x01 && out[3] == 1 && out[5] == 200 && out[10] == (1u << 2));
	CHECK(DualShock3OutputReport(0, 0, 9)[10] == (1u << 4)); // players past four: the fourth light
}

static void Manager()
{
	using namespace PS5UsbPads;
	using namespace PS5UsbPadLogic;

	// The console's transport says no, and nothing runs.
	{
		PS5UsbPads::Manager m;
		CHECK(!m.Start(CreateConsoleTransport(), false));
		CHECK(!m.Running());
		m.PollOnce(0.0, 0);
		CHECK(!m.Get(0).connected);
	}

	auto owned = std::make_unique<FakeTransport>();
	FakeTransport& t = *owned;
	PS5UsbPads::Manager m;
	CHECK(m.Start(std::move(owned), false));
	CHECK(m.Running());

	DeviceInfo x360;
	x360.id = 1;
	x360.vendor = 0x045e;
	x360.product = 0x028e;
	x360.interface_class = 0xff;
	x360.interface_subclass = 0x5d;
	x360.interface_protocol = 0x01;
	DeviceInfo keyboard;
	keyboard.id = 2;
	keyboard.vendor = 0x1111;
	keyboard.interface_class = 0x03;
	keyboard.interface_subclass = 0x01;
	keyboard.interface_protocol = 0x01;
	DeviceInfo hid;
	hid.id = 3;
	hid.vendor = 0x0079;
	hid.interface_class = 0x03;
	hid.report_descriptor.assign(s_generic_pad, s_generic_pad + sizeof(s_generic_pad));
	hid.name = "Generic Pad";
	DeviceInfo dualsense = hid;
	dualsense.id = 4;
	dualsense.vendor = 0x054c;

	t.devices = {keyboard, x360, dualsense};
	m.PollOnce(0.0, 0);
	Slot s0 = m.Get(0);
	CHECK(s0.connected && s0.protocol == Protocol::XInput && s0.name == "Xbox 360 Controller");
	CHECK(!m.Get(1).connected); // the keyboard and the DualSense are not taken
	const uint32_t gen0 = s0.generation;

	// A report.
	std::vector<uint8_t> r(20, 0);
	r[1] = 0x14;
	r[3] = 0x10; // A
	t.reports[10].push_back(r);
	m.PollOnce(0.1, 0);
	CHECK(m.Get(0).state.buttons == (1u << SDL_SOUTH));
	// The last of several waiting reports wins.
	r[3] = 0x20; // B
	t.reports[10].push_back(r);
	r[3] = 0x40; // X
	t.reports[10].push_back(r);
	m.PollOnce(0.2, 0);
	CHECK(m.Get(0).state.buttons == (1u << SDL_WEST));
	// No report: the state stays.
	m.PollOnce(0.3, 0);
	CHECK(m.Get(0).state.buttons == (1u << SDL_WEST));

	// Rumble is written once per change.
	m.SetRumble(0, 200, 100);
	m.PollOnce(0.4, 0);
	m.PollOnce(0.5, 0);
	CHECK(t.written[10].size() == 1);
	CHECK(t.written[10][0][3] == 200 && t.written[10][0][4] == 100);

	// A HID pad plugged in later is found at the next scan, not before.
	t.devices.push_back(hid);
	m.PollOnce(0.9, 0);
	CHECK(!m.Get(1).connected);
	m.PollOnce(1.0, 0);
	Slot s1 = m.Get(1);
	CHECK(s1.connected && s1.protocol == Protocol::Hid && s1.name == "Generic Pad");
	const uint8_t hid_report[] = {0x01, 0x02, 0x00, 0, 128, 128, 128};
	t.reports[11].emplace_back(hid_report, hid_report + sizeof(hid_report));
	m.PollOnce(1.1, 0);
	CHECK(m.Get(1).state.generic_buttons == 0x02);
	CHECK(m.Get(1).state.axes[SDL_AXIS_LEFT_X] == -1.0f);

	// Unplugged: found at the next scan, the slot freed, the device closed (rumble stopped first).
	t.devices = {hid};
	m.PollOnce(2.0, 0);
	CHECK(!m.Get(0).connected);
	CHECK(m.Get(0).state == MappedPad());
	CHECK(std::find(t.closed.begin(), t.closed.end(), 10) != t.closed.end());
	CHECK(t.written[10].size() == 2 && t.written[10][1][3] == 0 && t.written[10][1][4] == 0);
	CHECK(m.Get(1).connected);

	// A read that fails lets the pad go at once.
	t.gone.push_back(3);
	m.PollOnce(2.1, 0);
	CHECK(!m.Get(1).connected);
	t.gone.clear();
	t.devices.clear();
	m.PollOnce(3.0, 0);

	// Back again: a new generation in the first free slot.
	t.devices = {x360};
	m.PollOnce(4.0, 0);
	s0 = m.Get(0);
	CHECK(s0.connected && s0.generation != gen0);

	// More than four pads: the fifth waits.
	for (uint64_t id = 20; id < 25; id++)
	{
		DeviceInfo d = x360;
		d.id = id;
		t.devices.push_back(d);
	}
	m.PollOnce(5.0, 0);
	uint32_t connected = 0;
	for (uint32_t i = 0; i < MAX_PADS; i++)
		connected += m.Get(i).connected;
	CHECK(connected == 4);
	CHECK(!m.Get(9).connected);

	m.Stop();
	CHECK(!m.Running());
	CHECK(!m.Get(0).connected);

	// The thread starts and stops cleanly.
	auto threaded = std::make_unique<FakeTransport>();
	threaded->devices = {x360};
	PS5UsbPads::Manager mt;
	CHECK(mt.Start(std::move(threaded), true));
	for (int i = 0; i < 200 && !mt.Get(0).connected; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	CHECK(mt.Get(0).connected);
	mt.Stop();
}

// A DualShock 3 through the manager: woken with GET_REPORT 0xF2, its light set, pressure passed.
static void ManagerDualShock3()
{
	using namespace PS5UsbPadLogic;
	auto owned = std::make_unique<FakeTransport>();
	FakeTransport& t = *owned;
	PS5UsbPads::Manager m;
	CHECK(m.Start(std::move(owned), false));
	PS5UsbPads::DeviceInfo ds3;
	ds3.id = 77;
	ds3.vendor = 0x054c;
	ds3.product = 0x0268;
	ds3.interface_class = 0x03;
	ds3.interface_number = 0;
	t.devices = {ds3};
	m.PollOnce(0.0, 0);
	const PS5UsbPads::Slot s = m.Get(0);
	CHECK(s.connected && s.protocol == Protocol::DualShock3 && s.name == "DualShock 3");
	CHECK(!t.controls.empty() && t.controls[0].type == 0xa1 && t.controls[0].request == 0x01 && t.controls[0].value == 0x03f2);
	// The light, once, as a SET_REPORT output report.
	bool light = false;
	for (const auto& c : t.controls)
		light = light || (c.type == 0x21 && c.request == 0x09 && c.value == 0x0201 && c.data.size() == 36 && c.data[10] == (1u << 1));
	CHECK(light);

	std::vector<uint8_t> r(49, 0);
	r[0] = 0x01;
	r[3] = 0x40; // Cross
	r[24] = 100;
	t.reports[t.handle_of.begin()->first].push_back(r);
	m.PollOnce(0.5, 0);
	const PS5UsbPads::Slot after = m.Get(0);
	CHECK(after.state.has_pressure && Near(after.state.pressure[SDL_SOUTH], 100.0f / 255.0f));

	// The rumble goes out as the same report.
	const size_t before = t.controls.size();
	m.SetRumble(0, 255, 255);
	m.PollOnce(0.6, 0);
	CHECK(t.controls.size() == before + 1 && t.controls.back().data[5] == 255 && t.controls.back().data[3] == 1);
	m.Stop();
}

int main()
{
	Combo();
	Buzz();
	Pressure();
	Gyro();
	KeyboardNames();
	KeyboardReports();
	XInput();
	DualShock3();
	Hid();
	Manager();
	ManagerDualShock3();
	std::printf("input logic: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
