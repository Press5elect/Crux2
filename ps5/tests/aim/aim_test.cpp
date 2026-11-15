// Crux2 - the PC tests of the steady motion aim for the GunCon 2 (frontend/ps5_aim_logic.h), the
// sticks' response curve and R2's click for the light gun (frontend/ps5_input_source_logic.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A made-up controller is turned in the test: its true orientation is kept here and its
// gyroscope and accelerometer read from it, 250 times a second as a DualSense reports, so the aim
// sees what the controller would say. What it must do: point at the same place when the controller
// points the same way again, however it turned in between; learn the gyroscope's drift while it
// lies still; centre on a short calm hold of the button and not on a tap or in a swing; measure the
// screen from two corners; read an upright grip.

#include "ps5_aim_logic.h"
#include "ps5_input_source_logic.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace PS5AimLogic;
namespace D = PS5AimLogic::PorpoiseAim::detail;

static int s_checks = 0, s_failures = 0;
#define CHECK(x) \
	do \
	{ \
		s_checks++; \
		if (!(x)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
		} \
	} while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
constexpr float DEG = 3.14159265358979f / 180.0f;

/// A controller: its orientation (DualSense -> world, z up), turned at body rates.
struct Controller
{
	float q[4];
	float bias[3] = {0, 0, 0}; // the gyroscope's drift, added to what it reads
	uint64_t t_us = 1000000;

	/// Flat on the table, face up (its y up).
	Controller() : q{std::sin(45 * DEG), 0, 0, std::cos(45 * DEG)} {}
	/// Stood upright in the right hand: its x up.
	static Controller Upright()
	{
		Controller c;
		// DS x -> world z: a quarter turn about y, the other way.
		c.q[0] = 0;
		c.q[1] = -std::sin(45 * DEG);
		c.q[2] = 0;
		c.q[3] = std::cos(45 * DEG);
		return c;
	}

	void Accel(float out[3]) const
	{
		const D::V3 g = D::rotate_back(q, {0, 0, 1.035f});
		out[0] = g.x;
		out[1] = g.y;
		out[2] = g.z;
	}

	/// Turns at body rates w (rad/s) for seconds, every reading to the aim; Tick each 16 ms with
	/// the button as given.
	void Run(SteadyAim& aim, float wx, float wy, float wz, float seconds, bool button, uint32_t* events = nullptr)
	{
		const int n = static_cast<int>(seconds * 250.0f + 0.5f);
		for (int i = 0; i < n; i++)
		{
			const float dt = 0.004f;
			const float hx = wx * dt / 2, hy = wy * dt / 2, hz = wz * dt / 2;
			const float x = q[0], y = q[1], z = q[2], s = q[3];
			q[0] = x + s * hx + y * hz - z * hy;
			q[1] = y + s * hy + z * hx - x * hz;
			q[2] = z + s * hz + x * hy - y * hx;
			q[3] = s - x * hx - y * hy - z * hz;
			D::normalise(q);
			t_us += 4000;
			const float gyro[3] = {wx + bias[0], wy + bias[1], wz + bias[2]};
			float accel[3];
			Accel(accel);
			aim.Feed(gyro, accel, t_us);
			if (i % 4 == 3)
			{
				const uint32_t e = aim.Tick(button);
				if (events)
					*events |= e;
			}
		}
	}
};

static SteadyAim MakeAim(int smoothing = 0)
{
	SteadyAim aim;
	SteadySettings s;
	s.smoothing = smoothing;
	aim.SetSettings(s);
	return aim;
}

static void SamePlace()
{
	SteadyAim aim = MakeAim();
	Controller c;
	uint32_t events = 0;
	c.Run(aim, 0, 0, 0, 1.0f, false, &events);
	CHECK((events & SteadyAim::AUTO_CENTRED) != 0);
	CHECK(aim.Centred());
	CHECK(Near(aim.X(), 0, 0.02f) && Near(aim.Y(), 0, 0.02f));

	// Turning left by the default reach (15.5 degrees) is the left edge.
	c.Run(aim, 0, 15.5f * DEG / 0.5f, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.1f, false);
	CHECK(Near(aim.X(), -1.0f, 0.05f));
	CHECK(Near(aim.Y(), 0, 0.05f));
	// Up by half the reach up and down.
	const float half_y = PorpoiseAim::scale_for(5).half_y;
	c.Run(aim, half_y / 2 / 0.5f, 0, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.1f, false);
	CHECK(Near(aim.Y(), -0.5f, 0.06f));

	// A wild swing, and back the way it came by another path: the same place again.
	c.Run(aim, 0, 0, 3.0f, 0.3f, false);
	c.Run(aim, 0, -2.0f, 0, 0.4f, false);
	c.Run(aim, 0, 2.0f, 0, 0.4f, false);
	c.Run(aim, 0, 0, -3.0f, 0.3f, false);
	c.Run(aim, -half_y / 2 / 0.5f, 0, 0, 0.5f, false);
	c.Run(aim, 0, -15.5f * DEG / 0.5f, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.2f, false);
	CHECK(Near(aim.X(), 0, 0.05f) && Near(aim.Y(), 0, 0.05f));
	std::printf("same place: back at %.3f, %.3f\n", aim.X(), aim.Y());
}

static void Drift()
{
	SteadyAim aim = MakeAim();
	Controller c;
	c.bias[1] = 0.015f; // under a degree a second about the vertical, which no gravity corrects
	c.Run(aim, 0, 0, 0, 12.0f, false);
	// Learnt while it lay still: the pointer stays near where it was centred.
	CHECK(Near(aim.X(), 0, 0.12f));
	const float x0 = aim.X();
	c.Run(aim, 0, 0, 0, 10.0f, false);
	CHECK(Near(aim.X(), x0, 0.03f)); // and once learnt, it no longer moves
	std::printf("drift: %.3f after 12 s, %.3f after 22 s (an uncorrected drift: %.1f)\n", x0, aim.X(),
		0.015f * 22.0f / PorpoiseAim::scale_for(5).half_x);
}

static void Centring()
{
	SteadyAim aim = MakeAim();
	Controller c;
	c.Run(aim, 0, 0, 0, 1.0f, false);
	c.Run(aim, 0, -8 * DEG / 0.4f, 0, 0.4f, false);
	c.Run(aim, 0, 0, 0, 0.1f, false);
	const float x = aim.X();
	CHECK(x > 0.3f);

	// A tap does not centre.
	uint32_t events = 0;
	c.Run(aim, 0, 0, 0, 0.1f, true, &events);
	c.Run(aim, 0, 0, 0, 0.2f, false, &events);
	CHECK(!(events & SteadyAim::CENTRED) && Near(aim.X(), x, 0.03f));

	// Held in a swing it does not either.
	events = 0;
	c.Run(aim, 0, 2.5f, 0, 0.25f, true, &events);
	c.Run(aim, 0, -2.5f, 0, 0.25f, true, &events);
	c.Run(aim, 0, 0, 0, 0.05f, false, &events);
	CHECK(!(events & SteadyAim::CENTRED));

	// Held calmly for a moment: centred where it points.
	events = 0;
	c.Run(aim, 0, 0, 0, 0.4f, true, &events);
	c.Run(aim, 0, 0, 0, 0.1f, false, &events);
	CHECK((events & SteadyAim::CENTRED) != 0);
	CHECK(Near(aim.X(), 0, 0.03f) && Near(aim.Y(), 0, 0.03f));
	CHECK(aim.Measuring() == 0);
}

static void EdgeEase()
{
	SteadyAim aim = MakeAim();
	Controller c;
	c.Run(aim, 0, 0, 0, 1.0f, false);
	// Held calmly a little past the right edge, the centre follows: the edge comes to the hand.
	c.Run(aim, 0, -1.2f * PorpoiseAim::scale_for(5).half_x / 0.3f, 0, 0.3f, false);
	c.Run(aim, 0, 0, 0, 0.05f, false);
	const float before = aim.X();
	c.Run(aim, 0, 0, 0, 2.0f, false);
	CHECK(before > 1.1f && aim.X() < before - 0.1f && aim.X() >= 1.0f);
	// Swung far off the screen, nothing moves.
	c.Run(aim, 0, -1.5f, 0, 0.4f, false);
	c.Run(aim, 0, 0, 0, 0.05f, false);
	const float far = aim.X();
	c.Run(aim, 0, 0, 0, 1.0f, false);
	CHECK(far >= 1.5f && Near(aim.X(), far, 0.001f));
}

static void Measure()
{
	SteadyAim aim = MakeAim();
	Controller c;
	c.Run(aim, 0, 0, 0, 1.0f, false);
	uint32_t events = 0;
	c.Run(aim, 0, 0, 0, 2.2f, true, &events); // the long hold
	CHECK((events & SteadyAim::CENTRED) && (events & SteadyAim::MEASURE_TOP_LEFT));
	CHECK(aim.Measuring() == 1);
	events = 0;
	c.Run(aim, 0, 0, 0, 0.1f, false, &events); // let go: no corner yet
	CHECK(aim.Measuring() == 1 && events == 0);

	// The screen: 20 degrees to the sides, 12 up and down, its middle 3 degrees right of and 1 below
	// where the hold centred. Top-left first: left 17, up 13.
	c.Run(aim, 0, 17 * DEG / 0.5f, 0, 0.5f, false);
	c.Run(aim, 13 * DEG / 0.5f, 0, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.2f, false);
	events = 0;
	c.Run(aim, 0, 0, 0, 0.05f, true, &events);
	c.Run(aim, 0, 0, 0, 0.05f, false, &events);
	CHECK((events & SteadyAim::MEASURE_BOTTOM_RIGHT) && aim.Measuring() == 2);
	// Then bottom-right: right 40 and down 24 from there (down first, then across, as a hand might).
	c.Run(aim, -24 * DEG / 0.5f, 0, 0, 0.5f, false);
	// Across at the bottom: about the world's vertical, which is the controller's own up tilted back.
	const float tilt = -11 * DEG; // where it now points: 11 degrees down
	c.Run(aim, 0, -40 * DEG * std::cos(tilt) / 0.8f, 40 * DEG * std::sin(tilt) / 0.8f, 0.8f, false);
	c.Run(aim, 0, 0, 0, 0.2f, false);
	events = 0;
	c.Run(aim, 0, 0, 0, 0.05f, true, &events);
	c.Run(aim, 0, 0, 0, 0.05f, false, &events);
	CHECK((events & SteadyAim::MEASURED) != 0);
	std::printf("measured: %.2f by %.2f degrees, the bottom-right corner at %.3f, %.3f\n", aim.MeasuredX() / DEG,
		aim.MeasuredY() / DEG, aim.X(), aim.Y());
	CHECK(Near(aim.MeasuredX() / DEG, 20, 1.0f) && Near(aim.MeasuredY() / DEG, 12, 1.0f));
	CHECK(aim.Settings().speed == 0);
	CHECK(Near(aim.X(), 1, 0.06f) && Near(aim.Y(), 1, 0.06f));
	CHECK(SaneScreen(aim.MeasuredX(), aim.MeasuredY()));

	// A screen that cannot be: refused, the aim as it was.
	SteadyAim bad = MakeAim();
	Controller d;
	d.Run(bad, 0, 0, 0, 1.0f, false);
	CHECK(bad.StartMeasure());
	events = 0;
	d.Run(bad, 0, 0, 0, 0.05f, true, &events);
	d.Run(bad, 0, 0, 0, 0.05f, false, &events);
	d.Run(bad, 0, 0, 0, 0.05f, true, &events); // the same place twice
	d.Run(bad, 0, 0, 0, 0.05f, false, &events);
	CHECK((events & SteadyAim::MEASURE_FAILED) && bad.Measuring() == 0 && bad.Settings().speed == 5);

	// A corner pressed in a swing is not taken.
	SteadyAim moving = MakeAim();
	Controller e;
	e.Run(moving, 0, 0, 0, 1.0f, false);
	CHECK(moving.StartMeasure());
	events = 0;
	e.Run(moving, 0, 2.0f, 0, 0.1f, true, &events);
	CHECK((events & SteadyAim::HOLD_STILL) && moving.Measuring() == 1);
}

static void Upright()
{
	SteadyAim aim = MakeAim();
	Controller c = Controller::Upright();
	c.Run(aim, 0, 0, 0, 1.0f, false);
	CHECK(aim.Centred() && aim.GripInUse() == 1);
	CHECK(Near(aim.X(), 0, 0.02f) && Near(aim.Y(), 0, 0.02f));
	// Turning left about the vertical (now the controller's own x).
	c.Run(aim, 15.5f * DEG / 0.5f, 0, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.1f, false);
	CHECK(Near(std::fabs(aim.X()), 1.0f, 0.05f) && Near(aim.Y(), 0, 0.05f));
	const float x = aim.X();
	c.Run(aim, -15.5f * DEG / 0.5f, 0, 0, 0.5f, false);
	c.Run(aim, 0, 0, 0, 0.1f, false);
	CHECK(Near(aim.X(), 0, 0.03f));
	std::printf("upright: a turn left reads %.3f\n", x);

	// "In both hands" never takes the upright grip.
	SteadyAim two;
	SteadySettings s;
	s.grip = Grip::TwoHands;
	s.smoothing = 0;
	two.SetSettings(s);
	Controller d = Controller::Upright();
	d.Run(two, 0, 0, 0, 1.0f, false);
	CHECK(two.GripInUse() == 0);
}

static void Smoothing()
{
	SteadyAim aim = MakeAim(1);
	Controller c;
	c.Run(aim, 0, 0, 0, 1.0f, false);
	// A tremor of a fifth of a degree at 9 Hz: smoothed to less than the raw movement.
	float worst = 0;
	for (int i = 0; i < 40; i++)
	{
		const float w = 0.2f * DEG * 2 * 3.14159f * 9 * std::cos(i * 2 * 3.14159f * 9 * 0.016f);
		c.Run(aim, 0, w, 0, 0.016f, false);
		worst = std::max(worst, std::fabs(aim.X()));
	}
	CHECK(worst < 0.2f * DEG / PorpoiseAim::scale_for(5).half_x);
	// A quick sweep still gets there.
	c.Run(aim, 0, -10 * DEG / 0.2f, 0, 0.2f, false);
	c.Run(aim, 0, 0, 0, 0.3f, false);
	CHECK(Near(aim.X(), 10.0f / 15.5f, 0.05f));
}

static void Speeds()
{
	SteadySettings s;
	SteadyAim aim;
	s.speed = 1;
	aim.SetSettings(s);
	CHECK(Near(aim.CurrentScale().half_x / DEG, 30, 0.01f));
	s.speed = 10;
	aim.SetSettings(s);
	CHECK(Near(aim.CurrentScale().half_x / DEG, 7, 0.01f));
	s.speed = 0; // measured, but none: the default
	aim.SetSettings(s);
	CHECK(Near(aim.CurrentScale().half_x / DEG, 15.5f, 0.01f));
	s.measured_x = 21 * DEG;
	s.measured_y = 11 * DEG;
	aim.SetSettings(s);
	CHECK(Near(aim.CurrentScale().half_x / DEG, 21, 0.01f) && Near(aim.CurrentScale().half_y / DEG, 11, 0.01f));
}

static void Sticks()
{
	using namespace PS5InputLogic;
	float x = 0.37f, y = -0.81f;
	CurveStick(x, y, 0);
	CHECK(x == 0.37f && y == -0.81f); // Linear: exactly as it was
	x = 0.5f, y = 0;
	CurveStick(x, y, 1);
	CHECK(Near(x, std::pow(0.5f, 1.4f), 1e-5f) && y == 0);
	x = 1, y = 0;
	CurveStick(x, y, 3);
	CHECK(Near(x, 1, 1e-6f));
	x = 0.3f, y = 0.4f; // the direction is kept
	CurveStick(x, y, 2);
	CHECK(Near(y / x, 0.4f / 0.3f, 1e-4f) && Near(std::sqrt(x * x + y * y), std::pow(0.5f, 1.8f), 1e-4f));
	x = -1, y = -1; // a corner: the length held to 1
	CurveStick(x, y, 2);
	CHECK(Near(x, -1, 1e-6f) && Near(y, -1, 1e-6f));
	x = 0, y = 0;
	CurveStick(x, y, 2);
	CHECK(x == 0 && y == 0);
	CHECK(StickCurveFromName("Linear") == 0 && StickCurveFromName("Gentle") == 1 && StickCurveFromName("Gentlest") == 3 &&
		  StickCurveFromName("nonsense") == 0);
}

static void Trigger()
{
	using namespace PS5InputLogic;
	static_assert(offsetof(TriggerEffectParam, l2) == 8 && offsetof(TriggerEffectParam, r2) == 64);
	const TriggerEffectParam off = GunTriggerEffect(0);
	CHECK(off.trigger_mask == 3 && off.l2.mode == 0 && off.r2.mode == 0 && off.r2.data[2] == 0);
	const TriggerEffectParam firm = GunTriggerEffect(2);
	CHECK(firm.r2.mode == 2 && firm.r2.data[0] == 6 && firm.r2.data[1] == 8 && firm.r2.data[2] == 6 && firm.l2.mode == 0);
	CHECK(GunTriggerEffect(1).r2.data[2] == 3);
	CHECK(TriggerFeelFromName("Off") == 0 && TriggerFeelFromName("Light") == 1 && TriggerFeelFromName("Firm") == 2 &&
		  TriggerFeelFromName("") == 2);
	// The click: down only past where the resistance gives way, up again only well back.
	CHECK(!GunTriggerDown(false, 128) && !GunTriggerDown(false, 199) && GunTriggerDown(false, 200));
	CHECK(GunTriggerDown(true, 150) && !GunTriggerDown(true, 139));
}

int main()
{
	SamePlace();
	Drift();
	Centring();
	EdgeEase();
	Measure();
	Upright();
	Smoothing();
	Speeds();
	Sticks();
	Trigger();
	std::printf("aim: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
