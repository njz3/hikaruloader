/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/chips/x76f100.hpp, Copyright (C) 2026, Réda Chérif-Touil,
 * itself from MAME's src/devices/machine/x76f100.h (license: BSD-3-Clause, copyright-holders: smf).
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

#ifndef __HL_X76F100_H__
#define __HL_X76F100_H__

#include "hl/core.h"

namespace hl {

class State;

/* The Xicor X76F100 secure serial EEPROM: 112 bytes behind two 64-bit
 * passwords, one to read and one to write, on a two-wire bus (SCL, SDA) with
 * CS and RST. The Hikaru's ROM board has one (IC85), which the SH-4 drives
 * through bus page 14 (MAME's hikaru.cpp): bit 0 SDA, 1 SCL, 2 /CS, 3 /RST
 * written; bit 0 SDA read.
 *
 * Its protocol, as MAME has it: RST rising with CS low starts the 4-byte
 * response to reset, shifted out LSB first on SCL's falling edges, over and
 * over. A START (SDA falling with SCL high) is followed by the command byte,
 * MSB first, then 8 password bytes, each answered by an ACK (SDA low on the
 * ninth clock). The host then polls with the byte 55, after another START or
 * not: an ACK when the password matched, else a NAK (SDA high). A read then
 * shifts the bytes out, MSB first, the host ACKing each one to get the next;
 * a write takes 8 bytes and stores them as a page. STOP is SDA rising with
 * SCL high. The command: bit 7 set, bit 0 1 read, 0 write, bits 4-1 the
 * page of 8 bytes; FC changes the write password, FE the read password, both
 * after the write password. Eight wrong passwords in a row erase the
 * passwords and the data.
 *
 * Its image, 84 bytes, in MAME's order (x76f100.ic85):
 *   00  the response to reset (19 00 AA 55 when blank)
 *   04  the write password
 *   0C  the read password
 *   14  the data, 14 pages of 8 */
class X76F100 {
public:
	static const unsigned RESPONSE_SIZE = 4;
	static const unsigned PASSWORD_SIZE = 8;
	static const unsigned PAGE_SIZE = 8;
	static const unsigned DATA_SIZE = 112;
	static const unsigned IMAGE_RESPONSE = 0x00;
	static const unsigned IMAGE_WRITE_PASSWORD = 0x04;
	static const unsigned IMAGE_READ_PASSWORD = 0x0C;
	static const unsigned IMAGE_DATA = 0x14;
	static const unsigned IMAGE_SIZE = IMAGE_DATA + DATA_SIZE;

	/* A blank chip: the usual response to reset, no passwords */
	X76F100 ();

	/* The contents stay */
	void	 reset ();
	/* The pins' levels, CS and RST active low; the edges seen in the
	 * order CS, RST, SCL, SDA */
	void	 set_pins (bool cs, bool rst, bool scl, bool sda);
	/* SDA as the chip drives it: released (high) while not selected */
	bool	 sda_read () const;
	/* IMAGE_SIZE bytes; false, the image kept, for another size */
	bool	 load (const uint8_t *data, size_t size);

	int	 save_state (State *state);
	int	 load_state (State *state);

	uint8_t	 image[IMAGE_SIZE];
	/* Set by a page or a password stored, for the host to save */
	bool	 changed;
	/* Each command to the log ("x76f100: ..."), the first ones */
	bool	 log;

private:
	enum Phase {
		STOP,
		RESPONSE_TO_RESET,
		LOAD_COMMAND,
		LOAD_PASSWORD,
		VERIFY_PASSWORD,
		READ_DATA,
		WRITE_DATA
	};

	void	 on_cs (bool level);
	void	 on_rst (bool level);
	void	 on_scl (bool level);
	void	 on_sda (bool level);
	void	 byte_in ();
	void	 check_password ();
	void	 store_page ();
	void	 read_clock ();
	uint8_t	*password ();
	void	 password_ok ();
	/* The data's byte the command is at; -1 past its end */
	int	 data_offset () const;
	void	 tell (const char *fmt, ...);

	/* Plain data, for the states */
	struct {
		uint8_t	 write_buffer[PASSWORD_SIZE];
		uint32_t byte;
		uint8_t	 phase;
		uint8_t	 cs, rst, scl, sdaw, sdar;
		uint8_t	 shift, bit, command;
		uint8_t	 retries;	/* wrong passwords in a row */
		uint8_t	 accepted;
	} s;
	unsigned told;
};

} /* namespace hl */

#endif /* __HL_X76F100_H__ */
