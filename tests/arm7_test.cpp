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

/* The ARM7 core's tests: small programs whose results are known, one or
 * more per group of instructions. Run by ctest, or alone: arm7test.
 * arm7test --bench measures the interpreter's speed instead (--bench --jit:
 * the translations'). */

#include <chrono>
#include <initializer_list>

#include "cpu/arm/arm7.h"

/* 64 KB of RAM, little-endian, mirrored */
class TestBus : public Arm7::Bus {
public:
	uint8_t mem[0x10000];

	TestBus () { memset (mem, 0, sizeof (mem)); }

	uint32_t read32 (uint32_t a) override
	{
		a &= 0xFFFC;
		return mem[a] | (mem[a + 1] << 8) | (mem[a + 2] << 16) | ((uint32_t) mem[a + 3] << 24);
	}
	uint16_t read16 (uint32_t a) override
	{
		a &= 0xFFFE;
		return (uint16_t) (mem[a] | (mem[a + 1] << 8));
	}
	uint8_t read8 (uint32_t a) override { return mem[a & 0xFFFF]; }
	void write32 (uint32_t a, uint32_t v) override
	{
		a &= 0xFFFC;
		mem[a] = (uint8_t) v;
		mem[a + 1] = (uint8_t) (v >> 8);
		mem[a + 2] = (uint8_t) (v >> 16);
		mem[a + 3] = (uint8_t) (v >> 24);
	}
	void write16 (uint32_t a, uint16_t v) override
	{
		a &= 0xFFFE;
		mem[a] = (uint8_t) v;
		mem[a + 1] = (uint8_t) (v >> 8);
	}
	void write8 (uint32_t a, uint8_t v) override { mem[a & 0xFFFF] = v; }
};

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		uint32_t g_ = (uint32_t) (got_), w_ = (uint32_t) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %X, expected %X\n", name_, #got_, g_, w_); \
		} \
	} while (0)

/* The encoders, condition AL */
enum { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };
enum { LSL, LSR, ASR, ROR };

/* An immediate operand: v rotated right by 2 * rot */
static uint32_t imm (unsigned rot, uint32_t v) { return (1u << 25) | (rot << 8) | v; }
/* rm shifted by an immediate (0: LSR #32, ASR #32, RRX) */
static uint32_t shi (unsigned rm, unsigned type, unsigned n) { return (n << 7) | (type << 5) | rm; }
/* rm shifted by rs */
static uint32_t shr (unsigned rm, unsigned type, unsigned rs) { return (rs << 8) | (type << 5) | 0x10 | rm; }

static uint32_t
dp (unsigned op, bool s, unsigned rd, unsigned rn, uint32_t op2)
{
	return 0xE0000000 | (op << 21) | (s ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | op2;
}

/* LDR/STR rd, [rn, #off]: pre/post, writeback, byte */
static uint32_t
ldst (bool load, bool byte, bool pre, bool wback, unsigned rd, unsigned rn, int off)
{
	return 0xE4000000 | (pre ? 1u << 24 : 0) | (off >= 0 ? 1u << 23 : 0) |
	       (byte ? 1u << 22 : 0) | (wback ? 1u << 21 : 0) | (load ? 1u << 20 : 0) |
	       (rn << 16) | (rd << 12) | (uint32_t) (off >= 0 ? off : -off);
}

/* The halfword forms, pre-indexed with an immediate: sh 1 H, 2 SB, 3 SH */
static uint32_t
ldsth (bool load, unsigned sh, unsigned rd, unsigned rn, unsigned off)
{
	return 0xE1C00090 | (1u << 23) | (load ? 1u << 20 : 0) | (rn << 16) | (rd << 12) |
	       ((off & 0xF0) << 4) | (sh << 5) | (off & 15);
}

/* LDM/STM rn, {list}: pu 0 DA, 1 IA, 2 DB, 3 IB */
static uint32_t
ldstm (bool load, unsigned pu, bool s, bool wback, unsigned rn, uint32_t list)
{
	return 0xE8000000 | (pu << 23) | (s ? 1u << 22 : 0) | (wback ? 1u << 21 : 0) |
	       (load ? 1u << 20 : 0) | (rn << 16) | list;
}

/* MSR CPSR_c, #v and MRS rd, CPSR */
static uint32_t msr_c (uint32_t v) { return 0xE321F000 | v; }
static uint32_t mrs (unsigned rd, bool spsr) { return 0xE10F0000 | (spsr ? 1u << 22 : 0) | (rd << 12); }

static const uint32_t B_SELF = 0xEAFFFFFE;	/* B . */
static const uint32_t MOVS_PC_LR = 0xE1B0F00E;
static const uint32_t SUBS_PC_LR_4 = 0xE25EF004;

/* Loads the program at 0x100 followed by B ., starts in SVC with the
 * interrupts off, runs until the B . */
static uint32_t
load (Arm7 &cpu, TestBus &bus, std::initializer_list<uint32_t> code)
{
	uint32_t addr = 0x100;

	for (uint32_t w : code) {
		bus.write32 (addr, w);
		addr += 4;
	}
	bus.write32 (addr, B_SELF);
	cpu.reset ();
	cpu.regs.r[15] = 0x100;
	return addr;
}

static void
run (Arm7 &cpu, TestBus &bus, std::initializer_list<uint32_t> code, int max_steps = 1000)
{
	uint32_t end = load (cpu, bus, code);

	while (cpu.regs.r[15] != end && max_steps-- > 0)
		cpu.step ();
}

#define FLAGS(cpu_) ((cpu_).regs.cpsr >> 28)	/* NZCV */

/* fast_loops: a sound driver's scan loop over 64 slots of 0x40 bytes, the
 * flag at slot (none past 63), run budget cycles at a time with and without
 * it: the same registers, flags and cycles after each run () */
static void
test_scan_loop (unsigned slot, int budget)
{
	static const uint32_t code[] = {
		0xE3A0AA02,	/* 100: MOV r10, #0x2000 */
		0xE3A09000,	/* 104: MOV r9, #0 */
		0xE5DA0000,	/* 108: LDRB r0, [r10] */
		0xE3100080,	/*      TST r0, #0x80 */
		0x1A000004,	/*      BNE 128 */
		0xE28AA040,	/*      ADD r10, r10, #0x40 */
		0xE2899001,	/*      ADD r9, r9, #1 */
		0xE3590040,	/*      CMP r9, #0x40 */
		0x1AFFFFF8,	/*      BNE 108 */
		B_SELF,		/* 124: not found */
		B_SELF		/* 128: found */
	};
	TestBus slow_bus, fast_bus;
	Arm7 slow (&slow_bus), fast (&fast_bus);
	char name[64];
	unsigned i, same = 1, calls;

	for (TestBus *b : { &slow_bus, &fast_bus }) {
		for (i = 0; i < NUMELEM (code); i++)
			b->write32 (0x100 + 4 * i, code[i]);
		for (i = 0; i < 64; i++)
			b->mem[0x2000 + i * 0x40] = (uint8_t) (i == slot ? 0x81 : 0x05);
	}
	for (Arm7 *c : { &slow, &fast }) {
		c->reset ();
		c->regs.r[15] = 0x100;
	}
	slow.set_ram (slow_bus.mem, sizeof (slow_bus.mem));
	fast.set_ram (fast_bus.mem, sizeof (fast_bus.mem));
	fast.fast_loops = true;
	/* Enough for the 64 turns, 11 cycles each, whatever the budget */
	for (calls = 0; calls < 1000u / (unsigned) budget + 20; calls++) {
		int a = slow.run (budget), b = fast.run (budget);
		if (a != b || memcmp (slow.regs.r, fast.regs.r, sizeof (slow.regs.r)) ||
		    slow.regs.cpsr != fast.regs.cpsr || slow.regs.cycles != fast.regs.cycles)
			same = 0;
	}
	snprintf (name, sizeof (name), "scan loop, flag at %u, %d cycles a run", slot, budget);
	CHECK (name, same, 1);
	snprintf (name, sizeof (name), "scan loop, flag at %u, the end", slot);
	CHECK (name, fast.regs.r[15], slot < 64 ? 0x128u : 0x124u);
}

/* The translations (Arm7::jit) against step (): random programs of the
 * kinds the sound drivers run, on two CPUs run the same budgets at a time
 * (1 to 100 cycles), an FIQ now and then, an instruction rewritten between
 * two runs now and then (as the SH-4 loads code): the same registers,
 * cycles and memory after each run (). The RAM below 8000 (set_ram ()), the
 * bus above */
static uint32_t rng_state;

static uint32_t
rnd (void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

static uint32_t rnd32 (void) { return (rnd () << 16) ^ rnd (); }

/* An instruction for index i of n (the loop's tail at n): r0-r8 data, r9 a
 * small offset, r10 the loop's count, r11 a base in the RAM, r12 on the
 * bus, r13 the stack; those three written back alone */
static uint32_t
random_inst (unsigned i, unsigned n)
{
	const uint32_t cond = (rnd () % 10 < 7) ? 0xE0000000 : ((rnd () % 15) << 28);
	const unsigned k = rnd () % 100;

	if (k < 40) {
		unsigned op = rnd () % 16, rd = rnd () % 9, rn = rnd () % 16;
		bool s = (op >= TST && op <= CMN) || rnd () % 2;
		uint32_t op2;

		switch (rnd () % 4) {
		case 0: op2 = imm (rnd () % 16, rnd () % 256); break;
		case 1: op2 = shr (rnd () % 16, rnd () % 4, rnd () % 13); break;
		default: op2 = shi (rnd () % 16, rnd () % 4, rnd () % 32); break;
		}
		return (dp (op, s, rd, rn, op2) & 0x0FFFFFFF) | cond;
	}
	if (k < 58) {
		bool load = rnd () % 2, byte = rnd () % 3 == 0, pre = rnd () % 4 != 0;
		bool wback = rnd () % 4 == 0, up = rnd () % 2;
		unsigned rn = 11 + rnd () % 3, rd = load ? rnd () % 9 : rnd () % 16;
		uint32_t w = cond | 0x04000000 | (pre ? 1u << 24 : 0) | (up ? 1u << 23 : 0) |
		             (byte ? 1u << 22 : 0) | (wback ? 1u << 21 : 0) | (load ? 1u << 20 : 0) |
		             (rn << 16) | (rd << 12);

		if (rnd () % 4 == 0)	/* r9, LSL/LSR #0-2, not written back */
			return (w & ~(1u << 21)) | (1u << 24) | (1u << 25) | ((rnd () % 3) << 7) |
			       ((rnd () % 2) << 5) | 9;
		return w | (rnd () % 0x40);
	}
	if (k < 68) {
		bool load = rnd () % 3 != 0, pre = rnd () % 4 != 0, wback = rnd () % 4 == 0;
		bool up = rnd () % 2, reg = rnd () % 4 == 0;
		unsigned rn = 11 + rnd () % 3, rd = load ? rnd () % 9 : rnd () % 16;
		unsigned sh = load ? 1 + rnd () % 3 : 1, off = rnd () % 0x40;
		uint32_t w = cond | (pre ? 1u << 24 : 0) | (up ? 1u << 23 : 0) | (wback ? 1u << 21 : 0) |
		             (load ? 1u << 20 : 0) | (rn << 16) | (rd << 12) | 0x90 | (sh << 5);

		if (reg)	/* r9, not written back */
			return (w & ~(1u << 21)) | (1u << 24) | 9;
		return w | (1u << 22) | ((off & 0xF0) << 4) | (off & 15);
	}
	if (k < 75) {
		bool a = rnd () % 2, s = rnd () % 2;
		return cond | (a ? 1u << 21 : 0) | (s ? 1u << 20 : 0) | ((rnd () % 9) << 16) |
		       ((rnd () % 16) << 12) | ((rnd () % 16) << 8) | 0x90 | (rnd () % 16);
	}
	if (k < 78) {
		unsigned lo = rnd () % 9, hi = (lo + 1 + rnd () % 8) % 9;
		return cond | 0x00800090 | ((rnd () % 4) << 21) | ((rnd () % 2) << 20) | (hi << 16) |
		       (lo << 12) | ((rnd () % 16) << 8) | (rnd () % 16);
	}
	if (k < 83) {
		/* STMDB r13!, LDMIA r13!; an LDM from r11 */
		uint32_t list = (rnd () & 0x1FF) | 1;
		switch (rnd () % 3) {
		case 0: return cond | (ldstm (false, 2, false, true, 13, list) & 0x0FFFFFFF);
		case 1: return cond | (ldstm (true, 1, false, true, 13, list) & 0x0FFFFFFF);
		default: return cond | (ldstm (true, rnd () % 4, false, false, 11, list) & 0x0FFFFFFF);
		}
	}
	if (k < 86)
		return (mrs (rnd () % 9, rnd () % 2) & 0x0FFFFFFF) | cond;
	if (k < 88)
		return cond | 0x0328F000 | ((rnd () % 16) << 8) | (rnd () % 256);	/* MSR CPSR_f, # */
	if (k < 89)
		return cond | 0x01000090 | ((rnd () % 2) << 22) | (11u << 16) | ((rnd () % 9) << 12) |
		       (rnd () % 16);						/* SWP r11 */
	if (k < 90)
		return cond | 0x0F000000;					/* SWI */
	{
		/* Forward, within the program or to its tail */
		unsigned to = i + 1 + rnd () % 4;
		if (to > n)
			to = n;
		return (cond == 0xE0000000 ? 0x10000000 : cond) | 0x0A000000 |
		       ((uint32_t) ((int32_t) (to - i) - 2) & 0xFFFFFF);
	}
}

static void
test_jit_random (unsigned seed)
{
	static TestBus bus[2];
	Arm7 slow (&bus[0]), fast (&bus[1]);
	Arm7 *cpus[2] = { &slow, &fast };
	uint32_t code[200], regs[16], cpsr;
	unsigned i, calls, n;
	uint64_t total = 0;
	char name[96];
	int same = 1;

	rng_state = seed;
	n = 20 + rnd () % 150;
	for (i = 0; i < n; i++)
		code[i] = random_inst (i, n);
	for (i = 0; i < 9; i++)
		regs[i] = rnd32 ();
	regs[9] = rnd () % 256;
	regs[10] = 1 + rnd () % 40;
	regs[11] = 0x6000 + rnd () % 0x400;
	regs[12] = 0x9000 + rnd () % 0x400;
	regs[13] = 0x7000;
	regs[14] = rnd32 ();
	regs[15] = 0x100;
	cpsr = Arm7::MODE_SYS | (rnd32 () & 0xF0000000);
	for (int c = 0; c < 2; c++) {
		Arm7 &cpu = *cpus[c];

		memset (bus[c].mem, 0, sizeof (bus[c].mem));
		bus[c].write32 (0x08, MOVS_PC_LR);				/* SWI */
		bus[c].write32 (0x1C, dp (ADD, false, 8, 8, imm (0, 1)));	/* FIQ */
		bus[c].write32 (0x20, SUBS_PC_LR_4);
		for (i = 0; i < n; i++)
			bus[c].write32 (0x100 + 4 * i, code[i]);
		bus[c].write32 (0x100 + 4 * n, dp (SUB, true, 10, 10, imm (0, 1)));
		bus[c].write32 (0x104 + 4 * n, 0x1A000000 | (((uint32_t) -(int32_t) (n + 3)) & 0xFFFFFF));
		bus[c].write32 (0x108 + 4 * n, B_SELF);
		for (i = 0x6000; i < 0x7400; i++)
			bus[c].mem[i] = (uint8_t) (i * 7);
		cpu.set_ram (bus[c].mem, 0x8000);
		cpu.reset ();
		memcpy (cpu.regs.r, regs, sizeof (regs));
		cpu.regs.cpsr = cpsr;
		cpu.fast_loops = true;
	}
	fast.jit = true;
	for (calls = 0; calls < 600 && total < 40000 && same; calls++) {
		const int budget = 1 + (int) (rnd () % 100);
		const bool fiq = rnd () % 40 == 0;
		int done[2];

		if (rnd () % 30 == 0) {
			/* Code rewritten between two runs */
			const unsigned at = rnd () % n;
			const uint32_t w = random_inst (at, n);

			bus[0].write32 (0x100 + 4 * at, w);
			bus[1].write32 (0x100 + 4 * at, w);
		}
		for (int c = 0; c < 2; c++) {
			cpus[c]->set_fiq (fiq);
			done[c] = cpus[c]->run (budget);
			cpus[c]->set_fiq (false);
		}
		total += (uint64_t) done[0];
		if (done[0] != done[1] || memcmp (&slow.regs, &fast.regs, sizeof (slow.regs)) ||
		    memcmp (bus[0].mem, bus[1].mem, sizeof (bus[0].mem))) {
			same = 0;
			fprintf (stderr, "jit seed %u, run %u: %d/%d cycles, pc %08X/%08X, cpsr %08X/%08X\n",
			         seed, calls, done[0], done[1], slow.regs.r[15], fast.regs.r[15],
			         slow.regs.cpsr, fast.regs.cpsr);
			for (i = 0; i < 16; i++)
				if (slow.regs.r[i] != fast.regs.r[i])
					fprintf (stderr, "  r%u %08X/%08X\n", i, slow.regs.r[i], fast.regs.r[i]);
			for (i = 0; i < sizeof (bus[0].mem); i++)
				if (bus[0].mem[i] != bus[1].mem[i]) {
					fprintf (stderr, "  mem %04X %02X/%02X\n", i, bus[0].mem[i], bus[1].mem[i]);
					break;
				}
		}
	}
	snprintf (name, sizeof (name), "jit against step (), seed %u", seed);
	CHECK (name, same, 1);
}

/* A store into the block's own next instruction: run as written */
static void
test_jit_self_modifying (void)
{
	static TestBus bus[2];
	Arm7 slow (&bus[0]), fast (&bus[1]);

	for (int c = 0; c < 2; c++) {
		Arm7 &cpu = c ? fast : slow;

		load (cpu, bus[c], {
			dp (MOV, false, 0, 0, imm (0, 5)),	/* 100 */
			0xE59F1008,				/* 104: LDR r1, [pc, #8] (114) */
			0xE58F1000,				/* 108: STR r1, [pc, #0] (110) */
			dp (ADD, false, 0, 0, imm (0, 1)),	/* 10C */
			dp (ADD, false, 0, 0, imm (0, 2)),	/* 110: becomes 114's */
			dp (ADD, false, 0, 0, imm (0, 0x10)),	/* 114 */
		});
		cpu.set_ram (bus[c].mem, 0x8000);
	}
	fast.jit = true;
	slow.run (60);
	fast.run (60);
	CHECK ("self-modifying, step ()", slow.regs.r[0], 0x26);
	CHECK ("self-modifying, jit", fast.regs.r[0], 0x26);
	CHECK ("self-modifying, cycles", fast.regs.cycles, slow.regs.cycles);
}

/* --bench: the interpreter's speed on a loop of a sound driver's kind (a
 * load, a store, ALU operations, a shift, a condition, a branch: 9
 * instructions, 14 cycles), run as an AICA runs it: 64 cycles a sample,
 * 44100 samples a second, the RAM through set_ram () */
static int
bench (bool jit)
{
	static const uint32_t loop[] = {
		0xE3A02A01,	/* 100: MOV r2, #0x1000 */
		0xE2800001,	/* 104: ADD r0, r0, #1 */
		0xE5921000,	/*      LDR r1, [r2] */
		0xE2811003,	/*      ADD r1, r1, #3 */
		0xE5821004,	/*      STR r1, [r2, #4] */
		0xE20030FF,	/*      AND r3, r0, #0xFF */
		0xE3530000,	/*      CMP r3, #0 */
		0x02844001,	/*      ADDEQ r4, r4, #1 */
		0xE1A05125,	/*      MOV r5, r5, LSR #2 */
		0xEAFFFFF6	/*      B 104 */
	};
	const long seconds = 3;
	TestBus bus;
	Arm7 cpu (&bus);
	unsigned i;
	int owed = 0;

	cpu.set_ram (bus.mem, sizeof (bus.mem));
	cpu.jit = jit;
	for (i = 0; i < NUMELEM (loop); i++)
		bus.write32 (0x100 + 4 * i, loop[i]);
	cpu.reset ();
	cpu.regs.r[15] = 0x100;

	auto start = std::chrono::steady_clock::now ();
	for (long s = 0; s < 44100 * seconds; s++) {
		owed += 64;
		owed -= cpu.run (owed);
	}
	double host = std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
	double insts = (double) cpu.regs.r[0] * 9;

	printf ("arm7 bench: %.0f instructions in %.3f s, %.1f ns each (%.1f million a second);\n"
	        "  one AICA's ARM: %.1f ms of the host per second of sound, the two %.1f%% of a core\n",
	        insts, host, host * 1e9 / insts, insts / host / 1e6,
	        host / seconds * 1000.0, 2.0 * host / seconds * 100.0);
	return 0;
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	TestBus bus;
	Arm7 cpu (&bus);

	if (argc > 1 && !strcmp (argv[1], "--bench"))
		return bench (argc > 2 && !strcmp (argv[2], "--jit"));

	/* ADD, SUB and their flags */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 1)),
		dp (MVN, false, 2, 0, imm (0, 0)),		/* r2 = FFFFFFFF */
		dp (ADD, true, 1, 0, 2),			/* 1 + FFFFFFFF */
	});
	CHECK ("adds result", cpu.regs.r[1], 0);
	CHECK ("adds flags", FLAGS (cpu), 0x6);		/* Z C */

	run (cpu, bus, {
		dp (MVN, false, 0, 0, imm (1, 2)),		/* r0 = 7FFFFFFF */
		dp (ADD, true, 1, 0, imm (0, 1)),
	});
	CHECK ("adds overflow", cpu.regs.r[1], 0x80000000);
	CHECK ("adds overflow flags", FLAGS (cpu), 0x9);	/* N V */

	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 5)),
		dp (SUB, true, 1, 0, imm (0, 7)),
	});
	CHECK ("subs result", cpu.regs.r[1], 0xFFFFFFFE);
	CHECK ("subs borrow flags", FLAGS (cpu), 0x8);	/* N, C clear: a borrow */

	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 5)),
		dp (CMP, true, 0, 0, 0),			/* C set */
		dp (ADC, false, 1, 0, imm (0, 1)),
		dp (SBC, false, 2, 0, imm (0, 1)),
		dp (ADD, true, 4, 0, imm (0, 0)),		/* C clear */
		dp (ADC, false, 5, 0, imm (0, 1)),
		dp (SBC, false, 6, 0, imm (0, 1)),
		dp (RSB, false, 7, 0, imm (0, 8)),
	});
	CHECK ("adc with carry", cpu.regs.r[1], 7);
	CHECK ("sbc with carry", cpu.regs.r[2], 4);
	CHECK ("adc without carry", cpu.regs.r[5], 6);
	CHECK ("sbc without carry", cpu.regs.r[6], 3);
	CHECK ("rsb", cpu.regs.r[7], 3);

	/* The logical ops keep V, set C from the shifter */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 0xF0)),
		dp (AND, false, 1, 0, imm (0, 0x3C)),
		dp (EOR, false, 2, 0, imm (0, 0xFF)),
		dp (ORR, false, 3, 0, imm (0, 0x0F)),
		dp (BIC, false, 4, 0, imm (0, 0x30)),
		dp (TST, true, 0, 0, imm (0, 0x0F)),		/* Z */
	});
	CHECK ("and", cpu.regs.r[1], 0x30);
	CHECK ("eor", cpu.regs.r[2], 0x0F);
	CHECK ("orr", cpu.regs.r[3], 0xFF);
	CHECK ("bic", cpu.regs.r[4], 0xC0);
	CHECK ("tst flags", FLAGS (cpu) & 0x4, 0x4);

	/* The shifter: immediate forms */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 5)),
		dp (MOV, false, 1, 0, shi (0, LSL, 4)),
		dp (MOV, false, 2, 0, imm (1, 2)),		/* 80000000 */
		dp (MOV, true, 3, 0, shi (2, LSR, 0)),	/* LSR #32 */
	});
	CHECK ("lsl #4", cpu.regs.r[1], 0x50);
	CHECK ("lsr #32", cpu.regs.r[3], 0);
	CHECK ("lsr #32 carry", FLAGS (cpu), 0x6);	/* Z C */

	run (cpu, bus, {
		dp (MOV, false, 2, 0, imm (1, 2)),		/* 80000000 */
		dp (MOV, true, 3, 0, shi (2, ASR, 0)),	/* ASR #32 */
		dp (MOV, false, 4, 0, imm (0, 2)),
		dp (MOV, true, 5, 0, shi (4, ROR, 0)),	/* RRX, C was set */
	});
	CHECK ("asr #32", cpu.regs.r[3], 0xFFFFFFFF);
	CHECK ("rrx", cpu.regs.r[5], 0x80000001);
	CHECK ("rrx carry", FLAGS (cpu) & 0x2, 0);

	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (4, 0x12)),	/* 12000000 */
		dp (ORR, false, 0, 0, imm (8, 0x34)),	/* 00340000 */
		dp (ORR, false, 0, 0, imm (12, 0x56)),	/* 00005600 */
		dp (ORR, false, 0, 0, imm (0, 0x78)),	/* 12345678 */
		dp (MOV, false, 1, 0, shi (0, ROR, 8)),
		dp (MOV, false, 2, 0, shi (0, ASR, 4)),
		dp (MOV, false, 3, 0, shi (0, LSR, 28)),
	});
	CHECK ("imm build", cpu.regs.r[0], 0x12345678);
	CHECK ("ror #8", cpu.regs.r[1], 0x78123456);
	CHECK ("asr #4", cpu.regs.r[2], 0x01234567);
	CHECK ("lsr #28", cpu.regs.r[3], 1);

	/* The shifter by a register: 0, 32 and more */
	run (cpu, bus, {
		dp (MVN, false, 0, 0, imm (0, 0)),		/* FFFFFFFF */
		dp (MOV, false, 1, 0, imm (0, 32)),
		dp (MOV, true, 2, 0, shr (0, LSL, 1)),	/* LSL by 32 */
	});
	CHECK ("lsl by 32", cpu.regs.r[2], 0);
	CHECK ("lsl by 32 carry", FLAGS (cpu), 0x6);	/* Z, C = bit 0 */

	run (cpu, bus, {
		dp (MVN, false, 0, 0, imm (0, 0)),
		dp (MOV, false, 1, 0, imm (0, 40)),
		dp (MOV, true, 2, 0, shr (0, LSR, 1)),	/* LSR by 40 */
		dp (MOV, false, 3, 0, imm (0, 4)),
		dp (MOV, false, 4, 0, shr (0, ASR, 3)),
		dp (MOV, false, 5, 0, imm (0, 0)),
		dp (MOV, false, 6, 0, shr (3, LSL, 5)),	/* by 0: unchanged */
	});
	CHECK ("lsr by 40", cpu.regs.r[2], 0);
	CHECK ("lsr by 40 carry", FLAGS (cpu), 0x4);	/* Z, C clear */
	CHECK ("asr by 4", cpu.regs.r[4], 0xFFFFFFFF);
	CHECK ("lsl by 0", cpu.regs.r[6], 4);

	/* r15 as an operand: 8 ahead, 12 with a register shift */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, 15),			/* at 100 */
		dp (MOV, false, 1, 0, imm (0, 0)),
		dp (ADD, false, 2, 15, shr (1, LSL, 1)),	/* at 108 */
	});
	CHECK ("pc read", cpu.regs.r[0], 0x108);
	CHECK ("pc read, register shift", cpu.regs.r[2], 0x114);

	/* LDR and STR */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),		/* 1000 */
		dp (MOV, false, 1, 0, imm (0, 0xAB)),
		ldst (false, false, true, true, 1, 0, 4),	/* STR r1, [r0, #4]! */
		ldst (true, false, false, false, 2, 0, -4),	/* LDR r2, [r0], #-4 */
		ldst (true, false, true, false, 3, 0, 9),	/* LDR r3, [r0, #9] */
		ldst (true, true, true, false, 4, 0, 9),	/* LDRB r4, [r0, #9] */
		ldst (false, true, true, false, 1, 0, 0x10),	/* STRB r1, [r0, #16] */
	});
	CHECK ("str writeback", cpu.regs.r[0], 0x1000);
	CHECK ("str stored", bus.read32 (0x1004), 0xAB);
	CHECK ("ldr post-indexed", cpu.regs.r[2], 0xAB);
	CHECK ("strb", bus.read32 (0x1010), 0xAB);

	bus.write32 (0x1008, 0x11223344);
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),
		ldst (true, false, true, false, 3, 0, 9),	/* LDR r3, [r0, #9] */
		ldst (true, true, true, false, 4, 0, 9),	/* LDRB r4, [r0, #9] */
		0xE7905101,					/* LDR r5, [r0, r1, LSL #2] */
	});
	CHECK ("ldr unaligned", cpu.regs.r[3], 0x44112233);
	CHECK ("ldrb", cpu.regs.r[4], 0x33);

	bus.write32 (0x1008, 0x11223344);
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),
		dp (MOV, false, 1, 0, imm (0, 2)),
		0xE7905101,					/* LDR r5, [r0, r1, LSL #2] */
	});
	CHECK ("ldr register offset", cpu.regs.r[5], 0x11223344);

	/* The halfword transfers */
	bus.write32 (0x1000, 0x8001F0FE);
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),
		ldsth (true, 1, 1, 0, 2),			/* LDRH r1, [r0, #2] */
		ldsth (true, 2, 2, 0, 0),			/* LDRSB r2, [r0] */
		ldsth (true, 3, 3, 0, 2),			/* LDRSH r3, [r0, #2] */
		dp (MOV, false, 4, 0, imm (0, 0x55)),
		ldsth (false, 1, 4, 0, 0x22),		/* STRH r4, [r0, #0x22] */
	});
	CHECK ("ldrh", cpu.regs.r[1], 0x8001);
	CHECK ("ldrsb", cpu.regs.r[2], 0xFFFFFFFE);
	CHECK ("ldrsh", cpu.regs.r[3], 0xFFFF8001);
	CHECK ("strh", bus.read32 (0x1020), 0x00550000);

	/* LDM, STM: the four ways */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),		/* 1000 */
		dp (MOV, false, 1, 0, imm (0, 1)),
		dp (MOV, false, 2, 0, imm (0, 2)),
		dp (MOV, false, 3, 0, imm (0, 3)),
		ldstm (false, 1, false, true, 0, 0x000E),	/* STMIA r0!, {r1-r3} */
		ldstm (true, 2, false, false, 0, 0x0070),	/* LDMDB r0, {r4-r6} */
		dp (MOV, false, 7, 0, imm (10, 2)),		/* 2000 */
		ldstm (false, 2, false, true, 7, 0x000E),	/* STMDB r7!, {r1-r3} */
		ldstm (true, 1, false, false, 7, 0x0700),	/* LDMIA r7, {r8-r10} */
		ldstm (false, 3, false, false, 7, 0x0002),	/* STMIB r7, {r1} */
		ldstm (false, 0, false, false, 7, 0x0004),	/* STMDA r7, {r2} */
	});
	CHECK ("stmia writeback", cpu.regs.r[0], 0x100C);
	CHECK ("stmia", bus.read32 (0x1008), 3);
	CHECK ("ldmdb", cpu.regs.r[4] + cpu.regs.r[5] * 16 + cpu.regs.r[6] * 256, 0x321);
	CHECK ("stmdb writeback", cpu.regs.r[7], 0x1FF4);
	CHECK ("ldmia", cpu.regs.r[8] + cpu.regs.r[9] * 16 + cpu.regs.r[10] * 256, 0x321);
	CHECK ("stmib", bus.read32 (0x1FF8), 1);
	CHECK ("stmda", bus.read32 (0x1FF4), 2);

	/* B, BL and a return */
	run (cpu, bus, {
		0xEB000001,					/* 100: BL 10C */
		dp (MOV, false, 1, 0, imm (0, 1)),		/* 104 */
		0xEA000001,					/* 108: B 114 */
		dp (MOV, false, 2, 0, imm (0, 2)),		/* 10C */
		dp (MOV, false, 15, 0, 14),			/* 110: MOV PC, LR */
		dp (MOV, false, 3, 0, imm (0, 3)),		/* 114 */
	});
	CHECK ("bl link", cpu.regs.r[14], 0x104);
	CHECK ("bl target ran", cpu.regs.r[2], 2);
	CHECK ("returned", cpu.regs.r[1], 1);
	CHECK ("b", cpu.regs.r[3], 3);

	/* The multiplies */
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (0, 7)),
		dp (MOV, false, 1, 0, imm (0, 6)),
		dp (MOV, false, 2, 0, imm (0, 100)),
		0xE0030190,					/* MUL r3, r0, r1 */
		0xE0242190,					/* MLA r4, r0, r1, r2 */
		dp (MVN, false, 5, 0, imm (0, 0)),		/* -1 */
		dp (MOV, false, 6, 0, imm (0, 2)),
		0xE0887695,					/* UMULL r7, r8, r5, r6 */
		dp (MOV, false, 9, 0, imm (0, 10)),
		dp (MOV, false, 10, 0, imm (0, 0)),
		0xE0EA9695,					/* SMLAL r9, r10, r5, r6 */
	});
	CHECK ("mul", cpu.regs.r[3], 42);
	CHECK ("mla", cpu.regs.r[4], 142);
	CHECK ("umull lo", cpu.regs.r[7], 0xFFFFFFFE);
	CHECK ("umull hi", cpu.regs.r[8], 1);
	CHECK ("smlal lo", cpu.regs.r[9], 8);
	CHECK ("smlal hi", cpu.regs.r[10], 0);

	/* SWP */
	bus.write32 (0x1000, 0x12345678);
	run (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 1)),
		dp (MOV, false, 1, 0, imm (0, 0x99)),
		0xE1002091,					/* SWP r2, r1, [r0] */
	});
	CHECK ("swp loaded", cpu.regs.r[2], 0x12345678);
	CHECK ("swp stored", bus.read32 (0x1000), 0x99);

	/* MRS, MSR; the user mode cannot leave itself */
	run (cpu, bus, {
		mrs (0, false),
		msr_c (Arm7::MODE_USR),
		msr_c (Arm7::MODE_SVC),
		mrs (1, false),
	});
	CHECK ("mrs svc", cpu.regs.r[0] & 0xFF, Arm7::MODE_SVC | 0xC0);
	CHECK ("user stays user", cpu.regs.r[1] & 0x1F, Arm7::MODE_USR);

	/* The banked registers */
	run (cpu, bus, {
		dp (MOV, false, 8, 0, imm (0, 1)),		/* SVC's r8 is the user's */
		dp (MOV, false, 13, 0, imm (0, 0x10)),	/* SVC's r13 */
		msr_c (0xC0 | Arm7::MODE_FIQ),
		dp (MOV, false, 8, 0, imm (0, 2)),
		dp (MOV, false, 13, 0, imm (0, 0x20)),
		msr_c (0xC0 | Arm7::MODE_IRQ),
		dp (MOV, false, 3, 0, 8),			/* the user's r8 again */
		dp (MOV, false, 13, 0, imm (0, 0x30)),
		msr_c (0xC0 | Arm7::MODE_SVC),
		dp (MOV, false, 4, 0, 13),
		msr_c (0xC0 | Arm7::MODE_FIQ),
		dp (MOV, false, 5, 0, 8),
		dp (MOV, false, 6, 0, 13),
	});
	CHECK ("irq sees user r8", cpu.regs.r[3], 1);
	CHECK ("svc r13 kept", cpu.regs.r[4], 0x10);
	CHECK ("fiq r8 kept", cpu.regs.r[5], 2);
	CHECK ("fiq r13 kept", cpu.regs.r[6], 0x20);

	/* SWI and back */
	bus.write32 (0x08, MOVS_PC_LR);
	run (cpu, bus, {
		msr_c (Arm7::MODE_USR),
		0xEF000000,					/* 104: SWI 0 */
		dp (MOV, false, 5, 0, imm (0, 1)),		/* 108 */
	});
	CHECK ("swi returned", cpu.regs.r[5], 1);
	CHECK ("swi back to user", cpu.regs.cpsr & 0x1F, Arm7::MODE_USR);
	CHECK ("swi spsr", cpu.regs.spsr_svc & 0x1F, Arm7::MODE_USR);
	CHECK ("swi lr", cpu.regs.svc_r13_14[1], 0x108);

	/* An IRQ taken between two instructions, its handler returning with
	 * SUBS PC, LR, #4 */
	bus.write32 (0x18, dp (MOV, false, 6, 0, imm (0, 7)));
	bus.write32 (0x1C, SUBS_PC_LR_4);
	{
		uint32_t end = load (cpu, bus, {
			msr_c (Arm7::MODE_USR),		/* 100: IRQs on */
			dp (MOV, false, 1, 0, imm (0, 1)),	/* 104 */
			dp (MOV, false, 2, 0, imm (0, 2)),	/* 108 */
		});
		cpu.step ();
		cpu.step ();				/* 104 done */
		cpu.set_irq (true);
		cpu.step ();				/* taken */
		CHECK ("irq mode", cpu.regs.cpsr & 0x1F, Arm7::MODE_IRQ);
		CHECK ("irq lr", cpu.regs.r[14], 0x10C);
		CHECK ("irq masks irqs", (cpu.regs.cpsr >> 7) & 1, 1);
		cpu.set_irq (false);
		for (int i = 0; i < 10 && cpu.regs.r[15] != end; i++)
			cpu.step ();
		CHECK ("irq handler ran", cpu.regs.r[6], 7);
		CHECK ("irq returned to 108", cpu.regs.r[2], 2);
		CHECK ("irq back to user", cpu.regs.cpsr & 0x1F, Arm7::MODE_USR);
	}

	/* An FIQ, which also masks the IRQs and banks r8 */
	bus.write32 (0x1C, dp (MOV, false, 8, 0, imm (0, 9)));
	bus.write32 (0x20, SUBS_PC_LR_4);
	{
		uint32_t end = load (cpu, bus, {
			dp (MOV, false, 8, 0, imm (0, 3)),	/* 100 */
			msr_c (Arm7::MODE_USR),		/* 104: FIQs on */
			dp (MOV, false, 1, 0, imm (0, 1)),	/* 108 */
		});
		cpu.step ();
		cpu.step ();
		cpu.set_fiq (true);
		cpu.step ();
		CHECK ("fiq mode", cpu.regs.cpsr & 0x1F, Arm7::MODE_FIQ);
		CHECK ("fiq masks both", (cpu.regs.cpsr >> 6) & 3, 3);
		cpu.set_fiq (false);
		for (int i = 0; i < 10 && cpu.regs.r[15] != end; i++)
			cpu.step ();
		CHECK ("fiq returned", cpu.regs.r[1], 1);
		CHECK ("fiq r8 banked", cpu.regs.r[8], 3);
		CHECK ("fiq's own r8", cpu.regs.fiq_r8_14[0], 9);
	}

	/* LDM with ^ and PC: the SPSR back into the CPSR */
	bus.write32 (0x3000, 0x200);	/* the return address */
	bus.write32 (0x200, dp (MOV, false, 7, 0, imm (0, 7)));
	bus.write32 (0x204, B_SELF);
	load (cpu, bus, {
		dp (MOV, false, 0, 0, imm (10, 3)),		/* 100: r0 = 3000 */
		msr_c (0xC0 | Arm7::MODE_IRQ),		/* 104 */
		dp (MOV, false, 1, 0, imm (0, Arm7::MODE_USR)),
		0xE16FF001,					/* MSR SPSR_fsxc, r1 */
		ldstm (true, 1, true, false, 0, 0x8000),	/* LDMIA r0, {pc}^ */
	});
	for (int i = 0; i < 20 && cpu.regs.r[15] != 0x204; i++)
		cpu.step ();
	CHECK ("ldm ^ pc", cpu.regs.r[7], 7);
	CHECK ("ldm ^ restores user", cpu.regs.cpsr & 0x1F, Arm7::MODE_USR);

	/* The translations: as step () */
	test_jit_self_modifying ();
	for (unsigned seed = 1; seed <= 3000; seed++)
		test_jit_random (seed);

	/* The scan loops, run fast: as slow, whatever the budget cuts */
	for (int budget : { 1, 7, 11, 12, 19, 64, 100 })
		for (unsigned slot : { 0u, 1u, 37u, 63u, 64u })
			test_scan_loop (slot, budget);

	printf ("arm7test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
