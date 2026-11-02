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

#ifndef __HL_EEPROM_H__
#define __HL_EEPROM_H__

#include "hl/core.h"

namespace hl {

class State;

/* A 93C46 serial EEPROM in its 8-bit organization (ORG low): 128 bytes, 7-bit
 * addresses. Its host drives three lines, CS, SK (the clock) and DI, and
 * reads DO. With CS high, the bits come in on the clock's rising edges: a
 * start bit (1, the zeros before it ignored), a 2-bit opcode, the address:
 *
 *   10 READ:  DO gives a dummy 0, then the byte, MSB first, one bit a rising
 *             edge, then the next bytes as long as the clock goes on
 *   01 WRITE: then the 8 bits of the byte
 *   11 ERASE: the byte back to FF
 *   00 then the address's two high bits: 11 EWEN, 00 EWDS (writes enabled,
 *             disabled: they are disabled at power on), 10 ERAL (all FF),
 *             01 WRAL (then a byte, written everywhere)
 *
 * The writes complete at once: DO reads 1, ready, whenever no read is going
 * on. Its contents, in files and states, keep the order of the addresses. */
class Eeprom93C46 {
public:
	static const unsigned SIZE = 128;

	Eeprom93C46 ();

	/* The lines as a GPIO port drives them all at once */
	void	 write_lines (bool cs, bool sk, bool di);
	bool	 data_out () const;

	/* No command going on, writes disabled: the power on. The contents
	 * stay; a new EEPROM holds FF everywhere */
	void	 power_on ();
	/* A file of SIZE bytes; false when it is missing or of another size */
	bool	 load (const char *path);
	bool	 save (const char *path) const;

	int	 save_state (State *state);
	int	 load_state (State *state);

	uint8_t	 mem[SIZE];
	/* Set by each write or erase, for the host to save the file */
	bool	 changed;
	/* Its first commands to the log under that name; NULL: none */
	const char *log_name;

private:
	enum Phase {
		IDLE,		/* CS low, or a command done */
		START,		/* CS high, waiting for the start bit */
		COMMAND,	/* the opcode and the address */
		WRITING,	/* the byte of WRITE or WRAL */
		READING
	};

	void	 clock (bool di);
	void	 command ();
	void	 store (uint8_t value);

	/* Plain integers, for the states */
	uint32_t phase, bits, count, addr, all, shift, left;
	uint8_t	 cs, sk, dout, enabled;
	unsigned told;
};

} /* namespace hl */

#endif /* __HL_EEPROM_H__ */
