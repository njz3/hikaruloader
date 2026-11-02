/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cpu/tlcs90/tlcs90.hpp, Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __TLCS90_H__
#define __TLCS90_H__

#include <vector>

#include "hl/core.h"

/* The CPU of Toshiba's TLCS-90 family (TMP90C840, TMP90C844 and their PROM
 * versions, as the TMP90PH44 of the JVS I/O board 837-13551), after the 1988
 * Toshiba data book "TLCS-48/90", chapter TMP90C840 (the instructions, the
 * flags, the interrupts). A Z80's registers, its own encoding: the memory
 * operands come as prefixes (E0-F7) before the operation, the registers' as
 * F8-FE. Counted in states, two periods of the clock each. The chip around
 * the CPU (its ports, timers, serial channel) is its Tlcs90::Bus, which also
 * says which interrupt the CPU takes. */
class Tlcs90 {
public:
	class Bus {
	public:
		virtual ~Bus () {}
		virtual uint8_t	 read (uint16_t addr) = 0;
		virtual void	 write (uint16_t addr, uint8_t val) = 0;
		/* The vector of an interrupt the chip has for the CPU, 0 for
		 * none: a non-maskable one at any time, a maskable one only when
		 * maskable (IFF set). Giving it is its acknowledge */
		virtual uint16_t interrupt (bool maskable) = 0;
	};

	/* The flags, F from bit 7 down: I is the interrupts' enable (IFF),
	 * X the extended carry (a 16-bit result of 0 for INC and DEC rr) */
	enum {
		FLAG_S	= 0x80,
		FLAG_Z	= 0x40,
		FLAG_I	= 0x20,
		FLAG_H	= 0x10,
		FLAG_X	= 0x08,
		FLAG_V	= 0x04,
		FLAG_N	= 0x02,
		FLAG_C	= 0x01
	};
	/* The states from an interrupt's acceptance to its vector */
	static const unsigned INTERRUPT_STATES = 20;

	/* The registers between two instructions: plain data, for the states */
	struct Registers {
		uint8_t	 a, f, b, c, d, e, h, l;
		uint8_t	 a2, f2, b2, c2, d2, e2, h2, l2;
		uint16_t ix, iy, sp, pc;
		uint8_t	 halted, ei_pending;
	};

	explicit Tlcs90 (Bus *bus);

	/* PC 0, IFF 0, not halted; the other registers are left as they are
	 * (the data book: "CPU registers remain unchanged") */
	void	 reset ();
	/* Takes an interrupt the bus has, or runs one instruction, or idles two
	 * states when halted: the states it took */
	unsigned step ();
	/* The memory below end never changes (a ROM): its instructions are
	 * decoded once, the address of a memory operand at a register worked
	 * out again each time */
	void	 set_fixed (uint16_t end) { decoded.assign (end, Decoded ()); }

	void	 get_registers (Registers *out) const;
	void	 set_registers (const Registers *in);

	uint16_t pc () const { return r_pc; }
	/* For the tests */
	uint16_t sp () const { return r_sp; }
	uint8_t	 a () const { return r[A]; }
	uint8_t	 f () const { return r_f; }
	uint16_t bc () const { return r16 (BC); }
	uint16_t de () const { return r16 (DE); }
	uint16_t hl () const { return r16 (HL); }
	bool	 halted () const { return r_halted; }
	/* An opcode it does not know was met (and run as a NOP) */
	bool	 unknown_met;

private:
	/* The registers' numbers as the opcodes give them */
	enum { B, C, D, E, H, L, A };
	enum { BC, DE, HL, NO_PAIR, IX, IY, SP, AF, AF2 };

	/* In the order of the low three bits of their opcodes, where they come
	 * in groups of eight */
	enum Op {
		OP_NOP, OP_HALT, OP_DI, OP_EI, OP_SWI, OP_INCX, OP_DECX, OP_EX, OP_EXX,
		OP_DAA, OP_RCF, OP_SCF, OP_CCF, OP_CPL, OP_NEG, OP_MUL, OP_DIV,
		OP_ADD, OP_ADC, OP_SUB, OP_SBC, OP_AND, OP_XOR, OP_OR, OP_CP,
		OP_RLC, OP_RRC, OP_RL, OP_RR, OP_SLA, OP_SRA, OP_SLL, OP_SRL,
		OP_LDI, OP_LDIR, OP_LDD, OP_LDDR, OP_CPI, OP_CPIR, OP_CPD, OP_CPDR,
		OP_RLD, OP_RRD,
		OP_LD, OP_LDA, OP_LDAR, OP_PUSH, OP_POP, OP_INC, OP_DEC, OP_INCW, OP_DECW,
		OP_BIT, OP_RES, OP_SET, OP_TSET,
		OP_DJNZ, OP_JP, OP_JR, OP_CALL, OP_CALLR, OP_RET, OP_RETI, OP_UNKNOWN
	};

	struct Operand {
		enum Kind { NONE, R8, R16, IMM, MEM };
		/* A memory operand at a register pair plus disp, at HL + A, or
		 * at a fixed address */
		enum { HL_PLUS_A = 0xFE, FIXED = 0xFF };

		uint8_t	 kind = NONE;
		uint8_t	 reg = 0;
		/* The immediate, the address, the bit or the condition */
		uint16_t value = 0;
		uint8_t	 base = FIXED;
		int8_t	 disp = 0;
	};
	struct Instruction {
		uint8_t	 op = OP_NOP;
		bool	 wide = false;	/* 16 bits */
		Operand	 x, y;		/* the destination, the source */
		unsigned states = 0, states_not_taken = 0;

		/* The operation and its states, the same when not taken unless
		 * told */
		void set (uint8_t o, unsigned taken, unsigned not_taken = 0)
		{
			op = o;
			states = taken;
			states_not_taken = not_taken ? not_taken : taken;
		}
	};
	struct Decoded {
		Instruction ins;
		uint8_t	 length = 0;	/* 0: not decoded yet */
	};

	static uint8_t	 alu_op (uint8_t n) { return (uint8_t) (OP_ADD + (n & 7)); }
	static uint8_t	 shift_op (uint8_t n) { return (uint8_t) (OP_RLC + (n & 7)); }
	static Operand	 reg8 (uint8_t n);
	static Operand	 reg16 (uint8_t n);
	static Operand	 imm (uint16_t v);
	static Operand	 mem (uint16_t addr);
	/* A memory operand's address, as an immediate (LDA, JP, CALL) */
	static Operand	 itself (Operand m) { m.kind = Operand::IMM; return m; }

	uint8_t	 fetch () { return bus->read (r_pc++); }
	uint16_t fetch16 ();
	uint8_t	 rd (uint16_t addr) const { return bus->read (addr); }
	void	 wr (uint16_t addr, uint8_t v) { bus->write (addr, v); }
	uint16_t rd16 (uint16_t addr) const;
	void	 wr16 (uint16_t addr, uint16_t v);
	void	 push (uint16_t v);
	uint16_t pop ();

	uint16_t r16 (uint8_t n) const;
	void	 set_r16 (uint8_t n, uint16_t v);
	uint8_t	 get8 (const Operand &o) const;
	void	 put8 (const Operand &o, uint8_t v);
	uint16_t get16 (const Operand &o) const;
	void	 put16 (const Operand &o, uint16_t v);
	bool	 test (uint8_t cc) const;

	Operand	 mem_of (uint8_t prefix, uint8_t disp);
	Operand	 based (uint8_t base, uint8_t disp) const;
	uint16_t address_of (const Operand &o) const;
	void	 decode (Instruction &ins);
	void	 decode_prefixed (Instruction &ins, uint8_t prefix);
	unsigned execute (const Instruction &ins);
	void	 take_interrupt (uint16_t vector);

	void	 alu8 (uint8_t op, const Operand &x, uint8_t v);
	void	 alu16 (uint8_t op, const Operand &x, uint16_t v, bool flags_of_add);
	uint8_t	 shift (uint8_t op, uint8_t v, bool on_a);
	void	 flags_inc_dec8 (uint8_t v, bool up);
	void	 daa ();

	Bus	*bus;
	uint8_t	 r[7];		/* B C D E H L A */
	uint8_t	 r_f;
	uint8_t	 alt[8];	/* B' C' D' E' H' L' A' F' */
	uint16_t r_ix, r_iy, r_sp, r_pc;
	bool	 r_halted, r_ei_pending;
	/* By address, below set_fixed's end */
	std::vector<Decoded> decoded;
};

#endif /* __TLCS90_H__ */
