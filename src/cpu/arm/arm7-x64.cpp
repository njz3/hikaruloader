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

/*
 * The ARM7's translations to x86-64 (Arm7::jit): run () takes the RAM's code
 * a block at a time where step () would take its instructions one by one,
 * with the same registers, flags, memory and cycles.
 *
 * A block runs from its address up to a branch, a write to r15, an
 * instruction that may change the mode or the interrupts' mask, or 32
 * instructions. Its code first compares the RAM's words with those it was
 * translated from (the SH-4 loads the drivers between two run ()), and leaves
 * when they differ: run () translates them again. Within a block, the
 * registers stay in Arm7's own (regs.r[]), the flags in CPSR; each
 * instruction's condition is tested as step () tests it, its cycles counted
 * as step () counts them.
 *
 * The blocks go on to each other through the dispatcher, written once an
 * arena: while the budget lasts (run (): an instruction starts while done <
 * cycles), no interrupt waits (step () would take it), r15 is not the scan
 * loop's head (run () runs that one), it jumps to the block at r15 when the
 * table holds it, else back to run (), which translates it. The interrupts'
 * lines change only by a store to the bus (the AICA's registers): a block
 * leaves to the dispatcher after one, and after a store into its own words.
 *
 * Written out: data processing with an immediate or a register shifted by an
 * immediate, MUL and MLA, the word, byte and halfword transfers, LDM and STM
 * without S, B and BL (a block that branches to its own start loops within
 * itself). The others go through Arm7::execute (), r15 set as step () sets
 * it.
 *
 * The frame: rbx the Arm7, r12 its RAM, r13 the RAM's size, r14 the cycles
 * run, r15 the budget, ebp set by a store the block must leave after; rsi
 * and rdi kept across the calls out. The prologue is the SH-4's translations'
 * (sh4-x64.cpp), for the same unwind information.
 */

#include <deque>
#include <memory>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cpu/arm/arm7.h"
#include "cpu/x64/x64-emitter.h"

using namespace x64;

class Arm7Jit {
public:
	explicit Arm7Jit (Arm7 *arm);
	~Arm7Jit ();
	Arm7Jit (const Arm7Jit &) = delete;
	Arm7Jit &operator= (const Arm7Jit &) = delete;

	/* From the block at r15 (translated if need be) within budget: the
	 * cycles run, 0 for none (the interpreter then) */
	int	 run (int budget);

private:
	static const unsigned MAX_INSTS = 32, CACHE_SIZE = 4096;
	static const size_t ARENA_SIZE = 8u << 20, BLOCK_MAX = 32u << 10;
	typedef int (*Entry) (Arm7 *, int);

	/* The dispatcher reads pc and body */
	struct Block {
		uint32_t pc, n;
		uint8_t	*body;
		uint32_t words[MAX_INSTS];
	};

	/* What an instruction is to the translations */
	enum Kind { K_DP, K_MUL, K_SINGLE, K_HALF, K_MULTI, K_BRANCH, K_CALL };
	/* The shifter's carry for the flags: CPSR's C kept, a constant, r11 */
	enum Carry { C_KEPT, C_ZERO, C_ONE, C_REG };

	Arm7	*arm;
	CodeArena arena;
	bool	 started = false;
	void	*table = NULL;
	std::unordered_map<uint32_t, std::unique_ptr<Block>> blocks;
	Block	*cache[CACHE_SIZE] = {};
	int32_t	 off_r[16], off_cpsr, off_cycles, off_ram, off_ram_size;
	int32_t	 off_fiq_line, off_irq_line, off_fast_loops, off_scan_head;
	/* The arena's stubs: the entry (prologue), the dispatcher, the way
	 * out (epilogue) */
	Entry	 entry = NULL;
	uint8_t	*dispatch = NULL, *out_stub = NULL;

	/* While translating: the block, its labels */
	const Block *cur = NULL;
	std::deque<Label> labels;
	Label	&label () { labels.emplace_back (); return labels.back (); }

	bool	 start_arena ();
	bool	 emit_stubs ();
	void	 clear ();
	Block	*translate (uint32_t pc);
	static Kind classify (uint32_t inst, bool *ends);

	void	 load_reg (Emitter &e, Reg dst, unsigned n, uint32_t at, unsigned ahead);
	void	 load_raw (Emitter &e, Reg dst, unsigned n, uint32_t at);
	void	 add_cycles (Emitter &e, int n);
	void	 cond_test (Emitter &e, unsigned cond, Label &skip);
	Carry	 shift_imm (Emitter &e, unsigned type, unsigned amount, bool want_carry);
	void	 set_nz (Emitter &e, uint32_t keep, Carry carry);
	void	 set_nzcv (Emitter &e, bool sub_like);
	void	 mem_load (Emitter &e, unsigned size, bool sign, Reg addr);
	void	 mem_store (Emitter &e, unsigned size, Reg addr);
	void	 transfer_end (Emitter &e, uint32_t inst, bool load, bool reg_off,
		               int32_t off, int cycles, Label &out);

	void	 emit_dp (Emitter &e, uint32_t inst, uint32_t at, Label &out);
	void	 emit_mul (Emitter &e, uint32_t inst, uint32_t at);
	bool	 emit_single (Emitter &e, uint32_t inst, uint32_t at, Label &out);
	bool	 emit_half (Emitter &e, uint32_t inst, uint32_t at, Label &out);
	bool	 emit_multi (Emitter &e, uint32_t inst, uint32_t at, Label &out);
	void	 emit_branch (Emitter &e, uint32_t inst, uint32_t at, bool last, Label &top, Label &out);
	void	 emit_call (Emitter &e, uint32_t inst, uint32_t at, bool ends, Label &out);

	/* The calls out */
	static uint32_t bus_read32 (Arm7 *a, uint32_t addr) { return a->bus->read32 (addr); }
	static uint32_t bus_read16 (Arm7 *a, uint32_t addr) { return a->bus->read16 (addr); }
	static uint32_t bus_read8 (Arm7 *a, uint32_t addr) { return a->bus->read8 (addr); }
	static void bus_write32 (Arm7 *a, uint32_t addr, uint32_t v) { a->bus->write32 (addr, v); }
	static void bus_write16 (Arm7 *a, uint32_t addr, uint32_t v) { a->bus->write16 (addr, (uint16_t) v); }
	static void bus_write8 (Arm7 *a, uint32_t addr, uint32_t v) { a->bus->write8 (addr, (uint8_t) v); }
	static int execute (Arm7 *a, uint32_t inst) { return a->execute (inst); }
	/* branch ()'s look for a scan loop */
	static void scan_hook (Arm7 *a, uint32_t target)
	{
		if (a->fast_loops && target != a->scan.head && target != a->scan.rejected)
			a->find_scan_loop (target);
	}
};

enum { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };

static inline uint32_t
ror32 (uint32_t val, unsigned n)
{
	n &= 31;
	return n ? (val >> n) | (val << (32 - n)) : val;
}

/* The arithmetic group's with the carry (Emitter::Alu: the r/m32, r32 forms) */
static const Emitter::Alu ALU_ADC = (Emitter::Alu) 0x11, ALU_SBB = (Emitter::Alu) 0x19;

Arm7Jit::Arm7Jit (Arm7 *arm) : arm (arm), arena (ARENA_SIZE)
{
	const uint8_t *base = (const uint8_t *) arm;

	for (unsigned i = 0; i < 16; i++)
		off_r[i] = (int32_t) ((const uint8_t *) &arm->regs.r[i] - base);
	off_cpsr = (int32_t) ((const uint8_t *) &arm->regs.cpsr - base);
	off_cycles = (int32_t) ((const uint8_t *) &arm->regs.cycles - base);
	off_ram = (int32_t) ((const uint8_t *) &arm->ram - base);
	off_ram_size = (int32_t) ((const uint8_t *) &arm->ram_size - base);
	off_fiq_line = (int32_t) ((const uint8_t *) &arm->regs.fiq_line - base);
	off_irq_line = (int32_t) ((const uint8_t *) &arm->regs.irq_line - base);
	off_fast_loops = (int32_t) ((const uint8_t *) &arm->fast_loops - base);
	off_scan_head = (int32_t) ((const uint8_t *) &arm->scan.head - base);
}

Arm7Jit::~Arm7Jit ()
{
#ifdef _WIN32
	if (table)
		RtlDeleteFunctionTable ((PRUNTIME_FUNCTION) table);
#endif
}

/* The unwind information first, as the SH-4's arena has it, then one
 * function for the whole arena; then the stubs */
bool
Arm7Jit::start_arena ()
{
	static const uint8_t unwind[] = {
		0x01, 19, 9, 0x00,
		19, 0x42,	/* sub rsp, 40 */
		12, 0xF0, 10, 0xE0, 8, 0xD0, 6, 0xC0,	/* r15, r14, r13, r12 */
		4, 0x50, 3, 0x70, 2, 0x60, 1, 0x30,	/* rbp, rdi, rsi, rbx */
		0, 0,
	};
	uint8_t *at;

	arena.reset ();
	if (!arena.base ())
		return false;
	at = arena.take (sizeof (unwind), 4);
	if (!at)
		return false;
	memcpy (at, unwind, sizeof (unwind));
#ifdef _WIN32
	RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *) arena.take (sizeof (RUNTIME_FUNCTION), 4);

	if (!rf)
		return false;
	rf->BeginAddress = (DWORD) (arena.top () - arena.base ());
	rf->EndAddress = (DWORD) ARENA_SIZE;
	rf->UnwindData = (DWORD) (at - arena.base ());
	if (RtlAddFunctionTable (rf, 1, (DWORD64) arena.base ()))
		table = rf;
#endif
	if (!emit_stubs ())
		return false;
	started = true;
	return true;
}

/* The entry: the prologue, then the dispatcher; the way out: the cycles
 * counted and returned */
bool
Arm7Jit::emit_stubs ()
{
	const size_t max = 1024;
	uint8_t *buf = arena.take (max, 16);

	if (!buf)
		return false;
	Emitter e (buf, max);
	Label disp, out, fiq_off, irq_off, no_scan;

	entry = (Entry) e.here ();
	e.push (RBX);
	e.push (RSI);
	e.push (RDI);
	e.push (RBP);
	e.push (R12);
	e.push (R13);
	e.push (R14);
	e.push (R15);
	e.sub64_imm (RSP, 40);
	e.mov (RBX, RCX);
	e.mov32 (R15, RDX);
	e.alu32_reg_reg (Emitter::ALU_XOR, R14, R14);
	e.load64 (R12, RBX, off_ram);
	e.load32 (R13, RBX, off_ram_size);

	/* The dispatcher: as run () would go on */
	e.bind (disp);
	dispatch = e.here ();
	e.alu32_reg_reg (Emitter::ALU_XOR, RBP, RBP);
	e.alu32_reg_reg (Emitter::ALU_CMP, R14, R15);
	e.jcc (CC_GE, out);
	e.movzx8_mem (RAX, RBX, off_fiq_line);
	e.test32 (RAX, RAX);
	e.jcc (CC_E, fiq_off);
	e.test_mem_imm (RBX, off_cpsr, Arm7::PSR_F);
	e.jcc (CC_E, out);
	e.bind (fiq_off);
	e.movzx8_mem (RAX, RBX, off_irq_line);
	e.test32 (RAX, RAX);
	e.jcc (CC_E, irq_off);
	e.test_mem_imm (RBX, off_cpsr, Arm7::PSR_I);
	e.jcc (CC_E, out);
	e.bind (irq_off);
	e.load32 (RAX, RBX, off_r[15]);
	e.movzx8_mem (RCX, RBX, off_fast_loops);
	e.test32 (RCX, RCX);
	e.jcc (CC_E, no_scan);
	e.alu_reg_mem (Emitter::ALU_CMP, RAX, RBX, off_scan_head);
	e.jcc (CC_E, out);
	e.bind (no_scan);
	e.test8_imm (RAX, 3);
	e.jcc (CC_NE, out);
	e.alu32_reg_reg (Emitter::ALU_CMP, RAX, R13);
	e.jcc (CC_AE, out);
	e.mov32 (RCX, RAX);
	e.shr32_imm (RCX, 2);
	e.and32_imm (RCX, CACHE_SIZE - 1);
	e.mov_imm64 (RDX, (uint64_t) (uintptr_t) cache);
	e.load64_index (RDX, RDX, RCX, 8);
	e.test64 (RDX, RDX);
	e.jcc (CC_E, out);
	e.alu_reg_mem (Emitter::ALU_CMP, RAX, RDX, (int32_t) offsetof (Block, pc));
	e.jcc (CC_NE, out);
	e.load64 (RAX, RDX, (int32_t) offsetof (Block, body));
	e.jmp_reg (RAX);

	/* The way out */
	e.bind (out);
	out_stub = e.here ();
	e.load64 (RAX, RBX, off_cycles);
	e.alu64_reg_reg (Emitter::ALU_ADD, RAX, R14);
	e.store64 (RBX, off_cycles, RAX);
	e.mov32 (RAX, R14);
	e.add64_imm (RSP, 40);
	e.pop (R15);
	e.pop (R14);
	e.pop (R13);
	e.pop (R12);
	e.pop (RBP);
	e.pop (RDI);
	e.pop (RSI);
	e.pop (RBX);
	e.ret ();

	if (e.overflowed ())
		return false;
	arena.trim (buf + e.length ());
	return true;
}

void
Arm7Jit::clear ()
{
#ifdef _WIN32
	if (table)
		RtlDeleteFunctionTable ((PRUNTIME_FUNCTION) table);
#endif
	table = NULL;
	started = false;
	blocks.clear ();
	memset (cache, 0, sizeof (cache));
}

int
Arm7Jit::run (int budget)
{
	const uint32_t pc = arm->regs.r[15];
	Block *b;

	if ((pc & 3) || pc >= arm->ram_size)
		return 0;
	Block *&slot = cache[(pc >> 2) & (CACHE_SIZE - 1)];
	b = slot;
	if (!b || b->pc != pc) {
		auto it = blocks.find (pc);
		b = (it != blocks.end ()) ? it->second.get () : NULL;
	}
	if (!b || !b->body || memcmp (arm->ram + pc, b->words, b->n * 4u))
		b = translate (pc);
	if (!b)
		return 0;
	cache[(pc >> 2) & (CACHE_SIZE - 1)] = b;
	return entry (arm, budget);
}

/****************************************************************************
 The instructions
****************************************************************************/

/* As Arm7::execute () tells them apart; ends: the block stops after it */
Arm7Jit::Kind
Arm7Jit::classify (uint32_t inst, bool *ends)
{
	const unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	const bool pre = inst & (1u << 24), wback = inst & (1u << 21), load = inst & (1u << 20);

	*ends = false;
	switch ((inst >> 25) & 7) {
	case 0:
		if ((inst & 0x0FFFFFF0) == 0x012FFF10) {	/* BX */
			*ends = true;
			return K_CALL;
		}
		if ((inst & 0x0FC000F0) == 0x00000090) {	/* MUL, MLA */
			if (rn == 15) {
				*ends = true;
				return K_CALL;
			}
			return K_MUL;
		}
		if ((inst & 0x0F8000F0) == 0x00800090) {	/* the long ones */
			*ends = rn == 15 || rd == 15;
			return K_CALL;
		}
		if ((inst & 0x0FB00FF0) == 0x01000090) {	/* SWP: a store */
			*ends = true;
			return K_CALL;
		}
		if ((inst & 0x0E000090) == 0x00000090 && (inst & 0x60)) {
			const unsigned sh = (inst >> 5) & 3;

			if ((!load && sh != 1) || (rn == 15 && (!pre || wback))) {
				*ends = true;
				return K_CALL;
			}
			*ends = load && rd == 15;
			return K_HALF;
		}
		/* fall through */
	case 1:
		if ((inst & 0x0D900000) == 0x01000000) {
			/* MRS goes on unless into r15; MSR and the undefined
			 * ones stop */
			*ends = (inst & 0x0FBF0FFF) != 0x010F0000 || rd == 15;
			return K_CALL;
		} else {
			const unsigned op = (inst >> 21) & 15;
			const bool write = op < TST || op > CMN;
			const bool s = inst & (1u << 20);
			const bool reg_shift = !(inst & (1u << 25)) && (inst & 0x10);

			if (write && rd == 15) {
				*ends = true;
				return (s || reg_shift) ? K_CALL : K_DP;
			}
			return reg_shift ? K_CALL : K_DP;
		}
	case 2:
	case 3:
		if (((inst >> 25) & 7) == 3 && (inst & 0x10)) {	/* undefined */
			*ends = true;
			return K_CALL;
		}
		if (rn == 15 && (!pre || wback)) {
			*ends = true;
			return K_CALL;
		}
		*ends = load && rd == 15;
		return K_SINGLE;
	case 4:
		/* With S (the user bank, or the SPSR back), r15 the base or
		 * none: through execute (); r15 loaded ends */
		if ((inst & (1u << 22)) || rn == 15 || !(inst & 0xFFFF)) {
			*ends = true;
			return K_CALL;
		}
		*ends = load && (inst & 0x8000);
		return K_MULTI;
	case 5:
		*ends = true;
		return K_BRANCH;
	default:
		*ends = true;
		return K_CALL;
	}
}

/* get_reg (n, ahead): r15 the instruction's address plus ahead */
void
Arm7Jit::load_reg (Emitter &e, Reg dst, unsigned n, uint32_t at, unsigned ahead)
{
	if (n == 15)
		e.mov_imm32 (dst, at + ahead);
	else
		e.load32 (dst, RBX, off_r[n]);
}

/* regs.r[n] as it is while the instruction runs: r15 its address plus 4 */
void
Arm7Jit::load_raw (Emitter &e, Reg dst, unsigned n, uint32_t at)
{
	load_reg (e, dst, n, at, 4);
}

void
Arm7Jit::add_cycles (Emitter &e, int n)
{
	e.alu32_imm (Emitter::ALU_ADD, R14, n);
}

/* To skip when the condition fails (Arm7::condition ()) */
void
Arm7Jit::cond_test (Emitter &e, unsigned cond, Label &skip)
{
	static const uint32_t bits[8] = {
		(uint32_t) Arm7::PSR_Z, (uint32_t) Arm7::PSR_Z, (uint32_t) Arm7::PSR_C, (uint32_t) Arm7::PSR_C,
		(uint32_t) Arm7::PSR_N, (uint32_t) Arm7::PSR_N, (uint32_t) Arm7::PSR_V, (uint32_t) Arm7::PSR_V
	};

	if (cond < 8) {
		/* One flag, set (even) or clear (odd) */
		e.test_mem_imm (RBX, off_cpsr, bits[cond]);
		e.jcc ((cond & 1) ? CC_NE : CC_E, skip);
		return;
	}
	/* The table's bit for NZCV */
	e.load32 (RCX, RBX, off_cpsr);
	e.shr32_imm (RCX, 28);
	e.mov_imm32 (RAX, arm->conditions[cond]);
	e.shift32_cl (5, RAX);
	e.test8_imm (RAX, 1);
	e.jcc (CC_E, skip);
}

/* ecx shifted by an immediate as the shifter shifts it (0: LSR #32, ASR #32,
 * RRX); its carry in r11 when wanted */
Arm7Jit::Carry
Arm7Jit::shift_imm (Emitter &e, unsigned type, unsigned amount, bool want_carry)
{
	switch (type) {
	case 0:	/* LSL */
		if (amount == 0)
			return C_KEPT;
		e.shl32_imm (RCX, (uint8_t) amount);
		break;
	case 1:	/* LSR */
		if (amount == 0) {
			if (want_carry) {
				e.mov32 (R11, RCX);
				e.shr32_imm (R11, 31);
			}
			e.alu32_reg_reg (Emitter::ALU_XOR, RCX, RCX);
			return want_carry ? C_REG : C_KEPT;
		}
		e.shr32_imm (RCX, (uint8_t) amount);
		break;
	case 2:	/* ASR */
		if (amount == 0) {
			if (want_carry) {
				e.mov32 (R11, RCX);
				e.shr32_imm (R11, 31);
			}
			e.rot32 (Emitter::ROT_SAR, RCX, 31);
			return want_carry ? C_REG : C_KEPT;
		}
		e.rot32 (Emitter::ROT_SAR, RCX, (uint8_t) amount);
		break;
	default:
		if (amount == 0) {
			/* RRX: C in at the top, bit 0 out */
			e.bt_mem_imm (RBX, off_cpsr, 29);
			e.rot32 (Emitter::ROT_RCR, RCX, 1);
		} else
			e.rot32 (Emitter::ROT_ROR, RCX, (uint8_t) amount);
		break;
	}
	/* The last bit out (ror: the result's top bit) */
	if (!want_carry)
		return C_KEPT;
	e.setcc (CC_B, R11);
	e.movzx8 (R11, R11);
	return C_REG;
}

/* N and Z from eax, C as carry says, the rest of CPSR as keep keeps it */
void
Arm7Jit::set_nz (Emitter &e, uint32_t keep, Carry carry)
{
	e.test32 (RAX, RAX);
	e.setcc (CC_S, RDX);
	e.setcc (CC_E, R8);
	e.movzx8 (RDX, RDX);
	e.shl32_imm (RDX, 31);
	e.movzx8 (R8, R8);
	e.shl32_imm (R8, 30);
	e.alu32_reg_reg (Emitter::ALU_OR, RDX, R8);
	if (carry == C_REG) {
		e.shl32_imm (R11, 29);
		e.alu32_reg_reg (Emitter::ALU_OR, RDX, R11);
	} else if (carry == C_ONE)
		e.alu32_imm (Emitter::ALU_OR, RDX, (int32_t) Arm7::PSR_C);
	e.load32 (R8, RBX, off_cpsr);
	e.and32_imm (R8, keep);
	e.alu32_reg_reg (Emitter::ALU_OR, R8, RDX);
	e.store32 (RBX, off_cpsr, R8);
}

/* NZCV from the host's flags after an addition (C the carry) or a
 * subtraction (C no borrow) */
void
Arm7Jit::set_nzcv (Emitter &e, bool sub_like)
{
	e.setcc (CC_S, RDX);
	e.setcc (CC_E, R8);
	e.setcc (sub_like ? CC_AE : CC_B, R9);
	e.setcc (CC_O, R10);
	e.movzx8 (RDX, RDX);
	e.shl32_imm (RDX, 31);
	e.movzx8 (R8, R8);
	e.shl32_imm (R8, 30);
	e.alu32_reg_reg (Emitter::ALU_OR, RDX, R8);
	e.movzx8 (R9, R9);
	e.shl32_imm (R9, 29);
	e.alu32_reg_reg (Emitter::ALU_OR, RDX, R9);
	e.movzx8 (R10, R10);
	e.shl32_imm (R10, 28);
	e.alu32_reg_reg (Emitter::ALU_OR, RDX, R10);
	e.load32 (R8, RBX, off_cpsr);
	e.and32_imm (R8, 0x0FFFFFFF);
	e.alu32_reg_reg (Emitter::ALU_OR, R8, RDX);
	e.store32 (RBX, off_cpsr, R8);
}

/* eax = the size bytes at addr (aligned as the core aligns it): the RAM's,
 * or the bus' */
void
Arm7Jit::mem_load (Emitter &e, unsigned size, bool sign, Reg addr)
{
	Label &bus = label (), &done = label ();

	e.alu32_reg_reg (Emitter::ALU_CMP, addr, R13);
	e.jcc (CC_AE, bus);
	e.mov (RAX, R12);
	e.alu64_reg_reg (Emitter::ALU_ADD, RAX, addr);
	switch (size) {
	case 4: e.load32 (RAX, RAX, 0); break;
	case 2: if (sign) e.movsx16_mem (RAX, RAX, 0); else e.movzx16_mem (RAX, RAX, 0); break;
	default: if (sign) e.movsx8_mem (RAX, RAX, 0); else e.movzx8_mem (RAX, RAX, 0); break;
	}
	e.jmp (done);
	e.bind (bus);
	e.mov32 (RDX, addr);
	e.mov (RCX, RBX);
	switch (size) {
	case 4: e.call ((const void *) bus_read32); break;
	case 2: e.call ((const void *) bus_read16); if (sign) e.movsx16 (RAX, RAX); break;
	default: e.call ((const void *) bus_read8); if (sign) e.movsx8 (RAX, RAX); break;
	}
	e.bind (done);
}

/* edi's size bytes to addr (aligned): the RAM's, ebp set when within the
 * block's own words; or the bus', ebp set */
void
Arm7Jit::mem_store (Emitter &e, unsigned size, Reg addr)
{
	Label &bus = label (), &done = label ();

	e.alu32_reg_reg (Emitter::ALU_CMP, addr, R13);
	e.jcc (CC_AE, bus);
	switch (size) {
	case 4: e.store32_index (R12, addr, 1, RDI); break;
	case 2: e.store16_index (R12, addr, 1, RDI); break;
	default: e.mov32 (RAX, RDI); e.store8_index (R12, addr, 1, RAX); break;
	}
	e.lea32 (RAX, addr, -(int32_t) cur->pc);
	e.cmp32_imm (RAX, cur->n * 4);
	e.jcc (CC_AE, done);
	e.mov_imm32 (RBP, 1);
	e.jmp (done);
	e.bind (bus);
	e.mov32 (RDX, addr);
	e.mov32 (R8, RDI);
	e.mov (RCX, RBX);
	switch (size) {
	case 4: e.call ((const void *) bus_write32); break;
	case 2: e.call ((const void *) bus_write16); break;
	default: e.call ((const void *) bus_write8); break;
	}
	e.mov_imm32 (RBP, 1);
	e.bind (done);
}

/****************************************************************************
 Data processing, MUL
****************************************************************************/

/* Operand 2 an immediate or a register shifted by an immediate; rd 15
 * without S (the block ends) */
void
Arm7Jit::emit_dp (Emitter &e, uint32_t inst, uint32_t at, Label &out)
{
	const unsigned op = (inst >> 21) & 15, rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	const bool s = inst & (1u << 20);
	const bool logical = op == AND || op == EOR || op == TST || op == TEQ ||
	                     op == ORR || op == MOV || op == BIC || op == MVN;
	const bool write = op < TST || op > CMN;
	bool sub_like = false;
	Carry carry = C_KEPT;

	/* Operand 2 in ecx */
	if (inst & (1u << 25)) {
		const uint32_t rot = ((inst >> 8) & 15) * 2, val = ror32 (inst & 0xFF, rot);

		e.mov_imm32 (RCX, val);
		if (rot)
			carry = (val >> 31) ? C_ONE : C_ZERO;
	} else {
		load_reg (e, RCX, inst & 15, at, 8);
		carry = shift_imm (e, (inst >> 5) & 3, (inst >> 7) & 31, s && logical);
	}
	if (op != MOV && op != MVN)
		load_reg (e, RAX, rn, at, 8);

	switch (op) {
	case AND: case TST: e.alu32_reg_reg (Emitter::ALU_AND, RAX, RCX); break;
	case EOR: case TEQ: e.alu32_reg_reg (Emitter::ALU_XOR, RAX, RCX); break;
	case SUB: case CMP: e.alu32_reg_reg (Emitter::ALU_SUB, RAX, RCX); sub_like = true; break;
	case RSB:
		e.alu32_reg_reg (Emitter::ALU_SUB, RCX, RAX);
		e.mov32 (RAX, RCX);
		sub_like = true;
		break;
	case ADD: case CMN: e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RCX); break;
	case ADC:
		e.bt_mem_imm (RBX, off_cpsr, 29);
		e.alu32_reg_reg (ALU_ADC, RAX, RCX);
		break;
	case SBC:
		/* The borrow in: CF = !C */
		e.bt_mem_imm (RBX, off_cpsr, 29);
		e.byte (0xF5);		/* cmc */
		e.alu32_reg_reg (ALU_SBB, RAX, RCX);
		sub_like = true;
		break;
	case RSC:
		e.bt_mem_imm (RBX, off_cpsr, 29);
		e.byte (0xF5);
		e.alu32_reg_reg (ALU_SBB, RCX, RAX);
		e.mov32 (RAX, RCX);
		sub_like = true;
		break;
	case ORR: e.alu32_reg_reg (Emitter::ALU_OR, RAX, RCX); break;
	case MOV: e.mov32 (RAX, RCX); break;
	case BIC:
		e.not32 (RCX);
		e.alu32_reg_reg (Emitter::ALU_AND, RAX, RCX);
		break;
	default: /* MVN */
		e.not32 (RCX);
		e.mov32 (RAX, RCX);
		break;
	}

	if (s) {
		if (logical)
			set_nz (e, carry == C_KEPT ? 0x3FFFFFFF : 0x1FFFFFFF, carry);
		else
			set_nzcv (e, sub_like);
	}
	if (!write) {
		add_cycles (e, 1);
		return;
	}
	if (rd != 15) {
		e.store32 (RBX, off_r[rd], RAX);
		add_cycles (e, 1);
		return;
	}
	/* MOV PC, LR and the like */
	e.and32_imm (RAX, ~3u);
	e.store32 (RBX, off_r[15], RAX);
	add_cycles (e, 3);
	e.jmp (out);
}

void
Arm7Jit::emit_mul (Emitter &e, uint32_t inst, uint32_t at)
{
	const unsigned rd = (inst >> 16) & 15, rn = (inst >> 12) & 15;
	const unsigned rs = (inst >> 8) & 15, rm = inst & 15;
	const bool mla = inst & (1u << 21);

	load_raw (e, RAX, rm, at);
	load_raw (e, RCX, rs, at);
	e.imul32 (RAX, RCX);
	if (mla) {
		load_raw (e, RCX, rn, at);
		e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RCX);
	}
	e.store32 (RBX, off_r[rd], RAX);
	if (inst & (1u << 20))
		set_nz (e, 0x3FFFFFFF, C_KEPT);
	add_cycles (e, mla ? 4 : 3);
}

/****************************************************************************
 Transfers
****************************************************************************/

/* After the access: the base written back (esi the address; the offset off,
 * or at [rsp + 32]), then a load's rd (eax), r15 loaded ending the block;
 * the cycles */
void
Arm7Jit::transfer_end (Emitter &e, uint32_t inst, bool load, bool reg_off,
                       int32_t off, int cycles, Label &out)
{
	const bool pre = inst & (1u << 24), wback = inst & (1u << 21);
	const unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;

	if (!pre) {
		if (reg_off) {
			e.load32 (RDX, RSP, 32);
			e.alu32_reg_reg (Emitter::ALU_ADD, RDX, RSI);
		} else
			e.lea32 (RDX, RSI, off);
		e.store32 (RBX, off_r[rn], RDX);
	} else if (wback)
		e.store32 (RBX, off_r[rn], RSI);
	if (!load) {
		add_cycles (e, cycles);
		return;
	}
	if (rd != 15) {
		e.store32 (RBX, off_r[rd], RAX);
		add_cycles (e, cycles);
		return;
	}
	e.and32_imm (RAX, ~3u);
	e.store32 (RBX, off_r[15], RAX);
	add_cycles (e, cycles + 2);
	e.jmp (out);
}

/* LDR, STR and their B forms; true: a store (ebp may be set) */
bool
Arm7Jit::emit_single (Emitter &e, uint32_t inst, uint32_t at, Label &out)
{
	const bool pre = inst & (1u << 24), up = inst & (1u << 23);
	const bool byte = inst & (1u << 22), load = inst & (1u << 20);
	const unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15;
	const bool reg_off = inst & (1u << 25);
	int32_t off = 0;

	/* esi the base, then the address */
	load_reg (e, RSI, rn, at, 8);
	if (reg_off) {
		load_reg (e, RCX, inst & 15, at, 8);
		shift_imm (e, (inst >> 5) & 3, (inst >> 7) & 31, false);
		if (!up)
			e.neg32 (RCX);
		if (pre)
			e.alu32_reg_reg (Emitter::ALU_ADD, RSI, RCX);
		else
			e.store32 (RSP, 32, RCX);
	} else {
		off = (int32_t) (inst & 0xFFF);
		if (!up)
			off = -off;
		if (pre && off)
			e.alu32_imm (Emitter::ALU_ADD, RSI, off);
	}

	if (load) {
		if (byte)
			mem_load (e, 1, false, RSI);
		else {
			/* An unaligned word: the aligned one, rotated */
			e.mov32 (RCX, RSI);
			e.and32_imm (RCX, ~3u);
			mem_load (e, 4, false, RCX);
			e.mov32 (RCX, RSI);
			e.and32_imm (RCX, 3);
			e.shl32_imm (RCX, 3);
			e.shift32_cl (1, RAX);		/* ror eax, cl */
		}
		transfer_end (e, inst, true, reg_off, off, 3, out);
		return false;
	}
	/* r15 stored is the instruction plus 12 */
	load_reg (e, RDI, rd, at, 12);
	if (byte)
		mem_store (e, 1, RSI);
	else {
		e.mov32 (RCX, RSI);
		e.and32_imm (RCX, ~3u);
		mem_store (e, 4, RCX);
	}
	transfer_end (e, inst, false, reg_off, off, 2, out);
	return true;
}

/* LDRH, LDRSB, LDRSH, STRH; true: a store */
bool
Arm7Jit::emit_half (Emitter &e, uint32_t inst, uint32_t at, Label &out)
{
	const bool pre = inst & (1u << 24), up = inst & (1u << 23), load = inst & (1u << 20);
	const unsigned rn = (inst >> 16) & 15, rd = (inst >> 12) & 15, sh = (inst >> 5) & 3;
	const bool reg_off = !(inst & (1u << 22));
	int32_t off = 0;

	load_reg (e, RSI, rn, at, 8);
	if (reg_off) {
		/* The register as it is (r15: the instruction plus 4) */
		load_raw (e, RCX, inst & 15, at);
		if (!up)
			e.neg32 (RCX);
		if (pre)
			e.alu32_reg_reg (Emitter::ALU_ADD, RSI, RCX);
		else
			e.store32 (RSP, 32, RCX);
	} else {
		off = (int32_t) (((inst >> 4) & 0xF0) | (inst & 15));
		if (!up)
			off = -off;
		if (pre && off)
			e.alu32_imm (Emitter::ALU_ADD, RSI, off);
	}

	if (load) {
		if (sh == 2)
			mem_load (e, 1, true, RSI);
		else {
			e.mov32 (RCX, RSI);
			e.and32_imm (RCX, ~1u);
			mem_load (e, 2, sh == 3, RCX);
		}
		transfer_end (e, inst, true, reg_off, off, 3, out);
		return false;
	}
	load_reg (e, RDI, rd, at, 12);
	e.mov32 (RCX, RSI);
	e.and32_imm (RCX, ~1u);
	mem_store (e, 2, RCX);
	transfer_end (e, inst, false, reg_off, off, 2, out);
	return true;
}

/* LDM and STM without S, the base not r15, a register at least: the lowest
 * register at the lowest address; an LDM's base written back before its
 * loads, an STM's after its stores. True: an STM */
bool
Arm7Jit::emit_multi (Emitter &e, uint32_t inst, uint32_t at, Label &out)
{
	const bool pre = inst & (1u << 24), up = inst & (1u << 23);
	const bool wback = inst & (1u << 21), load = inst & (1u << 20);
	const unsigned rn = (inst >> 16) & 15;
	const uint32_t list = inst & 0xFFFF;
	unsigned n = 0, i;
	int32_t first, final;

	for (i = 0; i < 16; i++)
		n += (list >> i) & 1;
	if (up) {
		first = pre ? 4 : 0;
		final = (int32_t) n * 4;
	} else {
		first = pre ? -(int32_t) n * 4 : -(int32_t) n * 4 + 4;
		final = -(int32_t) n * 4;
	}
	/* esi the address */
	e.load32 (RSI, RBX, off_r[rn]);
	if (first)
		e.alu32_imm (Emitter::ALU_ADD, RSI, first);

	if (load) {
		if (wback) {
			e.load32 (RAX, RBX, off_r[rn]);
			e.alu32_imm (Emitter::ALU_ADD, RAX, final);
			e.store32 (RBX, off_r[rn], RAX);
		}
		for (i = 0; i < 16; i++) {
			if (!(list & (1u << i)))
				continue;
			e.mov32 (RCX, RSI);
			e.and32_imm (RCX, ~3u);
			mem_load (e, 4, false, RCX);
			if (i == 15)
				e.and32_imm (RAX, ~3u);
			e.store32 (RBX, off_r[i], RAX);
			e.alu32_imm (Emitter::ALU_ADD, RSI, 4);
		}
		if (list & 0x8000) {
			add_cycles (e, (int) n + 4);
			e.jmp (out);
		} else
			add_cycles (e, (int) n + 2);
		return false;
	}

	for (i = 0; i < 16; i++) {
		if (!(list & (1u << i)))
			continue;
		/* r15 stored is the instruction plus 12 */
		load_reg (e, RDI, i, at, 12);
		e.mov32 (RCX, RSI);
		e.and32_imm (RCX, ~3u);
		mem_store (e, 4, RCX);
		e.alu32_imm (Emitter::ALU_ADD, RSI, 4);
	}
	if (wback) {
		e.load32 (RAX, RBX, off_r[rn]);
		e.alu32_imm (Emitter::ALU_ADD, RAX, final);
		e.store32 (RBX, off_r[rn], RAX);
	}
	add_cycles (e, (int) n + 1);
	return true;
}

/****************************************************************************
 Branches, the others
****************************************************************************/

void
Arm7Jit::emit_branch (Emitter &e, uint32_t inst, uint32_t at, bool last, Label &top, Label &out)
{
	const uint32_t target = at + 8 + (uint32_t) ((int32_t) (inst << 8) >> 6);

	if (inst & (1u << 24))	/* BL */
		e.store32_imm (RBX, off_r[14], at + 4);
	if (inst == 0x1AFFFFF8) {
		e.mov (RCX, RBX);
		e.mov_imm32 (RDX, target);
		e.call ((const void *) scan_hook);
	}
	add_cycles (e, 3);
	e.store32_imm (RBX, off_r[15], target);
	if (last && target == cur->pc) {
		/* Back to the block's start while the budget lasts and r15 is
		 * not the scan loop's head (the dispatcher's tests: no store
		 * on the way, so no interrupt) */
		Label &away = label ();

		e.alu32_reg_reg (Emitter::ALU_CMP, R14, R15);
		e.jcc (CC_GE, away);
		e.test8_mem_imm (RBX, off_fast_loops, 1);
		e.jcc (CC_E, top);
		e.alu_mem_imm (Emitter::ALU_CMP, RBX, off_scan_head, (int32_t) target);
		e.jcc (CC_NE, top);
		e.bind (away);
	}
	e.jmp (out);
}

/* Through Arm7::execute (), r15 as step () leaves it */
void
Arm7Jit::emit_call (Emitter &e, uint32_t inst, uint32_t at, bool ends, Label &out)
{
	e.store32_imm (RBX, off_r[15], at + 4);
	e.mov (RCX, RBX);
	e.mov_imm32 (RDX, inst);
	e.call ((const void *) execute);
	e.alu32_reg_reg (Emitter::ALU_ADD, R14, RAX);
	if (ends)
		e.jmp (out);
}

/****************************************************************************
 Blocks
****************************************************************************/

Arm7Jit::Block *
Arm7Jit::translate (uint32_t pc)
{
	std::vector<std::pair<Label *, uint32_t>> exits;
	Block *b;
	uint8_t *buf;
	unsigned i, n = 0;

	if (!started || arena.left () < BLOCK_MAX + 256) {
		clear ();
		if (!start_arena ())
			return NULL;
	}
	std::unique_ptr<Block> &owned = blocks[pc];
	if (!owned)
		owned.reset (new Block ());
	b = owned.get ();
	b->pc = pc;
	b->body = NULL;
	while (n < MAX_INSTS && pc + 4 * n < arm->ram_size && arm->ram_size - (pc + 4 * n) >= 4) {
		uint32_t w;
		bool ends;

		memcpy (&w, arm->ram + pc + 4 * n, 4);
		b->words[n++] = w;
		if ((w >> 28) != 15) {
			classify (w, &ends);
			if (ends)
				break;
		}
	}
	b->n = n;
	if (!n)
		return NULL;

	buf = arena.take (BLOCK_MAX, 16);
	if (!buf)
		return NULL;
	Emitter e (buf, BLOCK_MAX);
	labels.clear ();
	cur = b;
	Label &top = label (), &out = label (), &stale = label ();

	/* Its words as translated, else back to run () (r15 the block's) */
	for (i = 0; i < n; i++) {
		e.alu_mem_imm (Emitter::ALU_CMP, R12, (int32_t) (pc + 4 * i), (int32_t) b->words[i]);
		e.jcc (CC_NE, stale);
	}
	e.bind (top);

	for (i = 0; i < n; i++) {
		const uint32_t w = b->words[i], at = pc + 4 * i, cond = w >> 28;
		const bool last = i + 1 == n;
		bool ends = false, store = false;
		Label *skip = NULL;

		if (cond == 15) {
			/* NV: never, a cycle */
			add_cycles (e, 1);
		} else {
			const Kind kind = classify (w, &ends);

			if (cond != 14) {
				skip = &label ();
				cond_test (e, cond, *skip);
			}
			switch (kind) {
			case K_DP: emit_dp (e, w, at, out); break;
			case K_MUL: emit_mul (e, w, at); break;
			case K_SINGLE: store = emit_single (e, w, at, out); break;
			case K_HALF: store = emit_half (e, w, at, out); break;
			case K_MULTI: store = emit_multi (e, w, at, out); break;
			case K_BRANCH: emit_branch (e, w, at, last, top, out); break;
			default: emit_call (e, w, at, ends, out); break;
			}
			if (skip) {
				Label &after = label ();

				/* The instruction ended the block (its jump out),
				 * or goes on past the condition's cycle */
				if (!ends)
					e.jmp (after);
				e.bind (*skip);
				add_cycles (e, 1);
				if (ends) {
					e.store32_imm (RBX, off_r[15], at + 4);
					e.jmp (out);
				}
				e.bind (after);
			}
		}
		if (ends)
			continue;
		if (last) {
			e.store32_imm (RBX, off_r[15], at + 4);
			e.jmp (out);
			break;
		}
		/* To the dispatcher after a store to the bus or to this code,
		 * or once the budget is spent */
		Label &exit = label ();

		exits.push_back (std::make_pair (&exit, at + 4));
		if (store) {
			e.test32 (RBP, RBP);
			e.jcc (CC_NE, exit);
		}
		e.alu32_reg_reg (Emitter::ALU_CMP, R14, R15);
		e.jcc (CC_GE, exit);
	}

	for (auto &x : exits) {
		e.bind (*x.first);
		e.store32_imm (RBX, off_r[15], x.second);
		e.jmp (out);
	}
	e.bind (out);
	e.jmp_abs (dispatch);
	e.bind (stale);
	e.jmp_abs (out_stub);

	if (e.overflowed ()) {
		arena.trim (buf);
		return NULL;
	}
	arena.trim (buf + e.length ());
	b->body = buf;
	cur = NULL;
	return b;
}

/****************************************************************************
 Arm7's side
****************************************************************************/

Arm7::~Arm7 ()
{
	delete translator;
}

int
Arm7::run_translated (int budget)
{
	if (!translator)
		translator = new Arm7Jit (this);
	return translator->run (budget);
}
