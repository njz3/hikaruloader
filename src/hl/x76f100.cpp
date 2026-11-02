/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/chips/x76f100.cpp, Copyright (C) 2026, Réda Chérif-Touil,
 * itself from MAME's src/devices/machine/x76f100.cpp (license: BSD-3-Clause, copyright-holders: smf).
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

#include <stdarg.h>

#include "hl/x76f100.h"
#include "hl/state.h"

namespace hl {

enum {
	COMMAND_WRITE = 0x80,
	COMMAND_READ = 0x81,
	COMMAND_CHANGE_WRITE_PASSWORD = 0xFC,
	COMMAND_CHANGE_READ_PASSWORD = 0xFE,
	COMMAND_ACK_PASSWORD = 0x55
};

/* Wrong passwords in a row before the chip erases itself */
static const uint8_t PASSWORD_RETRIES = 8;
/* The commands told in the log, at most */
static const unsigned MAX_TOLD = 32;

X76F100::X76F100 () :
	changed (false),
	log (true),
	told (0)
{
	memset (image, 0, sizeof (image));
	image[0] = 0x19;
	image[1] = 0x00;
	image[2] = 0xAA;
	image[3] = 0x55;
	reset ();
}

void
X76F100::reset ()
{
	memset (&s, 0, sizeof (s));
	s.phase = STOP;
}

bool
X76F100::load (const uint8_t *data, size_t size)
{
	if (size != IMAGE_SIZE)
		return false;
	memcpy (image, data, IMAGE_SIZE);
	return true;
}

void
X76F100::tell (const char *fmt, ...)
{
	char text[160];
	va_list args;

	if (!log || told > MAX_TOLD)
		return;
	if (++told > MAX_TOLD) {
		HL_PRINT ("x76f100: (the next commands not told)");
		return;
	}
	va_start (args, fmt);
	vsnprintf (text, sizeof (text), fmt, args);
	va_end (args);
	HL_PRINT ("x76f100: %s", text);
}

void
X76F100::set_pins (bool cs, bool rst, bool scl, bool sda)
{
	on_cs (cs);
	on_rst (rst);
	on_scl (scl);
	on_sda (sda);
}

bool
X76F100::sda_read () const
{
	if (s.cs)
		return true;
	return s.sdar != 0;
}

/* Either edge of CS drops the command going on. Whether SDA floats once
 * deselected is not known: sda_read () gives it high then, as MAME does */
void
X76F100::on_cs (bool level)
{
	if ((s.cs != 0) != level) {
		s.phase = STOP;
		if (level)
			s.sdar = 0;
	}
	s.cs = level;
}

void
X76F100::on_rst (bool level)
{
	if (!s.rst && level && !s.cs) {
		s.phase = RESPONSE_TO_RESET;
		s.bit = 0;
		s.byte = 0;
		tell ("reset: the response to reset %02X %02X %02X %02X",
		      image[0], image[1], image[2], image[3]);
	}
	s.rst = level;
}

void
X76F100::on_scl (bool level)
{
	if (!s.cs) {
		switch (s.phase) {
		case STOP:
			break;

		case RESPONSE_TO_RESET:
			if (s.scl && !level) {
				if (s.bit == 0)
					s.shift = image[IMAGE_RESPONSE + s.byte];
				s.sdar = s.shift & 1;
				s.shift >>= 1;
				if (++s.bit == 8) {
					s.bit = 0;
					if (++s.byte == RESPONSE_SIZE)
						s.byte = 0;
				}
			}
			break;

		case LOAD_COMMAND:
		case LOAD_PASSWORD:
		case VERIFY_PASSWORD:
		case WRITE_DATA:
			/* As in MAME: the bit taken on the rising edge, which makes
			 * SDA change while SCL is high, as the bus does not allow;
			 * neither the device's ID nor the commands checked */
			if (!s.scl && level) {
				if (s.bit < 8) {
					s.shift = (uint8_t) ((s.shift << 1) | (s.sdaw ? 1 : 0));
					s.bit++;
				} else {
					s.sdar = 0;
					byte_in ();
					s.bit = 0;
					s.shift = 0;
				}
			}
			break;

		case READ_DATA:
			if (!s.scl && level)
				read_clock ();
			break;
		}
	}
	s.scl = level;
}

/* START and STOP: SDA moving while SCL is high */
void
X76F100::on_sda (bool level)
{
	if (!s.cs && s.scl) {
		if (!s.sdaw && level) {
			s.phase = STOP;
			s.sdar = 0;
		}
		if (s.sdaw && !level) {
			/* A START while loading the password would be command C0;
			 * while reading, whether the read goes on is not known */
			if (s.phase == STOP)
				s.phase = LOAD_COMMAND;
			s.bit = 0;
			s.byte = 0;
			s.shift = 0;
			s.sdar = 0;
		}
	}
	s.sdaw = level;
}

/* The ninth clock of a byte from the host: the byte is in, SDA already low
 * (an ACK); only the password's poll can make it a NAK */
void
X76F100::byte_in ()
{
	switch (s.phase) {
	case LOAD_COMMAND:
		/* Not checked */
		s.command = s.shift;
		s.phase = LOAD_PASSWORD;
		break;

	case LOAD_PASSWORD:
		s.write_buffer[s.byte++] = s.shift;
		if (s.byte == PASSWORD_SIZE) {
			s.phase = VERIFY_PASSWORD;
			check_password ();
		}
		break;

	case VERIFY_PASSWORD:
		/* The poll is not taken as a command; the chip takes up to 10
		 * ms before it answers */
		if (s.shift == COMMAND_ACK_PASSWORD) {
			if (s.accepted)
				password_ok ();
			else
				s.sdar = 1;
		}
		break;

	case WRITE_DATA:
		/* Without a START before the poll the count is at 8 here (MAME
		 * then writes past its buffer): the bytes are dropped */
		if (s.byte >= PAGE_SIZE)
			break;
		s.write_buffer[s.byte++] = s.shift;
		if (s.byte == PAGE_SIZE)
			store_page ();
		break;

	default:
		break;
	}
}

/* The password received against the command's: here and not at each poll
 * of 55, which comes again and again and would run the count of retries
 * over */
void
X76F100::check_password ()
{
	const uint8_t *expected = password ();

	s.accepted = !memcmp (s.write_buffer, expected, PASSWORD_SIZE);
	tell ("command %02X (%s, page %u): password %s", s.command,
	      (s.command & 0x81) == COMMAND_READ ? "read" : s.command >= COMMAND_CHANGE_WRITE_PASSWORD ? "password change" : "write",
	      (s.command >> 1) & 0x0F, s.accepted ? "accepted" : "refused");
	if (s.accepted)
		return;
	if (++s.retries >= PASSWORD_RETRIES) {
		memset (image + IMAGE_WRITE_PASSWORD, 0, PASSWORD_SIZE);
		memset (image + IMAGE_READ_PASSWORD, 0, PASSWORD_SIZE);
		memset (image + IMAGE_DATA, 0, DATA_SIZE);
		s.retries = 0;
		changed = true;
		tell ("eight wrong passwords: the passwords and the data erased");
	}
}

/* A page received whole: a password, or the memory's bytes */
void
X76F100::store_page ()
{
	if (s.command == COMMAND_CHANGE_WRITE_PASSWORD)
		memcpy (image + IMAGE_WRITE_PASSWORD, s.write_buffer, PASSWORD_SIZE);
	else if (s.command == COMMAND_CHANGE_READ_PASSWORD)
		memcpy (image + IMAGE_READ_PASSWORD, s.write_buffer, PASSWORD_SIZE);
	else
		for (s.byte = 0; s.byte < PAGE_SIZE; s.byte++) {
			int offs = data_offset ();
			if (offs < 0)
				break;
			image[IMAGE_DATA + offs] = s.write_buffer[s.byte];
		}
	changed = true;
	tell ("command %02X: a page stored", s.command);
	s.byte = 0;
}

/* SCL rising in a read: 8 bits out, MSB first, then the host's ACK (the
 * next byte) or NAK on the ninth clock */
void
X76F100::read_clock ()
{
	if (s.bit < 8) {
		if (s.bit == 0) {
			int offs = data_offset ();
			s.shift = (offs >= 0) ? image[IMAGE_DATA + offs] : 0;
		}
		s.sdar = (s.shift & 0x80) != 0;
		s.shift <<= 1;
		s.bit++;
	} else {
		s.bit = 0;
		s.sdar = 0;
		if (!s.sdaw)
			s.byte++;
	}
}

/* The password the command is checked against */
uint8_t *
X76F100::password ()
{
	if ((s.command & 0xE1) == COMMAND_READ)
		return image + IMAGE_READ_PASSWORD;
	return image + IMAGE_WRITE_PASSWORD;
}

void
X76F100::password_ok ()
{
	s.retries = 0;
	if ((s.command & 0x81) == COMMAND_READ)
		s.phase = READ_DATA;
	else if ((s.command & 0x81) == COMMAND_WRITE)
		s.phase = WRITE_DATA;
	/* Bit 7 clear: left open in MAME */
}

/* Four bits of page in the command, but 14 pages only: what the chip does
 * for pages 14 and 15 is not known */
int
X76F100::data_offset () const
{
	unsigned page = (s.command >> 1) & 0x0F;
	unsigned offs = page * PAGE_SIZE + s.byte;

	return (offs < DATA_SIZE) ? (int) offs : -1;
}

int
X76F100::save_state (State *state)
{
	int ret = state->put (image, sizeof (image));
	ret |= state->put (&s, sizeof (s));
	return ret;
}

int
X76F100::load_state (State *state)
{
	int ret = state->get (image, sizeof (image));
	ret |= state->get (&s, sizeof (s));
	return ret;
}

} /* namespace hl */
