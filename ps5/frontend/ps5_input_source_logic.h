// Crux2 - the thinking behind the controller features of ps5_input_source.cpp, without the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Everything here is plain arithmetic over what a controller reported and when, so the PC tests
// (ps5/tests/input) hold it to account: the held combination that asks to leave the game, the
// double buzz that confirms it, the pressure a face button reports when R2 sets it, and the
// pointer the motion sensor aims with.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace PS5InputLogic
{
	// ---------------------------------------------------------------- leaving the game by a hold

	/// A combination of buttons held for a while. It counts only when the combination becomes
	/// complete (a combination already held when it is configured, or still held after it fired,
	/// does nothing until it is let go), it is lost the moment one of its buttons is released, and
	/// it fires once per hold. Extra buttons held at the same time do not matter.
	class HoldCombo
	{
	public:
		/// mask: the buttons (any bit numbering; the source uses SDL's), 0 for none. hold_us: how long.
		void Configure(uint32_t mask, uint64_t hold_us)
		{
			if (mask == m_mask && hold_us == m_hold_us)
				return;
			m_mask = mask;
			m_hold_us = hold_us;
			m_holding = false;
			m_complete_before = true; // whatever is held now must be let go first
		}

		uint32_t Mask() const { return m_mask; }

		/// Called with what is held now. True exactly once per hold, when it has lasted long enough.
		bool Update(uint32_t held, uint64_t now_us)
		{
			if (m_mask == 0)
				return false;

			const bool complete = (held & m_mask) == m_mask;
			if (!complete)
			{
				m_holding = false;
				m_complete_before = false;
				return false;
			}
			if (!m_complete_before)
			{
				m_complete_before = true;
				m_holding = true;
				m_since_us = now_us;
			}
			if (m_holding && now_us - m_since_us >= m_hold_us)
			{
				m_holding = false; // fired: nothing more until the combination is let go
				return true;
			}
			return false;
		}

		/// Whether a hold is running (and has not fired yet).
		bool Holding() const { return m_holding; }

		/// How far the running hold is, 0 to 1 (0 with none running).
		float Progress(uint64_t now_us) const
		{
			if (!m_holding || m_hold_us == 0)
				return 0.0f;
			return std::clamp(static_cast<float>(now_us - m_since_us) / static_cast<float>(m_hold_us), 0.0f, 1.0f);
		}

		/// Forget a running hold (the game stopped, the controller went away).
		void Cancel()
		{
			m_holding = false;
			m_complete_before = true;
		}

	private:
		uint32_t m_mask = 0;
		uint64_t m_hold_us = 0;
		uint64_t m_since_us = 0;
		bool m_holding = false;
		bool m_complete_before = true;
	};

	/// The choices of [PS5] LeaveGameCombo, as masks of SDL button bits (the numbering of
	/// ps5_input_source.cpp: L3 is bit 7, R3 bit 8, L1 bit 9, R1 bit 10).
	inline uint32_t LeaveComboMask(std::string_view name)
	{
		constexpr uint32_t L3 = 1u << 7, R3 = 1u << 8, L1 = 1u << 9, R1 = 1u << 10;
		if (name == "L3R3")
			return L3 | R3;
		if (name == "L1R1L3R3")
			return L1 | R1 | L3 | R3;
		return 0; // "Off", or anything not known
	}

	/// The hold of [PS5] LeaveGameHoldSeconds, kept between half a second and five.
	inline uint64_t LeaveHoldMicroseconds(float seconds)
	{
		if (!(seconds == seconds)) // not a number
			seconds = 2.0f;
		return static_cast<uint64_t>(std::clamp(seconds, 0.5f, 5.0f) * 1000000.0f);
	}

	// ---------------------------------------------------------------- the confirming double buzz

	/// Two short pulses of both motors: on, off, on. While it plays it takes the place of whatever
	/// the game asks of the motors; afterwards the game's own values are sent again.
	class DoubleBuzz
	{
	public:
		static constexpr uint64_t PULSE_US = 120000;
		static constexpr uint64_t GAP_US = 110000;
		static constexpr uint8_t STRENGTH = 200;

		void Start(uint64_t now_us)
		{
			m_active = true;
			m_start_us = now_us;
		}

		bool Active() const { return m_active; }

		/// The motors' strength at this time (the same for both), and whether the buzz still plays.
		/// Once it returns false it stays off until started again.
		bool Sample(uint64_t now_us, uint8_t* strength)
		{
			*strength = 0;
			if (!m_active)
				return false;
			const uint64_t t = now_us - m_start_us;
			if (t >= 2 * PULSE_US + GAP_US)
			{
				m_active = false;
				return false;
			}
			*strength = (t < PULSE_US || t >= PULSE_US + GAP_US) ? STRENGTH : 0;
			return true;
		}

	private:
		bool m_active = false;
		uint64_t m_start_us = 0;
	};

	// ---------------------------------------------------------------- pressure set by R2

	/// The pressure the DualShock 2's analog face buttons and d-pad report while "Pressure from R2"
	/// is on: with R2 at rest a press is a full one, so play without the trigger is unchanged;
	/// with R2 pulled, every press reports as much pressure as R2 is pulled, down to a floor that
	/// still counts as pressed. The trigger's value is 0 to 255, as the console reports it.
	inline float TriggerPressure(uint8_t trigger, uint8_t rest = 12, float floor = 0.04f)
	{
		if (trigger <= rest)
			return 1.0f;
		return std::clamp(static_cast<float>(trigger) / 255.0f, floor, 1.0f);
	}

	// ---------------------------------------------------------------- aiming with the motion sensor

	struct GyroAimSettings
	{
		/// How far the controller turns to move the pointer from the centre to the side of the
		/// screen, in degrees ([PS5] MotionAimSensitivity: smaller is faster). Up and down use the
		/// same degrees per pixel.
		float degrees_to_edge = 20.0f;
		/// Turning slower than this (degrees a second) is the hand's tremor, not aim.
		float dead_zone_dps = 1.0f;
		/// The time constant of the smoothing, in seconds (0: none).
		float smoothing_s = 0.02f;
		/// The screen's height over its width (9/16 for a 16:9 picture).
		float aspect = 9.0f / 16.0f;
		bool invert_x = false;
		bool invert_y = false;
	};

	/// The pointer, -1 to 1 across and down the screen (0, 0 the centre), moved by the angular
	/// velocity the controller reports: turning left (yaw, the sensor's y, positive counter-
	/// clockwise seen from above) moves it left, tilting up (pitch, the sensor's x) moves it up.
	/// The aim is relative, as a mouse's: Recenter puts the pointer back in the middle, which is
	/// where the controller should then point. At the edges it stops, as a mouse does.
	class GyroAim
	{
	public:
		void SetSettings(const GyroAimSettings& settings) { m_settings = settings; }
		const GyroAimSettings& Settings() const { return m_settings; }

		void Recenter()
		{
			m_raw_x = m_raw_y = 0.0f;
			m_x = m_y = 0.0f;
		}

		/// One sample: angular velocity about the sensor's x (pitch) and y (yaw) in radians a
		/// second, over dt seconds since the previous one. A dt that cannot be (none, negative, or
		/// longer than a tenth of a second: a gap in the samples) moves nothing.
		void Feed(float pitch_rad_s, float yaw_rad_s, float dt)
		{
			if (!(dt > 0.0f) || dt > 0.1f || !std::isfinite(pitch_rad_s) || !std::isfinite(yaw_rad_s))
				return;

			constexpr float RAD_TO_DEG = 57.29577951f;
			const float yaw = DeadZone(yaw_rad_s * RAD_TO_DEG);
			const float pitch = DeadZone(pitch_rad_s * RAD_TO_DEG);
			const float to_edge_x = std::max(m_settings.degrees_to_edge, 1.0f);
			const float to_edge_y = std::max(to_edge_x * m_settings.aspect, 0.5f);

			m_raw_x = std::clamp(m_raw_x + (m_settings.invert_x ? 1.0f : -1.0f) * yaw * dt / to_edge_x, -1.0f, 1.0f);
			m_raw_y = std::clamp(m_raw_y + (m_settings.invert_y ? 1.0f : -1.0f) * pitch * dt / to_edge_y, -1.0f, 1.0f);

			if (m_settings.smoothing_s > 0.0f)
			{
				const float alpha = 1.0f - std::exp(-dt / m_settings.smoothing_s);
				m_x += (m_raw_x - m_x) * alpha;
				m_y += (m_raw_y - m_y) * alpha;
			}
			else
			{
				m_x = m_raw_x;
				m_y = m_raw_y;
			}
		}

		float X() const { return m_x; }
		float Y() const { return m_y; }

	private:
		float DeadZone(float dps) const
		{
			const float dz = std::max(m_settings.dead_zone_dps, 0.0f);
			if (std::fabs(dps) <= dz)
				return 0.0f;
			// Taken off the top, so leaving the dead zone does not jump.
			return (dps > 0.0f) ? (dps - dz) : (dps + dz);
		}

		GyroAimSettings m_settings;
		float m_raw_x = 0.0f, m_raw_y = 0.0f;
		float m_x = 0.0f, m_y = 0.0f;
	};

	/// The time between two pad samples from their timestamps (microseconds, as the console
	/// writes them), or the fallback when the timestamps say nothing usable.
	inline float SampleInterval(uint64_t previous_us, uint64_t current_us, float fallback_s)
	{
		if (previous_us == 0 || current_us <= previous_us)
			return fallback_s;
		const float dt = static_cast<float>(current_us - previous_us) * 1e-6f;
		return (dt > 0.1f) ? fallback_s : dt;
	}
} // namespace PS5InputLogic
