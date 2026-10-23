// Crux2 - the controllers, through the console's pad service (ps5_input_source.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include "Input/InputSource.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "ps5_input_source_logic.h"
#include "ps5_usbpad_logic.h"

namespace PS5UsbPads
{
	class Manager;
}

class PS5InputSource final : public InputSource
{
public:
	static constexpr u32 MAX_PADS = 4; // the console's controllers: SDL-0 to SDL-3
	static constexpr u32 USB_PAD_BASE = 4; // USB pads: SDL-4 to SDL-7
	static constexpr u32 MAX_USB_PADS = 4;
	static constexpr u32 NUM_AXES = 6;

	PS5InputSource();
	~PS5InputSource() override;

	bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	bool ReloadDevices() override;
	void Shutdown() override;
	bool IsInitialized() override;

	void PollEvents() override;

	std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) override;
	TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) override;
	TinyString ConvertKeyToIcon(InputBindingKey key) override;

	std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
	std::vector<InputBindingKey> EnumerateMotors() override;
	bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) override;
	InputLayout GetControllerLayout(u32 index) override;

	void UpdateMotorState(InputBindingKey key, float intensity) override;
	void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
		float small_intensity) override;

	/// The automatic mapping of one controller, also used for the first-run defaults before any
	/// controller is connected (ps5_host.cpp).
	static void AppendGenericBindingMapping(u32 index, InputManager::GenericInputBindingMapping* mapping);

	/// "Leave the game" by holding a combination ([PS5] LeaveGameCombo, LeaveGameHoldSeconds):
	/// called once the hold is complete and the double buzz has started, with the player (0 for
	/// SDL-0) who held it. Called on the thread that polls input, while a game runs; the handler
	/// does what the quick menu's "Leave the game" does (posting it to wherever that runs). With no
	/// handler, the combination only buzzes.
	static void SetLeaveGameHandler(std::function<void(u32 player)> handler);

	/// How far a leave-the-game hold is (0 to 1; 0 with none running), for an on-screen ring.
	static float LeaveGameHoldProgress();

	/// Writes the bindings of a GunCon 2 in a USB port (0 or 1) for player 1's controller, aimed
	/// with its motion sensor: R2 the trigger, R1 a shot off the screen, Cross/Circle/Triangle
	/// the gun's A/B/C, Square the calibration shot, the touch pad's halves Select and Start, the
	/// d-pad the gun's d-pad. Bindings the user already has are kept. R3 puts the aim back in the
	/// middle (not a binding: the input source does it while motion aim is on).
	static void SetGunCon2Defaults(SettingsInterface& si, u32 usb_port);

private:
	struct Pad
	{
		s32 user_id = -1;
		s32 handle = -1;
		bool connected = false; // announced to the input manager
		u32 buttons = 0; // bit n: SDL button n is down
		std::array<float, 21> button_values = {}; // what each button was last reported as (pressure)
		std::array<u8, NUM_AXES> axes = {128, 128, 128, 128, 0, 0}; // as last reported
		std::array<u8, 2> rumble = {}; // what the emulator wants: large, small
		std::array<u8, 2> rumble_sent = {}; // what the controller was last told
		bool touch_is_back = false; // the click now held started on the left half
		bool vibration_mode_logged = false;

		PS5InputLogic::HoldCombo leave_combo;
		PS5InputLogic::DoubleBuzz buzz;
		u64 last_timestamp_us = 0; // of the last sample, for the motion sensor
		bool motion_on = false; // the motion sensor was switched on for this pad
	};

	struct UsbPad
	{
		bool connected = false;
		u32 generation = 0;
		std::string name;
		PS5UsbPadLogic::Protocol protocol = PS5UsbPadLogic::Protocol::None;
		PS5UsbPadLogic::MappedPad state;
	};

	void FindUsers();
	void OpenPad(u32 index);
	void SetVibrationMode(Pad& pad, u32 index);
	void ApplySample(u32 index, const void* sample);
	void SetConnected(u32 index, bool connected);
	void SendRumble(Pad& pad);
	static std::string DeviceName(u32 index);
	void ReadSettings(SettingsInterface& si);

	std::array<Pad, MAX_PADS> m_pads;

	// "Light bar in the player's colour" ([PS5] LightBars): each controller's light bar says which
	// player it is. Off: the bars are left as the console has them.
	void ShowPlayer(u32 index);
	bool m_light_bars = false;
	u64 m_last_user_scan = 0;
	bool m_initialized = false;

	// "Pressure from R2" ([PS5] PressureFromR2).
	bool m_pressure_from_r2 = false;

	// Leaving the game by a hold.
	u32 m_leave_mask = 0;
	u64 m_leave_hold_us = 2000000;

	// Aiming with the motion sensor ([PS5] MotionAim, or a GunCon 2 in a USB port).
	void UpdateMotionAim(u64 now_us);
	void SetMotionSensor(Pad& pad, bool on);
	bool m_motion_aim_setting = false;
	bool m_guncon_plugged = false;
	bool m_motion_active = false;
	u64 m_last_guncon_check = 0;
	PS5InputLogic::GyroAim m_aim;
	PS5InputLogic::GyroAimSettings m_aim_settings;
	bool m_aim_moved = false;

	// USB pads ([PS5] USBPads).
	void PollUsbPads();
	void SetUsbConnected(u32 slot, bool connected);
	void ApplyUsbState(u32 slot, const PS5UsbPadLogic::MappedPad& state);
	bool m_usb_pads_setting = false;
	std::unique_ptr<PS5UsbPads::Manager> m_usb;
	std::array<UsbPad, MAX_USB_PADS> m_usb_pads;
};
