/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cpu/tlcs90/tlcs90.cpp, Copyright (C) 2026, Réda Chérif-Touil.
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

/* An instruction is decoded into an operation and two operands (the
 * destination x, the source y), then run; those of the ROM are decoded once
 * (set_fixed). The states are the data book's: they grow with the time the
 * address of a memory operand takes. */

#include <utility>

#include "cpu/tlcs90/tlcs90.h"

static uint8_t
parity_flag (uint8_t v)
{
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return (v & 1) ? 0 : Tlcs90::FLAG_V;
}

/* S and Z of a result */
static uint8_t
sz (uint8_t v)
{
	return v ? (v & Tlcs90::FLAG_S) : Tlcs90::FLAG_Z;
}

Tlcs90::Tlcs90 (Bus *bus) :
	unknown_met (false),
	bus (bus),
	r_f (0),
	r_ix (0),
	r_iy (0),
	r_sp (0),
	r_pc (0),
	r_halted (false),
	r_ei_pending (false)
{
	memset (r, 0, sizeof (r));
	memset (alt, 0, sizeof (alt));
	reset ();
}

void
Tlcs90::reset ()
{
	r_pc = 0;
	r_f &= (uint8_t) ~FLAG_I;
	r_halted = false;
	r_ei_pending = false;
}

Tlcs90::Operand
Tlcs90::reg8 (uint8_t n)
{
	Operand o;
	o.kind = Operand::R8;
	o.reg = n;
	return o;
}

Tlcs90::Operand
Tlcs90::reg16 (uint8_t n)
{
	Operand o;
	o.kind = Operand::R16;
	o.reg = n;
	return o;
}

Tlcs90::Operand
Tlcs90::imm (uint16_t v)
{
	Operand o;
	o.kind = Operand::IMM;
	o.value = v;
	return o;
}

Tlcs90::Operand
Tlcs90::mem (uint16_t addr)
{
	Operand o;
	o.kind = Operand::MEM;
	o.value = addr;
	return o;
}

uint16_t
Tlcs90::fetch16 ()
{
	uint8_t lo = fetch ();
	return (uint16_t) (lo | (fetch () << 8));
}

uint16_t
Tlcs90::rd16 (uint16_t addr) const
{
	return (uint16_t) (rd (addr) | (rd ((uint16_t) (addr + 1)) << 8));
}

void
Tlcs90::wr16 (uint16_t addr, uint16_t v)
{
	wr (addr, (uint8_t) v);
	wr ((uint16_t) (addr + 1), (uint8_t) (v >> 8));
}

void
Tlcs90::push (uint16_t v)
{
	r_sp = (uint16_t) (r_sp - 2);
	wr16 (r_sp, v);
}

uint16_t
Tlcs90::pop ()
{
	uint16_t v = rd16 (r_sp);
	r_sp = (uint16_t) (r_sp + 2);
	return v;
}

uint16_t
Tlcs90::r16 (uint8_t n) const
{
	switch (n) {
	case BC: return (uint16_t) (r[B] << 8 | r[C]);
	case DE: return (uint16_t) (r[D] << 8 | r[E]);
	case HL: return (uint16_t) (r[H] << 8 | r[L]);
	case IX: return r_ix;
	case IY: return r_iy;
	case SP: return r_sp;
	case AF: return (uint16_t) (r[A] << 8 | r_f);
	default: return 0;
	}
}

void
Tlcs90::set_r16 (uint8_t n, uint16_t v)
{
	uint8_t hi = (uint8_t) (v >> 8), lo = (uint8_t) v;

	switch (n) {
	case BC: r[B] = hi; r[C] = lo; break;
	case DE: r[D] = hi; r[E] = lo; break;
	case HL: r[H] = hi; r[L] = lo; break;
	case IX: r_ix = v; break;
	case IY: r_iy = v; break;
	case SP: r_sp = v; break;
	case AF: r[A] = hi; r_f = lo; break;
	default: break;
	}
}

uint8_t
Tlcs90::get8 (const Operand &o) const
{
	switch (o.kind) {
	case Operand::R8: return r[o.reg];
	case Operand::IMM: return (uint8_t) o.value;
	case Operand::MEM: return rd (o.value);
	default: return 0;
	}
}

void
Tlcs90::put8 (const Operand &o, uint8_t v)
{
	if (o.kind == Operand::R8)
		r[o.reg] = v;
	else if (o.kind == Operand::MEM)
		wr (o.value, v);
}

uint16_t
Tlcs90::get16 (const Operand &o) const
{
	switch (o.kind) {
	case Operand::R16: return r16 (o.reg);
	case Operand::IMM: return o.value;
	case Operand::MEM: return rd16 (o.value);
	default: return 0;
	}
}

void
Tlcs90::put16 (const Operand &o, uint16_t v)
{
	if (o.kind == Operand::R16)
		set_r16 (o.reg, v);
	else if (o.kind == Operand::MEM)
		wr16 (o.value, v);
}

/* The conditions, cc 0 to 15: F, LT, LE, ULE, OV, MI, Z, C, then their
 * opposites T, GE, GT, UGT, NOV, PL, NZ, NC */
bool
Tlcs90::test (uint8_t cc) const
{
	bool s = r_f & FLAG_S, z = r_f & FLAG_Z, v = r_f & FLAG_V, c = r_f & FLAG_C;
	bool yes;

	switch (cc & 7) {
	case 0: yes = false; break;
	case 1: yes = s != v; break;
	case 2: yes = z || s != v; break;
	case 3: yes = c || z; break;
	case 4: yes = v; break;
	case 5: yes = s; break;
	case 6: yes = z; break;
	default: yes = c; break;
	}
	return (cc & 8) ? !yes : yes;
}

/* The memory operand of a prefix: E0-E6 (gg), E3 and EB (mn), E7 and EF
 * (FF00+n), E8-EE (gg) as a destination; F0-F2 and F4-F6 (ix+d), F3 and F7
 * (HL+A), A taken as signed */
Tlcs90::Operand
Tlcs90::mem_of (uint8_t prefix, uint8_t d)
{
	uint8_t low = prefix & 7;

	switch (prefix & 0xF8) {
	case 0xE0:
	case 0xE8:
		if (low == 3)
			return mem (fetch16 ());
		if (low == 7)
			return mem ((uint16_t) (0xFF00 | fetch ()));
		return based (low, 0);
	default:
		if (low == 3 || low == 7)
			return based (Operand::HL_PLUS_A, 0);
		return based ((uint8_t) (IX + (low & 3)), d);
	}
}

Tlcs90::Operand
Tlcs90::based (uint8_t base, uint8_t d) const
{
	Operand o = mem (0);

	o.base = base;
	o.disp = (int8_t) d;
	o.value = address_of (o);
	return o;
}

uint16_t
Tlcs90::address_of (const Operand &o) const
{
	if (o.base == Operand::HL_PLUS_A)
		return (uint16_t) (r16 (HL) + (int8_t) r[A]);
	return (uint16_t) (r16 (o.base) + o.disp);
}

void
Tlcs90::decode (Instruction &ins)
{
	uint8_t b0 = fetch ();
	/* The relative forms count from the instruction + 2 (TMP90C840 data
	 * book, MPU90-15), the displacement read before the PC it counts
	 * from; after a 16-bit one the PC is one past that */
	auto short_mem = [this] () { return mem ((uint16_t) (0xFF00 | fetch ())); };
	auto rel8 = [this] () { int8_t d = (int8_t) fetch (); return imm ((uint16_t) (d + r_pc)); };
	auto rel16 = [this] () { uint16_t d = fetch16 (); return imm ((uint16_t) (d + r_pc - 1)); };

	ins.wide = false;
	ins.x = ins.y = Operand ();
	switch (b0) {
	case 0x00: ins.set (OP_NOP, 2); return;
	case 0x01: ins.set (OP_HALT, 4); return;
	case 0x02: ins.set (OP_DI, 2); return;
	case 0x03: ins.set (OP_EI, 2); return;
	case 0x07: ins.set (OP_INCX, 10, 6); ins.x = short_mem (); return;
	case 0x08: ins.set (OP_EX, 2); ins.wide = true; ins.x = reg16 (DE); ins.y = reg16 (HL); return;
	case 0x09: ins.set (OP_EX, 2); ins.wide = true; ins.x = reg16 (AF); ins.y = reg16 (AF2); return;
	case 0x0A: ins.set (OP_EXX, 2); return;
	case 0x0B: ins.set (OP_DAA, 4); return;
	case 0x0C: ins.set (OP_RCF, 2); return;
	case 0x0D: ins.set (OP_SCF, 2); return;
	case 0x0E: ins.set (OP_CCF, 2); return;
	case 0x0F: ins.set (OP_DECX, 10, 6); ins.x = short_mem (); return;
	case 0x10: ins.set (OP_CPL, 2); return;
	case 0x11: ins.set (OP_NEG, 2); return;
	case 0x12:
	case 0x13:
		ins.set (b0 == 0x12 ? OP_MUL : OP_DIV, 16);
		ins.y = imm (fetch ());
		return;
	case 0x14:
	case 0x15:
	case 0x16:
		ins.set (OP_ADD, 6);
		ins.wide = true;
		ins.x = reg16 ((uint8_t) (IX + b0 - 0x14));
		ins.y = imm (fetch16 ());
		return;
	case 0x17: ins.set (OP_LDAR, 8); ins.y = rel16 (); return;
	case 0x18: ins.set (OP_DJNZ, 10); ins.x = reg8 (B); ins.y = rel8 (); return;
	case 0x19: ins.set (OP_DJNZ, 10); ins.wide = true; ins.x = reg16 (BC); ins.y = rel8 (); return;
	case 0x1A: ins.set (OP_JP, 8); ins.x = imm (8); ins.y = imm (fetch16 ()); return;
	case 0x1B: ins.set (OP_JR, 10); ins.x = imm (8); ins.y = rel16 (); return;
	case 0x1C: ins.set (OP_CALL, 14); ins.x = imm (8); ins.y = imm (fetch16 ()); return;
	case 0x1D: ins.set (OP_CALLR, 16); ins.y = rel16 (); return;
	case 0x1E: ins.set (OP_RET, 10); ins.x = imm (8); return;
	case 0x1F: ins.set (OP_RETI, 14); return;
	case 0xFF: ins.set (OP_SWI, 20); return;
	default: break;
	}

	switch (b0 & 0xF8) {
	case 0x20:
		if (b0 == 0x27) {
			ins.set (OP_LD, 8); ins.x = reg8 (A); ins.y = short_mem ();
		} else {
			ins.set (OP_LD, 2); ins.x = reg8 (A); ins.y = reg8 (b0 & 7);
		}
		return;
	case 0x28:
		if (b0 == 0x2F) {
			ins.set (OP_LD, 8); ins.x = short_mem (); ins.y = reg8 (A);
		} else {
			ins.set (OP_LD, 2); ins.x = reg8 (b0 & 7); ins.y = reg8 (A);
		}
		return;
	case 0x30:
		if (b0 == 0x37) {
			ins.set (OP_LD, 10); ins.x = short_mem (); ins.y = imm (fetch ());
		} else {
			ins.set (OP_LD, 4); ins.x = reg8 (b0 & 7); ins.y = imm (fetch ());
		}
		return;
	case 0x38:
		ins.wide = true;
		if (b0 == 0x3F) {
			ins.set (OP_LD, 14); ins.x = short_mem (); ins.y = imm (fetch16 ());
			return;
		}
		if (b0 == 0x3B)
			break;
		ins.set (OP_LD, 6); ins.x = reg16 (b0 & 7); ins.y = imm (fetch16 ());
		return;
	case 0x40:
		ins.wide = true;
		if (b0 == 0x47) {
			ins.set (OP_LD, 10); ins.x = reg16 (HL); ins.y = short_mem ();
			return;
		}
		if (b0 == 0x43)
			break;
		ins.set (OP_LD, 4); ins.x = reg16 (HL); ins.y = reg16 (b0 & 7);
		return;
	case 0x48:
		ins.wide = true;
		if (b0 == 0x4F) {
			ins.set (OP_LD, 10); ins.x = short_mem (); ins.y = reg16 (HL);
			return;
		}
		if (b0 == 0x4B)
			break;
		ins.set (OP_LD, 4); ins.x = reg16 (b0 & 7); ins.y = reg16 (HL);
		return;
	case 0x50:
	case 0x58:
		if ((b0 & 7) == 3 || (b0 & 7) == 7)
			break;
		ins.set (b0 < 0x58 ? OP_PUSH : OP_POP, b0 < 0x58 ? 8 : 10);
		ins.wide = true;
		ins.x = reg16 ((b0 & 7) == SP ? (uint8_t) AF : (uint8_t) (b0 & 7));
		return;
	case 0x60: ins.set (alu_op (b0), 8); ins.x = reg8 (A); ins.y = short_mem (); return;
	case 0x68: ins.set (alu_op (b0), 4); ins.x = reg8 (A); ins.y = imm (fetch ()); return;
	case 0x70: ins.set (alu_op (b0), 10); ins.wide = true; ins.x = reg16 (HL); ins.y = short_mem (); return;
	case 0x78: ins.set (alu_op (b0), 6); ins.wide = true; ins.x = reg16 (HL); ins.y = imm (fetch16 ()); return;
	case 0x80:
	case 0x88:
		ins.set (b0 < 0x88 ? OP_INC : OP_DEC, (b0 & 7) == 7 ? 10 : 2);
		ins.x = (b0 & 7) == 7 ? short_mem () : reg8 (b0 & 7);
		return;
	case 0x90:
	case 0x98:
		if ((b0 & 7) == 3)
			break;
		if ((b0 & 7) == 7) {
			ins.set (b0 < 0x98 ? OP_INCW : OP_DECW, 14);
			ins.x = short_mem ();
			return;
		}
		ins.set (b0 < 0x98 ? OP_INC : OP_DEC, 4);
		ins.wide = true;
		ins.x = reg16 (b0 & 7);
		return;
	case 0xA0: ins.set (shift_op (b0), 2); ins.x = reg8 (A); return;
	case 0xA8: ins.set (OP_BIT, 8); ins.x = imm (b0 & 7); ins.y = short_mem (); return;
	case 0xB0: ins.set (OP_RES, 12); ins.x = imm (b0 & 7); ins.y = short_mem (); return;
	case 0xB8: ins.set (OP_SET, 12); ins.x = imm (b0 & 7); ins.y = short_mem (); return;
	case 0xC0:
	case 0xC8:
		ins.set (OP_JR, 8, 4);
		ins.x = imm (b0 & 15);
		ins.y = rel8 ();
		return;
	case 0xE0:
	case 0xE8:
	case 0xF0:
	case 0xF8:
		decode_prefixed (ins, b0);
		return;
	default:
		break;
	}
	ins.set (OP_UNKNOWN, 2);
}

/* The prefixes E0 to FE: a memory operand (E0-F7) or a register (F8-FE),
 * then the opcode of the operation */
void
Tlcs90::decode_prefixed (Instruction &ins, uint8_t b0)
{
	if (b0 >= 0xF8) {
		/* The register g: 8 bits B to A, or 16 bits BC to SP */
		uint8_t g = b0 & 7, b1 = fetch ();

		switch (b1 & 0xF8) {
		case 0x10:
			if (b1 == 0x12 || b1 == 0x13) {
				ins.set (b1 == 0x12 ? OP_MUL : OP_DIV, 18);
				ins.y = reg8 (g);
				return;
			}
			if (b1 >= 0x14 && b1 <= 0x16) {
				ins.set (OP_ADD, 8);
				ins.wide = true;
				ins.x = reg16 ((uint8_t) (IX + b1 - 0x14));
				ins.y = reg16 (g);
				return;
			}
			break;
		case 0x18: ins.set (OP_TSET, 8); ins.x = imm (b1 & 7); ins.y = reg8 (g); return;
		case 0x30:
			if (b1 == 0x37)
				break;
			ins.set (OP_LD, 4); ins.x = reg8 (b1 & 7); ins.y = reg8 (g);
			return;
		case 0x38:
			if (b1 == 0x3B || b1 == 0x3F)
				break;
			ins.set (OP_LD, 6); ins.wide = true; ins.x = reg16 (b1 & 7); ins.y = reg16 (g);
			return;
		case 0x58:
			if (b0 != 0xFE)
				break;
			ins.set ((uint8_t) (OP_LDI + (b1 & 7)), 18, 14);
			return;
		case 0x60: ins.set (alu_op (b1), 4); ins.x = reg8 (A); ins.y = reg8 (g); return;
		case 0x68: ins.set (alu_op (b1), 6); ins.x = reg8 (g); ins.y = imm (fetch ()); return;
		case 0x70: ins.set (alu_op (b1), 8); ins.wide = true; ins.x = reg16 (HL); ins.y = reg16 (g); return;
		case 0xA0: ins.set (shift_op (b1), 4); ins.x = reg8 (g); return;
		case 0xA8: ins.set (OP_BIT, 4); ins.x = imm (b1 & 7); ins.y = reg8 (g); return;
		case 0xB0: ins.set (OP_RES, 4); ins.x = imm (b1 & 7); ins.y = reg8 (g); return;
		case 0xB8: ins.set (OP_SET, 4); ins.x = imm (b1 & 7); ins.y = reg8 (g); return;
		case 0xD0:
		case 0xD8:
			if (b0 != 0xFE)
				break;
			ins.set (OP_RET, 14, 6);
			ins.x = imm (b1 & 15);
			return;
		default:
			break;
		}
		ins.set (OP_UNKNOWN, 2);
		return;
	}

	/* A memory operand. Source forms: E0-E7, F0-F3; destination forms:
	 * E8-EF, F4-F7. The extra states of each address form, after the
	 * data book: (gg) 0, (mn) 4, (FF00+n) 2, (ix+d) 4, (HL+A) 8 */
	uint8_t low = b0 & 7;
	bool indexed = b0 >= 0xF0;
	uint8_t d = (indexed && low != 3 && low != 7) ? fetch () : 0;
	Operand m = mem_of (b0, d);
	unsigned extra = indexed ? ((low == 3 || low == 7) ? 8 : 4) : (low == 3 ? 4 : low == 7 ? 2 : 0);
	uint8_t b1 = fetch ();
	bool destination = (b0 & 0xF8) == 0xE8 || (b0 >= 0xF4 && b0 <= 0xF7);

	if (!destination) {
		switch (b1 & 0xF8) {
		case 0x10:
			if (b1 == 0x10 || b1 == 0x11) {
				ins.set (b1 == 0x10 ? OP_RLD : OP_RRD, 12 + extra);
				ins.x = m;
				return;
			}
			if (b1 == 0x12 || b1 == 0x13) {
				ins.set (b1 == 0x12 ? OP_MUL : OP_DIV, 18 + extra);
				ins.y = m;
				return;
			}
			if (b1 >= 0x14 && b1 <= 0x16) {
				ins.set (OP_ADD, 8 + extra);
				ins.wide = true;
				ins.x = reg16 ((uint8_t) (IX + b1 - 0x14));
				ins.y = m;
				return;
			}
			break;
		case 0x18: ins.set (OP_TSET, 12 + extra); ins.x = imm (b1 & 7); ins.y = m; return;
		case 0x28: ins.set (OP_LD, 6 + extra); ins.x = reg8 (b1 & 7); ins.y = m; return;
		case 0x48:
			if (b1 == 0x4B || b1 == 0x4F)
				break;
			ins.set (OP_LD, 8 + extra); ins.wide = true; ins.x = reg16 (b1 & 7); ins.y = m;
			return;
		case 0x50:
			if (b1 == 0x53 || b1 == 0x57)
				break;
			ins.set (OP_EX, 14 + extra); ins.wide = true; ins.x = m; ins.y = reg16 (b1 & 7);
			return;
		case 0x60: ins.set (alu_op (b1), 6 + extra); ins.x = reg8 (A); ins.y = m; return;
		case 0x70: ins.set (alu_op (b1), 8 + extra); ins.wide = true; ins.x = reg16 (HL); ins.y = m; return;
		case 0x80:
			if (b1 == 0x87) {
				ins.set (OP_INC, 8 + extra);
				ins.x = m;
				return;
			}
			break;
		case 0x88:
			if (b1 == 0x8F) {
				ins.set (OP_DEC, 8 + extra);
				ins.x = m;
				return;
			}
			break;
		case 0x90:
			if (b1 == 0x97) {
				ins.set (OP_INCW, 12 + extra);
				ins.x = m;
				return;
			}
			break;
		case 0x98:
			if (b1 == 0x9F) {
				ins.set (OP_DECW, 12 + extra);
				ins.x = m;
				return;
			}
			break;
		case 0xA0: ins.set (shift_op (b1), 8 + extra); ins.x = m; return;
		case 0xA8: ins.set (OP_BIT, 6 + extra); ins.x = imm (b1 & 7); ins.y = m; return;
		case 0xB0: ins.set (OP_RES, 10 + extra); ins.x = imm (b1 & 7); ins.y = m; return;
		case 0xB8: ins.set (OP_SET, 10 + extra); ins.x = imm (b1 & 7); ins.y = m; return;
		default:
			break;
		}
		ins.set (OP_UNKNOWN, 2);
		return;
	}

	switch (b1 & 0xF8) {
	case 0x20: ins.set (OP_LD, 6 + extra); ins.x = m; ins.y = reg8 (b1 & 7); return;
	case 0x30:
		if (b1 == 0x37) {
			ins.set (OP_LD, 8 + extra); ins.x = m; ins.y = imm (fetch ());
			return;
		}
		break;
	case 0x38:
		if (b1 == 0x3F) {
			ins.set (OP_LD, 12 + extra); ins.wide = true; ins.x = m; ins.y = imm (fetch16 ());
			return;
		}
		/* LDA rr, the address itself; the indexed forms only */
		if (indexed && b1 != 0x3B) {
			ins.set (OP_LDA, 6 + extra); ins.wide = true; ins.x = reg16 (b1 & 7); ins.y = itself (m);
			return;
		}
		break;
	case 0x40:
		if (b1 == 0x43 || b1 == 0x47)
			break;
		ins.set (OP_LD, 8 + extra); ins.wide = true; ins.x = m; ins.y = reg16 (b1 & 7);
		return;
	case 0x68:
		ins.set (alu_op (b1), (b1 == 0x6F ? 8 : 10) + extra);
		ins.x = m;
		ins.y = imm (fetch ());
		return;
	case 0xC0:
	case 0xC8:
		/* JP [cc,] to the address itself: (gg) is gg, (mn) is mn */
		ins.set (OP_JP, 8 + extra, 6 + extra); ins.x = imm (b1 & 15); ins.y = itself (m);
		return;
	case 0xD0:
	case 0xD8:
		ins.set (OP_CALL, 14 + extra, 6 + extra); ins.x = imm (b1 & 15); ins.y = itself (m);
		return;
	default:
		break;
	}
	ins.set (OP_UNKNOWN, 2);
}

void
Tlcs90::take_interrupt (uint16_t vector)
{
	r_halted = false;
	push (r_pc);
	push (r16 (AF));
	r_f &= (uint8_t) ~FLAG_I;
	r_pc = vector;
}

unsigned
Tlcs90::step ()
{
	uint16_t vector = bus->interrupt ((r_f & FLAG_I) != 0);
	uint16_t at;
	Instruction ins;
	bool after_ei;
	unsigned states;

	if (vector) {
		take_interrupt (vector);
		return INTERRUPT_STATES;
	}
	if (r_halted)
		return 2;

	at = r_pc;
	if (at < decoded.size () && decoded[at].length) {
		const Decoded &known = decoded[at];

		ins = known.ins;
		r_pc = (uint16_t) (at + known.length);
		/* The registers an operand's address takes may have changed */
		if (ins.x.base != Operand::FIXED)
			ins.x.value = address_of (ins.x);
		if (ins.y.base != Operand::FIXED)
			ins.y.value = address_of (ins.y);
	} else {
		unsigned length;

		decode (ins);
		if (ins.op == OP_UNKNOWN) {
			if (!unknown_met)
				HL_ERROR ("TLCS-90: unknown instruction %02X at %04X, run as a NOP",
				          rd (at), at);
			unknown_met = true;
		}
		length = (uint16_t) (r_pc - at);
		if (at + length <= decoded.size ()) {
			decoded[at].ins = ins;
			decoded[at].length = (uint8_t) length;
		}
	}
	after_ei = r_ei_pending;
	states = execute (ins);

	/* EI holds the interrupts off for the instruction after it */
	if (after_ei && ins.op != OP_EI) {
		r_ei_pending = false;
		r_f |= FLAG_I;
	}
	return states;
}

void
Tlcs90::alu8 (uint8_t op, const Operand &x, uint8_t v)
{
	uint8_t a = get8 (x);
	unsigned carry = (r_f & FLAG_C) ? 1 : 0;
	unsigned res;

	switch (op) {
	case OP_ADD:
	case OP_ADC:
		res = a + v + (op == OP_ADC ? carry : 0);
		r_f = (uint8_t) ((r_f & FLAG_I) | sz ((uint8_t) res) | ((a ^ v ^ res) & FLAG_H) |
		                 (((a ^ res) & (v ^ res) & 0x80) ? FLAG_V : 0) |
		                 (res > 0xFF ? FLAG_C | FLAG_X : 0));
		put8 (x, (uint8_t) res);
		return;
	case OP_SUB:
	case OP_SBC:
	case OP_CP:
		res = a - v - (op == OP_SBC ? carry : 0);
		r_f = (uint8_t) ((r_f & FLAG_I) | sz ((uint8_t) res) | ((a ^ v ^ res) & FLAG_H) | FLAG_N |
		                 (((a ^ v) & (a ^ res) & 0x80) ? FLAG_V : 0) |
		                 ((res & 0x100) ? FLAG_C | FLAG_X : 0));
		if (op != OP_CP)
			put8 (x, (uint8_t) res);
		return;
	case OP_AND: res = a & v; break;
	case OP_XOR: res = a ^ v; break;
	default: res = a | v; break;
	}
	r_f = (uint8_t) ((r_f & FLAG_I) | sz ((uint8_t) res) | parity_flag ((uint8_t) res) |
	                 (op == OP_AND ? FLAG_H : 0));
	put8 (x, (uint8_t) res);
}

/* ADD, ADC, SUB, SBC, AND, XOR, OR, CP on 16 bits. ADD rr,gg between two
 * registers leaves S, Z and V; every other form sets them */
void
Tlcs90::alu16 (uint8_t op, const Operand &x, uint16_t v, bool flags_of_add)
{
	uint16_t a = get16 (x);
	uint32_t carry = (r_f & FLAG_C) ? 1 : 0;
	uint32_t res;

	switch (op) {
	case OP_ADD:
	case OP_ADC:
		res = a + v + (op == OP_ADC ? carry : 0u);
		if (flags_of_add)
			r_f &= FLAG_S | FLAG_Z | FLAG_I | FLAG_V;
		else
			r_f = (uint8_t) ((r_f & FLAG_I) | ((uint16_t) res ? 0 : FLAG_Z) |
			                 ((res & 0x8000) ? FLAG_S : 0) |
			                 (((a ^ res) & (v ^ res) & 0x8000) ? FLAG_V : 0));
		r_f = (uint8_t) (r_f | (res > 0xFFFF ? FLAG_C | FLAG_X : 0) |
		                 (((a ^ v ^ res) & 0x1000) ? FLAG_H : 0));
		put16 (x, (uint16_t) res);
		return;
	case OP_SUB:
	case OP_SBC:
	case OP_CP:
		res = a - v - (op == OP_SBC ? carry : 0u);
		r_f = (uint8_t) ((r_f & FLAG_I) | FLAG_N | ((uint16_t) res ? 0 : FLAG_Z) |
		                 ((res & 0x8000) ? FLAG_S : 0) | ((res & 0x10000) ? FLAG_C | FLAG_X : 0) |
		                 (((a ^ v ^ res) & 0x1000) ? FLAG_H : 0) |
		                 (((a ^ v) & (a ^ res) & 0x8000) ? FLAG_V : 0));
		if (op != OP_CP)
			put16 (x, (uint16_t) res);
		return;
	case OP_AND: res = a & v; break;
	case OP_XOR: res = a ^ v; break;
	default: res = a | v; break;
	}
	r_f = (uint8_t) ((r_f & FLAG_I) | (res ? 0 : FLAG_Z) | ((res & 0x8000) ? FLAG_S : 0) |
	                 (op == OP_AND ? FLAG_H : 0));
	put16 (x, (uint16_t) res);
}

/* The rotations and shifts. On A they set C and X and leave S, Z and V;
 * elsewhere S, Z and the parity as well */
uint8_t
Tlcs90::shift (uint8_t op, uint8_t v, bool on_a)
{
	unsigned carry = (r_f & FLAG_C) ? 1 : 0;
	unsigned out, res;
	uint8_t result;

	switch (op) {
	case OP_RLC: out = v >> 7; res = (unsigned) (v << 1) | out; break;
	case OP_RRC: out = v & 1; res = (unsigned) (v >> 1) | out << 7; break;
	case OP_RL: out = v >> 7; res = (unsigned) (v << 1) | carry; break;
	case OP_RR: out = v & 1; res = (unsigned) (v >> 1) | carry << 7; break;
	case OP_SLA:
	case OP_SLL: out = v >> 7; res = (unsigned) (v << 1); break;
	case OP_SRA: out = v & 1; res = (unsigned) (v >> 1) | (v & 0x80u); break;
	default: out = v & 1; res = (unsigned) (v >> 1); break;
	}
	result = (uint8_t) res;
	if (on_a)
		r_f &= FLAG_S | FLAG_Z | FLAG_I | FLAG_V;
	else
		r_f = (uint8_t) ((r_f & FLAG_I) | sz (result) | parity_flag (result));
	if (out)
		r_f |= FLAG_C | FLAG_X;
	return result;
}

/* The flags of an 8-bit increment or decrement to v: C kept, X when 0 */
void
Tlcs90::flags_inc_dec8 (uint8_t v, bool up)
{
	r_f = (uint8_t) ((r_f & (FLAG_I | FLAG_C)) | sz (v) | (v ? 0 : FLAG_X));
	if (up)
		r_f |= (uint8_t) ((v == 0x80 ? FLAG_V : 0) | ((v & 15) == 0 ? FLAG_H : 0));
	else
		r_f |= (uint8_t) (FLAG_N | (v == 0x7F ? FLAG_V : 0) | ((v & 15) == 15 ? FLAG_H : 0));
}

void
Tlcs90::daa ()
{
	uint8_t a = r[A], lo = a & 15, hi = a >> 4;
	bool c = r_f & FLAG_C, n = r_f & FLAG_N, h = r_f & FLAG_H;
	uint8_t diff;

	if (c)
		diff = (lo <= 9 && !h) ? 0x60 : 0x66;
	else if (lo >= 10)
		diff = hi <= 8 ? 0x06 : 0x66;
	else if (hi >= 10)
		diff = h ? 0x66 : 0x60;
	else
		diff = h ? 0x06 : 0x00;
	r[A] = (uint8_t) (n ? a - diff : a + diff);
	r_f = (uint8_t) (sz (r[A]) | parity_flag (r[A]) | (r_f & (FLAG_I | FLAG_N)));
	if (c || (lo <= 9 ? hi >= 10 : hi >= 9))
		r_f |= FLAG_X | FLAG_C;
	if (n ? (h && lo <= 5) : lo >= 10)
		r_f |= FLAG_H;
}

unsigned
Tlcs90::execute (const Instruction &ins)
{
	const Operand &x = ins.x, &y = ins.y;

	switch (ins.op) {
	case OP_NOP:
		break;
	case OP_HALT:
		r_halted = true;
		break;
	case OP_DI:
		r_ei_pending = false;
		r_f &= (uint8_t) ~FLAG_I;
		break;
	case OP_EI:
		if (!(r_f & FLAG_I))
			r_ei_pending = true;
		break;
	case OP_SWI:
		take_interrupt (0x0010);
		break;
	case OP_INCX:
	case OP_DECX: {
		bool up = ins.op == OP_INCX;
		uint8_t v;

		/* Only when the last 16-bit result was 0 (X) */
		if (!(r_f & FLAG_X))
			return ins.states_not_taken;
		v = (uint8_t) (get8 (x) + (up ? 1 : 0xFF));
		put8 (x, v);
		flags_inc_dec8 (v, up);
		break;
	}
	case OP_EX:
		if (y.kind == Operand::R16 && y.reg == AF2) {
			std::swap (r[A], alt[6]);
			std::swap (r_f, alt[7]);
		} else {
			uint16_t v = get16 (x);

			put16 (x, get16 (y));
			put16 (y, v);
		}
		break;
	case OP_EXX:
		for (unsigned i = 0; i < 6; i++)
			std::swap (r[i], alt[i]);
		break;
	case OP_DAA:
		daa ();
		break;
	case OP_RCF:
		r_f &= FLAG_S | FLAG_Z | FLAG_I | FLAG_V;
		break;
	case OP_SCF:
		r_f = (uint8_t) ((r_f & (FLAG_S | FLAG_Z | FLAG_I | FLAG_V)) | FLAG_X | FLAG_C);
		break;
	case OP_CCF:
		r_f = (uint8_t) ((r_f & (FLAG_S | FLAG_Z | FLAG_I | FLAG_V)) |
		                 ((r_f & FLAG_C) ? FLAG_H : FLAG_X | FLAG_C));
		break;
	case OP_CPL:
		r[A] = (uint8_t) ~r[A];
		r_f |= FLAG_H | FLAG_N;
		break;
	case OP_NEG: {
		uint8_t v = r[A];

		r[A] = 0;
		alu8 (OP_SUB, reg8 (A), v);
		break;
	}
	case OP_MUL:
		set_r16 (HL, (uint16_t) (r[L] * get8 (y)));
		break;
	case OP_DIV: {
		uint16_t hl = r16 (HL);
		uint8_t v = get8 (y);

		if (!v) {
			r_f |= FLAG_V;
			set_r16 (HL, (uint16_t) (hl << 8 | ((hl >> 8) ^ 0xFF)));
		} else {
			unsigned q = hl / v;

			r[H] = (uint8_t) (hl % v);
			r[L] = (uint8_t) q;
			r_f = (uint8_t) (q > 0xFF ? r_f | FLAG_V : r_f & ~FLAG_V);
		}
		break;
	}
	case OP_ADD: case OP_ADC: case OP_SUB: case OP_SBC:
	case OP_AND: case OP_XOR: case OP_OR: case OP_CP:
		if (ins.wide)
			alu16 (ins.op, x, get16 (y), ins.op == OP_ADD && y.kind == Operand::R16);
		else
			alu8 (ins.op, x, get8 (y));
		break;
	case OP_RLC: case OP_RRC: case OP_RL: case OP_RR:
	case OP_SLA: case OP_SRA: case OP_SLL: case OP_SRL:
		/* RLCA and the like: one byte, two states */
		put8 (x, shift (ins.op, get8 (x), x.kind == Operand::R8 && x.reg == A && ins.states == 2));
		break;
	case OP_LDI: case OP_LDIR: case OP_LDD: case OP_LDDR: {
		bool up = ins.op == OP_LDI || ins.op == OP_LDIR;
		uint16_t step = up ? 1 : 0xFFFF;
		uint16_t bc = (uint16_t) (r16 (BC) - 1);

		wr (r16 (DE), rd (r16 (HL)));
		set_r16 (DE, (uint16_t) (r16 (DE) + step));
		set_r16 (HL, (uint16_t) (r16 (HL) + step));
		set_r16 (BC, bc);
		r_f = (uint8_t) ((r_f & (FLAG_S | FLAG_Z | FLAG_I | FLAG_X | FLAG_C)) | (bc ? FLAG_V : 0));
		if ((ins.op == OP_LDIR || ins.op == OP_LDDR) && bc) {
			r_pc = (uint16_t) (r_pc - 2);
			return ins.states;
		}
		return ins.states_not_taken;
	}
	case OP_CPI: case OP_CPIR: case OP_CPD: case OP_CPDR: {
		bool up = ins.op == OP_CPI || ins.op == OP_CPIR;
		uint8_t v = rd (r16 (HL)), res = (uint8_t) (r[A] - v);
		uint16_t bc = (uint16_t) (r16 (BC) - 1);

		set_r16 (HL, (uint16_t) (r16 (HL) + (up ? 1 : 0xFFFF)));
		set_r16 (BC, bc);
		r_f = (uint8_t) ((r_f & (FLAG_I | FLAG_C)) | sz (res) | ((r[A] ^ v ^ res) & FLAG_H) | FLAG_N |
		                 (bc ? FLAG_V : 0));
		if ((ins.op == OP_CPIR || ins.op == OP_CPDR) && bc && res) {
			r_pc = (uint16_t) (r_pc - 2);
			return ins.states;
		}
		return ins.states_not_taken;
	}
	case OP_RLD:
	case OP_RRD: {
		uint8_t a = r[A], v = get8 (x);

		if (ins.op == OP_RLD) {
			put8 (x, (uint8_t) (v << 4 | (a & 15)));
			r[A] = (uint8_t) ((a & 0xF0) | v >> 4);
		} else {
			put8 (x, (uint8_t) (v >> 4 | a << 4));
			r[A] = (uint8_t) ((a & 0xF0) | (v & 15));
		}
		r_f = (uint8_t) ((r_f & (FLAG_I | FLAG_C)) | sz (r[A]) | parity_flag (r[A]));
		break;
	}
	case OP_LD:
		if (ins.wide)
			put16 (x, get16 (y));
		else
			put8 (x, get8 (y));
		break;
	case OP_LDA:
	case OP_LDAR:
		put16 (x.kind == Operand::NONE ? reg16 (HL) : x, y.value);
		break;
	case OP_PUSH:
		push (get16 (x));
		break;
	case OP_POP:
		put16 (x, pop ());
		break;
	case OP_INC:
	case OP_DEC:
		if (ins.wide) {
			/* rr: X tells the result is 0, nothing else changes */
			uint16_t v = (uint16_t) (get16 (x) + (ins.op == OP_INC ? 1 : 0xFFFF));

			put16 (x, v);
			r_f = (uint8_t) (v ? r_f & ~FLAG_X : r_f | FLAG_X);
		} else {
			bool up = ins.op == OP_INC;
			uint8_t v = (uint8_t) (get8 (x) + (up ? 1 : 0xFF));

			put8 (x, v);
			flags_inc_dec8 (v, up);
		}
		break;
	case OP_INCW:
	case OP_DECW: {
		uint16_t a = get16 (x);
		bool up = ins.op == OP_INCW;
		uint16_t res = (uint16_t) (a + (up ? 1 : 0xFFFF));

		put16 (x, res);
		r_f = (uint8_t) ((r_f & (FLAG_I | FLAG_C)) | (up ? 0 : FLAG_N) | (res ? 0 : FLAG_Z | FLAG_X) |
		                 ((res & 0x8000) ? FLAG_S : 0) | (((a ^ res ^ 1) & 0x1000) ? FLAG_H : 0) |
		                 ((up ? res == 0x8000 : a == 0x8000) ? FLAG_V : 0));
		break;
	}
	case OP_BIT:
	case OP_TSET: {
		uint8_t v = get8 (y), bit = (uint8_t) (1u << x.value);

		r_f = (uint8_t) ((r_f & (FLAG_I | FLAG_C)) | FLAG_H |
		                 ((v & bit) ? (v & bit & FLAG_S) : FLAG_Z | FLAG_V));
		if (ins.op == OP_TSET)
			put8 (y, v | bit);
		break;
	}
	case OP_RES:
		put8 (y, (uint8_t) (get8 (y) & ~(1u << x.value)));
		break;
	case OP_SET:
		put8 (y, (uint8_t) (get8 (y) | 1u << x.value));
		break;
	case OP_DJNZ:
		if (ins.wide) {
			uint16_t bc = (uint16_t) (r16 (BC) - 1);

			set_r16 (BC, bc);
			if (bc)
				r_pc = y.value;
		} else if (--r[B])
			r_pc = y.value;
		break;
	case OP_JP:
	case OP_JR:
		if (!test ((uint8_t) x.value))
			return ins.states_not_taken;
		r_pc = y.value;
		break;
	case OP_CALL:
		if (!test ((uint8_t) x.value))
			return ins.states_not_taken;
		push (r_pc);
		r_pc = y.value;
		break;
	case OP_CALLR:
		push (r_pc);
		r_pc = y.value;
		break;
	case OP_RET:
		if (!test ((uint8_t) x.value))
			return ins.states_not_taken;
		r_pc = pop ();
		break;
	case OP_RETI:
		set_r16 (AF, pop ());
		r_pc = pop ();
		break;
	default:
		/* OP_UNKNOWN: a NOP, step () told */
		break;
	}
	return ins.states;
}

void
Tlcs90::get_registers (Registers *out) const
{
	memset (out, 0, sizeof (*out));
	out->a = r[A]; out->f = r_f; out->b = r[B]; out->c = r[C];
	out->d = r[D]; out->e = r[E]; out->h = r[H]; out->l = r[L];
	out->a2 = alt[6]; out->f2 = alt[7]; out->b2 = alt[0]; out->c2 = alt[1];
	out->d2 = alt[2]; out->e2 = alt[3]; out->h2 = alt[4]; out->l2 = alt[5];
	out->ix = r_ix; out->iy = r_iy; out->sp = r_sp; out->pc = r_pc;
	out->halted = r_halted;
	out->ei_pending = r_ei_pending;
}

void
Tlcs90::set_registers (const Registers *in)
{
	r[A] = in->a; r_f = in->f; r[B] = in->b; r[C] = in->c;
	r[D] = in->d; r[E] = in->e; r[H] = in->h; r[L] = in->l;
	alt[6] = in->a2; alt[7] = in->f2; alt[0] = in->b2; alt[1] = in->c2;
	alt[2] = in->d2; alt[3] = in->e2; alt[4] = in->h2; alt[5] = in->l2;
	r_ix = in->ix; r_iy = in->iy; r_sp = in->sp; r_pc = in->pc;
	r_halted = in->halted != 0;
	r_ei_pending = in->ei_pending != 0;
}
