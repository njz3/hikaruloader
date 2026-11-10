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

#include <cstring>

#include "hl/core.h"
#include "hl/state.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The 837-14381 "G2 Expansion Board"
 * ==================================
 *
 * Under the medal games' board (Dragon Treasure, the SWP hopper board): MAME
 * notes the part and the games that need it, and nothing documents it. What is
 * here is what their programs show on the G2 bus's outside (01000000):
 *
 *  004000-004003	"GAPS", the board's name, as the games compare it
 *  004020-00402F	Registers, read back as they were written
 *  180000-18FFFF	64 KB of battery-backed RAM: the bookkeeping
 *
 * The rest of the board, the medal mechanisms' ports, is not known.
 */

enum {
	NAME		= 0x004000,
	NAME_END	= 0x004004,
	REGS		= 0x004020,
	RAM		= 0x180000,
	RAM_SIZE	= 0x10000,
};

G2Board::G2Board (hl::Machine *mach) :
	hl::Device (mach)
{
	memset (ram, 0, sizeof (ram));
	memset (regs, 0, sizeof (regs));
}

void
G2Board::reset (hl::ResetType type)
{
	(void) type;
	memset (regs, 0, sizeof (regs));
}

int
G2Board::get (unsigned size, uint32_t addr, void *val)
{
	static const char name[] = "GAPS";
	uint32_t offs = addr & 0xFFFFFF;
	uint64_t v = 0xFFFFFFFFFFFFFFFFull;

	if (offs >= RAM && offs < RAM + RAM_SIZE && offs + size <= RAM + RAM_SIZE) {
		v = 0;
		memcpy (&v, ram + (offs - RAM), size);
	} else if (offs >= NAME && offs < NAME_END && size == 2)
		v = (uint8_t) name[offs - NAME] | ((uint8_t) name[offs - NAME + 1] << 8);
	else if (offs >= REGS && offs < REGS + sizeof (regs) && size == 2)
		v = regs[(offs - REGS) / 2];
	if (log)
		HL_PRINT ("g2 board: read %u at %06X: %llX", size, offs, (unsigned long long) v);
	set_ptr (val, size, v);
	return 0;
}

int
G2Board::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0xFFFFFF;

	if (log)
		HL_PRINT ("g2 board: write %u at %06X: %llX", size, offs, (unsigned long long) val);
	if (offs >= RAM && offs < RAM + RAM_SIZE && offs + size <= RAM + RAM_SIZE) {
		memcpy (ram + (offs - RAM), &val, size);
		changed = true;
	} else if (offs >= REGS && offs < REGS + sizeof (regs) && size == 2)
		regs[(offs - REGS) / 2] = (uint16_t) val;
	return 0;
}

int
G2Board::save_state (hl::State *state)
{
	int ret = state->put (ram, sizeof (ram));

	ret |= state->put (regs, sizeof (regs));
	return ret;
}

int
G2Board::load_state (hl::State *state)
{
	int ret = state->get (ram, sizeof (ram));

	ret |= state->get (regs, sizeof (regs));
	return ret;
}

bool
G2Board::set_option (const char *name, int value)
{
	if (strcmp (name, "log"))
		return false;
	log = value != 0;
	return true;
}

} /* namespace naomi */
