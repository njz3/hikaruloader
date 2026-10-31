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

#ifndef __HL_GAELCO_GPU_H__
#define __HL_GAELCO_GPU_H__

#include "hl/device.h"
#include "hl/renderer.h"

namespace gaelco {

/* The NEC PowerVR Neon 250 (PowerVR series 2) on the slave SH-4: its
 * registers at 14000000-14003FFF, its PCI configuration at 14004xxx, its
 * 32 MB at 18000000. Only what lets the games go on is known: the
 * interrupts (70 pending, 74 masked), a frame drawn when B0 is written 1
 * (its interrupt 7, at once). Nothing is drawn yet */
class Neon250 : public hl::Device {
public:
	Neon250 (hl::Machine *mach, hl::Buffer *vram);

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;

	/* The frames whose drawing the games started, from the reset */
	uint32_t frames;

private:
	void raise (unsigned bit);
	void update_irq ();

	hl::Buffer *vram;
	uint16_t pending;
	uint16_t mask;
	/* What was written, for the log and the Lua shell */
	uint32_t regs[0x5000 / 4];
	/* The registers read and written so far, logged the first time */
	uint8_t seen_r[0x5000 / 4 / 8];
	uint8_t seen_w[0x5000 / 4 / 8];
};

/* Gaelco's renderer: the window black until the Neon 250 draws */
class Renderer : public hl::Renderer {
public:
	Renderer () : hl::Renderer (640, 480) {}

protected:
	void on_end_frame () override;
};

} /* namespace gaelco */

#endif /* __HL_GAELCO_GPU_H__ */
