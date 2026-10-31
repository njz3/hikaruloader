/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/gaelco/atvtrack.cpp (license BSD-3-Clause,
 * copyright-holders Samuele Zannoli).
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

#include "hl/core.h"
#include "hl/machine.h"
#include "hl/state.h"

#include "cpu/sh/sh4.h"

#include "mach/gaelco/gaelco.h"
#include "mach/gaelco/gaelco-gpu.h"

namespace gaelco {

/*
 * The Neon 250's Registers
 * ========================
 *
 * The slave SH-4 sees them at 14000000; what is known, from what the games
 * wait for:
 *
 *  0000	Written non-zero to start the chip; the games then wait for 1
 *		at VRAM+1350, which the chip's own processor would answer
 *  0070	Interrupts pending; written, clears those bits
 *  0074	Interrupts masked (all at reset)
 *  00B0	0001: draws the frame, interrupt 7 when done (at once here);
 *		8001: the games wait for 1 at VRAM+814804
 *  4xxx	The PCI configuration
 *
 * Its interrupt is the slave's IRL pins at 14 (level 1).
 */

enum {
	REG_START	= 0x0000,
	REG_PENDING	= 0x0070,
	REG_MASK	= 0x0074,
	REG_RENDER	= 0x00B0,
};

/* The IRL pins the Neon 250 drives on the slave */
static const unsigned NEON_IRL_PINS = 14;

Neon250::Neon250 (hl::Machine *mach, hl::Buffer *vram) :
	hl::Device (mach),
	vram (vram)
{
}

void
Neon250::reset (hl::ResetType type)
{
	(void) type;
	frames = 0;
	pending = 0;
	mask = 0xFFFF;
	memset (regs, 0, sizeof (regs));
}

void
Neon250::update_irq ()
{
	Gaelco *gaelco = (Gaelco *) mach;
	gaelco->set_irl (gaelco->sh_s, NEON_IRL_PINS, (pending & ~mask) != 0, this);
}

void
Neon250::raise (unsigned bit)
{
	pending |= 1 << bit;
	update_irq ();
}

int
Neon250::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t offs = addr & 0x7FFF;
	uint32_t data;

	switch (offs) {
	case REG_PENDING:
		data = pending;
		break;
	case REG_MASK:
		data = mask;
		break;
	default:
		/* 0, as the games accept */
		data = 0;
		if (!(seen_r[offs / 4 / 8] & (1 << ((offs / 4) & 7)))) {
			seen_r[offs / 4 / 8] |= 1 << ((offs / 4) & 7);
			HL_PRINT ("neon250: frame %llu: first read of %04X",
			          (unsigned long long) mach->frame, offs);
		}
		break;
	}
	set_ptr (val, size, data);
	return 0;
}

int
Neon250::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0x7FFF;
	uint32_t data = (uint32_t) val;

	(void) size;
	if (offs < sizeof (regs))
		regs[offs / 4] = data;
	/* What the games use, for the reverse engineering */
	if (offs < sizeof (regs) && !(seen_w[offs / 4 / 8] & (1 << ((offs / 4) & 7)))) {
		seen_w[offs / 4 / 8] |= 1 << ((offs / 4) & 7);
		HL_PRINT ("neon250: frame %llu: first write of %04X = %08X",
		          (unsigned long long) mach->frame, offs, data);
	}

	switch (offs) {
	case REG_START:
		if (data)
			vram->put (1, 0x1350, 1);
		break;
	case REG_PENDING:
		pending &= ~data;
		update_irq ();
		break;
	case REG_MASK:
		mask = data;
		update_irq ();
		break;
	case REG_RENDER:
		if (data == 0x0001) {
			if (!frames)
				HL_PRINT ("neon250: frame %llu: the first frame drawn",
				          (unsigned long long) mach->frame);
			frames++;
			raise (7);
		} else if (data == 0x8001)
			vram->put (1, 0x814804, 1);
		break;
	default:
		break;
	}
	return 0;
}

const char *
Neon250::debug_string ()
{
	static char out[64];
	snprintf (out, sizeof (out), "NEON pending %04X mask %04X frames %u",
	          pending, mask, frames);
	return out;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Neon250::save_state (hl::State *state)
{
	int ret = 0;
	SAVE (frames);
	SAVE (pending);
	SAVE (mask);
	SAVE (regs);
	return ret;
}

int
Neon250::load_state (hl::State *state)
{
	int ret = 0;
	LOAD (frames);
	LOAD (pending);
	LOAD (mask);
	LOAD (regs);
	return ret;
}

#undef SAVE
#undef LOAD

void
Renderer::on_end_frame ()
{
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (0, 0, width, height);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
}

} /* namespace gaelco */
