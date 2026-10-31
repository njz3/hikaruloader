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

#ifndef __HL_GAELCO_H__
#define __HL_GAELCO_H__

#include "hl/mmap.h"
#include "hl/cpu.h"
#include "hl/machine.h"

namespace gaelco {

class Neon250;
class Fpga;
class Nand;

/* Gaelco's PowerVR boards: ATV Track and Gaelco Football (REF. 020419,
 * 020201), Smashing Drive (REF. 010131). Two SH-4s at 200 MHz, the
 * slave's NEC PowerVR Neon 250, an Altera FPGA for the shared RAM, the
 * inputs and the 32 kHz DAC; see gaelco.cpp */
class Gaelco : public hl::Machine {
public:
	/* NULL when the game's ROMs do not fit */
	static Gaelco *create (hl::Game *game);
	~Gaelco ();

	int run_frame () override;
	void on_reset (hl::ResetType type) override;
	int on_load_state (hl::State *state) override;
	int on_save_state (hl::State *state) override;
	const char *debug_string () override;
	void set_current_cpu (hl::Cpu *cpu) override { sh_current = cpu; }

	/* A CPU's IRL pins at pins (encoded: 0 level 15 ... 14 level 1),
	 * driven or let go */
	void set_irl (hl::Cpu *cpu, unsigned pins, bool on, hl::Device *from);
	/* The FPGA's control register: the slave runs while its bit 2 is set,
	 * once port A has started it */
	void control_put (uint32_t val);

	enum Board {
		BOARD_ATVTRACK,		/* REF. 020419: the NAND chips */
		BOARD_SMASHDRV		/* REF. 010131: the ROMs mapped */
	} board;

	/* CPUs */
	hl::Cpu *sh_m;
	hl::Cpu *sh_s;
	hl::Cpu *sh_current;
	hl::Mmap *mmap_m;
	hl::Mmap *mmap_s;

	/* Devices */
	Fpga *fpga;
	Nand *nand;		/* NULL on Smashing Drive */
	hl::Device *flash;	/* Smashing Drive's data flash, NULL else */
	Neon250 *gpu;

	/* The SH-4s' cycles a line, from their clock (SH4_CLOCK) */
	unsigned cycles_per_line;

	/* Port A: the master's selects the FPGA's ports (gaelco.cpp) */
	uint16_t porta_m;
	uint16_t porta_s;
	/* Set by port A (7800, bit 8): the slave may run */
	bool slave_enabled;
	/* The slave held in its reset */
	bool slave_held;

	/* The ADC (wheel, pedals): an ADC0834-like serial chip */
	struct {
		bool cs, clk;
		unsigned state;		/* the clock edges since CS went low */
		unsigned mux;		/* the bits written: start, SGL, ODD, SEL1 */
		uint8_t value;		/* the sample being sent */
		bool out;		/* DO */
	} adc;

	/* Memories */
	hl::Buffer *shared;	/* 1 KB in the FPGA, both CPUs' */
	hl::Buffer *ram_m;
	hl::Buffer *ram_s;
	hl::Buffer *vram;	/* the Neon 250's 32 MB */
	hl::Buffer *nand_data;	/* ATV Track's 4 NAND chips, interleaved */
	hl::Buffer *fpga_conf;	/* the EPC1PC8: the FPGA's configuration */
	hl::Buffer *rom;	/* Smashing Drive's program, 64 MB */
	hl::Buffer *data_rom;	/* and its data, 4 MB */

	explicit Gaelco (hl::Game *game);
};

} /* namespace gaelco */

#endif /* __HL_GAELCO_H__ */
