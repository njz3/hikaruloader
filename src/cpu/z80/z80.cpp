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

/* The instructions are decoded the usual way: x = bits 7-6, y = bits 5-3,
 * z = bits 2-0, p = y >> 1, q = y & 1. Under a DD or FD prefix, HL is IX or
 * IY, H and L their halves, and (HL) is (IX+d) or (IY+d); where (IX+d) is an
 * operand, H and L stay H and L. The T-states are the documented ones; a
 * prefix counts 4 more, in step (). */

#include <utility>

#include "cpu/z80/z80.h"

#define A	regs.af.b.h
#define F	regs.af.b.l
#define B	regs.bc.b.h
#define C	regs.bc.b.l
#define D	regs.de.b.h
#define E	regs.de.b.l
#define H	regs.hl.b.h
#define L	regs.hl.b.l
#define BC	regs.bc.w
#define DE	regs.de.w
#define HL	regs.hl.w
#define PC	regs.pc
#define SP	regs.sp
#define WZ	regs.wz

#define FC	FLAG_C
#define FN	FLAG_N
#define FPV	FLAG_PV
#define FX	FLAG_X
#define FH	FLAG_H
#define FY	FLAG_Y
#define FZ	FLAG_Z
#define FS	FLAG_S

static bool
even_parity (uint8_t v)
{
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return !(v & 1);
}

/* S, Z, Y, X and P/V of a result */
static uint8_t
szp (uint8_t v)
{
	return (v & (Z80::FLAG_S | Z80::FLAG_Y | Z80::FLAG_X)) |
	       (v ? 0 : Z80::FLAG_Z) |
	       (even_parity (v) ? Z80::FLAG_PV : 0);
}

Z80::Z80 (Bus *bus) :
	bus (bus)
{
	memset (&regs, 0, sizeof (regs));
	reset ();
}

void
Z80::reset ()
{
	PC = 0;
	regs.i = 0;
	regs.r = 0;
	regs.im = 0;
	regs.iff1 = regs.iff2 = false;
	regs.halted = false;
	regs.ei_delay = false;
	regs.nmi_pending = false;
	regs.af.w = 0xFFFF;
	SP = 0xFFFF;
	WZ = 0;
}

/* Memory */

uint16_t
Z80::read16 (uint16_t addr)
{
	uint16_t lo = read8 (addr);
	return lo | (read8 ((uint16_t) (addr + 1)) << 8);
}

void
Z80::write16 (uint16_t addr, uint16_t val)
{
	write8 (addr, (uint8_t) val);
	write8 ((uint16_t) (addr + 1), (uint8_t) (val >> 8));
}

uint16_t
Z80::fetch16 ()
{
	uint16_t val = read16 (PC);
	PC += 2;
	return val;
}

uint8_t
Z80::fetch_opcode ()
{
	/* R counts the M1 cycles, in its 7 low bits */
	regs.r = (regs.r & 0x80) | ((regs.r + 1) & 0x7F);
	return fetch8 ();
}

void
Z80::push16 (uint16_t val)
{
	SP -= 2;
	write16 (SP, val);
}

uint16_t
Z80::pop16 ()
{
	uint16_t val = read16 (SP);
	SP += 2;
	return val;
}

/* Registers */

Z80::Pair &
Z80::index_reg (Index index)
{
	return (index == INDEX_IX) ? regs.ix : (index == INDEX_IY) ? regs.iy : regs.hl;
}

uint8_t
Z80::get_r (unsigned i, Index index)
{
	switch (i) {
	case 0: return B;
	case 1: return C;
	case 2: return D;
	case 3: return E;
	case 4: return index_reg (index).b.h;
	case 5: return index_reg (index).b.l;
	case 7: return A;
	default: return 0xFF;
	}
}

void
Z80::set_r (unsigned i, Index index, uint8_t val)
{
	switch (i) {
	case 0: B = val; break;
	case 1: C = val; break;
	case 2: D = val; break;
	case 3: E = val; break;
	case 4: index_reg (index).b.h = val; break;
	case 5: index_reg (index).b.l = val; break;
	case 7: A = val; break;
	default: break;
	}
}

uint16_t
Z80::get_rp (unsigned i, Index index)
{
	switch (i) {
	case 0: return BC;
	case 1: return DE;
	case 2: return index_reg (index).w;
	default: return SP;
	}
}

void
Z80::set_rp (unsigned i, Index index, uint16_t val)
{
	switch (i) {
	case 0: BC = val; break;
	case 1: DE = val; break;
	case 2: index_reg (index).w = val; break;
	default: SP = val; break;
	}
}

uint16_t
Z80::get_rp2 (unsigned i, Index index)
{
	return (i == 3) ? regs.af.w : get_rp (i, index);
}

void
Z80::set_rp2 (unsigned i, Index index, uint16_t val)
{
	if (i == 3)
		regs.af.w = val;
	else
		set_rp (i, index, val);
}

uint16_t
Z80::operand_addr (Index index)
{
	uint16_t addr;

	if (index == INDEX_HL)
		return HL;
	addr = (uint16_t) (index_reg (index).w + (int8_t) fetch8 ());
	WZ = addr;
	return addr;
}

bool
Z80::condition (unsigned cc)
{
	switch (cc) {
	case 0: return !(F & FZ);
	case 1: return (F & FZ) != 0;
	case 2: return !(F & FC);
	case 3: return (F & FC) != 0;
	case 4: return !(F & FPV);
	case 5: return (F & FPV) != 0;
	case 6: return !(F & FS);
	default: return (F & FS) != 0;
	}
}

/* ALU */

/* ADD ADC SUB SBC AND XOR OR CP */
void
Z80::alu (unsigned op, uint8_t val)
{
	unsigned a = A, carry = F & FC, res;

	switch (op) {
	case 0:
		carry = 0;
		/* fall-through */
	case 1:
		res = a + val + carry;
		F = (res & (FS | FY | FX)) | ((uint8_t) res ? 0 : FZ) |
		    ((a ^ val ^ res) & FH) |
		    (((a ^ ~val) & (a ^ res) & 0x80) ? FPV : 0) |
		    ((res >> 8) & FC);
		A = (uint8_t) res;
		break;
	case 2:
		carry = 0;
		/* fall-through */
	case 3:
		res = a - val - carry;
		F = (res & (FS | FY | FX)) | ((uint8_t) res ? 0 : FZ) |
		    ((a ^ val ^ res) & FH) |
		    (((a ^ val) & (a ^ res) & 0x80) ? FPV : 0) |
		    FN | ((res >> 8) & FC);
		A = (uint8_t) res;
		break;
	case 4:
		A &= val;
		F = szp (A) | FH;
		break;
	case 5:
		A ^= val;
		F = szp (A);
		break;
	case 6:
		A |= val;
		F = szp (A);
		break;
	default:
		/* CP: X and Y come from the operand */
		res = a - val;
		F = (res & FS) | ((uint8_t) res ? 0 : FZ) | (val & (FY | FX)) |
		    ((a ^ val ^ res) & FH) |
		    (((a ^ val) & (a ^ res) & 0x80) ? FPV : 0) |
		    FN | ((res >> 8) & FC);
		break;
	}
}

uint8_t
Z80::inc8 (uint8_t val)
{
	uint8_t res = val + 1;

	F = (F & FC) | (res & (FS | FY | FX)) | (res ? 0 : FZ) |
	    ((res & 0x0F) ? 0 : FH) | ((val == 0x7F) ? FPV : 0);
	return res;
}

uint8_t
Z80::dec8 (uint8_t val)
{
	uint8_t res = val - 1;

	F = (F & FC) | FN | (res & (FS | FY | FX)) | (res ? 0 : FZ) |
	    ((val & 0x0F) ? 0 : FH) | ((val == 0x80) ? FPV : 0);
	return res;
}

uint16_t
Z80::add16 (uint16_t a, uint16_t b)
{
	uint32_t res = (uint32_t) a + b;

	F = (F & (FS | FZ | FPV)) | ((res >> 8) & (FY | FX)) |
	    (((a ^ b ^ res) >> 8) & FH) | ((res >> 16) & FC);
	WZ = a + 1;
	return (uint16_t) res;
}

void
Z80::adc16 (uint16_t val)
{
	uint32_t a = HL, res = a + val + (F & FC);

	F = ((res >> 8) & (FS | FY | FX)) | ((uint16_t) res ? 0 : FZ) |
	    (((a ^ val ^ res) >> 8) & FH) |
	    (((a ^ ~(uint32_t) val) & (a ^ res) & 0x8000) ? FPV : 0) |
	    ((res >> 16) & FC);
	WZ = (uint16_t) (a + 1);
	HL = (uint16_t) res;
}

void
Z80::sbc16 (uint16_t val)
{
	uint32_t a = HL, res = a - val - (F & FC);

	F = ((res >> 8) & (FS | FY | FX)) | ((uint16_t) res ? 0 : FZ) |
	    (((a ^ val ^ res) >> 8) & FH) |
	    (((a ^ val) & (a ^ res) & 0x8000) ? FPV : 0) |
	    FN | ((res >> 16) & FC);
	WZ = (uint16_t) (a + 1);
	HL = (uint16_t) res;
}

/* RLC RRC RL RR SLA SRA SLL SRL */
uint8_t
Z80::rotate (unsigned op, uint8_t val)
{
	uint8_t carry;

	switch (op) {
	case 0: carry = val >> 7; val = (uint8_t) ((val << 1) | carry); break;
	case 1: carry = val & 1; val = (uint8_t) ((val >> 1) | (carry << 7)); break;
	case 2: carry = val >> 7; val = (uint8_t) ((val << 1) | (F & FC)); break;
	case 3: carry = val & 1; val = (uint8_t) ((val >> 1) | ((F & FC) << 7)); break;
	case 4: carry = val >> 7; val = (uint8_t) (val << 1); break;
	case 5: carry = val & 1; val = (uint8_t) ((val >> 1) | (val & 0x80)); break;
	case 6: carry = val >> 7; val = (uint8_t) ((val << 1) | 1); break;
	default: carry = val & 1; val >>= 1; break;
	}
	F = szp (val) | carry;
	return val;
}

/* X and Y from xy: the register, or the high byte of the address */
void
Z80::bit (unsigned n, uint8_t val, uint8_t xy)
{
	F = (F & FC) | FH | (xy & (FY | FX)) |
	    ((val & (1 << n)) ? ((n == 7) ? FS : 0) : (FZ | FPV));
}

void
Z80::daa ()
{
	uint8_t a = A, fix = 0;
	bool carry = (F & FC) != 0, half;

	if ((F & FH) || (a & 0x0F) > 9)
		fix |= 0x06;
	if (carry || a > 0x99) {
		fix |= 0x60;
		carry = true;
	}
	if (F & FN) {
		half = (F & FH) && (a & 0x0F) < 6;
		A = a - fix;
	} else {
		half = (a & 0x0F) > 9;
		A = a + fix;
	}
	F = szp (A) | (F & FN) | (half ? FH : 0) | (carry ? FC : 0);
}

void
Z80::set_szp (uint8_t val)
{
	F = (F & (FH | FN | FC)) | szp (val);
}

/* Execution */

int
Z80::interrupt ()
{
	int t;

	/* After a HALT, PC is already past it */
	regs.halted = false;
	regs.r = (regs.r & 0x80) | ((regs.r + 1) & 0x7F);

	if (regs.nmi_pending) {
		regs.nmi_pending = false;
		regs.iff1 = false;
		push16 (PC);
		PC = 0x0066;
		WZ = PC;
		regs.cycles += 11;
		return 11;
	}

	regs.iff1 = regs.iff2 = false;
	{
		uint8_t data = bus->acknowledge ();

		switch (regs.im) {
		case 2:
			push16 (PC);
			PC = read16 ((uint16_t) ((regs.i << 8) | data));
			t = 19;
			break;
		case 1:
			push16 (PC);
			PC = 0x0038;
			t = 13;
			break;
		default:
			/* Mode 0: the device's instruction, an RST in practice */
			push16 (PC);
			PC = ((data & 0xC7) == 0xC7) ? (data & 0x38) : 0x0038;
			t = 13;
			break;
		}
	}
	WZ = PC;
	regs.cycles += t;
	return t;
}

int
Z80::step ()
{
	Index index = INDEX_HL;
	uint8_t op;
	int t = 0;

	if (regs.nmi_pending || (regs.irq_line && regs.iff1 && !regs.ei_delay))
		return interrupt ();
	regs.ei_delay = false;

	if (regs.halted) {
		/* NOPs until an interrupt */
		regs.r = (regs.r & 0x80) | ((regs.r + 1) & 0x7F);
		regs.cycles += 4;
		return 4;
	}

	op = fetch_opcode ();
	/* A run of prefixes: the last one counts */
	while (op == 0xDD || op == 0xFD) {
		index = (op == 0xDD) ? INDEX_IX : INDEX_IY;
		t += 4;
		op = fetch_opcode ();
	}
	if (op == 0xCB)
		t += execute_cb (index);
	else if (op == 0xED)
		t += execute_ed ();
	else
		t += execute (op, index);
	regs.cycles += t;
	return t;
}

int
Z80::run (int cycles)
{
	int done = 0;

	while (done < cycles)
		done += step ();
	return done;
}

int
Z80::execute (uint8_t op, Index index)
{
	unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
	bool indexed = index != INDEX_HL;
	Pair &hl = index_reg (index);
	uint16_t addr;
	uint8_t val;

	switch (x) {
	case 0:
		switch (z) {
		case 0:
			switch (y) {
			case 0:	/* NOP */
				return 4;
			case 1:	/* EX AF,AF' */
				std::swap (regs.af, regs.af2);
				return 4;
			case 2: { /* DJNZ d */
				int8_t d = (int8_t) fetch8 ();
				if (--B) {
					PC += d;
					WZ = PC;
					return 13;
				}
				return 8;
			}
			case 3: { /* JR d */
				int8_t d = (int8_t) fetch8 ();
				PC += d;
				WZ = PC;
				return 12;
			}
			default: { /* JR cc,d */
				int8_t d = (int8_t) fetch8 ();
				if (condition (y - 4)) {
					PC += d;
					WZ = PC;
					return 12;
				}
				return 7;
			}
			}
		case 1:
			if (!q) {	/* LD rp,nn */
				set_rp (p, index, fetch16 ());
				return 10;
			}
			/* ADD HL,rp */
			hl.w = add16 (hl.w, get_rp (p, index));
			return 11;
		case 2:
			/* y = 2p + q: q chooses between the store and the load */
			switch (y) {
			case 0:	/* LD (BC),A */
				write8 (BC, A);
				WZ = (uint16_t) (((BC + 1) & 0xFF) | (A << 8));
				return 7;
			case 1:	/* LD A,(BC) */
				A = read8 (BC);
				WZ = BC + 1;
				return 7;
			case 2:	/* LD (DE),A */
				write8 (DE, A);
				WZ = (uint16_t) (((DE + 1) & 0xFF) | (A << 8));
				return 7;
			case 3:	/* LD A,(DE) */
				A = read8 (DE);
				WZ = DE + 1;
				return 7;
			case 4:	/* LD (nn),HL */
				addr = fetch16 ();
				write16 (addr, hl.w);
				WZ = addr + 1;
				return 16;
			case 5:	/* LD HL,(nn) */
				addr = fetch16 ();
				hl.w = read16 (addr);
				WZ = addr + 1;
				return 16;
			case 6:	/* LD (nn),A */
				addr = fetch16 ();
				write8 (addr, A);
				WZ = (uint16_t) (((addr + 1) & 0xFF) | (A << 8));
				return 13;
			default: /* LD A,(nn) */
				addr = fetch16 ();
				A = read8 (addr);
				WZ = addr + 1;
				return 13;
			}
		case 3:	/* INC rp, DEC rp */
			set_rp (p, index, (uint16_t) (get_rp (p, index) + (q ? -1 : 1)));
			return 6;
		case 4:	/* INC r */
		case 5:	/* DEC r */
			if (y == 6) {
				addr = operand_addr (index);
				val = read8 (addr);
				write8 (addr, (z == 4) ? inc8 (val) : dec8 (val));
				return indexed ? 19 : 11;
			}
			val = get_r (y, index);
			set_r (y, index, (z == 4) ? inc8 (val) : dec8 (val));
			return 4;
		case 6:	/* LD r,n */
			if (y == 6) {
				addr = operand_addr (index);
				write8 (addr, fetch8 ());
				return indexed ? 15 : 10;
			}
			set_r (y, index, fetch8 ());
			return 7;
		default:
			switch (y) {
			case 0:	/* RLCA */
				A = (uint8_t) ((A << 1) | (A >> 7));
				F = (F & (FS | FZ | FPV)) | (A & (FY | FX | FC));
				break;
			case 1:	/* RRCA */
				F = (F & (FS | FZ | FPV)) | (A & FC);
				A = (uint8_t) ((A >> 1) | (A << 7));
				F |= A & (FY | FX);
				break;
			case 2: { /* RLA */
				uint8_t carry = A >> 7;
				A = (uint8_t) ((A << 1) | (F & FC));
				F = (F & (FS | FZ | FPV)) | (A & (FY | FX)) | carry;
				break;
			}
			case 3: { /* RRA */
				uint8_t carry = A & 1;
				A = (uint8_t) ((A >> 1) | ((F & FC) << 7));
				F = (F & (FS | FZ | FPV)) | (A & (FY | FX)) | carry;
				break;
			}
			case 4:	/* DAA */
				daa ();
				break;
			case 5:	/* CPL */
				A = ~A;
				F = (F & (FS | FZ | FPV | FC)) | FH | FN | (A & (FY | FX));
				break;
			case 6:	/* SCF */
				F = (F & (FS | FZ | FPV)) | FC | (A & (FY | FX));
				break;
			default: /* CCF */
				F = (F & (FS | FZ | FPV)) | ((F & FC) ? FH : 0) |
				    ((F & FC) ^ FC) | (A & (FY | FX));
				break;
			}
			return 4;
		}
	case 1:
		if (op == 0x76) {	/* HALT */
			regs.halted = true;
			return 4;
		}
		/* LD r,r'; with (IX+d), H and L are themselves */
		if (z == 6) {
			addr = operand_addr (index);
			set_r (y, INDEX_HL, read8 (addr));
			return indexed ? 15 : 7;
		}
		if (y == 6) {
			addr = operand_addr (index);
			write8 (addr, get_r (z, INDEX_HL));
			return indexed ? 15 : 7;
		}
		set_r (y, index, get_r (z, index));
		return 4;
	case 2:	/* ALU A,r */
		if (z == 6) {
			addr = operand_addr (index);
			alu (y, read8 (addr));
			return indexed ? 15 : 7;
		}
		alu (y, get_r (z, index));
		return 4;
	default:
		switch (z) {
		case 0:	/* RET cc */
			if (condition (y)) {
				PC = pop16 ();
				WZ = PC;
				return 11;
			}
			return 5;
		case 1:
			if (!q) {	/* POP rp2 */
				set_rp2 (p, index, pop16 ());
				return 10;
			}
			switch (p) {
			case 0:	/* RET */
				PC = pop16 ();
				WZ = PC;
				return 10;
			case 1:	/* EXX */
				std::swap (regs.bc, regs.bc2);
				std::swap (regs.de, regs.de2);
				std::swap (regs.hl, regs.hl2);
				return 4;
			case 2:	/* JP (HL) */
				PC = hl.w;
				return 4;
			default: /* LD SP,HL */
				SP = hl.w;
				return 6;
			}
		case 2:	/* JP cc,nn */
			addr = fetch16 ();
			WZ = addr;
			if (condition (y))
				PC = addr;
			return 10;
		case 3:
			switch (y) {
			case 0:	/* JP nn */
				PC = fetch16 ();
				WZ = PC;
				return 10;
			case 2: { /* OUT (n),A */
				uint8_t n = fetch8 ();
				bus->out ((uint16_t) ((A << 8) | n), A);
				WZ = (uint16_t) (((n + 1) & 0xFF) | (A << 8));
				return 11;
			}
			case 3: { /* IN A,(n) */
				uint16_t port = (uint16_t) ((A << 8) | fetch8 ());
				A = bus->in (port);
				WZ = port + 1;
				return 11;
			}
			case 4: { /* EX (SP),HL */
				uint16_t tmp = read16 (SP);
				write16 (SP, hl.w);
				hl.w = tmp;
				WZ = tmp;
				return 19;
			}
			case 5:	/* EX DE,HL: never IX or IY */
				std::swap (regs.de, regs.hl);
				return 4;
			case 6:	/* DI */
				regs.iff1 = regs.iff2 = false;
				return 4;
			case 7:	/* EI */
				regs.iff1 = regs.iff2 = true;
				regs.ei_delay = true;
				return 4;
			default: /* CB: in step () */
				return 4;
			}
		case 4:	/* CALL cc,nn */
			addr = fetch16 ();
			WZ = addr;
			if (condition (y)) {
				push16 (PC);
				PC = addr;
				return 17;
			}
			return 10;
		case 5:
			if (!q) {	/* PUSH rp2 */
				push16 (get_rp2 (p, index));
				return 11;
			}
			if (p == 0) {	/* CALL nn */
				addr = fetch16 ();
				push16 (PC);
				PC = addr;
				WZ = addr;
				return 17;
			}
			/* DD, ED, FD: in step () */
			return 4;
		case 6:	/* ALU A,n */
			alu (y, fetch8 ());
			return 7;
		default: /* RST */
			push16 (PC);
			PC = (uint16_t) (y * 8);
			WZ = PC;
			return 11;
		}
	}
}

int
Z80::execute_cb (Index index)
{
	uint16_t addr = 0;
	unsigned x, y, z;
	bool memory;
	uint8_t op, val;

	/* DDCB d op: the displacement comes first, and the opcode is not an
	 * M1 fetch */
	if (index != INDEX_HL) {
		addr = operand_addr (index);
		op = fetch8 ();
	} else
		op = fetch_opcode ();
	x = op >> 6;
	y = (op >> 3) & 7;
	z = op & 7;
	memory = (index != INDEX_HL) || (z == 6);
	if (index == INDEX_HL && z == 6)
		addr = HL;

	val = memory ? read8 (addr) : get_r (z, INDEX_HL);
	switch (x) {
	case 0:
		val = rotate (y, val);
		break;
	case 1:	/* BIT: nothing written */
		bit (y, val, memory ? (uint8_t) (WZ >> 8) : val);
		return memory ? ((index != INDEX_HL) ? 16 : 12) : 8;
	case 2:
		val &= (uint8_t) ~(1 << y);
		break;
	default:
		val |= (uint8_t) (1 << y);
		break;
	}
	if (!memory) {
		set_r (z, INDEX_HL, val);
		return 8;
	}
	write8 (addr, val);
	/* Undocumented: DDCB also copies the result to the register */
	if (index != INDEX_HL && z != 6)
		set_r (z, INDEX_HL, val);
	return (index != INDEX_HL) ? 19 : 15;
}

int
Z80::execute_ed ()
{
	static const uint8_t modes[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
	uint8_t op = fetch_opcode ();
	unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
	uint16_t addr;

	if (x == 1) {
		switch (z) {
		case 0: { /* IN r,(C); IN (C) sets the flags alone */
			uint8_t val = bus->in (BC);
			if (y != 6)
				set_r (y, INDEX_HL, val);
			F = (F & FC) | szp (val);
			WZ = BC + 1;
			return 12;
		}
		case 1:	/* OUT (C),r; OUT (C),0 */
			bus->out (BC, (y == 6) ? 0 : get_r (y, INDEX_HL));
			WZ = BC + 1;
			return 12;
		case 2:	/* SBC HL,rp; ADC HL,rp */
			if (q)
				adc16 (get_rp (p, INDEX_HL));
			else
				sbc16 (get_rp (p, INDEX_HL));
			return 15;
		case 3:	/* LD (nn),rp; LD rp,(nn) */
			addr = fetch16 ();
			if (q)
				set_rp (p, INDEX_HL, read16 (addr));
			else
				write16 (addr, get_rp (p, INDEX_HL));
			WZ = addr + 1;
			return 20;
		case 4: { /* NEG */
			uint8_t val = A;
			A = 0;
			alu (2, val);
			return 8;
		}
		case 5:	/* RETN, RETI */
			PC = pop16 ();
			WZ = PC;
			regs.iff1 = regs.iff2;
			return 14;
		case 6:	/* IM */
			regs.im = modes[y];
			return 8;
		default:
			switch (y) {
			case 0:	/* LD I,A */
				regs.i = A;
				return 9;
			case 1:	/* LD R,A */
				regs.r = A;
				return 9;
			case 2:	/* LD A,I */
			case 3:	/* LD A,R */
				A = (y == 2) ? regs.i : regs.r;
				r_read |= y == 3;
				F = (F & FC) | (A & (FS | FY | FX)) | (A ? 0 : FZ) |
				    (regs.iff2 ? FPV : 0);
				return 9;
			case 4: { /* RRD */
				uint8_t m = read8 (HL);
				write8 (HL, (uint8_t) ((A << 4) | (m >> 4)));
				A = (A & 0xF0) | (m & 0x0F);
				set_szp (A);
				F &= ~(FH | FN);
				WZ = HL + 1;
				return 18;
			}
			case 5: { /* RLD */
				uint8_t m = read8 (HL);
				write8 (HL, (uint8_t) ((m << 4) | (A & 0x0F)));
				A = (A & 0xF0) | (m >> 4);
				set_szp (A);
				F &= ~(FH | FN);
				WZ = HL + 1;
				return 18;
			}
			default:
				return 8;
			}
		}
	}

	/* The block instructions: LDI CPI INI OUTI, LDD..., LDIR..., LDDR... */
	if (x == 2 && z <= 3 && y >= 4) {
		int delta = (y & 1) ? -1 : 1;
		bool repeat = (y & 2) != 0;

		switch (z) {
		case 0: { /* LD */
			uint8_t val = read8 (HL), n;
			write8 (DE, val);
			HL += delta;
			DE += delta;
			BC--;
			n = val + A;
			F = (F & (FS | FZ | FC)) | ((n & 0x02) ? FY : 0) | (n & FX) |
			    (BC ? FPV : 0);
			if (repeat && BC) {
				PC -= 2;
				WZ = PC + 1;
				return 21;
			}
			return 16;
		}
		case 1: { /* CP */
			uint8_t val = read8 (HL), res = A - val, n;
			bool half = ((A ^ val ^ res) & FH) != 0;
			HL += delta;
			BC--;
			WZ += delta;
			n = res - (half ? 1 : 0);
			F = (F & FC) | FN | (res & FS) | (res ? 0 : FZ) | (half ? FH : 0) |
			    ((n & 0x02) ? FY : 0) | (n & FX) | (BC ? FPV : 0);
			if (repeat && BC && res) {
				PC -= 2;
				WZ = PC + 1;
				return 21;
			}
			return 16;
		}
		case 2: { /* IN */
			uint8_t val = bus->in (BC);
			WZ = BC + delta;
			write8 (HL, val);
			HL += delta;
			B--;
			F = (B & (FS | FY | FX)) | (B ? 0 : FZ) | ((val & 0x80) ? FN : 0);
			if (repeat && B) {
				PC -= 2;
				return 21;
			}
			return 16;
		}
		default: { /* OUT: B goes down first */
			uint8_t val = read8 (HL);
			B--;
			WZ = BC + delta;
			bus->out (BC, val);
			HL += delta;
			F = (B & (FS | FY | FX)) | (B ? 0 : FZ) | ((val & 0x80) ? FN : 0);
			if (repeat && B) {
				PC -= 2;
				return 21;
			}
			return 16;
		}
		}
	}
	/* The others do nothing */
	return 8;
}
