/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
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

/* The TLCS-90 core's tests (the CPU of the 837-13551's TMP90PH44): small
 * programs whose results the 1988 data book gives, the flags of the ALU, an
 * interrupt and its return. Run by ctest, or alone: tlcs90test. */

#include <initializer_list>

#include "cpu/tlcs90/tlcs90.h"

/* 64 KB of RAM; an interrupt pending at vector until taken */
class TestBus : public Tlcs90::Bus {
public:
	uint8_t mem[0x10000];
	uint16_t vector;

	TestBus () : vector (0) { memset (mem, 0, sizeof (mem)); }
	uint8_t read (uint16_t addr) override { return mem[addr]; }
	void write (uint16_t addr, uint8_t val) override { mem[addr] = val; }
	uint16_t interrupt (bool maskable) override
	{
		uint16_t v = maskable ? vector : 0;
		if (v)
			vector = 0;
		return v;
	}
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

/* Loads the program at 0, runs it until HALT; the states it took */
static unsigned
run (Tlcs90 &cpu, TestBus &bus, std::initializer_list<uint8_t> code, int max_steps = 10000)
{
	unsigned states = 0;
	size_t i = 0;

	memset (bus.mem, 0, sizeof (bus.mem));
	for (uint8_t b : code)
		bus.mem[i++] = b;
	cpu.reset ();
	while (!cpu.halted () && max_steps-- > 0)
		states += cpu.step ();
	return states;
}

/* The flags but X, which the data book leaves out of most tables */
#define FLAGS(cpu_) ((cpu_).f () & ~(Tlcs90::FLAG_X | Tlcs90::FLAG_I))

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	TestBus bus;
	Tlcs90 cpu (&bus);

	(void) argc;
	(void) argv;

	/* LD rr,nn (38+r), LD (mn),A (EB n m 26), LD A,(mn) (E3 n m 2E) */
	run (cpu, bus, { 0x3E, 0x00, 0xF0,		/* LD SP,F000 */
	                 0x36, 0x5A,			/* LD A,5A */
	                 0xEB, 0x00, 0x90, 0x26,	/* LD (9000),A */
	                 0x36, 0x00,			/* LD A,00 */
	                 0xE3, 0x00, 0x90, 0x2E,	/* LD A,(9000) */
	                 0x3A, 0x34, 0x12,		/* LD HL,1234 */
	                 0x01 });			/* HALT */
	CHECK ("ld (mn),a", bus.mem[0x9000], 0x5A);
	CHECK ("ld a,(mn)", cpu.a (), 0x5A);
	CHECK ("ld hl,nn", cpu.hl (), 0x1234);
	CHECK ("ld sp,nn", cpu.sp (), 0xF000);

	/* ADD A,n: 7F + 01, a signed overflow and a half carry */
	run (cpu, bus, { 0x36, 0x7F, 0x68, 0x01, 0x01 });
	CHECK ("add a", cpu.a (), 0x80);
	CHECK ("add flags", FLAGS (cpu), Tlcs90::FLAG_S | Tlcs90::FLAG_H | Tlcs90::FLAG_V);

	/* SUB A,n: 00 - 01, a borrow (C and X) */
	run (cpu, bus, { 0x36, 0x00, 0x6A, 0x01, 0x01 });
	CHECK ("sub a", cpu.a (), 0xFF);
	CHECK ("sub flags", FLAGS (cpu), Tlcs90::FLAG_S | Tlcs90::FLAG_H | Tlcs90::FLAG_N | Tlcs90::FLAG_C);
	CHECK ("sub x", cpu.f () & Tlcs90::FLAG_X, Tlcs90::FLAG_X);

	/* CP A,n equal: Z, A kept */
	run (cpu, bus, { 0x36, 0x42, 0x6F, 0x42, 0x01 });
	CHECK ("cp a", cpu.a (), 0x42);
	CHECK ("cp flags", FLAGS (cpu), Tlcs90::FLAG_Z | Tlcs90::FLAG_N);

	/* AND A,n: H set, V the parity (00: even) */
	run (cpu, bus, { 0x36, 0xF0, 0x6C, 0x0F, 0x01 });
	CHECK ("and flags", FLAGS (cpu), Tlcs90::FLAG_Z | Tlcs90::FLAG_H | Tlcs90::FLAG_V);

	/* DJNZ: B from 5 to 0, A counting the turns */
	run (cpu, bus, { 0x30, 0x05,			/* LD B,5 */
	                 0x36, 0x00,			/* LD A,0 */
	                 0x86,				/* INC A */
	                 0x18, 0xFD,			/* DJNZ -3 */
	                 0x01 });
	CHECK ("djnz turns", cpu.a (), 5);
	CHECK ("djnz b", cpu.bc () >> 8, 0);

	/* JR cc: Z after DEC to 0 (C6, cc 6), NZ not taken (CE, cc 14) */
	run (cpu, bus, { 0x36, 0x01,			/* LD A,1 */
	                 0x8E,				/* DEC A */
	                 0xCE, 0x02,			/* JR NZ,+2 */
	                 0xC6, 0x02,			/* JR Z,+2 */
	                 0x36, 0x99,			/* LD A,99 (skipped) */
	                 0x01 });
	CHECK ("jr z taken", cpu.a (), 0x00);

	/* CALL, RET, PUSH, POP */
	run (cpu, bus, { 0x3E, 0x00, 0xF0,		/* LD SP,F000 */
	                 0x3A, 0x34, 0x12,		/* LD HL,1234 */
	                 0x52,				/* PUSH HL */
	                 0x3A, 0x00, 0x00,		/* LD HL,0 */
	                 0x58,				/* POP BC */
	                 0x1C, 0x0F, 0x00,		/* CALL 000F */
	                 0x01,				/* 000E HALT */
	                 0x1E });			/* 000F RET */
	CHECK ("push pop", cpu.bc (), 0x1234);
	CHECK ("call returns", cpu.pc (), 0x000F);
	CHECK ("call's sp", cpu.sp (), 0xF000);

	/* MUL HL,n: L x n; DIV HL,n: L the quotient, H the remainder */
	run (cpu, bus, { 0x3A, 0x0C, 0x00, 0x12, 0x0B, 0x01 });
	CHECK ("mul", cpu.hl (), 0x0C * 0x0B);
	run (cpu, bus, { 0x3A, 0x64, 0x00, 0x13, 0x07, 0x01 });
	CHECK ("div quotient", cpu.hl () & 0xFF, 100 / 7);
	CHECK ("div remainder", cpu.hl () >> 8, 100 % 7);

	/* States: LD A,n 4, ADD A,n 4, HALT 4 */
	CHECK ("states", run (cpu, bus, { 0x36, 0x01, 0x68, 0x01, 0x01 }), 12);

	/* An interrupt: none while IFF is clear; after EI, taken once the
	 * instruction after it has run, PC and AF pushed, IFF cleared; RETI
	 * gives them back */
	memset (bus.mem, 0, sizeof (bus.mem));
	{
		static const uint8_t code[] = {
			0x3E, 0x00, 0xF0,		/* 0000 LD SP,F000 */
			0x36, 0x11,			/* 0003 LD A,11 */
			0x03,				/* 0005 EI */
			0x00,				/* 0006 NOP */
			0x00,				/* 0007 NOP */
			0x01,				/* 0008 HALT */
		};
		memcpy (bus.mem, code, sizeof (code));
		bus.mem[0x60] = 0x36;			/* 0060 LD A,22 */
		bus.mem[0x61] = 0x22;
		bus.mem[0x62] = 0x1F;			/* 0062 RETI */
	}
	cpu.reset ();
	bus.vector = 0x60;
	cpu.step ();
	CHECK ("no interrupt with IFF clear", cpu.pc (), 0x0003);
	cpu.step ();
	cpu.step ();
	CHECK ("ei", cpu.pc (), 0x0006);
	cpu.step ();
	CHECK ("not taken right after ei", cpu.pc (), 0x0007);
	CHECK ("taken", cpu.step (), Tlcs90::INTERRUPT_STATES);
	CHECK ("vector", cpu.pc (), 0x0060);
	CHECK ("iff cleared", cpu.f () & Tlcs90::FLAG_I, 0);
	CHECK ("pc pushed", bus.mem[0xEFFE] | (bus.mem[0xEFFF] << 8), 0x0007);
	CHECK ("af pushed", bus.mem[0xEFFD], 0x11);
	cpu.step ();
	CHECK ("handler", cpu.a (), 0x22);
	cpu.step ();
	CHECK ("reti pc", cpu.pc (), 0x0007);
	CHECK ("reti af", cpu.a (), 0x11);
	CHECK ("reti iff", cpu.f () & Tlcs90::FLAG_I, Tlcs90::FLAG_I);
	CHECK ("reti sp", cpu.sp (), 0xF000);

	printf ("tlcs90test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
