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

/* The SH-4 MMU's tests (sh4.cpp, "MMU"): small programs run from P2, the
 * TLBs loaded by LDTLB or through their arrays, the misses served by a
 * handler that loads the entry and runs the instruction again, the other
 * exceptions by one that notes EXPEVT and skips it; and the FPU's moves and
 * rounding.
 * Run by ctest, or alone: sh4mmutest. */

#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include <xmmintrin.h>

#include "hl/core.h"
#include "hl/machine.h"
#include "hl/mmap.h"

#include "cpu/sh/sh4.h"

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		unsigned g_ = (unsigned) (got_), w_ = (unsigned) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %X, expected %X\n", name_, #got_, g_, w_); \
		} \
	} while (0)

class TestMachine : public hl::Machine {
public:
	TestMachine () : hl::Machine (NULL) {}
	int run_frame () override { return 0; }
	const char *debug_string () override { return "sh4mmutest"; }
};

static TestMachine *mach;
static hl::Buffer *ram;		/* 16 MB at 0C000000 */
static Sh4 *cpu;

/* The instructions used */
#define MOVL_ST(m, n)	(0x2002 | ((n) << 8) | ((m) << 4))	/* mov.l Rm,@Rn */
#define MOVL_LD(m, n)	(0x6002 | ((n) << 8) | ((m) << 4))	/* mov.l @Rm,Rn */
#define MOVL_DEC(m, n)	(0x2006 | ((n) << 8) | ((m) << 4))	/* mov.l Rm,@-Rn */
#define ADDI(i, n)	(0x7000 | ((n) << 8) | ((i) & 0xFF))	/* add #i,Rn */
#define MOVI(i, n)	(0xE000 | ((n) << 8) | ((i) & 0xFF))	/* mov #i,Rn */
#define STC_SPC(n)	(0x0042 | ((n) << 8))			/* stc spc,Rn */
#define LDC_SPC(m)	(0x404E | ((m) << 8))			/* ldc Rm,spc */
#define JMP(m)		(0x402B | ((m) << 8))			/* jmp @Rm */
#define BRA(d)		(0xA000 | ((d) & 0xFFF))		/* bra PC+4+2d */
#define TRAPA(i)	(0xC300 | (i))
#define LDTLB		0x0038
#define RTE		0x002B
#define NOP		0x0009
#define STOP		BRA (-2)				/* bra . */

#define CODE	0xAC000000u	/* the programs, through P2 */
#define VBR_AT	0xAC001000u
#define PTEH	0xFF000000u
#define PTEL	0xFF000004u
#define TEA	0xFF00000Cu
#define MMUCR	0xFF000010u
#define TRA	0xFF000020u
#define EXPEVT	0xFF000024u

/* PTEL's: a page of 4 KB (SZ0), cacheable, dirty, read and write in any
 * mode; or as asked */
#define PTEL_4K(ppn)	((ppn) | 0x100 | 0x010 | 0x060 | 0x008 | 0x004)

static void
code (uint32_t addr, std::initializer_list<uint16_t> insns)
{
	for (uint16_t inst : insns) {
		ram->put (2, addr & 0x00FFFFFF, inst);
		addr += 2;
	}
}

static uint32_t
reg (unsigned addr)
{
	uint32_t v = 0;
	cpu->debug_read (4, addr, &v);
	return v;
}

static void
set_reg (unsigned addr, uint32_t v)
{
	cpu->debug_write (4, addr, v);
}

/* A reset, privileged in bank 0, interrupts masked; the handlers:
 *  VBR+400	the misses: MMUCR = r10 (URC), PTEL = r8, LDTLB, r13++
 *  VBR+100	the others: r12 = EXPEVT, r14++, SPC += 2 */
static void
start (uint32_t pc)
{
	cpu->reset (hl::RESET_HARD);
	cpu->regs.sr.full = 0x400000F0;
	cpu->regs.vbr = VBR_AT;
	cpu->regs.pc = pc;
	memset (cpu->regs.r, 0, sizeof (cpu->regs.r));
	cpu->regs.r[9] = PTEL;
	cpu->regs.r[11] = MMUCR;
	code (VBR_AT + 0x400, {
		MOVL_ST (10, 11), MOVL_ST (8, 9), LDTLB, ADDI (1, 13), RTE, NOP });
	code (VBR_AT + 0x100, {
		MOVL_LD (15, 12), ADDI (1, 14), STC_SPC (6), ADDI (2, 6), LDC_SPC (6), RTE, NOP });
	cpu->regs.r[15] = EXPEVT;
}

static void
run (void)
{
	cpu->set_state (hl::CPU_STATE_RUN);
	cpu->run (2000);
}

/* LDTLB, then a read and a write through the page */
static void
test_ldtlb (void)
{
	start (CODE);
	ram->put (4, 0x100010, 0x12345678);
	set_reg (PTEH, 0x00400000);
	set_reg (PTEL, PTEL_4K (0x0C100000));
	set_reg (MMUCR, 5 << 10);
	cpu->regs.r[1] = 1;
	cpu->regs.r[2] = MMUCR;
	cpu->regs.r[3] = 0x00400010;
	cpu->regs.r[5] = 0xCAFEF00D;
	code (CODE, { LDTLB, MOVL_ST (1, 2), MOVL_LD (3, 4), MOVL_ST (5, 3), STOP, NOP });
	run ();
	CHECK ("ldtlb", reg (0xF6000500) & 0xFFFFFD00, 0x00400100);
	CHECK ("ldtlb", reg (0xF7000500), PTEL_4K (0x0C100000));
	CHECK ("ldtlb read", cpu->regs.r[4], 0x12345678);
	CHECK ("ldtlb write", ram->get (4, 0x100010), 0xCAFEF00D);
	CHECK ("ldtlb", cpu->regs.r[13], 0);
}

/* A read misses: the handler loads the entry, the read again */
static void
test_read_miss (void)
{
	start (CODE);
	ram->put (4, 0x200020, 0xA5A5A5A5);
	set_reg (MMUCR, 1);
	cpu->regs.r[3] = 0x00500020;
	cpu->regs.r[8] = PTEL_4K (0x0C200000);
	cpu->regs.r[10] = (7 << 10) | 1;
	code (CODE, { NOP, MOVL_LD (3, 4), STOP, NOP });
	run ();
	CHECK ("read miss", cpu->regs.r[4], 0xA5A5A5A5);
	CHECK ("read miss", cpu->regs.r[13], 1);
	CHECK ("read miss EXPEVT", reg (EXPEVT), 0x040);
	CHECK ("read miss TEA", reg (TEA), 0x00500020);
	CHECK ("read miss PTEH", reg (PTEH), 0x00500000);
	CHECK ("read miss SPC", cpu->regs.spc, CODE + 2);
	CHECK ("read miss SR", cpu->regs.sr.full, 0x400000F0);
}

/* A pre-decrement's write misses: the register moved once */
static void
test_write_miss (void)
{
	start (CODE);
	set_reg (MMUCR, 1);
	cpu->regs.r[5] = 0x600DF00D;
	cpu->regs.r[6] = 0x00600008;
	cpu->regs.r[8] = PTEL_4K (0x0C300000);
	cpu->regs.r[10] = (3 << 10) | 1;
	code (CODE, { MOVL_DEC (5, 6), STOP, NOP });
	run ();
	CHECK ("write miss", cpu->regs.r[6], 0x00600004);
	CHECK ("write miss", ram->get (4, 0x300004), 0x600DF00D);
	CHECK ("write miss", cpu->regs.r[13], 1);
	CHECK ("write miss EXPEVT", reg (EXPEVT), 0x060);
}

/* A delay slot's read misses: the branch again */
static void
test_slot_miss (void)
{
	start (CODE);
	ram->put (4, 0x400000, 0x51075107);
	set_reg (MMUCR, 1);
	cpu->regs.r[3] = 0x00700000;
	cpu->regs.r[8] = PTEL_4K (0x0C400000);
	cpu->regs.r[10] = (9 << 10) | 1;
	/* bra to CODE + 12 */
	code (CODE, { NOP, BRA (3), MOVL_LD (3, 4), MOVI (1, 1), MOVI (1, 1), MOVI (1, 1), STOP, NOP });
	run ();
	CHECK ("slot miss", cpu->regs.r[4], 0x51075107);
	CHECK ("slot miss", cpu->regs.r[13], 1);
	CHECK ("slot miss SPC", cpu->regs.spc, CODE + 2);
	CHECK ("slot miss branch", cpu->regs.r[1], 0);
	CHECK ("slot miss PC", cpu->regs.pc, CODE + 12);
}

/* A read-only page written; a clean page written */
static void
test_protection (void)
{
	start (CODE);
	set_reg (MMUCR, 1);
	set_reg (PTEH, 0x00800000);
	set_reg (PTEL, (0x0C500000 | 0x100 | 0x010 | 0x040 | 0x004));	/* PR 2 */
	set_reg (MMUCR, (1 << 10) | 1);
	code (CODE, { LDTLB, MOVL_ST (5, 3), STOP, NOP });
	cpu->regs.r[3] = 0x00800000;
	cpu->regs.r[5] = 0xBAD0BAD0;
	run ();
	CHECK ("read-only", cpu->regs.r[14], 1);
	CHECK ("read-only EXPEVT", cpu->regs.r[12], 0x0C0);
	CHECK ("read-only", ram->get (4, 0x500000), 0);

	start (CODE);
	set_reg (PTEH, 0x00900000);
	set_reg (PTEL, (0x0C600000 | 0x100 | 0x010 | 0x060));		/* D clear */
	set_reg (MMUCR, (2 << 10) | 1);
	code (CODE, { LDTLB, MOVL_ST (5, 3), STOP, NOP });
	cpu->regs.r[3] = 0x00900000;
	cpu->regs.r[5] = 0xBAD0BAD0;
	run ();
	CHECK ("clean page", cpu->regs.r[14], 1);
	CHECK ("clean page EXPEVT", cpu->regs.r[12], 0x080);
}

/* The code through the ITLB: a mapped page, then a miss */
static void
test_fetch (void)
{
	start (CODE);
	code (0xAC009000, { MOVI (42, 4), STOP, NOP });
	code (0xAC00A000, { MOVI (43, 4), STOP, NOP });
	set_reg (PTEH, 0x00020000);
	set_reg (PTEL, PTEL_4K (0x0C009000));
	set_reg (MMUCR, (4 << 10) | 1);
	cpu->regs.r[7] = 0x00020000;
	code (CODE, { LDTLB, JMP (7), NOP });
	run ();
	CHECK ("fetch", cpu->regs.r[4], 42);
	CHECK ("fetch ITLB", reg (0xF2000000) & 0xFFFFFF00, 0x00020100);

	start (CODE);
	set_reg (MMUCR, 1);
	cpu->regs.r[7] = 0x00030000;
	cpu->regs.r[8] = PTEL_4K (0x0C00A000);
	cpu->regs.r[10] = (6 << 10) | 1;
	code (CODE, { JMP (7), NOP });
	run ();
	CHECK ("fetch miss", cpu->regs.r[4], 43);
	CHECK ("fetch miss", cpu->regs.r[13], 1);
	CHECK ("fetch miss EXPEVT", reg (EXPEVT), 0x040);
	CHECK ("fetch miss SPC", cpu->regs.spc, 0x00030000);
}

/* Entries written through the UTLB's arrays; an associative write's
 * invalidation; the ASIDs; 64 KB and 1 MB pages */
static void
test_arrays (void)
{
	start (CODE);
	ram->put (4, 0x734560, 0x0FF5E700);
	ram->put (4, 0x812340, 0x0FF5E701);
	/* Entry 10: 64 KB at 00A30000 (ASID 1), entry 11: 1 MB at 00B00000,
	 * shared */
	set_reg (0xF6000A00, 0x00A30000 | 0x100 | 1);
	set_reg (0xF7000A00, 0x0C730000 | 0x100 | 0x080 | 0x060 | 0x004);
	set_reg (0xF6000B00, 0x00B00000 | 0x100 | 5);
	set_reg (0xF7000B00, 0x0C800000 | 0x100 | 0x080 | 0x010 | 0x060 | 0x004 | 0x002);
	set_reg (PTEH, 1);
	set_reg (MMUCR, 1);
	cpu->regs.r[1] = 0x00A34560;
	cpu->regs.r[2] = 0x00B12340;
	code (CODE, { MOVL_LD (1, 4), MOVL_LD (2, 5), STOP, NOP });
	run ();
	CHECK ("64 KB page", cpu->regs.r[4], 0x0FF5E700);
	CHECK ("1 MB page, shared", cpu->regs.r[5], 0x0FF5E701);
	CHECK ("arrays", cpu->regs.r[13], 0);

	/* The ASID changed: entry 10 misses (the handler maps it again
	 * elsewhere), entry 11 is shared */
	start (CODE);
	set_reg (0xF6000A00, 0x00A30000 | 0x100 | 1);
	set_reg (0xF7000A00, 0x0C730000 | 0x100 | 0x080 | 0x060 | 0x004);
	set_reg (PTEH, 2);
	set_reg (MMUCR, 1);
	ram->put (4, 0x200560, 0x0A51D002);
	cpu->regs.r[1] = 0x00A34560;
	cpu->regs.r[8] = 0x0C200000 | 0x100 | 0x010 | 0x060 | 0x004;	/* 4 KB */
	cpu->regs.r[10] = (20 << 10) | 1;
	code (CODE, { MOVL_LD (1, 4), STOP, NOP });
	run ();
	CHECK ("other ASID", cpu->regs.r[13], 1);
	CHECK ("other ASID", cpu->regs.r[4], 0x0A51D002);

	/* An associative write clears V: the next access misses */
	start (CODE);
	set_reg (0xF6000C00, 0x00C00000 | 0x100);
	set_reg (0xF7000C00, PTEL_4K (0x0C900000));
	set_reg (0xF6000080, 0x00C00000);
	CHECK ("associative", reg (0xF6000C00) & 0x100, 0);
	set_reg (MMUCR, 1);
	ram->put (4, 0xA00000, 0x2A2A2A2A);
	cpu->regs.r[1] = 0x00C00000;
	cpu->regs.r[8] = PTEL_4K (0x0CA00000);
	cpu->regs.r[10] = (12 << 10) | 1;
	code (CODE, { MOVL_LD (1, 4), STOP, NOP });
	run ();
	CHECK ("associative", cpu->regs.r[13], 1);
	CHECK ("associative", cpu->regs.r[4], 0x2A2A2A2A);

	/* MMUCR.TI: all invalid */
	set_reg (MMUCR, 4);
	CHECK ("TI", reg (0xF6000C00) & 0x100, 0);
	CHECK ("TI reads 0", reg (MMUCR) & 4, 0);
}

/* TRAPA, the MMU off: TRA, SPC the next instruction */
static void
test_trapa (void)
{
	start (CODE);
	code (CODE, { NOP, TRAPA (0x20), NOP, MOVI (7, 2), STOP, NOP });
	run ();
	CHECK ("trapa", cpu->regs.r[14], 1);
	CHECK ("trapa EXPEVT", cpu->regs.r[12], 0x160);
	CHECK ("trapa TRA", reg (TRA), 0x80);
	CHECK ("trapa SPC", cpu->regs.spc, CODE + 6);
	CHECK ("trapa", cpu->regs.r[2], 7);
}

/* The MMU off: no translation, the code from blocks as before */
static void
test_off (void)
{
	start (CODE);
	ram->put (4, 0x400010, 0x0FF0FF00);
	cpu->regs.r[3] = 0x0C400010;
	code (CODE, { MOVL_LD (3, 4), STOP, NOP });
	run ();
	CHECK ("off", cpu->regs.r[4], 0x0FF0FF00);
	CHECK ("off", cpu->regs.r[13], 0);
}

/* An FPU register by its name ("fr4"), as the debugger reads it */
static uint32_t
fpu_reg (const char *name)
{
	for (unsigned i = 0; i < cpu->num_registers (); i++)
		if (!strcmp (cpu->register_name (i), name))
			return (uint32_t) cpu->get_register (i);
	return 0xDEADBEEF;
}

/* The FPU's moves: fmov.s moves 32 bits in double precision too (SZ's alone
 * sets the size), and two of them make DRn of FRn, its high half, and
 * FRn+1 (PHARRIER's unsigned division loads 2^31 so); a 64-bit one (SZ)
 * takes FRn from the lower address, FRn+1 from the higher, and stores them
 * back so (the games move pairs of singles that way) */
static void
test_fmov (void)
{
	static const uint32_t base = 0x0C000100;

	start (CODE);
	ram->put (4, base & 0xFFFFFF, 0x00000000);
	ram->put (4, (base + 4) & 0xFFFFFF, 0x41E00000);
	cpu->regs.fpscr.full = 0x00080000;
	cpu->regs.r[0] = base;
	code (CODE, { 0xF509, 0xF408, STOP, NOP });	/* fmov.s @r0+,fr5; fmov.s @r0,fr4 */
	run ();
	CHECK ("fmov.s PR", cpu->regs.r[0], base + 4);
	CHECK ("fmov.s PR FR4", fpu_reg ("fr4"), 0x41E00000);
	CHECK ("fmov.s PR DR4", cpu->regs.f.d[2].f == 2147483648.0, 1);

	start (CODE);
	ram->put (4, base & 0xFFFFFF, 0x3F800000);
	ram->put (4, (base + 4) & 0xFFFFFF, 0x40000000);
	cpu->regs.fpscr.full = 0x00100000;
	cpu->regs.r[0] = base;
	cpu->regs.r[1] = base + 0x10;
	code (CODE, { 0xF409, 0xF14A, STOP, NOP });	/* fmov @r0+,dr4; fmov dr4,@r1 */
	run ();
	CHECK ("fmov SZ", cpu->regs.r[0], base + 8);
	CHECK ("fmov SZ FR4", fpu_reg ("fr4"), 0x3F800000);
	CHECK ("fmov SZ FR5", fpu_reg ("fr5"), 0x40000000);
	CHECK ("fmov SZ store", ram->get (4, (base + 0x10) & 0xFFFFFF), 0x3F800000);
	CHECK ("fmov SZ store", ram->get (4, (base + 0x14) & 0xFFFFFF), 0x40000000);
}

/* The FPU's rounding as FPSCR.RM says: 1/3 to the nearest 3EAAAAAB, toward
 * zero (FPSCR's reset value, 00040001) 3EAAAAAA; the host's own mode back
 * after the slice */
static void
test_fround (void)
{
	static const uint32_t base = 0x0C000100;
	static const struct {
		uint32_t fpscr, want;
	} cases[] = { { 0x00000000, 0x3EAAAAAB }, { 0x00040001, 0x3EAAAAAA } };

	for (const auto &c : cases) {
		start (CODE);
		ram->put (4, base & 0xFFFFFF, 0x3F800000);
		ram->put (4, (base + 4) & 0xFFFFFF, 0x40400000);
		cpu->regs.fpscr.full = c.fpscr;
		cpu->regs.r[0] = base;
		/* fmov.s @r0+,fr4; fmov.s @r0+,fr5; fdiv fr5,fr4 */
		code (CODE, { 0xF409, 0xF509, 0xF453, STOP, NOP });
		unsigned host = _mm_getcsr ();
		run ();
		CHECK ("fdiv rounding", fpu_reg ("fr4"), c.want);
		CHECK ("host's MXCSR back", _mm_getcsr (), host);
	}
}

int
main (int argc, char **argv)
{
	hl::Mmap *mmap;

	(void) argc;
	(void) argv;
	mach = new TestMachine ();
	ram = hl::Buffer::create (16 * MB, 0, hl::Buffer::LE32);
	mach->register_buffer (ram);
	mmap = new hl::Mmap (mach);
	mmap->add_ram (0x0C000000, 0x0CFFFFFF, 0x00FFFFFF, 0, ram, "RAM");
	cpu = new Sh4 (mach, mmap, true, true);

	test_ldtlb ();
	test_read_miss ();
	test_write_miss ();
	test_slot_miss ();
	test_protection ();
	test_fetch ();
	test_arrays ();
	test_trapa ();
	test_off ();
	test_fmov ();
	test_fround ();

	printf ("sh4mmutest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
