// Crux2 - a PlayStation Move as player 1's GunCon 2, from Move Management's input stream.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Move Management for PS5 (GPL-3.0) is a payload of its own, loaded beside Crux2: it owns the
// Bluetooth (or the USB adapter), the controllers' registration and the camera, and sends each
// player's Move and Nav as snapshots to loopback UDP ports (its docs/PROTOCOL.md). With "Aim with a
// PlayStation Move" ([PS5] MoveAim, off by default) this source opens player 1's two, once (the Move
// on 127.0.0.1:9976, the Nav on 9980; if a port cannot be had, the log says so and it is tried again
// every five seconds), takes at most 64 datagrams from each a poll, and hands them to
// PS5MoveLogic::LightGun (ps5_move_logic.h says what is checked and how the aim is made).
//
// What the Move does, while a game runs:
//  * It aims the GunCon 2. PCSX2's GunCon aims where InputManager's pointer 0 is; the Move, while its
//    stream is live and the aim centred, sets that pointer and the controller's own motion aim is
//    left alone for player 1 (PS5InputSource::UpdateMotionAim stops there): one aim, never both
//    added. When the stream goes, the controller's aim takes over again.
//  * Its buttons press player 1's controller's buttons, the ones the GunCon's default bindings
//    (PS5InputSource::SetGunCon2Defaults) read: T the trigger (R2), the Move button A (Cross),
//    Cross B (Circle), Circle C (Triangle), Square the calibration shot (Square), Triangle a shot off
//    the screen (R1), Start the gun's Start (the touch pad); the Nav's stick and d-pad the gun's
//    d-pad, its Cross and Circle B and C, L1 Select, L2 a shot off the screen. They are added to
//    what the controller itself holds (PS5InputSource gives InputManager the two together), so the
//    controller keeps working as it did. With bindings of one's own, the Move presses whatever those
//    keys were bound to.
//  * Select centres the aim: point at the middle of the screen and press it. It centres by itself
//    half a second after the stream is found, as if pointed at the middle. The screen geometry Move
//    Management's dashboard measured for player 1 (Calibration: set up aiming, then save), in
//    /data/move-studio/aim-player-1.ini, is read each time the stream is found: with it, how far to
//    turn to the edges and where the middle lies are the measured ones; without it, Porpoise's
//    default reach.
// Nothing reaches the game outside a game (the interface keeps its own controls), and a stream that
// goes quiet for half a second, or that Move Management invalidates, lets go of all it held.
//
// The packet's times are CLOCK_MONOTONIC on this console, read here with clock_gettime (the title's,
// ps5_clock.h: kept to the kernel's, which the payload reads).

#include "ps5_move_source.h"

#include "Host.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"

#include <cstdio>
#include <ctime>
#include <memory>

extern "C" void ps5px_log(const char* format, ...);

namespace
{
	constexpr unsigned PLAYER = 0; // player 1: the GunCon's pointer is InputManager's pointer 0
	constexpr uint64_t SETTING_EVERY_US = 1000000;
	constexpr uint64_t RETRY_OPEN_US = 5000000;
	constexpr const char* PROFILE_PATH = "/data/move-studio/aim-player-1.ini";

	struct State
	{
		PS5Move::Socket move, nav;
		PS5MoveLogic::LightGun gun;
		bool enabled = false;
		bool open_failed_logged = false;
		uint64_t setting_read_us = 0;
		bool setting_read = false;
		uint64_t open_tried_us = 0;
		bool open_tried = false;
		uint32_t buttons = 0;
		uint8_t trigger = 0;
		uint32_t refused = 0, duplicates = 0; // since the stream was last found or lost, for the log
	};
	std::unique_ptr<State> s_state;

	uint64_t MonotonicMicroseconds()
	{
		timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
	}

	PS5MoveLogic::ScreenProfile ReadProfile()
	{
		std::FILE* f = std::fopen(PROFILE_PATH, "r");
		if (!f)
			return {};
		char line[160];
		const bool read = std::fgets(line, sizeof(line), f) != nullptr;
		std::fclose(f);
		return read ? PS5MoveLogic::ParseScreenProfile(line) : PS5MoveLogic::ScreenProfile{};
	}

	void CloseAll(State& s)
	{
		if (s.move.IsOpen() || s.nav.IsOpen())
			ps5px_log("move: the Move Management sockets are closed");
		s.move.Close();
		s.nav.Close();
		s.gun.Reset();
		s.buttons = 0;
		s.trigger = 0;
		s.open_tried = false;
		s.open_failed_logged = false;
	}

	void OpenAll(State& s, uint64_t now_us)
	{
		s.open_tried = true;
		s.open_tried_us = now_us;
		const uint16_t move_port = PS5MoveLogic::MOVE_PORT_BASE + PLAYER;
		const uint16_t nav_port = PS5MoveLogic::NAV_PORT_BASE + PLAYER;
		const bool move_ok = s.move.IsOpen() || s.move.Open(move_port);
		const bool nav_ok = s.nav.IsOpen() || s.nav.Open(nav_port);
		if (move_ok && nav_ok)
		{
			ps5px_log("move: listening for Move Management on 127.0.0.1 ports %u (Move) and %u (Nav)", move_port, nav_port);
			s.open_failed_logged = false;
		}
		else if (!s.open_failed_logged)
		{
			s.open_failed_logged = true;
			ps5px_log("move: cannot listen on 127.0.0.1 port%s%s%s (in use by another program?): trying again every 5 s",
				move_ok ? "" : " 9976", (!move_ok && !nav_ok) ? " and" : "", nav_ok ? "" : " 9980");
		}
	}

	void LogEvents(State& s)
	{
		const uint32_t e = s.gun.TakeEvents();
		using Gun = PS5MoveLogic::LightGun;
		if (e & Gun::MOVE_LOST)
			ps5px_log("move: player 1's Move stream lost (quiet for half a second, or invalidated): its buttons let go, "
					  "the controller's motion aim back (refused %u, duplicate %u)", s.refused, s.duplicates);
		if (e & Gun::MOVE_FOUND)
		{
			const PS5MoveLogic::ScreenProfile profile = ReadProfile();
			s.gun.SetProfile(profile);
			if (profile.valid)
				ps5px_log("move: player 1's Move stream found (generation %u); screen profile %s: %.1f by %.1f degrees to the "
						  "edges, centre offset %.1f, %.1f degrees",
					s.gun.MoveStream().Generation(), PROFILE_PATH, profile.half_x * 57.2958f, profile.half_y * 57.2958f,
					profile.yaw * 57.2958f, profile.pitch * 57.2958f);
			else
				ps5px_log("move: player 1's Move stream found (generation %u); no usable screen profile at %s: the default "
						  "reach; point at the middle of the screen and press Select to centre",
					s.gun.MoveStream().Generation(), PROFILE_PATH);
		}
		if (e & (Gun::MOVE_LOST | Gun::MOVE_FOUND))
			s.refused = s.duplicates = 0;
		if (e & Gun::NAV_FOUND)
			ps5px_log("move: player 1's Navigation controller stream found");
		if (e & Gun::NAV_LOST)
			ps5px_log("move: player 1's Navigation controller stream lost");
		if (e & Gun::AUTO_CENTRED)
			ps5px_log("move: aim centred by itself (point at the middle of the screen and press Select to centre again)");
		if (e & Gun::CENTRED)
			ps5px_log("move: aim centred (Select)");
		if (e & Gun::OPTICAL_FOUND)
			ps5px_log("move: the camera sees player 1's sphere (image-space only: not used for the aim)");
		if (e & Gun::OPTICAL_LOST)
			ps5px_log("move: the camera no longer sees player 1's sphere");
	}
} // namespace

bool PS5Move::Update(bool running)
{
	if (!s_state)
		s_state = std::make_unique<State>();
	State& s = *s_state;
	const uint64_t now_us = MonotonicMicroseconds();

	// The option, once a second (PollEvents runs without the settings' lock).
	if (!s.setting_read || now_us - s.setting_read_us >= SETTING_EVERY_US)
	{
		s.setting_read = true;
		s.setting_read_us = now_us;
		const bool enabled = Host::GetBoolSettingValue("PS5", "MoveAim", false);
		if (enabled != s.enabled)
		{
			s.enabled = enabled;
			ps5px_log("move: aim with a PlayStation Move (Move Management) %s", enabled ? "on" : "off");
			if (!enabled)
				CloseAll(s);
		}
	}
	if (!s.enabled)
		return false;
	if ((!s.move.IsOpen() || !s.nav.IsOpen()) && (!s.open_tried || now_us - s.open_tried_us >= RETRY_OPEN_US))
		OpenAll(s, now_us);

	auto take = [&s](bool nav) {
		return [&s, nav](const uint8_t* bytes, size_t size) {
			switch (s.gun.Offer(nav, bytes, size, MonotonicMicroseconds()))
			{
				case PS5MoveLogic::Stream::Offer::Refused:
					s.refused++;
					break;
				case PS5MoveLogic::Stream::Offer::Duplicate:
					s.duplicates++;
					break;
				default:
					break;
			}
		};
	};
	s.move.Drain(take(false));
	s.nav.Drain(take(true));

	const PS5MoveLogic::LightGun::Output out = s.gun.Tick(MonotonicMicroseconds());
	LogEvents(s);
	s.buttons = running ? out.sdl_buttons : 0;
	s.trigger = running ? out.trigger : 0;
	if (!running || !out.aiming)
		return false;

	float width = ImGuiManager::GetWindowWidth();
	float height = ImGuiManager::GetWindowHeight();
	if (!(width > 0.0f) || !(height > 0.0f))
	{
		width = 1920.0f;
		height = 1080.0f;
	}
	InputManager::UpdatePointerAbsolutePosition(0, (out.x * 0.5f + 0.5f) * width, (out.y * 0.5f + 0.5f) * height);
	return true;
}

uint32_t PS5Move::GameButtons()
{
	return s_state ? s_state->buttons : 0;
}

uint8_t PS5Move::Trigger()
{
	return s_state ? s_state->trigger : 0;
}

void PS5Move::Shutdown()
{
	if (s_state)
	{
		CloseAll(*s_state);
		s_state->enabled = false;
		s_state->setting_read = false;
	}
}
