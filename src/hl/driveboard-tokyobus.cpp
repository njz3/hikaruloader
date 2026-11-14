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

#include "hl/core.h"
#include "hl/driveboard.h"
#include "hl/state.h"

namespace hl {

/*
 * Tokyo Bus Guide's Motor Board
 * =============================
 * The wheel's motor board, wired to the 837-13844 I/O board as F355
 * Challenge's drive board (the command on outputs 15 to 22, the reply on
 * players 1's and 2's switches 3 to 6). The game checks it as it starts:
 * with the reply lines idle (FF) it shows MOTOR NETWORK ERROR and takes no
 * input at all. Its answers, as Flycast has them (the game plays with
 * them): 10 to F1, the command itself in the test mode (from F0 to FF),
 * 00 to the others. Its forces are not played: the power (40 to 4A),
 * the vibrations (80 to BF) and the test's rolls (E0 to EF) are taken and
 * left.
 */

DriveBoardTokyoBus::DriveBoardTokyoBus ()
{
	reset ();
}

void
DriveBoardTokyoBus::reset ()
{
	s.answer = 0x00;
	s.test = 0;
}

void
DriveBoardTokyoBus::nmi (uint8_t command, uint8_t wheel, Servo &servo)
{
	(void) wheel;
	(void) servo;
	if (command == 0xF0)
		s.test = 1;
	else if (command == 0xFF)
		s.test = 0;
	s.answer = s.test ? command : command == 0xF1 ? 0x10 : 0x00;
	if (log && command != logged) {
		logged = command;
		HL_PRINT ("motor board: command %02X (reply %02X)", command, s.answer);
	}
}

uint8_t
DriveBoardTokyoBus::reply () const
{
	return s.answer;
}

int
DriveBoardTokyoBus::save_state (State *state)
{
	return state->put (&s, sizeof (s));
}

int
DriveBoardTokyoBus::load_state (State *state)
{
	return state->get (&s, sizeof (s));
}

} /* namespace hl */
