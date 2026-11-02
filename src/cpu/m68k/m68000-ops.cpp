/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cpu/m68000/m68000_ops.cpp,
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

/* The 68000's instructions. The order of the bus cycles and the idle clocks
 * between them are those of the SingleStepTests 680x0 suite. */

#include <bit>
#include <utility>

#include "cpu/m68k/m68000.h"

/* OR, AND, SUB, ADD, EOR and CMP (as the immediate group numbers them) */
enum {
	ALU_OR,
	ALU_AND,
	ALU_SUB,
	ALU_ADD,
	ALU_EOR = 5,
	ALU_CMP
};

/* BTST, BCHG, BCLR, BSET (bits 7-6 of the word) */
enum {
	BIT_TEST,
	BIT_CHANGE,
	BIT_CLEAR,
	BIT_SET
};

/* ASd, LSd, ROXd, ROd: the type of the shift */
enum {
	SHIFT_ARITH,
	SHIFT_LOGICAL,
	ROTATE_X,
	ROTATE
};

/* OR, AND, SUB, ADD, EOR and CMP: the result and the flags */
uint32_t
M68000::alu (unsigned kind, Size s, uint32_t dst, uint32_t src)
{
	uint32_t r;

	switch (kind) {
	case ALU_OR:
		r = dst | src;
		flags_nz (s, r);
		return r;
	case ALU_AND:
		r = dst & src;
		flags_nz (s, r);
		return r;
	case ALU_EOR:
		r = dst ^ src;
		flags_nz (s, r);
		return r;
	case ALU_ADD:
		r = dst + src;
		flags_add (s, dst, src, r);
		return r;
	case ALU_SUB:
		r = dst - src;
		flags_sub (s, dst, src, r);
		return r;
	default: {
		/* CMP keeps X */
		uint16_t x = regs.sr & SR_X;
		r = dst - src;
		flags_sub (s, dst, src, r);
		regs.sr = (uint16_t) ((regs.sr & ~SR_X) | x);
		return dst;
	}
	}
}

/* The ALU on <ea> and Dn, its result to <ea> */
void
M68000::alu_to_ea (unsigned kind, Size s, unsigned mode, unsigned reg, unsigned dreg)
{
	Ea e = ea (mode, reg, s);
	uint32_t r = alu (kind, s, get (e, s), regs.d[dreg] & mask (s));

	prefetch ();
	write_rmw (s, e.addr, r);
}

/* The ALU on Dn and <ea>, its result to Dn */
void
M68000::alu_to_dn (unsigned kind, Size s, unsigned mode, unsigned reg, unsigned dreg)
{
	Ea e = ea (mode, reg, s);
	uint32_t r = alu (kind, s, regs.d[dreg] & mask (s), get (e, s));

	prefetch ();
	set_d (dreg, s, r);
	if (s == LONG)
		idle (e.kind == Ea::MEM ? 2 : 4);
}

void
M68000::execute ()
{
	switch (regs.ir >> 12) {
	case 0x0:
		op_0000 ();
		break;
	case 0x1:
		op_move (BYTE);
		break;
	case 0x2:
		op_move (LONG);
		break;
	case 0x3:
		op_move (WORD);
		break;
	case 0x4:
		op_0100 ();
		break;
	case 0x5:
		op_0101 ();
		break;
	case 0x6:
		op_0110 ();
		break;
	case 0x7:
		op_0111 ();
		break;
	case 0x8:
		op_1000 ();
		break;
	case 0x9:
		op_add_sub (false);
		break;
	case 0xA:
		illegal (10);
		break;
	case 0xB:
		op_1011 ();
		break;
	case 0xC:
		op_1100 ();
		break;
	case 0xD:
		op_add_sub (true);
		break;
	case 0xE:
		op_1110 ();
		break;
	default:
		illegal (11);
		break;
	}
}

/* 0000: the bit operations, MOVEP and the immediate operations */
void
M68000::op_0000 ()
{
	uint16_t op = regs.ir;

	if ((op & 0x0138) == 0x0108)
		op_movep ();
	else if (op & 0x0100)
		op_bit ((op >> 6) & 3, true);
	else if ((op & 0x0E00) == 0x0800)
		op_bit ((op >> 6) & 3, false);
	else
		op_immediate ();
}

/* BTST, BCHG, BCLR, BSET: the bit's number from Dn or from an immediate
 * word; modulo 32 in a data register, modulo 8 in memory */
void
M68000::op_bit (unsigned kind, bool dynamic)
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	uint32_t bit = dynamic ? regs.d[(op >> 9) & 7] : next_word (), v;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > (kind == BIT_TEST ? 4u : 1u)) ||
	    (!dynamic && mode == 7 && reg == 4)) {
		illegal ();
		return;
	}
	if (mode == 0) {
		uint32_t &d = regs.d[reg];
		bool was;

		bit &= 31;
		was = (d >> bit) & 1;
		regs.sr = (uint16_t) (was ? regs.sr & ~SR_Z : regs.sr | SR_Z);
		prefetch ();
		switch (kind) {
		case BIT_TEST:
			idle (2);
			break;
		case BIT_CHANGE:
			d ^= 1u << bit;
			idle (bit < 16 ? 2 : 4);
			break;
		case BIT_CLEAR:
			d &= ~(1u << bit);
			idle (bit < 16 ? 4 : 6);
			break;
		default:
			d |= 1u << bit;
			idle (bit < 16 ? 2 : 4);
			break;
		}
		return;
	}
	e = ea (mode, reg, BYTE);
	v = get (e, BYTE);
	bit &= 7;
	regs.sr = (uint16_t) (((v >> bit) & 1) ? regs.sr & ~SR_Z : regs.sr | SR_Z);
	if (kind == BIT_TEST) {
		prefetch ();
		if (e.kind == Ea::IMM)
			idle (2);
		return;
	}
	if (kind == BIT_CHANGE)
		v ^= 1u << bit;
	else if (kind == BIT_CLEAR)
		v &= ~(1u << bit);
	else
		v |= 1u << bit;
	prefetch ();
	write_byte (e.addr, (uint8_t) v);
}

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI; ORI, ANDI, EORI to CCR and SR */
void
M68000::op_immediate ()
{
	uint16_t op = regs.ir;
	unsigned kind = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
	unsigned size_bits = (op >> 6) & 3;
	uint32_t src, dst, r;
	Size s;
	Ea e;

	if (mode == 7 && reg == 4 && (kind == 0 || kind == 1 || kind == 5) && size_bits < 2) {
		op_to_sr_ccr (kind | (size_bits << 4));
		return;
	}
	if (kind == 4 || kind == 7 || size_bits == 3 || mode == 1 || (mode == 7 && reg > 1)) {
		illegal ();
		return;
	}
	s = size_bits_to (size_bits);
	src = s == LONG ? next_long () : (next_word () & mask (s));
	e = ea (mode, reg, s);
	dst = get (e, s);
	r = alu (kind, s, dst, src);
	if (e.kind == Ea::DREG) {
		prefetch ();
		if (kind != ALU_CMP)
			set_d (reg, s, r);
		if (s == LONG)
			idle (kind == ALU_CMP ? 2 : 4);
		return;
	}
	prefetch ();
	if (kind != ALU_CMP)
		write_rmw (s, e.addr, r);
}

/* ORI, ANDI, EORI to CCR (a byte) or to SR (a word, privileged) */
void
M68000::op_to_sr_ccr (unsigned kind)
{
	bool to_sr = (kind >> 4) != 0;
	uint16_t imm, cur, v;

	if (to_sr && !supervisor ()) {
		privilege ();
		return;
	}
	imm = next_word ();
	cur = to_sr ? regs.sr : (uint16_t) (regs.sr & 0xFF);
	switch (kind & 7) {
	case 0:
		v = cur | imm;
		break;
	case 1:
		v = cur & imm;
		break;
	default:
		v = cur ^ imm;
		break;
	}
	idle (8);
	if (to_sr)
		set_sr (v);
	else
		set_ccr (v);
	refill ();
}

/* MOVEP: bytes at every other address, the high byte first */
void
M68000::op_movep ()
{
	uint16_t op = regs.ir;
	unsigned dreg = (op >> 9) & 7, i;
	uint32_t addr = regs.a[op & 7] + (uint32_t) (int16_t) next_word (), v;

	switch ((op >> 6) & 3) {
	case 0:
		v = (uint32_t) read_byte (addr) << 8;
		v |= read_byte (addr + 2);
		set_d (dreg, WORD, v);
		break;
	case 1:
		v = 0;
		for (i = 0; i < 4; i++)
			v = (v << 8) | read_byte (addr + 2 * i);
		regs.d[dreg] = v;
		break;
	case 2:
		write_byte (addr, (uint8_t) (regs.d[dreg] >> 8));
		write_byte (addr + 2, (uint8_t) regs.d[dreg]);
		break;
	default:
		for (i = 0; i < 4; i++)
			write_byte (addr + 2 * i, (uint8_t) (regs.d[dreg] >> (24 - 8 * i)));
		break;
	}
	prefetch ();
}

/* MOVE and MOVEA. To -(An) the prefetch comes before the write; to an
 * absolute long address after a memory source, the second address word is
 * fetched after the write */
void
M68000::op_move (Size s)
{
	uint16_t op = regs.ir;
	unsigned smode = (op >> 3) & 7, sreg = op & 7;
	unsigned dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
	uint32_t v, step, addr;
	Ea src;

	if ((s == BYTE && (smode == 1 || dmode == 1)) || (smode == 7 && sreg > 4) ||
	    (dmode == 7 && dreg > 1)) {
		illegal ();
		return;
	}
	src = ea (smode, sreg, s);
	v = get (src, s);
	if (dmode == 1) {
		regs.a[dreg] = s == WORD ? sext (WORD, v) : v;
		prefetch ();
		return;
	}
	if (dmode == 0) {
		flags_nz (s, v);
		set_d (dreg, s, v);
		prefetch ();
		return;
	}
	step = step_of (s, dreg);
	switch (dmode) {
	case 2:
	case 3:
		addr = regs.a[dreg];
		flags_nz (s, v);
		write (s, addr, v);
		if (dmode == 3)
			regs.a[dreg] += step;
		prefetch ();
		return;
	case 4:
		flags_nz (s, v);
		prefetch ();
		if (s == LONG) {
			/* A word at a time, the low one first */
			regs.a[dreg] -= 2;
			write_word (regs.a[dreg], (uint16_t) v);
			regs.a[dreg] -= 2;
			write_word (regs.a[dreg], (uint16_t) (v >> 16));
		} else {
			regs.a[dreg] -= step;
			write (s, regs.a[dreg], v);
		}
		return;
	case 5:
		addr = regs.a[dreg] + (uint32_t) (int16_t) next_word ();
		break;
	case 6:
		idle (2);
		addr = regs.a[dreg] + index (next_word ());
		break;
	default:
		if (dreg == 0)
			addr = (uint32_t) (int16_t) next_word ();
		else if (src.kind == Ea::MEM) {
			/* The high word taken, the low one still in IRC */
			addr = (uint32_t) next_word () << 16;
			addr |= regs.irc;
			flags_nz (s, v);
			write (s, addr, v);
			next_word ();
			prefetch ();
			return;
		} else
			addr = next_long ();
		break;
	}
	flags_nz (s, v);
	write (s, addr, v);
	prefetch ();
}

/* 0100: the miscellaneous instructions */
void
M68000::op_0100 ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	unsigned size_bits = (op >> 6) & 3;

	if ((op & 0xF1C0) == 0x41C0) {				/* LEA */
		if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) {
			illegal ();
			return;
		}
		regs.a[(op >> 9) & 7] = control_address (mode, reg);
		prefetch ();
		return;
	}
	if ((op & 0xF1C0) == 0x4180) {				/* CHK */
		op_chk ();
		return;
	}
	if ((op & 0xF100) == 0x4100) {
		illegal ();
		return;
	}

	switch ((op >> 8) & 0xF) {
	case 0x0:
	case 0x2:
	case 0x4:
	case 0x6: {
		unsigned which = (op >> 9) & 3;	/* NEGX, CLR, NEG, NOT */
		uint32_t v, r;
		Size s;
		Ea e;

		if (size_bits == 3) {
			if (which == 0) {			/* MOVE from SR */
				if (mode == 1 || (mode == 7 && reg > 1)) {
					illegal ();
					return;
				}
				e = ea (mode, reg, WORD);
				if (e.kind == Ea::DREG) {
					set_d (reg, WORD, regs.sr);
					prefetch ();
					idle (2);
					return;
				}
				read_word (e.addr);
				prefetch ();
				write_word (e.addr, regs.sr);
				return;
			}
			if (which == 1) {
				illegal ();
				return;
			}
			if (which == 3 && !supervisor ()) {	/* MOVE to SR */
				privilege ();
				return;
			}
			if (mode == 1 || (mode == 7 && reg > 4)) {
				illegal ();
				return;
			}
			e = ea (mode, reg, WORD);		/* MOVE to CCR or SR */
			v = get (e, WORD);
			idle (4);
			if (which == 3)
				set_sr ((uint16_t) v);
			else
				set_ccr (v);
			refill ();
			return;
		}
		if (mode == 1 || (mode == 7 && reg > 1)) {
			illegal ();
			return;
		}
		s = size_bits_to (size_bits);
		e = ea (mode, reg, s);
		v = get (e, s);
		switch (which) {
		case 0: {					/* NEGX */
			uint32_t x = (regs.sr & SR_X) ? 1 : 0;
			uint16_t z = regs.sr & SR_Z;

			r = 0 - v - x;
			flags_sub (s, 0, v, r);
			/* Z only cleared */
			if (!(r & mask (s)))
				regs.sr = (uint16_t) ((regs.sr & ~SR_Z) | z);
			break;
		}
		case 1:						/* CLR */
			r = 0;
			regs.sr = (uint16_t) ((regs.sr & ~(SR_N | SR_V | SR_C)) | SR_Z);
			break;
		case 2:						/* NEG */
			r = 0 - v;
			flags_sub (s, 0, v, r);
			break;
		default:					/* NOT */
			r = ~v;
			flags_nz (s, r);
			break;
		}
		prefetch ();
		if (e.kind == Ea::DREG) {
			set_d (reg, s, r);
			if (s == LONG)
				idle (2);
		} else
			write_rmw (s, e.addr, r);
		return;
	}
	case 0x8:
		if (size_bits == 0) {					/* NBCD */
			op_nbcd ();
			return;
		}
		if (size_bits == 1) {
			bool absolute;
			uint32_t addr;

			if (mode == 0) {				/* SWAP */
				regs.d[reg] = (regs.d[reg] << 16) | (regs.d[reg] >> 16);
				flags_nz (LONG, regs.d[reg]);
				prefetch ();
				return;
			}
			if (mode == 1 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) {
				illegal ();
				return;
			}
			absolute = mode == 7 && reg < 2;		/* PEA */
			addr = control_address (mode, reg);
			if (absolute) {
				push_long (addr);
				prefetch ();
			} else {
				prefetch ();
				push_long (addr);
			}
			return;
		}
		if (mode == 0) {					/* EXT */
			if (size_bits == 2)
				set_d (reg, WORD, sext (BYTE, regs.d[reg]));
			else
				regs.d[reg] = sext (WORD, regs.d[reg]);
			flags_nz (size_bits == 2 ? WORD : LONG, regs.d[reg]);
			prefetch ();
			return;
		}
		op_movem (false);
		return;
	case 0xA:
		if (op == 0x4AFC) {
			illegal ();
			return;
		}
		if (mode == 1 || (mode == 7 && reg > 1)) {
			illegal ();
			return;
		}
		if (size_bits == 3) {					/* TAS */
			Ea e = ea (mode, reg, BYTE);
			uint32_t v;

			if (e.kind == Ea::DREG) {
				v = regs.d[reg] & 0xFF;
				flags_nz (BYTE, v);
				set_d (reg, BYTE, v | 0x80);
				prefetch ();
				return;
			}
			v = read_byte (e.addr);
			flags_nz (BYTE, v);
			idle (2);
			write_byte (e.addr, (uint8_t) (v | 0x80));
			prefetch ();
			return;
		} else {						/* TST */
			Size s = size_bits_to (size_bits);
			Ea e = ea (mode, reg, s);

			flags_nz (s, get (e, s));
			prefetch ();
			return;
		}
	case 0xC:
		if (size_bits >= 2) {
			op_movem (true);
			return;
		}
		illegal ();
		return;
	case 0xE:
		op_misc_4e ();
		return;
	default:
		illegal ();
		return;
	}
}

/* CHK: Dn above the bound traps after 4 clocks, Dn below 0 after 6. N then
 * tells Dn < 0 and is kept otherwise; Z, V and C as the suite has them */
void
M68000::op_chk ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	int16_t bound, v;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, WORD);
	bound = (int16_t) get (e, WORD);
	v = (int16_t) regs.d[(op >> 9) & 7];
	prefetch ();
	regs.sr &= (uint16_t) ~(SR_Z | SR_V | SR_C);
	if (v == 0)
		regs.sr |= SR_Z;
	if (v > bound || v < 0) {
		regs.sr = (uint16_t) (v < 0 ? regs.sr | SR_N : regs.sr & ~SR_N);
		idle (v > bound ? 4 : 6);
		exception (6, regs.pc);
		return;
	}
	idle (6);
}

/* MOVEM: the mask word, then a transfer per bit. From memory, one more word
 * is read after the last register; words are sign-extended. To -(An), the
 * registers go in reverse order, the low word first */
void
M68000::op_movem (bool to_registers)
{
	uint16_t op = regs.ir, list;
	unsigned mode = (op >> 3) & 7, reg = op & 7, i;
	Size s = (op & 0x40) ? LONG : WORD;
	uint32_t step = s == LONG ? 4 : 2, addr, v;

	if (mode < 2 || (to_registers && mode == 4) || (!to_registers && mode == 3) ||
	    (mode == 7 && reg > (to_registers ? 3u : 1u))) {
		illegal ();
		return;
	}
	list = next_word ();
	if (mode == 3 || mode == 4)
		addr = regs.a[reg];
	else
		addr = ea (mode, reg, s).addr;

	if (to_registers) {
		/* The 68000 has already stepped An when the first read faults */
		if (mode == 3 && (addr & 1))
			regs.a[reg] = addr + 2;
		for (i = 0; i < 16; i++) {
			if (!(list & (1u << i)))
				continue;
			v = s == LONG ? read_long (addr) : sext (WORD, read_word (addr));
			if (i < 8)
				regs.d[i] = v;
			else
				regs.a[i - 8] = v;
			addr += step;
		}
		read_word (addr);
		if (mode == 3)
			regs.a[reg] = addr;
		prefetch ();
		return;
	}
	if (mode == 4) {
		for (i = 0; i < 16; i++) {
			if (!(list & (1u << i)))
				continue;
			v = i < 8 ? regs.a[7 - i] : regs.d[15 - i];
			addr -= step;
			if (s == LONG)
				write_long_low_first (addr, v);
			else
				write_word (addr, (uint16_t) v);
		}
		regs.a[reg] = addr;
		prefetch ();
		return;
	}
	for (i = 0; i < 16; i++) {
		if (!(list & (1u << i)))
			continue;
		v = i < 8 ? regs.d[i] : regs.a[i - 8];
		if (s == LONG)
			write_long (addr, v);
		else
			write_word (addr, (uint16_t) v);
		addr += step;
	}
	prefetch ();
}

/* 4E40-4EFF: TRAP, LINK, UNLK, MOVE USP, the 4E7x group, JSR, JMP */
void
M68000::op_misc_4e ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	uint32_t target, next;

	if ((op & 0xFFF0) == 0x4E40) {				/* TRAP */
		idle (4);
		exception (32 + (op & 15), au - 2);
		return;
	}
	if ((op & 0xFFF8) == 0x4E50) {				/* LINK */
		uint32_t disp = sext (WORD, next_word ());

		regs.a[7] -= 4;
		write_long (regs.a[7], regs.a[reg]);
		regs.a[reg] = regs.a[7];
		regs.a[7] += disp;
		prefetch ();
		return;
	}
	if ((op & 0xFFF8) == 0x4E58) {				/* UNLK */
		regs.a[7] = regs.a[reg];
		regs.a[reg] = pop_long ();
		prefetch ();
		return;
	}
	if ((op & 0xFFF0) == 0x4E60) {				/* MOVE An,USP / USP,An */
		if (!supervisor ()) {
			privilege ();
			return;
		}
		if (op & 8)
			regs.a[reg] = regs.other_sp;
		else
			regs.other_sp = regs.a[reg];
		prefetch ();
		return;
	}
	switch (op) {
	case 0x4E70:						/* RESET */
		if (!supervisor ()) {
			privilege ();
			return;
		}
		idle (128);
		bus->reset_devices ();
		prefetch ();
		return;
	case 0x4E71:						/* NOP */
		prefetch ();
		return;
	case 0x4E72: {						/* STOP */
		uint16_t v;

		if (!supervisor ()) {
			privilege ();
			return;
		}
		v = next_word ();
		set_sr (v);
		regs.pc = au - 2;
		regs.stopped = true;
		return;
	}
	case 0x4E73: {						/* RTE */
		uint16_t sr;
		uint32_t pc;

		if (!supervisor ()) {
			privilege ();
			return;
		}
		pc = pop_frame (sr);
		set_sr (sr);
		jump (pc);
		return;
	}
	case 0x4E75:						/* RTS */
		jump (pop_long ());
		return;
	case 0x4E76:						/* TRAPV */
		prefetch ();
		if (regs.sr & SR_V)
			exception (7, regs.pc);
		return;
	case 0x4E77: {						/* RTR */
		uint16_t ccr;
		uint32_t pc = pop_frame (ccr);

		set_ccr (ccr);
		jump (pc);
		return;
	}
	default:
		break;
	}
	if ((op & 0xFF80) != 0x4E80 || mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) {
		illegal ();
		return;
	}
	/* JSR and JMP: the last extension word used from IRC, the queue then
	 * from the target */
	next = au - 2;
	switch (mode) {
	case 2:
		target = regs.a[reg];
		break;
	case 5:
		idle (2);
		target = regs.a[reg] + sext (WORD, regs.irc);
		next += 2;
		break;
	case 6:
		idle (6);
		target = regs.a[reg] + index (regs.irc);
		next += 2;
		break;
	default:
		switch (reg) {
		case 0:
			idle (2);
			target = sext (WORD, regs.irc);
			next += 2;
			break;
		case 1: {
			uint32_t hi = next_word ();

			target = (hi << 16) | regs.irc;
			next = au;
			break;
		}
		case 2:
			idle (2);
			target = au - 2 + sext (WORD, regs.irc);
			next += 2;
			break;
		default:
			idle (6);
			target = au - 2 + index (regs.irc);
			next += 2;
			break;
		}
		break;
	}
	if (op & 0x40) {
		jump (target);
		return;
	}
	au = target;
	regs.ir = fetch_word (au);
	push_long (next);
	regs.irc = fetch_word (au + 2);
	au += 4;
	regs.pc = target;
}

/* 0101: ADDQ, SUBQ, Scc, DBcc */
void
M68000::op_0101 ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	uint32_t q, r;
	bool add;
	Size s;
	Ea e;

	if (((op >> 6) & 3) == 3) {
		bool t = cond ((op >> 8) & 15);

		if (mode == 1) {					/* DBcc */
			uint32_t disp;
			uint16_t count;

			if (t) {
				idle (4);
				next_word ();
				prefetch ();
				return;
			}
			disp = sext (WORD, regs.irc);
			count = (uint16_t) (regs.d[reg] - 1);
			set_d (reg, WORD, count);
			idle (2);
			if (count != 0xFFFF) {
				jump (au - 2 + disp);
				return;
			}
			read_word (au - 2 + disp);
			next_word ();
			prefetch ();
			return;
		}
		if (mode == 7 && reg > 1) {
			illegal ();
			return;
		}
		e = ea (mode, reg, BYTE);				/* Scc */
		if (e.kind == Ea::DREG) {
			set_d (reg, BYTE, t ? 0xFF : 0);
			prefetch ();
			if (t)
				idle (2);
			return;
		}
		read_byte (e.addr);
		prefetch ();
		write_byte (e.addr, t ? 0xFF : 0);
		return;
	}
	s = size_bits_to ((op >> 6) & 3);
	q = ((op >> 9) & 7) ? ((op >> 9) & 7) : 8;
	add = !(op & 0x100);
	if ((s == BYTE && mode == 1) || (mode == 7 && reg > 1)) {
		illegal ();
		return;
	}
	if (mode == 1) {
		regs.a[reg] = add ? regs.a[reg] + q : regs.a[reg] - q;
		prefetch ();
		idle (s == WORD ? 4 : 2);
		return;
	}
	e = ea (mode, reg, s);
	r = alu (add ? ALU_ADD : ALU_SUB, s, get (e, s), q);
	prefetch ();
	if (e.kind == Ea::DREG) {
		set_d (reg, s, r);
		if (s == LONG)
			idle (4);
		return;
	}
	write_rmw (s, e.addr, r);
}

/* 0110: BRA, BSR, Bcc */
void
M68000::op_0110 ()
{
	uint16_t op = regs.ir;
	unsigned cc = (op >> 8) & 15;
	bool is_word = (op & 0xFF) == 0;
	uint32_t disp = is_word ? sext (WORD, regs.irc) : sext (BYTE, op);
	uint32_t base = au - 2;

	if (cc == 1) {							/* BSR */
		idle (2);
		push_long (base + (is_word ? 2 : 0));
		jump (base + disp);
		return;
	}
	if (cond (cc)) {
		idle (2);
		jump (base + disp);
		return;
	}
	idle (4);
	if (is_word)
		next_word ();
	prefetch ();
}

/* 0111: MOVEQ */
void
M68000::op_0111 ()
{
	uint16_t op = regs.ir;
	uint32_t v;

	if (op & 0x100) {
		illegal ();
		return;
	}
	v = sext (BYTE, op);
	regs.d[(op >> 9) & 7] = v;
	flags_nz (LONG, v);
	prefetch ();
}

/* 1000: OR, DIVU, DIVS, SBCD */
void
M68000::op_1000 ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7;
	Size s;

	if ((op & 0x1C0) == 0x0C0) {
		op_divu ();
		return;
	}
	if ((op & 0x1C0) == 0x1C0) {
		op_divs ();
		return;
	}
	if ((op & 0x1F0) == 0x100) {
		op_abcd_sbcd (false);
		return;
	}
	s = size_bits_to ((op >> 6) & 3);
	if (op & 0x100) {						/* OR Dn,<ea> */
		if (mode < 2 || (mode == 7 && reg > 1)) {
			illegal ();
			return;
		}
		alu_to_ea (ALU_OR, s, mode, reg, dreg);
		return;
	}
	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	alu_to_dn (ALU_OR, s, mode, reg, dreg);				/* OR <ea>,Dn */
}

/* 1001 and 1101: SUB and ADD, SUBA and ADDA, SUBX and ADDX */
void
M68000::op_add_sub (bool add)
{
	uint16_t op = regs.ir;
	unsigned opmode = (op >> 6) & 7;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7;
	unsigned kind = add ? ALU_ADD : ALU_SUB;
	Size s;

	if (opmode == 3 || opmode == 7) {				/* ADDA, SUBA */
		Ea e;
		uint32_t v;

		s = opmode == 3 ? WORD : LONG;
		if (mode == 7 && reg > 4) {
			illegal ();
			return;
		}
		e = ea (mode, reg, s);
		v = s == WORD ? sext (WORD, get (e, s)) : get (e, s);
		regs.a[dreg] = add ? regs.a[dreg] + v : regs.a[dreg] - v;
		prefetch ();
		idle (s == WORD || e.kind != Ea::MEM ? 4 : 2);
		return;
	}
	s = size_bits_to (opmode & 3);
	if ((opmode & 4) && mode < 2) {
		op_addx_subx (add);
		return;
	}
	if (opmode & 4) {						/* Dn,<ea> */
		if (mode == 7 && reg > 1) {
			illegal ();
			return;
		}
		alu_to_ea (kind, s, mode, reg, dreg);
		return;
	}
	if ((s == BYTE && mode == 1) || (mode == 7 && reg > 4)) {	/* <ea>,Dn */
		illegal ();
		return;
	}
	alu_to_dn (kind, s, mode, reg, dreg);
}

/* ADDX and SUBX: Z only cleared; -(An),-(An) reads and writes a long the
 * low word first */
void
M68000::op_addx_subx (bool add)
{
	uint16_t op = regs.ir;
	Size s = size_bits_to ((op >> 6) & 3);
	unsigned rx = op & 7, ry = (op >> 9) & 7;
	uint32_t x = (regs.sr & SR_X) ? 1 : 0;
	uint16_t z = regs.sr & SR_Z;
	uint32_t src, dst, daddr = 0, r;

	if (op & 8) {
		uint32_t step = step_of (s, rx), dstep = step_of (s, ry);

		idle (2);
		if (s == LONG) {
			uint32_t lo, dlo;

			/* A word at a time, the low one first */
			regs.a[rx] -= 2;
			lo = read_word (regs.a[rx]);
			regs.a[rx] -= 2;
			src = ((uint32_t) read_word (regs.a[rx]) << 16) | lo;
			regs.a[ry] -= 2;
			dlo = read_word (regs.a[ry]);
			regs.a[ry] -= 2;
			daddr = regs.a[ry];
			dst = ((uint32_t) read_word (daddr) << 16) | dlo;
		} else {
			regs.a[rx] -= step;
			src = read (s, regs.a[rx]);
			regs.a[ry] -= dstep;
			daddr = regs.a[ry];
			dst = read (s, daddr);
		}
	} else {
		src = regs.d[rx] & mask (s);
		dst = regs.d[ry] & mask (s);
	}
	r = add ? dst + src + x : dst - src - x;
	if (add)
		flags_add (s, dst, src, r);
	else
		flags_sub (s, dst, src, r);
	if (!(r & mask (s)))
		regs.sr = (uint16_t) ((regs.sr & ~SR_Z) | z);

	if (op & 8) {
		if (s == LONG) {
			write_word (daddr + 2, (uint16_t) r);
			prefetch ();
			write_word (daddr, (uint16_t) (r >> 16));
		} else {
			prefetch ();
			write (s, daddr, r);
		}
		return;
	}
	set_d (ry, s, r);
	prefetch ();
	if (s == LONG)
		idle (4);
}

/* 1011: CMP, CMPA, CMPM, EOR */
void
M68000::op_1011 ()
{
	uint16_t op = regs.ir;
	unsigned opmode = (op >> 6) & 7;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7;
	Size s;
	Ea e;

	if (opmode == 3 || opmode == 7) {				/* CMPA */
		uint32_t v;

		s = opmode == 3 ? WORD : LONG;
		if (mode == 7 && reg > 4) {
			illegal ();
			return;
		}
		e = ea (mode, reg, s);
		v = s == WORD ? sext (WORD, get (e, s)) : get (e, s);
		alu (ALU_CMP, LONG, regs.a[dreg], v);
		prefetch ();
		idle (2);
		return;
	}
	s = size_bits_to (opmode & 3);
	if (opmode & 4) {
		uint32_t r;

		if (mode == 1) {
			op_cmpm ();
			return;
		}
		if (mode == 7 && reg > 1) {				/* EOR Dn,<ea> */
			illegal ();
			return;
		}
		e = ea (mode, reg, s);
		r = alu (ALU_EOR, s, get (e, s), regs.d[dreg] & mask (s));
		prefetch ();
		if (e.kind == Ea::DREG) {
			set_d (reg, s, r);
			if (s == LONG)
				idle (4);
			return;
		}
		write_rmw (s, e.addr, r);
		return;
	}
	if ((s == BYTE && mode == 1) || (mode == 7 && reg > 4)) {	/* CMP <ea>,Dn */
		illegal ();
		return;
	}
	e = ea (mode, reg, s);
	alu (ALU_CMP, s, regs.d[dreg] & mask (s), get (e, s));
	prefetch ();
	if (s == LONG)
		idle (2);
}

void
M68000::op_cmpm ()
{
	uint16_t op = regs.ir;
	Size s = size_bits_to ((op >> 6) & 3);
	Ea src = ea (3, op & 7, s);
	uint32_t x = get (src, s);
	Ea dst = ea (3, (op >> 9) & 7, s);
	uint32_t y = get (dst, s);

	alu (ALU_CMP, s, y, x);
	prefetch ();
}

/* 1100: AND, MULU, MULS, ABCD, EXG */
void
M68000::op_1100 ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7;
	Size s;

	if ((op & 0x1C0) == 0x0C0) {
		op_mul (false);
		return;
	}
	if ((op & 0x1C0) == 0x1C0) {
		op_mul (true);
		return;
	}
	if ((op & 0x1F0) == 0x100) {
		op_abcd_sbcd (true);
		return;
	}
	if ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188) {
		op_exg ();
		return;
	}
	s = size_bits_to ((op >> 6) & 3);
	if (op & 0x100) {						/* AND Dn,<ea> */
		if (mode < 2 || (mode == 7 && reg > 1)) {
			illegal ();
			return;
		}
		alu_to_ea (ALU_AND, s, mode, reg, dreg);
		return;
	}
	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	alu_to_dn (ALU_AND, s, mode, reg, dreg);			/* AND <ea>,Dn */
}

void
M68000::op_exg ()
{
	uint16_t op = regs.ir;
	unsigned rx = (op >> 9) & 7, ry = op & 7;

	switch (op & 0x1F8) {
	case 0x140:
		std::swap (regs.d[rx], regs.d[ry]);
		break;
	case 0x148:
		std::swap (regs.a[rx], regs.a[ry]);
		break;
	default:
		std::swap (regs.d[rx], regs.a[ry]);
		break;
	}
	prefetch ();
	idle (2);
}

/* MULU: 38 clocks plus 2 per bit set in the source; MULS: plus 2 per change
 * between two bits of the source shifted left by one */
void
M68000::op_mul (bool is_signed)
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7, n;
	uint32_t src, r;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, WORD);
	src = get (e, WORD);
	if (is_signed) {
		r = (uint32_t) ((int32_t) (int16_t) src * (int32_t) (int16_t) regs.d[dreg]);
		n = (unsigned) std::popcount (((src << 1) ^ src) & 0xFFFF);
	} else {
		r = src * (regs.d[dreg] & 0xFFFF);
		n = (unsigned) std::popcount (src);
	}
	regs.d[dreg] = r;
	flags_nz (LONG, r);
	prefetch ();
	idle (34 + 2 * n);
}

/* DIVU: the clocks of the shift-and-subtract the 68000 does (the yacht.txt
 * flowchart). An overflow sets V and leaves Dn, N and Z */
void
M68000::op_divu ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7, i, clocks;
	uint32_t divisor, dividend, hdivisor, quot, rem;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, WORD);
	divisor = get (e, WORD);
	dividend = regs.d[dreg];
	if (divisor == 0) {
		divide_by_zero ();
		return;
	}
	if ((dividend >> 16) >= divisor) {
		regs.sr = (uint16_t) ((regs.sr & ~SR_C) | SR_V);
		idle (6);
		prefetch ();
		return;
	}
	clocks = 38 * 2;
	hdivisor = divisor << 16;
	for (i = 0; i < 15; i++) {
		if ((int32_t) dividend < 0) {
			dividend <<= 1;
			dividend -= hdivisor;
		} else {
			dividend <<= 1;
			if (dividend >= hdivisor) {
				dividend -= hdivisor;
				clocks += 2;
			} else
				clocks += 4;
		}
	}
	quot = regs.d[dreg] / divisor;
	rem = regs.d[dreg] % divisor;
	regs.d[dreg] = (rem << 16) | quot;
	flags_nz (WORD, quot);
	idle (clocks - 4);
	prefetch ();
}

/* DIVS: the microcycles of the 68000's divide loop (the yacht.txt
 * flowchart). An overflow sets V and leaves Dn, N and Z */
void
M68000::op_divs ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7, dreg = (op >> 9) & 7, micro;
	int32_t divisor, dividend, rem;
	uint32_t aquot;
	int64_t q;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > 4)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, WORD);
	divisor = (int16_t) get (e, WORD);
	dividend = (int32_t) regs.d[dreg];
	if (divisor == 0) {
		divide_by_zero ();
		return;
	}
	/* 6 or 7 microcycles into the loop; an overflow stops there */
	micro = dividend < 0 ? 7 : 6;
	q = (int64_t) dividend / divisor;
	if (q > 32767 || q < -32768) {
		regs.sr = (uint16_t) ((regs.sr & ~SR_C) | SR_V);
		idle (micro * 2);
		prefetch ();
		return;
	}
	/* 49 for the loop, one more per 0 among bits 15-1 of the absolute
	 * quotient, then the exit by the signs */
	aquot = (uint32_t) (q < 0 ? -q : q);
	micro += 49 + (unsigned) std::popcount (~aquot & 0xFFFEu);
	micro += divisor < 0 ? 4 : dividend < 0 ? 5 : 3;
	rem = (int32_t) ((int64_t) dividend % divisor);
	regs.d[dreg] = ((uint32_t) rem << 16) | ((uint32_t) q & 0xFFFF);
	flags_nz (WORD, (uint32_t) q);
	idle (micro * 2);
	prefetch ();
}

/* ABCD and SBCD: the binary sum or difference, then the decimal
 * correction; V and N as the 68000 leaves them (bit 7 before and after the
 * correction). Z only cleared */
uint32_t
M68000::bcd (bool add, uint32_t dst, uint32_t src)
{
	uint32_t x = (regs.sr & SR_X) ? 1 : 0;
	uint32_t res, carry, v;
	uint16_t f;

	if (add) {
		uint32_t ss = dst + src + x;
		uint32_t bc = ((dst & src) | (~ss & (dst | src))) & 0x88;
		uint32_t dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
		uint32_t corf = (bc | dc) - ((bc | dc) >> 2);

		res = ss + corf;
		carry = ((bc | (ss & ~res)) >> 7) & 1;
		v = ((~ss & res) >> 7) & 1;
	} else {
		uint32_t dd = dst - src - x;
		uint32_t bc = ((~dst & src) | (dd & ~dst) | (dd & src)) & 0x88;
		uint32_t corf = bc - (bc >> 2);

		res = dd - corf;
		carry = ((bc | (~dd & res)) >> 7) & 1;
		v = ((dd & ~res) >> 7) & 1;
	}
	f = regs.sr & (uint16_t) ~(SR_X | SR_N | SR_V | SR_C);
	if (carry)
		f |= SR_X | SR_C;
	if (v)
		f |= SR_V;
	if (res & 0x80)
		f |= SR_N;
	if (res & 0xFF)
		f &= (uint16_t) ~SR_Z;
	regs.sr = f;
	return res & 0xFF;
}

void
M68000::op_abcd_sbcd (bool add)
{
	uint16_t op = regs.ir;
	unsigned rx = op & 7, ry = (op >> 9) & 7;

	if (op & 8) {
		uint32_t src, dst, r;

		idle (2);
		regs.a[rx] -= step_of (BYTE, rx);
		src = read_byte (regs.a[rx]);
		regs.a[ry] -= step_of (BYTE, ry);
		dst = read_byte (regs.a[ry]);
		r = bcd (add, dst, src);
		prefetch ();
		write_byte (regs.a[ry], (uint8_t) r);
		return;
	}
	set_d (ry, BYTE, bcd (add, regs.d[ry] & 0xFF, regs.d[rx] & 0xFF));
	prefetch ();
	idle (2);
}

void
M68000::op_nbcd ()
{
	uint16_t op = regs.ir;
	unsigned mode = (op >> 3) & 7, reg = op & 7;
	uint32_t r;
	Ea e;

	if (mode == 1 || (mode == 7 && reg > 1)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, BYTE);
	r = bcd (false, 0, get (e, BYTE));
	prefetch ();
	if (e.kind == Ea::DREG) {
		set_d (reg, BYTE, r);
		idle (2);
		return;
	}
	write_byte (e.addr, (uint8_t) r);
}

/* 1110: the shifts and rotations */
void
M68000::op_1110 ()
{
	if (((regs.ir >> 6) & 3) == 3)
		op_shift_memory ();
	else
		op_shift_register ();
}

struct ShiftResult {
	uint32_t value;
	bool c, x, v;
};

/* A bit at a time: the carry is the last bit out; V, for ASL, any change
 * of the sign bit on the way */
static ShiftResult
shift_bits (unsigned kind, bool left, uint32_t v, uint32_t m, uint32_t top, unsigned count, bool x)
{
	ShiftResult r = { v & m, false, x, false };
	unsigned i;

	if (count == 0) {
		r.c = kind == ROTATE_X ? x : false;
		return r;
	}
	for (i = 0; i < count; i++) {
		uint32_t old = r.value, in = 0;
		bool out;

		if (left) {
			out = (old & top) != 0;
			if (kind == ROTATE)
				in = out;
			else if (kind == ROTATE_X)
				in = r.x;
			r.value = ((old << 1) | in) & m;
			if (kind == SHIFT_ARITH && ((old ^ r.value) & top))
				r.v = true;
		} else {
			out = (old & 1) != 0;
			if (kind == SHIFT_ARITH)
				in = (old & top) ? top : 0;
			else if (kind == ROTATE)
				in = out ? top : 0;
			else if (kind == ROTATE_X)
				in = r.x ? top : 0;
			r.value = ((old >> 1) | in) & m;
		}
		r.c = out;
		if (kind != ROTATE)
			r.x = out;
	}
	if (kind == ROTATE_X)
		r.c = r.x;
	/* ASR past the size: C and X cleared, as the suite has it */
	if (kind == SHIFT_ARITH && !left && count > (unsigned) std::countr_zero (top) + 1)
		r.c = r.x = false;
	return r;
}

/* The register shifts: 6 + 2n clocks, 8 + 2n for a long */
void
M68000::op_shift_register ()
{
	uint16_t op = regs.ir;
	Size s = size_bits_to ((op >> 6) & 3);
	unsigned kind = (op >> 3) & 3, reg = op & 7;
	bool left = (op & 0x100) != 0;
	unsigned count = (op & 0x20) ? (regs.d[(op >> 9) & 7] & 63) :
	                 (((op >> 9) & 7) ? ((op >> 9) & 7) : 8);
	ShiftResult r = shift_bits (kind, left, regs.d[reg], mask (s), msb (s), count, (regs.sr & SR_X) != 0);
	uint16_t f = regs.sr & (uint16_t) ~(SR_N | SR_Z | SR_V | SR_C |
	                                    (count && kind != ROTATE ? SR_X : 0));

	if (r.c)
		f |= SR_C;
	if (count && kind != ROTATE && r.x)
		f |= SR_X;
	if (r.v)
		f |= SR_V;
	if ((r.value & mask (s)) == 0)
		f |= SR_Z;
	if (r.value & msb (s))
		f |= SR_N;
	regs.sr = f;
	set_d (reg, s, r.value);
	prefetch ();
	idle ((s == LONG ? 4 : 2) + 2 * count);
}

/* The memory shifts: a bit of a word */
void
M68000::op_shift_memory ()
{
	uint16_t op = regs.ir, f;
	unsigned kind = (op >> 9) & 3, mode = (op >> 3) & 7, reg = op & 7;
	bool left = (op & 0x100) != 0;
	ShiftResult r;
	Ea e;

	if (op & 0x0800 || mode < 2 || (mode == 7 && reg > 1)) {
		illegal ();
		return;
	}
	e = ea (mode, reg, WORD);
	r = shift_bits (kind, left, read_word (e.addr), 0xFFFF, 0x8000, 1, (regs.sr & SR_X) != 0);
	f = regs.sr & (uint16_t) ~(SR_N | SR_Z | SR_V | SR_C | (kind != ROTATE ? SR_X : 0));
	if (r.c)
		f |= SR_C;
	if (kind != ROTATE && r.x)
		f |= SR_X;
	if (r.v)
		f |= SR_V;
	if (r.value == 0)
		f |= SR_Z;
	if (r.value & 0x8000)
		f |= SR_N;
	regs.sr = f;
	prefetch ();
	write_word (e.addr, (uint16_t) r.value);
}
