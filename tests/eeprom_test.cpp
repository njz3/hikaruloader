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

/* The 93C46's tests: its commands, driven as the MIE's programs drive it
 * (SK low then high, DO read after the rising edge). Run by ctest, or alone:
 * eepromtest. */

#include <stdio.h>

#include "hl/eeprom.h"

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

static hl::Eeprom93C46 *e;

static void
select_chip (void)
{
	e->write_lines (false, false, false);
	e->write_lines (true, false, false);
}

static void
deselect (void)
{
	e->write_lines (false, false, false);
}

/* n bits of value, MSB first */
static void
send (unsigned value, unsigned n)
{
	while (n--) {
		bool di = (value >> n) & 1;
		e->write_lines (true, false, di);
		e->write_lines (true, true, di);
	}
}

static unsigned
receive (unsigned n)
{
	unsigned value = 0;

	while (n--) {
		e->write_lines (true, false, false);
		e->write_lines (true, true, false);
		value = (value << 1) | (e->data_out () ? 1 : 0);
	}
	return value;
}

/* Start bit, opcode, address */
static void
command (unsigned op, unsigned addr)
{
	select_chip ();
	send (1, 1);
	send (op, 2);
	send (addr, 7);
}

static unsigned
read_byte (unsigned addr)
{
	unsigned value;

	command (2, addr);
	CHECK ("dummy bit", e->data_out (), 0);
	value = receive (8);
	deselect ();
	return value;
}

static void
write_byte (unsigned addr, unsigned value)
{
	command (1, addr);
	send (value, 8);
	deselect ();
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;
	e = new hl::Eeprom93C46 ();

	CHECK ("new: FF", read_byte (0), 0xFF);
	CHECK ("deselected: ready", e->data_out (), 1);

	/* Writes are disabled at power on */
	write_byte (5, 0x5A);
	CHECK ("write before EWEN", read_byte (5), 0xFF);
	CHECK ("nothing changed", e->changed, 0);

	command (0, 0x60);		/* EWEN */
	deselect ();
	write_byte (5, 0x5A);
	CHECK ("write", read_byte (5), 0x5A);
	CHECK ("changed", e->changed, 1);
	CHECK ("selected, idle: ready", (select_chip (), e->data_out ()), 1);
	deselect ();

	/* The zeros before the start bit do not count */
	select_chip ();
	send (0, 3);
	send (1, 1);
	send (2, 2);
	send (5, 7);
	receive (8);
	deselect ();
	CHECK ("zeros before the start bit", e->mem[5], 0x5A);

	/* A read goes on to the next byte, without a dummy bit */
	write_byte (6, 0xA5);
	command (2, 5);
	CHECK ("sequential, first", receive (8), 0x5A);
	CHECK ("sequential, second", receive (8), 0xA5);
	deselect ();
	command (2, 127);
	receive (8);
	CHECK ("sequential, wraps", receive (8), 0xFF);
	deselect ();

	command (3, 5);			/* ERASE */
	deselect ();
	CHECK ("erase", read_byte (5), 0xFF);
	CHECK ("erase, the next one stays", read_byte (6), 0xA5);

	command (0, 0x20);		/* WRAL */
	send (0x33, 8);
	deselect ();
	CHECK ("wral, first", read_byte (0), 0x33);
	CHECK ("wral, last", read_byte (127), 0x33);

	command (0, 0x40);		/* ERAL */
	deselect ();
	CHECK ("eral", read_byte (64), 0xFF);

	command (0, 0x00);		/* EWDS */
	deselect ();
	write_byte (1, 0x00);
	CHECK ("write after EWDS", read_byte (1), 0xFF);

	/* A command cut short by CS does nothing */
	command (0, 0x60);
	deselect ();
	command (1, 2);
	send (0x0F, 4);
	deselect ();
	CHECK ("write cut short", read_byte (2), 0xFF);

	/* The file, byte n at address n */
	write_byte (9, 0x42);
	CHECK ("save", e->save ("eepromtest.bin"), 1);
	{
		hl::Eeprom93C46 other;
		CHECK ("load", other.load ("eepromtest.bin"), 1);
		CHECK ("loaded", other.mem[9], 0x42);
		CHECK ("missing file", other.load ("eepromtest-missing.bin"), 0);
	}
	remove ("eepromtest.bin");

	delete e;
	printf ("eepromtest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
