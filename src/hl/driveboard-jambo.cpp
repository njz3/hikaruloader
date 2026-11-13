/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * What EPR-19338A does with each command, from its disassembly in
 * BackForceFeeder's docs (docs/EPR-19338A-ScudRace-DriveBoard), and the
 * values its Z80 writes, measured here (tests/driveboard_z80_test.cpp,
 * DRIVEBOARD_LIST).
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

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hl/core.h"
#include "hl/driveboard.h"
#include "hl/machine.h"
#include "hl/state.h"

namespace hl {

/*
 * Jambo! Safari's Drive Board
 * ===========================
 *
 * The deluxe cabinet's wheel (GAME ASSIGNMENTS, CABINET TYPE DX, the
 * default): a Model 3 drive board and its servo board, wired to the 837-13844
 * I/O board as F355 Challenge's (the command on outputs 15 to 22, the reply
 * on players 1's and 2's switches). Its EPROM is not known. Its commands are
 * those of Scud Race's board, whose program (EPR-19338A, Dirt Devils' too)
 * runs on the Z80 when the scud set is there (DriveBoardZ80).
 *
 * Without that EPROM, this simulation writes the servo board's registers as
 * EPR-19338A does, its values measured on its Z80, the wheel at its centre;
 * the DriveBoard turns them into forces as it does the Z80's. In the game
 * (C7), n the command's bits 0-2 (0-3 for 2x):
 *
 *  - 00, 08  the road effect stops
 *  - 01-07   road effect n, looped until the next 0x, added to the torque
 *            (FE at each NMI): 01, 03 a swing of 10 steps at some 21 Hz,
 *            04 of 10 at 23 Hz, 05 of 6 at 12 Hz, 06 and 07 of 11 at
 *            8.6 Hz, 11 steps to the left and to the right of the centre; 02
 *            bumps: a torque of one NMI at the end of the scale, then a
 *            ramp of 74 ms, at random intervals. The program's own shapes
 *            are in its tables: sines here
 *  - 10-17   the spring n: FE 80, FD (24, 25, 26, 27, 29, 2C, 2F, 4A), FC 4
 *            (2C + 8n: the servo board's spring, 5 to 12 of 15)
 *  - 20-2F   FB = 87 + 8n, the gain (the MOTOR POWER's curve at 100 %)
 *  - 50-57, 60-67  a push right (FE 80 - (21 + 2n)), left (80 + 21 + 2n),
 *            FD 00, held, the road effect off, until the next 1x
 *  - 90-9F   the motor idle (FF) for 60 to 90 ms, then the spring again
 *
 * and the modes: C1, C4, C8, C9 the motor free (FF); C6 a shake of 0.8 s
 * (a swing of 16 steps, 10 Hz), then FE 80; C7 the game (no vibration,
 * FB FF, FE 80, the spring 0); CA the test (80 FE 80, 81 FE 69 to the
 * right, 82 FE 97 to the left); CB the start-up again. EPR-19338A's
 * torque by the wheel's position, its tables', is left out: the servo
 * board's spring (FC 4) alone. The 3x (the servo board's own vibration) and
 * 4x (a power slide) are not played; Jambo! Safari sends 30 alone.
 *
 * The game sends C1 00 30 10 20 08 C1 as it starts, then in a ride 25 and
 * 11 by turns, 20 and 22 roped (a lower gain), 5x and 6x the rope's pull,
 * 5A 6A at a hit, 01 and 04 the ground changing (water, sand, grass), 0A
 * after START!, C6, C7, C9 its scenes. With the reply lines idle (FF), as
 * here, it plays and its tests run.
 */

enum {
	MODE_FREE,
	MODE_SHAKE,
	MODE_GAME,
	MODE_TEST
};

/* The servo board's registers */
#define SERVO_FC	0xFC
#define SERVO_FD	0xFD
#define SERVO_FE	0xFE
#define SERVO_FF	0xFF
#define SERVO_FB	0xFB

/* NMIs: C6's shake; the 9x idle, 60 ms and 10 more by step; a bump's ramp */
#define SHAKE_NMIS	400
#define IDLE_NMIS	30
#define RAMP_NMIS	37

/* The springs' FD, by n */
static const uint8_t spring_fd[8] = { 0x24, 0x25, 0x26, 0x27, 0x29, 0x2C, 0x2F, 0x4A };

/* The road effects 1 to 7 but 2: a swing's half amplitude (steps), its
 * period (NMIs), its offset (steps, > 0 to the left) */
static const struct {
	float	 amplitude, period, offset;
} road[8] = {
	{ 0, 0, 0 }, { 10, 24, 0 }, { 0, 0, 0 }, { 10, 24, 0 },
	{ 10, 22, 0 }, { 6, 41, 0 }, { 11, 58, 11 }, { 11, 58, -11 },
};

DriveBoardJambo::DriveBoardJambo (Machine *mach) :
	mach (mach)
{
	reset ();
}

void
DriveBoardJambo::reset ()
{
	memset (&s, 0, sizeof (s));
	s.last_command = 0xFF;
	s.mode = MODE_FREE;
	s.random = 0x1D872B41;
}

/* The next interval between two bumps, some 60 to 500 ms */
unsigned
DriveBoardJambo::next_bump ()
{
	s.random = s.random * 1103515245u + 12345u;
	return 30 + (s.random >> 16) % 220;
}

void
DriveBoardJambo::spring (Servo &servo)
{
	servo.servo_write (SERVO_FE, 0x80);
	servo.servo_write (SERVO_FD, spring_fd[s.spring & 7]);
	servo.servo_write (SERVO_FC, (uint8_t) (0x2C + 8 * (s.spring & 7)));
}

void
DriveBoardJambo::execute (uint8_t cmd, Servo &servo)
{
	uint8_t n = cmd & 7;

	switch (cmd >> 4) {
	case 0xC:
		switch (cmd & 0x0F) {
		case 0x6:
			s.mode = MODE_SHAKE;
			s.nmis = 0;
			return;
		case 0x7:
			s.mode = MODE_GAME;
			s.road = 0;
			s.push = 0;
			s.idle_nmis = 0;
			s.spring = 0;
			servo.servo_write (SERVO_FC, 0x0A);
			servo.servo_write (SERVO_FC, 0x0B);
			servo.servo_write (SERVO_FB, 0xFF);
			spring (servo);
			return;
		case 0xA:
			s.mode = MODE_TEST;
			servo.servo_write (SERVO_FE, 0x80);
			return;
		case 0xB:
		case 0xC:
		case 0xD:
		case 0xE:
		case 0xF:
			reset ();
			s.last_command = cmd;
			servo.servo_write (SERVO_FF, 0xFF);
			return;
		default:
			s.mode = MODE_FREE;
			servo.servo_write (SERVO_FF, 0xFF);
			return;
		}
	case 0x8:
		if (s.mode == MODE_TEST)
			servo.servo_write (SERVO_FE, n == 1 ? 0x69 : n == 2 ? 0x97 : 0x80);
		return;
	default:
		break;
	}
	if (s.mode != MODE_GAME)
		return;
	switch (cmd >> 4) {
	case 0x0:
		s.road = n;
		s.nmis = 0;
		s.bump_wait = 0;
		break;
	case 0x1:
		s.spring = n;
		s.push = 0;
		s.idle_nmis = 0;
		spring (servo);
		break;
	case 0x2:
		servo.servo_write (SERVO_FB, (uint8_t) (0x87 + 8 * (cmd & 0x0F)));
		break;
	case 0x5:
	case 0x6:
		s.push = (uint8_t) ((cmd >> 4) == 5 ? 0x80 - (21 + 2 * n) : 0x80 + 21 + 2 * n);
		servo.servo_write (SERVO_FD, 0x00);
		servo.servo_write (SERVO_FE, s.push);
		break;
	case 0x9:
		s.idle_nmis = (uint16_t) (IDLE_NMIS + 5 * (cmd & 3));
		servo.servo_write (SERVO_FF, 0xFF);
		break;
	default:
		break;
	}
}

void
DriveBoardJambo::nmi (uint8_t command, uint8_t wheel, Servo &servo)
{
	(void) wheel;
	if (command != s.last_command) {
		if (log)
			HL_PRINT ("drive board: frame %llu, command %02X (was %02X)",
			          (unsigned long long) mach->frame, command, s.last_command);
		s.last_command = command;
		execute (command, servo);
	}
	switch (s.mode) {
	case MODE_SHAKE:
		/* C6: a shake, then FE 80 held */
		if (s.nmis < SHAKE_NMIS) {
			float t = 16.0f * sinf (2.0f * 3.14159265f * s.nmis / 50.0f);
			servo.servo_write (SERVO_FE, (uint8_t) lrintf (0x80 + t));
			if (++s.nmis == SHAKE_NMIS)
				servo.servo_write (SERVO_FE, 0x80);
		}
		break;
	case MODE_GAME:
		if (s.idle_nmis) {
			if (!--s.idle_nmis)
				spring (servo);
			break;
		}
		if (s.push)
			break;
		if (s.road) {
			float t = 0;
			if (s.road == 2) {
				/* A bump: one NMI at 00, then a ramp from 76 to 8A */
				if (s.bump_wait)
					s.bump_wait--;
				else {
					s.nmis = 0;
					s.bump_wait = (uint16_t) (RAMP_NMIS + next_bump ());
				}
				t = s.nmis == 0 ? -128.0f : s.nmis <= RAMP_NMIS ?
				    -10.0f + 20.0f * (s.nmis - 1) / (RAMP_NMIS - 1) : 0.0f;
			} else
				t = road[s.road].offset +
				    road[s.road].amplitude * sinf (2.0f * 3.14159265f * s.nmis / road[s.road].period);
			s.nmis++;
			servo.servo_write (SERVO_FE, (uint8_t) std::clamp (lrintf (0x80 + t), 0L, 255L));
		} else
			servo.servo_write (SERVO_FE, 0x80);
		break;
	default:
		break;
	}
}

uint8_t
DriveBoardJambo::reply () const
{
	return 0xFF;
}

int
DriveBoardJambo::save_state (State *state)
{
	return state->put (&s, sizeof (s));
}

int
DriveBoardJambo::load_state (State *state)
{
	return state->get (&s, sizeof (s));
}

} /* namespace hl */
