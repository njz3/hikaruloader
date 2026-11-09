/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/m3comm.cpp (license BSD-3-Clause,
 * copyright-holders MetalliC).
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
#include "hl/state.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Communication Board
 * =======================
 *
 *  5F7018	The control: bit 0 the 68000's RAM (else the communication RAM)
 *		through the data port, 5 the 68000 out of reset, 14 clear: the
 *		board has the G1's DMA
 *  5F701C	The offset of the data port
 *  5F7020	The data: 16 bits, big endian, the offset going on by 2
 *  5F7024,28	Status
 *
 * Its 68000 is not run: a cabinet alone, with nobody at the other end.
 */

enum {
	COMM_CTRL	= 0x5F7018,
	COMM_OFFSET	= 0x5F701C,
	COMM_DATA	= 0x5F7020,
	COMM_STATUS1	= 0x5F7024,
	COMM_STATUS2	= 0x5F7028,
};

static const uint32_t RAM_SIZE = 128 * 1024;
static const uint16_t CTRL_CPU_RAM = 1 << 0, CTRL_RESET = 1 << 5, CTRL_G1DMA = 1 << 14;

static inline uint16_t
swap16 (uint16_t w)
{
	return (uint16_t) ((w >> 8) | (w << 8));
}

CommBoard::CommBoard () :
	m68k_ram (RAM_SIZE),
	comm_ram (RAM_SIZE)
{
	reset ();
}

void
CommBoard::reset ()
{
	std::fill (m68k_ram.begin (), m68k_ram.end (), 0);
	std::fill (comm_ram.begin (), comm_ram.end (), 0);
	control = 0xC000;
	offset = status1 = status2 = 0;
}

uint32_t
CommBoard::get (uint32_t addr)
{
	switch (addr) {
	case COMM_CTRL:
		return control;
	case COMM_OFFSET:
		return offset;
	case COMM_DATA: {
		const std::vector<uint8_t> &ram = (control & CTRL_CPU_RAM) ? m68k_ram : comm_ram;
		uint16_t value = (uint16_t) (ram[offset] | (ram[(offset + 1) & 0xFFFF] << 8));

		offset += 2;
		return swap16 (value);
	}
	case COMM_STATUS1:
		return status1;
	case COMM_STATUS2:
		return status2;
	default:
		return 0xFFFFFFFF;
	}
}

void
CommBoard::put (uint32_t addr, uint32_t val)
{
	switch (addr) {
	case COMM_CTRL:
		if (!(control & CTRL_RESET) && (val & CTRL_RESET)) {
			memset (comm_ram.data (), 0, 32);
			status1 = status2 = 0;
		}
		control = (uint16_t) val;
		break;
	case COMM_OFFSET:
		offset = (uint16_t) val;
		break;
	case COMM_DATA: {
		std::vector<uint8_t> &ram = (control & CTRL_CPU_RAM) ? m68k_ram : comm_ram;
		uint16_t value = swap16 ((uint16_t) val);

		ram[offset] = (uint8_t) value;
		ram[(offset + 1) & 0xFFFF] = (uint8_t) (value >> 8);
		offset += 2;
		break;
	}
	case COMM_STATUS1:
		status1 = (uint16_t) val;
		break;
	case COMM_STATUS2:
		status2 = (uint16_t) val;
		break;
	default:
		break;
	}
}

bool
CommBoard::dma_to_ram (uint8_t *out, uint32_t size)
{
	uint32_t i;

	if (control & CTRL_G1DMA)
		return false;
	for (i = 0; i < size; i++)
		out[i] = comm_ram[(uint16_t) offset++];
	return true;
}

bool
CommBoard::dma_from_ram (const uint8_t *in, uint32_t size)
{
	uint32_t i;

	if (control & CTRL_G1DMA)
		return false;
	for (i = 0; i < size; i++)
		comm_ram[(uint16_t) offset++] = in[i];
	return true;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
CommBoard::save_state (hl::State *state)
{
	int ret = 0;

	SAVE (control);
	SAVE (offset);
	SAVE (status1);
	SAVE (status2);
	ret |= state->put (m68k_ram.data (), RAM_SIZE);
	ret |= state->put (comm_ram.data (), RAM_SIZE);
	return ret;
}

int
CommBoard::load_state (hl::State *state)
{
	int ret = 0;

	LOAD (control);
	LOAD (offset);
	LOAD (status1);
	LOAD (status2);
	ret |= state->get (m68k_ram.data (), RAM_SIZE);
	ret |= state->get (comm_ram.data (), RAM_SIZE);
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
