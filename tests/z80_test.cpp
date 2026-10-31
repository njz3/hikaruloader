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

/* The Z80 core's tests: small programs whose results are known, one or more
 * per group of instructions. Run by ctest, or alone: z80test. */

#include <initializer_list>
#include <vector>

#include "cpu/z80/z80.h"

/* 64 KB of RAM, the ports echoed */
class TestBus : public Z80::Bus {
public:
	uint8_t mem[0x10000];
	uint8_t ports[0x100];
	uint8_t last_out_port, last_out_val, vector;

	TestBus () : last_out_port (0), last_out_val (0), vector (0xFF)
	{
		memset (mem, 0, sizeof (mem));
		memset (ports, 0, sizeof (ports));
	}
	uint8_t read (uint16_t addr) override { return mem[addr]; }
	void write (uint16_t addr, uint8_t val) override { mem[addr] = val; }
	uint8_t in (uint16_t port) override { return ports[port & 0xFF]; }
	void out (uint16_t port, uint8_t val) override
	{
		last_out_port = (uint8_t) port;
		last_out_val = val;
	}
	uint8_t acknowledge () override { return vector; }
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

/* Loads the program at 0, runs until it reaches HALT */
static void
run (Z80 &z, TestBus &bus, std::initializer_list<uint8_t> code, int max_steps = 10000)
{
	size_t i = 0;

	for (uint8_t b : code)
		bus.mem[i++] = b;
	z.reset ();
	z.regs.sp = 0xF000;
	while (!z.regs.halted && max_steps-- > 0)
		z.step ();
}

#define FLAGS(z_) ((z_).regs.af.b.l & ~(Z80::FLAG_X | Z80::FLAG_Y))

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	TestBus bus;
	Z80 z (&bus);

	(void) argc;
	(void) argv;

	/* Loads and stores: z = 2 is y = 2p + q */
	run (z, bus, { 0x3E, 0x5A, 0x32, 0x00, 0x90, 0x21, 0x34, 0x12, 0x22, 0x10, 0x90,
	               0x2A, 0x00, 0x90, 0x01, 0x20, 0x90, 0x02, 0x11, 0x00, 0x90, 0x1A,
	               0x76 });
	CHECK ("ld (nn),a", bus.mem[0x9000], 0x5A);
	CHECK ("ld (nn),hl", bus.mem[0x9010] | (bus.mem[0x9011] << 8), 0x1234);
	CHECK ("ld hl,(nn)", z.regs.hl.w, 0x005A);
	CHECK ("ld (bc),a", bus.mem[0x9020], 0x5A);
	CHECK ("ld a,(de)", z.regs.af.b.h, 0x5A);

	/* PUSH, POP, EX, EXX */
	run (z, bus, { 0x01, 0x22, 0x11, 0xC5, 0xE1, 0x11, 0x44, 0x33, 0xEB, 0xD9,
	               0x21, 0x99, 0x88, 0xD9, 0x76 });
	CHECK ("push bc / pop hl, ex de,hl", z.regs.de.w, 0x1122);
	CHECK ("ex de,hl", z.regs.hl.w, 0x3344);
	CHECK ("exx", z.regs.hl2.w, 0x8899);

	/* 8-bit ALU and its flags */
	run (z, bus, { 0x3E, 0x7F, 0xC6, 0x01, 0x76 });
	CHECK ("add 7f+1", z.regs.af.b.h, 0x80);
	CHECK ("add 7f+1 flags", FLAGS (z), Z80::FLAG_S | Z80::FLAG_H | Z80::FLAG_PV);
	run (z, bus, { 0x3E, 0xFF, 0xC6, 0x01, 0x76 });
	CHECK ("add ff+1 flags", FLAGS (z), Z80::FLAG_Z | Z80::FLAG_H | Z80::FLAG_C);
	run (z, bus, { 0x3E, 0x00, 0xD6, 0x01, 0x76 });
	CHECK ("sub 0-1", z.regs.af.b.h, 0xFF);
	CHECK ("sub 0-1 flags", FLAGS (z), Z80::FLAG_S | Z80::FLAG_H | Z80::FLAG_N | Z80::FLAG_C);
	run (z, bus, { 0x3E, 0x80, 0xD6, 0x01, 0x76 });
	CHECK ("sub 80-1 overflow", FLAGS (z), Z80::FLAG_H | Z80::FLAG_PV | Z80::FLAG_N);
	run (z, bus, { 0x37, 0x3E, 0x10, 0xCE, 0x05, 0x76 });
	CHECK ("adc with carry", z.regs.af.b.h, 0x16);
	run (z, bus, { 0x3E, 0x42, 0xFE, 0x42, 0x76 });
	CHECK ("cp equal", z.regs.af.b.h, 0x42);
	CHECK ("cp equal flags", FLAGS (z), Z80::FLAG_Z | Z80::FLAG_N);
	run (z, bus, { 0x3E, 0xF0, 0xE6, 0x3C, 0x76 });
	CHECK ("and", z.regs.af.b.h, 0x30);
	CHECK ("and flags", FLAGS (z), Z80::FLAG_H | Z80::FLAG_PV);
	run (z, bus, { 0x3E, 0x55, 0xAF, 0x76 });
	CHECK ("xor a flags", FLAGS (z), Z80::FLAG_Z | Z80::FLAG_PV);
	/* SCF, CCF: no carry, as INC and DEC keep it */
	run (z, bus, { 0x37, 0x3F, 0x06, 0x7F, 0x04, 0x0E, 0x01, 0x0D, 0x76 });
	CHECK ("inc 7f", z.regs.bc.b.h, 0x80);
	CHECK ("dec 1 flags", FLAGS (z), Z80::FLAG_Z | Z80::FLAG_N);
	run (z, bus, { 0x3E, 0x15, 0xC6, 0x27, 0x27, 0x76 });
	CHECK ("daa 15+27", z.regs.af.b.h, 0x42);
	run (z, bus, { 0x3E, 0x42, 0xD6, 0x15, 0x27, 0x76 });
	CHECK ("daa 42-15", z.regs.af.b.h, 0x27);
	run (z, bus, { 0x3E, 0x01, 0xED, 0x44, 0x76 });
	CHECK ("neg", z.regs.af.b.h, 0xFF);
	CHECK ("neg flags", FLAGS (z), Z80::FLAG_S | Z80::FLAG_H | Z80::FLAG_N | Z80::FLAG_C);

	/* 16-bit */
	run (z, bus, { 0x21, 0x00, 0xF0, 0x01, 0x00, 0x20, 0x09, 0x76 });
	CHECK ("add hl,bc", z.regs.hl.w, 0x1000);
	CHECK ("add hl,bc carry", z.regs.af.b.l & Z80::FLAG_C, Z80::FLAG_C);
	run (z, bus, { 0x21, 0xFF, 0x7F, 0x11, 0x01, 0x00, 0xB7, 0xED, 0x5A, 0x76 });
	CHECK ("adc hl,de", z.regs.hl.w, 0x8000);
	CHECK ("adc hl,de flags", FLAGS (z), Z80::FLAG_S | Z80::FLAG_H | Z80::FLAG_PV);
	run (z, bus, { 0x21, 0x34, 0x12, 0x37, 0xED, 0x62, 0x76 });
	CHECK ("sbc hl,hl with carry", z.regs.hl.w, 0xFFFF);
	run (z, bus, { 0x01, 0x00, 0x00, 0x0B, 0x76 });
	CHECK ("dec bc", z.regs.bc.w, 0xFFFF);

	/* Rotations, CB */
	run (z, bus, { 0x3E, 0x81, 0x07, 0x76 });
	CHECK ("rlca", z.regs.af.b.h, 0x03);
	CHECK ("rlca carry", z.regs.af.b.l & Z80::FLAG_C, Z80::FLAG_C);
	run (z, bus, { 0x37, 0x3E, 0x02, 0x1F, 0x76 });
	CHECK ("rra", z.regs.af.b.h, 0x81);
	run (z, bus, { 0x06, 0x81, 0xCB, 0x20, 0x76 });
	CHECK ("sla b", z.regs.bc.b.h, 0x02);
	run (z, bus, { 0x0E, 0x80, 0xCB, 0x29, 0x76 });
	CHECK ("sra c", z.regs.bc.b.l, 0xC0);
	run (z, bus, { 0x16, 0x01, 0xCB, 0x3A, 0x76 });
	CHECK ("srl d", z.regs.de.b.h, 0x00);
	CHECK ("srl d flags", FLAGS (z), Z80::FLAG_Z | Z80::FLAG_PV | Z80::FLAG_C);
	run (z, bus, { 0x1E, 0x40, 0xCB, 0x33, 0x76 });
	CHECK ("sll e", z.regs.de.b.l, 0x81);
	run (z, bus, { 0x21, 0x00, 0x90, 0x36, 0x80, 0xCB, 0x7E, 0x76 });
	CHECK ("bit 7,(hl)", FLAGS (z) & (Z80::FLAG_Z | Z80::FLAG_S | Z80::FLAG_H), Z80::FLAG_S | Z80::FLAG_H);
	run (z, bus, { 0x21, 0x00, 0x90, 0x36, 0x00, 0xCB, 0xC6, 0xCB, 0xFE, 0x76 });
	CHECK ("set 0 / set 7,(hl)", bus.mem[0x9000], 0x81);
	run (z, bus, { 0x3E, 0xFF, 0xCB, 0xA7, 0x76 });
	CHECK ("res 4,a", z.regs.af.b.h, 0xEF);

	/* IX, IY, DDCB */
	run (z, bus, { 0xDD, 0x21, 0x00, 0x90, 0xDD, 0x36, 0x05, 0x77, 0xDD, 0x7E, 0x05,
	               0xFD, 0x21, 0x10, 0x90, 0xFD, 0x34, 0xFF, 0x76 });
	CHECK ("ld (ix+5),n / ld a,(ix+5)", z.regs.af.b.h, 0x77);
	CHECK ("inc (iy-1)", bus.mem[0x900F], 0x01);
	run (z, bus, { 0xDD, 0x21, 0x00, 0x90, 0xDD, 0x36, 0x02, 0x01, 0xDD, 0xCB, 0x02, 0x00,
	               0x76 });
	CHECK ("rlc (ix+2),b", bus.mem[0x9002], 0x02);
	CHECK ("rlc (ix+2),b copy", z.regs.bc.b.h, 0x02);
	run (z, bus, { 0xDD, 0x26, 0x12, 0xDD, 0x2E, 0x34, 0xDD, 0x7C, 0x76 });
	CHECK ("ld ixh,n / ld a,ixh", z.regs.af.b.h, 0x12);
	CHECK ("ld ixl,n", z.regs.ix.w, 0x1234);
	run (z, bus, { 0xDD, 0x21, 0x00, 0x90, 0x21, 0x66, 0x55, 0xDD, 0x75, 0x00, 0xDD, 0x66,
	               0x00, 0x76 });
	CHECK ("ld (ix+0),l keeps l", bus.mem[0x9000], 0x66);
	CHECK ("ld h,(ix+0) keeps h", z.regs.hl.w, 0x6666);
	CHECK ("ld h,(ix+0) keeps ix", z.regs.ix.w, 0x9000);
	run (z, bus, { 0xDD, 0x21, 0xF0, 0x0F, 0x01, 0x10, 0x00, 0xDD, 0x09, 0x76 });
	CHECK ("add ix,bc", z.regs.ix.w, 0x1000);

	/* Jumps, calls */
	run (z, bus, { 0x06, 0x05, 0x3E, 0x00, 0x3C, 0x10, 0xFD, 0x76 });
	CHECK ("djnz", z.regs.af.b.h, 0x05);
	run (z, bus, { 0x3E, 0x03, 0x3D, 0x20, 0xFD, 0x76 });
	CHECK ("jr nz", z.regs.af.b.h, 0x00);
	run (z, bus, { 0xCD, 0x10, 0x00, 0x76, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	               0x3E, 0x99, 0xC9 });
	CHECK ("call / ret", z.regs.af.b.h, 0x99);
	CHECK ("call / ret sp", z.regs.sp, 0xF000);
	run (z, bus, { 0x21, 0x08, 0x00, 0xE9, 0x3E, 0x11, 0x76, 0, 0x3E, 0x22, 0x76 });
	CHECK ("jp (hl)", z.regs.af.b.h, 0x22);
	run (z, bus, { 0xAF, 0xCA, 0x07, 0x00, 0x3E, 0x01, 0x76, 0x3E, 0x02, 0x76 });
	CHECK ("jp z", z.regs.af.b.h, 0x02);

	/* Blocks */
	for (int i = 0; i < 5; i++)
		bus.mem[0x9000 + i] = (uint8_t) (0x10 + i);
	run (z, bus, { 0x21, 0x00, 0x90, 0x11, 0x00, 0xA0, 0x01, 0x05, 0x00, 0xED, 0xB0, 0x76 });
	CHECK ("ldir", bus.mem[0xA004], 0x14);
	CHECK ("ldir bc", z.regs.bc.w, 0);
	CHECK ("ldir pv", z.regs.af.b.l & Z80::FLAG_PV, 0);
	run (z, bus, { 0x21, 0x00, 0x90, 0x01, 0x05, 0x00, 0x3E, 0x12, 0xED, 0xB1, 0x76 });
	CHECK ("cpir hl", z.regs.hl.w, 0x9003);
	CHECK ("cpir found", z.regs.af.b.l & Z80::FLAG_Z, Z80::FLAG_Z);

	/* ED */
	run (z, bus, { 0x3E, 0x84, 0x21, 0x00, 0x90, 0x36, 0x20, 0xED, 0x6F, 0x76 });
	CHECK ("rld (hl)", bus.mem[0x9000], 0x04);
	CHECK ("rld a", z.regs.af.b.h, 0x82);
	run (z, bus, { 0x3E, 0x47, 0xED, 0x47, 0x3E, 0x00, 0xED, 0x57, 0x76 });
	CHECK ("ld i,a / ld a,i", z.regs.af.b.h, 0x47);

	/* Ports */
	bus.ports[0x15] = 0x60;
	run (z, bus, { 0x3E, 0x5A, 0xD3, 0x10, 0xDB, 0x15, 0x76 });
	CHECK ("out (n),a", bus.last_out_val, 0x5A);
	CHECK ("out (n),a port", bus.last_out_port, 0x10);
	CHECK ("in a,(n)", z.regs.af.b.h, 0x60);
	run (z, bus, { 0x01, 0x15, 0x00, 0xED, 0x78, 0x76 });
	CHECK ("in a,(c)", z.regs.af.b.h, 0x60);

	/* Interrupts: mode 2 through the vector, mode 1 at 38 */
	bus.mem[0x89F4] = 0x00;
	bus.mem[0x89F5] = 0x01;		/* the vector F4: 0100 */
	bus.mem[0x0100] = 0x3E;		/* LD A,77h; HALT */
	bus.mem[0x0101] = 0x77;
	bus.mem[0x0102] = 0x76;
	bus.vector = 0xF4;
	run (z, bus, { 0x3E, 0x89, 0xED, 0x47, 0xED, 0x5E, 0xFB, 0x00, 0x76 }, 20);
	CHECK ("halted before the interrupt", z.regs.halted, 1);
	z.set_irq (true);
	z.step ();
	z.set_irq (false);
	CHECK ("im 2 vector", z.regs.pc, 0x0100);
	CHECK ("im 2 return address", bus.mem[z.regs.sp] | (bus.mem[z.regs.sp + 1] << 8), 0x0009);
	CHECK ("im 2 iff1", z.regs.iff1, 0);
	z.step ();
	CHECK ("im 2 handler ran", z.regs.af.b.h, 0x77);

	bus.mem[0x0038] = 0x3E;		/* LD A,38h; HALT */
	bus.mem[0x0039] = 0x38;
	bus.mem[0x003A] = 0x76;
	run (z, bus, { 0xED, 0x56, 0xFB, 0x76 }, 20);
	z.set_irq (true);
	z.step ();
	z.set_irq (false);
	z.step ();
	CHECK ("im 1 at 38", z.regs.af.b.h, 0x38);

	/* EI waits one instruction */
	run (z, bus, { 0xED, 0x56, 0x76 }, 20);
	z.regs.halted = false;
	bus.mem[z.regs.pc] = 0xFB;
	bus.mem[z.regs.pc + 1] = 0x00;
	z.set_irq (true);
	z.step ();
	CHECK ("ei delay", z.regs.pc, 0x0004);
	z.step ();
	z.step ();
	z.set_irq (false);
	CHECK ("taken after ei and one more", z.regs.pc, 0x0038);

	printf ("z80test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
