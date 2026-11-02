/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
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

/* The X76F100's tests (the ROM board's secure EEPROM): its protocol driven
 * as a host drives its pins, the BOOTROM's way (a START before the poll of
 * the password): the response to reset, a read, a write, a wrong password,
 * the erasure after eight. Run by ctest, or alone: x76f100test. */

#include <stdio.h>

#include "hl/x76f100.h"

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

static hl::X76F100 *chip;
/* The pins as the host drives them: CS and RST active low */
static bool cs = true, rst = false, scl = true, sda = true;

static void
drive (void)
{
	chip->set_pins (cs, rst, scl, sda);
}

/* SDA falling while SCL is high */
static void
start (void)
{
	scl = false; drive ();
	sda = true; drive ();
	scl = true; drive ();
	sda = false; drive ();
}

/* SDA rising while SCL is high */
static void
stop (void)
{
	scl = false; drive ();
	sda = false; drive ();
	scl = true; drive ();
	sda = true; drive ();
}

/* A byte, MSB first, then the ninth clock: true for an ACK */
static bool
send (uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		scl = false; drive ();
		sda = (byte >> i) & 1; drive ();
		scl = true; drive ();
	}
	scl = false; drive ();
	sda = true; drive ();
	scl = true; drive ();
	return !chip->sda_read ();
}

/* A byte from the chip, MSB first, then the host's ACK (more to come) */
static uint8_t
receive (bool ack)
{
	uint8_t byte = 0;

	for (int i = 0; i < 8; i++) {
		scl = false; drive ();
		scl = true; drive ();
		byte = (uint8_t) ((byte << 1) | (chip->sda_read () ? 1 : 0));
	}
	scl = false; drive ();
	sda = !ack; drive ();
	scl = true; drive ();
	return byte;
}

/* The command and its password, then the poll after a START: true when the
 * password is accepted */
static bool
command (uint8_t cmd, const uint8_t *password)
{
	start ();
	send (cmd);
	for (int i = 0; i < 8; i++)
		send (password[i]);
	start ();
	return send (0x55);
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	static const uint8_t zeros[8] = { 0 };
	static const uint8_t wrong[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t image[hl::X76F100::IMAGE_SIZE];
	unsigned i;

	(void) argc;
	(void) argv;

	chip = new hl::X76F100 ();
	chip->log = false;
	/* An image as MAME's x76f100.ic85: no passwords, a serial number */
	memcpy (image, chip->image, sizeof (image));
	memcpy (image + hl::X76F100::IMAGE_DATA, "BBAE-01A0792661G", 16);
	CHECK ("load", chip->load (image, sizeof (image)), 1);
	CHECK ("load of another size", chip->load (image, 10), 0);
	drive ();

	/* Deselected, SDA reads high */
	CHECK ("released", chip->sda_read (), 1);

	/* The response to reset: RST rising with CS low, the bits LSB first on
	 * SCL's falling edges */
	cs = false; scl = true; drive ();
	rst = true; drive ();
	for (i = 0; i < 4; i++) {
		uint8_t byte = 0;
		for (int b = 0; b < 8; b++) {
			scl = false; drive ();
			byte |= (uint8_t) ((chip->sda_read () ? 1 : 0) << b);
			scl = true; drive ();
		}
		CHECK ("response to reset", byte, image[i]);
	}
	rst = false; drive ();
	/* A command starts from a STOP */
	stop ();

	/* A read of page 0, the read password all zeros */
	CHECK ("read password", command (0x81, zeros), 1);
	for (i = 0; i < 16; i++)
		CHECK ("read", receive (i < 15), image[hl::X76F100::IMAGE_DATA + i]);
	stop ();

	/* A write of page 3, then read back */
	CHECK ("write password", command (0x86, zeros), 1);
	for (i = 0; i < 8; i++)
		send ((uint8_t) (0xA0 + i));
	stop ();
	CHECK ("written", chip->changed, 1);
	CHECK ("page 3", chip->image[hl::X76F100::IMAGE_DATA + 24], 0xA0);
	CHECK ("page 3 end", chip->image[hl::X76F100::IMAGE_DATA + 31], 0xA7);
	CHECK ("page 4 left", chip->image[hl::X76F100::IMAGE_DATA + 32], 0);
	CHECK ("read page 3", command (0x87, zeros), 1);
	CHECK ("read back", receive (true), 0xA0);
	CHECK ("read back 2", receive (false), 0xA1);
	stop ();

	/* A wrong password: a NAK, the data not given */
	CHECK ("wrong password", command (0x81, wrong), 0);
	stop ();
	CHECK ("kept after one", chip->image[hl::X76F100::IMAGE_DATA], 'B');

	/* Eight in a row erase the passwords and the data */
	for (i = 1; i < 8; i++) {
		command (0x81, wrong);
		stop ();
	}
	CHECK ("erased", chip->image[hl::X76F100::IMAGE_DATA], 0);
	CHECK ("erased page 3", chip->image[hl::X76F100::IMAGE_DATA + 24], 0);
	CHECK ("response kept", chip->image[0], 0x19);

	/* Deselected in the middle of a read: SDA released, the command gone */
	CHECK ("read again", command (0x81, zeros), 1);
	cs = true; drive ();
	CHECK ("released again", chip->sda_read (), 1);

	delete chip;
	printf ("x76f100test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
