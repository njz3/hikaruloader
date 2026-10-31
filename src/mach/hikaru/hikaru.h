/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_HIKARU_H__
#define __HL_HIKARU_H__

#include <string>

#include "hl/mmap.h"
#include "hl/cpu.h"
#include "hl/machine.h"
#include "hl/jvs.h"
#include "hl/nvram.h"

namespace hikaru {

class Gpu;


typedef struct {
	bool has_rom;
	unsigned eprom_bank[2];
	unsigned maskrom_bank[2];
	unsigned eeprom_bank;
	uint32_t eprom_bank_size;
	uint32_t maskrom_bank_size;
	bool maskrom_is_stretched;
} hikaru_rombd_config_t;

/* The SEGA Hikaru: two SH-4s, their memory controllers, the GPU, the
 * MIE, two AICAs, and the ROM board. */
class Hikaru : public hl::Machine {
public:
	/* NULL when the game's ROMs or BIOS do not fit */
	static Hikaru *create (hl::Game *game);
	~Hikaru ();

	int run_frame () override;
	void on_reset (hl::ResetType type) override;
	int on_load_state (hl::State *state) override;
	int on_save_state (hl::State *state) override;
	const char *debug_string () override;
	void set_current_cpu (hl::Cpu *cpu) override { sh_current = cpu; }
	void raise_gpu_irq ();
	void raise_aica_irq ();
	void raise_memctl_irq ();
	/* The MAINBD's EEPROM and backup RAM, to the game's nvram file */
	void save_nvram ();


	/* CPU (master) */
	hl::Cpu *sh_m;
	hl::Mmap *mmap_m;

	/* CPU (slave) */
	hl::Cpu *sh_s;
	hl::Mmap *mmap_s;

	hl::Cpu *sh_current;

	/* Devices */
	hl::Device *memctl_m;
	hl::Device *memctl_s;
	hl::Device *mscomm;
	/* The communication board, linking the cabinets */
	hl::Device *comm;
	hl::Device *mie;
	/* The cabinet's JVS bus, and the I/O board on it; the drive board
	 * on that, NULL without */
	hl::JvsBus *jvs;
	hl::Device *io_board;
	hl::DriveBoard *drive_board;
	hl::Device *aica_m;
	hl::Device *aica_s;
	/* The cabinet's speakers (games.json's "audio"): what the frames'
	 * sound is made of */
	enum AudioSetup {
		AUDIO_STEREO,		/* the main board's */
		AUDIO_STEREO_MIX,	/* both boards' together */
		AUDIO_QUAD		/* the main board's, the option board's */
	} audio_setup;
	/* The SH-4s' cycles a line, from their clock (SH4_CLOCK) */
	unsigned cycles_per_line;
	Gpu *gpu;
	/* What the bridges' local buses reach without a device of its own
	 * (formerly unk_m and unk_s); see hikaru.cpp */
	hl::Device *local_m;
	hl::Device *local_s;

	/* Port A (master and slave) */
	uint16_t porta_m;
	unsigned porta_m_bit0_buffer;

	uint16_t porta_s;
	unsigned porta_s_bit1_buffer;

	/* RAM areas */
	hl::Buffer *ram_m, *ram_s;
	hl::Buffer *cmdram;
	hl::Buffer *texram[2];
	hl::Buffer *fb;
	hl::Buffer *aica_ram_m;
	hl::Buffer *aica_ram_s;
	hl::Buffer *mie_ram;
	/* The backup RAM, a battery's: the games' records and bookkeeping */
	hl::Buffer *bram;

	/* What nvram/<game>.nvram holds, as last loaded or saved; its path,
	 * empty when nothing is saved */
	hl::Nvram nvram;
	std::string nvram_path;
	/* The frames before the EEPROM's save (none when 0); those since the
	 * backup RAM was last looked at */
	unsigned nvram_due = 0;
	unsigned nvram_frames = 0;

	/* ROM data */
	hl::Buffer *bootrom;
	hl::Buffer *eprom;
	hl::Buffer *maskrom;

	/* ROMBD configuration */
	hikaru_rombd_config_t rombd_config;


private:
	explicit Hikaru (hl::Game *game);
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_H__ */
