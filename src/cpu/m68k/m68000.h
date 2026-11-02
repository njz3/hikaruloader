/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cpu/m68000/m68000.hpp,
 * Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __M68000_H__
#define __M68000_H__

#include "hl/core.h"

/* The Motorola 68000, interpreted: its instructions, exceptions and bus
 * cycles as the M68000 User's Manual gives them, in the order of the
 * SingleStepTests 680x0 suite; every bus cycle takes 4 clocks (no wait
 * state). The communication board's CPU (hikaru-commbd.cpp). Its memory is
 * its owner's, through an M68000::Bus. */
class M68000 {
public:
	/* What the CPU sees outside its pins: words at even addresses, a 24-bit
	 * address */
	class Bus {
	public:
		virtual ~Bus () {}
		virtual uint16_t read16 (uint32_t addr) = 0;
		virtual uint8_t	 read8 (uint32_t addr) = 0;
		virtual void	 write16 (uint32_t addr, uint16_t val) = 0;
		virtual void	 write8 (uint32_t addr, uint8_t val) = 0;
		/* The interrupt acknowledge at a level: a vector number, or -1
		 * when the device answers with VPA (an autovector) */
		virtual int	 int_ack (unsigned level) { (void) level; return -1; }
		/* The RESET instruction: the devices' reset line pulses */
		virtual void	 reset_devices () {}
	};

	/* The status register's bits */
	enum {
		SR_C	= 0x0001,
		SR_V	= 0x0002,
		SR_Z	= 0x0004,
		SR_N	= 0x0008,
		SR_X	= 0x0010,
		SR_I	= 0x0700,	/* the interrupt mask */
		SR_S	= 0x2000,	/* supervisor */
		SR_T	= 0x8000,	/* trace */
		SR_MASK	= 0xA71F
	};

	explicit M68000 (Bus *bus);

	/* The reset exception: SSP and PC from the first two long words */
	void	 reset ();
	/* Runs whole instructions until at least cycles clocks have gone;
	 * returns how many did */
	uint32_t run (uint32_t cycles);
	/* Takes a pending interrupt or runs one instruction (a stopped or
	 * halted CPU waits 4 clocks): its clocks */
	uint32_t step ();

	/* The level on the IPL pins, 0 to 7; 7 is taken on its rising edge,
	 * whatever the mask */
	void	 set_ipl (unsigned level);
	/* For an owner that sets an event in the middle of a run (a write of
	 * its program starts a timer): the run ends on the first instruction
	 * that starts at or after cycles (as regs.cycles counts them) */
	void	 end_run_at (uint64_t cycles) { if (cycles < run_end) run_end = cycles; }

	/* What the 68000 keeps between two instructions: plain data, for the
	 * save states. pc is the address of the word in ir, irc the word
	 * after it (the prefetch) */
	struct Registers {
		uint32_t d[8];
		uint32_t a[8];		/* a[7]: the stack pointer in use */
		uint32_t other_sp;	/* USP in supervisor mode, SSP in user mode */
		uint32_t pc;
		uint16_t sr;
		uint16_t ir, irc;
		uint8_t	 ipl;		/* the level on the interrupt pins */
		bool	 nmi_edge;	/* level 7 seen rising, not taken yet */
		bool	 stopped, halted;
		uint64_t cycles;
	} regs;

	/* After loading regs: the prefetch's address follows pc */
	void	 loaded () { au = regs.pc + 4; faulting = false; }

private:
	enum Size { BYTE, WORD, LONG };

	/* A word or long access at an odd address: the instruction stops and
	 * the address error exception runs instead. The access does nothing,
	 * nor any after it; the registers go back to what they were at it
	 * when the instruction returns */
	struct AddressError {
		uint32_t addr;
		bool	 write;
		bool	 program;
	};
	void	 fault (uint32_t addr, bool write, bool program);
	bool	 faulted ();

	/* An operand once its extension words are read: in a register, in
	 * memory at addr, or the immediate imm */
	struct Ea {
		enum Kind { DREG, AREG, MEM, IMM } kind;
		uint8_t	 reg;
		uint32_t addr;
		uint32_t imm;
	};

	/* Bus cycles, 4 clocks each */
	uint16_t read_word (uint32_t addr);
	uint16_t fetch_word (uint32_t addr);
	uint8_t	 read_byte (uint32_t addr);
	uint32_t read_long (uint32_t addr);
	void	 write_word (uint32_t addr, uint16_t v);
	void	 write_byte (uint32_t addr, uint8_t v);
	void	 write_long (uint32_t addr, uint32_t v);
	void	 write_long_low_first (uint32_t addr, uint32_t v);
	uint32_t read (Size s, uint32_t addr);
	void	 write (Size s, uint32_t addr, uint32_t v);
	void	 write_rmw (Size s, uint32_t addr, uint32_t v);
	void	 idle (unsigned clocks) { regs.cycles += clocks; }

	/* The prefetch queue */
	uint16_t next_word ();
	uint32_t next_long ();
	void	 prefetch ();
	void	 jump (uint32_t addr);
	void	 refill () { jump (au - 2); }
	void	 push_long (uint32_t v);
	uint32_t pop_long ();

	/* Operands */
	Ea	 ea (unsigned mode, unsigned reg, Size s);
	uint32_t get (const Ea &e, Size s);
	uint32_t index (uint16_t ext) const;
	uint32_t control_address (unsigned mode, unsigned reg);

	/* The status register and the flags */
	void	 set_sr (uint16_t v);
	void	 set_ccr (uint32_t v) { regs.sr = (uint16_t) ((regs.sr & 0xFF00) | (v & 0x1F)); }
	bool	 supervisor () const { return (regs.sr & SR_S) != 0; }
	bool	 cond (unsigned cc) const;
	void	 flags_nz (Size s, uint32_t r);
	void	 flags_add (Size s, uint32_t x, uint32_t y, uint32_t r);
	void	 flags_sub (Size s, uint32_t x, uint32_t y, uint32_t r);

	/* Exceptions */
	void	 push_frame (uint16_t sr, uint32_t return_pc);
	uint32_t pop_frame (uint16_t &sr);
	void	 enter_vector (uint32_t vector);
	void	 divide_by_zero ();
	void	 exception (unsigned vector, uint32_t return_pc);
	void	 address_error (AddressError e);
	void	 interrupt (unsigned level);
	void	 privilege ();
	void	 illegal (unsigned vector = 4);

	/* The instructions, a group per top nibble of the opcode
	 * (m68000-ops.cpp) */
	void	 execute ();
	void	 op_0000 ();
	void	 op_move (Size s);
	void	 op_0100 ();
	void	 op_0101 ();
	void	 op_0110 ();
	void	 op_0111 ();
	void	 op_1000 ();
	void	 op_add_sub (bool add);
	void	 op_1011 ();
	void	 op_1100 ();
	void	 op_1110 ();

	void	 op_bit (unsigned kind, bool dynamic);
	void	 op_immediate ();
	void	 op_to_sr_ccr (unsigned kind);
	void	 op_movep ();
	void	 op_movem (bool to_registers);
	void	 op_misc_4e ();
	void	 op_shift_memory ();
	void	 op_shift_register ();
	void	 op_divu ();
	void	 op_divs ();
	void	 op_mul (bool is_signed);
	void	 op_abcd_sbcd (bool add);
	void	 op_nbcd ();
	void	 op_addx_subx (bool add);
	void	 op_cmpm ();
	void	 op_chk ();
	void	 op_exg ();

	uint32_t alu (unsigned kind, Size s, uint32_t dst, uint32_t src);
	void	 alu_to_ea (unsigned kind, Size s, unsigned mode, unsigned reg, unsigned dreg);
	void	 alu_to_dn (unsigned kind, Size s, unsigned mode, unsigned reg, unsigned dreg);
	uint32_t bcd (bool add, uint32_t dst, uint32_t src);

	static uint32_t mask (Size s) { return s == BYTE ? 0xFFu : s == WORD ? 0xFFFFu : 0xFFFFFFFFu; }
	static uint32_t msb (Size s) { return s == BYTE ? 0x80u : s == WORD ? 0x8000u : 0x80000000u; }
	static uint32_t sext (Size s, uint32_t v)
	{
		return s == BYTE ? (uint32_t) (int8_t) v : s == WORD ? (uint32_t) (int16_t) v : v;
	}
	/* How far (An)+ and -(An) move An: A7 by 2 for a byte, to stay even */
	static uint32_t step_of (Size s, unsigned reg) { return s == BYTE ? (reg == 7 ? 2u : 1u) : s == WORD ? 2u : 4u; }
	static Size size_bits_to (unsigned bits) { return bits == 0 ? BYTE : bits == 1 ? WORD : LONG; }
	void	 set_d (unsigned n, Size s, uint32_t v)
	{
		uint32_t m = mask (s);
		regs.d[n & 7] = (regs.d[n & 7] & ~m) | (v & m);
	}

	Bus	*bus;
	uint32_t au;		/* the address of the next word to prefetch */
	uint16_t ird;		/* the instruction being run: ir may already
				 * hold the next one */
	/* The address error the instruction met, and the CPU at it */
	bool	 faulting;
	AddressError fault_at;
	Registers kept;
	uint32_t kept_au;
	uint16_t kept_ird;
	uint64_t run_end;	/* where the run in progress stops */
};

#endif /* __M68000_H__ */
