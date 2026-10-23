// Crux2 - the controllers, through the console's pad service.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The emulator's settings, automatic mapping and interface navigation all speak the SDL gamepad
// source's language (devices "SDL-0".., bindings such as "SDL-0/DPadUp" or "SDL-0/+LeftX"). This
// source answers to that name and that numbering, so a settings file or an input profile from a PC
// works unchanged, and reads the DualSense (or DualShock 4) of each logged-in user with scePad:
// the user who started the title is SDL-0, the other logged-in users follow in login order.
// USB pads that are not PlayStation controllers would be SDL-4 to SDL-7 (ps5_usbpad.h: the
// console's USB transport is still a stub, so none appear yet).
//
// What a title cannot see: the Create (Share) button and the PS button, which the console keeps.
// The touch pad's click stands in for them: pressed on its left half it is "Back" (the automatic
// mapping's Select), anywhere else it is "Touchpad" (the automatic mapping's Start). The Options
// button ("Start" here, as SDL names it) opens the quick menu by default, and is the interface's
// Start button.
//
// Besides the buttons and sticks, this source does what a PS2 game on a DualSense needs:
//
//  * Rumble. A PS5 title's DualSense starts in haptics mode, in which scePadSetVibration's two
//    motor values are accepted and do nothing (PSWin build 19 found that for its games: "rumble
//    works in games" only after the switch). scePadSetVibrationMode(handle, 2) puts it in the
//    two-motor ("compatible") mode, where the large and small values drive the controller's
//    actuators as the DualShock's motors; it answers 0 when taken. It is set when a pad is opened
//    and again each time the controller reconnects (one switched off and on may come back in
//    haptics mode). A DualShock 4 has real motors and may refuse the call; that is logged once and
//    harmless. The DualShock 2's small motor is on/off and its large one 0 to 255: PCSX2 hands both
//    as intensities, sent here as they come.
//  * The keyboard (ps5_keyboard.cpp) is read here, on the polling thread, so its key events reach
//    InputManager with the controllers'.
//  * Holding a combination asks to leave the game; pressure from R2; aiming a GunCon 2 with the
//    motion sensor. Each is described where it happens below.

#include "ps5_input_source.h"

#include "ps5_keyboard.h"
#include "ps5_usbpad.h"

#include "Config.h"
#include "Host.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/HostSys.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"

#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

extern "C" {
// No SDK header declares these; the signatures are the ones the template's PS5 layer calls.
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(s32* user_id);
int sceUserServiceGetLoginUserIdList(s32* user_ids); // four ids, -1 for an empty slot
int scePadInit(void);
int scePadOpen(s32 user_id, s32 port_type, s32 index, const void* params);
int scePadGetHandle(s32 user_id, s32 port_type, s32 index);
int scePadClose(s32 handle);
int scePadRead(s32 handle, void* samples, s32 capacity);
int scePadSetVibration(s32 handle, const void* param); // { u8 large, u8 small }
int scePadSetVibrationMode(s32 handle, s32 mode); // 2: the two rumble motors
int scePadSetLightBar(s32 handle, const void* color); // { u8 red, u8 green, u8 blue, u8 0 }
void ps5px_log(const char* format, ...);
}

namespace
{
	constexpr u32 MOTOR_INDEX_LARGE = 0;
	constexpr u32 MOTOR_INDEX_SMALL = 1;

	// The two-motor mode of scePadSetVibrationMode (the other is the DualSense's haptics).
	constexpr s32 VIBRATION_MODE_COMPATIBLE = 2;

	// One sample as the console writes it: 120 bytes.
	struct PadSample
	{
		u32 buttons;
		u8 left_x, left_y, right_x, right_y, l2, r2;
		u8 padding[2];
		float orientation[4];
		float acceleration[3];
		float angular_velocity[3]; // radians a second about x (pitch), y (yaw), z (roll)
		u8 touch_count;
		u8 touch_reserved[3];
		u32 touch_reserved2;
		struct
		{
			u16 x, y;
			u8 id;
			u8 reserved[3];
		} touch[2];
		s32 connected;
		u64 timestamp_us;
		u8 extension[16];
		u8 connected_count;
		u8 remaining[15];
	};
	static_assert(sizeof(PadSample) == 120, "the console's pad samples are 120 bytes");
	static_assert(offsetof(PadSample, angular_velocity) == 40, "the angular velocity sits at 40");
	static_assert(offsetof(PadSample, touch_count) == 52, "the touch data sits at 52");
	static_assert(offsetof(PadSample, connected) == 0x4c, "the connection state sits at 0x4c");
	static_assert(offsetof(PadSample, timestamp_us) == 0x50, "the timestamp sits at 0x50");

	// The console's button bits.
	enum : u32
	{
		PAD_L3 = 0x000002,
		PAD_R3 = 0x000004,
		PAD_OPTIONS = 0x000008,
		PAD_UP = 0x000010,
		PAD_RIGHT = 0x000020,
		PAD_DOWN = 0x000040,
		PAD_LEFT = 0x000080,
		PAD_L1 = 0x000400,
		PAD_R1 = 0x000800,
		PAD_TRIANGLE = 0x001000,
		PAD_CIRCLE = 0x002000,
		PAD_CROSS = 0x004000,
		PAD_SQUARE = 0x008000,
		PAD_TOUCH_PAD = 0x100000,
		// Set while the shell has the pad (the home screen, a system dialog): not input for the title.
		PAD_INTERCEPTED = 0x80000000u,
	};

	// The touch pad is 1920 units wide on both the DualSense and the DualShock 4.
	constexpr u16 TOUCH_PAD_MIDDLE = 960;

	// SDL's gamepad button and axis numbering, which the binding strings and keys are made of.
	enum : u32
	{
		BUTTON_SOUTH,
		BUTTON_EAST,
		BUTTON_WEST,
		BUTTON_NORTH,
		BUTTON_BACK,
		BUTTON_GUIDE,
		BUTTON_START,
		BUTTON_LEFT_STICK,
		BUTTON_RIGHT_STICK,
		BUTTON_LEFT_SHOULDER,
		BUTTON_RIGHT_SHOULDER,
		BUTTON_DPAD_UP,
		BUTTON_DPAD_DOWN,
		BUTTON_DPAD_LEFT,
		BUTTON_DPAD_RIGHT,
		BUTTON_MISC1,
		BUTTON_RIGHT_PADDLE1,
		BUTTON_LEFT_PADDLE1,
		BUTTON_RIGHT_PADDLE2,
		BUTTON_LEFT_PADDLE2,
		BUTTON_TOUCHPAD,
		BUTTON_COUNT
	};

	enum : u32
	{
		AXIS_LEFT_X,
		AXIS_LEFT_Y,
		AXIS_RIGHT_X,
		AXIS_RIGHT_Y,
		AXIS_LEFT_TRIGGER,
		AXIS_RIGHT_TRIGGER,
		AXIS_COUNT
	};
	static_assert(AXIS_COUNT == PS5InputSource::NUM_AXES);
	static_assert(static_cast<u32>(AXIS_COUNT) == static_cast<u32>(PS5UsbPadLogic::SDL_AXIS_COUNT));
	static_assert(BUTTON_LEFT_STICK == 7 && BUTTON_RIGHT_STICK == 8 && BUTTON_LEFT_SHOULDER == 9 &&
				  BUTTON_RIGHT_SHOULDER == 10, "PS5InputLogic::LeaveComboMask uses these bits");

	// Buttons and axes with no SDL name, as SDL's own source names a joystick's: "Button<n>" and
	// "Axis<n>". Their key data starts past SDL's, so the two never meet. (USB pads use them; any
	// device index parses them.)
	constexpr u32 GENERIC_BUTTON_BASE = 32;
	constexpr u32 GENERIC_AXIS_BASE = 16;

	// The buttons whose pressure "Pressure from R2" sets: the DualShock 2's analog face buttons
	// and d-pad.
	constexpr u32 PRESSURE_BUTTONS = (1u << BUTTON_SOUTH) | (1u << BUTTON_EAST) | (1u << BUTTON_WEST) | (1u << BUTTON_NORTH) |
									 (1u << BUTTON_DPAD_UP) | (1u << BUTTON_DPAD_DOWN) | (1u << BUTTON_DPAD_LEFT) |
									 (1u << BUTTON_DPAD_RIGHT);

	struct ButtonInfo
	{
		const char* name; // in a binding string, as the SDL source writes it
		const char* legacy_name; // as the SDL 2 builds wrote the face buttons
		const char* label; // on a PlayStation controller
		const char* icon;
		GenericInputBinding generic_binding;
	};

	// clang-format off
	constexpr std::array<ButtonInfo, BUTTON_COUNT> s_button_info = {{
		{"FaceSouth", "A", "Cross", ICON_PF_BUTTON_CROSS, GenericInputBinding::Cross},
		{"FaceEast", "B", "Circle", ICON_PF_BUTTON_CIRCLE, GenericInputBinding::Circle},
		{"FaceWest", "X", "Square", ICON_PF_BUTTON_SQUARE, GenericInputBinding::Square},
		{"FaceNorth", "Y", "Triangle", ICON_PF_BUTTON_TRIANGLE, GenericInputBinding::Triangle},
		{"Back", nullptr, "Touch Pad (left)", ICON_PF_DUALSENSE_TOUCHPAD, GenericInputBinding::Select},
		{"Guide", nullptr, "PS", ICON_PF_PLAYSTATION, GenericInputBinding::System},
		{"Start", nullptr, "Options", ICON_PF_DUALSENSE_OPTIONS, GenericInputBinding::Start},
		{"LeftStick", nullptr, "L3", ICON_PF_LEFT_ANALOG_CLICK, GenericInputBinding::L3},
		{"RightStick", nullptr, "R3", ICON_PF_RIGHT_ANALOG_CLICK, GenericInputBinding::R3},
		{"LeftShoulder", nullptr, "L1", ICON_PF_LEFT_SHOULDER_L1, GenericInputBinding::L1},
		{"RightShoulder", nullptr, "R1", ICON_PF_RIGHT_SHOULDER_R1, GenericInputBinding::R1},
		{"DPadUp", nullptr, "D-Pad Up", ICON_PF_DPAD_UP, GenericInputBinding::DPadUp},
		{"DPadDown", nullptr, "D-Pad Down", ICON_PF_DPAD_DOWN, GenericInputBinding::DPadDown},
		{"DPadLeft", nullptr, "D-Pad Left", ICON_PF_DPAD_LEFT, GenericInputBinding::DPadLeft},
		{"DPadRight", nullptr, "D-Pad Right", ICON_PF_DPAD_RIGHT, GenericInputBinding::DPadRight},
		{"Misc1", nullptr, "Misc 1", nullptr, GenericInputBinding::Unknown},
		{"Paddle1", nullptr, "Paddle 1", nullptr, GenericInputBinding::Unknown},
		{"Paddle2", nullptr, "Paddle 2", nullptr, GenericInputBinding::Unknown},
		{"Paddle3", nullptr, "Paddle 3", nullptr, GenericInputBinding::Unknown},
		{"Paddle4", nullptr, "Paddle 4", nullptr, GenericInputBinding::Unknown},
		{"Touchpad", nullptr, "Touch Pad (right)", ICON_PF_DUALSENSE_TOUCHPAD, GenericInputBinding::Unknown},
	}};

	struct AxisInfo
	{
		const char* name;
		const char* label;
		const char* icon_negative;
		const char* icon_positive;
		GenericInputBinding generic_binding_negative;
		GenericInputBinding generic_binding_positive;
	};

	constexpr std::array<AxisInfo, AXIS_COUNT> s_axis_info = {{
		{"LeftX", "Left X", ICON_PF_LEFT_ANALOG_LEFT, ICON_PF_LEFT_ANALOG_RIGHT, GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},
		{"LeftY", "Left Y", ICON_PF_LEFT_ANALOG_UP, ICON_PF_LEFT_ANALOG_DOWN, GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},
		{"RightX", "Right X", ICON_PF_RIGHT_ANALOG_LEFT, ICON_PF_RIGHT_ANALOG_RIGHT, GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight},
		{"RightY", "Right Y", ICON_PF_RIGHT_ANALOG_UP, ICON_PF_RIGHT_ANALOG_DOWN, GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},
		{"LeftTrigger", "L2", nullptr, ICON_PF_LEFT_TRIGGER_L2, GenericInputBinding::Unknown, GenericInputBinding::L2},
		{"RightTrigger", "R2", nullptr, ICON_PF_RIGHT_TRIGGER_R2, GenericInputBinding::Unknown, GenericInputBinding::R2},
	}};
	// clang-format on

	// The console's bits for the buttons that map one to one.
	constexpr std::array<std::pair<u32, u32>, 13> s_button_bits = {{
		{PAD_CROSS, BUTTON_SOUTH},
		{PAD_CIRCLE, BUTTON_EAST},
		{PAD_SQUARE, BUTTON_WEST},
		{PAD_TRIANGLE, BUTTON_NORTH},
		{PAD_OPTIONS, BUTTON_START},
		{PAD_L3, BUTTON_LEFT_STICK},
		{PAD_R3, BUTTON_RIGHT_STICK},
		{PAD_L1, BUTTON_LEFT_SHOULDER},
		{PAD_R1, BUTTON_RIGHT_SHOULDER},
		{PAD_UP, BUTTON_DPAD_UP},
		{PAD_DOWN, BUTTON_DPAD_DOWN},
		{PAD_LEFT, BUTTON_DPAD_LEFT},
		{PAD_RIGHT, BUTTON_DPAD_RIGHT},
	}};

	// A generic USB HID pad's buttons for the automatic mapping. There is no standard order; this
	// is the commonest of the cheap "USB gamepad" order (the face buttons first, then the
	// shoulders, then Select and Start, then the stick clicks). Anything else is bound by hand.
	// clang-format off
	constexpr std::array<std::pair<GenericInputBinding, u32>, 12> s_hid_button_guess = {{
		{GenericInputBinding::Cross, 0}, {GenericInputBinding::Circle, 1}, {GenericInputBinding::Square, 2},
		{GenericInputBinding::Triangle, 3}, {GenericInputBinding::L1, 4}, {GenericInputBinding::R1, 5},
		{GenericInputBinding::L2, 6}, {GenericInputBinding::R2, 7}, {GenericInputBinding::Select, 8},
		{GenericInputBinding::Start, 9}, {GenericInputBinding::L3, 10}, {GenericInputBinding::R3, 11},
	}};
	// clang-format on

	bool IsDecimalString(std::string_view str)
	{
		if (str.empty())
			return false;
		for (const char ch : str)
		{
			if (ch < '0' || ch > '9')
				return false;
		}
		return true;
	}

	bool IsDeviceIdentifier(std::string_view device)
	{
		return device.starts_with("SDL-") && IsDecimalString(device.substr(4));
	}

	float NormalizeStick(u8 value)
	{
		// 128 is the centre: -1 at 0, +1 at 255.
		return (value >= 128) ? (static_cast<float>(value - 128) / 127.0f) : (static_cast<float>(value) - 128.0f) / 128.0f;
	}

	u32 ButtonsFromSample(const PadSample& sample, bool touch_is_back)
	{
		u32 buttons = 0;
		for (const auto& [bit, button] : s_button_bits)
		{
			if (sample.buttons & bit)
				buttons |= (1u << button);
		}

		if (sample.buttons & PAD_TOUCH_PAD)
			buttons |= touch_is_back ? (1u << BUTTON_BACK) : (1u << BUTTON_TOUCHPAD);
		return buttons;
	}

	InputBindingKey MakeDeviceKey(u32 index)
	{
		InputBindingKey key = {};
		key.source_type = InputSourceType::SDL;
		key.source_index = index;
		return key;
	}

	u64 NowMicroseconds()
	{
		const u64 ticks = GetCPUTicks();
		const u64 freq = GetTickFrequency();
		return (ticks / freq) * 1000000ull + ((ticks % freq) * 1000000ull) / freq;
	}

	// Parses "<prefix><n>" with n below limit.
	std::optional<u32> NumberAfter(std::string_view str, std::string_view prefix, u32 limit)
	{
		if (!str.starts_with(prefix) || !IsDecimalString(str.substr(prefix.size())) || str.size() - prefix.size() > 3)
			return std::nullopt;
		const std::optional<u32> n = StringUtil::FromChars<u32>(str.substr(prefix.size()));
		return (n.has_value() && n.value() < limit) ? n : std::nullopt;
	}

	// ------------------------------------------------------------------ leave-the-game handler

	std::mutex s_leave_mutex;
	std::function<void(u32)> s_leave_handler;
	std::atomic<float> s_leave_progress{0.0f};

	// ------------------------------------------------------------------ optional pad functions

	// scePadSetMotionSensorState is in libScePad's stub, but it is not imported: a title that
	// imports what the console's module does not export does not start, and the title has never
	// called it. So it is looked up while the title runs (ps5_keyboard.cpp's lookup), once.
	using SetMotionSensorStateFn = int (*)(s32 handle, bool enable);
	SetMotionSensorStateFn ResolveSetMotionSensorState()
	{
		static bool looked = false;
		static SetMotionSensorStateFn fn = nullptr;
		if (!looked)
		{
			looked = true;
			fn = reinterpret_cast<SetMotionSensorStateFn>(
				PS5SystemModules::FindFunction("libScePad", "scePadSetMotionSensorState", "clVvL4ZDntw"));
			ps5px_log("pads: scePadSetMotionSensorState %s", fn ? "found" : "not found: the motion sensor may report nothing");
		}
		return fn;
	}
} // namespace

PS5InputSource::PS5InputSource() = default;

PS5InputSource::~PS5InputSource() = default;

std::string PS5InputSource::DeviceName(u32 index)
{
	return fmt::format("Wireless Controller (player {})", index + 1);
}

void PS5InputSource::SetLeaveGameHandler(std::function<void(u32 player)> handler)
{
	std::lock_guard lock(s_leave_mutex);
	s_leave_handler = std::move(handler);
}

float PS5InputSource::LeaveGameHoldProgress()
{
	return s_leave_progress.load(std::memory_order_relaxed);
}

void PS5InputSource::ReadSettings(SettingsInterface& si)
{
	m_light_bars = si.GetBoolValue("PS5", "LightBars", false);

	// Holding L3 and R3 for two seconds asks to leave the game ("Off" turns it off).
	m_leave_mask = PS5InputLogic::LeaveComboMask(si.GetStringValue("PS5", "LeaveGameCombo", "L3R3"));
	m_leave_hold_us = PS5InputLogic::LeaveHoldMicroseconds(si.GetFloatValue("PS5", "LeaveGameHoldSeconds", 2.0f));

	m_pressure_from_r2 = si.GetBoolValue("PS5", "PressureFromR2", false);

	m_motion_aim_setting = si.GetBoolValue("PS5", "MotionAim", false);
	m_aim_settings.degrees_to_edge = std::clamp(si.GetFloatValue("PS5", "MotionAimSensitivity", 20.0f), 5.0f, 60.0f);
	m_aim_settings.smoothing_s = std::clamp(si.GetFloatValue("PS5", "MotionAimSmoothingMs", 20.0f), 0.0f, 100.0f) / 1000.0f;
	m_aim_settings.dead_zone_dps = std::clamp(si.GetFloatValue("PS5", "MotionAimDeadzone", 1.0f), 0.0f, 10.0f);
	m_aim_settings.invert_x = si.GetBoolValue("PS5", "MotionAimInvertX", false);
	m_aim_settings.invert_y = si.GetBoolValue("PS5", "MotionAimInvertY", false);
	m_guncon_plugged = (USB::GetConfigDevice(si, 0) == "guncon2" || USB::GetConfigDevice(si, 1) == "guncon2");

	PS5Keyboard::SetEnabled(si.GetBoolValue("PS5", "USBKeyboard", true));
	m_usb_pads_setting = si.GetBoolValue("PS5", "USBPads", false);
}

bool PS5InputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	const int user_rc = sceUserServiceInitialize(nullptr);
	const int pad_rc = scePadInit();
	ps5px_log("pads: sceUserServiceInitialize 0x%x, scePadInit 0x%x", static_cast<u32>(user_rc), static_cast<u32>(pad_rc));
	m_initialized = true;
	ReadSettings(si);
	ps5px_log("pads: leave-the-game hold %s, pressure from R2 %s, motion aim %s%s, USB keyboard %s, USB pads %s",
		m_leave_mask ? "on" : "off", m_pressure_from_r2 ? "on" : "off", m_motion_aim_setting ? "on" : "off",
		m_guncon_plugged ? " (a GunCon 2 is plugged in)" : "", si.GetBoolValue("PS5", "USBKeyboard", true) ? "on" : "off",
		m_usb_pads_setting ? "on" : "off");

	// Connections are announced from PollEvents, which runs without the settings lock: the
	// announcement loads the bindings of the device, and so takes that lock itself.
	settings_lock.unlock();
	FindUsers();
	if (m_usb_pads_setting)
	{
		m_usb = std::make_unique<PS5UsbPads::Manager>();
		if (!m_usb->Start(PS5UsbPads::CreateConsoleTransport()))
			m_usb.reset();
	}
	settings_lock.lock();
	return true;
}

void PS5InputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	const bool light_bars = m_light_bars;
	const bool usb_pads = m_usb_pads_setting;
	ReadSettings(si);

	if (light_bars != m_light_bars)
	{
		// (Switched off, the bars keep their colour until the controller is next connected: taking it
		// back is a call the title does not import.)
		for (u32 i = 0; i < MAX_PADS; i++)
			ShowPlayer(i);
	}

	if (usb_pads != m_usb_pads_setting)
	{
		settings_lock.unlock();
		if (m_usb_pads_setting && !m_usb)
		{
			m_usb = std::make_unique<PS5UsbPads::Manager>();
			if (!m_usb->Start(PS5UsbPads::CreateConsoleTransport()))
				m_usb.reset();
		}
		else if (!m_usb_pads_setting && m_usb)
		{
			for (u32 i = 0; i < MAX_USB_PADS; i++)
				SetUsbConnected(i, false);
			m_usb->Stop();
			m_usb.reset();
		}
		settings_lock.lock();
	}
}

// The colours the PlayStation 4 gave its players: blue, red, green, pink.
void PS5InputSource::ShowPlayer(u32 index)
{
	static constexpr u8 colours[MAX_PADS][4] = {{0, 48, 255, 0}, {255, 24, 24, 0}, {24, 220, 40, 0}, {255, 48, 200, 0}};
	const Pad& pad = m_pads[index];
	if (!m_light_bars || pad.handle < 0)
		return;
	const int rc = scePadSetLightBar(pad.handle, colours[index]);
	if (rc != 0)
		ps5px_log("pads: player %u: the light bar was not set: 0x%x", index + 1, static_cast<u32>(rc));
}

bool PS5InputSource::ReloadDevices()
{
	FindUsers();
	return false;
}

void PS5InputSource::Shutdown()
{
	for (u32 i = 0; i < MAX_PADS; i++)
	{
		Pad& pad = m_pads[i];
		if (pad.handle >= 0)
		{
			pad.rumble = {};
			pad.buzz = {};
			SendRumble(pad);
			SetMotionSensor(pad, false);
		}
		SetConnected(i, false);
		// The handles stay open: the pad service keeps one per user for the life of the title, and a
		// second scePadOpen for the same user fails.
	}
	if (m_usb)
	{
		for (u32 i = 0; i < MAX_USB_PADS; i++)
			SetUsbConnected(i, false);
		m_usb->Stop();
		m_usb.reset();
	}
	m_motion_active = false;
	s_leave_progress.store(0.0f, std::memory_order_relaxed);
	m_initialized = false;
}

bool PS5InputSource::IsInitialized()
{
	return m_initialized;
}

void PS5InputSource::FindUsers()
{
	m_last_user_scan = GetCPUTicks();

	// The user who started the title is player 1, whatever the login order.
	s32 initial = -1;
	if (sceUserServiceGetInitialUser(&initial) < 0)
		initial = -1;

	std::array<s32, 16> listed; // four used; room to spare if a firmware's list is longer
	listed.fill(-1);
	if (sceUserServiceGetLoginUserIdList(listed.data()) < 0)
		listed.fill(-1);

	std::array<s32, MAX_PADS> users;
	users.fill(-1);
	u32 count = 0;
	if (initial >= 0)
		users[count++] = initial;
	for (u32 i = 0; i < 4 && count < MAX_PADS; i++)
	{
		if (listed[i] >= 0 && listed[i] != initial)
			users[count++] = listed[i];
	}

	// First every slot whose user changed (a login or a logout while the title runs) is closed,
	// then the slots are opened: a user who moved from one slot to another must not have the handle
	// just taken for the new slot closed by the old one.
	for (u32 i = 0; i < MAX_PADS; i++)
	{
		Pad& pad = m_pads[i];
		if (pad.user_id == users[i])
			continue;

		SetConnected(i, false);
		if (pad.handle >= 0)
			scePadClose(pad.handle);
		pad = Pad();
		pad.user_id = users[i];
	}
	for (u32 i = 0; i < MAX_PADS; i++)
	{
		if (m_pads[i].user_id >= 0 && m_pads[i].handle < 0)
			OpenPad(i);
	}
}

void PS5InputSource::OpenPad(u32 index)
{
	Pad& pad = m_pads[index];
	pad.handle = scePadOpen(pad.user_id, 0, 0, nullptr);
	if (pad.handle < 0)
	{
		// Already open (the pad service allows one handle per user): take that one.
		const int existing = scePadGetHandle(pad.user_id, 0, 0);
		if (existing >= 0)
			pad.handle = existing;
	}
	if (pad.handle < 0)
	{
		// A title can start before the pad service has published the device: FindUsers tries again.
		pad.handle = -1;
		return;
	}

	ps5px_log("pads: player %u is user 0x%x, handle %d", index + 1, static_cast<u32>(pad.user_id), pad.handle);
	SetVibrationMode(pad, index);
	ShowPlayer(index);
}

void PS5InputSource::SetVibrationMode(Pad& pad, u32 index)
{
	// A PS5 title's controllers start in haptics mode, where the two-motor values go nowhere (the
	// file's head comment says more). Logged the first time for each player, and whenever it is
	// refused.
	const int rc = scePadSetVibrationMode(pad.handle, VIBRATION_MODE_COMPATIBLE);
	if (!pad.vibration_mode_logged || rc != 0)
	{
		pad.vibration_mode_logged = true;
		ps5px_log("pads: player %u: two-motor rumble mode 0x%x%s", index + 1, static_cast<u32>(rc),
			rc == 0 ? "" : " (refused: a DualShock 4 has its motors anyway)");
	}
}

void PS5InputSource::SetConnected(u32 index, bool connected)
{
	Pad& pad = m_pads[index];
	if (pad.connected == connected)
		return;

	pad.connected = connected;
	if (connected)
	{
		InputManager::OnInputDeviceConnected(fmt::format("SDL-{}", index), DeviceName(index));
	}
	else
	{
		// Let go of everything that was held: the buttons, and the sticks and triggers back to rest.
		for (u32 button = 0; button < BUTTON_COUNT; button++)
		{
			if (pad.button_values[button] != 0.0f)
			{
				InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::SDL, index, button), 0.0f,
					s_button_info[button].generic_binding);
			}
		}
		for (u32 axis = 0; axis < AXIS_COUNT; axis++)
		{
			const u8 rest = (axis >= AXIS_LEFT_TRIGGER) ? 0 : 128;
			if (pad.axes[axis] != rest)
			{
				InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::SDL, index, static_cast<s32>(axis)), 0.0f,
					GenericInputBinding::Unknown, s_axis_info[axis].generic_binding_negative,
					s_axis_info[axis].generic_binding_positive);
			}
		}
		pad.buttons = 0;
		pad.button_values = {};
		pad.axes = {128, 128, 128, 128, 0, 0};
		pad.touch_is_back = false;
		pad.leave_combo.Cancel();
		pad.last_timestamp_us = 0;
		InputManager::OnInputDeviceDisconnected(MakeDeviceKey(index), fmt::format("SDL-{}", index));
	}
}

void PS5InputSource::ApplySample(u32 index, const void* sample_ptr)
{
	const PadSample& sample = *static_cast<const PadSample*>(sample_ptr);
	Pad& pad = m_pads[index];
	const bool running = VMManager::GetState() == VMState::Running;

	// The touch pad's click: left half Back (Select), anywhere else (or no finger reported) Touchpad
	// (Start). Which half is decided when the click starts, so a finger sliding across does not
	// change it. With no game running, or one held still under the quick menu, there is only the
	// interface, whose hints show the touch pad for one button: the whole pad is "Back" then.
	const bool touch_down = (sample.buttons & PAD_TOUCH_PAD) != 0;
	const bool touch_was_down = (pad.buttons & ((1u << BUTTON_BACK) | (1u << BUTTON_TOUCHPAD))) != 0;
	if (touch_down && !touch_was_down)
	{
		pad.touch_is_back = !running ||
							(sample.touch_count > 0 && sample.touch_count <= 2 && sample.touch[0].x < TOUCH_PAD_MIDDLE);
	}

	const u32 buttons = ButtonsFromSample(sample, pad.touch_is_back);
	const u32 pressed = buttons & ~pad.buttons;
	pad.buttons = buttons;

	// Pressure from R2 ([PS5] PressureFromR2, off by default, while a game runs). A DualSense's
	// face buttons and d-pad are digital; the DualShock 2's report how hard they are pressed, and
	// some games read that (a light press of Square lowers a gun without firing it; a racing
	// game's throttle on Cross). PCSX2's own answer is the "Pressure" modifier binding (PadDualshock2:
	// held, every button reports PressureModifier, half by default): a fixed second level, bindable
	// to any button already, and enough for most games. This adds a continuous one: R2 at rest,
	// a press is a full one (play without the trigger is unchanged); R2 pulled, every face button
	// and d-pad press reports as much pressure as the trigger is pulled
	// (PS5InputLogic::TriggerPressure). R2 is then the pressure and not the game's R2: its own
	// events are held at rest while the option is on, so a game that needs R2 should have the
	// option off (it is a per-game setting like any other). The pad's button bindings are axis
	// handlers in InputManager, so a value below 1 reaches PadDualshock2 as that pressure, and a
	// changed value for a button still held is passed on as it changes.
	const bool pressure = m_pressure_from_r2 && running;
	const float pressure_value = pressure ? PS5InputLogic::TriggerPressure(sample.r2) : 1.0f;

	for (u32 button = 0; button < BUTTON_COUNT; button++)
	{
		const bool down = (buttons & (1u << button)) != 0;
		const float value = !down ? 0.0f : ((PRESSURE_BUTTONS & (1u << button)) ? pressure_value : 1.0f);
		if (value == pad.button_values[button])
			continue;
		pad.button_values[button] = value;
		InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::SDL, index, button), value,
			s_button_info[button].generic_binding);
	}

	const std::array<u8, AXIS_COUNT> axes = {
		sample.left_x, sample.left_y, sample.right_x, sample.right_y, sample.l2, pressure ? u8{0} : sample.r2};
	for (u32 axis = 0; axis < AXIS_COUNT; axis++)
	{
		if (axes[axis] == pad.axes[axis])
			continue;
		pad.axes[axis] = axes[axis];

		const bool trigger = (axis >= AXIS_LEFT_TRIGGER);
		const float value = trigger ? (static_cast<float>(axes[axis]) / 255.0f) : NormalizeStick(axes[axis]);
		InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::SDL, index, static_cast<s32>(axis)), value,
			GenericInputBinding::Unknown, s_axis_info[axis].generic_binding_negative, s_axis_info[axis].generic_binding_positive);
	}

	// Leaving the game by a hold ([PS5] LeaveGameCombo: L3+R3 by default, L1+R1+L3+R3, or Off;
	// [PS5] LeaveGameHoldSeconds, 2 by default). Only while a game runs; the combination must
	// become complete and stay so for the whole hold, and letting go of any of it cancels, so a
	// click of both sticks in play does nothing. Once taken, a double buzz says so and the
	// handler the host gave (SetLeaveGameHandler) does what the quick menu's "Leave the game"
	// does. The buttons still reach the game while held.
	const u64 now_us = NowMicroseconds();
	pad.leave_combo.Configure(m_leave_mask, m_leave_hold_us);
	if (!running)
	{
		pad.leave_combo.Cancel();
	}
	else if (pad.leave_combo.Update(buttons, now_us))
	{
		ps5px_log("pads: player %u held the leave-the-game combination", index + 1);
		pad.buzz.Start(now_us);
		SendRumble(pad);
		std::function<void(u32)> handler;
		{
			std::lock_guard lock(s_leave_mutex);
			handler = s_leave_handler;
		}
		if (handler)
			handler(index);
	}

	// Motion aim: R3 puts the pointer back in the middle (player 1, while aiming).
	if (index == 0 && m_motion_active)
	{
		if (pressed & (1u << BUTTON_RIGHT_STICK))
		{
			m_aim.Recenter();
			m_aim_moved = true;
		}
		const float dt = PS5InputLogic::SampleInterval(pad.last_timestamp_us, sample.timestamp_us, 1.0f / 250.0f);
		m_aim.Feed(sample.angular_velocity[0], sample.angular_velocity[1], dt);
		m_aim_moved = true;
	}
	pad.last_timestamp_us = sample.timestamp_us;
}

void PS5InputSource::SetMotionSensor(Pad& pad, bool on)
{
	if (pad.handle < 0 || pad.motion_on == on)
		return;
	pad.motion_on = on;
	if (const SetMotionSensorStateFn fn = ResolveSetMotionSensorState())
	{
		const int rc = fn(pad.handle, on);
		ps5px_log("pads: motion sensor %s: 0x%x", on ? "on" : "off", static_cast<u32>(rc));
	}
}

// Aiming a light gun with the motion sensor. PCSX2's GunCon 2 (USB/usb-lightgun) aims where the
// host's pointer is: InputManager's pointer 0, in window pixels, which GSTranslateWindowToDisplay-
// Coordinates maps onto the game's picture (outside it is off the screen). With [PS5] MotionAim on,
// or a GunCon 2 in either USB port, player 1's controller turns its angular velocity into that
// pointer while a game runs (PS5InputLogic::GyroAim: [PS5] MotionAimSensitivity is the turn from
// the middle to the side of the screen, in degrees, 20 by default; MotionAimSmoothingMs, 20;
// MotionAimDeadzone, 1 degree a second; MotionAimInvertX/Y for a controller whose axes run the
// other way than assumed). The aim is relative, as a mouse's: point at the middle of the screen
// and press R3 to put the pointer there (it starts there too). The GunCon's buttons are ordinary
// bindings (SetGunCon2Defaults). A GunCon set up with "Relative" bindings reads those instead of
// the pointer, as on a PC.
void PS5InputSource::UpdateMotionAim(u64 now_us)
{
	// Whether a GunCon is plugged in is read again once a second while a game runs: a game's own
	// settings can plug one in, and they are not this source's settings.
	const bool running = VMManager::GetState() == VMState::Running;
	if (running && now_us - m_last_guncon_check >= 1000000)
	{
		m_last_guncon_check = now_us;
		m_guncon_plugged = Host::GetStringSettingValue("USB1", "Type", "None") == "guncon2" ||
						   Host::GetStringSettingValue("USB2", "Type", "None") == "guncon2";
	}

	Pad& pad = m_pads[0];
	const bool active = running && pad.connected && (m_motion_aim_setting || m_guncon_plugged);
	if (active != m_motion_active)
	{
		m_motion_active = active;
		SetMotionSensor(pad, active);
		if (active)
		{
			m_aim.Recenter();
			m_aim_moved = true;
			ps5px_log("pads: aiming with player 1's motion sensor (%s)", m_guncon_plugged ? "a GunCon 2 is plugged in" : "MotionAim");
		}
	}
	if (!active || !m_aim_moved)
		return;
	m_aim_moved = false;

	float width = ImGuiManager::GetWindowWidth();
	float height = ImGuiManager::GetWindowHeight();
	if (!(width > 0.0f) || !(height > 0.0f))
	{
		width = 1920.0f;
		height = 1080.0f;
	}
	PS5InputLogic::GyroAimSettings settings = m_aim_settings;
	settings.aspect = height / width;
	m_aim.SetSettings(settings);
	InputManager::UpdatePointerAbsolutePosition(0, (m_aim.X() * 0.5f + 0.5f) * width, (m_aim.Y() * 0.5f + 0.5f) * height);
}

void PS5InputSource::PollEvents()
{
	if (!m_initialized)
		return;

	// Users log in and out, and a pad may not be there yet when the title starts.
	bool missing = false;
	for (const Pad& pad : m_pads)
		missing = missing || (pad.user_id < 0) || (pad.handle < 0);
	if (missing && (GetCPUTicks() - m_last_user_scan) >= 2 * GetTickFrequency())
		FindUsers();

	// Settings reach the aim between polls; it is given them before the samples move it.
	{
		PS5InputLogic::GyroAimSettings settings = m_aim_settings;
		settings.aspect = m_aim.Settings().aspect;
		m_aim.SetSettings(settings);
	}

	for (u32 i = 0; i < MAX_PADS; i++)
	{
		Pad& pad = m_pads[i];
		if (pad.handle < 0)
			continue;

		static PadSample samples[16];
		const int count = scePadRead(pad.handle, samples, 16);
		if (count < 0)
		{
			// The handle is no longer good (its user logged out): FindUsers opens the pad again.
			SetConnected(i, false);
			pad.handle = -1;
			pad.motion_on = false;
			continue;
		}
		for (int n = 0; n < count && n < 16; n++)
		{
			PadSample sample = samples[n];
			if (sample.buttons & PAD_INTERCEPTED)
			{
				// The shell has the pad (the home screen, a system dialog): nothing is held for the
				// title, and nothing else such a sample says is the title's to act on (its connection
				// state least of all: a controller must not seem unplugged at each press of PS).
				sample.buttons = 0;
				sample.left_x = sample.left_y = sample.right_x = sample.right_y = 128;
				sample.l2 = sample.r2 = 0;
				sample.touch_count = 0;
				sample.angular_velocity[0] = sample.angular_velocity[1] = sample.angular_velocity[2] = 0.0f;
				if (pad.connected)
					ApplySample(i, &sample);
				continue;
			}

			const bool connected = (sample.connected != 0);
			if (connected != pad.connected)
			{
				SetConnected(i, connected);
				if (connected)
				{
					// A controller that was switched off forgets what it was told: the rumble mode,
					// the motors, the motion sensor.
					SetVibrationMode(pad, i);
					pad.rumble_sent = {};
					SendRumble(pad);
					if (pad.motion_on)
					{
						pad.motion_on = false;
						SetMotionSensor(pad, true);
					}
				}
			}
			if (connected)
				ApplySample(i, &sample);
		}

		// The confirming double buzz plays over the game's rumble, and ends by giving it back.
		if (pad.buzz.Active())
			SendRumble(pad);
	}

	// The on-screen ring of a hold that is running.
	{
		const u64 now_us = NowMicroseconds();
		float progress = 0.0f;
		for (const Pad& pad : m_pads)
			progress = std::max(progress, pad.leave_combo.Progress(now_us));
		s_leave_progress.store(progress, std::memory_order_relaxed);
		UpdateMotionAim(now_us);
	}

	PS5Keyboard::Update();

	if (m_usb)
		PollUsbPads();
}

// ------------------------------------------------------------------------------ USB pads

void PS5InputSource::SetUsbConnected(u32 slot, bool connected)
{
	UsbPad& usb = m_usb_pads[slot];
	if (usb.connected == connected)
		return;
	const u32 index = USB_PAD_BASE + slot;
	if (connected)
	{
		usb.connected = true;
		usb.state = {};
		InputManager::OnInputDeviceConnected(fmt::format("SDL-{}", index), usb.name);
		return;
	}
	ApplyUsbState(slot, PS5UsbPadLogic::MappedPad());
	usb.connected = false;
	InputManager::OnInputDeviceDisconnected(MakeDeviceKey(index), fmt::format("SDL-{}", index));
}

void PS5InputSource::ApplyUsbState(u32 slot, const PS5UsbPadLogic::MappedPad& state)
{
	UsbPad& usb = m_usb_pads[slot];
	const u32 index = USB_PAD_BASE + slot;
	const PS5UsbPadLogic::MappedPad old = usb.state;
	usb.state = state;

	const u32 changed = old.buttons ^ state.buttons;
	for (u32 button = 0; button < BUTTON_COUNT; button++)
	{
		if (changed & (1u << button))
		{
			InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::SDL, index, button),
				(state.buttons & (1u << button)) ? 1.0f : 0.0f, s_button_info[button].generic_binding);
		}
	}
	const u32 generic_changed = old.generic_buttons ^ state.generic_buttons;
	for (u32 button = 0; button < PS5UsbPadLogic::MAX_GENERIC_BUTTONS; button++)
	{
		if (generic_changed & (1u << button))
		{
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::SDL, index, static_cast<s32>(GENERIC_BUTTON_BASE + button)),
				(state.generic_buttons & (1u << button)) ? 1.0f : 0.0f);
		}
	}
	for (u32 axis = 0; axis < AXIS_COUNT; axis++)
	{
		if (old.axes[axis] != state.axes[axis])
		{
			InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::SDL, index, static_cast<s32>(axis)),
				state.axes[axis], GenericInputBinding::Unknown, s_axis_info[axis].generic_binding_negative,
				s_axis_info[axis].generic_binding_positive);
		}
	}
	for (u32 axis = 0; axis < PS5UsbPadLogic::MAX_GENERIC_AXES; axis++)
	{
		if (old.generic_axes[axis] != state.generic_axes[axis])
		{
			InputManager::InvokeEvents(
				MakeGenericControllerAxisKey(InputSourceType::SDL, index, static_cast<s32>(GENERIC_AXIS_BASE + axis)),
				state.generic_axes[axis]);
		}
	}
}

void PS5InputSource::PollUsbPads()
{
	for (u32 slot = 0; slot < MAX_USB_PADS; slot++)
	{
		const PS5UsbPads::Slot s = m_usb->Get(slot);
		UsbPad& usb = m_usb_pads[slot];
		if (usb.connected && (!s.connected || s.generation != usb.generation))
			SetUsbConnected(slot, false);
		if (!s.connected)
			continue;
		if (!usb.connected)
		{
			usb.generation = s.generation;
			usb.name = s.name;
			usb.protocol = s.protocol;
			SetUsbConnected(slot, true);
		}
		if (!(s.state == usb.state))
			ApplyUsbState(slot, s.state);
	}
}

// ------------------------------------------------------------------------------ the rest

std::vector<std::pair<std::string, std::string>> PS5InputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> ret;
	for (u32 i = 0; i < MAX_PADS; i++)
	{
		if (m_pads[i].connected)
			ret.emplace_back(fmt::format("SDL-{}", i), DeviceName(i));
	}
	for (u32 i = 0; i < MAX_USB_PADS; i++)
	{
		if (m_usb_pads[i].connected)
			ret.emplace_back(fmt::format("SDL-{}", USB_PAD_BASE + i), m_usb_pads[i].name);
	}
	return ret;
}

std::vector<InputBindingKey> PS5InputSource::EnumerateMotors()
{
	std::vector<InputBindingKey> ret;

	InputBindingKey key = {};
	key.source_type = InputSourceType::SDL;
	key.source_subtype = InputSubclass::ControllerMotor;
	const auto add = [&ret, &key](u32 index) {
		key.source_index = index;
		key.data = MOTOR_INDEX_LARGE;
		ret.push_back(key);
		key.data = MOTOR_INDEX_SMALL;
		ret.push_back(key);
	};
	for (u32 i = 0; i < MAX_PADS; i++)
	{
		if (m_pads[i].connected)
			add(i);
	}
	for (u32 i = 0; i < MAX_USB_PADS; i++)
	{
		if (m_usb_pads[i].connected && m_usb_pads[i].protocol == PS5UsbPadLogic::Protocol::XInput)
			add(USB_PAD_BASE + i);
	}
	return ret;
}

void PS5InputSource::AppendGenericBindingMapping(u32 index, InputManager::GenericInputBindingMapping* mapping)
{
	for (const AxisInfo& ai : s_axis_info)
	{
		if (ai.generic_binding_negative != GenericInputBinding::Unknown)
			mapping->emplace_back(ai.generic_binding_negative, fmt::format("SDL-{}/-{}", index, ai.name));
		if (ai.generic_binding_positive != GenericInputBinding::Unknown)
			mapping->emplace_back(ai.generic_binding_positive, fmt::format("SDL-{}/+{}", index, ai.name));
	}
	for (const ButtonInfo& bi : s_button_info)
	{
		// The PS button never reaches a title.
		if (bi.generic_binding == GenericInputBinding::Unknown || bi.generic_binding == GenericInputBinding::System)
			continue;

		// A game's Start is the right half of the touch pad: the Options button opens the quick menu
		// (ps5_host.cpp, the hotkey's default).
		const char* const name = (bi.generic_binding == GenericInputBinding::Start) ? "Touchpad" : bi.name;
		mapping->emplace_back(bi.generic_binding, fmt::format("SDL-{}/{}", index, name));
	}
	mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/SmallMotor", index));
	mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/LargeMotor", index));
}

bool PS5InputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	if (!IsDeviceIdentifier(device))
		return false;

	const std::optional<u32> index = StringUtil::FromChars<u32>(device.substr(4));
	if (!index.has_value() || index.value() >= USB_PAD_BASE + MAX_USB_PADS)
		return false;

	if (index.value() < MAX_PADS)
	{
		AppendGenericBindingMapping(index.value(), mapping);
		return true;
	}

	// A USB pad: an Xbox 360 pad's buttons have SDL's names (its Start is Start: it has no touch
	// pad); a generic pad has its sticks and hat under SDL's names and its buttons numbered.
	const u32 i = index.value();
	const UsbPad& usb = m_usb_pads[i - USB_PAD_BASE];
	for (const AxisInfo& ai : s_axis_info)
	{
		if (ai.generic_binding_negative != GenericInputBinding::Unknown)
			mapping->emplace_back(ai.generic_binding_negative, fmt::format("SDL-{}/-{}", i, ai.name));
		if (ai.generic_binding_positive != GenericInputBinding::Unknown)
			mapping->emplace_back(ai.generic_binding_positive, fmt::format("SDL-{}/+{}", i, ai.name));
	}
	if (usb.protocol == PS5UsbPadLogic::Protocol::Hid)
	{
		for (u32 b = BUTTON_DPAD_UP; b <= BUTTON_DPAD_RIGHT; b++)
			mapping->emplace_back(s_button_info[b].generic_binding, fmt::format("SDL-{}/{}", i, s_button_info[b].name));
		for (const auto& [binding, button] : s_hid_button_guess)
			mapping->emplace_back(binding, fmt::format("SDL-{}/Button{}", i, button));
	}
	else
	{
		for (const ButtonInfo& bi : s_button_info)
		{
			if (bi.generic_binding == GenericInputBinding::Unknown || bi.generic_binding == GenericInputBinding::System)
				continue;
			mapping->emplace_back(bi.generic_binding, fmt::format("SDL-{}/{}", i, bi.name));
		}
		mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/SmallMotor", i));
		mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/LargeMotor", i));
	}
	return true;
}

InputLayout PS5InputSource::GetControllerLayout(u32 index)
{
	if (index >= USB_PAD_BASE && index < USB_PAD_BASE + MAX_USB_PADS)
	{
		const UsbPad& usb = m_usb_pads[index - USB_PAD_BASE];
		return (usb.protocol == PS5UsbPadLogic::Protocol::XInput) ? InputLayout::Xbox : InputLayout::Generic;
	}
	return InputLayout::Playstation;
}

void PS5InputSource::SendRumble(Pad& pad)
{
	// While the confirming double buzz plays it takes the motors; when it has ended, the game's
	// values are what is sent.
	std::array<u8, 2> want = pad.rumble;
	u8 buzz = 0;
	const bool buzzing = pad.buzz.Sample(NowMicroseconds(), &buzz);
	if (buzzing)
		want = {buzz, buzz};

	// A motor that is running is told again each time the emulator repeats its state (twice a
	// second): whether the console keeps a vibration going without that has not been measured.
	const bool running = (want[MOTOR_INDEX_LARGE] != 0 || want[MOTOR_INDEX_SMALL] != 0);
	if (pad.handle < 0 || (want == pad.rumble_sent && (!running || buzzing)))
		return;

	const u8 param[2] = {want[MOTOR_INDEX_LARGE], want[MOTOR_INDEX_SMALL]};
	(void)scePadSetVibration(pad.handle, param);
	pad.rumble_sent = want;
}

void PS5InputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	const u8 value = static_cast<u8>(std::clamp(intensity, 0.0f, 1.0f) * 255.0f + 0.5f);
	if (key.source_index >= USB_PAD_BASE && key.source_index < USB_PAD_BASE + MAX_USB_PADS)
	{
		// The USB pad's motors: both values kept here and sent by the reading thread.
		if (!m_usb || key.source_subtype != InputSubclass::ControllerMotor || key.data > 1)
			return;
		static std::array<std::array<u8, 2>, MAX_USB_PADS> s_usb_rumble = {};
		auto& r = s_usb_rumble[key.source_index - USB_PAD_BASE];
		r[key.data] = value;
		m_usb->SetRumble(key.source_index - USB_PAD_BASE, r[MOTOR_INDEX_LARGE], r[MOTOR_INDEX_SMALL]);
		return;
	}
	if (key.source_index >= MAX_PADS)
		return;

	Pad& pad = m_pads[key.source_index];
	if (key.source_subtype == InputSubclass::ControllerHaptic)
	{
		// A profile made for a controller with one haptic effect: both motors.
		pad.rumble = {value, value};
	}
	else if (key.source_subtype == InputSubclass::ControllerMotor && key.data <= 1)
	{
		pad.rumble[key.data] = value;
	}
	else
	{
		return;
	}
	SendRumble(pad);
}

void PS5InputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
	float small_intensity)
{
	if (large_key.source_index != small_key.source_index || large_key.source_subtype != InputSubclass::ControllerMotor ||
		small_key.source_subtype != InputSubclass::ControllerMotor || large_key.source_index >= MAX_PADS ||
		large_key.data > 1 || small_key.data > 1)
	{
		UpdateMotorState(large_key, large_intensity);
		UpdateMotorState(small_key, small_intensity);
		return;
	}

	Pad& pad = m_pads[large_key.source_index];
	pad.rumble[large_key.data] = static_cast<u8>(std::clamp(large_intensity, 0.0f, 1.0f) * 255.0f + 0.5f);
	pad.rumble[small_key.data] = static_cast<u8>(std::clamp(small_intensity, 0.0f, 1.0f) * 255.0f + 0.5f);
	SendRumble(pad);
}

std::optional<InputBindingKey> PS5InputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!IsDeviceIdentifier(device) || binding.empty())
		return std::nullopt;

	const std::optional<u32> index = StringUtil::FromChars<u32>(device.substr(4));
	if (!index.has_value())
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::SDL;
	key.source_index = index.value();

	if (binding.ends_with("Motor"))
	{
		key.source_subtype = InputSubclass::ControllerMotor;
		if (binding == "LargeMotor")
			key.data = MOTOR_INDEX_LARGE;
		else if (binding == "SmallMotor")
			key.data = MOTOR_INDEX_SMALL;
		else
			return std::nullopt;
		return key;
	}
	else if (binding.ends_with("Haptic"))
	{
		key.source_subtype = InputSubclass::ControllerHaptic;
		key.data = 0;
		return key;
	}
	else if (binding[0] == '+' || binding[0] == '-' || binding.starts_with("Full"))
	{
		std::string_view axis_name = binding.substr(binding[0] == 'F' ? 4 : 1);
		key.modifier = (binding[0] == 'F') ? InputModifier::FullAxis :
					   (binding[0] == '-') ? InputModifier::Negate :
											 InputModifier::None;
		if (!axis_name.empty() && axis_name.back() == '~')
		{
			axis_name = axis_name.substr(0, axis_name.size() - 1);
			key.invert = true;
		}

		for (u32 i = 0; i < AXIS_COUNT; i++)
		{
			if (axis_name == s_axis_info[i].name)
			{
				key.source_subtype = InputSubclass::ControllerAxis;
				key.data = i;
				return key;
			}
		}
		if (const std::optional<u32> n = NumberAfter(axis_name, "Axis", PS5UsbPadLogic::MAX_GENERIC_AXES))
		{
			key.source_subtype = InputSubclass::ControllerAxis;
			key.data = GENERIC_AXIS_BASE + n.value();
			return key;
		}
	}
	else
	{
		for (u32 i = 0; i < BUTTON_COUNT; i++)
		{
			// The face buttons also under the names a settings file of an SDL 2 build has; a
			// PlayStation controller's are where those names say.
			if (binding == s_button_info[i].name || (s_button_info[i].legacy_name && binding == s_button_info[i].legacy_name))
			{
				key.source_subtype = InputSubclass::ControllerButton;
				key.data = i;
				return key;
			}
		}
		if (const std::optional<u32> n = NumberAfter(binding, "Button", PS5UsbPadLogic::MAX_GENERIC_BUTTONS))
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = GENERIC_BUTTON_BASE + n.value();
			return key;
		}
	}

	// A hat of some other controller, or a name nothing here has.
	return std::nullopt;
}

TinyString PS5InputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;
	if (key.source_type != InputSourceType::SDL)
		return ret;

	const u32 index = key.source_index;
	if (key.source_subtype == InputSubclass::ControllerAxis && key.data < AXIS_COUNT)
	{
		const AxisInfo& ai = s_axis_info[key.data];
		if (display)
		{
			if (key.data >= AXIS_LEFT_TRIGGER)
			{
				ret.format("SDL-{} {}", index, ai.label);
			}
			else
			{
				const char* const modifier =
					(key.modifier == InputModifier::FullAxis ? "Full " : (key.modifier == InputModifier::Negate ? "-" : "+"));
				ret.format("SDL-{} {}{}", index, modifier, ai.label);
			}
		}
		else
		{
			const char* const modifier =
				(key.modifier == InputModifier::FullAxis ? "Full" : (key.modifier == InputModifier::Negate ? "-" : "+"));
			ret.format("SDL-{}/{}{}{}", index, modifier, ai.name, key.invert ? "~" : "");
		}
	}
	else if (key.source_subtype == InputSubclass::ControllerAxis && key.data >= GENERIC_AXIS_BASE &&
			 key.data < GENERIC_AXIS_BASE + PS5UsbPadLogic::MAX_GENERIC_AXES)
	{
		const u32 n = key.data - GENERIC_AXIS_BASE;
		if (display)
		{
			const char* const modifier =
				(key.modifier == InputModifier::FullAxis ? "Full " : (key.modifier == InputModifier::Negate ? "-" : "+"));
			ret.format("SDL-{} {}Axis {}", index, modifier, n);
		}
		else
		{
			const char* const modifier =
				(key.modifier == InputModifier::FullAxis ? "Full" : (key.modifier == InputModifier::Negate ? "-" : "+"));
			ret.format("SDL-{}/{}Axis{}{}", index, modifier, n, key.invert ? "~" : "");
		}
	}
	else if (key.source_subtype == InputSubclass::ControllerButton && key.data < BUTTON_COUNT)
	{
		if (display)
			ret.format("SDL-{} {}", index, s_button_info[key.data].label);
		else
			ret.format("SDL-{}/{}", index, s_button_info[key.data].name);
	}
	else if (key.source_subtype == InputSubclass::ControllerButton && key.data >= GENERIC_BUTTON_BASE &&
			 key.data < GENERIC_BUTTON_BASE + PS5UsbPadLogic::MAX_GENERIC_BUTTONS)
	{
		const u32 n = key.data - GENERIC_BUTTON_BASE;
		if (display)
			ret.format("SDL-{} Button {}", index, n);
		else
			ret.format("SDL-{}/Button{}", index, n);
	}
	else if (key.source_subtype == InputSubclass::ControllerMotor)
	{
		ret.format("SDL-{}/{}Motor", index, (key.data == MOTOR_INDEX_SMALL) ? "Small" : "Large");
	}
	else if (key.source_subtype == InputSubclass::ControllerHaptic)
	{
		ret.format("SDL-{}/Haptic", index);
	}

	return ret;
}

TinyString PS5InputSource::ConvertKeyToIcon(InputBindingKey key)
{
	TinyString ret;
	if (key.source_type != InputSourceType::SDL)
		return ret;

	const u32 index = key.source_index;
	if (key.source_subtype == InputSubclass::ControllerAxis && key.data < AXIS_COUNT && key.modifier != InputModifier::FullAxis)
	{
		const AxisInfo& ai = s_axis_info[key.data];
		const char* const icon = (key.modifier == InputModifier::None) ? ai.icon_positive : ai.icon_negative;
		if (icon)
			ret.format("SDL-{}  {}", index, icon);
	}
	else if (key.source_subtype == InputSubclass::ControllerButton && key.data < BUTTON_COUNT)
	{
		// The left-half click has no glyph of its own: spelled out, so it is not taken for the whole pad.
		const ButtonInfo& bi = s_button_info[key.data];
		if (bi.icon && key.data != BUTTON_BACK)
			ret.format("SDL-{}  {}", index, bi.icon);
	}

	return ret;
}

void PS5InputSource::SetGunCon2Defaults(SettingsInterface& si, u32 usb_port)
{
	if (usb_port >= 2)
		return;
	const std::string section = USB::GetConfigSection(static_cast<int>(usb_port));
	// clang-format off
	static constexpr std::pair<const char*, const char*> bindings[] = {
		{"Trigger", "SDL-0/+RightTrigger"},
		{"ShootOffscreen", "SDL-0/RightShoulder"},
		{"Recalibrate", "SDL-0/FaceWest"},
		{"A", "SDL-0/FaceSouth"},
		{"B", "SDL-0/FaceEast"},
		{"C", "SDL-0/FaceNorth"},
		{"Select", "SDL-0/Back"},
		{"Start", "SDL-0/Touchpad"},
		{"Up", "SDL-0/DPadUp"},
		{"Down", "SDL-0/DPadDown"},
		{"Left", "SDL-0/DPadLeft"},
		{"Right", "SDL-0/DPadRight"},
	};
	// clang-format on
	for (const auto& [name, binding] : bindings)
	{
		const std::string key = USB::GetConfigSubKey("guncon2", name);
		if (!si.ContainsValue(section.c_str(), key.c_str()))
			si.SetStringValue(section.c_str(), key.c_str(), binding);
	}
}
