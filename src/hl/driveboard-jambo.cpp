/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * The commands' families as Supermodel's simulated wheel board reads them
 * (Src/Model3/DriveBoard/WheelBoard.cpp, Copyright 2011-2021 Bart
 * Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle and Spindizzi, GPL v3
 * or later).
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
#include <cstring>

#include "hl/core.h"
#include "hl/driveboard.h"
#include "hl/ffb.h"
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
 * runs on the Z80 when the scud set is there (DriveBoardZ80) and answers them
 * as the game expects: the OUTPUT TEST's ROLL LEFT (82) and ROLL RIGHT (81)
 * turn the wheel, 80 stops it.
 *
 * Without that EPROM, this simulation:
 *
 *  - 00-0F  one of the board's own sequences: a jolt here, 01 to 0F (the
 *           game sends 01 and 0A while driving, 00 and 08 as it starts)
 *  - 10-1F  the centring's strength (10 none)
 *  - 20-2F  the friction's
 *  - 30-3F  a vibration's
 *  - 50-5F, 60-6F  a torque to the right, to the left: (n + 1) / 16
 *  - 80     the test's stop; 81 a roll to the right, 82 to the left
 *  - C0-CF  the board's mode: C1 the game, CA the test; CB and above a
 *           reset, all stopped
 *  - D0-DF  what the board answers; E0-EF none; F0-FF an echo
 *
 * The game sends C1 00 30 10 20 08 C1 as it starts (all off), then while
 * driving 25 and 11 in turns (a friction, a light centring). With the reply
 * lines idle (FF), as here, it plays and its tests run.
 */

/* A sequence's jolt, NMIs at 500 Hz. As strong as EPR-19338A makes them: the
 * OUTPUT TEST's rolls (the servo board's torque 80 -+ 17), the game's 11 (the
 * servo board's spring at 6 of 15) and 25 (its friction at 2 of 15) */
#define JOLT_NMIS	100
#define JOLT		0.4f
#define ROLL		0.35f
#define SPRING_BASE	0.5f
#define SPRING_STEP	(1.0f / 30)
#define FRICTION_STEP	0.05f

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
}

void
DriveBoardJambo::set (int effect, float force)
{
	ForceFeedback::set ((ForceFeedback::Effect) effect, force, mach->frame);
}

void
DriveBoardJambo::execute (uint8_t cmd)
{
	uint8_t n = cmd & 0x0F;

	switch (cmd >> 4) {
	case 0x0:
		s.jolt_nmis = n ? JOLT_NMIS : 0;
		set (ForceFeedback::VIBRATE, n ? JOLT : s.vibration / 15.0f);
		break;
	case 0x1:
		set (ForceFeedback::SPRING, n ? std::min (SPRING_BASE + n * SPRING_STEP, 1.0f) : 0.0f);
		break;
	case 0x2:
		set (ForceFeedback::FRICTION, n * FRICTION_STEP);
		break;
	case 0x3:
		s.vibration = n;
		if (!s.jolt_nmis)
			set (ForceFeedback::VIBRATE, n / 15.0f);
		break;
	case 0x5:
		set (ForceFeedback::CONSTANT, -(n + 1) / 16.0f);
		break;
	case 0x6:
		set (ForceFeedback::CONSTANT, (n + 1) / 16.0f);
		break;
	case 0x8:
		set (ForceFeedback::CONSTANT, n == 1 ? -ROLL : n == 2 ? ROLL : 0.0f);
		break;
	case 0xC:
		if (n >= 0xB) {
			s.vibration = 0;
			s.jolt_nmis = 0;
			set (ForceFeedback::STOP, 0.0f);
		}
		break;
	default:
		break;
	}
}

void
DriveBoardJambo::nmi (uint8_t command, uint8_t wheel, Servo &servo)
{
	(void) servo;
	(void) wheel;
	if (s.jolt_nmis && !--s.jolt_nmis)
		set (ForceFeedback::VIBRATE, s.vibration / 15.0f);
	if (command == s.last_command)
		return;
	if (log)
		HL_PRINT ("drive board: frame %llu, command %02X (was %02X)",
		          (unsigned long long) mach->frame, command, s.last_command);
	s.last_command = command;
	execute (command);
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
