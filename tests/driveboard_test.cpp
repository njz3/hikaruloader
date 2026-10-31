/*
 * Hikaruloader (formerly Valkyrie)
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

/* The simulated EPR-20512 drive board's tests: its start-up, how it takes
 * the commands, the torques it writes to the servo board, its replies. Run
 * by ctest, or alone: driveboardtest. */

#include <stdio.h>

#include <vector>

#include "hl/driveboard.h"

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		unsigned g_ = (unsigned) (got_), w_ = (unsigned) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %X, expected %X\n", name_, #got_, g_, w_); \
		} \
	} while (0)

/* The servo board: what was written, the last value of each register */
class TestServo : public hl::DriveBoardProgram::Servo {
public:
	std::vector<std::pair<uint8_t, uint8_t>> writes;
	uint8_t reg[256];

	TestServo () { memset (reg, 0, sizeof (reg)); }
	void servo_write (uint8_t r, uint8_t v) override
	{
		writes.push_back ({ r, v });
		reg[r] = v;
	}
};

static hl::DriveBoardSr2 *board;
static TestServo *servo;

static void
run (uint8_t command, unsigned nmis, uint8_t wheel = 0x80)
{
	while (nmis--)
		board->nmi (command, wheel, *servo);
}

/* Through the start-up, to the main loop */
static void
start (void)
{
	board->reset ();
	run (0xFF, 4000 + 3 * 500);
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;
	board = new hl::DriveBoardSr2 ();
	servo = new TestServo ();

	/* The start-up's replies and lengths */
	board->reset ();
	run (0xFF, 1);
	CHECK ("set-up", board->reply (), 0xCF);
	CHECK ("set-up: the servo board idle", servo->reg[0xFF], 0xFF);
	run (0xFF, 3998);
	CHECK ("8 s of CF", board->reply (), 0xCF);
	run (0xFF, 1);
	CHECK ("then CE", board->reply (), 0xCE);
	/* The cockpit's centre in CE, the wheel's in CD */
	run (0x70, 500);
	CHECK ("then CD", board->reply (), 0xCD);
	run (0x78, 499);
	CHECK ("1 s of CD", board->reply (), 0xCD);
	run (0x78, 1);
	CHECK ("then CC, ready", board->reply (), 0xCC);
	run (0xB1, 500);
	CHECK ("then the status", board->reply (), 0x07);
	CHECK ("the motor free", servo->reg[0xFF], 0xFF);
	run (0xD3, 4);
	CHECK ("D3: the wheel's centre", board->reply (), 0x78);
	run (0xD4, 4);
	CHECK ("D4: the cockpit's centre", board->reply (), 0x70);

	/* A command counts at 4 NMIs */
	start ();
	run (0xF5, 3);
	CHECK ("3 NMIs: not yet", board->reply (), 0x07);
	run (0xF5, 1);
	CHECK ("4 NMIs: the echo", board->reply (), 0xF5);
	run (0xD1, 4);
	CHECK ("D1: DIP 1, the strength's row 4", board->reply (), 0xEF);
	run (0xD5, 4, 0x9A);
	CHECK ("D5: the wheel", board->reply (), 0x9A);
	run (0xD0, 4);
	CHECK ("D0: the status", board->reply (), 0x07);

	/* The torques, in a game only */
	start ();
	servo->writes.clear ();
	run (0x20, 4);
	CHECK ("a torque outside a game: nothing", servo->writes.size (), 0);
	run (0xC7, 4);
	CHECK ("C7: the strength's FD", servo->reg[0xFD], 0x31);
	CHECK ("C7: the strength's FC", servo->reg[0xFC], 0x2C);
	CHECK ("C7: the gain", servo->reg[0xFB], 0xFF);
	run (0x10, 4);
	CHECK ("10: left, FE 90", servo->reg[0xFE], 0x90);
	CHECK ("a torque's FD 11", servo->reg[0xFD], 0x11);
	run (0x3F, 4);
	CHECK ("3F: FE BF", servo->reg[0xFE], 0xBF);
	run (0x45, 4);
	CHECK ("45: right, FE 7B", servo->reg[0xFE], 0x7B);
	servo->writes.clear ();
	run (0x45, 40);
	CHECK ("the same torque again: nothing more", servo->writes.size (), 0);
	CHECK ("held", servo->reg[0xFE], 0x7B);
	/* Too short a command: not taken */
	run (0x30, 2);
	run (0x45, 10);
	CHECK ("a command of 2 NMIs: lost", servo->writes.size (), 0);

	/* Attract: the motor free; C2's shake, then FE 80 held */
	run (0xC1, 4);
	CHECK ("C1: the motor free", servo->reg[0xFF], 0xFF);
	servo->writes.clear ();
	run (0xC2, 4);
	run (0xC2, 300);
	CHECK ("C2: a torque an NMI", servo->writes.size (), 300);
	CHECK ("then FE 80", servo->reg[0xFE], 0x80);
	run (0x10, 4);
	CHECK ("a torque in attract: nothing", servo->reg[0xFE], 0x80);

	/* The test: CA, then 81 / 82 */
	run (0xCA, 4);
	run (0x81, 4);
	CHECK ("test 81: a push right", servo->reg[0xFE], 0x69);
	run (0x82, 4);
	CHECK ("test 82: a push left", servo->reg[0xFE], 0x97);

	/* CB: the start-up again from CE */
	run (0xCB, 4);
	CHECK ("CB: CE again", board->reply (), 0xCE);
	run (0xCB, 1000);
	CHECK ("then CC", board->reply (), 0xCC);

	delete board;
	delete servo;
	printf ("driveboardtest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
