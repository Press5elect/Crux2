// Crux2 - a PlayStation Move as player 1's GunCon 2, from Move Management's input stream: the parts
// with no console in them (ps5/tests/move checks them on the PC). The console's side is
// ps5_move_source.cpp.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Move Management for PS5 (GPL-3.0; https://github.com/REXCRAFT88/Move-Management-PS5) is a separate
// payload that owns the Move's Bluetooth, USB registration and camera, and publishes each player's
// Move and Navigation controller as 184-byte "MVB1" snapshots over loopback UDP (its
// docs/PROTOCOL.md; P1's Move on port 9976, P1's Nav on 9980). This reads them as that document and
// its reference client (integrations/move_management_client.hpp) do: every field little-endian and
// read byte by byte, wrong magic/version/size, nonzero reserved fields, flags other than 0 and 1,
// non-finite floats, optics out of range, timestamps in the future or older than half a second
// refused; within a live stream a sequence number not above the last one is a duplicate or out of
// order and refused, and a new run of the stream (after half a second of nothing, an invalidation,
// or the producer starting again with its sequence reset) is a new connection generation: the aim
// starts again from nothing.
//
// What the stream gives and does not (PROTOCOL.md, INTEGRATION.md): acceleration in g with gravity
// and angular velocity in rad/s, already in a DualSense's axes (pad X = Move X, pad Y = Move Z,
// pad Z = -Move Y, factory scale and the optional session gyro zero applied once by the producer: not
// applied again here). The quaternion is identity, not a measured pose, and is not used. The camera's
// sphere position is image-space only (pixels of a 320x240 picture and the apparent size): Move
// Management itself says it is not a validated world position or pose, and that optical drift
// correction needs an alignment and fusion policy it does not have yet. So it is checked (a packet
// with a bad optical block is refused, as the document asks) and reported as seen or not, but it does
// not move the pointer: the aim is the gyroscope's and the accelerometer's alone.
//
// The aim is Porpoise's (Project Porpoise, src/porpoise_aim.cpp, Copyright (C) 2026 Ruben,
// GPL-3.0-or-later): the gyroscope and accelerometer fused into an orientation (gravity keeps "up";
// the drift is learnt while the controller lies still), from it where the controller points (yaw and
// pitch against gravity, so it reads the same however it is twisted in the hand), and from that the
// pointer against the direction that was "the middle of the screen" when it was last centred. Move
// Management's own screen-aiming setup uses the same code, unchanged (its third_party/porpoise_aim),
// and saves what it measured for each player as one line, "MVA1 half_x half_y yaw_offset
// pitch_offset" (radians; /data/move-studio/aim-player-N.ini): how far the controller turns from the
// middle to the screen's edges, and how far the middle of the screen lies from where the player
// pointed when told "point at the centre". The yaw has no fixed zero (the gyroscope knows only turns),
// so a centring is always needed here too: the profile then makes it exact (the measured edges and
// the centre's offset); without one, Porpoise's default reach (15.5 degrees to the side) is used. The
// functions of porpoise_aim.cpp this needs are in ps5_aim_logic.h (PorpoiseAim), as they are, with
// their notice.

#pragma once

#include "ps5_aim_logic.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace PS5MoveLogic
{
	// ---------------------------------------------------------------- the wire (PROTOCOL.md)

	constexpr uint16_t MOVE_PORT_BASE = 9976; // primary Move, + player (0..3)
	constexpr uint16_t NAV_PORT_BASE = 9980; // Navigation controller, + player
	constexpr uint32_t MAGIC = 0x3142564d; // "MVB1"
	constexpr size_t SIZE_V1 = 152;
	constexpr size_t SIZE_V2 = 184;
	constexpr size_t PAD_OFFSET = 32;
	constexpr size_t PAD_SIZE = 120;
	constexpr uint64_t INPUT_STALE_US = 500000; // input older than this is stale
	constexpr uint64_t OPTICAL_STALE_US = 200000; // camera observations older than this are stale
	/// How far ahead of this title's clock a packet's time may be. Both ends read CLOCK_MONOTONIC on the
	/// same console, but the title's clock_gettime is answered from the processor's counter and kept to
	/// the kernel's (ps5_clock.h): within a millisecond or so, never more than 50 ms behind. A packet
	/// from further ahead than that is from the future and refused.
	constexpr uint64_t FUTURE_SLACK_US = 50000;

	// The wire's button bits.
	enum : uint32_t
	{
		WIRE_SELECT = 0x1,
		WIRE_L3 = 0x2,
		WIRE_R3 = 0x4,
		WIRE_START = 0x8,
		WIRE_UP = 0x10,
		WIRE_RIGHT = 0x20,
		WIRE_DOWN = 0x40,
		WIRE_LEFT = 0x80,
		WIRE_L2 = 0x100, // the Move's T, the Nav's L2
		WIRE_R2 = 0x200,
		WIRE_L1 = 0x400,
		WIRE_R1 = 0x800,
		WIRE_TRIANGLE = 0x1000,
		WIRE_CIRCLE = 0x2000,
		WIRE_CROSS = 0x4000,
		WIRE_SQUARE = 0x8000,
		WIRE_PS = 0x10000,
		WIRE_MOVE = 0x20000, // the Move button (not Cross)
	};

	inline uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
	inline uint32_t U32(const uint8_t* p)
	{
		return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
	}
	inline uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | (uint64_t(U32(p + 4)) << 32); }
	inline float F32(const uint8_t* p)
	{
		const uint32_t bits = U32(p);
		float f;
		std::memcpy(&f, &bits, sizeof(f));
		return f;
	}

	/// One snapshot, as read off the wire.
	struct Packet
	{
		uint16_t version = 0;
		uint64_t sequence = 0;
		uint64_t sent_us = 0; // the producer's CLOCK_MONOTONIC
		bool valid = false; // false: the producer invalidated the stream (a disconnect)
		bool connected = false;
		uint32_t buttons = 0;
		uint8_t lx = 128, ly = 128, rx = 128, ry = 128; // centre 128, y down
		uint8_t l2 = 0, r2 = 0; // the Move's T is l2
		float accel[3] = {}; // g, gravity included, in a DualSense's axes
		float gyro[3] = {}; // rad/s, the same axes
		uint64_t sample_us = 0;
		bool optical_valid = false; // the camera saw the sphere (image space only)
		float optical_x = 0, optical_y = 0, optical_w = 0, optical_h = 0;
		uint64_t optical_us = 0;
	};

	/// Reads one datagram (n bytes) received at now_us. False for anything PROTOCOL.md says to refuse.
	inline bool Decode(const uint8_t* b, size_t n, uint64_t now_us, Packet& out)
	{
		if (!b || n < PAD_OFFSET || U32(b) != MAGIC)
			return false;
		const uint16_t version = U16(b + 4);
		const size_t expected = (version == 1) ? SIZE_V1 : (version == 2) ? SIZE_V2 : 0;
		if (expected == 0 || n != expected || U16(b + 6) != expected)
			return false;
		if (U32(b + 28) != 0 || U32(b + 24) > 1)
			return false;
		const uint64_t sent = U64(b + 16);
		if (sent > now_us + FUTURE_SLACK_US || (now_us > sent && now_us - sent > INPUT_STALE_US))
			return false;
		const uint8_t* p = b + PAD_OFFSET;
		const uint32_t connected = U32(p + 76);
		if (connected > 1)
			return false;
		for (size_t i = 0; i < 10; i++) // the quaternion, the acceleration, the angular velocity
		{
			if (!std::isfinite(F32(p + 12 + i * 4)))
				return false;
		}
		Packet pk;
		pk.version = version;
		pk.sequence = U64(b + 8);
		pk.sent_us = sent;
		pk.valid = U32(b + 24) == 1;
		pk.connected = connected == 1;
		pk.buttons = U32(p);
		pk.lx = p[4];
		pk.ly = p[5];
		pk.rx = p[6];
		pk.ry = p[7];
		pk.l2 = p[8];
		pk.r2 = p[9];
		for (int i = 0; i < 3; i++)
		{
			pk.accel[i] = F32(p + 28 + i * 4);
			pk.gyro[i] = F32(p + 40 + i * 4);
		}
		pk.sample_us = U64(p + 80);
		if (pk.sample_us > sent)
			return false; // a reading taken after it was sent
		if (version == 2)
		{
			const uint32_t optical_valid = U32(b + 176);
			if (U32(b + 180) != 0 || optical_valid > 1)
				return false;
			pk.optical_x = F32(b + 152);
			pk.optical_y = F32(b + 156);
			pk.optical_w = F32(b + 160);
			pk.optical_h = F32(b + 164);
			pk.optical_us = U64(b + 168);
			pk.optical_valid = optical_valid == 1;
			if (pk.optical_us > sent)
				return false;
			if (pk.optical_valid &&
				(!std::isfinite(pk.optical_x) || !std::isfinite(pk.optical_y) || !std::isfinite(pk.optical_w) ||
					!std::isfinite(pk.optical_h) || pk.optical_x < 0 || pk.optical_x > 319 || pk.optical_y < 0 ||
					pk.optical_y > 239 || pk.optical_w < 1 || pk.optical_w > 100 || pk.optical_h < 1 || pk.optical_h > 100))
				return false;
		}
		out = pk;
		return true;
	}

	/// One stream (one port): what was last accepted, and whether it is live.
	class Stream
	{
	public:
		enum class Offer
		{
			Refused, // not a packet PROTOCOL.md accepts
			Duplicate, // a sequence number not above the live stream's last
			Accepted,
			NewGeneration, // accepted, and the start of a new connection
		};

		Offer Take(const uint8_t* b, size_t n, uint64_t now_us)
		{
			Packet pk;
			if (!Decode(b, n, now_us, pk))
				return Offer::Refused;
			const bool active = Active(now_us);
			if (active && pk.sequence <= m_packet.sequence)
				return Offer::Duplicate;
			const bool was_live = Live(now_us);
			m_packet = pk;
			m_have = true;
			m_rx_us = now_us;
			if (Usable(pk) && !was_live)
			{
				m_generation++;
				return Offer::NewGeneration;
			}
			return Offer::Accepted;
		}

		/// Something accepted within the last half second (it may say "invalid").
		bool Active(uint64_t now_us) const { return m_have && now_us >= m_rx_us && now_us - m_rx_us <= INPUT_STALE_US; }

		/// Input to use: active, valid, connected, and not stale by its own time.
		bool Live(uint64_t now_us) const
		{
			return Active(now_us) && Usable(m_packet) && (now_us <= m_packet.sent_us || now_us - m_packet.sent_us <= INPUT_STALE_US);
		}

		bool OpticalFresh(uint64_t now_us) const
		{
			return Live(now_us) && m_packet.version == 2 && m_packet.optical_valid &&
				   (now_us <= m_packet.optical_us || now_us - m_packet.optical_us <= OPTICAL_STALE_US);
		}

		const Packet& Last() const { return m_packet; }
		uint32_t Generation() const { return m_generation; }
		void Reset()
		{
			m_have = false;
			m_packet = Packet();
		}

	private:
		static bool Usable(const Packet& p) { return p.valid && p.connected; }

		Packet m_packet;
		bool m_have = false;
		uint64_t m_rx_us = 0;
		uint32_t m_generation = 0;
	};

	// ---------------------------------------------------------------- the saved screen (CALIBRATION.md)

	struct ScreenProfile
	{
		bool valid = false;
		float half_x = 0, half_y = 0, yaw = 0, pitch = 0; // radians
	};

	/// The first line of aim-player-N.ini, with the checks of Move Management's
	/// integrations/aim_screen_profile.hpp (the spans its setup accepts, the offsets it allows).
	inline ScreenProfile ParseScreenProfile(const char* line)
	{
		ScreenProfile p;
		char extra;
		if (!line || std::sscanf(line, "MVA1 %f %f %f %f %c", &p.half_x, &p.half_y, &p.yaw, &p.pitch, &extra) != 4)
			return {};
		constexpr float DEG = 57.2957795131f;
		const bool ok = std::isfinite(p.half_x) && std::isfinite(p.half_y) && std::isfinite(p.yaw) && std::isfinite(p.pitch) &&
						p.half_x * DEG > 3 && p.half_x * DEG < 75 && p.half_y * DEG > 2 && p.half_y * DEG < 60 &&
						p.half_y / p.half_x > 0.25f && p.half_y / p.half_x < 1.3f && std::fabs(p.yaw) < 1.31f &&
						std::fabs(p.pitch) < 1.05f;
		if (!ok)
			return {};
		p.valid = true;
		return p;
	}

	// ---------------------------------------------------------------- Porpoise's aim

	/// Porpoise's functions (fuse, remote_angles, scale_for, pointer, level_grip, Smoother; the grip
	/// used here, kGripNormal), shared with the DualSense's aim in ps5_aim_logic.h.
	namespace PorpoiseAim = PS5AimLogic::PorpoiseAim;

	// ---------------------------------------------------------------- held long enough to be seen

	/// A press shorter than a poll held for at least MIN_HOLD_US (the game reads the gun once a frame;
	/// the stream may carry a press and its release between two polls). Bit n: one input.
	class Hold
	{
	public:
		static constexpr uint64_t MIN_HOLD_US = 50000;

		void Reading(uint32_t down, uint64_t now_us)
		{
			const uint32_t went = down & ~m_down;
			for (uint32_t bit = 0; bit < 32; bit++)
			{
				if (went & (1u << bit))
					m_since[bit] = now_us;
			}
			m_latched |= went;
			m_down = down;
		}

		uint32_t Given(uint64_t now_us)
		{
			uint32_t out = m_down;
			for (uint32_t bit = 0; bit < 32; bit++)
			{
				const uint32_t mask = 1u << bit;
				if (!(m_latched & mask) || (m_down & mask))
					continue;
				if (now_us >= m_since[bit] && now_us - m_since[bit] < MIN_HOLD_US)
					out |= mask;
				else
					m_latched &= ~mask;
			}
			return out;
		}

		void Reset() { m_down = m_latched = 0; }

	private:
		uint32_t m_down = 0, m_latched = 0;
		uint64_t m_since[32] = {};
	};

	// ---------------------------------------------------------------- the light gun

	/// SDL's gamepad button numbers, as ps5_input_source.cpp names player 1's controller's buttons
	/// ("SDL-0/FaceSouth" ...): the Move presses the same keys, so the GunCon 2's bindings of
	/// PS5InputSource::SetGunCon2Defaults take it without any binding of its own.
	enum : uint32_t
	{
		SDL_SOUTH = 0, // GunCon A
		SDL_EAST = 1, // GunCon B
		SDL_WEST = 2, // the calibration shot
		SDL_NORTH = 3, // GunCon C
		SDL_BACK = 4, // GunCon Select (the touch pad's left half)
		SDL_RIGHT_SHOULDER = 10, // a shot off the screen
		SDL_DPAD_UP = 11,
		SDL_DPAD_DOWN = 12,
		SDL_DPAD_LEFT = 13,
		SDL_DPAD_RIGHT = 14,
		SDL_TOUCHPAD = 20, // GunCon Start (the touch pad's right half)
	};
	constexpr uint32_t Sdl(uint32_t button) { return 1u << button; }

	// Inputs as the Hold counts them: the wire's bits, and T as one of its own (bit 31: unused on the
	// wire).
	constexpr uint32_t HELD_T = 0x80000000u;
	// The Nav's stick as d-pad bits (the wire's d-pad bits are the same places).
	constexpr uint32_t STICK_UP = 1u << 24, STICK_DOWN = 1u << 25, STICK_LEFT = 1u << 26, STICK_RIGHT = 1u << 27;

	/// The Move's buttons (held: the wire's bits, HELD_T for T) as player 1's SDL buttons:
	///   Move button -> A, Cross -> B, Circle -> C, Square -> the calibration shot, Triangle -> a shot
	///   off the screen (a reload), Start -> Start. T is the trigger (an axis: see Output). Select
	///   centres the aim and is not passed on; PS is left alone.
	inline uint32_t MapMove(uint32_t held)
	{
		uint32_t out = 0;
		if (held & WIRE_MOVE)
			out |= Sdl(SDL_SOUTH);
		if (held & WIRE_CROSS)
			out |= Sdl(SDL_EAST);
		if (held & WIRE_CIRCLE)
			out |= Sdl(SDL_NORTH);
		if (held & WIRE_SQUARE)
			out |= Sdl(SDL_WEST);
		if (held & WIRE_TRIANGLE)
			out |= Sdl(SDL_RIGHT_SHOULDER);
		if (held & WIRE_START)
			out |= Sdl(SDL_TOUCHPAD);
		return out;
	}

	/// The Nav's: its stick and its d-pad -> the gun's d-pad, Cross -> B, Circle -> C, L1 -> Select,
	/// L2 -> a shot off the screen.
	inline uint32_t MapNav(uint32_t held)
	{
		uint32_t out = 0;
		if (held & (WIRE_UP | STICK_UP))
			out |= Sdl(SDL_DPAD_UP);
		if (held & (WIRE_DOWN | STICK_DOWN))
			out |= Sdl(SDL_DPAD_DOWN);
		if (held & (WIRE_LEFT | STICK_LEFT))
			out |= Sdl(SDL_DPAD_LEFT);
		if (held & (WIRE_RIGHT | STICK_RIGHT))
			out |= Sdl(SDL_DPAD_RIGHT);
		if (held & WIRE_CROSS)
			out |= Sdl(SDL_EAST);
		if (held & WIRE_CIRCLE)
			out |= Sdl(SDL_NORTH);
		if (held & WIRE_L1)
			out |= Sdl(SDL_BACK);
		if (held & (WIRE_L2 | HELD_T))
			out |= Sdl(SDL_RIGHT_SHOULDER);
		return out;
	}

	/// A trigger's analog value (0..255) as a click: pulled past PRESS it is down, and it is let go
	/// below RELEASE (a light gun's trigger is a click; half of T's long travel is not a shot).
	inline bool TriggerDown(bool was_down, uint8_t value)
	{
		constexpr uint8_t PRESS = 80, RELEASE = 48;
		return was_down ? value >= RELEASE : value >= PRESS;
	}

	/// A stick axis (0..255, centre 128) as -1, 0 or +1, with the same kind of hysteresis.
	inline int StickDirection(int was, uint8_t value)
	{
		const int d = int(value) - 128;
		constexpr int PRESS = 64, RELEASE = 40;
		if (was > 0 && d >= RELEASE)
			return 1;
		if (was < 0 && d <= -RELEASE)
			return -1;
		return d >= PRESS ? 1 : d <= -PRESS ? -1 : 0;
	}

	class LightGun
	{
	public:
		enum Event : uint32_t
		{
			MOVE_FOUND = 1, // the Move's stream became live (a new generation)
			MOVE_LOST = 2,
			NAV_FOUND = 4,
			NAV_LOST = 8,
			CENTRED = 16, // by Select
			AUTO_CENTRED = 32, // by itself, once the new generation's fusion settled
			OPTICAL_FOUND = 64, // the camera's sphere (reported only)
			OPTICAL_LOST = 128,
		};

		struct Output
		{
			uint32_t sdl_buttons = 0; // player 1's SDL buttons held by the Move and the Nav
			uint8_t trigger = 0; // 0 or 255: the right trigger's axis (the GunCon's trigger)
			bool aiming = false; // the Move is live and centred: x and y are where it points
			float x = 0, y = 0; // -1..1 to the screen's edges, x right, y down; past 1 off the screen
		};

		void SetProfile(const ScreenProfile& profile) { m_profile = profile; }
		const ScreenProfile& Profile() const { return m_profile; }

		/// One datagram from the Move's (nav false) or the Nav's port.
		Stream::Offer Offer(bool nav, const uint8_t* b, size_t n, uint64_t now_us)
		{
			Stream& stream = nav ? m_nav : m_move;
			const Stream::Offer r = stream.Take(b, n, now_us);
			if (r != Stream::Offer::Accepted && r != Stream::Offer::NewGeneration)
				return r;
			if (r == Stream::Offer::NewGeneration)
			{
				if (nav)
					StartNav();
				else
					StartMove();
				m_events |= nav ? NAV_FOUND : MOVE_FOUND;
				(nav ? m_nav_reported : m_move_reported) = true;
			}
			const Packet& p = stream.Last();
			if (!p.valid || !p.connected)
				return r; // an invalidation: Tick lets go
			if (nav)
				NavReading(p, now_us);
			else
				MoveReading(p, now_us);
			return r;
		}

		/// Once a poll: what player 1's gun is given now. Streams that went quiet or were invalidated
		/// let go of everything they held.
		Output Tick(uint64_t now_us)
		{
			Output out;
			const bool move_live = m_move.Live(now_us), nav_live = m_nav.Live(now_us);
			if (!move_live && m_move_reported)
			{
				m_move_reported = false;
				m_events |= MOVE_LOST;
				StartMove();
			}
			if (!nav_live && m_nav_reported)
			{
				m_nav_reported = false;
				m_events |= NAV_LOST;
				StartNav();
			}
			const bool optical = m_move.OpticalFresh(now_us);
			if (optical != m_optical_reported)
			{
				m_optical_reported = optical;
				m_events |= optical ? OPTICAL_FOUND : OPTICAL_LOST;
			}

			if (move_live)
			{
				MoveSide& m = m_m;
				// Centring by itself: half a second in, once gravity has settled and the controller
				// is fairly still, or after three seconds regardless (Porpoise's rule), as if it was
				// pointed at the middle of the screen. Select centres it at any time.
				if (!m.centred && m.fusion.started && m.fusion.age > 0.5f && (m.turning < 0.8f || m.fusion.age > 3.0f))
				{
					Centre();
					m_events |= AUTO_CENTRED;
				}
				const uint32_t held = m.hold.Given(now_us);
				out.sdl_buttons |= MapMove(held);
				out.trigger = (held & HELD_T) ? 255 : 0;
				if (m.centred)
				{
					const PorpoiseAim::Angles now = PorpoiseAim::remote_angles(m.fusion, m.basis);
					float x, y;
					PorpoiseAim::pointer(now, m.centre, m.scale, x, y);
					m.smoother.step(x, y, m.elapsed, SMOOTHING);
					m.elapsed = 0;
					out.aiming = true;
					out.x = std::clamp(x, -1.5f, 1.5f);
					out.y = std::clamp(y, -1.5f, 1.5f);
				}
			}
			if (nav_live)
				out.sdl_buttons |= MapNav(m_n.hold.Given(now_us));
			return out;
		}

		/// What happened since the last call (Event bits), for the log.
		uint32_t TakeEvents()
		{
			const uint32_t e = m_events;
			m_events = 0;
			return e;
		}

		/// Everything let go and forgotten (the option was switched off, the sockets closed).
		void Reset()
		{
			m_move.Reset();
			m_nav.Reset();
			StartMove();
			StartNav();
			m_move_reported = m_nav_reported = m_optical_reported = false;
			m_events = 0;
		}

		const Stream& MoveStream() const { return m_move; }
		const Stream& NavStream() const { return m_nav; }
		bool Centred() const { return m_m.centred; }

	private:
		static constexpr int SMOOTHING = 1; // Porpoise's lightest
		static constexpr int DEFAULT_SPEED = 5; // Porpoise's default reach: 15.5 degrees to the side

		struct MoveSide
		{
			PorpoiseAim::Fusion fusion;
			PorpoiseAim::Basis basis = PorpoiseAim::kGripNormal;
			PorpoiseAim::Angles centre;
			PorpoiseAim::Scale scale;
			PorpoiseAim::Smoother smoother;
			float up[3] = {0, 0, 0};
			float turning = 0;
			float elapsed = 0; // seconds of readings since the last Tick
			uint64_t last_sample_us = 0;
			uint32_t buttons = 0;
			bool t_down = false;
			bool centred = false;
			Hold hold;
		};
		struct NavSide
		{
			int sx = 0, sy = 0;
			Hold hold;
		};

		void StartMove() { m_m = MoveSide(); }
		void StartNav() { m_n = NavSide(); }

		void Centre()
		{
			MoveSide& m = m_m;
			PorpoiseAim::level_grip(PorpoiseAim::kGripNormal, m.up, m.basis);
			m.centre = PorpoiseAim::remote_angles(m.fusion, m.basis);
			if (m_profile.valid)
			{
				// Where the middle of the screen lies from "the centre" the player pointed at.
				m.centre.yaw = PorpoiseAim::detail::wrap(m.centre.yaw + m_profile.yaw);
				m.centre.pitch += m_profile.pitch;
				m.scale = PorpoiseAim::scale_for(DEFAULT_SPEED, m_profile.half_x, m_profile.half_y);
			}
			else
			{
				m.scale = PorpoiseAim::scale_for(DEFAULT_SPEED);
			}
			m.smoother.reset();
			m.centred = true;
		}

		void MoveReading(const Packet& p, uint64_t now_us)
		{
			MoveSide& m = m_m;
			// Every reading, in order, into the orientation. A reading time that does not move on
			// (none, or the same again) is not fused twice.
			const uint64_t t = p.sample_us ? p.sample_us : p.sent_us;
			if (!m.last_sample_us || t > m.last_sample_us)
			{
				const float dt = m.last_sample_us ? float(t - m.last_sample_us) * 1e-6f : 0.0f;
				PorpoiseAim::fuse(m.fusion, p.gyro, p.accel, dt);
				m.last_sample_us = t;
				const float step = std::min(dt, 0.05f);
				m.elapsed += step;
				// "Up", eased over a sixth of a second, for levelling the grip at a centring.
				const bool first = m.up[0] == 0 && m.up[1] == 0 && m.up[2] == 0;
				const float k = first ? 1.0f : std::min(1.0f, step / 0.16f);
				for (int a = 0; a < 3; a++)
					m.up[a] += (p.accel[a] - m.up[a]) * k;
				float g2 = 0;
				for (int a = 0; a < 3; a++)
				{
					const float g = p.gyro[a] - m.fusion.bias[a];
					g2 += g * g;
				}
				m.turning = std::sqrt(g2);
			}
			const bool select_pressed = (p.buttons & WIRE_SELECT) && !(m.buttons & WIRE_SELECT);
			m.buttons = p.buttons;
			if (select_pressed && m.fusion.started)
			{
				Centre();
				m_events |= CENTRED;
			}
			m.t_down = TriggerDown(m.t_down, p.l2);
			m.hold.Reading((p.buttons & ~(WIRE_SELECT | WIRE_L2)) | (m.t_down ? HELD_T : 0), now_us);
		}

		void NavReading(const Packet& p, uint64_t now_us)
		{
			NavSide& n = m_n;
			n.sx = StickDirection(n.sx, p.lx);
			n.sy = StickDirection(n.sy, p.ly); // y down
			uint32_t held = p.buttons;
			if (n.sx > 0)
				held |= STICK_RIGHT;
			if (n.sx < 0)
				held |= STICK_LEFT;
			if (n.sy > 0)
				held |= STICK_DOWN;
			if (n.sy < 0)
				held |= STICK_UP;
			if (p.l2 >= 128)
				held |= HELD_T;
			n.hold.Reading(held, now_us);
		}

		Stream m_move, m_nav;
		MoveSide m_m;
		NavSide m_n;
		ScreenProfile m_profile;
		bool m_move_reported = false, m_nav_reported = false, m_optical_reported = false;
		uint32_t m_events = 0;
	};
} // namespace PS5MoveLogic
