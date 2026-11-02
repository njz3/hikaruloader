/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cpu/m68000/m68000.cpp,
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

/* The 68000's bus cycles, its operands, flags and exceptions; the
 * instructions are in m68000-ops.cpp */

#include "cpu/m68k/m68000.h"

M68000::M68000 (Bus *bus) :
	bus (bus),
	au (0),
	ird (0),
	faulting (false),
	fault_at (),
	kept (),
	kept_au (0),
	kept_ird (0),
	run_end (0)
{
	memset (&regs, 0, sizeof (regs));
	regs.sr = SR_S | SR_I;
}

/*
 * Bus cycles
 *
 * A word or long access at an odd address faults before its cycle starts;
 * after a fault, none starts.
 */

void
M68000::fault (uint32_t addr, bool write, bool program)
{
	faulting = true;
	fault_at.addr = addr;
	fault_at.write = write;
	fault_at.program = program;
	kept = regs;
	kept_au = au;
	kept_ird = ird;
}

/* Whether the instruction met a fault: the registers then as they were at
 * it */
bool
M68000::faulted ()
{
	if (!faulting)
		return false;
	faulting = false;
	/* The level on the pins is the board's, not the instruction's */
	kept.ipl = regs.ipl;
	regs = kept;
	au = kept_au;
	ird = kept_ird;
	return true;
}

uint16_t
M68000::read_word (uint32_t addr)
{
	if (faulting)
		return 0;
	if (addr & 1) {
		fault (addr, false, false);
		return 0;
	}
	regs.cycles += 4;
	return bus->read16 (addr & 0xFFFFFF);
}

uint16_t
M68000::fetch_word (uint32_t addr)
{
	if (faulting)
		return 0;
	if (addr & 1) {
		fault (addr, false, true);
		return 0;
	}
	regs.cycles += 4;
	return bus->read16 (addr & 0xFFFFFF);
}

uint8_t
M68000::read_byte (uint32_t addr)
{
	if (faulting)
		return 0;
	regs.cycles += 4;
	return bus->read8 (addr & 0xFFFFFF);
}

uint32_t
M68000::read_long (uint32_t addr)
{
	uint32_t hi = read_word (addr);
	return (hi << 16) | read_word (addr + 2);
}

void
M68000::write_word (uint32_t addr, uint16_t v)
{
	if (faulting)
		return;
	if (addr & 1) {
		fault (addr, true, false);
		return;
	}
	regs.cycles += 4;
	bus->write16 (addr & 0xFFFFFF, v);
}

void
M68000::write_byte (uint32_t addr, uint8_t v)
{
	if (faulting)
		return;
	regs.cycles += 4;
	bus->write8 (addr & 0xFFFFFF, v);
}

void
M68000::write_long (uint32_t addr, uint32_t v)
{
	write_word (addr, (uint16_t) (v >> 16));
	write_word (addr + 2, (uint16_t) v);
}

/* The read-modify-write instructions write the low word first: an odd
 * address faults on that word */
void
M68000::write_long_low_first (uint32_t addr, uint32_t v)
{
	write_word (addr + 2, (uint16_t) v);
	write_word (addr, (uint16_t) (v >> 16));
}

uint32_t
M68000::read (Size s, uint32_t addr)
{
	switch (s) {
	case BYTE:
		return read_byte (addr);
	case WORD:
		return read_word (addr);
	default:
		return read_long (addr);
	}
}

void
M68000::write (Size s, uint32_t addr, uint32_t v)
{
	switch (s) {
	case BYTE:
		write_byte (addr, (uint8_t) v);
		break;
	case WORD:
		write_word (addr, (uint16_t) v);
		break;
	default:
		write_long (addr, v);
		break;
	}
}

void
M68000::write_rmw (Size s, uint32_t addr, uint32_t v)
{
	if (s == LONG)
		write_long_low_first (addr, v);
	else
		write (s, addr, v);
}

/*
 * The prefetch queue: IR holds the instruction, IRC the word after it. Each
 * extension word taken from IRC fetches the next one.
 */

uint16_t
M68000::next_word ()
{
	uint16_t v = regs.irc;
	regs.irc = fetch_word (au);
	au += 2;
	return v;
}

uint32_t
M68000::next_long ()
{
	uint32_t hi = next_word ();
	return (hi << 16) | next_word ();
}

/* An instruction's end: IRC moves to IR and the next word comes in */
void
M68000::prefetch ()
{
	regs.ir = regs.irc;
	regs.irc = fetch_word (au);
	au += 2;
	regs.pc = au - 4;
}

/* A change of flow: both words of the queue from the new address */
void
M68000::jump (uint32_t addr)
{
	au = addr;
	regs.ir = fetch_word (au);
	regs.irc = fetch_word (au + 2);
	au += 4;
	regs.pc = addr;
}

void
M68000::push_long (uint32_t v)
{
	regs.a[7] -= 4;
	write_long (regs.a[7], v);
}

uint32_t
M68000::pop_long ()
{
	uint32_t v = read_long (regs.a[7]);
	regs.a[7] += 4;
	return v;
}

/*
 * The status register and the flags
 */

void
M68000::set_sr (uint16_t v)
{
	v &= SR_MASK;
	if ((v ^ regs.sr) & SR_S) {
		uint32_t sp = regs.a[7];
		regs.a[7] = regs.other_sp;
		regs.other_sp = sp;
	}
	regs.sr = v;
}

bool
M68000::cond (unsigned cc) const
{
	bool c = regs.sr & SR_C, v = regs.sr & SR_V, z = regs.sr & SR_Z, n = regs.sr & SR_N;

	switch (cc & 15) {
	case 0: return true;
	case 1: return false;
	case 2: return !c && !z;	/* HI */
	case 3: return c || z;		/* LS */
	case 4: return !c;		/* CC */
	case 5: return c;		/* CS */
	case 6: return !z;		/* NE */
	case 7: return z;		/* EQ */
	case 8: return !v;		/* VC */
	case 9: return v;		/* VS */
	case 10: return !n;		/* PL */
	case 11: return n;		/* MI */
	case 12: return n == v;		/* GE */
	case 13: return n != v;		/* LT */
	case 14: return !z && n == v;	/* GT */
	default: return z || n != v;	/* LE */
	}
}

/* N and Z from the result, V and C cleared, X kept */
void
M68000::flags_nz (Size s, uint32_t r)
{
	uint16_t f = regs.sr & (uint16_t) ~(SR_N | SR_Z | SR_V | SR_C);

	if ((r & mask (s)) == 0)
		f |= SR_Z;
	if (r & msb (s))
		f |= SR_N;
	regs.sr = f;
}

/* r = x + y; X and C alike */
void
M68000::flags_add (Size s, uint32_t x, uint32_t y, uint32_t r)
{
	uint32_t m = msb (s);
	uint16_t f = regs.sr & (uint16_t) ~(SR_X | SR_N | SR_Z | SR_V | SR_C);

	if ((r & mask (s)) == 0)
		f |= SR_Z;
	if (r & m)
		f |= SR_N;
	if ((~(x ^ y) & (x ^ r)) & m)
		f |= SR_V;
	if (((x & y) | (~r & (x | y))) & m)
		f |= SR_C | SR_X;
	regs.sr = f;
}

/* r = x - y; X and C alike */
void
M68000::flags_sub (Size s, uint32_t x, uint32_t y, uint32_t r)
{
	uint32_t m = msb (s);
	uint16_t f = regs.sr & (uint16_t) ~(SR_X | SR_N | SR_Z | SR_V | SR_C);

	if ((r & mask (s)) == 0)
		f |= SR_Z;
	if (r & m)
		f |= SR_N;
	if (((x ^ y) & (x ^ r)) & m)
		f |= SR_V;
	if (((~x & y) | (r & (~x | y))) & m)
		f |= SR_C | SR_X;
	regs.sr = f;
}

/*
 * Operands
 */

/* d8(An,Xn) and d8(PC,Xn): the index word */
uint32_t
M68000::index (uint16_t ext) const
{
	unsigned n = (ext >> 12) & 7;
	uint32_t x = (ext & 0x8000) ? regs.a[n] : regs.d[n];

	if (!(ext & 0x0800))
		x = (uint32_t) (int16_t) x;
	return x + (uint32_t) (int8_t) ext;
}

/* Reads the extension words of an operand and works out where it is; (An)+
 * and -(An) change An here */
M68000::Ea
M68000::ea (unsigned mode, unsigned reg, Size s)
{
	Ea e = { Ea::MEM, (uint8_t) reg, 0, 0 };
	uint32_t step = step_of (s, reg);

	switch (mode) {
	case 0:
		e.kind = Ea::DREG;
		break;
	case 1:
		e.kind = Ea::AREG;
		break;
	case 2:
		e.addr = regs.a[reg];
		break;
	case 3:
		e.addr = regs.a[reg];
		regs.a[reg] += step;
		break;
	case 4:
		idle (2);
		regs.a[reg] -= step;
		e.addr = regs.a[reg];
		break;
	case 5:
		e.addr = regs.a[reg] + (uint32_t) (int16_t) next_word ();
		break;
	case 6:
		idle (2);
		e.addr = regs.a[reg] + index (next_word ());
		break;
	default:
		switch (reg) {
		case 0:
			e.addr = (uint32_t) (int16_t) next_word ();
			break;
		case 1:
			e.addr = next_long ();
			break;
		case 2:
			e.addr = au - 2;
			e.addr += (uint32_t) (int16_t) next_word ();
			break;
		case 3:
			idle (2);
			e.addr = au - 2;
			e.addr += index (next_word ());
			break;
		default:
			e.kind = Ea::IMM;
			e.imm = s == LONG ? next_long () : (next_word () & mask (s));
			break;
		}
		break;
	}
	return e;
}

uint32_t
M68000::get (const Ea &e, Size s)
{
	switch (e.kind) {
	case Ea::DREG:
		return regs.d[e.reg] & mask (s);
	case Ea::AREG:
		return regs.a[e.reg] & mask (s);
	case Ea::MEM:
		return read (s, e.addr);
	default:
		return e.imm;
	}
}

/* LEA and PEA: the address of a control operand */
uint32_t
M68000::control_address (unsigned mode, unsigned reg)
{
	if (mode == 6 || (mode == 7 && reg == 3)) {
		uint32_t base = mode == 6 ? regs.a[reg] : au - 2, addr;

		idle (2);
		addr = base + index (next_word ());
		idle (2);
		return addr;
	}
	return ea (mode, reg, LONG).addr;
}

/*
 * Exceptions
 */

/* The frame of groups 1 and 2 on the supervisor stack: the low word of the
 * return PC first, then SR, then its high word */
void
M68000::push_frame (uint16_t sr, uint32_t return_pc)
{
	regs.a[7] -= 6;
	write_word (regs.a[7] + 4, (uint16_t) return_pc);
	write_word (regs.a[7], sr);
	write_word (regs.a[7] + 2, (uint16_t) (return_pc >> 16));
}

/* That frame taken back by RTE and RTR: its SR, and the PC it returns to */
uint32_t
M68000::pop_frame (uint16_t &sr)
{
	uint32_t hi = read_word (regs.a[7] + 2), lo;

	sr = read_word (regs.a[7]);
	lo = read_word (regs.a[7] + 4);
	regs.a[7] += 6;
	return (hi << 16) | lo;
}

/* The new PC from the vector, and the queue filled from there */
void
M68000::enter_vector (uint32_t vector)
{
	uint32_t target = read_long (vector * 4);

	au = target;
	regs.ir = fetch_word (au);
	idle (2);
	regs.irc = fetch_word (au + 2);
	au += 4;
	regs.pc = target;
}

/* Groups 1 and 2: the return PC and SR on the supervisor stack, then the
 * new PC from the vector */
void
M68000::exception (unsigned vector, uint32_t return_pc)
{
	uint16_t old = regs.sr;

	set_sr ((uint16_t) ((regs.sr | SR_S) & ~SR_T));
	push_frame (old, return_pc);
	enter_vector (vector);
}

/* Group 0: the access that faulted, the instruction and the PC of the
 * prefetch on the supervisor stack. A fault while doing so halts */
void
M68000::address_error (AddressError e)
{
	uint16_t old = regs.sr;
	unsigned fc = e.program ? (supervisor () ? 6 : 2) : (supervisor () ? 5 : 1);
	uint16_t status = (uint16_t) ((ird & 0xFFE0) | (e.write ? 0 : 0x10) | (e.program ? 0x08 : 0) | fc);
	uint32_t pc = au - 4;

	idle (4);
	set_sr ((uint16_t) ((regs.sr | SR_S) & ~SR_T));
	regs.a[7] -= 14;
	write_word (regs.a[7] + 12, (uint16_t) pc);
	write_word (regs.a[7] + 8, old);
	write_word (regs.a[7] + 10, (uint16_t) (pc >> 16));
	write_word (regs.a[7] + 6, ird);
	write_word (regs.a[7] + 4, (uint16_t) e.addr);
	write_word (regs.a[7], status);
	write_word (regs.a[7] + 2, (uint16_t) (e.addr >> 16));
	enter_vector (3);
	if (faulted ())
		regs.halted = true;
}

/* The interrupt acknowledge, then as a group 1 exception. Odin's guess:
 * the autovector's cycle waits for the E clock, 4 clocks more than a
 * vectored one, which gives the manual's 44 clocks */
void
M68000::interrupt (unsigned level)
{
	uint16_t old = regs.sr;
	uint32_t return_pc = regs.pc;
	int vector;

	regs.stopped = false;
	idle (6);
	vector = bus->int_ack (level);
	regs.cycles += 4;
	if (vector < 0) {
		vector = (int) (24 + level);
		idle (4);
	}
	set_sr ((uint16_t) (((regs.sr | SR_S) & ~(SR_T | SR_I)) | (level << 8)));
	push_frame (old, return_pc);
	enter_vector ((uint32_t) vector);
}

/* DIVU and DIVS by 0: the flags, then the trap of vector 5 */
void
M68000::divide_by_zero ()
{
	regs.sr = (uint16_t) ((regs.sr & ~(SR_N | SR_V | SR_C)) | SR_Z);
	idle (8);
	exception (5, au - 2);
}

/* A privilege violation and the illegal instructions: the PC of the
 * instruction */
void
M68000::privilege ()
{
	idle (4);
	exception (8, regs.pc);
}

void
M68000::illegal (unsigned vector)
{
	idle (4);
	exception (vector, regs.pc);
}

/*
 * Control
 */

void
M68000::reset ()
{
	regs.stopped = false;
	regs.halted = false;
	regs.nmi_edge = false;
	regs.sr = SR_S | SR_I;
	regs.a[7] = read_long (0);
	jump (read_long (4));
	if (faulted ())
		regs.halted = true;
}

void
M68000::set_ipl (unsigned level)
{
	level &= 7;
	if (level == 7 && regs.ipl != 7)
		regs.nmi_edge = true;
	regs.ipl = (uint8_t) level;
}

uint32_t
M68000::step ()
{
	uint64_t start = regs.cycles;
	unsigned mask_level;

	if (regs.halted) {
		regs.cycles += 4;
		return 4;
	}
	mask_level = (regs.sr & SR_I) >> 8;
	if (regs.nmi_edge) {
		regs.nmi_edge = false;
		interrupt (7);
	} else if (regs.ipl > mask_level)
		interrupt (regs.ipl);
	else if (regs.stopped)
		regs.cycles += 4;
	else {
		bool trace = (regs.sr & SR_T) != 0;

		ird = regs.ir;
		execute ();
		if (trace && !regs.stopped && !faulting) {
			idle (4);
			exception (9, regs.pc);
		}
	}
	if (faulted ())
		address_error (fault_at);
	return (uint32_t) (regs.cycles - start);
}

uint32_t
M68000::run (uint32_t cycles)
{
	uint64_t start = regs.cycles;

	run_end = start + cycles;
	while (regs.cycles < run_end)
		step ();
	return (uint32_t) (regs.cycles - start);
}
