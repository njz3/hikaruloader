/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/multiboard.cpp (GPL-2.0-or-later,
 * Copyright 2023 flyinghead).
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
#include "hl/machine.h"

#include "cpu/sh/sh4.h"
#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Multiboard Link
 * ===================
 *
 * The boards of a cabinet of several screens. The master has the cartridge; a
 * slave only its BIOS: it gets its program through a memory of 1 MB in two
 * banks, which the master writes on its G2 bus and the slaves read on their G1
 * bus. The master's status register, at 01008000:
 *
 *  0	the bank the master writes (the slaves the other one); a slave whose bit
 *	below is set is interrupted when it changes
 *  1-3	the slaves 1, 2, 3 selected
 *  4-7	each board ready (written by the slaves at 005F705C)
 *
 * Here the boards are objects of one process, each run by its thread; they all
 * stop every 8 lines (about 64 times a frame: the BIOS names the slaves with
 * questions they must answer within a millisecond), when sync () runs.
 *
 * Within a slice the threads go at their own pace, so what a board sees of
 * another's moves waits for that stop: a slave reads and writes the bank of
 * the status it last saw there, and its ready bit's changes reach the master
 * there. Read live, the exchanges hung on which thread got there first:
 * Airline Pilots' master waited for its slaves forever in most runs ("Please
 * Wait until Slaves synchronize..."), so neither the game nor its sound
 * driver ever started.
 */

enum {
	G1_OFFSET	= 0x5F7050,
	G1_DATA		= 0x5F7054,
	G1_DMA_OFFSET	= 0x5F7058,
	G1_STATUS	= 0x5F705C,
	G1_BOARD_ID	= 0x5F706C,
	G1_CONFIG_SLOT	= 0x5F70C4,
	G1_BASE		= 0x5F7080,
	G2_BASE		= 0x1010000,
	G2_STATUS	= 0x1008000,
	G2_MEMORY	= 0x1020000,
};

static const uint32_t MEM_WORDS = 0x100000 / 2;

/* The Holly's external interrupt of the G1 bus's device */
static const uint32_t EXT_GDROM = 1;

MultiboardPort::MultiboardPort (Board *board_, Multiboard *link_, unsigned id_) :
	hl::Device (board_->machine),
	board (board_),
	link (link_),
	id (id_)
{
}

void
MultiboardPort::reset (hl::ResetType type)
{
	(void) type;
	offset = 0;
	dma_offset = 0;
	seen_status = 0;
	ready_due = unready_due = false;
	if (!master ())
		link->status.fetch_and ((uint16_t) ~(0x10 << id));
	else
		link->status = 0;
}

bool
MultiboardPort::g1_get (uint32_t addr, uint32_t *val)
{
	switch (addr) {
	case G1_OFFSET:
		*val = offset;
		return true;
	case G1_DATA: {
		uint32_t bank = (bank_status () & 1) ? 0 : 0x80000, at;

		if (master ()) {
			bank = 0x80000 - bank;
			at = offset + bank / 2;
		} else
			at = (offset & 0xFFFF) + (id - 1) * 0x10000 + bank / 2;
		*val = link->data[at & (MEM_WORDS - 1)];
		offset++;
		return true;
	}
	case G1_DMA_OFFSET:
		*val = dma_offset;
		return true;
	case G1_BOARD_ID:
		/* Bit 3 the link's transport, bit 4 its speed */
		*val = id | 0x10;
		return true;
	case G1_BASE + 0x08:
		*val = 0x80;
		return true;
	case G1_BASE + 0x10:
		*val = 0x60;
		return true;
	case G1_BASE + 0x14:
		*val = 0x43;
		return true;
	case G1_CONFIG_SLOT:
		*val = id;
		return true;
	default:
		return false;
	}
}

bool
MultiboardPort::g1_put (uint32_t addr, uint32_t val)
{
	switch (addr) {
	case G1_OFFSET:
		offset = val;
		return true;
	case G1_DATA: {
		uint32_t bank = (bank_status () & 1) ? 0 : 0x80000, at;

		if (master ()) {
			bank = 0x80000 - bank;
			at = offset + bank / 2;
		} else
			at = (offset & 0xFFFF) + (id - 1) * 0x10000 + bank / 2;
		link->data[at & (MEM_WORDS - 1)] = (uint16_t) val;
		offset++;
		return true;
	}
	case G1_DMA_OFFSET:
		dma_offset = (uint16_t) val;
		return true;
	case G1_STATUS:
		if (!master ()) {
			if (val == 3)
				board->holly->set_external (EXT_GDROM, false);
			else if (val == 4)
				ready_due = true;
		}
		return true;
	case G1_CONFIG_SLOT:
		return true;
	default:
		return false;
	}
}

void
MultiboardPort::software_reset ()
{
	if (!master ()) {
		ready_due = false;
		unready_due = true;
	}
}

bool
MultiboardPort::dma (uint32_t size, uint8_t *out)
{
	uint32_t bank, from;

	if (master () || board->has_cart)
		return false;
	bank = (bank_status () & 1) ? 0 : 0x80000;
	from = (id - 1) * 0x10000 + bank / 2 + dma_offset;
	if (from + size / 2 > MEM_WORDS)
		size = (MEM_WORDS - from) * 2;
	memcpy (out, &link->data[from], size);
	return true;
}

int
MultiboardPort::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t a = addr & 0x01FFFFFF, data = 0;

	switch (a) {
	case G2_BASE + 0x08:
		data = 0x80;
		break;
	case G2_BASE + 0x10:
		data = 0x60;
		break;
	case G2_BASE + 0x14:
		data = 0x43;
		break;
	case G2_BASE + 0xC0:
		data = id | 0x10;
		break;
	case G2_STATUS:
		data = master () ? (0xFF00u | link->status) : 0;
		break;
	default:
		if (a >= G2_MEMORY && a - G2_MEMORY < MEM_WORDS * 2 && master ()) {
			uint32_t bank = (link->status & 1) ? 0x80000 : 0;
			uint32_t at = (a - G2_MEMORY + bank) / 2;

			at &= MEM_WORDS - 1;
			data = link->data[at];
			if (size >= 4)
				data |= (uint32_t) link->data[(at + 1) & (MEM_WORDS - 1)] << 16;
		}
		break;
	}
	set_ptr (val, size, data);
	return 0;
}

int
MultiboardPort::put (unsigned size, uint32_t addr, uint64_t val_)
{
	uint32_t a = addr & 0x01FFFFFF, val = (uint32_t) val_;

	if (a == G2_STATUS) {
		if (master ())
			link->status = (uint16_t) val;
	} else if (a >= G2_MEMORY && a - G2_MEMORY < MEM_WORDS * 2 && master ()) {
		uint32_t bank = (link->status & 1) ? 0x80000 : 0;
		uint32_t at = (a - G2_MEMORY + bank) / 2;
		uint16_t status = link->status;
		auto store = [&] (uint32_t where) {
			where &= MEM_WORDS - 1;
			link->data[where] = (uint16_t) val;
			if (size >= 4)
				link->data[(where + 1) & (MEM_WORDS - 1)] = (uint16_t) (val >> 16);
		};

		if ((status & 2) || a >= 0x1040000) {
			unsigned slot = (a - G2_MEMORY) / 0x20000 + 1;

			/* Only the boards there have a memory */
			if (slot < link->boards)
				store (at);
		}
		if (a < 0x1040000) {
			if ((status & 4) && link->boards >= 3)
				store (at + 0x10000);
			if ((status & 8) && link->boards >= 4)
				store (at + 0x20000);
		}
	}
	return 0;
}

void
MultiboardPort::sync ()
{
	uint16_t now;

	if (master ())
		return;
	if (unready_due)
		link->status.fetch_and ((uint16_t) ~(0x10 << id));
	if (ready_due)
		link->status.fetch_or ((uint16_t) (0x10 << id));
	ready_due = unready_due = false;
	now = link->status;
	if (((now ^ seen_status) & 1) && ((1 << id) & now))
		board->holly->set_external (EXT_GDROM, true);
	seen_status = now;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
MultiboardPort::save_state (hl::State *state)
{
	int ret = 0;
	uint16_t status = link->status;

	SAVE (offset);
	SAVE (dma_offset);
	SAVE (seen_status);
	SAVE (status);
	if (master ())
		ret |= state->put (link->data.data (), MEM_WORDS * 2);
	return ret;
}

int
MultiboardPort::load_state (hl::State *state)
{
	int ret = 0;
	uint16_t status = 0;

	LOAD (offset);
	LOAD (dma_offset);
	LOAD (seen_status);
	LOAD (status);
	if (master ()) {
		link->status = status;
		ret |= state->get (link->data.data (), MEM_WORDS * 2);
	}
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
