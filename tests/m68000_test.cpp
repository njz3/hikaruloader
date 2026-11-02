/*
 * Hikaruloader
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

/* The 68000 core's tests: small programs whose results are known, their
 * flags and clocks, an interrupt, a trap and a division by zero. Run by
 * ctest, or alone: m68000test. */

#include <initializer_list>
#include <utility>

#include "cpu/m68k/m68000.h"

/* 64 KB of RAM, big endian; the interrupts autovectored */
class TestBus : public M68000::Bus {
public:
	uint8_t mem[0x10000];

	TestBus () { memset (mem, 0, sizeof (mem)); }
	uint16_t read16 (uint32_t addr) override
	{
		return (uint16_t) ((mem[addr & 0xFFFF] << 8) | mem[(addr + 1) & 0xFFFF]);
	}
	uint8_t read8 (uint32_t addr) override { return mem[addr & 0xFFFF]; }
	void write16 (uint32_t addr, uint16_t val) override
	{
		mem[addr & 0xFFFF] = (uint8_t) (val >> 8);
		mem[(addr + 1) & 0xFFFF] = (uint8_t) val;
	}
	void write8 (uint32_t addr, uint8_t val) override { mem[addr & 0xFFFF] = val; }

	void put32 (uint32_t addr, uint32_t val)
	{
		write16 (addr, (uint16_t) (val >> 16));
		write16 (addr + 2, (uint16_t) val);
	}
	uint32_t get32 (uint32_t addr) { return ((uint32_t) read16 (addr) << 16) | read16 (addr + 2); }
};

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

#define CODE	0x400
#define STACK	0xF000

/* Loads the program at CODE (its words) and the vectors (a number, an
 * offset in the program), then runs from the reset until a STOP */
static void
run (M68000 &cpu, TestBus &bus, std::initializer_list<uint16_t> code,
     std::initializer_list<std::pair<unsigned, uint32_t>> vectors = {}, int max_steps = 10000)
{
	uint32_t at = CODE;

	memset (bus.mem, 0, sizeof (bus.mem));
	bus.put32 (0, STACK);
	bus.put32 (4, CODE);
	for (const auto &v : vectors)
		bus.put32 (v.first * 4, CODE + v.second);
	for (uint16_t w : code) {
		bus.write16 (at, w);
		at += 2;
	}
	cpu.set_ipl (0);
	cpu.reset ();
	while (!cpu.regs.stopped && !cpu.regs.halted && max_steps-- > 0)
		cpu.step ();
}

/* The flags before the STOP, which loads all of SR: MOVE SR,D7 copied them */
#define FLAGS(cpu_) ((cpu_).regs.d[7] & 0x1F)
#define SAVE_FLAGS 0x40C7

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	TestBus bus;
	M68000 cpu (&bus);
	uint32_t clocks;

	(void) argc;
	(void) argv;

	/* The reset: SSP and PC from the vectors, supervisor, interrupts
	 * masked */
	run (cpu, bus, { 0x4E72, 0x2700 });		/* STOP #$2700 */
	CHECK ("reset ssp", cpu.regs.a[7], STACK);
	CHECK ("reset sr", cpu.regs.sr, 0x2700);
	CHECK ("stop pc", cpu.regs.pc, CODE + 4);

	/* MOVEQ, ADDQ and their clocks */
	run (cpu, bus, { 0x7005, 0x5680, SAVE_FLAGS, 0x4E72, 0x2700 });	/* MOVEQ #5,D0; ADDQ.L #3,D0 */
	CHECK ("moveq / addq.l", cpu.regs.d[0], 8);
	CHECK ("addq.l flags", FLAGS (cpu), 0);
	run (cpu, bus, { 0x4E71, 0x7005, 0x5680, 0x4E72, 0x2700 });
	cpu.reset ();
	cpu.step ();					/* NOP */
	clocks = cpu.step ();				/* MOVEQ */
	CHECK ("moveq clocks", clocks, 4);
	clocks = cpu.step ();				/* ADDQ.L #3,D0 */
	CHECK ("addq.l dn clocks", clocks, 8);

	/* A byte carry: X, C and Z; the rest of D0 kept */
	run (cpu, bus, { 0x70FF, 0x5200, SAVE_FLAGS, 0x4E72, 0x2700 });	/* MOVEQ #-1,D0; ADDQ.B #1,D0 */
	CHECK ("addq.b ff+1", cpu.regs.d[0], 0xFFFFFF00);
	CHECK ("addq.b ff+1 flags", FLAGS (cpu), M68000::SR_X | M68000::SR_Z | M68000::SR_C);

	/* A signed overflow: N and V */
	run (cpu, bus, { 0x243C, 0x7FFF, 0xFFFF,	/* MOVE.L #$7FFFFFFF,D2 */
	                 0x5282,			/* ADDQ.L #1,D2 */
	                 SAVE_FLAGS, 0x4E72, 0x2700 });
	CHECK ("addq.l overflow", cpu.regs.d[2], 0x80000000);
	CHECK ("addq.l overflow flags", FLAGS (cpu), M68000::SR_N | M68000::SR_V);

	/* SUB below zero, CMP keeping X */
	run (cpu, bus, { 0x7001, 0x7202,		/* MOVEQ #1,D0; MOVEQ #2,D1 */
	                 0x9081,			/* SUB.L D1,D0 */
	                 0x7403, 0xB480,		/* MOVEQ #3,D2; CMP.L D0,D2 */
	                 SAVE_FLAGS, 0x4E72, 0x2700 });
	CHECK ("sub.l 1-2", cpu.regs.d[0], 0xFFFFFFFF);
	CHECK ("cmp.l 3-(-1) flags, x kept", FLAGS (cpu), M68000::SR_X | M68000::SR_C);

	/* Memory: (An)+, a long big endian */
	run (cpu, bus, { 0x41F8, 0x2000,		/* LEA $2000.W,A0 */
	                 0x30FC, 0x1234,		/* MOVE.W #$1234,(A0)+ */
	                 0x243C, 0x8000, 0x0001,	/* MOVE.L #$80000001,D2 */
	                 0x2082,			/* MOVE.L D2,(A0) */
	                 0x3610,			/* MOVE.W (A0),D3 */
	                 SAVE_FLAGS, 0x4E72, 0x2700 });
	CHECK ("move.w #,(a0)+", bus.read16 (0x2000), 0x1234);
	CHECK ("(a0)+", cpu.regs.a[0], 0x2002);
	CHECK ("move.l d2,(a0)", bus.get32 (0x2002), 0x80000001);
	CHECK ("move.w (a0),d3", cpu.regs.d[3] & 0xFFFF, 0x8000);
	CHECK ("move.w flags", FLAGS (cpu), M68000::SR_N);

	/* DBRA: 5 turns */
	run (cpu, bus, { 0x7604, 0x7800,		/* MOVEQ #4,D3; MOVEQ #0,D4 */
	                 0x5444,			/* loop: ADDQ.W #2,D4 */
	                 0x51CB, 0xFFFC,		/* DBRA D3,loop */
	                 0x4E72, 0x2700 });
	CHECK ("dbra turns", cpu.regs.d[4], 10);
	CHECK ("dbra count", cpu.regs.d[3] & 0xFFFF, 0xFFFF);

	/* BSR and RTS */
	run (cpu, bus, { 0x6106,			/* BSR.S sub (CODE + 8) */
	                 0x4E72, 0x2700,		/* STOP #$2700 */
	                 0x4E71,			/* NOP */
	                 0x7A2A, 0x4E75 });		/* sub: MOVEQ #42,D5; RTS */
	CHECK ("bsr / rts", cpu.regs.d[5], 42);
	CHECK ("bsr / rts sp", cpu.regs.a[7], STACK);
	CHECK ("bsr / rts pc", cpu.regs.pc, CODE + 6);

	/* MULU, DIVU */
	run (cpu, bus, { 0x7064, 0xC0FC, 0x0003,	/* MOVEQ #100,D0; MULU #3,D0 */
	                 0x80FC, 0x0007,		/* DIVU #7,D0 */
	                 0x4E72, 0x2700 });
	CHECK ("mulu / divu", cpu.regs.d[0], (300 % 7) << 16 | (300 / 7));

	/* An interrupt: level 2 autovectored (vector 26), taken once the mask
	 * lets it; the handler's RTE back after the STOP */
	run (cpu, bus, { 0x46FC, 0x2000,		/* MOVE #$2000,SR */
	                 0x4E72, 0x2000,		/* STOP #$2000 */
	                 0x7E01,			/* MOVEQ #1,D7 */
	                 0x4E72, 0x2700,		/* STOP #$2700 */
	                 0x7C55, 0x4E73 },		/* handler: MOVEQ #$55,D6; RTE */
	     { { 26, 14 } });
	CHECK ("stopped, waiting", cpu.regs.stopped, 1);
	cpu.set_ipl (2);
	clocks = cpu.step ();
	CHECK ("interrupt clocks", clocks, 44);
	CHECK ("interrupt vector", cpu.regs.pc, CODE + 14);
	CHECK ("interrupt mask", cpu.regs.sr & M68000::SR_I, 0x0200);
	CHECK ("interrupt return", bus.get32 (STACK - 4), CODE + 8);
	CHECK ("interrupt saved sr", bus.read16 (STACK - 6), 0x2000);
	cpu.set_ipl (0);
	while (!cpu.regs.stopped)
		cpu.step ();
	CHECK ("handler ran", cpu.regs.d[6], 0x55);
	CHECK ("after rte", cpu.regs.d[7], 1);
	CHECK ("rte sp", cpu.regs.a[7], STACK);

	/* A level below the mask waits */
	run (cpu, bus, { 0x4E72, 0x2300 });		/* STOP #$2300 */
	cpu.set_ipl (3);
	cpu.step ();
	CHECK ("level 3 masked by 3", cpu.regs.stopped, 1);

	/* TRAP #0 (vector 32) from user mode, then a privileged STOP there
	 * (vector 8): the stacks swapped each time */
	run (cpu, bus, { 0x43F8, 0x7000,		/* LEA $7000.W,A1 */
	                 0x4E61,			/* MOVE A1,USP */
	                 0x46FC, 0x0000,		/* MOVE #$0000,SR: user mode */
	                 0x4E40,			/* TRAP #0 */
	                 0x4E72, 0x2700,		/* STOP: privileged */
	                 0x7E07, 0x4E73,		/* trap: MOVEQ #7,D7; RTE */
	                 0x4E72, 0x2700 },		/* privilege: STOP #$2700 */
	     { { 32, 16 }, { 8, 20 } });
	CHECK ("trap handler", cpu.regs.d[7], 7);
	CHECK ("privilege violation in user mode", cpu.regs.pc, CODE + 24);
	CHECK ("privilege: the user stack aside", cpu.regs.other_sp, 0x7000);
	CHECK ("privilege: the supervisor stack", cpu.regs.a[7], STACK - 6);
	CHECK ("privilege: the instruction's pc", bus.get32 (STACK - 4), CODE + 12);

	/* DIVU by zero: vector 5 */
	run (cpu, bus, { 0x7001, 0x80FC, 0x0000,	/* MOVEQ #1,D0; DIVU #0,D0 */
	                 0x4E72, 0x2700,
	                 0x7C05, 0x4E72, 0x2700 },	/* zero: MOVEQ #5,D6; STOP */
	     { { 5, 10 } });
	CHECK ("divide by zero vector", cpu.regs.d[6], 5);
	CHECK ("divide by zero return", bus.get32 (STACK - 4), CODE + 6);

	/* A word read at an odd address: the address error (vector 3) */
	run (cpu, bus, { 0x41F8, 0x2001,		/* LEA $2001.W,A0 */
	                 0x3010,			/* MOVE.W (A0),D0 */
	                 0x4E72, 0x2700,
	                 0x7C03, 0x4E72, 0x2700 },	/* error: MOVEQ #3,D6; STOP */
	     { { 3, 10 } });
	CHECK ("address error vector", cpu.regs.d[6], 3);
	CHECK ("address error address", bus.get32 (STACK - 12), 0x2001);

	printf ("m68000test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
