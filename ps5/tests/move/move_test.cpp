// Crux2 - the PlayStation Move light gun's PC test (ps5/frontend/ps5_move_logic.h and the socket of
// ps5_move_source.h): packets refused as Move Management's docs/PROTOCOL.md says, the stream's
// generations, the mapping, Porpoise's aim with and without a saved screen, and a stand-in producer
// sending over loopback UDP from another thread, as the payload does.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   c++ -std=c++20 -I ps5/frontend ps5/tests/move/move_test.cpp -pthread -o move_test && ./move_test

#include "ps5_move_logic.h"
#include "ps5_move_source.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace PS5MoveLogic;

namespace
{
	int g_failures = 0;
#define CHECK(cond)                                                              \
	do                                                                           \
	{                                                                            \
		if (!(cond))                                                             \
		{                                                                        \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
			g_failures++;                                                        \
		}                                                                        \
	} while (0)

	uint64_t Monotonic()
	{
		timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
	}

	// One snapshot as Move Management's move_bridge.c / nav_bridge.c write it.
	struct Snap
	{
		uint16_t version = 2;
		uint64_t sequence = 0;
		uint64_t sent_us = 0;
		bool valid = true;
		uint32_t buttons = 0;
		uint8_t lx = 128, ly = 128, l2 = 0;
		float accel[3] = {0, 1.0f, 0}; // level, buttons up: gravity on pad Y
		float gyro[3] = {0, 0, 0};
		bool optical = false;
		float ox = 160, oy = 120, ow = 10, oh = 10;
		uint64_t optical_us = 0;
	};

	void Put16(uint8_t* p, uint16_t v)
	{
		p[0] = uint8_t(v);
		p[1] = uint8_t(v >> 8);
	}
	void Put32(uint8_t* p, uint32_t v)
	{
		for (int i = 0; i < 4; i++)
			p[i] = uint8_t(v >> (8 * i));
	}
	void Put64(uint8_t* p, uint64_t v)
	{
		for (int i = 0; i < 8; i++)
			p[i] = uint8_t(v >> (8 * i));
	}
	void PutF(uint8_t* p, float f)
	{
		uint32_t bits;
		std::memcpy(&bits, &f, 4);
		Put32(p, bits);
	}

	std::vector<uint8_t> Wire(const Snap& s)
	{
		std::vector<uint8_t> b(s.version == 1 ? SIZE_V1 : SIZE_V2, 0);
		Put32(b.data(), MAGIC);
		Put16(b.data() + 4, s.version);
		Put16(b.data() + 6, uint16_t(b.size()));
		Put64(b.data() + 8, s.sequence);
		Put64(b.data() + 16, s.sent_us);
		Put32(b.data() + 24, s.valid ? 1 : 0);
		if (s.valid)
		{
			uint8_t* p = b.data() + 32;
			Put32(p, s.buttons);
			p[4] = s.lx;
			p[5] = s.ly;
			p[6] = p[7] = 128;
			p[8] = s.l2;
			PutF(p + 24, 1.0f);
			for (int i = 0; i < 3; i++)
			{
				PutF(p + 28 + 4 * i, s.accel[i]);
				PutF(p + 40 + 4 * i, s.gyro[i]);
			}
			Put32(p + 76, 1);
			Put64(p + 80, s.sent_us);
		}
		if (s.version == 2 && s.optical)
		{
			PutF(b.data() + 152, s.ox);
			PutF(b.data() + 156, s.oy);
			PutF(b.data() + 160, s.ow);
			PutF(b.data() + 164, s.oh);
			Put64(b.data() + 168, s.optical_us ? s.optical_us : s.sent_us);
			Put32(b.data() + 176, 1);
		}
		return b;
	}

	bool Decodes(const std::vector<uint8_t>& b, uint64_t now)
	{
		Packet p;
		return Decode(b.data(), b.size(), now, p);
	}

	// ------------------------------------------------------------ the decoder

	void TestDecode()
	{
		const uint64_t now = 10'000'000;
		Snap s;
		s.sent_us = now - 1000;
		s.buttons = WIRE_MOVE | WIRE_L2;
		s.l2 = 200;
		s.gyro[1] = 0.5f;
		{
			Packet p;
			const auto b = Wire(s);
			CHECK(Decode(b.data(), b.size(), now, p));
			CHECK(p.valid && p.connected && p.buttons == (WIRE_MOVE | WIRE_L2) && p.l2 == 200);
			CHECK(p.accel[1] == 1.0f && p.gyro[1] == 0.5f && p.sample_us == s.sent_us);
		}
		Snap v1 = s;
		v1.version = 1;
		CHECK(Decodes(Wire(v1), now)); // the old 152-byte packet

		auto broken = [&](auto&& change) {
			auto b = Wire(s);
			change(b);
			return !Decodes(b, now);
		};
		CHECK(broken([](auto& b) { b[0] ^= 1; })); // magic
		CHECK(broken([](auto& b) { Put16(b.data() + 4, 3); })); // version
		CHECK(broken([](auto& b) { Put16(b.data() + 6, 183); })); // stated size
		CHECK(broken([](auto& b) { b.resize(183); })); // truncated
		CHECK(broken([](auto& b) { b.resize(100); }));
		CHECK(broken([](auto& b) { b.resize(20); }));
		CHECK(broken([](auto& b) { b.push_back(0); })); // too long
		CHECK(broken([](auto& b) { Put32(b.data() + 28, 1); })); // reserved
		CHECK(broken([](auto& b) { Put32(b.data() + 180, 1); })); // reserved (v2)
		CHECK(broken([](auto& b) { Put32(b.data() + 24, 2); })); // valid flag
		CHECK(broken([](auto& b) { Put32(b.data() + 32 + 76, 2); })); // connected flag
		CHECK(broken([](auto& b) { Put32(b.data() + 176, 2); })); // optical flag
		CHECK(broken([](auto& b) { PutF(b.data() + 32 + 44, std::nanf("")); })); // NaN gyro
		CHECK(broken([](auto& b) { PutF(b.data() + 32 + 28, INFINITY); })); // infinite acceleration
		CHECK(broken([](auto& b) { PutF(b.data() + 32 + 12, std::nanf("")); })); // NaN quaternion
		CHECK(broken([&](auto& b) { Put64(b.data() + 16, now + FUTURE_SLACK_US + 1000); })); // future
		CHECK(broken([&](auto& b) { Put64(b.data() + 16, now - INPUT_STALE_US - 1); })); // stale
		CHECK(broken([&](auto& b) { Put64(b.data() + 32 + 80, s.sent_us + 1); })); // reading after sending
		{
			auto b = Wire(s);
			Put64(b.data() + 16, now + 2000); // a little ahead: the title's clock may lag the kernel's
			Put64(b.data() + 32 + 80, now + 2000);
			CHECK(Decodes(b, now));
		}
		Snap o = s;
		o.optical = true;
		CHECK(Decodes(Wire(o), now));
		Snap bad = o;
		bad.ox = 320; // out of the picture
		CHECK(!Decodes(Wire(bad), now));
		bad = o;
		bad.ow = 0;
		CHECK(!Decodes(Wire(bad), now));
		bad = o;
		bad.oy = std::nanf("");
		CHECK(!Decodes(Wire(bad), now));
		bad = o;
		bad.optical_us = o.sent_us + 1; // seen after it was sent
		CHECK(!Decodes(Wire(bad), now));
		Snap invalid = s;
		invalid.valid = false; // the producer's invalidation: accepted, not usable
		{
			Packet p;
			const auto b = Wire(invalid);
			CHECK(Decode(b.data(), b.size(), now, p) && !p.valid && !p.connected);
		}
	}

	// ------------------------------------------------------------ streams and generations

	void TestStream()
	{
		Stream st;
		uint64_t now = 20'000'000;
		Snap s;
		s.sent_us = now;
		s.sequence = 5;
		auto offer = [&](const Snap& x, uint64_t at) {
			const auto b = Wire(x);
			return st.Take(b.data(), b.size(), at);
		};
		CHECK(offer(s, now) == Stream::Offer::NewGeneration);
		CHECK(st.Generation() == 1 && st.Live(now));
		CHECK(offer(s, now + 1000) == Stream::Offer::Duplicate); // the same sequence again
		Snap older = s;
		older.sequence = 4;
		CHECK(offer(older, now + 1000) == Stream::Offer::Duplicate); // out of order
		s.sequence = 6;
		s.sent_us = now + 4000;
		CHECK(offer(s, now + 4000) == Stream::Offer::Accepted);
		CHECK(st.Live(now + 4000 + INPUT_STALE_US));
		CHECK(!st.Live(now + 4000 + INPUT_STALE_US + 1)); // timed out
		// The producer restarted: its sequence from 0, after the timeout: a new generation.
		now += 2'000'000;
		Snap restart = s;
		restart.sequence = 0;
		restart.sent_us = now;
		CHECK(offer(restart, now) == Stream::Offer::NewGeneration && st.Generation() == 2);
		// Invalidated, then back: a new generation.
		Snap inv = restart;
		inv.sequence = 1;
		inv.valid = false;
		CHECK(offer(inv, now + 100) == Stream::Offer::Accepted && !st.Live(now + 100) && st.Active(now + 100));
		Snap back = restart;
		back.sequence = 2;
		back.sent_us = now + 200;
		CHECK(offer(back, now + 200) == Stream::Offer::NewGeneration && st.Generation() == 3);
		// A restart within the half second (sequence reset) is refused until the stream times out.
		Snap early = back;
		early.sequence = 0;
		early.sent_us = now + 300;
		CHECK(offer(early, now + 300) == Stream::Offer::Duplicate);
		// Optics: fresh for 200 ms only, and a sphere lost is not a lost stream.
		Snap op = back;
		op.sequence = 3;
		op.sent_us = now + 400;
		op.optical = true;
		CHECK(offer(op, now + 400) == Stream::Offer::Accepted && st.OpticalFresh(now + 400));
		CHECK(!st.OpticalFresh(now + 400 + OPTICAL_STALE_US + 1) && st.Live(now + 400 + OPTICAL_STALE_US + 1));
	}

	// ------------------------------------------------------------ the profile

	void TestProfile()
	{
		const ScreenProfile p = ParseScreenProfile("MVA1 0.2 0.12 0.05 -0.03\n");
		CHECK(p.valid && std::fabs(p.half_x - 0.2f) < 1e-6f && std::fabs(p.pitch + 0.03f) < 1e-6f);
		CHECK(!ParseScreenProfile("MVA1 0.2 0.12 0.05\n").valid); // a field short
		CHECK(!ParseScreenProfile("MVA1 0.2 0.12 0.05 0.01 x\n").valid); // more after it
		CHECK(!ParseScreenProfile("MVA2 0.2 0.12 0.05 0.01\n").valid);
		CHECK(!ParseScreenProfile("MVA1 0.01 0.12 0.05 0.01\n").valid); // span too small
		CHECK(!ParseScreenProfile("MVA1 0.2 0.3 0.05 0.01\n").valid); // taller than wide past 1.3
		CHECK(!ParseScreenProfile("MVA1 0.2 0.12 1.4 0.01\n").valid); // offset too far
		CHECK(!ParseScreenProfile("MVA1 nan 0.12 0 0\n").valid);
		CHECK(!ParseScreenProfile(nullptr).valid);
	}

	// ------------------------------------------------------------ the gun

	struct Feeder
	{
		Feeder(LightGun& g, uint64_t start) : gun(g), now(start) {}
		LightGun& gun;
		uint64_t now;
		uint64_t seq = 1;
		// The Move's orientation (its axes into the world's, gravity along the world's y at the
		// start), so the accelerometer reads what the gyroscope turned it to.
		float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		LightGun::Output Step(const Snap& change, uint64_t dt_us = 4000, bool nav = false)
		{
			now += dt_us;
			Snap s = change;
			s.sequence = seq++;
			s.sent_us = now;
			if (!nav)
			{
				// r = r * (1 + [w dt]x), then made orthonormal again; the accelerometer is r's
				// transpose times "up".
				const float dt = float(dt_us) * 1e-6f;
				const float w[3] = {s.gyro[0] * dt, s.gyro[1] * dt, s.gyro[2] * dt};
				const float k[3][3] = {{1, -w[2], w[1]}, {w[2], 1, -w[0]}, {-w[1], w[0], 1}};
				float n[3][3] = {};
				for (int i = 0; i < 3; i++)
					for (int j = 0; j < 3; j++)
						for (int m = 0; m < 3; m++)
							n[i][j] += r[i][m] * k[m][j];
				for (int j = 0; j < 3; j++) // Gram-Schmidt on the columns
				{
					for (int c = 0; c < j; c++)
					{
						float d = 0;
						for (int i = 0; i < 3; i++)
							d += n[i][j] * n[i][c];
						for (int i = 0; i < 3; i++)
							n[i][j] -= d * n[i][c];
					}
					float len = 0;
					for (int i = 0; i < 3; i++)
						len += n[i][j] * n[i][j];
					len = std::sqrt(len);
					for (int i = 0; i < 3; i++)
						n[i][j] /= len;
				}
				std::memcpy(r, n, sizeof(r));
				for (int i = 0; i < 3; i++)
					s.accel[i] = r[1][i]; // r^T (0, 1, 0)
			}
			const auto b = Wire(s);
			gun.Offer(nav, b.data(), b.size(), now);
			return gun.Tick(now);
		}
		// n readings of still or turning
		LightGun::Output Run(int n, const Snap& s, bool nav = false)
		{
			LightGun::Output o;
			for (int i = 0; i < n; i++)
				o = Step(s, 4000, nav);
			return o;
		}
	};

	void TestMapping()
	{
		CHECK(MapMove(WIRE_MOVE) == Sdl(SDL_SOUTH));
		CHECK(MapMove(WIRE_CROSS) == Sdl(SDL_EAST));
		CHECK(MapMove(WIRE_CIRCLE) == Sdl(SDL_NORTH));
		CHECK(MapMove(WIRE_SQUARE) == Sdl(SDL_WEST));
		CHECK(MapMove(WIRE_TRIANGLE) == Sdl(SDL_RIGHT_SHOULDER));
		CHECK(MapMove(WIRE_START) == Sdl(SDL_TOUCHPAD));
		CHECK(MapMove(WIRE_SELECT | WIRE_PS | WIRE_L2) == 0); // Select centres; PS; T is the axis
		CHECK(MapNav(WIRE_UP | WIRE_CROSS) == (Sdl(SDL_DPAD_UP) | Sdl(SDL_EAST)));
		CHECK(MapNav(STICK_LEFT | WIRE_L1) == (Sdl(SDL_DPAD_LEFT) | Sdl(SDL_BACK)));
		CHECK(TriggerDown(false, 79) == false && TriggerDown(false, 80) && TriggerDown(true, 48) && !TriggerDown(true, 47));
		CHECK(StickDirection(0, 255) == 1 && StickDirection(0, 0) == -1 && StickDirection(0, 128 + 63) == 0);
		CHECK(StickDirection(1, 128 + 40) == 1 && StickDirection(1, 128 + 39) == 0);

		LightGun gun;
		Feeder f(gun, 30'000'000);
		Snap s;
		LightGun::Output o = f.Run(5, s);
		CHECK(gun.TakeEvents() & LightGun::MOVE_FOUND);
		CHECK(o.sdl_buttons == 0 && o.trigger == 0);
		s.l2 = 255;
		s.buttons = WIRE_L2;
		o = f.Step(s);
		CHECK(o.trigger == 255);
		s.l2 = 60; // inside the hysteresis: still down
		o = f.Step(s, 60000);
		CHECK(o.trigger == 255);
		s.l2 = 0;
		s.buttons = 0;
		o = f.Step(s, 60000);
		CHECK(o.trigger == 0);
		s.buttons = WIRE_MOVE | WIRE_SELECT;
		o = f.Step(s);
		CHECK(o.sdl_buttons == Sdl(SDL_SOUTH)); // Select is not passed on
		// A tap pressed and released between two polls is still given, for 50 ms.
		{
			s.buttons = 0;
			f.Step(s, 60000);
			Snap tap = s;
			tap.buttons = WIRE_CROSS;
			f.now += 2000;
			tap.sequence = f.seq++;
			tap.sent_us = f.now;
			auto b = Wire(tap);
			gun.Offer(false, b.data(), b.size(), f.now);
			Snap up = s;
			f.now += 2000;
			up.sequence = f.seq++;
			up.sent_us = f.now;
			b = Wire(up);
			gun.Offer(false, b.data(), b.size(), f.now);
			CHECK(gun.Tick(f.now).sdl_buttons == Sdl(SDL_EAST));
			CHECK(gun.Tick(f.now + 40000).sdl_buttons == Sdl(SDL_EAST));
			CHECK(gun.Tick(f.now + 50000).sdl_buttons == 0);
		}
		// Invalidation lets go at once; silence after half a second.
		s.buttons = WIRE_CIRCLE;
		s.l2 = 255;
		o = f.Step(s, 60000);
		CHECK(o.sdl_buttons == Sdl(SDL_NORTH) && o.trigger == 255 && o.aiming);
		Snap inv = s;
		inv.valid = false;
		o = f.Step(inv);
		CHECK(o.sdl_buttons == 0 && o.trigger == 0 && !o.aiming);
		CHECK(gun.TakeEvents() & LightGun::MOVE_LOST);
		o = f.Run(3, s);
		CHECK(o.sdl_buttons == Sdl(SDL_NORTH) && !o.aiming); // a new generation: the aim starts again
		CHECK(gun.TakeEvents() & LightGun::MOVE_FOUND);
		CHECK(gun.Tick(f.now + INPUT_STALE_US).trigger == 255);
		o = gun.Tick(f.now + INPUT_STALE_US + 1);
		CHECK(o.sdl_buttons == 0 && o.trigger == 0 && !o.aiming);
		CHECK(gun.TakeEvents() & LightGun::MOVE_LOST);

		// The Nav: stick and d-pad to the gun's d-pad, alongside the Move's buttons.
		LightGun two;
		Feeder m(two, 40'000'000);
		Feeder n(two, 40'000'000);
		Snap nav;
		nav.accel[1] = 0;
		nav.lx = 255; // right
		nav.buttons = WIRE_CROSS;
		Snap move;
		move.buttons = WIRE_MOVE;
		m.Step(move);
		n.now = m.now;
		o = n.Step(nav, 0, true);
		CHECK(o.sdl_buttons == (Sdl(SDL_SOUTH) | Sdl(SDL_DPAD_RIGHT) | Sdl(SDL_EAST)));
		CHECK(two.TakeEvents() & LightGun::NAV_FOUND);
		nav.lx = 128;
		nav.ly = 0; // up
		nav.buttons = 0;
		o = n.Step(nav, 60000, true);
		CHECK((o.sdl_buttons & ~Sdl(SDL_SOUTH)) == Sdl(SDL_DPAD_UP));
		o = two.Tick(n.now + INPUT_STALE_US + 1);
		CHECK(o.sdl_buttons == 0);
	}

	void TestAim()
	{
		LightGun gun;
		Feeder f(gun, 50'000'000);
		Snap still;
		LightGun::Output o = f.Run(100, still); // 0.4 s: not centred yet
		CHECK(!o.aiming);
		o = f.Run(50, still); // settled: centred by itself
		CHECK(o.aiming && std::fabs(o.x) < 0.01f && std::fabs(o.y) < 0.01f);
		CHECK(gun.TakeEvents() & LightGun::AUTO_CENTRED);

		// Turning about pad Y (gravity's axis: yaw), +0.5 rad/s for 0.2 s = 0.1 rad to the left.
		const float half_x = PorpoiseAim::scale_for(5).half_x;
		Snap left = still;
		left.gyro[1] = 0.5f;
		o = f.Run(50, left);
		o = f.Run(100, still); // the smoothing settles
		CHECK(o.x < 0 && std::fabs(o.x + 0.1f / half_x) < 0.05f && std::fabs(o.y) < 0.03f);
		// Pitching about pad X: +0.5 rad/s for 0.1 s = 0.05 rad up.
		Snap up = still;
		up.gyro[0] = 0.5f;
		f.Run(25, up);
		o = f.Run(100, still);
		const float half_y = PorpoiseAim::scale_for(5).half_y;
		CHECK(o.y < 0 && std::fabs(o.y + 0.05f / half_y) < 0.1f);
		// Select: centred where it points now.
		Snap select = still;
		select.buttons = WIRE_SELECT;
		f.Step(select);
		o = f.Run(100, still);
		CHECK(std::fabs(o.x) < 0.01f && std::fabs(o.y) < 0.01f);
		CHECK(gun.TakeEvents() & LightGun::CENTRED);
		// A twist about the pointing direction (pad Z, the Move's length) hardly moves the pointer.
		Snap twist = still;
		twist.gyro[2] = 1.0f;
		o = f.Run(50, twist);
		CHECK(std::fabs(o.x) < 0.1f && std::fabs(o.y) < 0.1f);
		twist.gyro[2] = -1.0f;
		f.Run(50, twist);
		f.Step(select);
		f.Step(still);

		// With the saved screen: the measured reach, and the middle offset from the centring.
		LightGun gun2;
		gun2.SetProfile(ParseScreenProfile("MVA1 0.2 0.12 0.05 0.03\n"));
		Feeder g(gun2, 60'000'000);
		o = g.Run(150, still);
		CHECK(o.aiming);
		CHECK(std::fabs(o.x - (-0.05f / 0.2f)) < 0.02f); // the middle lies 0.05 rad to the right
		CHECK(std::fabs(o.y - (0.03f / 0.12f)) < 0.02f); // and 0.03 rad up
		Snap right = still;
		right.gyro[1] = -0.5f; // 0.05 rad right: onto the middle
		g.Run(25, right);
		o = g.Run(100, still);
		CHECK(std::fabs(o.x) < 0.05f);
		// Far off the screen: clamped, still a position off it.
		right.gyro[1] = -3.0f;
		o = g.Run(50, right);
		CHECK(o.x == 1.5f);
	}

	// ------------------------------------------------------------ over loopback, from another thread

	void TestLoopback()
	{
		PS5Move::Socket move, nav;
		uint16_t move_port = MOVE_PORT_BASE, nav_port = NAV_PORT_BASE;
		if (!move.Open(move_port) || !nav.Open(nav_port))
		{
			// The documented ports taken on this PC (another run of this test): any free two, the
			// same code.
			CHECK(move.Open(0) && nav.Open(0));
			move_port = move.Port();
			nav_port = nav.Port();
		}
		std::printf("loopback: Move on %u, Nav on %u\n", move_port, nav_port);
		CHECK(move.Port() == move_port);

		const int out = ::socket(AF_INET, SOCK_DGRAM, 0);
		CHECK(out >= 0);
		sockaddr_in to = {};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		auto send = [&](uint16_t port, const std::vector<uint8_t>& b) {
			to.sin_port = htons(port);
			return ::sendto(out, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == ssize_t(b.size());
		};

		// The producer: 250 readings a second, as move_bridge.c sends them, with garbage mixed in;
		// then quiet; then started again with its sequence from 0; then invalidated.
		std::atomic<int> phase{0};
		std::atomic<bool> stop{false};
		std::thread producer([&] {
			uint64_t seq = 100;
			auto run = [&](int ms, uint32_t buttons, uint8_t t, float yaw, bool garbage) {
				const uint64_t end = Monotonic() + uint64_t(ms) * 1000;
				int i = 0;
				while (Monotonic() < end && !stop)
				{
					Snap s;
					s.sequence = seq++;
					s.sent_us = Monotonic();
					s.buttons = buttons;
					s.l2 = t;
					s.gyro[1] = yaw;
					s.optical = (i % 2) == 0;
					send(move_port, Wire(s));
					if (garbage && (i % 5) == 0)
					{
						auto b = Wire(s);
						b.resize(100); // truncated
						send(move_port, b);
						auto n = Wire(s);
						PutF(n.data() + 32 + 40, std::nanf("")); // NaN
						Put64(n.data() + 8, seq + 1000);
						send(move_port, n);
						send(move_port, Wire(s)); // a duplicate
					}
					if ((i % 4) == 0)
					{
						Snap nv;
						nv.sequence = seq;
						nv.sent_us = s.sent_us;
						nv.accel[1] = 0;
						nv.lx = 0; // left
						send(nav_port, Wire(nv));
					}
					i++;
					std::this_thread::sleep_for(std::chrono::microseconds(4000));
				}
			};
			phase = 1;
			run(900, 0, 0, 0, true); // found, still: centres by itself
			phase = 2;
			run(400, WIRE_MOVE, 255, 0.5f, true); // A and the trigger, turning left
			phase = 3;
			std::this_thread::sleep_for(std::chrono::milliseconds(700)); // quiet: lost
			phase = 4;
			seq = 0; // restarted
			run(300, WIRE_CROSS, 0, 0, false);
			phase = 5;
			Snap inv;
			inv.sequence = seq++;
			inv.sent_us = Monotonic();
			inv.valid = false;
			send(move_port, Wire(inv));
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			phase = 6;
		});

		LightGun gun;
		uint32_t events = 0;
		bool aimed_still = false, fired = false, turned_left = false, lost_released = false, restarted = false,
			 invalidated_released = false, nav_left = false, optics = false;
		unsigned refused = 0, duplicates = 0, most = 0;
		while (phase.load() != 6)
		{
			auto take = [&](bool is_nav) {
				return [&, is_nav](const uint8_t* b, size_t n) {
					const auto r = gun.Offer(is_nav, b, n, Monotonic());
					refused += r == Stream::Offer::Refused;
					duplicates += r == Stream::Offer::Duplicate;
				};
			};
			most = std::max(most, move.Drain(take(false)));
			nav.Drain(take(true));
			const LightGun::Output o = gun.Tick(Monotonic());
			const uint32_t e = gun.TakeEvents();
			events |= e;
			optics = optics || gun.MoveStream().OpticalFresh(Monotonic());
			const int ph = phase.load();
			if (ph == 1 && o.aiming && std::fabs(o.x) < 0.05f)
				aimed_still = true;
			if (ph == 1 && (o.sdl_buttons & Sdl(SDL_DPAD_LEFT)))
				nav_left = true;
			if (ph == 2 && o.trigger == 255 && (o.sdl_buttons & Sdl(SDL_SOUTH)))
				fired = true;
			if (ph == 2 && o.aiming && o.x < -0.3f)
				turned_left = true;
			if (ph == 3 && (e & LightGun::MOVE_LOST) && o.sdl_buttons == 0 && o.trigger == 0 && !o.aiming)
				lost_released = true;
			if (ph == 4 && (o.sdl_buttons & Sdl(SDL_EAST)) && gun.MoveStream().Generation() >= 2)
				restarted = true;
			if (ph >= 5 && restarted && (o.sdl_buttons & MapMove(~0u)) == 0 && o.trigger == 0 && !o.aiming && (e & LightGun::MOVE_LOST))
				invalidated_released = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(16)); // a frame's poll
		}
		producer.join();
		std::printf("loopback: refused %u, duplicates %u, at most %u datagrams in a poll, events 0x%x\n", refused,
			duplicates, most, events);
		CHECK(aimed_still);
		CHECK(nav_left);
		CHECK(fired);
		CHECK(turned_left);
		CHECK(lost_released);
		CHECK(restarted);
		CHECK(invalidated_released);
		CHECK(optics);
		CHECK(refused > 0 && duplicates > 0);
		CHECK(events & LightGun::NAV_FOUND);

		// A flood: more than a poll's budget waits for the next poll, none is lost to the budget.
		{
			LightGun flood;
			const uint64_t base = Monotonic();
			for (int i = 0; i < 200; i++)
			{
				Snap s;
				s.sequence = 1000 + i;
				s.sent_us = base;
				s.buttons = (i == 199) ? uint32_t(WIRE_START) : 0u;
				send(move_port, Wire(s));
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			unsigned first = 0, total = 0, polls = 0;
			auto count = [&](const uint8_t* b, size_t n) { flood.Offer(false, b, n, Monotonic()); };
			for (unsigned got; (got = move.Drain(count)) > 0; polls++)
			{
				if (polls == 0)
					first = got;
				total += got;
			}
			CHECK(first == PS5Move::Socket::DRAIN_BUDGET);
			CHECK(total == 200);
			CHECK(flood.Tick(Monotonic()).sdl_buttons == Sdl(SDL_TOUCHPAD));
		}
		::close(out);
	}
} // namespace

int main()
{
	TestDecode();
	TestStream();
	TestProfile();
	TestMapping();
	TestAim();
	TestLoopback();
	if (g_failures)
	{
		std::printf("move: %d check(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("move: all checks passed\n");
	return 0;
}
