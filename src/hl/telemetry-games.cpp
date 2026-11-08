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
 * display. Addresses are the master SH-4's (its RAM at 0C000000). */

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
	return NULL;
}

} /* namespace telemetry */
} /* namespace hl */
