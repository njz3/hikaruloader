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

#include <stdio.h>
#include <string.h>

#include "hl/eeprom.h"
#include "hl/state.h"

namespace hl {

#define ADDR_BITS	7

Eeprom93C46::Eeprom93C46 ()
{
	memset (mem, 0xFF, sizeof (mem));
	changed = false;
	power_on ();
}

void
Eeprom93C46::power_on ()
{
	phase = IDLE;
	bits = count = addr = all = shift = left = 0;
	cs = sk = 0;
	dout = 1;
	enabled = 0;
}

void
Eeprom93C46::write_lines (bool new_cs, bool new_sk, bool di)
{
	/* Deselected, it drops the command; selected, it waits for a new one */
	if (!new_cs)
		phase = IDLE;
	else if (!cs)
		phase = START;
	if (new_cs && new_sk && !sk)
		clock (di);
	cs = new_cs;
	sk = new_sk;
}

bool
Eeprom93C46::data_out () const
{
	return (cs && phase == READING) ? dout != 0 : true;
}

/* A rising edge of SK, with CS high */
void
Eeprom93C46::clock (bool di)
{
	switch (phase) {
	case START:
		if (di) {
			phase = COMMAND;
			bits = count = 0;
		}
		break;
	case COMMAND:
		bits = (bits << 1) | (di ? 1 : 0);
		if (++count == 2 + ADDR_BITS)
			command ();
		break;
	case WRITING:
		bits = (bits << 1) | (di ? 1 : 0);
		if (++count == 8) {
			store ((uint8_t) bits);
			phase = IDLE;
		}
		break;
	case READING:
		/* On to the next byte when this one is out */
		if (!left) {
			addr = (addr + 1) % SIZE;
			shift = mem[addr];
			left = 8;
		}
		left--;
		dout = (shift >> left) & 1;
		break;
	default:
		break;
	}
}

void
Eeprom93C46::command ()
{
	unsigned op = bits >> ADDR_BITS;

	addr = bits & (SIZE - 1);
	phase = IDLE;
	switch (op) {
	case 2:
		phase = READING;
		shift = mem[addr];
		left = 8;
		dout = 0;
		break;
	case 1:
		phase = WRITING;
		bits = count = all = 0;
		break;
	case 3:
		if (enabled) {
			mem[addr] = 0xFF;
			changed = true;
		}
		break;
	default:
		switch (addr >> (ADDR_BITS - 2)) {
		case 3:
			enabled = 1;
			break;
		case 0:
			enabled = 0;
			break;
		case 2:
			if (enabled) {
				memset (mem, 0xFF, sizeof (mem));
				changed = true;
			}
			break;
		default:
			phase = WRITING;
			bits = count = 0;
			all = 1;
			break;
		}
		break;
	}
}

void
Eeprom93C46::store (uint8_t value)
{
	if (!enabled)
		return;
	if (all)
		memset (mem, value, sizeof (mem));
	else
		mem[addr] = value;
	changed = true;
}

bool
Eeprom93C46::load (const char *path)
{
	uint8_t data[SIZE];
	bool ok;
	FILE *fp;

	fp = fopen (path, "rb");
	if (!fp)
		return false;
	ok = fread (data, 1, SIZE, fp) == SIZE && fgetc (fp) == EOF;
	fclose (fp);
	if (ok)
		memcpy (mem, data, SIZE);
	return ok;
}

bool
Eeprom93C46::save (const char *path) const
{
	FILE *fp;
	bool ok;

	fp = fopen (path, "wb");
	if (!fp)
		return false;
	ok = fwrite (mem, 1, SIZE, fp) == SIZE;
	return (fclose (fp) == 0) && ok;
}

int
Eeprom93C46::save_state (State *state)
{
	int ret = state->put (mem, sizeof (mem));
	ret |= state->put (&phase, sizeof (phase));
	ret |= state->put (&bits, sizeof (bits));
	ret |= state->put (&count, sizeof (count));
	ret |= state->put (&addr, sizeof (addr));
	ret |= state->put (&all, sizeof (all));
	ret |= state->put (&shift, sizeof (shift));
	ret |= state->put (&left, sizeof (left));
	ret |= state->put (&cs, sizeof (cs));
	ret |= state->put (&sk, sizeof (sk));
	ret |= state->put (&dout, sizeof (dout));
	ret |= state->put (&enabled, sizeof (enabled));
	return ret;
}

int
Eeprom93C46::load_state (State *state)
{
	int ret = state->get (mem, sizeof (mem));
	ret |= state->get (&phase, sizeof (phase));
	ret |= state->get (&bits, sizeof (bits));
	ret |= state->get (&count, sizeof (count));
	ret |= state->get (&addr, sizeof (addr));
	ret |= state->get (&all, sizeof (all));
	ret |= state->get (&shift, sizeof (shift));
	ret |= state->get (&left, sizeof (left));
	ret |= state->get (&cs, sizeof (cs));
	ret |= state->get (&sk, sizeof (sk));
	ret |= state->get (&dout, sizeof (dout));
	ret |= state->get (&enabled, sizeof (enabled));
	return ret;
}

} /* namespace hl */
