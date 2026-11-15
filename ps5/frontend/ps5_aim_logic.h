// Crux2 - aiming a light gun with a controller's motion sensor: the parts with no console in them
// (ps5/tests/aim checks them on the PC). Used by the DualSense's GunCon 2 aim (ps5_input_source.cpp)
// and by the PlayStation Move's (ps5_move_logic.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The aim is Porpoise's (Project Porpoise, src/porpoise_aim.cpp, Copyright (C) 2026 Ruben,
// GPL-3.0-or-later): the gyroscope and the accelerometer fused into the controller's orientation
// (gravity keeps "up"; the gyroscope's drift is learnt while the controller lies still), from it
// where the controller points (left-right and up-down against gravity, so it reads the same however
// it is twisted in the hand), and from that the pointer against the direction that was "the middle
// of the screen" when it was last centred. Pointing at the same place always gives the same place on
// the screen: nothing adds up as a mouse's movements do. Its functions are below as they are
// (PorpoiseAim), with their notice.
//
// SteadyAim is what porpoise_pad.cpp does with them for one controller (read_motion), here for a
// GunCon 2 rather than a Wii Remote: centring by itself once the fusion has settled, again on a short
// hold of a button while the controller is fairly still, the grip read from how it is held when it is
// centred (two hands, or stood upright in one hand with the index finger on the trigger), the centre
// eased after a pointer held calmly against an edge, Porpoise's light smoothing, its pointer speeds,
// and the screen measured by pointing at two of its corners.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PS5AimLogic
{
	/* From Porpoise 61f38f6 (GPL-3.0-or-later): src/porpoise_aim.cpp and .hpp (the grips, fuse,
	 * remote_angles, scale_for, pointer, ease_edge, level_grip, Smoother), unchanged but for being
	 * inline in this header.
	 * Porpoise - aiming a DualSense like a Wii Remote.
	 * Copyright (C) 2026 Ruben (Project Porpoise)
	 * SPDX-License-Identifier: GPL-3.0-or-later */
	namespace PorpoiseAim
	{
		/* A grip: the Remote's axes in the DualSense's (x right, y out of its face, z toward the
		 * player). Rows: the Remote's left, back and up. */
		struct Basis
		{
			float m[3][3];
		};
		/* Two hands, face up, the trigger edge toward the TV. (For a Move in Move Management's axes:
		 * held with its buttons up and the sphere toward the TV.) */
		inline constexpr Basis kGripNormal{{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}};
		/* Stood on its left grip, held in the right hand: the face toward the left, the right grip
		 * up, the index finger on R2, the thumb on the face buttons. */
		inline constexpr Basis kGripUprightRight{{{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}};
		/* The mirror image, in the left hand: the face toward the right, L2 under the index
		 * finger. */
		inline constexpr Basis kGripUprightLeft{{{0, -1, 0}, {0, 0, 1}, {-1, 0, 0}}};

		struct Fusion
		{
			float q[4] = {0, 0, 0, 1};
			float bias[3] = {0, 0, 0};
			bool started = false;
			float age = 0;
			float still = 0;
		};
		struct Angles
		{
			float yaw = 0, pitch = 0, roll = 0;
		};
		struct Scale
		{
			float half_x = 0, half_y = 0;
		};

		namespace detail
		{
			constexpr float kPi = 3.14159265358979f;
			struct V3
			{
				float x, y, z;
			};
			inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
			inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
			inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
			inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
			inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
			inline V3 rotate(const float q[4], V3 v)
			{
				const V3 u{q[0], q[1], q[2]};
				const float s = q[3];
				return u * (2.0f * dot(u, v)) + v * (s * s - dot(u, u)) + cross(u, v) * (2.0f * s);
			}
			inline V3 rotate_back(const float q[4], V3 v)
			{
				const float c[4] = {-q[0], -q[1], -q[2], q[3]};
				return rotate(c, v);
			}
			inline void normalise(float q[4])
			{
				const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
				if (n <= 0)
				{
					q[0] = q[1] = q[2] = 0;
					q[3] = 1;
					return;
				}
				for (int i = 0; i < 4; ++i)
					q[i] /= n;
			}
			inline float wrap(float a)
			{
				while (a > kPi)
					a -= 2 * kPi;
				while (a < -kPi)
					a += 2 * kPi;
				return a;
			}
			inline V3 row(const Basis& b, int r) { return {b.m[r][0], b.m[r][1], b.m[r][2]}; }
			/* The accelerometer at rest reads a little over 1 g on a PS5. */
			constexpr float kRestG = 1.035f;
		} // namespace detail

		inline void fuse(Fusion& f, const float gyro[3], const float accel[3], float dt)
		{
			using namespace detail;
			const V3 a{accel[0], accel[1], accel[2]};
			const float an = std::sqrt(dot(a, a));
			if (!f.started)
			{
				/* Start level with gravity: the accelerometer reads "up". */
				if (an < 0.5f)
					return;
				const V3 up = a * (1.0f / an);
				const V3 z{0, 0, 1};
				const V3 axis = cross(up, z);
				const float s = std::sqrt(dot(axis, axis)), c = dot(up, z);
				if (s < 1e-6f)
				{
					f.q[0] = c > 0 ? 0.0f : 1.0f;
					f.q[1] = f.q[2] = 0;
					f.q[3] = c > 0 ? 1.0f : 0.0f;
				}
				else
				{
					const float half = std::atan2(s, c) / 2;
					const V3 n = axis * (1.0f / s);
					f.q[0] = n.x * std::sin(half);
					f.q[1] = n.y * std::sin(half);
					f.q[2] = n.z * std::sin(half);
					f.q[3] = std::cos(half);
				}
				f.started = true;
				f.age = f.still = 0;
				return;
			}
			dt = std::clamp(dt, 0.0f, 0.05f);
			f.age += dt;
			const V3 raw{gyro[0], gyro[1], gyro[2]};
			V3 w = raw - V3{f.bias[0], f.bias[1], f.bias[2]};

			/* The gyroscope's drift: what it reads while the controller lies still. */
			const bool quiet = std::fabs(w.x) < 0.04f && std::fabs(w.y) < 0.04f && std::fabs(w.z) < 0.04f &&
							   std::fabs(an - kRestG) < 0.06f;
			f.still = quiet ? f.still + dt : 0.0f;
			if (f.still > 0.4f)
			{
				const float k = std::min(1.0f, dt / 1.5f);
				f.bias[0] += (raw.x - f.bias[0]) * k;
				f.bias[1] += (raw.y - f.bias[1]) * k;
				f.bias[2] += (raw.z - f.bias[2]) * k;
				w = raw - V3{f.bias[0], f.bias[1], f.bias[2]};
			}

			/* Gravity pulls the estimate of "up" back, gently once settled. */
			if (an > 0.8f && an < 1.25f)
			{
				const V3 measured = a * (1.0f / an);
				const V3 estimated = rotate_back(f.q, {0, 0, 1});
				const float kp = f.age < 1.0f ? 5.0f : 0.5f;
				w = w + cross(measured, estimated) * kp;
			}

			/* q += q * (0, w) dt / 2 */
			const float hx = w.x * dt / 2, hy = w.y * dt / 2, hz = w.z * dt / 2;
			const float x = f.q[0], y = f.q[1], z = f.q[2], s = f.q[3];
			f.q[0] = x + s * hx + y * hz - z * hy;
			f.q[1] = y + s * hy + z * hx - x * hz;
			f.q[2] = z + s * hz + x * hy - y * hx;
			f.q[3] = s - x * hx - y * hy - z * hz;
			normalise(f.q);
		}

		inline Angles remote_angles(const Fusion& f, const Basis& b)
		{
			using namespace detail;
			/* The world: x "left", y "back", z up (yaw's zero is arbitrary). */
			const V3 left = rotate(f.q, row(b, 0));
			const V3 back = rotate(f.q, row(b, 1));
			const V3 up = rotate(f.q, row(b, 2));
			const V3 fwd = back * -1.0f;
			Angles out;
			out.yaw = std::atan2(-fwd.x, -fwd.y);
			out.pitch = std::asin(std::clamp(fwd.z, -1.0f, 1.0f));
			/* Roll: the Remote's top against the vertical through where it points. */
			const V3 level_left{std::cos(out.yaw), -std::sin(out.yaw), 0};
			out.roll = -std::atan2(dot(level_left, up), dot(level_left, left));
			return out;
		}

		inline Scale scale_for(int speed, float measured_x = 0, float measured_y = 0)
		{
			constexpr float kTall = 0.65f; /* up-down turning to the edge, against left-right's */
			if (measured_x > 0.03f && measured_y > 0.03f)
				return {measured_x, measured_y};
			/* Degrees from the middle to the edge; 5 is about a real Remote's. */
			static constexpr float kDegrees[10] = {30, 26, 22, 18.5f, 15.5f, 13, 11, 9.5f, 8, 7};
			const float h = kDegrees[std::clamp(speed, 1, 10) - 1] * detail::kPi / 180;
			return {h, h * kTall};
		}

		inline void pointer(const Angles& now, const Angles& centre, const Scale& scale, float& x, float& y)
		{
			x = detail::wrap(now.yaw - centre.yaw) / scale.half_x;
			y = -(now.pitch - centre.pitch) / scale.half_y;
		}

		/* A pointer held calmly just past an edge (not swung far off the screen) pulls the centre
		 * after it, slowly, so a pointer that has drifted comes back in line when it is pushed
		 * against the edge. dt: seconds since the last call. */
		inline void ease_edge(const Angles& now, Angles& centre, const Scale& scale, float dt)
		{
			using namespace detail;
			constexpr float kEdge = 1.02f, kFar = 1.6f, kRate = 0.4f; /* screen halves a second */
			float x, y;
			pointer(now, centre, scale, x, y);
			const float step = kRate * std::clamp(dt, 0.0f, 0.05f);
			if (std::fabs(x) > kEdge && std::fabs(x) < kFar)
			{
				const float move = std::min(step, std::fabs(x) - kEdge);
				centre.yaw = wrap(centre.yaw + std::copysign(move, x) * scale.half_x);
			}
			if (std::fabs(y) > kEdge && std::fabs(y) < kFar)
			{
				const float move = std::min(step, std::fabs(y) - kEdge);
				centre.pitch -= std::copysign(move, y) * scale.half_y;
			}
		}

		inline bool level_grip(const Basis& grip, const float up[3], Basis& out)
		{
			using namespace detail;
			out = grip;
			V3 u{up[0], up[1], up[2]};
			const float n = std::sqrt(dot(u, u));
			if (n < 0.5f)
				return false;
			u = u * (1.0f / n);
			const V3 fwd = row(grip, 1) * -1.0f;
			const float along = dot(fwd, u);
			if (std::fabs(along) > 0.82f) /* more than ~55 degrees up or down */
				return false;
			V3 f = fwd - u * along;
			f = f * (1.0f / std::sqrt(dot(f, f)));
			const V3 back = f * -1.0f, left = cross(back, u);
			const V3 rows[3] = {left, back, u};
			for (int r = 0; r < 3; ++r)
			{
				out.m[r][0] = rows[r].x;
				out.m[r][1] = rows[r].y;
				out.m[r][2] = rows[r].z;
			}
			return true;
		}

		/* Smoothing for the pointer (0 off .. 3 strong): steadies a still hand, stays quick when it
		 * moves. */
		struct Smoother
		{
			float sx = 0, sy = 0, dx = 0, dy = 0;
			bool started = false;
			void reset() { started = false; }
			void step(float& x, float& y, float dt, int strength)
			{
				/* A "one euro" filter: strong when the pointer is nearly still (hand tremor), light
				 * when it moves (no lag on a sweep). */
				static constexpr float kMin[4] = {0, 3.0f, 1.6f, 0.9f}, kBeta[4] = {0, 0.35f, 0.18f, 0.09f};
				strength = std::clamp(strength, 0, 3);
				if (strength == 0 || dt <= 0)
				{
					sx = x;
					sy = y;
					started = true;
					return;
				}
				if (!started)
				{
					sx = x;
					sy = y;
					dx = dy = 0;
					started = true;
					return;
				}
				auto alpha = [&](float cutoff) {
					const float tau = 1.0f / (2 * detail::kPi * cutoff);
					return 1.0f / (1.0f + tau / dt);
				};
				const float ad = alpha(1.0f);
				dx += ((x - sx) / dt - dx) * ad;
				dy += ((y - sy) / dt - dy) * ad;
				const float speed = std::sqrt(dx * dx + dy * dy);
				const float a = alpha(kMin[strength] + kBeta[strength] * speed);
				/* A jump (a re-centring, back from off the screen) is taken at once. */
				if (std::fabs(x - sx) > 0.8f || std::fabs(y - sy) > 0.8f)
					sx = x, sy = y;
				else
				{
					sx += (x - sx) * a;
					sy += (y - sy) * a;
				}
				x = sx;
				y = sy;
			}
		};
	} // namespace PorpoiseAim

	// ---------------------------------------------------------------- the DualSense's steady aim

	enum class Grip : int
	{
		Auto = 0, // read from how it is held when it is centred
		TwoHands = 1, // face up, held in both hands
		Upright = 2, // stood on a grip in one hand, the index finger on R2 (or L2 in the left hand)
	};

	struct SteadySettings
	{
		/// Porpoise's pointer speeds, 1 (slowest: 30 degrees from the middle to the side) to 10 (7
		/// degrees); 0: the screen measured by pointing at its corners (measured_x/y), or 5 when
		/// none was measured.
		int speed = 5;
		float measured_x = 0, measured_y = 0; // radians from the middle to the side and to the top
		int smoothing = 1; // 0 off .. 3 strong (Porpoise's Light by default)
		Grip grip = Grip::Auto;
		bool invert_x = false, invert_y = false;
	};

	/// How far a measured screen may reach (Porpoise's setup accepts these spans).
	inline bool SaneScreen(float half_x, float half_y)
	{
		constexpr float DEG = 57.2957795131f;
		return std::isfinite(half_x) && std::isfinite(half_y) && half_x * DEG > 3 && half_x * DEG < 75 && half_y * DEG > 2 &&
			   half_y * DEG < 60 && half_y / half_x > 0.25f && half_y / half_x < 1.3f;
	}

	/// The grip gravity shows, for this setting: the upright grips when the controller's right or left
	/// side is up, two hands otherwise. -1 when it is not clear (moving, or tilted in between).
	inline int SeenGrip(const float up[3], Grip setting)
	{
		if (setting == Grip::TwoHands)
			return 0;
		const float n = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (n < 0.6f || n > 1.4f)
			return -1;
		const float ux = up[0] / n, uy = up[1] / n;
		if (ux > 0.75f)
			return 1; // its right side up: in the right hand
		if (ux < -0.75f)
			return 2;
		if (uy > 0.75f && setting == Grip::Auto)
			return 0;
		return -1;
	}

	inline const PorpoiseAim::Basis& GripBasis(int grip)
	{
		return grip == 1 ? PorpoiseAim::kGripUprightRight : grip == 2 ? PorpoiseAim::kGripUprightLeft : PorpoiseAim::kGripNormal;
	}

	/// One controller's steady aim. Each reading goes to Feed, in order; once a poll, Tick says where it
	/// points.
	class SteadyAim
	{
	public:
		/// How long the centring button is held, with the controller fairly still, to centre; and how
		/// long in all to start measuring the screen.
		static constexpr float CENTRE_HOLD_S = 0.3f;
		static constexpr float MEASURE_HOLD_S = 2.0f;

		enum Event : uint32_t
		{
			AUTO_CENTRED = 1, // by itself, once the fusion settled
			CENTRED = 2, // by the button
			REGRIPPED = 4, // held another way for a few seconds: centred again for it
			MEASURE_TOP_LEFT = 8, // measuring: now point at the screen's top-left corner and press
			MEASURE_BOTTOM_RIGHT = 16, // and now at its bottom-right corner
			MEASURED = 32, // the screen was measured: MeasuredX/Y
			MEASURE_FAILED = 64, // what was pointed at did not look like a screen
			HOLD_STILL = 128, // a corner was pressed while the controller moved: not taken
		};

		void SetSettings(const SteadySettings& s) { m_settings = s; }
		const SteadySettings& Settings() const { return m_settings; }

		/// Everything forgotten: a controller that went away, an aim switched off.
		void Reset() { m = State(); }

		/// One reading: angular velocity (rad/s) and acceleration (g, gravity included) in the
		/// DualSense's axes, and when it was taken (microseconds; 0 when not known: then dt_s).
		void Feed(const float gyro[3], const float accel[3], uint64_t sample_us, float dt_s = 0.004f)
		{
			if (!std::isfinite(gyro[0]) || !std::isfinite(gyro[1]) || !std::isfinite(gyro[2]) || !std::isfinite(accel[0]) ||
				!std::isfinite(accel[1]) || !std::isfinite(accel[2]))
				return;
			float dt;
			if (sample_us)
			{
				if (m.last_us && sample_us <= m.last_us)
					return; // already seen
				dt = m.last_us ? float(sample_us - m.last_us) * 1e-6f : 0.0f;
				m.last_us = sample_us;
			}
			else
			{
				dt = m.fusion.started ? dt_s : 0.0f;
			}
			PorpoiseAim::fuse(m.fusion, gyro, accel, dt);
			const float step = std::min(dt, 0.05f);
			m.elapsed += step;
			// "Up", eased over a sixth of a second, for levelling the grip at a centring.
			const bool first = m.up[0] == 0 && m.up[1] == 0 && m.up[2] == 0;
			const float k = first ? 1.0f : std::min(1.0f, step / 0.16f);
			float g2 = 0;
			for (int a = 0; a < 3; a++)
			{
				m.up[a] += (accel[a] - m.up[a]) * k;
				const float g = gyro[a] - m.fusion.bias[a];
				g2 += g * g;
			}
			m.turning = std::sqrt(g2);
		}

		/// Once a poll: button_held is the centring button (held a moment while fairly still: centre;
		/// held on: measure the screen; while measuring, a press takes a corner). Returns the Event
		/// bits of what happened.
		uint32_t Tick(bool button_held)
		{
			uint32_t events = 0;
			const float elapsed = m.elapsed;
			m.elapsed = 0;
			if (!m.fusion.started)
			{
				m.button_was = button_held;
				return 0;
			}
			const bool pressed = button_held && !m.button_was;
			m.button_was = button_held;

			// The grip: read at a centring; after that another hold must be seen steadily for three
			// seconds (so a swing never flips it).
			const int seen = SeenGrip(m.up, m_settings.grip);
			bool regripped = false;
			if (m.grip < 0)
				m.grip = seen >= 0 ? seen : (m_settings.grip == Grip::Upright ? 1 : 0);
			else if (m.centred && seen >= 0 && seen != m.grip && m.turning < 1.0f && m.measure == 0)
			{
				if (seen != m.grip_seen)
					m.grip_wait = 0;
				m.grip_seen = seen;
				m.grip_wait += elapsed;
				if (m.grip_wait > 3.0f)
				{
					m.grip = seen;
					m.grip_wait = 0;
					regripped = true;
				}
			}
			else
			{
				m.grip_wait = 0;
			}

			bool centre_now = false;
			if (m.measure != 0)
			{
				// Measuring: the button is let go after the long hold, then each press takes a corner.
				if (pressed && m.measure_armed)
				{
					if (!m.centred || m.turning > 1.0f)
					{
						events |= HOLD_STILL;
					}
					else
					{
						const PorpoiseAim::Angles now = PorpoiseAim::remote_angles(m.fusion, m.basis);
						const float rel_yaw = PorpoiseAim::detail::wrap(now.yaw - m.centre.yaw);
						const float rel_pitch = now.pitch - m.centre.pitch;
						if (m.measure == 1)
						{
							m.corner[0] = rel_yaw;
							m.corner[1] = rel_pitch;
							m.measure = 2;
							events |= MEASURE_BOTTOM_RIGHT;
						}
						else
						{
							const float half_x = (rel_yaw - m.corner[0]) / 2, half_y = (m.corner[1] - rel_pitch) / 2;
							m.measure = 0;
							if (SaneScreen(half_x, half_y))
							{
								// The middle is half way between the corners.
								m.centre.yaw = PorpoiseAim::detail::wrap(m.centre.yaw + (rel_yaw + m.corner[0]) / 2);
								m.centre.pitch += (rel_pitch + m.corner[1]) / 2;
								m.measured_x = half_x;
								m.measured_y = half_y;
								m_settings.speed = 0;
								m_settings.measured_x = half_x;
								m_settings.measured_y = half_y;
								m.smoother.reset();
								events |= MEASURED;
							}
							else
							{
								events |= MEASURE_FAILED;
							}
						}
					}
				}
				if (!button_held)
					m.measure_armed = true;
				m.hold = 0;
				m.hold_total = 0;
				m.hold_done = button_held; // a press that took a corner does not also centre
			}
			else if (!button_held)
			{
				m.hold = 0;
				m.hold_total = 0;
				m.hold_done = false;
			}
			else
			{
				m.hold_total += elapsed;
				if (!m.hold_done)
				{
					// Held a moment with the controller fairly still: a squeeze mid-swing never centres.
					m.hold = m.turning < 1.2f ? m.hold + elapsed : 0.0f;
					if (m.hold >= CENTRE_HOLD_S)
					{
						m.hold_done = true;
						centre_now = true;
					}
				}
				if (m.hold_done && m.hold_total >= MEASURE_HOLD_S)
				{
					m.measure = 1;
					m.measure_armed = false;
					events |= MEASURE_TOP_LEFT;
				}
			}

			// Centring by itself half a second in, once gravity has settled and the controller is
			// fairly still (or after three seconds regardless), as if it was pointed at the middle.
			const bool settled = m.fusion.age > 0.5f && (m.turning < 0.8f || m.fusion.age > 3.0f);
			if ((!m.centred && settled) || centre_now || regripped)
			{
				if (centre_now && seen >= 0)
					m.grip = seen; // a centring reads the hold again
				PorpoiseAim::level_grip(GripBasis(m.grip), m.up, m.basis);
				m.centre = PorpoiseAim::remote_angles(m.fusion, m.basis);
				m.smoother.reset();
				events |= centre_now ? CENTRED : regripped ? REGRIPPED : AUTO_CENTRED;
				m.centred = true;
				m.grip_wait = 0;
			}
			if (!m.centred)
				m.basis = GripBasis(m.grip);

			if (m.centred)
			{
				const PorpoiseAim::Scale scale = CurrentScale();
				const PorpoiseAim::Angles now = PorpoiseAim::remote_angles(m.fusion, m.basis);
				// Held calmly against an edge, a drifted pointer eases back (not while measuring).
				if (m.turning < 0.5f && m.measure == 0)
					PorpoiseAim::ease_edge(now, m.centre, scale, elapsed);
				float x, y;
				PorpoiseAim::pointer(now, m.centre, scale, x, y);
				m.smoother.step(x, y, elapsed, m_settings.smoothing);
				m.x = std::clamp(m_settings.invert_x ? -x : x, -1.5f, 1.5f);
				m.y = std::clamp(m_settings.invert_y ? -y : y, -1.5f, 1.5f);
			}
			return events;
		}

		/// Centred: X and Y are where it points (-1..1 to the screen's edges, x right, y down; past 1
		/// off the screen).
		bool Centred() const { return m.centred; }
		float X() const { return m.x; }
		float Y() const { return m.y; }
		/// 0 not measuring, 1 waiting for the top-left corner, 2 for the bottom-right.
		int Measuring() const { return m.measure; }
		void CancelMeasure() { m.measure = 0; }
		/// Measuring asked for from elsewhere (the settings): as the long hold does. False while the
		/// aim is not centred yet.
		bool StartMeasure()
		{
			if (!m.centred)
				return false;
			m.measure = 1;
			m.measure_armed = !m.button_was;
			return true;
		}
		float MeasuredX() const { return m.measured_x; }
		float MeasuredY() const { return m.measured_y; }
		int GripInUse() const { return m.grip; }
		float Turning() const { return m.turning; }

		PorpoiseAim::Scale CurrentScale() const
		{
			const bool measured = m_settings.speed == 0 && m_settings.measured_x > 0 && m_settings.measured_y > 0;
			return measured ? PorpoiseAim::scale_for(5, m_settings.measured_x, m_settings.measured_y) :
							  PorpoiseAim::scale_for(m_settings.speed == 0 ? 5 : m_settings.speed);
		}

	private:
		struct State
		{
			PorpoiseAim::Fusion fusion;
			PorpoiseAim::Basis basis = PorpoiseAim::kGripNormal;
			PorpoiseAim::Angles centre;
			PorpoiseAim::Smoother smoother;
			float up[3] = {0, 0, 0};
			float turning = 0;
			float elapsed = 0; // seconds of readings since the last Tick
			uint64_t last_us = 0;
			bool centred = false;
			int grip = -1, grip_seen = -1;
			float grip_wait = 0;
			bool button_was = false;
			float hold = 0, hold_total = 0;
			bool hold_done = false;
			int measure = 0;
			bool measure_armed = false;
			float corner[2] = {0, 0};
			float measured_x = 0, measured_y = 0;
			float x = 0, y = 0;
		};
		SteadySettings m_settings;
		State m;
	};
} // namespace PS5AimLogic
