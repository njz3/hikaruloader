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

#include "cpu/arm/arm7.h"

/* The ARM state's instructions, 32 bits each, the condition in the top four:
 *
 *	cond 000x xxxx ... data processing, PSR transfers, multiplies, SWP,
 *	                   halfword transfers, BX
 *	cond 001x xxxx ... data processing with an immediate, MSR
 *	cond 01xx xxxx ... single data transfers (LDR, STR, the B ones)
 *	cond 100x xxxx ... block transfers (LDM, STM)
 *	cond 101x xxxx ... branches (B, BL)
 *	cond 110x, 1110 .. coprocessors: none here, undefined
 *	cond 1111 xxxx ... SWI
 *
 * r[15] holds the address of the next instruction: while one runs, it is
 * that instruction's address plus 4, and the code reads it plus 8. */

static inline uint32_t
ror32 (uint32_t val, unsigned n)
{
	n &= 31;
	return n ? (val >> n) | (val << (32 - n)) : val;
}

Arm7::Arm7 (Bus *bus) :
	bus (bus),
	ram (NULL),
	ram_size (0)
{
	unsigned cond, flags;

	for (cond = 0; cond < 16; cond++) {
		conditions[cond] = 0;
		for (flags = 0; flags < 16; flags++) {
			bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1, t;

			switch (cond) {
			case 0x0:	t = z; break;
			case 0x1:	t = !z; break;
			case 0x2:	t = c; break;
			case 0x3:	t = !c; break;
			case 0x4:	t = n; break;
			case 0x5:	t = !n; break;
			case 0x6:	t = v; break;
			case 0x7:	t = !v; break;
			case 0x8:	t = c && !z; break;
			case 0x9:	t = !c || z; break;
			case 0xA:	t = n == v; break;
			case 0xB:	t = n != v; break;
			case 0xC:	t = !z && n == v; break;
			case 0xD:	t = z || n != v; break;
			case 0xE:	t = true; break;
			default:	t = false; break;	/* NV: never, on ARMv4 */
			}
			if (t)
				conditions[cond] |= (uint16_t) (1u << flags);
		}
	}
	reset ();
}

void
Arm7::reset ()
{
	memset (&regs, 0, sizeof (regs));
	regs.cpsr = MODE_SVC | PSR_I | PSR_F;
	regs.r[15] = 0;
}

/****************************************************************************
 Modes and banked registers
****************************************************************************/

uint32_t *
Arm7::spsr ()
{
	switch (regs.cpsr & PSR_MODE) {
	case MODE_FIQ:	return &regs.spsr_fiq;
	case MODE_IRQ:	return &regs.spsr_irq;
	case MODE_SVC:	return &regs.spsr_svc;
	case MODE_ABT:	return &regs.spsr_abt;
	case MODE_UND:	return &regs.spsr_und;
	default:	return NULL;
	}
}

/* Where a mode keeps its r13 and r14 while another runs */
static uint32_t *
bank_r13_14 (Arm7::Registers &regs, uint32_t mode)
{
	switch (mode) {
	case Arm7::MODE_FIQ:	return &regs.fiq_r8_14[5];
	case Arm7::MODE_IRQ:	return regs.irq_r13_14;
	case Arm7::MODE_SVC:	return regs.svc_r13_14;
	case Arm7::MODE_ABT:	return regs.abt_r13_14;
	case Arm7::MODE_UND:	return regs.und_r13_14;
	default:		return &regs.usr_r8_14[5];
	}
}

void
Arm7::switch_mode (uint32_t mode)
{
	uint32_t old = regs.cpsr & PSR_MODE;
	unsigned i;

	mode &= PSR_MODE;
	if (mode != old) {
		/* Out: r8-r12 to the FIQ's bank or the others' (shared), r13
		 * and r14 to the mode's */
		uint32_t *r8_12 = (old == MODE_FIQ) ? regs.fiq_r8_14 : regs.usr_r8_14;
		uint32_t *r13_14 = bank_r13_14 (regs, old);
		for (i = 0; i < 5; i++)
			r8_12[i] = regs.r[8 + i];
		r13_14[0] = regs.r[13];
		r13_14[1] = regs.r[14];

		/* In, likewise */
		r8_12 = (mode == MODE_FIQ) ? regs.fiq_r8_14 : regs.usr_r8_14;
		r13_14 = bank_r13_14 (regs, mode);
		for (i = 0; i < 5; i++)
			regs.r[8 + i] = r8_12[i];
		regs.r[13] = r13_14[0];
		regs.r[14] = r13_14[1];
	}
	regs.cpsr = (regs.cpsr & ~PSR_MODE) | mode;
}

uint32_t
Arm7::get_user_reg (unsigned n)
{
	uint32_t mode = regs.cpsr & PSR_MODE;

	if (n < 8 || n == 15 || mode == MODE_USR || mode == MODE_SYS)
		return regs.r[n];
	if (mode == MODE_FIQ || n >= 13)
		return regs.usr_r8_14[n - 8];
	return regs.r[n];
}

void
Arm7::set_user_reg (unsigned n, uint32_t val)
{
	uint32_t mode = regs.cpsr & PSR_MODE;

	if (n < 8 || n == 15 || mode == MODE_USR || mode == MODE_SYS)
		regs.r[n] = (n == 15) ? (val & ~3u) : val;
	else if (mode == MODE_FIQ || n >= 13)
		regs.usr_r8_14[n - 8] = val;
	else
		regs.r[n] = val;
}

void
Arm7::exception (uint32_t mode, uint32_t vector, uint32_t lr, bool fiq_off)
{
	uint32_t old = regs.cpsr;

	switch_mode (mode);
	*spsr () = old;
	regs.r[14] = lr;
	regs.cpsr |= PSR_I;
	if (fiq_off)
		regs.cpsr |= PSR_F;
	regs.cpsr &= ~PSR_T;
	regs.r[15] = vector;
}

/****************************************************************************
 Execution
****************************************************************************/

int
Arm7::run (int cycles)
{
	int done = 0;

	while (done < cycles) {
		if (fast_loops && scan.head && regs.r[15] == scan.head) {
			done += run_scan_loop (cycles - done);
			if (done >= cycles)
				break;
		}
		done += step ();
	}
	return done;
}

/* A data processing instruction's immediate, rotated */
static uint32_t
dp_immediate (uint32_t inst)
{
	uint32_t imm = inst & 0xFF, rot = ((inst >> 8) & 0xF) * 2;

	return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
}

/* From the BNE back to head: the loop, if it is one of those */
void
Arm7::find_scan_loop (uint32_t head)
{
	uint32_t w[7];
	unsigned i, rd, rn, rc;

	scan.head = 0;
	scan.rejected = head;
	if (head & 3 || head + 7 * 4 > ram_size)
		return;
	for (i = 0; i < 7; i++)
		w[i] = load32 (head + 4 * i);
	rd = (w[0] >> 12) & 15;
	rn = (w[0] >> 16) & 15;
	rc = (w[4] >> 12) & 15;
	if ((w[0] & 0xFFF00FFF) != 0xE5D00000 ||	/* LDRB rd, [rn] */
	    (w[1] & 0xFFF0F000) != 0xE3100000 ||	/* TST rd, #mask */
	    ((w[1] >> 16) & 15) != rd ||
	    (w[2] & 0xFF000000) != 0x1A000000 ||	/* BNE out */
	    (w[3] & 0xFFF00000) != 0xE2800000 ||	/* ADD rn, rn, #stride */
	    ((w[3] >> 16) & 15) != rn || ((w[3] >> 12) & 15) != rn ||
	    (w[4] & 0xFFF00000) != 0xE2800000 ||	/* ADD rc, rc, #inc */
	    ((w[4] >> 16) & 15) != rc ||
	    (w[5] & 0xFFF0F000) != 0xE3500000 ||	/* CMP rc, #limit */
	    ((w[5] >> 16) & 15) != rc ||
	    w[6] != 0x1AFFFFF8 ||			/* BNE head */
	    rd == rn || rd == rc || rn == rc || rd == 15 || rn == 15 || rc == 15)
		return;

	memcpy (scan.words, w, sizeof (w));
	scan.rd = rd;
	scan.rn = rn;
	scan.rc = rc;
	scan.mask = dp_immediate (w[1]);
	scan.stride = dp_immediate (w[3]);
	scan.inc = dp_immediate (w[4]);
	scan.limit = dp_immediate (w[5]);
	scan.head = head;
	scan.rejected = 0;
}

/* The turns that neither leave (a byte & mask) nor end (count reaching
 * limit), each of them whole within budget, as step () would take them:
 * LDRB 3, TST 1, BNE not taken 1, ADD 1, ADD 1, CMP 1, BNE taken 3. The rest,
 * one instruction at a time */
int
Arm7::run_scan_loop (int budget)
{
	uint32_t base, count, res, f;
	uint8_t last = 0;
	int done = 0;
	unsigned i;

	/* The same code still, and no interrupt to take on the way */
	for (i = 0; i < 7; i++)
		if (load32 (scan.head + 4 * i) != scan.words[i]) {
			scan.head = 0;
			return 0;
		}
	if ((regs.fiq_line && !(regs.cpsr & PSR_F)) || (regs.irq_line && !(regs.cpsr & PSR_I)))
		return 0;

	base = regs.r[scan.rn];
	count = regs.r[scan.rc];
	/* run () stops past budget: after the turn's last instruction at
	 * the latest when the others came before it */
	while (done + 8 < budget && base < ram_size) {
		uint8_t b = ram[base];

		if ((b & scan.mask) || count + scan.inc == scan.limit)
			break;
		last = b;
		base += scan.stride;
		count += scan.inc;
		done += 11;
	}
	if (!done)
		return 0;

	/* The last turn's LDRB, ADDs and CMP */
	regs.r[scan.rd] = last;
	regs.r[scan.rn] = base;
	regs.r[scan.rc] = count;
	res = count - scan.limit;
	f = regs.cpsr & ~(PSR_N | PSR_Z | PSR_C | PSR_V);
	if (res & 0x80000000)
		f |= PSR_N;
	if (res == 0)
		f |= PSR_Z;
	if (count >= scan.limit)
		f |= PSR_C;
	if (((count ^ scan.limit) & (count ^ res)) >> 31)
		f |= PSR_V;
	regs.cpsr = f;
	regs.cycles += done;
	return done;
}

int
Arm7::step ()
{
	uint32_t inst;
	int cycles;

	/* The interrupts, between instructions: FIQ first */
	if (regs.fiq_line && !(regs.cpsr & PSR_F)) {
		exception (MODE_FIQ, 0x1C, regs.r[15] + 4, true);
		regs.cycles += 3;
		return 3;
	}
	if (regs.irq_line && !(regs.cpsr & PSR_I)) {
		exception (MODE_IRQ, 0x18, regs.r[15] + 4, false);
		regs.cycles += 3;
		return 3;
	}

	inst = load32 (regs.r[15] & ~3u);
	regs.r[15] += 4;
	cycles = condition (inst >> 28) ? execute (inst) : 1;
	regs.cycles += cycles;
	return cycles;
}

bool
Arm7::condition (uint32_t cond) const
{
	return (conditions[cond & 15] >> (regs.cpsr >> 28)) & 1;
}

uint32_t
Arm7::get_reg (unsigned n, uint32_t ahead) const
{
	/* r[15] is the instruction plus 4 */
	return (n == 15) ? regs.r[15] - 4 + ahead : regs.r[n];
}

void
Arm7::set_reg (unsigned n, uint32_t val)
{
	regs.r[n] = (n == 15) ? (val & ~3u) : val;
}

uint32_t
Arm7::read32 (uint32_t addr)
{
	/* An unaligned word reads the aligned one, rotated to the address */
	return ror32 (load32 (addr & ~3u), (addr & 3) * 8);
}

void
Arm7::write32 (uint32_t addr, uint32_t val)
{
	store32 (addr & ~3u, val);
}

int
Arm7::execute (uint32_t inst)
{
	switch ((inst >> 25) & 7) {
	case 0:
		if ((inst & 0x0FFFFFF0) == 0x012FFF10) {
			/* BX: no Thumb here */
			uint32_t target = get_reg (inst & 15);
			if (target & 1)
				HL_ERROR ("ARM7 @%08X: BX to Thumb code (%08X), not emulated",
				          regs.r[15] - 4, target);
			set_reg (15, target);
			return 3;
		}
		if ((inst & 0x0FC000F0) == 0x00000090)
			return multiply (inst);
		if ((inst & 0x0F8000F0) == 0x00800090)
			return multiply_long (inst);
		if ((inst & 0x0FB00FF0) == 0x01000090)
			return swap (inst);
		if ((inst & 0x0E000090) == 0x00000090 && (inst & 0x60))
			return halfword_transfer (inst);
		if ((inst & 0x0D900000) == 0x01000000)
			return psr_transfer (inst);
		return data_processing (inst);
	case 1:
		if ((inst & 0x0D900000) == 0x01000000)
			return psr_transfer (inst);
		return data_processing (inst);
	case 2:
		return single_transfer (inst);
	case 3:
		if (inst & 0x10)
			return undefined (inst);
		return single_transfer (inst);
	case 4:
		return block_transfer (inst);
	case 5:
		return branch (inst);
	case 6:
		return undefined (inst);
	default:
		if (inst & 0x01000000) {
			/* SWI */
			exception (MODE_SVC, 0x08, regs.r[15], false);
			return 3;
		}
		return undefined (inst);
	}
}

int
Arm7::undefined (uint32_t inst)
{
	HL_ERROR ("ARM7 @%08X: undefined instruction %08X", regs.r[15] - 4, inst);
	exception (MODE_UND, 0x04, regs.r[15], false);
	return 3;
}

/****************************************************************************
 Data processing
****************************************************************************/

uint32_t
Arm7::shifter_operand (uint32_t inst, bool *carry)
{
	bool c = regs.cpsr & PSR_C;
	uint32_t val, amount;

	if (inst & (1u << 25)) {
		/* An 8-bit immediate rotated right by twice the rotation */
		uint32_t rot = ((inst >> 8) & 15) * 2;
		val = ror32 (inst & 0xFF, rot);
		*carry = rot ? (val >> 31) : c;
		return val;
	}

	if (inst & 0x10) {
		/* By a register's low byte: r15 reads 12 ahead */
		val = get_reg (inst & 15, 12);
		amount = get_reg ((inst >> 8) & 15) & 0xFF;
		if (amount == 0) {
			*carry = c;
			return val;
		}
		switch ((inst >> 5) & 3) {
		case 0:	/* LSL */
			if (amount < 32) {
				*carry = (val >> (32 - amount)) & 1;
				return val << amount;
			}
			*carry = (amount == 32) ? (val & 1) : 0;
			return 0;
		case 1:	/* LSR */
			if (amount < 32) {
				*carry = (val >> (amount - 1)) & 1;
				return val >> amount;
			}
			*carry = (amount == 32) ? (val >> 31) : 0;
			return 0;
		case 2:	/* ASR */
			if (amount < 32) {
				*carry = (val >> (amount - 1)) & 1;
				return (uint32_t) ((int32_t) val >> amount);
			}
			*carry = val >> 31;
			return (val >> 31) ? 0xFFFFFFFF : 0;
		default: /* ROR */
			amount &= 31;
			if (amount == 0) {
				*carry = val >> 31;
				return val;
			}
			*carry = (val >> (amount - 1)) & 1;
			return ror32 (val, amount);
		}
	}

	/* By an immediate: 0 means 32 for LSR and ASR, RRX for ROR */
	val = get_reg (inst & 15);
	amount = (inst >> 7) & 31;
	switch ((inst >> 5) & 3) {
	case 0:	/* LSL */
		if (amount == 0) {
			*carry = c;
			return val;
		}
		*carry = (val >> (32 - amount)) & 1;
		return val << amount;
	case 1:	/* LSR */
		if (amount == 0) {
			*carry = val >> 31;
			return 0;
		}
		*carry = (val >> (amount - 1)) & 1;
		return val >> amount;
	case 2:	/* ASR */
		if (amount == 0) {
			*carry = val >> 31;
			return (val >> 31) ? 0xFFFFFFFF : 0;
		}
		*carry = (val >> (amount - 1)) & 1;
		return (uint32_t) ((int32_t) val >> amount);
	default:
		if (amount == 0) {
			/* RRX */
			*carry = val & 1;
			return ((uint32_t) c << 31) | (val >> 1);
		}
		*carry = (val >> (amount - 1)) & 1;
		return ror32 (val, amount);
	}
}

int
Arm7::data_processing (uint32_t inst)
{
	unsigned op = (inst >> 21) & 15;
	bool s = inst & (1u << 20);
	unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	bool reg_shift = !(inst & (1u << 25)) && (inst & 0x10);
	bool shift_carry;
	uint32_t a, b, res = 0;
	uint64_t wide;
	bool c = regs.cpsr & PSR_C, v = regs.cpsr & PSR_V;
	bool logical = false, write = true;
	int cycles = reg_shift ? 2 : 1;

	b = shifter_operand (inst, &shift_carry);
	a = get_reg (rn, reg_shift ? 12 : 8);

	switch (op) {
	case 0x0:	/* AND */
		res = a & b;
		logical = true;
		break;
	case 0x1:	/* EOR */
		res = a ^ b;
		logical = true;
		break;
	case 0x2:	/* SUB */
		res = a - b;
		c = a >= b;
		v = ((a ^ b) & (a ^ res)) >> 31;
		break;
	case 0x3:	/* RSB */
		res = b - a;
		c = b >= a;
		v = ((b ^ a) & (b ^ res)) >> 31;
		break;
	case 0x4:	/* ADD */
		wide = (uint64_t) a + b;
		res = (uint32_t) wide;
		c = wide >> 32;
		v = (~(a ^ b) & (a ^ res)) >> 31;
		break;
	case 0x5:	/* ADC */
		wide = (uint64_t) a + b + c;
		res = (uint32_t) wide;
		c = wide >> 32;
		v = (~(a ^ b) & (a ^ res)) >> 31;
		break;
	case 0x6:	/* SBC */
		wide = (uint64_t) a - b - !c;
		res = (uint32_t) wide;
		c = (uint64_t) a >= (uint64_t) b + !(regs.cpsr & PSR_C);
		v = ((a ^ b) & (a ^ res)) >> 31;
		break;
	case 0x7:	/* RSC */
		wide = (uint64_t) b - a - !c;
		res = (uint32_t) wide;
		c = (uint64_t) b >= (uint64_t) a + !(regs.cpsr & PSR_C);
		v = ((b ^ a) & (b ^ res)) >> 31;
		break;
	case 0x8:	/* TST */
		res = a & b;
		logical = true;
		write = false;
		break;
	case 0x9:	/* TEQ */
		res = a ^ b;
		logical = true;
		write = false;
		break;
	case 0xA:	/* CMP */
		res = a - b;
		c = a >= b;
		v = ((a ^ b) & (a ^ res)) >> 31;
		write = false;
		break;
	case 0xB:	/* CMN */
		wide = (uint64_t) a + b;
		res = (uint32_t) wide;
		c = wide >> 32;
		v = (~(a ^ b) & (a ^ res)) >> 31;
		write = false;
		break;
	case 0xC:	/* ORR */
		res = a | b;
		logical = true;
		break;
	case 0xD:	/* MOV */
		res = b;
		logical = true;
		break;
	case 0xE:	/* BIC */
		res = a & ~b;
		logical = true;
		break;
	default:	/* MVN */
		res = ~b;
		logical = true;
		break;
	}

	if (write)
		set_reg (rd, res);

	if (s && write && rd == 15) {
		/* MOVS PC, LR and the like: back from an exception */
		uint32_t *saved = spsr ();
		if (saved) {
			uint32_t psr = *saved;
			switch_mode (psr & PSR_MODE);
			regs.cpsr = psr;
		}
	} else if (s) {
		uint32_t f = regs.cpsr & ~(PSR_N | PSR_Z | PSR_C | PSR_V);
		if (res & 0x80000000)
			f |= PSR_N;
		if (res == 0)
			f |= PSR_Z;
		if (logical ? shift_carry : c)
			f |= PSR_C;
		if (!logical && v)
			f |= PSR_V;
		else if (logical && (regs.cpsr & PSR_V))
			f |= PSR_V;
		regs.cpsr = f;
	}

	if (write && rd == 15)
		cycles += 2;
	return cycles;
}

int
Arm7::psr_transfer (uint32_t inst)
{
	bool use_spsr = inst & (1u << 22);
	uint32_t *saved = spsr ();

	if ((inst & 0x0FBF0FFF) == 0x010F0000) {
		/* MRS */
		uint32_t val = use_spsr ? (saved ? *saved : regs.cpsr) : regs.cpsr;
		set_reg ((inst >> 12) & 15, val);
		return 1;
	}

	if ((inst & 0x0DB0F000) == 0x0120F000) {
		/* MSR, from a register or an immediate, the fields in 19-16 */
		uint32_t val, mask = 0;
		bool unused;

		if (inst & (1u << 25))
			val = shifter_operand (inst, &unused);
		else
			val = get_reg (inst & 15);
		if (inst & (1u << 16)) mask |= 0x000000FF;
		if (inst & (1u << 17)) mask |= 0x0000FF00;
		if (inst & (1u << 18)) mask |= 0x00FF0000;
		if (inst & (1u << 19)) mask |= 0xFF000000;

		if (use_spsr) {
			if (saved)
				*saved = (*saved & ~mask) | (val & mask);
		} else {
			uint32_t psr;
			/* The user mode changes the flags alone */
			if ((regs.cpsr & PSR_MODE) == MODE_USR)
				mask &= 0xFF000000;
			psr = (regs.cpsr & ~mask) | (val & mask);
			switch_mode (psr & PSR_MODE);
			regs.cpsr = psr;
		}
		return 1;
	}

	return undefined (inst);
}

/****************************************************************************
 Multiplies and SWP
****************************************************************************/

int
Arm7::multiply (uint32_t inst)
{
	unsigned rd = (inst >> 16) & 15, rn = (inst >> 12) & 15;
	unsigned rs = (inst >> 8) & 15, rm = inst & 15;
	uint32_t res = regs.r[rm] * regs.r[rs];

	if (inst & (1u << 21))	/* MLA */
		res += regs.r[rn];
	set_reg (rd, res);
	if (inst & (1u << 20)) {
		regs.cpsr &= ~(PSR_N | PSR_Z);
		if (res & 0x80000000)
			regs.cpsr |= PSR_N;
		if (res == 0)
			regs.cpsr |= PSR_Z;
	}
	return (inst & (1u << 21)) ? 4 : 3;
}

int
Arm7::multiply_long (uint32_t inst)
{
	unsigned hi = (inst >> 16) & 15, lo = (inst >> 12) & 15;
	unsigned rs = (inst >> 8) & 15, rm = inst & 15;
	uint64_t res;

	if (inst & (1u << 22))	/* SMULL, SMLAL */
		res = (uint64_t) ((int64_t) (int32_t) regs.r[rm] *
		                  (int64_t) (int32_t) regs.r[rs]);
	else			/* UMULL, UMLAL */
		res = (uint64_t) regs.r[rm] * regs.r[rs];
	if (inst & (1u << 21))
		res += ((uint64_t) regs.r[hi] << 32) | regs.r[lo];
	regs.r[lo] = (uint32_t) res;
	regs.r[hi] = (uint32_t) (res >> 32);
	if (inst & (1u << 20)) {
		regs.cpsr &= ~(PSR_N | PSR_Z);
		if (res >> 63)
			regs.cpsr |= PSR_N;
		if (res == 0)
			regs.cpsr |= PSR_Z;
	}
	return (inst & (1u << 21)) ? 5 : 4;
}

int
Arm7::swap (uint32_t inst)
{
	unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15, rm = inst & 15;
	uint32_t addr = regs.r[rn], src = regs.r[rm], val;

	if (inst & (1u << 22)) {	/* SWPB */
		val = load8 (addr);
		store8 (addr, (uint8_t) src);
	} else {
		val = read32 (addr);
		write32 (addr, src);
	}
	set_reg (rd, val);
	return 4;
}

/****************************************************************************
 Transfers
****************************************************************************/

int
Arm7::halfword_transfer (uint32_t inst)
{
	bool pre = inst & (1u << 24), up = inst & (1u << 23);
	bool wback = inst & (1u << 21), load = inst & (1u << 20);
	unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	unsigned sh = (inst >> 5) & 3;
	uint32_t base = get_reg (rn), offset, addr;

	if (inst & (1u << 22))
		offset = ((inst >> 4) & 0xF0) | (inst & 15);
	else
		offset = regs.r[inst & 15];
	if (!up)
		offset = 0 - offset;
	addr = pre ? base + offset : base;

	if (!load && sh != 1)
		return undefined (inst);	/* LDRD, STRD: ARMv5E */

	if (load) {
		uint32_t val;
		if (sh == 1)		/* LDRH */
			val = load16 (addr & ~1u);
		else if (sh == 2)	/* LDRSB */
			val = (uint32_t) (int32_t) (int8_t) load8 (addr);
		else			/* LDRSH */
			val = (uint32_t) (int32_t) (int16_t) load16 (addr & ~1u);
		if (!pre)
			set_reg (rn, base + offset);
		else if (wback)
			set_reg (rn, addr);
		set_reg (rd, val);
		return (rd == 15) ? 5 : 3;
	}

	/* STRH */
	store16 (addr & ~1u, (uint16_t) get_reg (rd, 12));
	if (!pre)
		set_reg (rn, base + offset);
	else if (wback)
		set_reg (rn, addr);
	return 2;
}

int
Arm7::single_transfer (uint32_t inst)
{
	bool pre = inst & (1u << 24), up = inst & (1u << 23);
	bool byte = inst & (1u << 22), wback = inst & (1u << 21);
	bool load = inst & (1u << 20);
	unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	uint32_t base = get_reg (rn), offset, addr;

	if (inst & (1u << 25)) {
		/* A register shifted by an immediate: the shifter's own
		 * forms, its carry unused */
		bool unused;
		offset = shifter_operand (inst & ~(1u << 25), &unused);
	} else
		offset = inst & 0xFFF;
	if (!up)
		offset = 0 - offset;
	addr = pre ? base + offset : base;

	if (load) {
		uint32_t val = byte ? load8 (addr) : read32 (addr);
		/* Post-indexed, or pre-indexed with W: the base is written
		 * back, and a load into it wins */
		if (!pre)
			set_reg (rn, base + offset);
		else if (wback)
			set_reg (rn, addr);
		set_reg (rd, val);
		return (rd == 15) ? 5 : 3;
	}

	/* r15 stored is the instruction plus 12 */
	if (byte)
		store8 (addr, (uint8_t) get_reg (rd, 12));
	else
		write32 (addr, get_reg (rd, 12));
	if (!pre)
		set_reg (rn, base + offset);
	else if (wback)
		set_reg (rn, addr);
	return 2;
}

int
Arm7::block_transfer (uint32_t inst)
{
	bool pre = inst & (1u << 24), up = inst & (1u << 23);
	bool s = inst & (1u << 22), wback = inst & (1u << 21);
	bool load = inst & (1u << 20);
	unsigned rn = (inst >> 16) & 15;
	uint32_t list = inst & 0xFFFF;
	uint32_t base = regs.r[rn], addr, final;
	unsigned n = 0, i;
	/* The user bank, for S without r15 in an LDM (or any STM) */
	bool user = s && !(load && (list & 0x8000));

	for (i = 0; i < 16; i++)
		n += (list >> i) & 1;
	if (n == 0)
		return 1;

	/* The lowest register at the lowest address, always */
	if (up) {
		addr = pre ? base + 4 : base;
		final = base + n * 4;
	} else {
		addr = pre ? base - n * 4 : base - n * 4 + 4;
		final = base - n * 4;
	}

	if (load) {
		if (wback)
			regs.r[rn] = final;
		for (i = 0; i < 16; i++) {
			if (!(list & (1u << i)))
				continue;
			uint32_t val = load32 (addr & ~3u);
			if (user)
				set_user_reg (i, val);
			else
				set_reg (i, val);
			addr += 4;
		}
		if (s && (list & 0x8000)) {
			/* LDM ... PC}^: back from an exception */
			uint32_t *saved = spsr ();
			if (saved) {
				uint32_t psr = *saved;
				switch_mode (psr & PSR_MODE);
				regs.cpsr = psr;
			}
		}
		return n + 2 + ((list & 0x8000) ? 2 : 0);
	}

	for (i = 0; i < 16; i++) {
		if (!(list & (1u << i)))
			continue;
		uint32_t val = (i == 15) ? get_reg (15, 12) :
		               user ? get_user_reg (i) : regs.r[i];
		store32 (addr & ~3u, val);
		addr += 4;
	}
	if (wback)
		regs.r[rn] = final;
	return n + 1;
}

int
Arm7::branch (uint32_t inst)
{
	/* The offset in words, from the instruction plus 8 */
	int32_t offset = (int32_t) (inst << 8) >> 6;

	if (inst & (1u << 24))	/* BL */
		regs.r[14] = regs.r[15];
	regs.r[15] = regs.r[15] + 4 + offset;
	/* A BNE seven instructions back: a scan loop, perhaps */
	if (fast_loops && inst == 0x1AFFFFF8 && regs.r[15] != scan.head && regs.r[15] != scan.rejected)
		find_scan_loop (regs.r[15]);
	return 3;
}
