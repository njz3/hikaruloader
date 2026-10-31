/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-mie.h, Copyright (C) 2011, Stefano Teso (released under the GNU GPL version 2 or later).
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

#ifndef __HL_HIKARU_MIE_H__
#define __HL_HIKARU_MIE_H__

#include <vector>

#include "hl/device.h"
#include "hl/buffer.h"
#include "hl/machine.h"
#include "hl/jvs.h"
#include "hl/eeprom.h"
#include "cpu/z80/z80.h"

namespace hikaru {

/* The MIE (315-6146): the main board's I/O controller. A Z80 running the
 * program the game puts in its RAM, which drives the JVS bus through a UART;
 * GPIO ports with the MAINBD switches and a 93C46 EEPROM, which the SH-4
 * sees too; see hikaru-mie.cpp. */
class Mie : public hl::Device, public Z80::Bus {
public:
	/* ram: the Z80's 8000-FFFF, which the SH-4 sees too */
	Mie (hl::Machine *mach, hl::Buffer *ram, hl::JvsBus *jvs);
	~Mie ();

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	/* cycles: the SH-4's */
	int exec (int cycles) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;
	/* "log_ports": the Z80's port accesses, to the log */
	bool set_option (const char *name, int value) override;
	/* HR_ARM_PROFILE's for the Z80 (HR_Z80_PROFILE=1): its busiest
	 * addresses, to the log; once, while its RAM is there */
	void report_z80_profile ();

	/* The Z80's side */
	uint8_t read (uint16_t addr) override;
	void write (uint16_t addr, uint8_t val) override;
	uint8_t in (uint16_t port) override;
	void out (uint16_t port, uint8_t val) override;
	uint8_t acknowledge () override;

	hl::Buffer *regs;
	/* The Z80's 7800-7FFF and 8000-FFFF */
	hl::Buffer *ram_lo, *ram;
	hl::JvsBus *jvs;
	Z80 *z80;
	/* On GPIO F and G; kept with the backup RAM in nvram/<game>.nvram
	 * (Hikaru::save_nvram) */
	hl::Eeprom93C46 eeprom;
	/* Valkyrie's: no Z80, the mailbox faked */
	bool hack;

	/* The Z80's ports */
	struct {
		/* The UART to the JVS bus, a 16550 */
		uint8_t lcr, ier, fcr, mcr, scr, dll, dlm;
		/* 90: bit 0 drives the RS-485 line; its fall ends a packet */
		uint8_t jvs_control;
		/* 30, 50, 70: the interrupts, levels 0 to 6 */
		uint8_t irq_enable, irq_level, irq_pending;
		uint8_t gpio[8], gpio_dir;
		/* Those nobody knows yet, as written */
		uint8_t other[256];
	} io;
	/* The Z80's T-states owed */
	int owed;
	/* The SH-4s hold the Z80's bus (BUSRQ, granted at once): the Z80
	 * stops; see bus_get () */
	bool busrq;
	/* The Z80's T-states per 100 SH-4 cycles */
	int speed;
	bool log_ports;
	/* HR_Z80_PROFILE: the T-states spent at each address */
	std::vector<uint32_t> z80_pcs;
	/* The Z80's idle loops, their turns counted at once (MIE_IDLE_SKIP=0
	 * in the environment: each instruction run); see run_z80 () */
	bool idle_skip;
	struct {
		/* A head the Z80 came back to, as it was there */
		uint16_t head;
		bool valid;
		/* One turn found: back as it was, nothing changed */
		bool confirmed;
		/* A write, a port, the SH-4's write since the head: the next
		 * turn may go elsewhere */
		bool touched;
		Z80::Registers regs;
		/* A turn's T-states, and R's count */
		int t;
		uint8_t r;
	} idle;

private:
	void run_z80 ();
	void came_back ();
	void update_irq ();
	void raise_irq (unsigned level);
	void write_lo (uint16_t addr, uint8_t val);
	/* The ports, for the Z80 and the SH-4 */
	uint8_t port_read (uint8_t port);
	void port_write (uint8_t port, uint8_t val);
	uint8_t gpio_read (unsigned port);
	void gpio_write (unsigned port, uint8_t val);
	void gpio_set_dir (uint8_t dir);
	void drive (unsigned port);
	void save_eeprom ();
	/* The SH-4's 00800000-0081FFFF */
	uint8_t sh4_read (uint8_t port);
	void sh4_write (uint8_t port, uint8_t val);
	int get_hack (unsigned size, uint32_t addr, void *val);

public:
	/* The bus request, on the local bus at 02000000 (all 16 MB alike):
	 * bit 8 written requests the Z80's bus, bit 8 read is clear once it
	 * is granted (BUSAK) */
	uint16_t bus_get () const { return (busrq ? 0 : 0x100) | 0x600; }
	void bus_put (uint64_t val) { busrq = (val >> 8) & 1; }
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_MIE_H__ */
