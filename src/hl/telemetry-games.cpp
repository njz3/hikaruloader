/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Hikaruloader is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Hikaruloader is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.
 */

/* The racing games' telemetry decoders (hl/telemetry.h): where each game
 * keeps its vehicles and its race in its RAM, found by recording the RAM
 * while the game was driven, each value checked against the game's own
 * display. Addresses are the master SH-4's (its RAM at 0C000000; NAOMI's
 * "ram", the others' "ram_m"). */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hl/telemetry.h"
#include "hl/buffer.h"
#include "hl/games.h"
#include "hl/machine.h"

namespace hl {
namespace telemetry {

namespace {

/* A machine's memory by its name ("ram_m"), seen at base */
class Ram {
public:
	Ram (Machine *mach, const char *name, uint32_t base) : base (base)
	{
		for (const auto &m : mach->memories)
			if (!strcmp (m.first, name))
				buf = m.second;
	}

	bool ok () const { return buf != NULL; }

	bool has (uint32_t addr, unsigned size) const
	{
		return buf && addr >= base && addr - base + size <= buf->size ();
	}
	uint32_t u32 (uint32_t addr) const { return has (addr, 4) ? (uint32_t) buf->get (4, addr - base) : 0; }
	uint16_t u16 (uint32_t addr) const { return has (addr, 2) ? (uint16_t) buf->get (2, addr - base) : 0; }
	uint8_t u8 (uint32_t addr) const { return has (addr, 1) ? (uint8_t) buf->get (1, addr - base) : 0; }
	int32_t i32 (uint32_t addr) const { return (int32_t) u32 (addr); }
	int16_t i16 (uint32_t addr) const { return (int16_t) u16 (addr); }
	float f32 (uint32_t addr) const
	{
		uint32_t u = u32 (addr);
		float f;
		memcpy (&f, &u, 4);
		return f;
	}

private:
	Buffer *buf = NULL;
	uint32_t base;
};

/* The game's family: its parent's name, or its own */
const char *
family (const Machine *mach)
{
	const Game *g = mach->game;
	return !g ? "" : g->parent[0] ? g->parent : g->name;
}

/* A world mirrored (its x the other way, the left the up x forward): x and
 * the vectors' x turned round, the right worked out again */
void
mirror_x (float v[3])
{
	v[0] = -v[0];
}

void
cross (const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* The orientation from a mirrored world's up and forward rows */
void
orient_mirrored (Vehicle &v, const float up_row[3], const float fwd_row[3])
{
	float up[3] = { up_row[0], up_row[1], up_row[2] };
	float fwd[3] = { fwd_row[0], fwd_row[1], fwd_row[2] };
	float right[3];

	mirror_x (up);
	mirror_x (fwd);
	cross (up, fwd, right);
	set_orientation (v, right, up, fwd);
}

/* The orientation from angles: the heading (+ turning right), the pitch (+
 * nose up), the roll (+ leaning right) */
void
orient_angles (Vehicle &v, float heading, float pitch, float roll)
{
	float fwd[3] = { std::sin (heading) * std::cos (pitch), std::sin (pitch), std::cos (heading) * std::cos (pitch) };
	float right0[3] = { std::cos (heading), 0, -std::sin (heading) }, up0[3];
	cross (fwd, right0, up0);
	float right[3], up[3];
	for (int i = 0; i < 3; i++) {
		right[i] = right0[i] * std::cos (roll) - up0[i] * std::sin (roll);
		up[i] = up0[i] * std::cos (roll) + right0[i] * std::sin (roll);
	}
	set_orientation (v, right, up, fwd);
}

/* An angle within -pi..pi */
float
wrap_angle (float a)
{
	while (a > 3.14159265f)
		a -= 6.2831853f;
	while (a < -3.14159265f)
		a += 6.2831853f;
	return a;
}

/****************************************************************************
 NASCAR Arcade (sgnascar, sgnascaro)

 Checked in races on Talladega (beginner), AUTOMATIC, the defaults. The
 world is mirrored (its matrices' rows left = up x forward; steering right
 lowers its heading): x is turned round.
 - The player's body: 0C42DED4, x y z (m) at +0, its velocity at +C (m a
   frame).
 - Each car's matrix: 0C42F5B4 + k x 2E8, rows left, up, forward (3 x 4
   floats, the translation last), car 0 the player's.
 - The cars: 0C7C2D00 + k x 280, 30 of them, car 0 the player: x y z at +A0,
   km/h at +40.
 - Their laps: 0C42E0A8 + k x 14, +4 the segment within the lap (152 a lap
   on Talladega), +C the laps done. The places: 0C42E320 + i x 14, +8 the
   car in place i + 1; the player's own 0C42C578 (from 0).
 - The player's car: m/s 0C7C112C; gear 0C7C0F50 (1..4); rpm shown
   0C7C0F64; throttle, brake, steer (+1 left) 0C7C10BC, C0, C4; draft
   0C7C1AC4 (0..150).
 - The race clocks, in 1/6000 s: time left 0C42F448, race time 0C42F444,
   the laps' starts after the time left (0C42F448 + 4 x laps done), last
   lap 0C42DCC8 (+ 4 x the lap), best lap 0C42DCB8 (0FFFFFFF none).
 - The state: mode 0C80C184 (3 attract, C select, D the game), and in the
   game the race's phase 0C42E058 (B..D the rolling start, F racing, 10..13
   time up, result, game over).
****************************************************************************/

class Nascar : public Decoder {
public:
	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram_m", 0x0C000000);
		static const unsigned CARS = 30;
		const float T = 6000.0f;

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t mode = ram.u32 (0x0C80C184), phase = ram.u32 (0x0C42E058);
		if (mode == 3)
			out.state = State::ATTRACT;
		else if (mode == 0x0C)
			out.state = State::SELECT;
		else if (mode == 0x0D)
			out.state = (phase >= 0x0B && phase <= 0x0F) ? State::RACE :
			            (phase >= 0x10 && phase <= 0x13) ? State::OVER : State::SELECT;
		else
			out.state = State::UNKNOWN;
		out.finished = out.state == State::OVER;
		out.laps = 4;
		out.track = "";

		/* The places of all the cars */
		int place[CARS] = {};
		for (unsigned i = 0; i < CARS; i++) {
			uint32_t k = ram.u32 (0x0C42E320 + i * 0x14 + 8);
			if (k < CARS && !place[k])
				place[k] = (int) i + 1;
		}

		for (unsigned k = 0; k < CARS; k++) {
			Vehicle v;
			uint32_t car = 0x0C7C2D00 + k * 0x280, m = 0x0C42F5B4 + k * 0x2E8, lap = 0x0C42E0A8 + k * 0x14;
			float up[3] = { ram.f32 (m + 0x10), ram.f32 (m + 0x14), ram.f32 (m + 0x18) };
			float fwd[3] = { ram.f32 (m + 0x20), ram.f32 (m + 0x24), ram.f32 (m + 0x28) };

			v.x = -ram.f32 (car + 0xA0);
			v.y = ram.f32 (car + 0xA4);
			v.z = ram.f32 (car + 0xA8);
			orient_mirrored (v, up, fwd);
			v.speed = ram.f32 (car + 0x40) / 3.6f;
			uint32_t laps = ram.u32 (lap + 0xC), segment = ram.u32 (lap + 4);
			v.lap = (int) laps + 1;
			v.progress = (float) laps + (float) segment / 152.0f;
			v.position = place[k];
			if (k == 0) {
				const uint32_t P = 0x0C42DED4;
				v.x = -ram.f32 (P);
				v.y = ram.f32 (P + 4);
				v.z = ram.f32 (P + 8);
				v.vx = -ram.f32 (P + 0xC) * (float) FPS;
				v.vy = ram.f32 (P + 0x10) * (float) FPS;
				v.vz = ram.f32 (P + 0x14) * (float) FPS;
				v.has_velocity = true;
				v.speed = ram.f32 (0x0C7C112C);
				uint32_t gear = ram.u32 (0x0C7C0F50);
				v.gear = (gear >= 1 && gear <= 4) ? (int) gear : 0;
				v.gears = 4;
				v.rpm = ram.f32 (0x0C7C0F64);
				v.rpm_idle = 800;
				v.rpm_max = 9500;
				v.throttle = ram.f32 (0x0C7C10BC);
				v.brake = ram.f32 (0x0C7C10C0);
				v.steer = ram.f32 (0x0C7C10C4);
				v.position = (int) ram.u32 (0x0C42C578) + 1;
				v.extra.push_back ({ "draft", (float) ram.u32 (0x0C7C1AC4) / 150.0f });
				out.player = v;
			} else
				out.others.push_back (v);
		}

		uint32_t done = ram.u32 (0x0C42E0A8 + 0xC);
		uint32_t race = ram.u32 (0x0C42F444);
		out.time_left = ram.u32 (0x0C42F448) / T;
		out.race_time = race / T;
		out.lap_time = (race - (done ? ram.u32 (0x0C42F448 + 4 * done) : 0)) / T;
		if (done)
			out.last_lap = ram.u32 (0x0C42DCC8 + 4 * (done - 1)) / T;
		uint32_t best = ram.u32 (0x0C42DCB8);
		if (best && best != 0x0FFFFFFF)
			out.best_lap = best / T;
		return true;
	}
};

/****************************************************************************
 Star Wars: Racer Arcade (swracer)

 Checked in races on Tatooine, the defaults. The world is mirrored (its
 matrices' rows right, up, back, with up x forward the left; the pod turning
 right lowers its heading): x is turned round. A unit is 0.669 m, the HUD's
 km/h being |v| x 216 / 1.4943 (v in units a frame).
 - The scene 0C016E2C: 12C6C0 the race (its countdown too), 1735C8 the
   result (RETIRED, TIME OUT), CB5D0 the pod's choice, C8600 the course's,
   1E06B8 the course's introduction, A962C the attract's demo, 0 the logo
   or GAME OVER.
 - The player's pod, at [0C016E08] in the race (elsewhere between two
   races): x y z +108; rows right, up, back +D8, +E8, +F8; velocity +330;
   the engines' own rows at +118 (left) and +158 (right), the same for the
   two, turned from the cockpit's about the pod's length only (some 8
   degrees at full steering, 15 while it changes), x y z +148 and +188;
   its steering as used +410 ((right thrust - left thrust) / 2); the
   engines' thrust as used, right +6C and left +C8, of +270 (their
   maximum); +D0's bit 29 set while a hit's jolt lasts (a wall, a pod; its
   frames counted in bits 23-7), bit 31 while boosting; probes from +448 a
   2C each, a distance and a normal (the ground's up, a wall's level).
 - The race: 0C41F9F8 the lap (from 1), +8 the place (from 0), +C the racers
   less one, +10 the seconds left, +14 the race's time and +18.. the laps'
   (BCD m'ss"mmm); 0C4056F8 the course (0 Tatooine, 1 Etti IV).
 - The pods: 0C418B94 + i x 83C, 12 of them, 0 the player's (its position
   and progress alone): x y z, velocity +C (units a frame), rows right, up,
   back +668, +678, +688; -30 the segments driven (400 a lap on Tatooine),
   -28 the fraction of the segment. No place kept for them: they are ranked
   by their progress.
 - The bass shaker of the deluxe cabinet's seat is fed with the option
   sound board's speakers, the rear channels here: their low frequencies are
   sent as "shaker" (0..1), the engines' rumble, the hits, the landings as
   the cabinet's seat feels them.
****************************************************************************/

class SwRacer : public Decoder {
public:
	bool makes_ffb () const override { return true; }

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram_m", 0x0C000000);
		static const unsigned PODS = 12;
		const float U = 1.0f / 1.4943f;

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t scene = ram.u32 (0x0C016E2C);
		switch (scene) {
		case 0x0012C6C0: out.state = State::RACE; break;
		case 0x001735C8: out.state = State::OVER; break;
		case 0x000CB5D0: case 0x000C8600: case 0x001E06B8: out.state = State::SELECT; break;
		case 0x000A962C: case 0: out.state = State::ATTRACT; break;
		default: out.state = State::UNKNOWN; break;
		}
		out.finished = out.state == State::OVER;
		uint32_t course = ram.u32 (0x0C4056F8);
		out.track = course == 0 ? "Tatooine" : course == 1 ? "Etti IV" : std::to_string (course);
		out.player.extra.push_back ({ "shaker", shaker (mach) });
		if (out.state != State::RACE)
			return true;

		uint32_t o = ram.u32 (0x0C016E08);
		if (!ram.has (o, 0x500))
			return true;
		Vehicle &p = out.player;
		pod (ram, p, o + 0x108, o + 0x330, o + 0xE8, o + 0xF8, U);
		float max = ram.f32 (o + 0x270);
		float right = max > 0 ? ram.f32 (o + 0x6C) / max : NAN;
		float left = max > 0 ? ram.f32 (o + 0xC8) / max : NAN;
		p.throttle = (left + right) / 2;
		p.steer = 2 * ram.f32 (o + 0x410);
		/* +D0: bit 29 while a hit's jolt lasts, its frames counted in
		 * bits 23-7 (from 0002); bit 31 while boosting */
		uint32_t hit = ram.u32 (o + 0xD0);
		bool colliding = (hit & 0x20000000) != 0;
		bool boosting = (hit & 0x80000000) != 0;
		uint32_t count = (hit >> 7) & 0x1FFFF;
		bool hit_now = colliding && (!was_hit || count < last_count);
		was_hit = colliding;
		last_count = count;
		/* The probes: the nearest ground under the pod, a wall touched */
		float hover = NAN;
		bool wall = false;
		for (unsigned k = 0; k < 8; k++) {
			uint32_t a = o + 0x448 + 0x2C * k;
			float d = ram.f32 (a), nx = ram.f32 (a + 4), ny = ram.f32 (a + 8), nz = ram.f32 (a + 0xC);
			if (ny > 0.5f && (std::isnan (hover) || -d < hover))
				hover = -d;
			if (std::fabs (ny) < 0.5f && std::fabs (nx) + std::fabs (nz) > 0.5f)
				wall = true;
		}
		/* The engines' tilt from the cockpit: their rows (the same for
		 * the two), > 0 banked to the right */
		float ex = ram.f32 (o + 0x118), ey = ram.f32 (o + 0x11C), ez = ram.f32 (o + 0x120);
		float along_right = ex * ram.f32 (o + 0xD8) + ey * ram.f32 (o + 0xDC) + ez * ram.f32 (o + 0xE0);
		float along_up = ex * ram.f32 (o + 0xE8) + ey * ram.f32 (o + 0xEC) + ez * ram.f32 (o + 0xF0);
		p.extra.push_back ({ "engines_roll", -atan2f (along_up, along_right) });
		p.extra.push_back ({ "thrust_left", left });
		p.extra.push_back ({ "thrust_right", right });
		p.extra.push_back ({ "boost", boosting ? 1.0f : 0.0f });
		p.extra.push_back ({ "boosts_left", (float) ram.u32 (0x0C41FA38) });
		p.extra.push_back ({ "hit", hit_now ? 1.0f : 0.0f });
		p.extra.push_back ({ "colliding", colliding ? 1.0f : 0.0f });
		p.extra.push_back ({ "wall", wall ? 1.0f : 0.0f });
		p.extra.push_back ({ "hover", std::isnan (hover) ? NAN : hover * U });

		/* The race */
		const uint32_t R = 0x0C41F9F8;
		p.lap = (int) ram.u32 (R);
		p.position = (int) ram.u32 (R + 8) + 1;
		out.laps = 4;
		out.time_left = (float) ram.u32 (R + 0x10);
		out.race_time = bcd_time (ram.u32 (R + 0x14));
		float done = 0;
		for (int k = 0; k < 4 && k + 1 < p.lap; k++) {
			float t = bcd_time (ram.u32 (R + 0x18 + 4 * k));
			done += t;
			out.last_lap = t;
			if (t > 0 && (std::isnan (out.best_lap) || t < out.best_lap))
				out.best_lap = t;
		}
		out.lap_time = out.race_time - done;

		/* The pods: the player's progress, then the others' */
		float progress[PODS];
		for (unsigned i = 0; i < PODS; i++) {
			uint32_t b = 0x0C418B94 + i * 0x83C;
			progress[i] = ((float) ram.u32 (b - 0x30) + ram.f32 (b - 0x28)) / 400.0f;
		}
		p.progress = progress[0];
		for (unsigned i = 1; i < PODS; i++) {
			uint32_t b = 0x0C418B94 + i * 0x83C;
			Vehicle v;
			pod (ram, v, b, b + 0xC, b + 0x678, b + 0x688, U);
			v.progress = progress[i];
			v.lap = (int) (ram.u32 (b - 0x30) / 400) + 1;
			int place = 1;
			for (unsigned j = 0; j < PODS; j++)
				if (j != i && progress[j] > progress[i])
					place++;
			v.position = place;
			out.others.push_back (v);
		}
		return true;
	}

private:
	bool was_hit = false;
	uint32_t last_count = 0;
	float lowpass[2] = { 0, 0 };

	/* A pod's position, velocity (units a frame) and up and back rows */
	static void pod (const Ram &ram, Vehicle &v, uint32_t pos, uint32_t vel, uint32_t up_row, uint32_t back_row, float U)
	{
		const float F = (float) FPS;
		float up[3] = { ram.f32 (up_row), ram.f32 (up_row + 4), ram.f32 (up_row + 8) };
		float fwd[3] = { -ram.f32 (back_row), -ram.f32 (back_row + 4), -ram.f32 (back_row + 8) };

		v.x = -ram.f32 (pos) * U;
		v.y = ram.f32 (pos + 4) * U;
		v.z = ram.f32 (pos + 8) * U;
		v.vx = -ram.f32 (vel) * F * U;
		v.vy = ram.f32 (vel + 4) * F * U;
		v.vz = ram.f32 (vel + 8) * F * U;
		v.has_velocity = true;
		v.speed = std::sqrt (v.vx * v.vx + v.vy * v.vy + v.vz * v.vz);
		orient_mirrored (v, up, fwd);
	}

	/* m'ss"mmm in BCD */
	static float bcd_time (uint32_t v)
	{
		auto digit = [&] (int n) { return (int) ((v >> (4 * n)) & 0xF); };
		int ms = digit (0) + 10 * digit (1) + 100 * digit (2);
		int s = digit (3) + 10 * digit (4);
		int m = digit (5) + 10 * digit (6);
		return (float) (m * 60 + s) + ms / 1000.0f;
	}

	/* The rear channels' sound below about 100 Hz, its level this frame */
	float shaker (Machine *mach)
	{
		const std::vector<int16_t> &a = mach->audio;
		const unsigned ch = mach->audio_channels;
		const float k = 1.0f - std::exp (-2.0f * 3.14159265f * 100.0f / 44100.0f);
		double sum = 0;
		size_t n = 0;

		if (ch < 4 || a.size () < ch)
			return NAN;
		for (size_t i = 0; i + ch <= a.size (); i += ch) {
			for (int c = 0; c < 2; c++) {
				lowpass[c] += k * ((float) a[i + 2 + c] - lowpass[c]);
				sum += (double) lowpass[c] * lowpass[c];
			}
			n += 2;
		}
		return n ? std::min (1.0f, (float) std::sqrt (sum / n) / 16384.0f) : NAN;
	}
};

/****************************************************************************
 Gaelco: ATV Track (atvtrack, atvtracka), Smashing Drive (smashdrv, smashdrvb,
 smashdrvs)

 Their unit is the centimetre, their physics a step a frame (a jump falls at
 9.99 m/s^2); velocities in cm a frame. y up; matrices of 3 x 3 floats,
 row-major, their columns right, up, forward, the world as Hikaruloader's
 (the heading grows turning right). The addresses are the games' (8C......,
 the master's RAM).

 ATV Track: the game's state 8C09CB14 (1 attract, 2 the menus and results,
 3 a stage: its introduction, countdown, race; 4 game over, 6 time out);
 the quad: position 8C198004, matrix 8C198040, velocity 8C1980C4; the
 wheels' loads 8C19831C + AC x n (all 0 in the air); gear 8C1997A4, the
 engine 8C19979C (0..1); throttle, right and left brakes, steering
 8C199600..0C; time left 8C1B94B0 plus its "+N" 8C1B9498 (s), race time
 8C1B9480 (frames); the checkpoints passed 8C1B9698, missed 8C1B969C; the
 stage 8C0A1EAC, the distance along it 8C1BD7F8 (cm; 8C1BD7D8 goes on from
 the stage before); the CPU's riders: see riders ().

 Smashing Drive: the game's state 8C0766B8 (2 attract, 3 the choices and
 introduction, 5 and 6 driving, then continue and game over, 8 ranking, 9
 demo); the time 8C200B5C less the ride's 8C200B64 (frames), driving while
 some is left; the taxi: position 8C201530, matrix 8C20154C, velocity
 8C2015B8; 8C2015F8 the wheels off the ground (F all four); steering, gas
 8C201058, 8C20105C; brake and horn bytes 8C201060, 8C201064. No rival: the
 other taxis are traffic (8C211494 + k x 1F4).
****************************************************************************/

class Gaelco : public Decoder {
public:
	explicit Gaelco (bool atv) : atv (atv) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram_m", 0x0C000000);
		auto a = [] (uint32_t addr) { return addr & 0x1FFFFFFF; };

		(void) frame;
		if (!ram.ok ())
			return false;
		Vehicle &p = out.player;
		if (atv) {
			switch (ram.u32 (a (0x8C09CB14))) {
			case 1: out.state = State::ATTRACT; break;
			case 2: out.state = State::SELECT; break;
			case 3: out.state = State::RACE; break;
			case 4: case 6: out.state = State::OVER; break;
			default: out.state = State::UNKNOWN; break;
			}
			body (ram, p, a (0x8C198004), a (0x8C198040), a (0x8C1980C4));
			int airborne = 1;
			for (int n = 0; n < 4; n++) {
				bool on = ram.f32 (a (0x8C19831C + 0xAC * n)) != 0.0f;
				p.wheels[n].contact = on ? 1 : 0;
				if (on)
					airborne = 0;
			}
			p.airborne = airborne;
			p.gear = (int) ram.u32 (a (0x8C1997A4));
			p.throttle = ram.f32 (a (0x8C199604));
			p.brake = std::max (ram.f32 (a (0x8C199608)), ram.f32 (a (0x8C19960C)));
			p.steer = -ram.f32 (a (0x8C199600));
			p.extra.push_back ({ "engine", ram.f32 (a (0x8C19979C)) });
			p.extra.push_back ({ "brake_right", ram.f32 (a (0x8C199608)) });
			p.extra.push_back ({ "brake_left", ram.f32 (a (0x8C19960C)) });
			float mine = ram.f32 (a (0x8C1BD7F8));
			p.extra.push_back ({ "stage_distance", mine / 100.0f });
			out.time_left = (float) (std::max (ram.i32 (a (0x8C1B94B0)), 0) + std::max (ram.i32 (a (0x8C1B9498)), 0));
			out.race_time = ram.u32 (a (0x8C1B9480)) / (float) FPS;
			out.checkpoint = (int) ram.u32 (a (0x8C1B9698));
			out.stage = ram.i32 (a (0x8C0A1EAC));
			riders (ram, out, mine);
		} else {
			uint32_t st = ram.u32 (a (0x8C0766B8));
			float left = ((float) ram.u32 (a (0x8C200B5C)) - (float) ram.u32 (a (0x8C200B64))) / (float) FPS;
			if (st == 5 || st == 6)
				out.state = left > 0 ? State::RACE : State::OVER;
			else if (st == 3)
				out.state = State::SELECT;
			else if (st == 2 || st == 8 || st == 9)
				out.state = State::ATTRACT;
			else
				out.state = State::UNKNOWN;
			out.time_left = std::max (left, 0.0f);
			out.race_time = ram.u32 (a (0x8C200B64)) / (float) FPS;
			body (ram, p, a (0x8C201530), a (0x8C20154C), a (0x8C2015B8));
			uint32_t off = ram.u32 (a (0x8C2015F8)) & 0xF;
			for (int n = 0; n < 4; n++)
				p.wheels[n].contact = (off >> n) & 1 ? 0 : 1;
			p.airborne = off == 0xF ? 1 : 0;
			p.steer = -ram.f32 (a (0x8C201058));
			p.throttle = ram.f32 (a (0x8C20105C));
			p.brake = ram.u8 (a (0x8C201060)) ? 1.0f : 0.0f;
			p.extra.push_back ({ "horn", ram.u8 (a (0x8C201064)) ? 1.0f : 0.0f });
		}
		out.finished = out.state == State::OVER;
		return true;
	}

private:
	bool atv;

	/* ATV Track's riders the CPU drives, two at most, in the entity pool
	 * 0C1AF8E0 + k x 164 (90 entries, the order they came in): class 001D
	 * in the high half of +0, +F8 their body's position (the body, as the
	 * player's 8C198000: 8C1BD858 or 8C1BF1C8), +13C their progress along
	 * the stage (cm, as the player's 8C1BD7F8). They come once the player
	 * has gone far enough, and wait for them; the game shows no place:
	 * worked out from the progress */
	void riders (const Ram &ram, Frame &out, float mine)
	{
		static const uint32_t POOL = 0x0C1AF8E0, ENTRY = 0x164, ENTRIES = 90;
		std::vector<float> progress (1, mine);
		size_t first = out.others.size ();

		for (uint32_t k = 0; k < ENTRIES; k++) {
			uint32_t e = POOL + k * ENTRY, b;

			if ((ram.u32 (e) >> 16) != 0x001D)
				continue;
			b = (ram.u32 (e + 0xF8) & 0x1FFFFFFF) - 4;
			if (b != 0x0C1BD858 && b != 0x0C1BF1C8)
				continue;
			Vehicle v;
			body (ram, v, b + 0x04, b + 0x40, b + 0xC4);
			int airborne = 1;
			for (int n = 0; n < 4; n++) {
				bool on = ram.f32 (b + 0x31C + 0xAC * n) != 0.0f;
				v.wheels[n].contact = on ? 1 : 0;
				if (on)
					airborne = 0;
			}
			v.airborne = airborne;
			v.gear = (int) ram.u32 (b + 0x17A4);
			v.throttle = ram.f32 (b + 0x1604);
			v.brake = std::max (ram.f32 (b + 0x1608), ram.f32 (b + 0x160C));
			v.steer = -ram.f32 (b + 0x1600);
			v.extra.push_back ({ "engine", ram.f32 (b + 0x179C) });
			progress.push_back (ram.f32 (e + 0x13C));
			v.extra.push_back ({ "stage_distance", progress.back () / 100.0f });
			out.others.push_back (v);
		}
		/* Places: 1 and those ahead */
		for (size_t i = 0; i < progress.size (); i++) {
			int place = 1;
			for (size_t j = 0; j < progress.size (); j++)
				if (j != i && progress[j] > progress[i])
					place++;
			(i ? out.others[first + i - 1] : out.player).position = place;
		}
	}

	/* Position (cm), matrix (columns right, up, forward), velocity (cm a
	 * frame) */
	static void body (const Ram &ram, Vehicle &v, uint32_t pos, uint32_t mat, uint32_t vel)
	{
		float m[9];
		for (int i = 0; i < 9; i++)
			m[i] = ram.f32 (mat + 4 * i);
		float right[3] = { m[0], m[3], m[6] }, up[3] = { m[1], m[4], m[7] }, fwd[3] = { m[2], m[5], m[8] };

		v.x = ram.f32 (pos) / 100.0f;
		v.y = ram.f32 (pos + 4) / 100.0f;
		v.z = ram.f32 (pos + 8) / 100.0f;
		v.vx = ram.f32 (vel) * 0.6f;
		v.vy = ram.f32 (vel + 4) * 0.6f;
		v.vz = ram.f32 (vel + 8) * 0.6f;
		v.has_velocity = true;
		v.speed = std::sqrt (v.vx * v.vx + v.vy * v.vy + v.vz * v.vz);
		set_orientation (v, right, up, fwd);
	}
};

/****************************************************************************
 Crazy Taxi (crzytaxi), NAOMI

 Checked in rides from the start of play, the defaults. A unit is 10 cm (the
 HUD's distances are the units / 10); the game's gravity is some 4 g (0.110
 unit a frame per frame in a fall). The world is mirrored (the matrix's rows
 left, up, forward; steering right lowers its heading): x is turned round.
 - The game: 0C288B60 is 1 from START to the end of GAME OVER; the scene
   0C288B64 in pairs, starting and running: 0/1 the play (the boot's intro
   too), 2/3 the demo or GAME OVER, 4/5 the driver's choice, 6/7 the title,
   8/9 RESULT, A/B HOW TO PLAY; in the play, 0C288C38: 0 the intro, 1
   driving from GO!, 2 a customer getting off (115 frames, the taxi held,
   the clock stopped), 3 TIME UP.
 - The time left 0C288C44 (frames, the HUD's number), the time played
   0C288C40 (frames).
 - The taxi: x y z 0C22C760, its velocity 0C22C76C (units a frame), its
   matrix 0C22C814 (4 x 4, rows left, up, forward), its wheels' speed
   0C22C8BC (units a frame, signed; held in the air, spinning against a
   wall), the automatic gear 0C22C880 (0 REVERSE, 1 to 4 in DRIVE), rpm
   0C22C8B8; each wheel's ground 0C22C900 + n x 84 (front left, front
   right, rear left, rear right), FFFFFFFF off the ground.
 - Steering (+1 right), accelerator and brake as the game reads them:
   0C288BAC, B0, B4.
 - The customer: 0C288C3C 1 aboard (in the demo too), the time left to his
   stop 0C288C7C (frames), the distance to it 0C288E64 (units, the HUD's
   metres: to the stop's zone, not written inside it), the fare 0C2A5F98
   (cents).
 - The velocity swings across the taxi from a frame to the next, a few cm a
   frame cruising, a metre against a wall or on a kerb, the body with it:
   the mean of two frames is sent. No hit is flagged: one is that mean's
   horizontal change of more than a unit a frame in a frame (driving,
   cornering and the kerbs stay under 0.86, the trees, walls and fences
   from 1.2 to 3.2).
 - A customer gets off when the taxi stops (wheels' speed 0.15 at most)
   inside his stop's zone, a polygon (the HUD's distance is to the circle
   round it). The money: the total earned 0C288C58 (cents; the fare paid
   some 50 frames after the stop, cleared at the next game's intro), the
   customers delivered 0C22D5AC (RESULT's number, 115 frames after the
   stop), the last ride's rank 0C2A6098 (-1 BAD, 0 SLOW, 1 NORMAL, 2
   SPEEDY), the combo 0C2A1628 (the HUD's COMBO; cleared at a stop, at
   TIME UP, while a customer boards), the ranking's place 0C288C14 (100
   none yet), RESULT's class 0C2A721C (0 S to 5 E, from RESULT's first
   frame to GAME OVER's end); the rides ended 0C288C4C (at the stop).
 Not found: the driver; the classes' money thresholds.
****************************************************************************/

class CrazyTaxi : public Decoder {
public:
	bool makes_ffb () const override { return true; }

	FfbScale ffb_scale () const override
	{
		FfbScale scale;

		scale.top_speed = 36.0f;
		scale.hard_turn = HARD_TURN;
		scale.bump = BUMP;
		return scale;
	}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		const float U = 0.1f;			/* m a unit */
		const float V = U * (float) FPS;	/* m/s a unit a frame */

		if (!ram.ok ())
			return false;
		uint32_t game = ram.u32 (0x0C288B60), scene = ram.u32 (0x0C288B64), phase = ram.u32 (0x0C288C38);
		if (game != 1)
			out.state = State::ATTRACT;
		else if (scene <= 1)
			out.state = phase == 1 || phase == 2 ? State::RACE : phase == 0 ? State::SELECT : State::OVER;
		else if (scene == 4 || scene == 5)
			out.state = State::SELECT;
		else if (scene == 2 || scene == 3 || scene == 8 || scene == 9)
			out.state = State::OVER;
		else
			out.state = State::ATTRACT;
		out.finished = out.state == State::OVER;
		out.transmission = "AT";
		out.time_left = (float) std::max (ram.i32 (0x0C288C44), 0) / (float) FPS;
		out.race_time = (float) ram.u32 (0x0C288C40) / (float) FPS;

		Vehicle &p = out.player;
		const uint32_t M = 0x0C22C814;
		float up[3] = { ram.f32 (M + 0x10), ram.f32 (M + 0x14), ram.f32 (M + 0x18) };
		float fwd[3] = { ram.f32 (M + 0x20), ram.f32 (M + 0x24), ram.f32 (M + 0x28) };
		p.x = -ram.f32 (0x0C22C760) * U;
		p.y = ram.f32 (0x0C22C764) * U;
		p.z = ram.f32 (0x0C22C768) * U;
		orient_mirrored (p, up, fwd);

		/* The velocity, the mean of this frame's and the last's; a hit,
		 * that mean's horizontal change */
		float v[3] = { -ram.f32 (0x0C22C76C) * V, ram.f32 (0x0C22C770) * V, ram.f32 (0x0C22C774) * V };
		bool follows = have_last && frame == last_frame + 1;
		float mean[3];
		for (int i = 0; i < 3; i++)
			mean[i] = follows ? (v[i] + last_v[i]) / 2 : v[i];
		float dv = follows && have_mean ? std::hypot (mean[0] - last_mean[0], mean[2] - last_mean[2]) : 0.0f;
		p.impact = dv > HIT * V ? dv : 0.0f;
		p.vx = mean[0];
		p.vy = mean[1];
		p.vz = mean[2];
		p.has_velocity = true;
		p.speed = std::sqrt (p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
		have_mean = follows;
		have_last = true;
		last_frame = frame;
		memcpy (last_v, v, sizeof (v));
		memcpy (last_mean, mean, sizeof (mean));

		int off = 0;
		for (int n = 0; n < 4; n++) {
			bool down = ram.u32 (0x0C22C900 + 0x84 * n) != 0xFFFFFFFF;
			p.wheels[n].contact = down ? 1 : 0;
			off += down ? 0 : 1;
		}
		p.airborne = off == 4 ? 1 : 0;
		uint32_t gear = ram.u32 (0x0C22C880);
		p.gear = gear == 0 ? -1 : gear <= 4 ? (int) gear : UNKNOWN;
		p.gears = 4;
		p.rpm = ram.f32 (0x0C22C8B8);
		p.steer = -ram.f32 (0x0C288BAC);
		p.throttle = ram.f32 (0x0C288BB0);
		p.brake = ram.f32 (0x0C288BB4);
		p.extra.push_back ({ "wheel_speed", ram.f32 (0x0C22C8BC) * V });
		bool aboard = out.state == State::RACE && ram.u32 (0x0C288C3C) == 1;
		p.extra.push_back ({ "customer", aboard ? 1.0f : 0.0f });
		p.extra.push_back ({ "customer_time", aboard ? (float) std::max (ram.i32 (0x0C288C7C), 0) / (float) FPS : NAN });
		p.extra.push_back ({ "destination", aboard ? ram.f32 (0x0C288E64) * U : NAN });
		p.extra.push_back ({ "fare", aboard ? (float) ram.u32 (0x0C2A5F98) / 100.0f : NAN });
		bool playing = game == 1 && scene <= 1 && phase != 0;
		bool result = game == 1 && (scene == 8 || scene == 9 || scene == 2 || scene == 3);
		int32_t rank = ram.i32 (0x0C2A6098), place = ram.i32 (0x0C288C14), grade = ram.i32 (0x0C2A721C);
		p.extra.push_back ({ "earned", playing || result ? (float) ram.u32 (0x0C288C58) / 100.0f : NAN });
		p.extra.push_back ({ "customers", playing || result ? (float) ram.u32 (0x0C22D5AC) : NAN });
		/* The rides ended in this game 0C288C4C (at the stop) */
		bool ridden = playing && ram.u32 (0x0C288C4C) > 0;
		p.extra.push_back ({ "ride_rank", ridden && rank >= -1 && rank <= 2 ? (float) rank : NAN });
		p.extra.push_back ({ "combo", playing ? (float) ram.u32 (0x0C2A1628) : NAN });
		p.extra.push_back ({ "ranking", (playing || result) && place > 0 && place < 100 ? (float) place : NAN });
		p.extra.push_back ({ "class", result && grade >= 0 && grade <= 5 ? (float) grade : NAN });
		return true;
	}

private:
	/* A hit, units a frame (above). The effects' scale, measured in
	 * rides: 50 m/s^2 sideways in the hard turns (30 in an ordinary one);
	 * the body bouncing on the roads' rises some 40 m/s^2 off its average
	 * (the game's 4 g), more on landing and on the kerbs */
	static constexpr float HIT = 1.0f;
	static constexpr float HARD_TURN = 50.0f;
	static constexpr float BUMP = 60.0f;

	bool have_last = false, have_mean = false;
	uint64_t last_frame = 0;
	float last_v[3] = {}, last_mean[3] = {};
};

/****************************************************************************
 Jambo! Safari (jambo), NAOMI

 Checked in Expert and Beginner rides of the first area, a capture. A unit
 is 10 cm (the game's km/h is the units a frame x 21.6; the HUD's distance
 to a roped animal the units / 10). The world is mirrored (steering right
 lowers its heading): x is turned round. The jeep never leaves the ground.
 - The game 0C2AAD2C: 0 the attract, 3 the mode's and the ranger's choice,
   4 an area (its intro, the play, CONTINUE?), 7 RESULTS, 8 GAME OVER; in an
   area, the time left 0C2B90D4 (s, below 0 from TIME UP), the intro's
   count 0C2B90E0 (START! once past 0C15B730), the capture's scene
   0C168190 (1 the net's hit, 2 GREAT!) and 0C2AAD30 5 the binoculars after
   it, the game driving.
 - The jeep: x y z 0C2B5084, the last frame's 0C2B5090 (units); its matrix
   0C2B27D8 (3 x 3, rows 10 apart, columns right, up, back); the speed
   0C2B1F20 (units a frame, signed); a tumble 0C2B1FFC (rad, about the nose,
   the right side down: a roped animal yanking it, up to a roll-over); each
   wheel's ground 0C2B2470 + n x 60 (front right, front left, rear right,
   rear left), bits 8-11: C, D, E grass, 2 sand or mud, 4 water.
 - The inputs as read: steering 0C2B5120 (0 to 118, 59 the centre, + right),
   accelerator 0C2AB3D4 and brake 0C2AB3D8 (0 to 254), the lever 0C2AB3B8
   (10 LEVER UP, throwing the rope, then the net; 20 LEVER DOWN, pulling).
 - The rope on an animal 0C1437B4; RANGER POINTS 0C2B7FB8; the animals
   caught 0C2B7FBC.
 - No hit is flagged: one is a change of the horizontal velocity of more
   than a unit a frame in a frame (free driving stays under 0.22, a tree
   28); roped, the animal's yanks reach 4 (12 once): 8 then.
 Not found: the rope's length (the HUD's distance), the crates' animals.
****************************************************************************/

class Jambo : public Decoder {
public:
	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		const float U = 0.1f;			/* m a unit */
		const float V = U * (float) FPS;	/* m/s a unit a frame */

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t mode = ram.u32 (0x0C2AAD2C);
		float timer = ram.f32 (0x0C2B90D4);
		if (mode == 4) {
			if (timer < 0)
				out.state = State::OVER;
			else if (ram.u32 (0x0C2B90E0) <= ram.u32 (0x0C15B730))
				out.state = State::SELECT;
			else if (ram.u32 (0x0C168190) || ram.u32 (0x0C2AAD30) == 5)
				/* The capture's scenes, the game driving */
				out.state = State::REPLAY;
			else
				out.state = State::RACE;
		} else if (mode == 7 || mode == 8)
			out.state = State::OVER;
		else if (mode == 3)
			out.state = State::SELECT;
		else
			out.state = State::ATTRACT;
		out.finished = out.state == State::OVER;
		out.time_left = std::max (timer, 0.0f);

		Vehicle &p = out.player;
		const uint32_t M = 0x0C2B27D8;
		float up[3] = { ram.f32 (M + 0x04), ram.f32 (M + 0x14), ram.f32 (M + 0x24) };
		float fwd[3] = { -ram.f32 (M + 0x08), -ram.f32 (M + 0x18), -ram.f32 (M + 0x28) };
		p.x = -ram.f32 (0x0C2B5084) * U;
		p.y = ram.f32 (0x0C2B5088) * U;
		p.z = ram.f32 (0x0C2B508C) * U;
		orient_mirrored (p, up, fwd);
		float tumble = ram.f32 (0x0C2B1FFC);
		p.roll += tumble;
		p.vx = -(ram.f32 (0x0C2B5084) - ram.f32 (0x0C2B5090)) * V;
		p.vy = (ram.f32 (0x0C2B5088) - ram.f32 (0x0C2B5094)) * V;
		p.vz = (ram.f32 (0x0C2B508C) - ram.f32 (0x0C2B5098)) * V;
		p.has_velocity = true;
		p.speed = std::fabs (ram.f32 (0x0C2B1F20)) * V;

		/* A hit: the horizontal velocity's change */
		bool roped = out.state == State::RACE && ram.u32 (0x0C1437B4) == 1;
		float dv = have_last ? std::hypot (p.vx - last_vx, p.vz - last_vz) : 0.0f;
		p.impact = out.state == State::RACE && dv > (roped ? ROPED_HIT : HIT) * V ? dv : 0.0f;
		have_last = true;
		last_vx = p.vx;
		last_vz = p.vz;

		/* The game's wheels: front right, front left, rear right, rear
		 * left */
		static const int order[4] = { 1, 0, 3, 2 };
		for (int n = 0; n < 4; n++) {
			uint32_t ground = (ram.u32 (0x0C2B2470 + 0x60 * n) >> 8) & 0xF;
			p.wheels[order[n]].surface = ground == 4 ? "water" : ground == 2 ? "sand" :
			                             ground >= 0xC && ground <= 0xE ? "grass" : "";
		}
		p.steer = -std::clamp ((ram.f32 (0x0C2B5120) - 59.0f) / 59.0f, -1.0f, 1.0f);
		p.throttle = ram.u8 (0x0C2AB3D4) / 254.0f;
		p.brake = ram.u8 (0x0C2AB3D8) / 254.0f;
		uint8_t lever = ram.u8 (0x0C2AB3B8);
		p.extra.push_back ({ "lever", (lever & 0x10) ? 1.0f : (lever & 0x20) ? -1.0f : 0.0f });
		p.extra.push_back ({ "tumble", tumble });
		p.extra.push_back ({ "roped", roped ? 1.0f : 0.0f });
		p.extra.push_back ({ "score", (float) ram.u32 (0x0C2B7FB8) });
		p.extra.push_back ({ "captured", (float) ram.u32 (0x0C2B7FBC) });
		return true;
	}

private:
	/* A hit, units a frame (above) */
	static constexpr float HIT = 1.0f;
	static constexpr float ROPED_HIT = 8.0f;

	bool have_last = false;
	float last_vx = 0, last_vz = 0;
};

/****************************************************************************
 The King of Route 66 (kingrt66, Rev A), NAOMI 2

 Checked in the first two stages of AREA 1 with the Texas Hawk, the
 defaults. A unit is a metre (the HUD's km/h is the velocity x 3.6). The
 world is mirrored (the matrices' rows left, up, forward; steering right
 lowers its heading): x is turned round. The game moves the truck at 30 Hz,
 every other frame: the velocity is left to the deriver, which takes such
 moves (the game's own velocity, 0C784590, misses the crashes: the solver
 puts the body back, the vector keeps the wheels' motion).
 - The game's block B, the pointer at 0C25FB5C (0C3C5464): its stage's
   flags +4, the game's +8, the time left +58 (seconds in the high half,
   sixtieths in the low; frozen at a stage's end), the distance to the goal
   +30 (m, the HUD's), the speed +34 (km/h, the wheels': never below 0, on
   at a wall), rpm +38, the 66 emblems +20, the reward +28 ($), a copy of
   the tractor's position +DC and its heading +F8 (16 bits a turn).
 - The state: attract while the game's flags have E0000 (the title, the
   GAME RULE demo with its own truck); time up to GAME OVER, the stage's
   40000; YOU MADE IT! and the load's result, 80000 and 2000; driving from
   GET GOING! (the truck rolling, the inputs live), 0C339724 not 0 with the
   stage's 400 and not 4000; WRONG WAY 20000.
 - The tractor's block T, on the heap (its place changes from a start to
   another), the pointer at 0CD122B0 (0 before the stage): x y z T+508, its
   matrix T+56C (3 x 3, rows 10 apart: left, up, forward), the automatic's
   gear T+C40 (-1 REVERSE, 0 to 4 the five gears, the fifth above some 130
   km/h, with the nitro); the trailer: x y z
   T+E20, its matrix T+E84, 8.4 m behind (left behind in the stages without
   a load: hitched within 12 m). Taken while the tractor agrees with B's
   copy (T is the last truck's after a game over), else B's position and
   heading.
 - The nitro: the cans B+0 bits 10-13 (0 to 3), B+4 bit 0 from the order
   to the burn's end (some 76 frames of charge), bit 1 its burn (118
   frames). The game orders it by voice alone (the CB-MIC's word).
 - The rival: x y z B+7C, its trailer's B+88, its heading B+98 (16 bits, as
   the player's B+F8).
 - The inputs as read: steering 0C37543C (128 the centre, + right),
   accelerator 0C37543E, brake 0C375440 (0 to 255); the lever 0C375434 (20
   D, 10 R).
 kingrt66p (the prototype): the same blocks, B's pointer 0C24C394, the
 stage's word 0C326094, T's pointer 0CC8031C, the trailer 4 bytes lower in
 T (T+E1C, its matrix T+E80), the raw inputs 13100 lower (0C36233C).
 Not found: a hit flag, the wheels, the rival's speed (worked out from its
 moves).
****************************************************************************/

class KingRt66 : public Decoder {
public:
	/* The release's and the prototype's (kingrt66p): B's pointer, the
	 * stage's word, the tractor's pointer, the trailer's x y z and matrix
	 * in T, the raw steering (the pedals 2 and 4 on) */
	struct Map {
		uint32_t B, staged, T, trailer, tmatrix, inputs;
	};
	static constexpr Map RELEASE = { 0x0C25FB5C, 0x0C339724, 0x0CD122B0, 0xE20, 0xE84, 0x0C37543C };
	static constexpr Map PROTOTYPE = { 0x0C24C394, 0x0C326094, 0x0CC8031C, 0xE1C, 0xE80, 0x0C36233C };

	explicit KingRt66 (const Map &map) : L (map) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t B = ram.u32 (L.B);
		if (!ram.has (B, 0x110))
			return false;
		uint32_t fs = ram.u32 (B + 0x04), fg = ram.u32 (B + 0x08);
		bool staged = ram.u32 (L.staged) != 0;
		bool cleared = (fs & 0x80000) && (fs & 0x2000);
		if (fg & 0xE0000)
			out.state = State::ATTRACT;
		else if ((fs & 0x40000) || cleared)
			out.state = State::OVER;
		else if (staged && (fs & 0x400) && !(fs & 0x4000))
			out.state = State::RACE;
		else
			out.state = State::SELECT;
		out.finished = cleared;
		out.transmission = "AT";
		uint32_t t = ram.u32 (B + 0x58);
		out.time_left = (float) (t >> 16) + (float) (t & 0xFFFF) / (float) FPS;

		Vehicle &p = out.player;
		uint32_t T0 = ram.u32 (L.T);
		bool block = ram.has (T0, 0x1000);
		const uint32_t T = T0 + 0x508, M = T0 + 0x56C;
		float pos[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, 1 };
		if (block)
			for (int i = 0; i < 3; i++) {
				pos[i] = ram.f32 (T + 4 * i);
				fwd[i] = ram.f32 (M + 0x20 + 4 * i);
			}
		float copy[3] = { ram.f32 (B + 0xDC), ram.f32 (B + 0xE0), ram.f32 (B + 0xE4) };
		bool body = block && std::hypot (pos[0] - copy[0], pos[2] - copy[2]) < 5.0f;
		if (body) {
			float up[3] = { ram.f32 (M + 0x10), ram.f32 (M + 0x14), ram.f32 (M + 0x18) };
			p.x = -pos[0];
			p.y = pos[1];
			p.z = pos[2];
			orient_mirrored (p, up, fwd);
		} else {
			/* B's copy: the heading alone */
			float h = -(float) ram.u16 (B + 0xF8) * 6.2831853f / 65536.0f;
			float right[3] = { std::cos (h), 0, -std::sin (h) }, up[3] = { 0, 1, 0 };
			float ahead[3] = { std::sin (h), 0, std::cos (h) };
			p.x = -copy[0];
			p.y = copy[1];
			p.z = copy[2];
			set_orientation (p, right, up, ahead);
		}
		p.speed = ram.f32 (B + 0x34) / 3.6f;
		p.rpm = ram.f32 (B + 0x38);
		int32_t gear = body ? ram.i32 (T0 + 0xC40) : INT32_MAX;
		p.gear = gear < 0 ? -1 : gear <= 4 ? gear + 1 : UNKNOWN;
		p.gears = 5;
		p.steer = -std::clamp ((ram.u8 (L.inputs) - 128.0f) / 127.0f, -1.0f, 1.0f);
		p.throttle = ram.u8 (L.inputs + 2) / 255.0f;
		p.brake = ram.u8 (L.inputs + 4) / 255.0f;

		/* The trailer: its angle to the tractor, > 0 the cab turned right
		 * of it */
		const uint32_t TT = T0 + L.trailer, TM = T0 + L.tmatrix;
		float tp[3] = { ram.f32 (TT), ram.f32 (TT + 4), ram.f32 (TT + 8) };
		bool hitched = body && std::sqrt ((tp[0] - pos[0]) * (tp[0] - pos[0]) + (tp[1] - pos[1]) * (tp[1] - pos[1]) +
		                                  (tp[2] - pos[2]) * (tp[2] - pos[2])) < 12.0f;
		float angle = 0.0f;
		if (hitched) {
			float a = std::atan2 (fwd[0], fwd[2]) - std::atan2 (ram.f32 (TM + 0x20), ram.f32 (TM + 0x28));
			while (a > 3.14159265f)
				a -= 6.2831853f;
			while (a < -3.14159265f)
				a += 6.2831853f;
			angle = -a;
		}
		bool racing = out.state == State::RACE;
		p.extra.push_back ({ "trailer", hitched ? 1.0f : 0.0f });
		p.extra.push_back ({ "trailer_angle", hitched ? angle : NAN });
		p.extra.push_back ({ "distance_to_goal", ram.f32 (B + 0x30) });
		p.extra.push_back ({ "wrong_way", racing && (fs & 0x20000) ? 1.0f : 0.0f });
		p.extra.push_back ({ "emblems", (float) ram.u32 (B + 0x20) });
		p.extra.push_back ({ "reward", racing ? (float) ram.u32 (B + 0x28) : NAN });
		/* The nitro: the cans left, the order (its charge and burn), the
		 * burn */
		p.extra.push_back ({ "nitro", racing ? (float) ((ram.u32 (B) >> 10) & 15) : NAN });
		p.extra.push_back ({ "nitro_on", racing && (fs & 1) ? 1.0f : 0.0f });
		p.extra.push_back ({ "nitro_boost", racing && (fs & 2) ? 1.0f : 0.0f });

		/* The rival, while driving */
		if (racing) {
			Vehicle v;
			v.x = -ram.f32 (B + 0x7C);
			v.y = ram.f32 (B + 0x80);
			v.z = ram.f32 (B + 0x84);
			orient_angles (v, -(float) ram.u16 (B + 0x98) * 6.2831853f / 65536.0f, 0, 0);
			out.others.push_back (v);
		}
		return true;
	}

private:
	const Map &L;
};

/****************************************************************************
 F355 Challenge (f355twin, f355twn2, f355), NAOMI

 Checked on f355twin in Training and Race at Motegi, AT and semi-automatic,
 the defaults; the master board's RAM (it draws nothing). A unit is a metre
 (the velocity in m/s, the HUD's km/h the speed x 3.6). The world is
 mirrored (steering right lowers its heading): x is turned round.
 - The mode 0C3A38F8: 7 the course's, level's and mode's choice, 8 the
   intro and the countdown, 9 the race (from GO), 10 GOAL and the results
   (the game driving), 13 TIME UP and GAME OVER, 17 the VMU's save; the
   others the attract (its demo replays a race in the car).
 - The car 0C1E0040: x y z +10, its matrix +7C (rows 10 apart: right, up,
   back), the gear +1D0 (1 to 6), the laps done +38 (16 bits), the lap's
   start +134, the best +138, the last +13C (ticks of 1/60.2 s, the game's
   clock), the place +140 (from 0), the best's and the last's fractions
   +144 and +148 (the sub-frame crossing), the fractions' sum +14C; the
   transmission 0C1E0028 (2 AT, 1 the paddles, 0 the H stick: 0 neutral at
   +1D0, no reverse; offered on a deluxe cabinet alone, its level
   SIMULATOR), the level 0C1E002C (0 NOVICE, 1 INTERMEDIATE, 2 SIMULATOR),
   the mode 0C1E0030 (0 TRAINING, 1 DRIVING, 2 RACE), the race's laps
   0C41DF80 (16 bits, -1 none), the assists 0C1E88CC (bits 3 SC, 2 TC, 1
   ABS, 0 IBS: 1 on).
 - The physics 0C24D378: the velocity +C (m/s), rpm +1C, the speed +2C,
   each wheel's spin +58 + n x 14 (rad/s; front left, front right, rear
   left, rear right); frozen after GOAL.
 - The race's clock 0C1E8188 (ticks); the time left 0C1E818C (frames, the
   HUD's R.TIME 10 frames under; 37.4 s more at each line).
 - The inputs as read: steering, accelerator, brake, clutch 0C1E8B74 to 77
   (128 the centre, + right; 0 to 254).
 - The opponents: the 7 cars after the player's, 548 apart (their first
   word 0 when unused: TRAINING, DRIVING), their x y z +10, matrix +7C,
   velocity +24 and speed +C4 a frame times their scale +6C (0.95 to 1.24
   a car), laps +38, place +140; sent from GO to GAME OVER (they drive on
   after GOAL, they stand after TIME UP).
 f355 (the deluxe, its H stick with the defaults): mode 0C3C319C (each
 value from the choice on one more, 15 NAME ENTRY), physics 0C26D020, the
 car's x y z 0C20003C, matrix 0C200070, gear 0C2001A8 (0 neutral), the
 transmission, level and mode 0C208160 to 68, the assists 0C2081D0, the
 race's laps 0C43D7B0, the inputs 0C20842C, the laps done 0C2000CC, the
 lap times 0C20010C, the clock 0C207A6C, the time left 0C207A70; its cars
 0C200020, 4F0 apart (x y z +1C, matrix +50, velocity +90, scale +228,
 speed +34, laps +AC, place +F8). f355twn2 (checked at Atlanta, 5 laps):
 mode 0C329380 (the same values), physics 0C1F0164, the car 0C2E3740 (the
 twin's layout, its gear +1CC), the transmission 0C2EBC9C, level
 0C2EB7FC, mode 0C2EB9FC, the assists 0C2EB618, the race's laps 0C3A0C44,
 the inputs 0C2EC060, the clock 0C2EBB70, the time left 0C2EB7F4.
 Not found: the wheels' ground; f355twn2's H stick (its neutral taken as
 the others'); the opponents' rpm, gear and wheels (none kept).
****************************************************************************/

class F355 : public Decoder {
public:
	enum Set { TWIN, TWIN2, DELUXE };

	explicit F355 (Set set) : set (set) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		/* The layouts: mode, physics, the car's x y z, matrix, gear */
		static const struct {
			uint32_t mode, physics, pos, matrix, gear;
			unsigned shift;
		} sets[3] = {
			{ 0x0C3A38F8, 0x0C24D378, 0x0C1E0050, 0x0C1E00BC, 0x0C1E0210, 0 },
			{ 0x0C329380, 0x0C1F0164, 0x0C2E3750, 0x0C2E37BC, 0x0C2E390C, 0 },
			{ 0x0C3C319C, 0x0C26D020, 0x0C20003C, 0x0C200070, 0x0C2001A8, 1 },
		};
		const auto &L = sets[set];

		(void) frame;
		if (!ram.ok ())
			return false;
		/* The deluxe's numbers are one more from the choice on */
		uint32_t raw = ram.u32 (L.mode), mode = raw;
		if (mode >= 7 + L.shift)
			mode -= L.shift;
		out.state = mode == 9 ? State::RACE : mode == 7 || mode == 8 ? State::SELECT :
		            mode == 10 || mode == 13 || mode == 17 || (set == DELUXE && raw == 15) ? State::OVER :
		            State::ATTRACT;
		out.finished = mode == 10;

		Vehicle &p = out.player;
		const uint32_t M = L.matrix, P = L.physics;
		float up[3] = { ram.f32 (M + 0x10), ram.f32 (M + 0x14), ram.f32 (M + 0x18) };
		float fwd[3] = { -ram.f32 (M + 0x20), -ram.f32 (M + 0x24), -ram.f32 (M + 0x28) };
		p.x = -ram.f32 (L.pos);
		p.y = ram.f32 (L.pos + 4);
		p.z = ram.f32 (L.pos + 8);
		orient_mirrored (p, up, fwd);
		/* The physics' own while it runs */
		if (out.state == State::RACE) {
			p.vx = -ram.f32 (P + 0x0C);
			p.vy = ram.f32 (P + 0x10);
			p.vz = ram.f32 (P + 0x14);
			p.has_velocity = true;
		}
		p.speed = ram.f32 (P + 0x2C);
		p.rpm = ram.f32 (P + 0x1C);
		p.rpm_max = 8500.0f;
		uint32_t gear = ram.u32 (L.gear);
		p.gear = gear >= 1 && gear <= 6 ? (int) gear : UNKNOWN;
		p.gears = 6;
		/* The wheels' spin: front left, front right, rear left, rear right */
		for (int n = 0; n < 4; n++)
			p.wheels[n].spin = ram.f32 (P + 0x58 + 0x14 * n);
		/* The race's: the transmission's, level's and mode's words; the
		 * assists; the race's laps; the inputs; the car's laps done and
		 * its lap times (start, best, last, place, their fractions, the
		 * fractions' sum); the race's clock; the time left */
		static const struct {
			uint32_t box, level, race_mode, assists, laps, inputs, laps_done, times, clock, left;
		} races[3] = {
			{ 0x0C1E0028, 0x0C1E002C, 0x0C1E0030, 0x0C1E88CC, 0x0C41DF80, 0x0C1E8B74, 0x0C1E0078, 0x0C1E0174,
			  0x0C1E8188, 0x0C1E818C },
			{ 0x0C2EBC9C, 0x0C2EB7FC, 0x0C2EB9FC, 0x0C2EB618, 0x0C3A0C44, 0x0C2EC060, 0x0C2E3778, 0x0C2E3874,
			  0x0C2EBB70, 0x0C2EB7F4 },
			{ 0x0C208160, 0x0C208164, 0x0C208168, 0x0C2081D0, 0x0C43D7B0, 0x0C20842C, 0x0C2000CC, 0x0C20010C,
			  0x0C207A6C, 0x0C207A70 },
		};
		const auto &R = races[set];
		uint32_t box = ram.u32 (R.box);
		out.transmission = box == 2 ? "AT" : box <= 1 ? "MT" : "";
		if (box == 0 && ram.u32 (L.gear) == 0)
			p.gear = 0;
		p.extra.push_back ({ "shifter", box <= 2 ? (float) box : NAN });
		p.extra.push_back ({ "level", (float) ram.u32 (R.level) });
		p.extra.push_back ({ "race_mode", (float) ram.u32 (R.race_mode) });
		uint8_t assists = ram.u8 (R.assists);
		p.extra.push_back ({ "sc", (assists & 8) ? 1.0f : 0.0f });
		p.extra.push_back ({ "tc", (assists & 4) ? 1.0f : 0.0f });
		p.extra.push_back ({ "abs", (assists & 2) ? 1.0f : 0.0f });
		p.extra.push_back ({ "ibs", (assists & 1) ? 1.0f : 0.0f });
		int16_t laps = (int16_t) ram.u16 (R.laps);
		if (laps > 0)
			out.laps = laps;
		p.steer = -std::clamp ((ram.u8 (R.inputs) - 128.0f) / 127.0f, -1.0f, 1.0f);
		p.throttle = ram.u8 (R.inputs + 1) / 254.0f;
		p.brake = ram.u8 (R.inputs + 2) / 254.0f;
		p.clutch = ram.u8 (R.inputs + 3) / 254.0f;
		/* The race's from GO on; the last lap at GOAL */
		if (mode < 9)
			return true;
		unsigned done = ram.u16 (R.laps_done);
		p.lap = (int) (mode == 9 ? done + 1 : std::max (done, 1u));
		const uint32_t LT = R.times;
		p.position = ram.u16 (LT + 0x0C) + 1;
		const float TICK = 60.2f;
		float t = ram.f32 (R.clock);
		out.race_time = t / TICK;
		out.lap_time = std::max ((t - (float) ram.u32 (LT) + ram.f32 (LT + 0x18)) / TICK, 0.0f);
		if (done) {
			out.best_lap = ((float) ram.u32 (LT + 4) + ram.f32 (LT + 0x10)) / TICK;
			out.last_lap = ((float) ram.u32 (LT + 8) + ram.f32 (LT + 0x14)) / TICK;
		}
		out.time_left = (float) std::max (ram.i32 (R.left) - 10, 0) / (float) FPS;

		/* The opponents, from GO to GAME OVER: the cars after the player's
		 * in use. Each moves by its velocity times its own scale */
		static const struct {
			uint32_t base, stride, pos, matrix, vel, scale, speed, laps, place;
		} cars[3] = {
			{ 0x0C1E0040, 0x548, 0x10, 0x7C, 0x24, 0x6C, 0xC4, 0x38, 0x140 },
			{ 0x0C2E3740, 0x548, 0x10, 0x7C, 0x24, 0x6C, 0xC4, 0x38, 0x140 },
			{ 0x0C200020, 0x4F0, 0x1C, 0x50, 0x90, 0x228, 0x34, 0xAC, 0xF8 },
		};
		const auto &C = cars[set];
		if (mode != 9 && mode != 10)
			return true;
		for (unsigned k = 1; k < 8; k++) {
			const uint32_t O = C.base + C.stride * k, CM = O + C.matrix;
			if (!ram.u32 (O))
				continue;
			Vehicle v;
			float vup[3] = { ram.f32 (CM + 0x10), ram.f32 (CM + 0x14), ram.f32 (CM + 0x18) };
			float vfwd[3] = { -ram.f32 (CM + 0x20), -ram.f32 (CM + 0x24), -ram.f32 (CM + 0x28) };
			float scale = ram.f32 (O + C.scale) * (float) FPS;
			v.x = -ram.f32 (O + C.pos);
			v.y = ram.f32 (O + C.pos + 4);
			v.z = ram.f32 (O + C.pos + 8);
			orient_mirrored (v, vup, vfwd);
			v.vx = -ram.f32 (O + C.vel) * scale;
			v.vy = ram.f32 (O + C.vel + 4) * scale;
			v.vz = ram.f32 (O + C.vel + 8) * scale;
			v.has_velocity = true;
			v.speed = ram.f32 (O + C.speed) * scale;
			unsigned vd = ram.u16 (O + C.laps);
			v.lap = (int) (laps > 0 ? std::min (vd + 1, (unsigned) laps) : vd + 1);
			v.position = ram.u16 (O + C.place) + 1;
			out.others.push_back (v);
		}
		return true;
	}

private:
	Set set;
};

/****************************************************************************
 18 Wheeler: American Pro Trucker (18wheelr and its sets), NAOMI

 Checked in the first stage (Asphalt Cowboy, the fuel tanker), the
 defaults. A unit is a metre (the truck's size); the truck goes through the
 world 1.7 times as fast as its speed and the HUD say. The game moves it at
 30 Hz: the velocity is left to the deriver. The world is mirrored
 (steering right lowers its heading): x is turned round.
 - The scene, from the sound bank loaded (0C3AC8C4): 15 the attract (its
   HOW TO PLAY drives the truck), 4 the truck's choice, 7 CONTINUE to the
   ranking, 12 the NAME ENTRY, FFFFFFFF loading and the ending's staff roll
   (the state as it was); in the others (9 to C the stages, E the
   parkings), 0C3ACB34 1 driving (TIME UP once the time left is 0), else
   READY, the map and the trailer's choice.
 - The time left 0C3ACBB0 (frames), the reward 0C3ACBC4 ($).
 - The tractor: x y z 0C4E9F90; its matrix in a list drawn, 0C289620 (4 x 4,
   rows right, up, back, the position): taken when its position is the
   tractor's (the slot holds another thing at a stop, after TIME UP); the
   trailer's 0C2895E0, its position the kingpin. The speed 0C4E9F60 (m/s,
   below 0 reversing), rpm 0C4E9F40, the gearbox 0C4E9F38 (1 to 3 in LOW,
   4 in HIGH, 5 in R).
 - The lever 0C3AC92C (0 none, driving as LOW; 2 LOW, 1 HIGH, 6 R); the
   inputs as the game takes them: steering 0C3ACC80 (+ right, 1 short of
   the lock), accelerator 0C3ACC88, brake 0C3ACC90; the horn 0C2B4FE5 bit 2.
 - The reward on the HUD 0C3ACBCC, the hits taking from the stage's
   0C3ACBC4: the damage their ratio.
 - The stage, by the time left's word T: staged T - 78 (1 to the goal, 5
   with the lever in R, 0 from the goal on), its number T - 5C (1 to 4, 5
   the parking bonus after the first stage won, 6 after the third, 9 the
   ending), the money T - 8, the rival there first T + C4 (1),
   the control T + E8 (f32: 1.0 while the player drives). The goal: staged
   0 with 0C3ACB34 still 1 (YOU MADE IT and RESULT, the truck frozen); the
   place 2 when the rival came first, else 1. The parking bonus (bank E,
   after a stage won): driving, then parked when the control drops to 0
   with time left (the time held), failed at T below 0.
 - The rival (Lizard Tail): its task 0C3B11A0 (8000 set), its function
   +10, its work +20 (one of a pool 804 apart: another in the third and
   fourth stages), its trailer's task the next (work at +44); the bodies at
   the works' +19C. The first, third and fourth stages': 0C0A4EE6 waiting
   at its start until the player reaches the route's next block, 0C0A4F5A
   driving; the second's: 0C0ADDF4 running ahead, 0C0ADF72 waiting,
   0C0AE0C6 racing (its distance's field 0). Its tractor's x y z (y off
   for single frames, 3 to 18 m down: the ground under it work + 88, + 0.55
   m, then), its pitch, heading and roll before them (-C, -8, -4, 16 bits a
   turn; 0 facing -z, - nose down, + its right side up), its speed work +
   40 (x 30 the HUD's m/s), its distance to the player work + 190; its
   trailer's heading at -8. The game keeps no gap and compares the trucks
   only at the goals: in the second stage the rival takes its own highway,
   its progress along it work + 48 (x 1.7 m), against the player's
   straight distance to the goal; in the others worked out along the
   player's forward, from its wheels (0C4EA0A0 ...: the list's matrix is
   not always the truck's).
 The other sets (18wheelu, 18wheels, 18wheelro, 18wheelrt): the same values
 at their own addresses (the table below), the same world; 18wheelu's
 steering the game's own clamped to 1 at the lock (the parent's 1.37);
 their second stage's rival run to its race, the third and fourth stages
 not.
 Not found: the rival's bonus (no fixed address: 5000 when the player came
 first), the parking's box, the second stage's goal in RAM (measured).
****************************************************************************/

class Wheeler18 : public Decoder {
public:
	enum Set { PARENT, UPRIGHT, STANDARD, ORIGINAL, REV_T };

	/* Each set's addresses: the parent's above */
	struct Map {
		uint32_t bank, lever, run, left, full, reward, steer, horn;
		uint32_t matrix, tmatrix, pos, speed, rpm, gearbox;
		uint32_t wheel_rr, wheel_lr, wheel_rf, wheel_lf;
		uint32_t node, work, waiting, driving, lead, wait2, race2;
	};

	explicit Wheeler18 (Set set) : L (maps[set]) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t bank = ram.u32 (L.bank);
		bool run = ram.u32 (L.run) == 1, parking = bank == 0x0E;
		int32_t left = ram.i32 (L.left);
		/* The stage's (the player's block, its fields by the time left):
		 * staged, its number, the rival there first, the money */
		bool staged = ram.u32 (L.left - 0x78) != 0;
		bool goal = false, parked = false;
		uint32_t number = ram.u32 (L.left - 0x5C);
		/* The ending's staff roll (the stage 9) */
		if (bank == 0xFFFFFFFF && number == 9)
			out.state = State::OVER;
		else if (bank == 0xFFFFFFFF)
			out.state = state;
		else if (bank == 0x15)
			out.state = State::ATTRACT;
		else if (bank == 4)
			out.state = State::SELECT;
		else if (bank == 7 || bank == 0x12)
			out.state = State::OVER;
		else if (run && !parking && !staged && bank >= 9 && bank <= 0x0C) {
			out.state = State::OVER;
			goal = true;
		} else if (run && parking && left > 0 && ram.f32 (L.left + 0xE8) < 0.5f) {
			/* The truck parked: the control taken back, the time held */
			out.state = State::OVER;
			parked = true;
		} else if (run)
			out.state = left > 0 ? State::RACE : State::OVER;
		else
			out.state = State::SELECT;
		state = out.state;
		out.finished = goal || parked;
		out.transmission = "AT";
		out.time_left = (float) std::max (left, 0) / (float) FPS;
		if (number >= 1 && number <= 6)
			out.stage = (int) number;

		Vehicle &p = out.player;
		float pos[3] = { ram.f32 (L.pos), ram.f32 (L.pos + 4), ram.f32 (L.pos + 8) };
		const uint32_t M = L.matrix, TM = L.tmatrix;
		bool slot = std::fabs (ram.f32 (M + 0x30) - pos[0]) < 0.5f && std::fabs (ram.f32 (M + 0x38) - pos[2]) < 0.5f;
		if (slot)
			for (int i = 0; i < 3; i++) {
				up[i] = ram.f32 (M + 0x10 + 4 * i);
				fwd[i] = -ram.f32 (M + 0x20 + 4 * i);
				trailer[i] = -ram.f32 (TM + 0x20 + 4 * i);
			}
		p.x = -pos[0];
		p.y = pos[1];
		p.z = pos[2];
		/* Another thing in the slot (at a stop, after a load, for a while
		 * in the third stage): the truck's forward from its wheels, level */
		float wf[3];
		for (int i = 0; i < 3; i++)
			wf[i] = (ram.f32 (L.wheel_rf + 4 * i) + ram.f32 (L.wheel_lf + 4 * i) -
			         ram.f32 (L.wheel_rr + 4 * i) - ram.f32 (L.wheel_lr + 4 * i)) / 2;
		float wn = std::sqrt (wf[0] * wf[0] + wf[1] * wf[1] + wf[2] * wf[2]);
		if (!slot && wn > 1.0f)
			orient_angles (p, std::atan2 (-wf[0], wf[2]), std::asin (std::clamp (wf[1] / wn, -1.0f, 1.0f)), 0);
		else
			orient_mirrored (p, up, fwd);
		/* READY: the truck settling on its springs, no hit */
		if (out.state != State::RACE)
			p.impact = 0.0f;
		/* At the goal the physics stop: the truck stands */
		p.speed = goal ? 0.0f : std::fabs (ram.f32 (L.speed));
		p.rpm = ram.f32 (L.rpm);
		uint8_t lever = ram.u8 (L.lever);
		p.gear = lever == 6 ? -1 : lever == 1 ? 2 : 1;
		p.gears = 2;
		p.steer = -std::clamp (ram.f32 (L.steer), -1.0f, 1.0f);
		p.throttle = ram.f32 (L.steer + 8);
		p.brake = ram.f32 (L.steer + 0x10);

		/* The trailer's angle to the tractor, > 0 the cab turned right of
		 * it (the headings in the mirrored world) */
		float a = std::atan2 (fwd[0], fwd[2]) - std::atan2 (trailer[0], trailer[2]);
		while (a > 3.14159265f)
			a -= 6.2831853f;
		while (a < -3.14159265f)
			a += 6.2831853f;
		p.extra.push_back ({ "trailer_angle", -a });
		p.extra.push_back ({ "gearbox", (float) ram.u32 (L.gearbox) });
		p.extra.push_back ({ "horn", (ram.u8 (L.horn) & 4) ? 1.0f : 0.0f });
		int32_t full = ram.i32 (L.full), reward = ram.i32 (L.reward);
		p.extra.push_back ({ "reward", (float) reward });
		if (out.state == State::RACE && full > 0)
			p.damage = std::clamp (1.0f - (float) reward / (float) full, 0.0f, 1.0f);

		p.extra.push_back ({ "money", (float) ram.i32 (L.left - 8) });
		p.extra.push_back ({ "parking", parking ? 1.0f : 0.0f });

		bool stage = bank >= 9 && bank <= 0x0C;
		rival (ram, stage, ram.u32 (L.left + 0xC4) == 1, pos, out);
		/* The result: the rival there first or not */
		if (goal) {
			bool second = ram.u32 (L.left + 0xC4) == 1;
			p.position = second ? 2 : 1;
			if (!out.others.empty ())
				out.others.back ().position = second ? 1 : 2;
		}
		return true;
	}

private:
	/* From the code's literals, each value checked in its set: the data in
	 * blocks shifted alike (18wheelu: +5E0, +5E8, +5F0; 18wheels +400;
	 * 18wheelro and 18wheelrt -1F60, -20C8 the physics, -2098 the rival),
	 * the matrices' buffer 32-byte aligned */
	static constexpr Map maps[5] = {
		{ 0x0C3AC8C4, 0x0C3AC92C, 0x0C3ACB34, 0x0C3ACBB0, 0x0C3ACBC4, 0x0C3ACBCC, 0x0C3ACC80, 0x0C2B4FE5,
		  0x0C289620, 0x0C2895E0, 0x0C4E9F90, 0x0C4E9F60, 0x0C4E9F40, 0x0C4E9F38,
		  0x0C4EA0A0, 0x0C4EA240, 0x0C4EA3E0, 0x0C4EA4B0,
		  0x0C3B11A0, 0x0C3B6644, 0x0C0A4EE6, 0x0C0A4F5A, 0x0C0ADDF4, 0x0C0ADF72, 0x0C0AE0C6 },
		{ 0x0C3ACEAC, 0x0C3ACF14, 0x0C3AD124, 0x0C3AD1A0, 0x0C3AD1B4, 0x0C3AD1BC, 0x0C3AD270, 0x0C2B55CD,
		  0x0C289C00, 0x0C289BC0, 0x0C4EA580, 0x0C4EA550, 0x0C4EA530, 0x0C4EA528,
		  0x0C4EA690, 0x0C4EA830, 0x0C4EA9D0, 0x0C4EAAA0,
		  0x0C3B1790, 0x0C3B6C34, 0x0C0A54BA, 0x0C0A552E, 0x0C0AE3C8, 0x0C0AE546, 0x0C0AE69A },
		{ 0x0C3ACCC4, 0x0C3ACD2C, 0x0C3ACF34, 0x0C3ACFB0, 0x0C3ACFC4, 0x0C3ACFCC, 0x0C3AD080, 0x0C2B53E5,
		  0x0C289A20, 0x0C2899E0, 0x0C4EA390, 0x0C4EA360, 0x0C4EA340, 0x0C4EA338,
		  0x0C4EA4A0, 0x0C4EA640, 0x0C4EA7E0, 0x0C4EA8B0,
		  0x0C3B15A0, 0x0C3B6A44, 0x0C0A52EE, 0x0C0A5362, 0x0C0AE1FC, 0x0C0AE37A, 0x0C0AE4CE },
		{ 0x0C3AA964, 0x0C3AA9CC, 0x0C3AABD4, 0x0C3AAC50, 0x0C3AAC64, 0x0C3AAC6C, 0x0C3AAD20, 0x0C2B3085,
		  0x0C2876C0, 0x0C287680, 0x0C4E7EC8, 0x0C4E7E98, 0x0C4E7E78, 0x0C4E7E70,
		  0x0C4E7FD8, 0x0C4E8178, 0x0C4E8318, 0x0C4E83E8,
		  0x0C3AF108, 0x0C3B45AC, 0x0C0A4DBE, 0x0C0A4E32, 0x0C0ADCCC, 0x0C0ADE4A, 0x0C0ADF9E },
		{ 0x0C3AA964, 0x0C3AA9CC, 0x0C3AABD4, 0x0C3AAC50, 0x0C3AAC64, 0x0C3AAC6C, 0x0C3AAD20, 0x0C2B3085,
		  0x0C2876C0, 0x0C287680, 0x0C4E7EC8, 0x0C4E7E98, 0x0C4E7E78, 0x0C4E7E70,
		  0x0C4E7FD8, 0x0C4E8178, 0x0C4E8318, 0x0C4E83E8,
		  0x0C3AF108, 0x0C3B45AC, 0x0C0A4DCE, 0x0C0A4E42, 0x0C0ADCDC, 0x0C0ADE5A, 0x0C0ADFAE },
	};
	const Map &L;
	State state = State::ATTRACT;
	/* The last matrix found */
	float up[3] = { 0, 1, 0 }, fwd[3] = { 0, 0, 1 }, trailer[3] = { 0, 0, 1 };
	/* The second stage's goal (x z, the game's frame) and the rival's
	 * progress at its zone, measured in the parent's runs */
	static constexpr float GOAL2_X = 1241.4f, GOAL2_Z = 4856.2f, RIVAL2_GOAL = 1398.46f;

	void rival (const Ram &ram, bool stage, bool rival_first, const float pos[3], Frame &out)
	{
		/* Its task's node, function, work; its trailer's the next node.
		 * The work one of the pool's (0x804 apart: another in the fourth
		 * stage), the bodies at its +19C */
		uint32_t node = ram.u32 (L.node), fn = ram.u32 (L.node + 0x10), work = ram.u32 (L.node + 0x20);
		uint32_t twork = ram.u32 (L.node + 0x44);
		bool lead = L.lead && fn == L.lead;
		bool wait2 = L.wait2 && fn == L.wait2;
		bool race2 = L.race2 && fn == L.race2;
		bool driving = fn == L.driving || lead || race2, waiting = fn == L.waiting || wait2;
		bool pooled = work >= L.work && (work - L.work) % 0x804 == 0 && (work - L.work) / 0x804 < 16;
		uint32_t P = ram.u32 (work + 0x19C), T = ram.u32 (twork + 0x19C);
		if (!stage || !(node & 0x8000) || !pooled || (!driving && !waiting) ||
		    !ram.has (P - 0x0C, 0x18) || !ram.has (T - 0x0C, 0x18))
			return;
		const float TURN = 6.2831853f / 65536.0f;
		Vehicle v;
		float x = ram.f32 (P), y = ram.f32 (P + 4), z = ram.f32 (P + 8);
		/* Its height off for single frames (3 to 18 m down): the ground's
		 * under it then */
		float ground = ram.f32 (work + 0x88) + 0.55f;
		if (std::fabs (y - ground) > 2.0f)
			y = ground;
		float yaw = (float) (int16_t) ram.u16 (P - 8) * TURN;
		v.x = -x;
		v.y = y;
		v.z = z;
		orient_angles (v, wrap_angle (3.14159265f - yaw), (float) (int16_t) ram.u16 (P - 0x0C) * TURN,
		               -(float) (int16_t) ram.u16 (P - 4) * TURN);
		v.speed = driving ? 30.0f * ram.f32 (work + 0x40) : 0.0f;
		v.extra.push_back ({ "driving", driving ? 1.0f : 0.0f });
		v.extra.push_back ({ "trailer_angle", wrap_angle ((float) (int16_t) ram.u16 (T - 8) * TURN - yaw) });
		/* Its distance's field the first, third and fourth stages' run
		 * (0 in the second's) */
		v.extra.push_back ({ "distance", fn == L.driving ? ram.f32 (work + 0x190) : NAN });

		Vehicle &p = out.player;
		if (lead || wait2 || race2) {
			/* The second stage: the rival on its own highway, its progress
			 * along it work + 48 (x 1.7 m; 1398.46 at its goal's zone),
			 * against the player's straight distance to the goal (the
			 * game compares nothing before the goals) */
			float rival_left = rival_first ? 0.0f : std::max ((RIVAL2_GOAL - ram.f32 (work + 0x48)) * 1.7f, 0.0f);
			float player_left = std::hypot (pos[0] - GOAL2_X, pos[2] - GOAL2_Z);
			float gap = rival_left - player_left;
			bool first = rival_first || gap < 0;
			if (first)
				p.dist_ahead = -gap;
			else
				p.dist_behind = gap;
			p.position = first ? 2 : 1;
			v.position = first ? 1 : 2;
			p.extra.push_back ({ "gap", gap });
			out.others.push_back (v);
			return;
		}

		/* Ahead of the player or behind it, along its forward from its
		 * wheels (in the game's frame) */
		float fx = (ram.f32 (L.wheel_rf) + ram.f32 (L.wheel_lf) - ram.f32 (L.wheel_rr) - ram.f32 (L.wheel_lr)) / 2;
		float fz = (ram.f32 (L.wheel_rf + 8) + ram.f32 (L.wheel_lf + 8) - ram.f32 (L.wheel_rr + 8) - ram.f32 (L.wheel_lr + 8)) / 2;
		float n = std::hypot (fx, fz);
		if (n > 0.1f) {
			float ahead = ((x - pos[0]) * fx + (z - pos[2]) * fz) / n;
			if (ahead > 0)
				p.dist_ahead = ahead;
			else
				p.dist_behind = -ahead;
			p.position = ahead > 0 ? 2 : 1;
			v.position = ahead > 0 ? 1 : 2;
			p.extra.push_back ({ "gap", -ahead });
		}
		out.others.push_back (v);
	}
};

/****************************************************************************
 Club Kart: European Session (clubkrt, clubk2k3), NAOMI

 Checked in Beginner races on Cote d'Azur Type C (2 laps) and Circuit Type
 B (3), the defaults, no card. A unit is a metre (the kart's size, a lap of
 1496 m); the velocities in units a frame. The world is mirrored (steering
 right lowers its heading): x is turned round. No gearbox.
 - The scene 0C30F300: 3 the attract (replays), 4, 7, 8, A, B the choices,
   C the race's (its phase 0C30F83D: below B the flyover and 3-2-1, from B
   the kart free), 12 Information, 14 GAME OVER, FFFFFFFF between two
   scenes (the state as it was). Over at TIME IS UP (0C319CDC 1) or once
   the player's laps are done.
 - The time left 0C319CD4 (frames); the laps of the race 0C319B2E (a byte).
 - The karts 0C31A49C + k x 94, k 0 the player, 1 to 7 the others:
   accelerator +0, brake +4 (as the game takes them), the velocity +8,
   the speed +14, the matrix +30 (rows 10 apart: right, up, back), x y z
   +60.
 - Their races 0C318B3C + k x 218: the place +8 (from 0), the laps done +C,
   each lap's time +10 + 4 n and the best +70, the race's +74 (ms, the
   game's clock).
 - The player's steering as the game takes it 0C31F0C0 (+1 left; 0C004080,
   a copy on the stack, was off at times).
 clubk2k3 (2003): the same, the scene 0C29B7A0 (15 the kart's choice), the
   phase 0C29C229, the time-up flag 0C2A4AC0, the laps 0C2A4912, the time
   left 0C2A4AB4, the karts 0C2A4FA0, their races 0C2A3780 + k x 22C (laps
   done +204, the laps' times +19C, the best +1FC, the race's +200), the
   steering 0C2AA774.
 Not found: the engine (a value with the speed alone), the wheels, the
 hits.
****************************************************************************/

class ClubKart : public Decoder {
public:
	/* The scene, the race's phase (a byte), the time-up flag, the race's laps
	 * (a byte), the time left, the karts (94 apart), their races (their
	 * stride; in each the laps done, the laps' times, the best, the race's
	 * time; the place at +8), the player's steering */
	struct Map {
		uint32_t scene, phase, timeup, laps, left, karts, races, stride;
		uint32_t done, times, best, total, steer;
	};
	static constexpr Map EUROPEAN = { 0x0C30F300, 0x0C30F83D, 0x0C319CDC, 0x0C319B2E, 0x0C319CD4, 0x0C31A49C,
	                                  0x0C318B3C, 0x218, 0x0C, 0x10, 0x70, 0x74, 0x0C31F0C0 };
	static constexpr Map EUROPEAN_2003 = { 0x0C29B7A0, 0x0C29C229, 0x0C2A4AC0, 0x0C2A4912, 0x0C2A4AB4, 0x0C2A4FA0,
	                                       0x0C2A3780, 0x22C, 0x204, 0x19C, 0x1FC, 0x200, 0x0C2AA774 };

	explicit ClubKart (const Map &map) : L (map) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		const uint32_t K0 = L.karts, R0 = L.races;

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t scene = ram.u32 (L.scene);
		unsigned laps = ram.u8 (L.laps), done = ram.u32 (R0 + L.done);
		bool timeup = ram.u32 (L.timeup) == 1;
		if (scene == 0x0C) {
			if (timeup || (laps && done >= laps))
				out.state = State::OVER;
			else
				out.state = ram.u8 (L.phase) >= 0x0B ? State::RACE : State::SELECT;
		} else if (scene == 0x12 || scene == 0x14)
			out.state = State::OVER;
		else if (scene == 3)
			out.state = State::ATTRACT;
		else if (scene == 0xFFFFFFFF)
			out.state = state;
		else
			out.state = State::SELECT;
		state = out.state;
		out.finished = scene == 0x0C && laps && done >= laps;
		if (scene != 0x0C)
			return true;

		out.laps = (int) laps;
		out.time_left = (float) std::max (ram.i32 (L.left), 0) / (float) FPS;
		uint32_t lap = ram.u32 (R0 + L.times + 4 * std::min (done, 11u));
		if (done < laps && lap != 0xFFFFFFFF)
			out.lap_time = (float) lap / 1000.0f;
		if (done)
			out.last_lap = (float) ram.u32 (R0 + L.times + 4 * std::min (done - 1, 11u)) / 1000.0f;
		if (ram.u32 (R0 + L.best))
			out.best_lap = (float) ram.u32 (R0 + L.best) / 1000.0f;
		out.race_time = (float) ram.u32 (R0 + L.total) / 1000.0f;

		for (unsigned k = 0; k < 8; k++) {
			const uint32_t R = R0 + L.stride * k;
			Vehicle kart;
			kart_record (ram, K0 + 0x94 * k, kart);
			kart.position = (int) ram.u32 (R + 0x08) + 1;
			unsigned kd = ram.u32 (R + L.done);
			kart.lap = (int) (laps ? std::min (kd + 1, laps) : kd + 1);
			if (k == 0) {
				kart.steer = std::clamp (ram.f32 (L.steer), -1.0f, 1.0f);
				out.player = kart;
			} else
				out.others.push_back (kart);
		}
		return true;
	}

	/* A kart's record: the pedals as the game takes them (the others'
	 * accelerator below 0 at times), its velocity, speed, matrix, x y z */
	static void kart_record (const Ram &ram, uint32_t K, Vehicle &kart)
	{
		const float V = (float) FPS;	/* m/s a unit a frame */
		float up[3] = { ram.f32 (K + 0x40), ram.f32 (K + 0x44), ram.f32 (K + 0x48) };
		float fwd[3] = { -ram.f32 (K + 0x50), -ram.f32 (K + 0x54), -ram.f32 (K + 0x58) };

		kart.x = -ram.f32 (K + 0x60);
		kart.y = ram.f32 (K + 0x64);
		kart.z = ram.f32 (K + 0x68);
		orient_mirrored (kart, up, fwd);
		kart.vx = -ram.f32 (K + 0x08) * V;
		kart.vy = ram.f32 (K + 0x0C) * V;
		kart.vz = ram.f32 (K + 0x10) * V;
		kart.has_velocity = true;
		kart.speed = ram.f32 (K + 0x14) * V;
		kart.throttle = std::clamp (ram.f32 (K + 0x00), 0.0f, 1.0f);
		kart.brake = std::clamp (ram.f32 (K + 0x04), 0.0f, 1.0f);
	}

private:
	const Map &L;
	State state = State::ATTRACT;
};

/****************************************************************************
 Club Kart Prize (clubkprz, clubkpzb), NAOMI

 A medal game: one kart, one lap against the clock, coins to pick up.
 Checked on INTERMEDIATE, one lap each, a TIME UP (clubkprz); clubkpzb's
 addresses are clubkprz's + 28F80 (its scene + 28F00). The kart's record
 as Club Kart's, at its pointer + 1710 (0 outside the race's scene).
 - The scene: 3 the attract (its HOW TO PLAY drives a kart, no pointer),
   18 the course's choice, 19 loading, 1A the race (its phase from B the
   kart free), 1B the All or Nothing stage's loading, 1C that stage (its
   kart not found), 1E GAME OVER; FFFFFFFF between two (the state as it
   was). The race's state: 0 before the start, 1 racing, 2 TIME UP, 3
   GOAL, 4 the time bonus, 5 done.
 - The time left (frames), the time-up flag, the lap's time (ms, the
   race's), the money (pence, the HUD's), the player's steering (+1 left).
 - The All or Nothing stage (1C): its kart at another pointer + 1710 (0 in
   the race; kept after the stage, its record then garbage: read in 1C
   alone), driven by the game but for the one brake; its state (0 the
   countdown, 1 running, 2 won, 3 lost, 8 the result, A away), the run's
   word (1 stopped or fallen, then 2 won or 3 lost: the verdict kept),
   fallen into the sea. 1D the ranking's name entry after a win.
 - The hopper board's meters as the game keeps them (pence): the credit,
   the won and the paid, those two not cleared at a new game (a change, not
   a level). The bonus stage's words and these not at the race's shift in
   clubkpzb: each found on its own.
****************************************************************************/

class ClubKartPrize : public Decoder {
public:
	/* The scene, then the race's (all at the same shift from a set to the
	 * other): the phase, the time-up flag, the time left, the kart's
	 * pointer, the race's state, the lap's time, the money; the steering;
	 * the bonus stage's kart pointer, state, run over, fallen; the meters
	 * (credit, won + 10, paid + 14) */
	struct Map {
		uint32_t scene, phase, timeup, left, kart, race, lap, money, steer;
		uint32_t bonus_kart, bonus_state, bonus_over, bonus_fell, meters;
	};
	static constexpr Map PRIZE = { 0x0C2E10A0, 0x0C16FDE0, 0x0C170034, 0x0C170030, 0x0C16F7A4,
	                               0x0C16F768, 0x0C16F830, 0x0C16FD7C, 0x0C2EE064,
	                               0x0C16F61C, 0x0C16BE2C, 0x0C16BDF4, 0x0C16B81C, 0x0C5B0328 };
	static constexpr Map PRIZE_B = { 0x0C309FA0, 0x0C198D60, 0x0C198FB4, 0x0C198FB0, 0x0C198724,
	                                 0x0C1986E8, 0x0C1987B0, 0x0C198CFC, 0x0C316F48,
	                                 0x0C1985B0, 0x0C194DC0, 0x0C194D88, 0x0C194794, 0x0DBB20D8 };

	explicit ClubKartPrize (const Map &map) : L (map) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t scene = ram.u32 (L.scene), race = ram.u32 (L.race);
		bool timeup = ram.u32 (L.timeup) == 1;
		uint32_t bonus = ram.u32 (L.bonus_state);
		if (scene == 0x1A) {
			if (timeup || race >= 2)
				out.state = State::OVER;
			else
				out.state = ram.u32 (L.phase) >= 0x0B && race == 1 ? State::RACE : State::SELECT;
		} else if (scene == 0x1C) {
			bool stopped = ram.u32 (L.bonus_over) != 0 || ram.u32 (L.bonus_fell) != 0;
			out.state = bonus == 0 ? State::SELECT : bonus == 1 && !stopped ? State::RACE : State::OVER;
		} else if (scene == 0x1B || scene == 0x1D || scene == 0x1E)
			out.state = State::OVER;
		else if (scene == 3)
			out.state = State::ATTRACT;
		else if (scene == 0xFFFFFFFF)
			out.state = state;
		else
			out.state = State::SELECT;
		state = out.state;
		/* The bonus stage's verdict in its run's word: 2 won, 3 lost (the
		 * state then on to its result, 8, and away, A) */
		uint32_t verdict = scene == 0x1C ? ram.u32 (L.bonus_over) : 0;
		out.finished = (scene == 0x1A && race >= 3) || verdict == 2;
		Vehicle &p = out.player;
		p.extra.push_back ({ "credit", (float) ram.i32 (L.meters) });
		p.extra.push_back ({ "won", (float) ram.i32 (L.meters + 0x10) });
		p.extra.push_back ({ "paid", (float) ram.i32 (L.meters + 0x14) });
		if (scene == 0x1C) {
			/* All or Nothing: the kart the game drives, the brake the
			 * player's */
			uint32_t K = ram.u32 (L.bonus_kart) + 0x1710;
			if (ram.u32 (L.bonus_kart) && ram.has (K, 0x94))
				ClubKart::kart_record (ram, K, p);
			p.steer = std::clamp (ram.f32 (L.steer), -1.0f, 1.0f);
			p.extra.push_back ({ "bonus", 1.0f });
			p.extra.push_back ({ "fell", ram.u32 (L.bonus_fell) ? 1.0f : 0.0f });
			p.extra.push_back ({ "bonus_won", verdict == 2 ? 1.0f : verdict == 3 ? 0.0f : NAN });
			return true;
		}
		if (scene != 0x1A)
			return true;

		out.laps = 1;
		out.time_left = (float) std::max (ram.i32 (L.left), 0) / (float) FPS;
		uint32_t lap = ram.u32 (L.lap);
		if (lap != 0xFFFFFFFF) {
			out.lap_time = (float) lap / 1000.0f;
			out.race_time = out.lap_time;
		}
		uint32_t K = ram.u32 (L.kart) + 0x1710;
		if (ram.u32 (L.kart) && ram.has (K, 0x94))
			ClubKart::kart_record (ram, K, p);
		p.position = 1;
		p.lap = 1;
		p.steer = std::clamp (ram.f32 (L.steer), -1.0f, 1.0f);
		p.extra.push_back ({ "money", (float) ram.i32 (L.money) });
		return true;
	}

private:
	const Map &L;
	State state = State::ATTRACT;
};

/****************************************************************************
 Initial D Arcade Stage (initdexp, initdv3e), NAOMI 2 GD-ROM

 Checked in Legend of the Streets with the AE86, AT and MT, on Myogi (3
 laps, a race won, one lost, a TIME IS UP), Usui and Happogahara, no card,
 the defaults. A unit is a metre. The world is mirrored (steering right
 lowers its heading): x is turned round. No reverse gear.
 - The scene 0C229310: 0 the attract (the replay after GAME OVER too), 1
   the menus and RESULT, 2 the story and CONTINUE?, 3 the race's;
   0C2092BC 1 from the story before the race to the attract after GAME
   OVER (and through the loading's last frames: after a choice, still the
   choice).
 - The race's objects, on the heap, from the anchor A = [0C7D9F4C]: the
   HUD's H = [A + 4F4] (its handler 0C23EAFC at +0), the timer's T = [A +
   410] (0C23923C): the phase A - 34C (2 from GO to the finish), the
   countdown A - 2CC (2 in 3-2-1); the time left T + 8, the place H + 8 (from
   0), the race's time H + 1C and each lap's H + 20 + 4 k (1/6000 s, -1 not
   begun), the lap's fraction H + 54 (the rival's + 58), the gap to the
   rival H + 5C (m, > 0 ahead).
 - The car 0C3017C0: x y z, its matrix +1E0 (rows 10 apart: right, up,
   back); the rival's the same at +270. The car's wheels' speed 0C7DBE94,
   the gear 0C7DBC0C (0 before GO, 1 to 5), rpm 0C7DBC1C; steering (+ left),
   accelerator and brake as the game takes them 0C7DBE28, 18, 24; MT
   0C228CD8 1; the course 0C228CA0 (0 Myogi, 1 Usui, 3 Happogahara).
 initdv3e (Ver. 3 Export): the same objects at others: the scene
 0C31DE58 (4 the race's), raced 0C2F4E24, the anchor [0C8FF174], H = [A +
 4FC] (0C3873C4), T = [A + 418] (0C38143C), the HUD's fractions and gap 8
 later (H + 5C, 60, 64), the race's end 0C31DE54 (100 from the finish or
 the time up to the next race: OVER while the HUD is freed); the car
 0C401B14, speed 0C900C2C, gear 0C900E88, rpm 0C900EA0, steering,
 accelerator and brake 0C900BB8, A8, B4; MT 0C31C9E0; the course 0C31C9A0
 (0 Myogi, 5 Irohazaka, the others not seen; set once confirmed).
 Not found: the race's laps (the course's screen alone), a hit flag, the
 wheels' grounds (0C7DBD00 + 4 k: ids not named).
****************************************************************************/

class InitialD : public Decoder {
public:
	/* The scene and the race's, the raced flag, the anchor's pointer, the
	 * HUD's and the timer's slots and handlers, the HUD's fractions, the
	 * race's end (0 none), the car, its speed, gear, rpm, steering (the
	 * pedals -10 and -4), MT, the course, the courses' names */
	struct Map {
		uint32_t scene, race_scene, raced, anchor, hud, timer, hud_handler, timer_handler, fractions, ended;
		uint32_t car, speed, gear, rpm, steer, mt, course;
		const char *courses[6];
	};
	static constexpr Map EXPORT = { 0x0C229310, 3, 0x0C2092BC, 0x0C7D9F4C, 0x4F4, 0x410, 0x0C23EAFC, 0x0C23923C, 0x54, 0,
	                                0x0C3017C0, 0x0C7DBE94, 0x0C7DBC0C, 0x0C7DBC1C, 0x0C7DBE28, 0x0C228CD8, 0x0C228CA0,
	                                { "Myogi", "Usui", "", "Happogahara", "", "" } };
	static constexpr Map V3_EXPORT = { 0x0C31DE58, 4, 0x0C2F4E24, 0x0C8FF174, 0x4FC, 0x418, 0x0C3873C4, 0x0C38143C, 0x5C,
	                                   0x0C31DE54, 0x0C401B14, 0x0C900C2C, 0x0C900E88, 0x0C900EA0, 0x0C900BB8, 0x0C31C9E0,
	                                   0x0C31C9A0, { "Myogi", "", "", "", "", "Irohazaka" } };

	explicit InitialD (const Map &map) : L (map) {}

	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		const float TICK = 6000.0f;

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t scene = ram.u32 (L.scene), raced = ram.u32 (L.raced);
		const uint32_t RACE = L.race_scene;
		uint32_t A = ram.u32 (L.anchor), H = 0, T = 0;
		if (scene == RACE && ram.has (A - 0x400, 0x900)) {
			uint32_t h = ram.u32 (A + L.hud), t = ram.u32 (A + L.timer);
			if (ram.has (h, 0x70) && ram.u32 (h) == L.hud_handler && ram.has (t, 0x10) && ram.u32 (t) == L.timer_handler) {
				H = h;
				T = t;
			}
		}
		uint32_t total = H ? ram.u32 (H + 0x1C) : 0;
		if (scene == 0)
			out.state = State::ATTRACT;
		else if (scene == RACE && H && ram.u32 (A - 0x34C) == 2)
			out.state = State::RACE;
		else if (scene == RACE && H && ram.u32 (A - 0x2CC) == 2)
			out.state = State::SELECT;
		else if (scene == RACE && H && total > 0)
			out.state = State::OVER;
		/* Ver. 3: between the HUD freed and RESULT, the race's end word */
		else if (scene == RACE && L.ended && ram.u32 (L.ended) == 0x100)
			out.state = State::OVER;
		else if (scene == RACE)
			out.state = State::SELECT;
		else if (raced == 1 && state != State::SELECT)
			out.state = State::OVER;
		else
			out.state = State::SELECT;
		state = out.state;
		/* The finish seen in the race's last frames (not a time up) */
		if (out.state == State::RACE)
			finish = false;
		out.finished = out.state == State::OVER && finish;
		out.transmission = ram.u32 (L.mt) == 1 ? "MT" : "AT";
		uint32_t course = ram.u32 (L.course);
		out.track = course < 6 ? L.courses[course] : "";

		const uint32_t B = L.car;
		Vehicle &p = out.player;
		car (ram, B, p);
		p.speed = ram.f32 (L.speed);
		p.rpm = std::max (ram.f32 (L.rpm), 0.0f);
		uint32_t gear = ram.u32 (L.gear);
		p.gear = gear <= 5 ? (int) gear : UNKNOWN;
		p.gears = 5;
		p.steer = std::clamp (ram.f32 (L.steer), -1.0f, 1.0f);
		p.throttle = ram.f32 (L.steer - 0x10);
		p.brake = ram.f32 (L.steer - 4);
		Vehicle rival;
		car (ram, B + 0x270, rival);
		if (!H) {
			out.others.push_back (rival);
			return true;
		}

		out.time_left = (float) std::max (ram.i32 (T + 8), 0) / TICK;
		/* The phase ends with time left at the finish, none at a time up */
		if (out.state == State::OVER) {
			finish = out.time_left > 0;
			out.finished = finish;
		}
		out.race_time = (float) total / TICK;
		/* The laps begun; the one running, the last, the best of those done */
		int32_t laps[4];
		int lap = 0;
		while (lap < 4 && (laps[lap] = ram.i32 (H + 0x20 + 4 * lap)) >= 0)
			lap++;
		if (lap)
			out.lap_time = (float) laps[lap - 1] / TICK;
		/* At the finish every lap begun is done */
		int complete = out.finished ? lap : lap - 1;
		for (int k = 0; k < complete; k++) {
			out.last_lap = (float) laps[k] / TICK;
			out.best_lap = std::isnan (out.best_lap) ? out.last_lap : std::min (out.best_lap, out.last_lap);
		}
		p.lap = lap;
		p.position = (int) ram.u32 (H + 8) + 1;
		const uint32_t F = H + L.fractions;
		/* The grid is behind the line: the fraction is the lap's end until
		 * the car crosses it (no car goes half a lap in 10 s); at the
		 * finish it is back to 0 with the last lap done */
		float f = ram.f32 (F);
		if (lap == 1 && f > 0.5f && out.race_time < 10.0f)
			f -= 1.0f;
		if (out.finished && f < 0.5f)
			f += 1.0f;
		p.progress = lap ? (float) (lap - 1) + f : 0.0f;
		rival.position = 3 - p.position;
		/* The rival's fraction of its own lap: within half a lap of the
		 * player's */
		float d = ram.f32 (F + 4) - ram.f32 (F);
		d -= std::floor (d + 0.5f);
		rival.progress = lap ? p.progress + d : 0.0f;
		float gap = ram.f32 (F + 8);
		if (gap >= 0)
			p.dist_behind = gap;
		else
			p.dist_ahead = -gap;
		p.extra.push_back ({ "gap", gap });
		out.others.push_back (rival);
		return true;
	}

private:
	/* A car's position and matrix (rows right, up, back) */
	static void car (const Ram &ram, uint32_t at, Vehicle &v)
	{
		const uint32_t M = at + 0x1E0;
		float up[3] = { ram.f32 (M + 0x10), ram.f32 (M + 0x14), ram.f32 (M + 0x18) };
		float fwd[3] = { -ram.f32 (M + 0x20), -ram.f32 (M + 0x24), -ram.f32 (M + 0x28) };
		v.x = -ram.f32 (at);
		v.y = ram.f32 (at + 4);
		v.z = ram.f32 (at + 8);
		orient_mirrored (v, up, fwd);
	}

	const Map &L;
	State state = State::ATTRACT;
	bool finish = false;
};

/****************************************************************************
 Tokyo Bus Guide (tokyobus), NAOMI

 Checked on the first course (Nakanohashi to Shinjuku): the terminal, the
 practice part to the first stop, the scored part to the second, the
 junctions and their lights, contacts, a time up, the attract's demo. A unit
 is a metre; the game runs at 30 Hz, its speed in metres a tick. The world
 is mirrored (steering right lowers its heading): x is turned round.
 - The points 0C244F94 (the HUD's bar, 100 at the start and again after the
   practice, which ends at the first stop): each penalty takes its amount,
   3 a second once late; at 0 the game is over.
 - The state: 0C1D8140 1 the attract (its demo drives the bus); the scene
   0C1D6C48 A the course's choice, 3 the route's map before the first stop
   (the stops served 0C2450E0 at 0), 5 the terminal and the practice, 4 the
   scored part; the bus's sequence 0C244F88: 2 driving (the red lights
   too), 7 off the route (the game puts the bus back), 3, 4, 5 a stop
   served (the route's map between), 0 the doors and the people boarding
   (at the terminal before the drive); 4 and 5 with no points left the time
   up, GAME OVER, HI SCORE.
 - A stop is served stopped within 5 m of its point 0C2452D8 (x y z; the
   distance word below is only written within some 110 m); the bus's state
   0C1D8500: 1 driving, 2 a contact (8 to 18 frames), 3 stopped at a stop
   served, 0 the doors and boarding. The people carried 0C1D8160; the
   game's winker 0C1D84A8 (0 off, 1 left, 2 right: the switches step it);
   the mistakes 0C1D8170 (a penalty, or 3 points late); the newest
   penalty's message 0C244F28 + 4 (an entry of the table 0C06A438 + 4: 0 to
   2 a contact, 6 to 8 the wrong course, 15 no winker, 16 a red light run,
   17 stopped past the line, 23 a sudden start, 32 no announcement).
 - The bus: the driver's eye 0C1D8180 (x y z, 2 m above the road), the
   body's matrix 0C1D82D0 (rows 10 apart: right, up, back; the yaw alone),
   the speed 0C1D84C8 (x 30 m/s), rpm 0C1D8534, the gear 0C1D8540 (0 to 2
   the first to the third, 5 reverse).
 - The inputs as read: steering, accelerator, brake 0C173F60, 62, 64 (16
   bits: 8000 the centre, FEFF right or down); the switches 0C254CDF (02
   the horn, 04 DOOR CLOSE, 08 ANNOUNCE, 10 VIEW) and 0C254CE1 (01 and 02
   the winkers, left and right).
 - The doors 0C1D860C (0 closed, 1 opening, 2 open, 3 closing); the clock
   0C244FA0 and the time due at the next stop 0C244F9C (ticks since
   midnight), late 0C245028; the distance to the next stop 0C244FB0 (m,
   written within some 110 m only, the last stop's kept beyond); the
   people waiting at the stop 0C24516C; the penalties shown 0C244FB4.
 Not found: the route's end (22 stops), the people getting off, the other
 courses, the traffic light ahead (each junction's own).
****************************************************************************/

class TokyoBus : public Decoder {
public:
	bool decode (Machine *mach, uint64_t frame, Frame &out) override
	{
		Ram ram (mach, "ram", 0x0C000000);
		const float TICK = 30.0f;	/* the game's ticks a second */

		(void) frame;
		if (!ram.ok ())
			return false;
		uint32_t seq = ram.u32 (0x0C244F88), scene = ram.u32 (0x0C1D6C48);
		uint32_t points = ram.u32 (0x0C244F94), served = ram.u32 (0x0C2450E0);
		if (ram.u32 (0x0C1D8140) == 1)
			out.state = State::ATTRACT;
		/* The course's choice and the route's map: the demo's sequence
		 * still there */
		else if (scene == 0x0A || (scene == 3 && served == 0))
			out.state = State::SELECT;
		else if ((seq == 4 || seq == 5) && points == 0)
			out.state = State::OVER;
		/* Driving, and the stops served (their route's map, the boarding) */
		else if ((points > 0 && (seq == 2 || seq == 3 || seq == 4 || seq == 5 || seq == 7)) ||
		         (seq == 0 && scene == 4))
			out.state = State::RACE;
		else
			out.state = State::SELECT;
		out.transmission = "AT";

		Vehicle &p = out.player;
		const uint32_t M = 0x0C1D82D0;
		float up[3] = { ram.f32 (M + 0x10), ram.f32 (M + 0x14), ram.f32 (M + 0x18) };
		float fwd[3] = { -ram.f32 (M + 0x20), -ram.f32 (M + 0x24), -ram.f32 (M + 0x28) };
		p.x = -ram.f32 (0x0C1D8180);
		p.y = ram.f32 (0x0C1D8184);
		p.z = ram.f32 (0x0C1D8188);
		orient_mirrored (p, up, fwd);
		p.speed = std::fabs (ram.f32 (0x0C1D84C8)) * TICK;
		p.rpm = ram.f32 (0x0C1D8534);
		uint32_t gear = ram.u32 (0x0C1D8540);
		p.gear = gear == 5 ? -1 : gear <= 3 ? (int) gear + 1 : UNKNOWN;
		p.steer = -std::clamp (((float) ram.u16 (0x0C173F60) - 32768.0f) / 32511.0f, -1.0f, 1.0f);
		p.throttle = std::clamp (ram.u16 (0x0C173F62) / 65279.0f, 0.0f, 1.0f);
		p.brake = std::clamp (ram.u16 (0x0C173F64) / 65279.0f, 0.0f, 1.0f);
		/* Put back on the route: no hit */
		if (seq == 7)
			p.impact = 0.0f;

		uint8_t sw1 = ram.u8 (0x0C254CDF);
		uint32_t winker = ram.u32 (0x0C1D84A8), bus = ram.u32 (0x0C1D8500);
		p.extra.push_back ({ "horn", (sw1 & 0x02) ? 1.0f : 0.0f });
		p.extra.push_back ({ "winker_left", winker == 1 ? 1.0f : 0.0f });
		p.extra.push_back ({ "winker_right", winker == 2 ? 1.0f : 0.0f });
		p.extra.push_back ({ "doors", (float) ram.u32 (0x0C1D860C) });
		p.extra.push_back ({ "off_route", seq == 7 ? 1.0f : 0.0f });
		if (out.state != State::RACE && out.state != State::OVER)
			return true;
		p.extra.push_back ({ "points", (float) points });
		p.extra.push_back ({ "stops", (float) served });
		p.extra.push_back ({ "passengers", (float) ram.u32 (0x0C1D8160) });
		p.extra.push_back ({ "mistakes", (float) ram.u32 (0x0C1D8170) });
		/* The newest penalty's message, by its entry in the table */
		uint32_t msg = ram.u32 (0x0C244F28 + 4);
		float penalty = NAN;
		for (int i = 0; msg && i < 34; i++)
			if (ram.u32 (0x0C06A438 + 4 * i) + 4 == msg)
				penalty = (float) i;
		p.extra.push_back ({ "last_penalty", penalty });
		if (out.state != State::RACE)
			return true;
		bool contact = bus == 2;
		p.extra.push_back ({ "contact", contact ? 1.0f : 0.0f });
		p.extra.push_back ({ "at_stop", bus == 0 || bus == 3 ? 1.0f : 0.0f });
		/* The speed jumps for a tick at a contact */
		if (contact)
			p.speed = last_speed;
		last_speed = p.speed;
		/* The timetable: the time to the next stop's (< 0 late) */
		int32_t now = ram.i32 (0x0C244FA0), due = ram.i32 (0x0C244F9C);
		p.extra.push_back ({ "clock", (float) now / TICK });
		p.extra.push_back ({ "schedule", (float) (due - now) / TICK });
		p.extra.push_back ({ "late", ram.u32 (0x0C245028) ? 1.0f : 0.0f });
		/* The distance to the next stop's point, at any range (the
		 * game's word only within some 110 m) */
		float sx = ram.f32 (0x0C2452D8), sz = ram.f32 (0x0C2452E0);
		float stop = sx != 0.0f || sz != 0.0f ?
		             std::hypot (ram.f32 (0x0C1D8180) - sx, ram.f32 (0x0C1D8188) - sz) : NAN;
		p.extra.push_back ({ "distance_to_stop", stop });
		p.extra.push_back ({ "waiting", (float) ram.u32 (0x0C24516C) });
		p.extra.push_back ({ "penalties", (float) ram.u32 (0x0C244FB4) });
		return true;
	}

private:
	float last_speed = 0.0f;
};

} /* namespace */

std::unique_ptr<Decoder>
make_decoder (Machine *mach)
{
	const char *f = family (mach);

	if (!strcmp (f, "sgnascar"))
		return std::unique_ptr<Decoder> (new Nascar);
	if (!strcmp (f, "swracer"))
		return std::unique_ptr<Decoder> (new SwRacer);
	if (!strcmp (f, "atvtrack") || !strcmp (f, "smashdrv"))
		return std::unique_ptr<Decoder> (new Gaelco (!strcmp (f, "atvtrack")));
	if (!strcmp (f, "crzytaxi"))
		return std::unique_ptr<Decoder> (new CrazyTaxi);
	if (!strcmp (f, "tokyobus"))
		return std::unique_ptr<Decoder> (new TokyoBus);
	if (!strcmp (f, "jambo"))
		return std::unique_ptr<Decoder> (new Jambo);
	/* The sets themselves where their clones' maps are others */
	const char *name = mach->game ? mach->game->name : "";
	if (!strcmp (name, "kingrt66"))
		return std::unique_ptr<Decoder> (new KingRt66 (KingRt66::RELEASE));
	if (!strcmp (name, "kingrt66p"))
		return std::unique_ptr<Decoder> (new KingRt66 (KingRt66::PROTOTYPE));
	if (!strcmp (name, "f355twin"))
		return std::unique_ptr<Decoder> (new F355 (F355::TWIN));
	if (!strcmp (name, "f355twn2"))
		return std::unique_ptr<Decoder> (new F355 (F355::TWIN2));
	if (!strcmp (name, "f355"))
		return std::unique_ptr<Decoder> (new F355 (F355::DELUXE));
	static const struct {
		const char *name;
		Wheeler18::Set set;
	} wheelers[] = {
		{ "18wheelr", Wheeler18::PARENT }, { "18wheelu", Wheeler18::UPRIGHT }, { "18wheels", Wheeler18::STANDARD },
		{ "18wheelro", Wheeler18::ORIGINAL }, { "18wheelrt", Wheeler18::REV_T },
	};
	for (const auto &w : wheelers)
		if (!strcmp (name, w.name))
			return std::unique_ptr<Decoder> (new Wheeler18 (w.set));
	if (!strcmp (name, "clubkrt"))
		return std::unique_ptr<Decoder> (new ClubKart (ClubKart::EUROPEAN));
	if (!strcmp (name, "clubk2k3"))
		return std::unique_ptr<Decoder> (new ClubKart (ClubKart::EUROPEAN_2003));
	if (!strcmp (name, "clubkprz"))
		return std::unique_ptr<Decoder> (new ClubKartPrize (ClubKartPrize::PRIZE));
	if (!strcmp (name, "clubkpzb"))
		return std::unique_ptr<Decoder> (new ClubKartPrize (ClubKartPrize::PRIZE_B));
	if (!strcmp (name, "initdexp"))
		return std::unique_ptr<Decoder> (new InitialD (InitialD::EXPORT));
	if (!strcmp (name, "initdv3e"))
		return std::unique_ptr<Decoder> (new InitialD (InitialD::V3_EXPORT));
	return NULL;
}

} /* namespace telemetry */
} /* namespace hl */
