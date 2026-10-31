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

#ifndef __ARM7_H__
#define __ARM7_H__

#include "hl/core.h"

/* An ARM7 (ARMv4, the ARM state alone: no Thumb), interpreted: data
 * processing with the barrel shifter, the multiplies (long ones too), the
 * word, byte and halfword transfers, the block transfers, SWP, MRS and MSR,
 * the branches; the modes and their banked registers, the exceptions, the
 * IRQ and FIQ lines. Cycles are approximate. Its memory is its owner's,
 * through an Arm7::Bus; it keeps no state outside the object, so a machine
 * may have several (the Hikaru's two AICAs each have one). */
class Arm7 {
public:
	/* What the ARM7 is wired to. Addresses are as the code gives them:
	 * aligned, the halfword and word ones (the core aligns them) */
	class Bus {
	public:
		virtual ~Bus () {}
		virtual uint32_t read32 (uint32_t addr) = 0;
		virtual uint16_t read16 (uint32_t addr) = 0;
		virtual uint8_t	 read8 (uint32_t addr) = 0;
		virtual void	 write32 (uint32_t addr, uint32_t val) = 0;
		virtual void	 write16 (uint32_t addr, uint16_t val) = 0;
		virtual void	 write8 (uint32_t addr, uint8_t val) = 0;
	};

	/* The modes, in CPSR's low five bits */
	enum Mode {
		MODE_USR = 0x10,
		MODE_FIQ = 0x11,
		MODE_IRQ = 0x12,
		MODE_SVC = 0x13,
		MODE_ABT = 0x17,
		MODE_UND = 0x1B,
		MODE_SYS = 0x1F
	};

	/* CPSR's bits */
	enum {
		PSR_N = 1u << 31,
		PSR_Z = 1u << 30,
		PSR_C = 1u << 29,
		PSR_V = 1u << 28,
		PSR_I = 1u << 7,	/* IRQs off */
		PSR_F = 1u << 6,	/* FIQs off */
		PSR_T = 1u << 5,	/* Thumb: not emulated */
		PSR_MODE = 0x1F
	};

	explicit Arm7 (Bus *bus);

	/* The bus' RAM from address 0, size bytes, read and written here
	 * without the bus (little-endian, as the hosts are): faster. The bus
	 * still has the rest */
	void	 set_ram (uint8_t *ptr, uint32_t size) { ram = ptr; ram_size = size; }

	/* What the RESET pin does: the SVC mode, IRQs and FIQs off, from 0 */
	void	 reset ();
	/* Runs instructions until at least cycles cycles have gone; returns
	 * how many did */
	int	 run (int cycles);
	/* The sound drivers' scan loops (LDRB, TST #mask, BNE out, ADD
	 * stride, ADD count, CMP limit, BNE back: 7 instructions, 11 cycles a
	 * turn) run natively, as many whole turns as run () would take: the
	 * same registers, flags and cycles as one instruction at a time. Off
	 * by default */
	bool	 fast_loops = false;
	/* One instruction, or the taking of an interrupt: its cycles */
	int	 step ();

	/* The nIRQ and nFIQ pins, level-triggered: held, the ARM takes the
	 * interrupt as soon as its CPSR lets it */
	void	 set_irq (bool held) { regs.irq_line = held; }
	void	 set_fiq (bool held) { regs.fiq_line = held; }

	/* r[15] is the address of the next instruction to run */
	uint32_t pc () const { return regs.r[15]; }

	/* The registers: plain data, for the save states. r[] are the current
	 * mode's; the others' banked registers wait in the banks */
	struct Registers {
		uint32_t r[16];
		uint32_t cpsr;
		uint32_t usr_r8_14[7];	/* also SYS's */
		uint32_t fiq_r8_14[7];
		uint32_t irq_r13_14[2];
		uint32_t svc_r13_14[2];
		uint32_t abt_r13_14[2];
		uint32_t und_r13_14[2];
		uint32_t spsr_fiq, spsr_irq, spsr_svc, spsr_abt, spsr_und;
		bool	 irq_line;
		bool	 fiq_line;
		uint64_t cycles;
	} regs;

private:
	/* The current mode's SPSR; NULL in USR and SYS, which have none */
	uint32_t *spsr ();
	/* Moves the banked registers for a change of mode */
	void	 switch_mode (uint32_t mode);
	/* r8-r14 of the user mode, for the LDM and STM with the S bit */
	uint32_t get_user_reg (unsigned n);
	void	 set_user_reg (unsigned n, uint32_t val);
	/* Enters an exception: the mode, LR, the SPSR, the vector */
	void	 exception (uint32_t mode, uint32_t vector, uint32_t lr, bool fiq_off);

	bool	 condition (uint32_t cond) const;
	/* Operand 2 of a data processing instruction, and the shifter's carry */
	uint32_t shifter_operand (uint32_t inst, bool *carry);
	/* A register as an operand: r15 reads ahead (8, 12 for a shift by a
	 * register) */
	uint32_t get_reg (unsigned n, uint32_t ahead = 8) const;
	void	 set_reg (unsigned n, uint32_t val);

	uint32_t read32 (uint32_t addr);
	void	 write32 (uint32_t addr, uint32_t val);
	/* The RAM's, or the bus'. Aligned addresses */
	uint32_t load32 (uint32_t addr)
	{
		uint32_t val;
		if (addr < ram_size) {
			memcpy (&val, ram + addr, 4);
			return val;
		}
		return bus->read32 (addr);
	}
	uint16_t load16 (uint32_t addr)
	{
		uint16_t val;
		if (addr < ram_size) {
			memcpy (&val, ram + addr, 2);
			return val;
		}
		return bus->read16 (addr);
	}
	uint8_t	 load8 (uint32_t addr)
	{
		return (addr < ram_size) ? ram[addr] : bus->read8 (addr);
	}
	void	 store32 (uint32_t addr, uint32_t val)
	{
		if (addr < ram_size)
			memcpy (ram + addr, &val, 4);
		else
			bus->write32 (addr, val);
	}
	void	 store16 (uint32_t addr, uint16_t val)
	{
		if (addr < ram_size)
			memcpy (ram + addr, &val, 2);
		else
			bus->write16 (addr, val);
	}
	void	 store8 (uint32_t addr, uint8_t val)
	{
		if (addr < ram_size)
			ram[addr] = val;
		else
			bus->write8 (addr, val);
	}

	int	 execute (uint32_t inst);
	int	 data_processing (uint32_t inst);
	int	 psr_transfer (uint32_t inst);
	int	 multiply (uint32_t inst);
	int	 multiply_long (uint32_t inst);
	int	 swap (uint32_t inst);
	int	 halfword_transfer (uint32_t inst);
	int	 single_transfer (uint32_t inst);
	int	 block_transfer (uint32_t inst);
	int	 branch (uint32_t inst);
	int	 undefined (uint32_t inst);

	/* fast_loops: the scan loop last found, its words and fields (head 0:
	 * none), and the last loop it was not */
	struct ScanLoop {
		uint32_t head, rejected;
		uint32_t words[7];
		unsigned rd, rn, rc;
		uint32_t mask, stride, inc, limit;
	} scan = {};
	void	 find_scan_loop (uint32_t head);
	/* Whole turns, within budget cycles; their cycles */
	int	 run_scan_loop (int budget);

	Bus	*bus;
	uint8_t	*ram;
	uint32_t ram_size;
	/* Each condition's truth, a bit for each NZCV */
	uint16_t conditions[16];
};

#endif /* __ARM7_H__ */
