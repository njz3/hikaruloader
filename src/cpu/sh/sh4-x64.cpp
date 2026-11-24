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

/* The SH-4 blocks translated to x86-64; see sh4-x64.h.
 *
 * A block's function, Windows' x64 calling convention (ctx in rcx, the
 * block's program counter in edx):
 *
 *	push rbx; push rsi; sub rsp, 40		; 1 + 1 + 7 bytes: the
 *						; unwind information's
 *	mov rbx, rcx; mov esi, edx		; rbx = ctx, esi = pc
 *	for each instruction i, one of:
 *	- an ALU instruction or a bt, bf written out (native_op ()): the
 *	  registers in ctx read and written, T in SR, a bt or bf taken
 *	  setting PC and going out;
 *	- a 32-bit load or store (memory_op ()): the address to ecx, the
 *	  access as the backend's class writes it (load32 (), store32 ()),
 *	  then, not the last, the interrupt check below;
 *	- the FPU's arithmetic in single precision (fpu_op ()), in SSE, its
 *	  handler below when FPSCR.PR is set;
 *	- a jump and its delay slot written out (jump_op (); bt/s and bf/s
 *	  taken), PC its target, then out (the count i + 2); a slot's FPU
 *	  work tested before the jump (FPSCR.PR, SR.FD), the jump's handler
 *	  then;
 *	- any other, its handler:
 *		lea eax, [rsi + 2i]; mov [rbx + PC], eax
 *		mov rcx, rbx; mov edx, inst; call handler
 *		(a branch)	lea eax, [rsi + 2i]; cmp [rbx + PC], eax; jne out_i
 *		(not the last)	cmp byte [rbx + irq_ready], 0; jne out_i
 *	out_i:	PC set (the next one's, or a branch's target), r8d = the
 *		instructions run (i + 1), to the probe, or, to a fixed
 *		address, linked to its block (see "Links"); in essence, to
 *		the end:
 *	end:	sub [rbx + remaining], r8d
 *		mov rcx, rbx; mov edx, esi; call sh4_chain; test rax, rax
 *		jz leave; mov esi, [rbx + PC]; jmp rax	; the next block's body
 *	leave:	mov eax, [rbx + chain.n]; add rsp, 40; pop rsi; pop rbx; ret
 *
 * The branches' handlers leave PC at their target less 2, as for the
 * interpreter's loop: + 2 then. A body (past the prologue, rbx and rsi set)
 * is gone on to with the same frame: the unwind information is the same
 * for all.
 *
 * Registers: the GPRs a block uses are kept in rdi, rbp and r12-r15 (the
 * prologue saves them), six at most, loaded at their first use, the one
 * least used given up for another (Sh4RegCache): a dependent chain of
 * instructions no longer goes through memory at each one. Those changed are
 * written back to ctx at each way out of the block, before a handler's call
 * (then none kept: it may change any), and on an access's slow path (the
 * interpreter reads r15 and PC there); nothing else reads them in ctx while
 * the block runs. */

#include <algorithm>
#include <new>
#include <chrono>

#include "cpu/sh/sh4-x64.h"
#include "cpu/sh/sh4.h"
#include "hl/cpu.h"
#include "hl/mmap.h"
#include "hl/profile.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace x64;

/* The functions' generations, all backends together: a block keeps one
 * of a backend gone, it is not taken for the new one's */
static unsigned generations = 0;

Sh4X64::Sh4X64 (Kind kind, const hl::Mmap *mmap) :
	backend_kind (kind), layout (mmap->layout ()), arena (ARENA_SIZE),
	generation (++generations)
{
	start_arena ();
	clear_near ();
}

Sh4X64 *
Sh4X64::create (hl::Mmap *mmap)
{
#ifdef _WIN32
	if (mmap->fastmem ())
		return new Sh4X64Fastmem (mmap);
	if (mmap->page_table ())
		return new Sh4X64Pages (mmap);
	return new Sh4X64Calls (mmap);
#else
	/* The functions follow Windows' calling convention, and only there
	 * is their unwind information registered */
	(void) mmap;
	return NULL;
#endif
}

bool
Sh4X64::fits (const hl::Mmap *mmap) const
{
	Kind wanted = mmap->fastmem () ? FASTMEM : mmap->page_table () ? PAGES : CALLS;
	return wanted == backend_kind && mmap->layout () == layout;
}

Sh4X64::~Sh4X64 ()
{
	clear ();
}

/* The shared unwind information first: the prologue pushes rbx, rsi, rdi,
 * rbp, r12-r15 (1 byte each, 2 for r12-r15), then takes 40 bytes (Windows'
 * "x64 exception handling" UNWIND_INFO). Then one function for the whole
 * arena: a translation's frame is the same anywhere past its prologue, and
 * only there does it call (or fault, or go on to another's body), so its
 * prologue's codes all apply to any address an unwinding meets. One table
 * registered a translation took a twentieth of VON4's master SH-4 (the
 * driver's NtProtectVirtualMemory each time) */
bool
Sh4X64::start_arena ()
{
	static const uint8_t unwind[] = {
		0x01,		/* version 1, no flags */
		19,		/* prologue size */
		9,		/* unwind codes */
		0x00,		/* no frame register */
		19, 0x42,	/* at 19: UWOP_ALLOC_SMALL, (40 - 8) / 8 */
		12, 0xF0,	/* at 12: UWOP_PUSH_NONVOL r15 */
		10, 0xE0,	/* r14 */
		8, 0xD0,	/* r13 */
		6, 0xC0,	/* r12 */
		4, 0x50,	/* rbp */
		3, 0x70,	/* rdi */
		2, 0x60,	/* rsi */
		1, 0x30,	/* rbx */
		0, 0,		/* padding to an even count */
	};
	uint8_t *at;

	arena.reset ();
	if (!arena.base ())
		return false;
	at = arena.take (sizeof (unwind), 4);
	memcpy (at, unwind, sizeof (unwind));
	unwind_rva = (uint32_t) (at - arena.base ());
#ifdef _WIN32
	RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *) arena.take (sizeof (RUNTIME_FUNCTION), 4);

	if (!rf)
		return false;
	rf->BeginAddress = (DWORD) (arena.top () - arena.base ());
	rf->EndAddress = (DWORD) ARENA_SIZE;
	rf->UnwindData = unwind_rva;
	if (RtlAddFunctionTable (rf, 1, (DWORD64) arena.base ()))
		table = rf;
#endif
	return true;
}

void
Sh4X64::clear ()
{
#ifdef _WIN32
	if (table)
		RtlDeleteFunctionTable ((PRUNTIME_FUNCTION) table);
#endif
	table = NULL;
	num_clears++;
	generation = ++generations;
	linked.clear ();
	cached.clear ();
	dyn_tries.clear ();
	start_arena ();
	clear_near ();
}

/* Links
 *
 * A way out to a fixed address (a bt or bf taken, bra, bsr, the block's
 * end, a loop's way back once the idle loops' looks let it go on) is
 * written
 *
 *	lea eax, [rsi + to]; mov [rbx + PC], eax	(PC set)
 *	mov r8d, count
 *	call link_checks
 *	jmp rel32			(to link; linked, to the body)
 *
 * link_checks (the stubs') takes the count off and makes the probe's
 * checks but the near table's, the slice holding a block of MAX_OPS (else
 * back through the probe's own checks, the way out's call dropped); then
 * sets chain.stores and esi as the probe does, the last ones kept in r11
 * and r10, and r9 the jump's rel32 field (its return address's). Unlinked,
 * the jump goes to the link stub: the near table as the probe reads it;
 * the block there, the rel32 pointed at its body (link ()) and on to it;
 * else chain.stores and esi put back, sh4_chain () (the probe's miss).
 * Linked, it goes to the body at once: no table read, no jump through a
 * register (the probe's look took a sixth of Crazy Taxi's translations'
 * samples; the frames 1 to 6% shorter). The near table's other conditions
 * hold as long as the code epoch stays: unlink () at each new one
 * (Sh4BlockCache's epoch_moved), the arena's clearing drops them all.
 * Under the MMU and with the instruction cache modelled, the probe */
void
Sh4X64::unlink ()
{
	const uint32_t none = 1;

	for (uint8_t *site : linked) {
		const int32_t r = (int32_t) (stubs[0].link - (site + 4));

		memcpy (site, &r, 4);
	}
	linked.clear ();
	for (uint8_t *field : cached)
		memcpy (field, &none, 4);
	cached.clear ();
}

/* A way out to an address known as it runs (rts, jmp @Rn, braf, a
 * handler's branch), past the loops' way back (end):
 *
 *	sub [rbx + remaining], r8d; call link_checks
 *	cmp esi, imm32			(the last address, 1 for none)
 *	jne other
 *	jmp rel32			(its body)
 * other: lea r9, [rip - ...]	(the imm32's address)
 *	jmp rel32			(link_dyn, or look for good)
 *
 * link_dyn: the near table as the probe reads it; the block there, the
 * address and its body kept in the way out (link_dyn ()). A way out that
 * went to DYN_TRIES others (a function's return to its callers) goes to
 * look from then on: the near table's look without a call */
void *
Sh4X64::link_dyn (Sh4X64 *jit, uint8_t *cached_at, void *body, uint32_t pc)
{
	uint8_t *jump = cached_at + DYN_BODY, *stub = cached_at + DYN_STUB;
	const int64_t rel = (int64_t) ((uint8_t *) body - (jump + 4));
	int32_t r = (int32_t) rel;
	uint32_t was;

	if (++jit->dyn_tries[cached_at] > DYN_TRIES) {
		const int64_t to_look = (int64_t) (jit->stubs[0].look - (stub + 4));
		int32_t l = (int32_t) to_look;

		if (to_look == (int64_t) l)
			memcpy (stub, &l, 4);
		return body;
	}
	if (rel != (int64_t) r)
		return body;
	memcpy (&was, cached_at, 4);
	memcpy (jump, &r, 4);
	memcpy (cached_at, &pc, 4);
	if (was == 1)
		jit->cached.push_back (cached_at);
	jit->num_links++;
	return body;
}

void *
Sh4X64::link (Sh4X64 *jit, uint8_t *site, void *body)
{
	const int64_t rel = (int64_t) ((uint8_t *) body - (site + 4));
	int32_t r = (int32_t) rel;

	if (rel == (int64_t) r) {
		memcpy (site, &r, 4);
		jit->linked.push_back (site);
		jit->num_links++;
	}
	return body;
}

/* The ways out all translations share, under the MMU (gen checked) and
 * without: written once a generation, at its start */
void
Sh4X64::print_kinds (char cpu) const
{
	uint64_t total = 0;

	for (const auto &k : kind_bytes)
		total += k.second;
	if (!total)
		return;
	std::vector<std::pair<uint64_t, std::string>> v;
	for (const auto &k : kind_bytes)
		v.push_back (std::make_pair (k.second, k.first));
	std::sort (v.rbegin (), v.rend ());
	HL_PRINT ("sh4 %c: the translations' bytes by their kind of code (%.1f MB):", cpu, total / 1048576.0);
	for (const auto &k : v)
		HL_PRINT ("  %5.1f%%  %s", 100.0 * k.first / total, k.second.c_str ());
}

bool
Sh4X64::emit_stubs (Sh4 *ctx)
{
	const Sh4Offsets o (ctx);
	const bool counting = hl::Profile::enabled;
	const size_t max = 1024;
	unsigned m;

	for (m = 0; m < 2; m++) {
		uint8_t *buf = arena.take (max, 16);
		Label checks, chain, leave;

		if (!buf)
			return false;
		Emitter e (buf, max);
		Stubs &s = stubs[m];
		/* HR_SAMPLE's kinds of code: the stubs' own */
		hl::Profile::Marks stub_marks;
		auto stub_mark = [&] (const char *what) {
			if (hl::Profile::sampling)
				stub_marks.push_back (std::make_pair ((uint32_t) e.length (), what));
		};

		/* The probe: the near table's entry for PC, its translation's
		 * body gone on to when the run loop would do no more
		 * (sh4_chain ()): its code compared in this epoch, the slice
		 * with room for it, no interrupt to take, no idle loop noting,
		 * the MMU off (under it, the entry noted under the same state
		 * of it), the CPU running. Else sh4_chain () (the near table
		 * filled there) */
		s.probe = e.here ();
		stub_mark ("stub: probe");
		e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
		if (counting)
			e.add64_mem_imm (RBX, o.chain_ends, 1);
		s.checks = e.here ();
		stub_mark ("stub: probe's look");
		if (ctx->chain.enabled) {
			e.load32 (RAX, RBX, o.pc);
			e.mov32 (RCX, RAX);
			e.shr32_imm (RCX, 1);
			e.and32_imm (RCX, near_mask);
			e.shl32_imm (RCX, 5);
			e.mov_imm64 (RDX, (uint64_t) (uintptr_t) near_table.get ());
			e.alu64_reg_reg (Emitter::ALU_ADD, RDX, RCX);
			e.cmp32_mem (RDX, 0, RAX);
			e.jcc (CC_NE, chain);
			e.load32 (RCX, RDX, 4);
			e.alu_reg_mem (Emitter::ALU_CMP, RCX, RBX, o.epoch);
			e.jcc (CC_NE, chain);
			e.load32 (RCX, RDX, 8);
			e.cmp32_mem (RBX, o.remaining, RCX);
			e.jcc (CC_LE, chain);
			e.cmp8_mem_imm (RBX, o.irq_ready, 0);
			e.jcc (CC_NE, chain);
			e.cmp8_mem_imm (RBX, o.noting, 0);
			e.jcc (CC_NE, chain);
			if (m) {
				/* The translations at translated addresses look
				 * at the gen noted */
				e.load32 (RCX, RDX, 12);
				e.alu_reg_mem (Emitter::ALU_CMP, RCX, RBX, o.mmu_gen);
				e.jcc (CC_NE, chain);
				e.store32 (RBX, o.mmu_gen_entry, RCX);
			} else {
				e.cmp8_mem_imm (RBX, o.mmu_on, 0);
				e.jcc (CC_NE, chain);
			}
			e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.state, (int32_t) hl::CPU_STATE_RUN);
			e.jcc (CC_NE, chain);
			e.load64 (RCX, RBX, o.stores);
			e.store64 (RBX, o.chain_stores, RCX);
			if (counting)
				e.add64_mem_imm (RBX, o.chain_gone, 1);
			e.mov32 (RSI, RAX);
			e.load64 (RAX, RDX, 16);
			stub_mark ("stub: probe's jump");
			e.jmp_reg (RAX);
		}
		/* The links' checks and the link (see "Links") */
		s.link_checks = s.link = NULL;
		if (m == 0 && ctx->chain.enabled) {
			Label fail, miss;

			s.link_checks = e.here ();
			stub_mark ("stub: link checks");
			e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
			e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.remaining, (int32_t) Sh4Block::MAX_OPS);
			e.jcc (CC_LE, fail);
			e.cmp8_mem_imm (RBX, o.irq_ready, 0);
			e.jcc (CC_NE, fail);
			e.cmp8_mem_imm (RBX, o.noting, 0);
			e.jcc (CC_NE, fail);
			e.cmp8_mem_imm (RBX, o.mmu_on, 0);
			e.jcc (CC_NE, fail);
			e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.state, (int32_t) hl::CPU_STATE_RUN);
			e.jcc (CC_NE, fail);
			e.load64 (R11, RBX, o.chain_stores);
			e.load64 (RCX, RBX, o.stores);
			e.store64 (RBX, o.chain_stores, RCX);
			if (counting)
				e.add64_mem_imm (RBX, o.chain_gone, 1);
			e.mov32 (R10, RSI);
			e.load32 (RSI, RBX, o.pc);
			/* The way out's jump (unlinked: to link, which wants
			 * its rel32 field) */
			e.load64 (R9, RSP, 0);
			e.add64_imm (R9, 1);
			e.ret ();
			e.bind (fail);
			e.add64_imm (RSP, 8);
			e.jmp_abs (s.checks);

			s.link = e.here ();
			stub_mark ("stub: link");
			e.mov32 (RAX, RSI);
			e.mov32 (RCX, RAX);
			e.shr32_imm (RCX, 1);
			e.and32_imm (RCX, near_mask);
			e.shl32_imm (RCX, 5);
			e.mov_imm64 (RDX, (uint64_t) (uintptr_t) near_table.get ());
			e.alu64_reg_reg (Emitter::ALU_ADD, RDX, RCX);
			e.cmp32_mem (RDX, 0, RAX);
			e.jcc (CC_NE, miss);
			e.load32 (RCX, RDX, 4);
			e.alu_reg_mem (Emitter::ALU_CMP, RCX, RBX, o.epoch);
			e.jcc (CC_NE, miss);
			e.load32 (RCX, RDX, 8);
			e.cmp32_mem (RBX, o.remaining, RCX);
			e.jcc (CC_LE, miss);
			e.load64 (R8, RDX, 16);
			e.mov (RDX, R9);
			e.mov_imm64 (RCX, (uint64_t) (uintptr_t) this);
			e.call ((const void *) &Sh4X64::link);
			e.jmp_reg (RAX);
			e.bind (miss);
			e.store64 (RBX, o.chain_stores, R11);
			e.mov32 (RSI, R10);
			if (counting)
				e.add64_mem_imm (RBX, o.chain_gone, -1);
			e.jmp (chain);

			/* link_dyn and look: the near table's entry for esi
			 * (rdx), else back as link's miss */
			auto near_entry = [&] (Label &away) {
				e.mov32 (RAX, RSI);
				e.mov32 (RCX, RAX);
				e.shr32_imm (RCX, 1);
				e.and32_imm (RCX, near_mask);
				e.shl32_imm (RCX, 5);
				e.mov_imm64 (RDX, (uint64_t) (uintptr_t) near_table.get ());
				e.alu64_reg_reg (Emitter::ALU_ADD, RDX, RCX);
				e.cmp32_mem (RDX, 0, RAX);
				e.jcc (CC_NE, away);
				e.load32 (RCX, RDX, 4);
				e.alu_reg_mem (Emitter::ALU_CMP, RCX, RBX, o.epoch);
				e.jcc (CC_NE, away);
				e.load32 (RCX, RDX, 8);
				e.cmp32_mem (RBX, o.remaining, RCX);
				e.jcc (CC_LE, away);
			};

			s.link_dyn = e.here ();
			stub_mark ("stub: link (address as run)");
			near_entry (miss);
			e.load64 (R8, RDX, 16);
			e.mov (RDX, R9);
			e.mov32 (R9, RSI);
			e.mov_imm64 (RCX, (uint64_t) (uintptr_t) this);
			e.call ((const void *) &Sh4X64::link_dyn);
			e.jmp_reg (RAX);

			s.look = e.here ();
			stub_mark ("stub: look (address as run)");
			near_entry (miss);
			e.load64 (RAX, RDX, 16);
			e.jmp_reg (RAX);
		}

		/* sh4_chain (): the next block's body, or back to the run loop
		 * with the last block's count */
		e.bind (chain);
		s.chain = e.here ();
		stub_mark ("stub: sh4_chain call");
		e.mov (RCX, RBX);
		e.mov32 (RDX, RSI);
		e.call ((const void *) sh4_chain);
		e.test64 (RAX, RAX);
		e.jcc (CC_E, leave);
		e.load32 (RSI, RBX, o.pc);
		e.jmp_reg (RAX);
		e.bind (leave);
		stub_mark ("stub: back to the run loop");
		e.load32 (RAX, RBX, o.chain_n);
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
		if (hl::Profile::sampling)
			hl::Profile::code_written (buf, e.length (), ctx->config.master ? 'M' : 'S', 0, NULL, 0,
			                           &stub_marks);
	}
	stubs_generation = generation;
	return true;
}

void
Sh4X64::clear_near ()
{
	for (unsigned i = 0; i < (1u << NEAR_BITS); i++)
		near_table[i] = Near { 1, 0, 0, 0, NULL, NULL };
}

void
Sh4X64::note (uint32_t pc, const Sh4Block *block, uint32_t epoch, uint32_t gen)
{
	Near &n = near_table[(pc >> 1) & near_mask];

	if (!block->checkable || block->checked != epoch || !body (block))
		return;
	/* Its address as run (under the MMU, a translated one, which the
	 * MMU's state maps to the block: gen) */
	n.pc = pc;
	n.gen = gen;
	n.checked = block->checked;
	n.count = (int32_t) block->ops.size ();
	n.body = block->native_body;
	n.native = (Sh4Native) block->native;
}

Sh4Offsets::Sh4Offsets (Sh4 *ctx)
{
	const uint8_t *base = (const uint8_t *) ctx;

	pc = (int32_t) ((const uint8_t *) &ctx->regs.pc - base);
	pr = (int32_t) ((const uint8_t *) &ctx->regs.pr - base);
	macl = (int32_t) ((const uint8_t *) &ctx->regs.mac.field.lo - base);
	mach = (int32_t) ((const uint8_t *) &ctx->regs.mac.field.hi - base);
	target = (int32_t) ((const uint8_t *) &ctx->jit_target - base);
	chain_n = (int32_t) ((const uint8_t *) &ctx->chain.n - base);
	epoch = (int32_t) ((const uint8_t *) &ctx->blocks.epoch - base);
	state = (int32_t) ((const uint8_t *) &ctx->state - base);
	mmu_on = (int32_t) ((const uint8_t *) &ctx->mmu_exact - base);
	noting = (int32_t) ((const uint8_t *) ctx->idle.noting_flag () - base);
	chain_stores = (int32_t) ((const uint8_t *) &ctx->chain.stores - base);
	chain_ends = (int32_t) ((const uint8_t *) &ctx->chain.ends - base);
	chain_gone = (int32_t) ((const uint8_t *) &ctx->chain.gone - base);
	icache = (int32_t) ((const uint8_t *) ctx->blocks.icache.tags - base);
	icache_held = (int32_t) ((const uint8_t *) ctx->blocks.icache.held - base);
	icache_data = (int32_t) ((const uint8_t *) ctx->blocks.icache.data - base);
	icache_fetched = (int32_t) ((const uint8_t *) &ctx->blocks.icache.fetched - base);
	held_epoch = (int32_t) ((const uint8_t *) ctx->blocks.held_epoch_at () - base);
	jit_index = (int32_t) ((const uint8_t *) &ctx->jit_index - base);
	in_slot = (int32_t) ((const uint8_t *) &ctx->in_slot - base);
	slot_pc = (int32_t) ((const uint8_t *) &ctx->slot_pc - base);
	sq = (int32_t) ((const uint8_t *) ctx->sq - base);
	jit_tables = (int32_t) ((const uint8_t *) ctx->jit_tables - base);
	mmu_gen = (int32_t) ((const uint8_t *) &ctx->mmu_gen - base);
	mmu_gen_entry = (int32_t) ((const uint8_t *) &ctx->mmu_gen_entry - base);
	sr = (int32_t) ((const uint8_t *) &ctx->regs.sr - base);
	gbr = (int32_t) ((const uint8_t *) &ctx->regs.gbr - base);
	remaining = (int32_t) ((const uint8_t *) &ctx->remaining - base);
	irq_ready = (int32_t) ((const uint8_t *) &ctx->irq_ready - base);
	stores = (int32_t) ((const uint8_t *) &ctx->stores - base);
	far_stores = (int32_t) ((const uint8_t *) &ctx->far_stores - base);
	for (unsigned i = 0; i < 16; i++)
		r[i] = (int32_t) ((const uint8_t *) &ctx->regs.r[i] - base);
	rbank = (int32_t) ((const uint8_t *) &ctx->regs.rbank[0] - base);
	fr = (int32_t) ((const uint8_t *) &ctx->regs.f - base);
	xf = (int32_t) ((const uint8_t *) &ctx->regs.x - base);
	fpscr = (int32_t) ((const uint8_t *) &ctx->regs.fpscr - base);
	fpul = (int32_t) ((const uint8_t *) &ctx->regs.fpul - base);
}

/* Registers (above) */

static const Reg cached[Sh4RegCache::NUM] = { R12, R13, R14, R15, RDI, RBP };

void
Sh4RegCache::reset (const Sh4Offsets *offsets)
{
	o = offsets;
	forget ();
	clock = 0;
}

void
Sh4RegCache::forget ()
{
	for (unsigned k = 0; k < NUM; k++) {
		held[k] = -1;
		dirty[k] = false;
		used[k] = 0;
	}
}

int
Sh4RegCache::take (Emitter &e, unsigned n, bool load)
{
	unsigned k, pick = 0;

	for (k = 0; k < NUM; k++)
		if (held[k] == (int8_t) n) {
			used[k] = ++clock;
			return (int) k;
		}
	/* A free one, else the one least used, written back first */
	for (k = 0; k < NUM; k++) {
		if (held[k] < 0) {
			pick = k;
			break;
		}
		if (used[k] < used[pick])
			pick = k;
	}
	if (held[pick] >= 0 && dirty[pick])
		put_back (e, cached[pick], held[pick]);
	held[pick] = (int8_t) n;
	dirty[pick] = false;
	used[pick] = ++clock;
	if (load) {
		if (n == T) {
			e.load32 (cached[pick], RBX, o->sr);
			e.and32_imm (cached[pick], 1);
		} else
			e.load32 (cached[pick], RBX, o->r[n]);
	}
	return (int) pick;
}

void
Sh4RegCache::put_back (Emitter &e, Reg r, int n) const
{
	if (n == (int) T) {
		e.alu_mem_imm (Emitter::ALU_AND, RBX, o->sr, ~1);
		e.alu_mem_reg (Emitter::ALU_OR, RBX, o->sr, r);
	} else
		e.store32 (RBX, o->r[n], r);
}

void
Sh4RegCache::t_to_sr (Emitter &e)
{
	for (unsigned k = 0; k < NUM; k++)
		if (held[k] == (int8_t) T && dirty[k]) {
			put_back (e, cached[k], T);
			dirty[k] = false;
		}
}

void
Sh4RegCache::t_drop ()
{
	for (unsigned k = 0; k < NUM; k++)
		if (held[k] == (int8_t) T) {
			held[k] = -1;
			dirty[k] = false;
		}
}

Reg
Sh4RegCache::read (Emitter &e, unsigned n)
{
	return cached[take (e, n, true)];
}

Reg
Sh4RegCache::write (Emitter &e, unsigned n)
{
	int k = take (e, n, false);

	dirty[k] = true;
	return cached[k];
}

Reg
Sh4RegCache::modify (Emitter &e, unsigned n)
{
	int k = take (e, n, true);

	dirty[k] = true;
	return cached[k];
}

bool
Sh4RegCache::peek (unsigned n, Reg &r) const
{
	for (unsigned k = 0; k < NUM; k++)
		if (held[k] == (int8_t) n) {
			r = cached[k];
			return true;
		}
	return false;
}

void
Sh4RegCache::store_dirty (Emitter &e) const
{
	for (unsigned k = 0; k < NUM; k++)
		if (held[k] >= 0 && dirty[k])
			put_back (e, cached[k], held[k]);
}

void
Sh4RegCache::flush (Emitter &e)
{
	store_dirty (e);
	for (unsigned k = 0; k < NUM; k++)
		dirty[k] = false;
}

Sh4RegCache::Spills
Sh4RegCache::spills () const
{
	Spills v;

	/* T's place: SR's (put back with an and and an or) */
	for (unsigned k = 0; k < NUM; k++)
		if (held[k] >= 0 && dirty[k])
			v.r[v.n++] = std::make_pair (cached[k], held[k] == (int8_t) T ? o->sr : o->r[held[k]]);
	return v;
}

/* Under the MMU, before a call that may take an exception: the instruction
 * it is (a delay slot's: its branch, the slot's address kept as
 * delay_slot () keeps it, in_slot set until the call is back) */
void
Sh4X64::call_index (Emitter &e, const Sh4Offsets &o, int32_t at, bool slot)
{
	if (!mmu_mode)
		return;
	e.store32_imm (RBX, o.jit_index, (uint32_t) (at / 2 - (slot ? 1 : 0)));
	if (slot) {
		e.store8_imm (RBX, o.in_slot, 1);
		e.lea32 (RAX, RSI, at);
		e.store32 (RBX, o.slot_pc, RAX);
	}
}

void
Sh4X64::mark (Emitter &e, const char *what)
{
	if (hl::Profile::sampling)
		marks.push_back (std::make_pair ((uint32_t) e.length (), what));
}

/* The memory accesses */

/* A store away from the stack (ecx its address, beyond 512 bytes of r15),
 * counted as sh4_put () does: r15 where the block keeps it */
void
Sh4X64::count_far (Emitter &e, const Sh4Offsets &o)
{
	Label on_stack;
	Reg sp;

	e.mov32 (RAX, RCX);
	if (rc.peek (15, sp))
		e.alu32_reg_reg (Emitter::ALU_SUB, RAX, sp);
	else
		e.alu_reg_mem (Emitter::ALU_SUB, RAX, RBX, o.r[15]);
	e.lea32 (RAX, RAX, 0x200);
	e.cmp32_imm (RAX, 0x400);
	e.jcc (CC_B, on_stack);
	e.add64_mem_imm (RBX, o.far_stores, 1);
	e.bind (on_stack);
}

void
Sh4X64::count_store (Emitter &e, const Sh4Offsets &o)
{
	if (!store_counted) {
		e.add64_mem_imm (RBX, o.stores, 1);
		store_counted = true;
	}
	if (store_base != 15)
		count_far (e, o);
}

/* The slow paths: the program counter set, then the interpreter's own
 * access (ecx the address, r10 the value) */
void
Sh4X64::call_read (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size, bool slot)
{
	/* Sh4::hot_read's register (the multiboard port's data, read in a
	 * loop): its function called here, as sh4_read () would */
	const bool hot = hot_fn && !mmu_mode && size <= 4;
	Label normal, done;

	e.lea32 (R11, RSI, at);
	e.store32 (RBX, o.pc, R11);
	if (hot) {
		e.cmp32_imm (RCX, 0xE0000000);
		e.jcc (CC_AE, normal);
		e.mov32 (RAX, RCX);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.cmp32_imm (RAX, hot_addr);
		e.jcc (CC_NE, normal);
		e.mov_imm64 (RCX, (uint64_t) (uintptr_t) hot_obj);
		e.call ((const void *) hot_fn);
		if (size == 2)
			e.movsx16 (RAX, RAX);
		else if (size == 1)
			e.movsx8 (RAX, RAX);
		e.jmp (done);
		e.bind (normal);
	}
	call_index (e, o, at, slot);
	e.mov32 (RDX, RCX);
	e.mov (RCX, RBX);
	switch (size) {
	case 1: e.call ((const void *) sh4_read8s); break;
	case 2: e.call ((const void *) sh4_read16s); break;
	case 4: e.call ((const void *) sh4_read32); break;
	default: e.call ((const void *) sh4_read64); break;
	}
	if (hot)
		e.bind (done);
	if (slot && mmu_mode)
		e.store8_imm (RBX, o.in_slot, 0);
}

/* A store queue's word (E0000000-E3FFFFFF), aligned: into Sh4::sq, the
 * stores counted (away from the stack) as sh4_put () counts them; no code
 * there (no epoch); under the MMU, privileged alone. Else the
 * interpreter's access */
void
Sh4X64::sq_store (Emitter &e, const Sh4Offsets &o, unsigned size, Label &back)
{
	Label &other = label ();

	e.lea32 (RAX, RCX, 0x20000000);
	e.cmp32_imm (RAX, 0x04000000);
	e.jcc (CC_AE, other);
	/* Under the MMU, privileged: no translation, no fault (a user mode's
	 * SQMD left to the interpreter's access) */
	if (mmu_mode) {
		e.test_mem_imm (RBX, o.sr, 0x40000000);
		e.jcc (CC_E, other);
	}
	if (size > 1) {
		e.test32_imm (RCX, size - 1);
		e.jcc (CC_NE, other);
	}
	e.mov32 (RAX, RCX);
	e.and32_imm (RAX, 0x3F);
	e.add64_imm (RAX, o.sq);
	switch (size) {
	case 1: e.store8_index (RBX, RAX, 1, R10); break;
	case 2: e.store16_index (RBX, RAX, 1, R10); break;
	case 4: e.store32_index (RBX, RAX, 1, R10); break;
	default: e.store64_index (RBX, RAX, 1, R10); break;
	}
	e.add64_mem_imm (RBX, o.stores, 1);
	e.add64_mem_imm (RBX, o.far_stores, 1);
	e.jmp (back);
	e.bind (other);
}

void
Sh4X64::call_write (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size, bool slot)
{
	e.lea32 (R11, RSI, at);
	e.store32 (RBX, o.pc, R11);
	call_index (e, o, at, slot);
	e.mov (R8, R10);
	e.mov32 (RDX, RCX);
	e.mov (RCX, RBX);
	switch (size) {
	case 1: e.call ((const void *) sh4_write8); break;
	case 2: e.call ((const void *) sh4_write16); break;
	case 4: e.call ((const void *) sh4_write32); break;
	default: e.call ((const void *) sh4_write64); break;
	}
	if (slot && mmu_mode)
		e.store8_imm (RBX, o.in_slot, 0);
}

void
Sh4X64Calls::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	rc.store_dirty (e);
	call_read (e, o, at, size, in_slot);
}

void
Sh4X64Calls::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	rc.store_dirty (e);
	call_write (e, o, at, size, in_slot);
}

/* The memory map's address of an access the code may make itself, to edx:
 * below the P3 area and the on-chip registers (sh4_get () checks them,
 * then takes 29 bits), aligned (an access across two pages never) */
static void
straight_address (Emitter &e, Label &slow, unsigned size)
{
	e.cmp32_imm (RCX, 0xC0000000);
	e.jcc (CC_AE, slow);
	if (size > 1) {
		e.test32_imm (RCX, size - 1);
		e.jcc (CC_NE, slow);
	}
	e.mov32 (RDX, RCX);
	e.and32_imm (RDX, 0x1FFFFFFF);
}

/* The access at r9 + index (rdx, or rcx with the page tables' entries):
 * a load to eax (sign extended) or rax, a store of r10 */
static void
access_load (Emitter &e, unsigned size, Reg index = RDX)
{
	switch (size) {
	case 1: e.load8s_index (RAX, R9, index, 1); break;
	case 2: e.load16s_index (RAX, R9, index, 1); break;
	case 4: e.load32_index (RAX, R9, index, 1); break;
	default: e.load64_index (RAX, R9, index, 1); break;
	}
}

static void
access_store (Emitter &e, unsigned size, Reg index = RDX)
{
	switch (size) {
	case 1: e.store8_index (R9, index, 1, R10); break;
	case 2: e.store16_index (R9, index, 1, R10); break;
	case 4: e.store32_index (R9, index, 1, R10); break;
	default: e.store64_index (R9, index, 1, R10); break;
	}
}

Sh4X64Pages::Sh4X64Pages (const hl::Mmap *mmap) :
	Sh4X64 (PAGES, mmap),
	read_table (mmap->wide_read_table ()), write_table (mmap->wide_write_table ())
{
}

/* Under the MMU: ecx in P1 or P2 and SR.MD set, else slow (the TLB) */
void
Sh4X64::mmu_guard (Emitter &e, const Sh4Offsets &o, Label &slow)
{
	e.lea32 (RAX, RCX, (int32_t) 0x80000000);
	e.cmp32_imm (RAX, 0x40000000);
	e.jcc (CC_AE, slow);
	e.test_mem_imm (RBX, o.sr, 0x40000000);
	e.jcc (CC_E, slow);
}

void
Sh4X64::stamp_check (Emitter &e, const Sh4Offsets &o, Label &slow)
{
	e.load64 (R8, RBX, o.jit_tables + 16);
	e.load32_index (R11, R8, RAX, 4);
	e.alu_reg_mem (Emitter::ALU_CMP, R11, RBX, o.epoch);
	e.jcc (CC_AE, slow);
}

/* r9 = the page's entry in the table over the 32-bit addresses
 * (Mmap::wide_read_table (): none at P3 and P4, the aliases the same, so
 * no test of the area and no 29-bit mask, three instructions less an
 * access), its host address less its own: the access at r9 + rcx. The
 * table read through ctx (Sh4::jit_tables). Aligned (an access across two
 * pages never). A store to code compared in this epoch, the slow path
 * (sh4_write () moves the epoch) */
void
Sh4X64Pages::lookup (Emitter &e, const Sh4Offsets &o, const intptr_t *table, Label &slow,
                     unsigned size, bool store)
{
	mark (e, store ? "store: alignment" : "load: alignment");
	if (size > 1) {
		e.test8_imm (RCX, (uint8_t) (size - 1));
		e.jcc (CC_NE, slow);
	}
	if (mmu_mode)
		mmu_guard (e, o, slow);
	if (store) {
		mark (e, "store: code stamp");
		e.mov32 (RAX, RCX);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, slow);
	}
	mark (e, store ? "store: page" : "load: page");
	(void) table;
	e.mov32 (RAX, RCX);
	e.shr32_imm (RAX, hl::Mmap::TABLE_PAGE_BITS);
	e.load64 (R8, RBX, o.jit_tables + (store ? 8 : 0));
	e.load64_index (R9, R8, RAX, 8);
	e.test64 (R9, R9);
	e.jcc (CC_E, slow);
}

void
Sh4X64Pages::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	lookup (e, o, read_table, slow, size, false);
	mark (e, "load: access");
	access_load (e, size, RCX);
	e.bind (back);
	slow_path (slow, back, false, at, size);
	mark (e, "memory: after");
}

void
Sh4X64Pages::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	lookup (e, o, write_table, slow, size, true);
	mark (e, "store: access");
	access_store (e, size, RCX);
	mark (e, "store: counts");
	count_store (e, o);
	e.bind (back);
	slow_path (slow, back, true, at, size);
	mark (e, "memory: after");
}

Sh4X64Fastmem::Sh4X64Fastmem (const hl::Mmap *mmap) :
	Sh4X64 (FASTMEM, mmap),
	base (mmap->fastmem_base ()), readable (mmap->fastmem_readable ()),
	writable (mmap->fastmem_writable ())
{
}

/* edx the address, its 64 KB granule's bit set in the map; r9 the base. A
 * store to code compared in this epoch, the slow path */
void
Sh4X64Fastmem::check (Emitter &e, const Sh4Offsets &o, const uint8_t *bits, Label &slow,
                      unsigned size, bool store)
{
	if (mmu_mode)
		mmu_guard (e, o, slow);
	straight_address (e, slow, size);
	if (store) {
		e.mov32 (RAX, RDX);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, slow);
	}
	e.mov32 (RAX, RDX);
	e.shr32_imm (RAX, 16);
	e.mov_imm64 (R8, (uint64_t) (uintptr_t) bits);
	e.bt_mem (R8, RAX);
	e.jcc (CC_AE, slow);		/* CF clear */
	e.mov_imm64 (R9, (uint64_t) (uintptr_t) base);
}

void
Sh4X64Fastmem::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	check (e, o, readable, slow, size, false);
	access_load (e, size);
	e.bind (back);
	slow_path (slow, back, false, at, size);
}

void
Sh4X64Fastmem::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	check (e, o, writable, slow, size, true);
	access_store (e, size);
	count_store (e, o);
	e.bind (back);
	slow_path (slow, back, true, at, size);
}

/* The FPU's moves (fmov, sh-insns-interp.h): 32 bits, a single, unless
 * FPSCR.SZ, 64 bits, a pair; the singles at n ^ 1 (FR ()), a pair's words
 * swapped (MOVE64), XDn for an odd n. The size move_sz says, or both, SZ
 * tested as they run; the GPRs in their host registers before, the same
 * for both sizes */
bool
Sh4X64::fpu_move_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	const unsigned kind = inst & 0xF00F;
	Label pair, done;
	Reg base = RAX, index = RAX;
	unsigned sz;

	if (kind < 0xF006 || kind > 0xF00C)
		return false;

	store_base = (kind == 0xF00A || kind == 0xF00B) ? (int) n : -1;
	auto single = [&] (unsigned r) { return o.fr + 4 * (int32_t) (r ^ 1); };
	auto pair_at = [&] (unsigned r) { return ((r & 1) ? o.xf : o.fr) + 8 * (int32_t) (r >> 1); };

	switch (kind) {
	case 0xF008: base = rc.read (e, m); break;
	case 0xF009: base = rc.modify (e, m); break;
	case 0xF006: base = rc.read (e, m); index = rc.read (e, 0); break;
	case 0xF00A: base = rc.read (e, n); break;
	case 0xF00B: base = rc.modify (e, n); break;
	case 0xF007: base = rc.read (e, n); index = rc.read (e, 0); break;
	}

	if (move_sz < 0) {
		e.test_mem_imm (RBX, o.fpscr, 0x00100000);
		e.jcc (CC_NE, pair);
	}
	for (sz = 0; sz < 2; sz++) {
		const unsigned size = sz ? 8 : 4;

		if (move_sz >= 0 && (int) sz != move_sz)
			continue;
		if (sz && move_sz < 0)
			e.bind (pair);
		switch (kind) {
		case 0xF008:			/* fmov @Rm,FRn */
		case 0xF009:			/* fmov @Rm+,FRn */
		case 0xF006:			/* fmov @(R0,Rm),FRn */
			e.mov32 (RCX, base);
			if (kind == 0xF006)
				e.alu32_reg_reg (Emitter::ALU_ADD, RCX, index);
			load (e, o, at, size);
			if (sz) {
				e.ror64_imm (RAX, 32);
				e.store64 (RBX, pair_at (n), RAX);
			} else
				e.store32 (RBX, single (n), RAX);
			if (kind == 0xF009)
				e.alu32_imm (Emitter::ALU_ADD, base, (int32_t) size);
			break;
		case 0xF00A:			/* fmov FRm,@Rn */
		case 0xF00B:			/* fmov FRm,@-Rn */
		case 0xF007:			/* fmov FRm,@(R0,Rn) */
			e.mov32 (RCX, base);
			if (kind == 0xF007)
				e.alu32_reg_reg (Emitter::ALU_ADD, RCX, index);
			if (kind == 0xF00B)
				e.lea32 (RCX, RCX, -(int32_t) size);
			if (sz) {
				e.load64 (R10, RBX, pair_at (m));
				e.ror64_imm (R10, 32);
			} else
				e.load32 (R10, RBX, single (m));
			store (e, o, at, size);
			if (kind == 0xF00B)
				e.alu32_imm (Emitter::ALU_ADD, base, -(int32_t) size);
			break;
		default:			/* fmov FRm,FRn */
			if (sz) {
				e.load64 (RAX, RBX, pair_at (m));
				e.store64 (RBX, pair_at (n), RAX);
			} else {
				e.load32 (RAX, RBX, single (m));
				e.store32 (RBX, single (n), RAX);
			}
			break;
		}
		if (!sz && move_sz < 0)
			e.jmp (done);
	}
	e.bind (done);
	return true;
}

/* A PC-relative load's host address, read at translation: in RAM or ROM
 * (as sh4_read () finds it), for the memory map translated for (a new one,
 * a new backend: the translations dropped). NULL for the others, which go
 * the way of the other loads: the operand cache's RAM, a device, the code
 * fetched outside RAM and ROM, a misaligned one */
const uint8_t *
Sh4X64::literal (uint32_t addr, unsigned size) const
{
	if (!translating || (addr & 0x1FFFFFFF) >= 0x1C000000 || (addr & (size - 1)))
		return NULL;
	return translating->direct (addr & 0x1FFFFFFF, false);
}

/* The moves to and from memory, as their handlers have them
 * (sh-insns-interp.h): the address to ecx, the value to r10, the access,
 * the registers after. false for the others */
bool
Sh4X64::memory_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at, uint32_t pc)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	/* mov.b, mov.w, mov.l: the size from the low bits */
	static const unsigned sizes[4] = { 1, 2, 4, 4 };
	const unsigned size = sizes[inst & 3];

	if ((inst & 0xF000) == 0xF000) {
		/* fmov FRm,FRn: no access */
		if ((inst & 0xF00F) == 0xF00C)
			no_device = true;
		return fpu_move_op (e, o, inst, at);
	}

	/* ecx = Rb (+ R0) + disp; r10 = Rv; Rd = eax; Rb += k */
	store_base = -1;
	auto address = [&] (unsigned b, int32_t disp, bool indexed) {
		store_base = indexed ? -1 : (int) b;
		e.mov32 (RCX, rc.read (e, b));
		if (indexed)
			e.alu32_reg_reg (Emitter::ALU_ADD, RCX, rc.read (e, 0));
		if (disp)
			e.lea32 (RCX, RCX, disp);
	};
	auto value = [&] (unsigned v) { e.mov32 (R10, rc.read (e, v)); };
	auto result = [&] (unsigned d) { e.mov32 (rc.write (e, d), RAX); };
	auto step = [&] (unsigned b, int32_t k) { e.alu32_imm (Emitter::ALU_ADD, rc.modify (e, b), k); };

	switch (inst & 0xF00F) {
	case 0x6000:				/* mov.b @Rm,Rn */
	case 0x6001:				/* mov.w @Rm,Rn */
	case 0x6002:				/* mov.l @Rm,Rn */
		address (m, 0, false);
		load (e, o, at, size);
		result (n);
		return true;
	case 0x6004:				/* mov.b @Rm+,Rn */
	case 0x6005:				/* mov.w @Rm+,Rn */
	case 0x6006:				/* mov.l @Rm+,Rn */
		address (m, 0, false);
		load (e, o, at, size);
		result (n);
		if (n != m)
			step (m, (int32_t) size);
		return true;
	case 0x2000:				/* mov.b Rm,@Rn */
	case 0x2001:				/* mov.w Rm,@Rn */
	case 0x2002:				/* mov.l Rm,@Rn */
		address (n, 0, false);
		value (m);
		store (e, o, at, size);
		return true;
	case 0x2004:				/* mov.b Rm,@-Rn */
	case 0x2005:				/* mov.w Rm,@-Rn */
	case 0x2006:				/* mov.l Rm,@-Rn */
		address (n, -(int32_t) size, false);
		value (m);
		store (e, o, at, size);
		step (n, -(int32_t) size);
		return true;
	case 0x000C:				/* mov.b @(R0,Rm),Rn */
	case 0x000D:				/* mov.w @(R0,Rm),Rn */
	case 0x000E:				/* mov.l @(R0,Rm),Rn */
		address (m, 0, true);
		load (e, o, at, size);
		result (n);
		return true;
	case 0x0004:				/* mov.b Rm,@(R0,Rn) */
	case 0x0005:				/* mov.w Rm,@(R0,Rn) */
	case 0x0006:				/* mov.l Rm,@(R0,Rn) */
		address (n, 0, true);
		value (m);
		store (e, o, at, size);
		return true;
	}
	switch (inst & 0xF000) {
	case 0x5000:				/* mov.l @(disp,Rm),Rn */
		address (m, (inst & 15) * 4, false);
		load (e, o, at, 4);
		result (n);
		return true;
	case 0x1000:				/* mov.l Rm,@(disp,Rn) */
		address (n, (inst & 15) * 4, false);
		value (m);
		store (e, o, at, 4);
		return true;
	case 0xD000: {				/* mov.l @(disp,PC),Rn */
		/* ((PC + 4) & ~3) + disp * 4, PC's low bits the block's own */
		int32_t from_esi = (int32_t) (((pc + 4) & ~3u) - (pc - (uint32_t) at)) +
		                   (int32_t) ((inst & 0xFF) * 4);
		const uint8_t *host = virt_mode ? NULL : literal (pc + (uint32_t) from_esi - (uint32_t) at, 4);

		if (host) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) host);
			e.load32 (RAX, RAX, 0);
			no_device = true;
		} else {
			e.lea32 (RCX, RSI, from_esi);
			load (e, o, at, 4);
		}
		result (n);
		return true;
	}
	case 0x9000: {				/* mov.w @(disp,PC),Rn */
		int32_t from_esi = at + 4 + (int32_t) ((inst & 0xFF) * 2);
		const uint8_t *host = virt_mode ? NULL : literal (pc + (uint32_t) from_esi - (uint32_t) at, 2);

		if (host) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) host);
			e.movsx16_mem (RAX, RAX, 0);
			no_device = true;
		} else {
			e.lea32 (RCX, RSI, from_esi);
			load (e, o, at, 2);
		}
		result (n);
		return true;
	}
	}
	switch (inst & 0xFF00) {
	case 0xC600:				/* mov.l @(disp,GBR),R0 */
	case 0xC500:				/* mov.w @(disp,GBR),R0 */
	case 0xC400: {				/* mov.b @(disp,GBR),R0 */
		const unsigned s = 1u << ((inst >> 8) & 3);
		e.load32 (RCX, RBX, o.gbr);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 0xFF) * s));
		load (e, o, at, s);
		result (0);
		return true;
	}
	case 0xC200:				/* mov.l R0,@(disp,GBR) */
	case 0xC100:				/* mov.w R0,@(disp,GBR) */
	case 0xC000: {				/* mov.b R0,@(disp,GBR) */
		const unsigned s = 1u << ((inst >> 8) & 3);
		e.load32 (RCX, RBX, o.gbr);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 0xFF) * s));
		value (0);
		store (e, o, at, s);
		return true;
	}
	case 0x8500:				/* mov.w @(disp,Rm),R0 */
	case 0x8400: {				/* mov.b @(disp,Rm),R0 */
		const unsigned s = (inst & 0x0100) ? 2 : 1;
		address (m, (int32_t) ((inst & 15) * s), false);
		load (e, o, at, s);
		result (0);
		return true;
	}
	case 0x8100:				/* mov.w R0,@(disp,Rn) */
	case 0x8000: {				/* mov.b R0,@(disp,Rn) */
		const unsigned s = (inst & 0x0100) ? 2 : 1;
		address (m, (int32_t) ((inst & 15) * s), false);
		value (0);
		store (e, o, at, s);
		return true;
	}
	}
	switch (inst & 0xF0FF) {
	case 0x4022:				/* sts.l PR,@-Rn */
	case 0x4012:				/* sts.l MACL,@-Rn */
	case 0x4002:				/* sts.l MACH,@-Rn */
	case 0x4052:				/* sts.l FPUL,@-Rn */
		address (n, -4, false);
		e.load32 (R10, RBX, (inst & 0xF0FF) == 0x4022 ? o.pr : (inst & 0xF0FF) == 0x4012 ? o.macl :
		                    (inst & 0xF0FF) == 0x4002 ? o.mach : o.fpul);
		store (e, o, at, 4);
		step (n, -4);
		return true;
	case 0x4026:				/* lds.l @Rn+,PR */
	case 0x4016:				/* lds.l @Rn+,MACL */
	case 0x4006:				/* lds.l @Rn+,MACH */
	case 0x4056:				/* lds.l @Rn+,FPUL */
		address (n, 0, false);
		load (e, o, at, 4);
		e.store32 (RBX, (inst & 0xF0FF) == 0x4026 ? o.pr : (inst & 0xF0FF) == 0x4016 ? o.macl :
		                (inst & 0xF0FF) == 0x4006 ? o.mach : o.fpul, RAX);
		step (n, 4);
		return true;
	}
	switch (inst & 0xF08F) {
	case 0x4083:				/* stc.l Rm_BANK,@-Rn */
		address (n, -4, false);
		e.load32 (R10, RBX, o.rbank + 4 * (int32_t) ((inst >> 4) & 7));
		store (e, o, at, 4);
		step (n, -4);
		return true;
	case 0x4087:				/* ldc.l @Rn+,Rm_BANK */
		address (n, 0, false);
		load (e, o, at, 4);
		e.store32 (RBX, o.rbank + 4 * (int32_t) ((inst >> 4) & 7), RAX);
		step (n, 4);
		return true;
	}
	if ((inst & 0xF0FF) == 0x00C3) {	/* movca.l R0,@Rn */
		address (n, 0, false);
		value (0);
		store (e, o, at, 4);
		return true;
	}
	return false;
}

/* T = the condition, from the flags of the instruction just written: kept
 * in its host register (Sh4RegCache::T) */
static void
set_t (Emitter &e, Sh4RegCache &rc, Cond c)
{
	e.setcc (c, RAX);
	e.movzx8 (rc.write (e, Sh4RegCache::T), RAX);
}

/* The instructions written out in x86-64: the registers' and T's alone,
 * as the interpreter's handlers (sh-insns-interp.h) have them, the GPRs in
 * their host registers. They read no memory and touch no interrupt bit: no
 * interrupt to look for after them, the program counter left as is (the
 * next call, the exit, set it). false for the others, which the handlers
 * run */
bool
Sh4X64::native_op (Emitter &e, const Sh4Offsets &o, uint16_t inst)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	const int32_t simm = (int8_t) inst;
	const uint32_t uimm = (uint8_t) inst;
	Reg a, d;

	if (inst == 0x0009 || inst == 0x0000)	/* nop (0000 as one) */
		return true;
	if (inst == 0x0008 || inst == 0x0018) {	/* clrt, sett */
		e.mov_imm32 (rc.write (e, Sh4RegCache::T), inst == 0x0018 ? 1 : 0);
		return true;
	}
	if (inst == 0x0019) {			/* div0u: Q, M, T clear */
		rc.t_drop ();
		e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~0x301);
		return true;
	}
	switch (inst & 0xF000) {
	case 0xE000:				/* mov #imm,Rn */
		e.mov_imm32 (rc.write (e, n), (uint32_t) simm);
		return true;
	case 0x7000:				/* add #imm,Rn */
		e.alu32_imm (Emitter::ALU_ADD, rc.modify (e, n), simm);
		return true;
	}
	switch (inst & 0xFF00) {
	case 0xC800:				/* tst #imm,R0 */
		e.test32_imm (rc.read (e, 0), uimm);
		set_t (e, rc, CC_E);
		return true;
	case 0xC900:				/* and #imm,R0 */
		e.alu32_imm (Emitter::ALU_AND, rc.modify (e, 0), (int32_t) uimm);
		return true;
	case 0xCA00:				/* xor #imm,R0 */
		e.alu32_imm (Emitter::ALU_XOR, rc.modify (e, 0), (int32_t) uimm);
		return true;
	case 0xCB00:				/* or #imm,R0 */
		e.alu32_imm (Emitter::ALU_OR, rc.modify (e, 0), (int32_t) uimm);
		return true;
	case 0x8800:				/* cmp/eq #imm,R0 */
		e.cmp32_imm (rc.read (e, 0), (uint32_t) simm);
		set_t (e, rc, CC_E);
		return true;
	}
	switch (inst & 0xF00F) {
	case 0x6003:				/* mov Rm,Rn */
		a = rc.read (e, m);
		d = rc.write (e, n);
		if (d != a)
			e.mov32 (d, a);
		return true;
	case 0x2007:				/* div0s Rm,Rn: Q Rn's sign, M Rm's, T Q ^ M */
		a = rc.read (e, m);
		d = rc.read (e, n);
		e.mov32 (RCX, d);
		e.shr32_imm (RCX, 31);			/* Q */
		e.mov32 (RDX, a);
		e.shr32_imm (RDX, 31);			/* M */
		rc.t_drop ();				/* T written below */
		e.load32 (RAX, RBX, o.sr);
		e.and32_imm (RAX, ~0x301u);
		e.mov32 (R8, RCX);
		e.alu32_reg_reg (Emitter::ALU_XOR, R8, RDX);	/* T */
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, R8);
		e.shl32_imm (RCX, 8);
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, RCX);
		e.shl32_imm (RDX, 9);
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, RDX);
		e.store32 (RBX, o.sr, RAX);
		return true;
	case 0x3004: {				/* div1 Rm,Rn: a step, as the handler */
		/* Q0, Rn's top bit; Rn = Rn << 1 | T; Rm read after (Rn's
		 * own when n is m); subtracted when Q (before) is M, else
		 * added, tmp1 the borrow or the carry; Q = Q0 ^ tmp1 ^ M,
		 * T = (Q == M): all without a branch */
		a = rc.read (e, m);
		d = rc.modify (e, n);
		rc.t_to_sr (e);
		rc.t_drop ();				/* T written below */
		e.bt_mem_imm (RBX, o.sr, 0);
		e.rot32 (Emitter::ROT_RCL, d, 1);	/* CF: Q0 */
		e.setcc (CC_B, R8);
		e.movzx8 (R8, R8);			/* Q0 */
		e.mov32 (R9, d);			/* tmp0 */
		e.load32 (RAX, RBX, o.sr);
		e.mov32 (R10, RAX);
		e.shr32_imm (R10, 8);
		e.and32_imm (R10, 1);			/* Q before */
		e.mov32 (R11, RAX);
		e.shr32_imm (R11, 9);
		e.and32_imm (R11, 1);			/* M */
		e.mov32 (RCX, R10);
		e.alu32_reg_reg (Emitter::ALU_XOR, RCX, R11);
		e.alu32_imm (Emitter::ALU_ADD, RCX, -1);	/* s: all ones to subtract */
		e.mov32 (RDX, a);
		e.alu32_reg_reg (Emitter::ALU_XOR, RDX, RCX);
		e.alu32_reg_reg (Emitter::ALU_ADD, d, RDX);
		e.alu32_reg_reg (Emitter::ALU_SUB, d, RCX);	/* Rn - Rm, or Rn + Rm */
		e.alu32_reg_reg (Emitter::ALU_CMP, d, R9);
		e.setcc (CC_A, RDX);
		e.movzx8 (RDX, RDX);			/* Rn > tmp0: the borrow */
		e.setcc (CC_B, R9);
		e.movzx8 (R9, R9);			/* Rn < tmp0: the carry */
		e.and32_imm (RCX, 1);
		e.alu32_reg_reg (Emitter::ALU_XOR, RDX, R9);
		e.alu32_reg_reg (Emitter::ALU_AND, RDX, RCX);
		e.alu32_reg_reg (Emitter::ALU_XOR, RDX, R9);	/* tmp1 */
		e.alu32_reg_reg (Emitter::ALU_XOR, RDX, R8);	/* Q0 ^ tmp1 */
		e.mov32 (R8, RDX);
		e.alu32_imm (Emitter::ALU_XOR, R8, 1);		/* T = !(Q ^ M) */
		e.alu32_reg_reg (Emitter::ALU_XOR, RDX, R11);	/* Q */
		e.and32_imm (RAX, ~0x101u);
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, R8);
		e.shl32_imm (RDX, 8);
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, RDX);
		e.store32 (RBX, o.sr, RAX);
		return true;
	}
	case 0x300C:				/* add Rm,Rn */
	case 0x3008:				/* sub Rm,Rn */
	case 0x2009:				/* and Rm,Rn */
	case 0x200B:				/* or Rm,Rn */
	case 0x200A: {				/* xor Rm,Rn */
		static const Emitter::Alu ops[16] = {
			Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD,
			Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD,
			Emitter::ALU_SUB, Emitter::ALU_AND, Emitter::ALU_XOR, Emitter::ALU_OR,
			Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD, Emitter::ALU_ADD,
		};
		a = rc.read (e, m);
		e.alu32_reg_reg (ops[inst & 15], rc.modify (e, n), a);
		return true;
	}
	case 0x6007:				/* not Rm,Rn */
		a = rc.read (e, m);
		d = rc.write (e, n);
		if (d != a)
			e.mov32 (d, a);
		e.not32 (d);
		return true;
	case 0x600C:				/* extu.b Rm,Rn */
		a = rc.read (e, m);
		e.movzx8 (rc.write (e, n), a);
		return true;
	case 0x600D:				/* extu.w Rm,Rn */
		a = rc.read (e, m);
		e.movzx16 (rc.write (e, n), a);
		return true;
	case 0x2008:				/* tst Rm,Rn */
		a = rc.read (e, m);
		e.test32 (rc.read (e, n), a);
		set_t (e, rc, CC_E);
		return true;
	case 0x3000:				/* cmp/eq Rm,Rn */
	case 0x3002:				/* cmp/hs Rm,Rn */
	case 0x3003:				/* cmp/ge Rm,Rn */
	case 0x3006:				/* cmp/hi Rm,Rn */
	case 0x3007: {				/* cmp/gt Rm,Rn */
		static const Cond conds[8] = {
			CC_E, CC_E, CC_AE, CC_GE, CC_E, CC_E, CC_A, CC_G
		};
		a = rc.read (e, m);
		e.alu32_reg_reg (Emitter::ALU_CMP, rc.read (e, n), a);
		set_t (e, rc, conds[inst & 7]);
		return true;
	}
	case 0x600E:				/* exts.b Rm,Rn */
		a = rc.read (e, m);
		e.movsx8 (rc.write (e, n), a);
		return true;
	case 0x600F:				/* exts.w Rm,Rn */
		a = rc.read (e, m);
		e.movsx16 (rc.write (e, n), a);
		return true;
	case 0x600B:				/* neg Rm,Rn */
		a = rc.read (e, m);
		d = rc.write (e, n);
		if (d != a)
			e.mov32 (d, a);
		e.neg32 (d);
		return true;
	case 0x6008:				/* swap.b Rm,Rn: the low half's bytes */
		e.mov32 (RAX, rc.read (e, m));
		e.mov32 (RCX, RAX);
		e.alu32_imm (Emitter::ALU_AND, RCX, (int32_t) 0xFFFF0000);
		e.mov32 (RDX, RAX);
		e.shl32_imm (RDX, 8);
		e.alu32_imm (Emitter::ALU_AND, RDX, 0xFF00);
		e.alu32_reg_reg (Emitter::ALU_OR, RCX, RDX);
		e.shr32_imm (RAX, 8);
		e.alu32_imm (Emitter::ALU_AND, RAX, 0xFF);
		e.alu32_reg_reg (Emitter::ALU_OR, RCX, RAX);
		e.mov32 (rc.write (e, n), RCX);
		return true;
	case 0x200D:				/* xtrct Rm,Rn: Rm's low half, Rn's high */
		e.mov32 (RAX, rc.read (e, m));
		e.shl32_imm (RAX, 16);
		e.mov32 (RCX, rc.read (e, n));
		e.shr32_imm (RCX, 16);
		e.alu32_reg_reg (Emitter::ALU_OR, RAX, RCX);
		e.mov32 (rc.write (e, n), RAX);
		return true;
	case 0x6009:				/* swap.w Rm,Rn */
		a = rc.read (e, m);
		d = rc.write (e, n);
		if (d != a)
			e.mov32 (d, a);
		e.rot32 (Emitter::ROT_ROL, d, 16);
		return true;
	case 0x0007:				/* mul.l Rm,Rn: MACL */
		a = rc.read (e, m);
		e.mov32 (RAX, rc.read (e, n));
		e.imul32 (RAX, a);
		e.store32 (RBX, o.macl, RAX);
		return true;
	case 0x200F:				/* muls.w Rm,Rn: MACL */
	case 0x200E:				/* mulu.w Rm,Rn */
		a = rc.read (e, m);
		d = rc.read (e, n);
		if (inst & 1) {
			e.movsx16 (RAX, d);
			e.movsx16 (RCX, a);
		} else {
			e.movzx16 (RAX, d);
			e.movzx16 (RCX, a);
		}
		e.imul32 (RAX, RCX);
		e.store32 (RBX, o.macl, RAX);
		return true;
	case 0x300E:				/* addc Rm,Rn: T the carry */
	case 0x300A: {				/* subc Rm,Rn: T the borrow */
		const Reg t = rc.read (e, Sh4RegCache::T);

		e.mov32 (RAX, t);
		a = rc.read (e, m);
		d = rc.modify (e, n);
		e.neg32 (RAX);				/* CF: T */
		e.alu32_reg_reg ((inst & 4) ? Emitter::ALU_ADC : Emitter::ALU_SBB, d, a);
		set_t (e, rc, CC_B);
		return true;
	}
	case 0x400C:				/* shad Rm,Rn */
	case 0x400D: {				/* shld Rm,Rn: as the handlers */
		Label right, none, done;

		e.mov32 (RCX, rc.read (e, m));
		d = rc.modify (e, n);
		e.test32 (RCX, RCX);
		e.jcc (CC_S, right);
		e.shift32_cl (4, d);			/* Rn << (Rm & 31) */
		e.jmp (done);
		e.bind (right);
		e.and32_imm (RCX, 31);
		e.jcc (CC_E, none);
		e.neg32 (RCX);				/* 32 - (Rm & 31): (~Rm & 31) + 1 */
		e.shift32_cl ((inst & 1) ? 5 : 7, d);
		e.jmp (done);
		e.bind (none);
		if (inst & 1)
			e.alu32_reg_reg (Emitter::ALU_XOR, d, d);	/* shld: 0 */
		else
			e.rot32 (Emitter::ROT_SAR, d, 31);	/* shad: the sign */
		e.bind (done);
		return true;
	}
	}
	switch (inst & 0xF0FF) {
	/* The control registers read and GBR written, as their handlers:
	 * stc SR alone took 5000 calls a frame in Club Kart 2003 */
	case 0x0002:				/* stc SR,Rn */
		rc.t_to_sr (e);
		e.load32 (rc.write (e, n), RBX, o.sr);
		return true;
	case 0x0012:				/* stc GBR,Rn */
		e.load32 (rc.write (e, n), RBX, o.gbr);
		return true;
	case 0x401E:				/* ldc Rn,GBR */
		e.store32 (RBX, o.gbr, rc.read (e, n));
		return true;
	case 0x4010:				/* dt Rn */
		e.alu32_imm (Emitter::ALU_SUB, rc.modify (e, n), 1);
		set_t (e, rc, CC_E);
		return true;
	case 0x4000:				/* shll Rn */
		e.shl32_imm (rc.modify (e, n), 1);
		set_t (e, rc, CC_B);
		return true;
	case 0x4001:				/* shlr Rn */
		e.shr32_imm (rc.modify (e, n), 1);
		set_t (e, rc, CC_B);
		return true;
	case 0x4008: e.shl32_imm (rc.modify (e, n), 2); return true;	/* shll2 */
	case 0x4018: e.shl32_imm (rc.modify (e, n), 8); return true;	/* shll8 */
	case 0x4028: e.shl32_imm (rc.modify (e, n), 16); return true;	/* shll16 */
	case 0x4009: e.shr32_imm (rc.modify (e, n), 2); return true;	/* shlr2 */
	case 0x4019: e.shr32_imm (rc.modify (e, n), 8); return true;	/* shlr8 */
	case 0x4029: e.shr32_imm (rc.modify (e, n), 16); return true;	/* shlr16 */
	case 0x0029: {				/* movt Rn */
		const Reg t = rc.read (e, Sh4RegCache::T);

		d = rc.write (e, n);
		e.mov32 (d, t);
		return true;
	}
	case 0x4024:				/* rotcl Rn: through T */
	case 0x4025:				/* rotcr Rn */
		d = rc.modify (e, n);
		rc.t_to_sr (e);
		e.bt_mem_imm (RBX, o.sr, 0);
		e.rot32 ((inst & 1) ? Emitter::ROT_RCR : Emitter::ROT_RCL, d, 1);
		set_t (e, rc, CC_B);
		return true;
	case 0x4004:				/* rotl Rn: T the bit out */
	case 0x4005:				/* rotr Rn */
	case 0x4021:				/* shar Rn */
		e.rot32 ((inst & 0x20) ? Emitter::ROT_SAR :
		         (inst & 1) ? Emitter::ROT_ROR : Emitter::ROT_ROL, rc.modify (e, n), 1);
		set_t (e, rc, CC_B);
		return true;
	case 0x002A:				/* sts PR,Rn */
		e.load32 (rc.write (e, n), RBX, o.pr);
		return true;
	case 0x0082: case 0x0092: case 0x00A2: case 0x00B2:	/* stc Rm_BANK,Rn */
	case 0x00C2: case 0x00D2: case 0x00E2: case 0x00F2:
		e.load32 (rc.write (e, n), RBX, o.rbank + 4 * (int32_t) ((inst >> 4) & 7));
		return true;
	case 0x408E: case 0x409E: case 0x40AE: case 0x40BE:	/* ldc Rn,Rm_BANK */
	case 0x40CE: case 0x40DE: case 0x40EE: case 0x40FE:
		e.store32 (RBX, o.rbank + 4 * (int32_t) ((inst >> 4) & 7), rc.read (e, n));
		return true;
	case 0x0093:				/* ocbi @Rn: nothing, as their handlers */
	case 0x00A3:				/* ocbp @Rn */
	case 0x00B3:				/* ocbwb @Rn */
		return true;
	case 0x402A:				/* lds Rn,PR */
		e.store32 (RBX, o.pr, rc.read (e, n));
		return true;
	case 0x001A:				/* sts MACL,Rn */
		e.load32 (rc.write (e, n), RBX, o.macl);
		return true;
	case 0x000A:				/* sts MACH,Rn */
		e.load32 (rc.write (e, n), RBX, o.mach);
		return true;
	case 0x4015:				/* cmp/pl Rn */
	case 0x4011:				/* cmp/pz Rn */
		e.cmp32_imm (rc.read (e, n), 0);
		set_t (e, rc, (inst & 4) ? CC_G : CC_GE);
		return true;
	/* The FPU's: FPUL, the singles at n ^ 1 (sh4.cpp, FR ()) */
	case 0x005A:				/* sts FPUL,Rn */
		e.load32 (rc.write (e, n), RBX, o.fpul);
		return true;
	case 0x405A:				/* lds Rm,FPUL */
		e.store32 (RBX, o.fpul, rc.read (e, n));
		return true;
	case 0xF00D:				/* fsts FPUL,FRn */
		e.load32 (RAX, RBX, o.fpul);
		e.store32 (RBX, o.fr + 4 * (int32_t) (n ^ 1), RAX);
		return true;
	case 0xF01D:				/* flds FRm,FPUL */
		e.load32 (RAX, RBX, o.fr + 4 * (int32_t) (n ^ 1));
		e.store32 (RBX, o.fpul, RAX);
		return true;
	case 0xF08D:				/* fldi0 FRn */
	case 0xF09D:				/* fldi1 FRn */
		e.store32_imm (RBX, o.fr + 4 * (int32_t) (n ^ 1),
		               (inst & 0x10) ? 0x3F800000 : 0);
		return true;
	}
	if (inst == 0xF3FD) {			/* fschg: FPSCR.SZ */
		e.alu_mem_imm (Emitter::ALU_XOR, RBX, o.fpscr, 0x00100000);
		return true;
	}
	return false;
}

/* k pairs "rotcl Rq; div1 Rm,Rn" at once (T, Q, M in r8d, r9d, r10d; a = Rm
 * the divisor, d = Rn the remainder, q = Rq the dividend's bits going out
 * at its top and the quotient's coming in at its bottom), M clear: a
 * non-restoring division, Q the remainder's sign (33 bits), T the next
 * quotient bit. Its remainder made non-negative, P = Rn (Q clear) or Rn +
 * Rm (Q set, carried out), when below Rm: the k steps divide P * 2^k + Rq's
 * top k bits by Rm, quotient bits t1..tk, remainder r; then Rq = Rq << k |
 * T << (k - 1) | t1..t(k-1), T = tk, Q = !tk, Rn = r, or r - Rm with Q set.
 * Else (M set, Rm 0, P not below Rm), the steps one at a time (slow). Exact
 * to the steps' bits: checked on 200000 divisions against the manual's DIV1 */
void
Sh4X64::division_pairs (Emitter &e, Reg a, Reg d, Reg q, unsigned k, const std::function<void ()> &slow_steps)
{
	Label slow, done, plain;

	e.test32 (R10, R10);
	e.jcc (CC_NE, slow);			/* M */
	e.test32 (a, a);
	e.jcc (CC_E, slow);
	e.mov32 (RAX, d);
	e.test32 (R9, R9);
	e.jcc (CC_E, plain);
	e.alu32_reg_reg (Emitter::ALU_ADD, RAX, a);	/* Q set: Rn + Rm, carried */
	e.jcc (CC_AE, slow);
	e.jmp (done);
	e.bind (plain);
	e.alu32_reg_reg (Emitter::ALU_CMP, RAX, a);	/* Q clear: Rn below Rm */
	e.jcc (CC_AE, slow);
	e.bind (done);
	/* rax = P << k | Rq >> (32 - k), rdx 0, divided by Rm */
	e.shl64_imm (RAX, (uint8_t) k);
	e.mov32 (RCX, q);
	e.shr32_imm (RCX, (uint8_t) (32 - k));
	e.alu64_reg_reg (Emitter::ALU_OR, RAX, RCX);
	e.alu32_reg_reg (Emitter::ALU_XOR, RDX, RDX);
	e.mov32 (RCX, a);
	e.div64 (RCX);
	/* Rq = Rq << k | T << (k - 1) | quotient >> 1 */
	e.shl32_imm (q, (uint8_t) k);
	e.shl32_imm (R8, (uint8_t) (k - 1));
	e.alu32_reg_reg (Emitter::ALU_OR, q, R8);
	e.mov32 (RCX, RAX);
	e.shr32_imm (RCX, 1);
	e.alu32_reg_reg (Emitter::ALU_OR, q, RCX);
	/* T, Q; Rn the remainder, less Rm with Q */
	e.mov32 (R8, RAX);
	e.and32_imm (R8, 1);
	e.mov32 (R9, R8);
	e.alu32_imm (Emitter::ALU_XOR, R9, 1);
	e.mov32 (d, RDX);
	Label &end = label ();
	e.test32 (R8, R8);
	e.jcc (CC_NE, end);
	e.alu32_reg_reg (Emitter::ALU_SUB, d, a);
	e.jmp (end);
	e.bind (slow);
	slow_steps ();
	e.bind (end);
}

/* A division's steps: "div1 Rm,Rn" and "rotcl Rq" one after the other (the
 * library's 32-bit divisions, 32 of each), at least four, Rm, Rn, Rq apart:
 * T, Q and M kept in r8d, r9d, r10d from the first to the last, SR read and
 * written once, each step as native_op () writes it (div1's without a
 * branch, rotcl's through CF). Gun Survivor 2 spent a fifth of its SH-4 in
 * them */
unsigned
Sh4X64::division_run (Emitter &e, const Sh4Offsets &o, const Sh4Block *block, size_t i)
{
	const size_t n = block->ops.size ();
	unsigned rm = 16, rn = 16, rq = 16;
	size_t k;

	for (k = i; k < n; k++) {
		uint16_t inst = block->ops[k].inst;

		if ((inst & 0xF00F) == 0x3004) {
			unsigned m = (inst >> 4) & 15, nn = (inst >> 8) & 15;

			if (rm == 16) {
				rm = m;
				rn = nn;
			} else if (m != rm || nn != rn)
				break;
		} else if ((inst & 0xF0FF) == 0x4024) {
			unsigned q = (inst >> 8) & 15;

			if (rq == 16)
				rq = q;
			else if (q != rq)
				break;
		} else
			break;
		if (block->ops[k].patched)
			break;
	}
	if (k - i < 4 || rm == 16 || rq == 16 || rm == rn || rq == rn || rq == rm)
		return 0;

	Reg a = rc.read (e, rm), d = rc.modify (e, rn), q = rc.modify (e, rq);

	rc.t_to_sr (e);
	rc.t_drop ();				/* T written back below */
	e.load32 (RAX, RBX, o.sr);
	e.mov32 (R8, RAX);
	e.and32_imm (R8, 1);			/* T */
	e.mov32 (R9, RAX);
	e.shr32_imm (R9, 8);
	e.and32_imm (R9, 1);			/* Q */
	e.mov32 (R10, RAX);
	e.shr32_imm (R10, 9);
	e.and32_imm (R10, 1);			/* M */
	/* The steps from op j0 to j1, one at a time */
	auto steps = [&] (size_t j0, size_t j1) {
	for (size_t j = j0; j < j1; j++) {
		if ((block->ops[j].inst & 0xF00F) == 0x3004) {
			/* div1: as native_op (), on the registers */
			e.mov32 (RAX, d);
			e.shr32_imm (RAX, 31);			/* Q0 */
			e.alu32_reg_reg (Emitter::ALU_ADD, d, d);
			e.alu32_reg_reg (Emitter::ALU_OR, d, R8);	/* Rn << 1 | T */
			e.mov32 (RCX, d);			/* tmp0 */
			e.mov32 (RDX, R9);
			e.alu32_reg_reg (Emitter::ALU_XOR, RDX, R10);
			e.alu32_imm (Emitter::ALU_ADD, RDX, -1);	/* s */
			e.mov32 (R8, a);
			e.alu32_reg_reg (Emitter::ALU_XOR, R8, RDX);
			e.alu32_reg_reg (Emitter::ALU_ADD, d, R8);
			e.alu32_reg_reg (Emitter::ALU_SUB, d, RDX);
			e.alu32_reg_reg (Emitter::ALU_CMP, d, RCX);
			e.setcc (CC_A, R8);
			e.setcc (CC_B, RCX);
			e.movzx8 (R8, R8);			/* the borrow */
			e.movzx8 (RCX, RCX);			/* the carry */
			e.and32_imm (RDX, 1);
			e.alu32_reg_reg (Emitter::ALU_XOR, R8, RCX);
			e.alu32_reg_reg (Emitter::ALU_AND, R8, RDX);
			e.alu32_reg_reg (Emitter::ALU_XOR, R8, RCX);	/* tmp1 */
			e.alu32_reg_reg (Emitter::ALU_XOR, RAX, R8);	/* Q0 ^ tmp1 */
			e.mov32 (R8, RAX);
			e.alu32_imm (Emitter::ALU_XOR, R8, 1);		/* T */
			e.alu32_reg_reg (Emitter::ALU_XOR, RAX, R10);
			e.mov32 (R9, RAX);			/* Q */
		} else {
			/* rotcl: CF = T (neg sets it unless 0), then out */
			e.neg32 (R8);
			e.rot32 (Emitter::ROT_RCL, q, 1);
			e.setcc (CC_B, R8);
			e.movzx8 (R8, R8);
		}
	}
	};
	/* The longest run of pairs "rotcl Rq; div1 Rm,Rn" in it: done at once
	 * (division_pairs ()), the other steps one at a time */
	size_t p0 = i, best0 = i, best = 0;

	while (p0 < k) {
		size_t p = p0;

		while (p + 1 < k && (block->ops[p].inst & 0xF0FF) == 0x4024 &&
		       (block->ops[p + 1].inst & 0xF00F) == 0x3004)
			p += 2;
		if ((p - p0) / 2 > best) {
			best = (p - p0) / 2;
			best0 = p0;
		}
		p0 = p > p0 ? p : p0 + 1;
	}
	if (best >= 4) {
		steps (i, best0);
		division_pairs (e, a, d, q, (unsigned) best, [&] () { steps (best0, best0 + 2 * best); });
		steps (best0 + 2 * best, k);
	} else
		steps (i, k);
	e.load32 (RAX, RBX, o.sr);
	e.and32_imm (RAX, ~0x101u);
	e.alu32_reg_reg (Emitter::ALU_OR, RAX, R8);
	e.shl32_imm (R9, 8);
	e.alu32_reg_reg (Emitter::ALU_OR, RAX, R9);
	e.store32 (RBX, o.sr, RAX);
	return (unsigned) (k - i);
}

/* A delay loop on a memory word, "while (n--)" with n in RAM (the stack's,
 * or through a register or the literals): its six instructions run as
 * many turns as leave it, the last one run as written (the loop goes out of
 * itself), taken here at once: the word down by one a turn, six cycles a
 * turn off the slice. Nothing else runs meanwhile, no interrupt comes; the
 * registers and T each turn sets are set again by the turn that runs. The
 * word: aligned, in a RAM page the table maps for writes, no code compared
 * there in this epoch; else the loop runs as it is. Turns with room in the
 * slice for the one that runs after: (remaining - 7) / 6. 18 Wheeler,
 * Slashout, Cannon Spike, Soul Surfer and others spent a tenth to a third
 * of their SH-4 in them */
void
Sh4X64::memory_loop (Emitter &e, const Sh4Offsets &o, const Sh4Block *block)
{
	Label &out = label ();
	const unsigned kind = block->mem_loop;

	/* ecx the word's address */
	if (kind == Sh4Block::MEM_LOOP_PL) {
		const uint32_t lit = ((block->pc + 4) & ~3u) + (uint32_t) ((block->words[0] & 0xFF) * 4);
		const uint8_t *host = literal (lit, 4);

		if (!host)
			return;
		e.mov_imm64 (RAX, (uint64_t) (uintptr_t) host);
		e.load32 (RCX, RAX, 0);
	} else {
		e.load32 (RCX, RBX, o.r[block->mem_reg]);
		if (block->mem_disp)
			e.alu32_imm (Emitter::ALU_ADD, RCX, 4 * (int32_t) block->mem_disp);
	}
	/* Aligned, in RAM (the table's write page), not on code */
	e.test32_imm (RCX, 3);
	e.jcc (CC_NE, out);
	if (backend_kind == PAGES) {
		e.mov32 (RAX, RCX);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, out);
		e.mov32 (RAX, RCX);
		e.shr32_imm (RAX, hl::Mmap::TABLE_PAGE_BITS);
		e.load64 (R8, RBX, o.jit_tables + 8);
		e.load64_index (R9, R8, RAX, 8);
		e.test64 (R9, R9);
		e.jcc (CC_E, out);
		e.alu64_reg_reg (Emitter::ALU_ADD, R9, RCX);	/* r9: the word */
	} else
		return;
	/* v, the word: the turns before the last, v - 1 (the stack's and EQ's
	 * leave at v 0, as unsigned; PL's at v <= 0) */
	e.load32 (R10, R9, 0);
	if (kind == Sh4Block::MEM_LOOP_PL) {
		e.cmp32_imm (R10, 1);
		e.jcc (CC_LE, out);
	} else {
		e.test32 (R10, R10);
		e.jcc (CC_E, out);
	}
	e.lea32 (R11, R10, -1);				/* the turns by the word */
	/* The turns by the slice, room left for the block after: (remaining
	 * - its instructions - 1) / 6 */
	e.load32 (RAX, RBX, o.remaining);
	e.alu32_imm (Emitter::ALU_SUB, RAX, (int32_t) block->ops.size () + 1);
	e.jcc (CC_LE, out);
	e.mov_imm32 (RCX, 0xAAAAAAAB);			/* / 6: * (2^34 / 6 + 1) >> 34 */
	e.movsxd (RAX, RAX);
	e.imul64 (RAX, RCX);
	e.sar64_imm (RAX, 34);				/* RAX = (remaining - 7) / 6 */
	e.alu32_reg_reg (Emitter::ALU_CMP, R11, RAX);
	Label &by_word = label ();
	e.jcc (CC_B, by_word);				/* unsigned: the word's fewer */
	e.mov32 (R11, RAX);
	e.bind (by_word);
	e.test32 (R11, R11);
	e.jcc (CC_E, out);
	/* k = r11: the word down by k, 6k cycles off */
	e.alu32_reg_reg (Emitter::ALU_SUB, R10, R11);
	e.store32 (R9, 0, R10);
	e.lea32 (RAX, R11, 0);
	e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RAX);
	e.alu32_reg_reg (Emitter::ALU_ADD, RAX, R11);
	e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RAX);	/* 6k */
	e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, RAX);
	e.bind (out);
}

/* An access group (access_group ()): the 32-bit loads or stores through
 * one base register, in a row, at offsets known from its value at the
 * first: mov.l @Rm, @Rm+, @(disp,Rm), Rm,@Rn, Rm,@-Rn, Rm,@(disp,Rn), and
 * fmov.s's (FPSCR.SZ seen clear). The base not loaded, nor stored, by the
 * group. Checked once: aligned, one page of the table (a RAM's), no code
 * compared there in this epoch for stores; then each access straight to
 * the host page, no interrupt to look for (no device). Else the ops'
 * handlers one after the other (a slow path), an interrupt looked for after
 * each as the accesses written out do. Both ways leave ctx as the ops do,
 * nothing kept in the host registers. Each access had its own alignment
 * test, page lookup and interrupt check: 14 instructions for an fmov.s
 * @Rm+, 4 here */
namespace {
struct GroupAccess {
	int32_t	 off;
	bool	 fpu;
	unsigned reg;		/* loaded or stored */
};
}

unsigned
Sh4X64::access_group (Emitter &e, const Sh4Offsets &o, const Sh4Block *block, size_t i,
                      Label *seq, bool *forced, bool singles)
{
	const size_t n = block->ops.size ();
	GroupAccess acc[Sh4Block::MAX_OPS];
	unsigned count = 0, k;
	int base = -1, store = -1;
	int32_t cur = 0, lo = 0, hi = 0;

	if (mmu_mode || backend_kind != PAGES)
		return 0;
	for (k = (unsigned) i; k < n; k++) {
		const uint16_t inst = block->ops[k].inst;
		const unsigned rn = (inst >> 8) & 15, rm = (inst >> 4) & 15;
		int b = -1, st = -1, step = 0;
		int32_t disp = 0;
		unsigned reg = 0;
		bool fpu = false;

		if (block->ops[k].patched)
			break;
		switch (inst & 0xF00F) {
		case 0x6002: b = rm; st = 0; reg = rn; break;			/* mov.l @Rm,Rn */
		case 0x6006: b = rm; st = 0; reg = rn; step = 4; break;	/* mov.l @Rm+,Rn */
		case 0x2002: b = rn; st = 1; reg = rm; break;			/* mov.l Rm,@Rn */
		case 0x2006: b = rn; st = 1; reg = rm; step = -4; break;	/* mov.l Rm,@-Rn */
		case 0xF008: if (singles) { b = rm; st = 0; reg = rn; fpu = true; } break;		/* fmov.s @Rm,FRn */
		case 0xF009: if (singles) { b = rm; st = 0; reg = rn; fpu = true; step = 4; } break;	/* fmov.s @Rm+,FRn */
		case 0xF00A: if (singles) { b = rn; st = 1; reg = rm; fpu = true; } break;		/* fmov.s FRm,@Rn */
		case 0xF00B: if (singles) { b = rn; st = 1; reg = rm; fpu = true; step = -4; } break;	/* fmov.s FRm,@-Rn */
		}
		if (b < 0) {
			switch (inst & 0xF000) {
			case 0x5000: b = rm; st = 0; reg = rn; disp = 4 * (inst & 15); break;	/* mov.l @(disp,Rm),Rn */
			case 0x1000: b = rn; st = 1; reg = rm; disp = 4 * (inst & 15); break;	/* mov.l Rm,@(disp,Rn) */
			}
		}
		if (b < 0 || (base >= 0 && (b != base || st != store)))
			break;
		/* The base neither loaded nor stored (its value moves) */
		if (!fpu && reg == (unsigned) b)
			break;
		base = b;
		store = st;
		if (step < 0)
			cur += step;
		acc[count++] = GroupAccess { cur + disp, fpu, reg };
		if (step > 0)
			cur += step;
	}
	if (count < 3)
		return 0;
	lo = hi = acc[0].off;
	for (k = 1; k < count; k++) {
		lo = MIN2 (lo, acc[k].off);
		hi = MAX2 (hi, acc[k].off);
	}

	Label &slow = label (), &done = label ();
	Reg b;

	mark (e, "memory: group");
	rc.flush (e);
	b = rc.read (e, (unsigned) base);
	e.mov32 (RCX, b);
	/* Aligned (the offsets 4's), in one page */
	e.lea32 (RAX, RCX, lo);
	e.test8_imm (RAX, 3);
	e.jcc (CC_NE, slow);
	if (store && sq_inline) {
		/* Into a store queue (E0000000-E3FFFFFF, its 64 bytes again
		 * and again): Sh4::sq's words, counted as sq_store () counts
		 * them (no code there) */
		Label &other = label ();

		e.lea32 (RDX, RAX, 0x20000000);
		e.cmp32_imm (RDX, 0x04000000 - 64);
		e.jcc (CC_AE, other);
		for (k = 0; k < count; k++) {
			const int32_t at = acc[k].fpu ? o.fr + 4 * (int32_t) (acc[k].reg ^ 1) : o.r[acc[k].reg];
			Reg v;

			e.lea32 (RDX, RCX, acc[k].off);
			e.and32_imm (RDX, 0x3F);
			e.add64_imm (RDX, o.sq);
			if (acc[k].fpu || !rc.peek (acc[k].reg, v)) {
				e.load32 (RAX, RBX, at);
				v = RAX;
			}
			e.store32_index (RBX, RDX, 1, v);
		}
		if (cur)
			e.alu_mem_imm (Emitter::ALU_ADD, RBX, o.r[base], cur);
		e.add64_mem_imm (RBX, o.stores, (int32_t) count);
		e.add64_mem_imm (RBX, o.far_stores, (int32_t) count);
		e.jmp (done);
		e.bind (other);
	}
	e.lea32 (RDX, RCX, hi + 3);
	e.alu32_reg_reg (Emitter::ALU_XOR, RDX, RAX);
	e.test32_imm (RDX, ~((1u << hl::Mmap::TABLE_PAGE_BITS) - 1));
	e.jcc (CC_NE, slow);
	if (store) {
		/* No code compared there in this epoch, at either end (the
		 * group spans 64 bytes at most: two stamps) */
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, slow);
		e.lea32 (RAX, RCX, hi + 3);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, slow);
		e.lea32 (RAX, RCX, lo);
	}
	e.shr32_imm (RAX, hl::Mmap::TABLE_PAGE_BITS);
	e.load64 (R8, RBX, o.jit_tables + (store ? 8 : 0));
	e.load64_index (R9, R8, RAX, 8);
	e.test64 (R9, R9);
	e.jcc (CC_E, slow);
	/* r9 + rcx: the base's host address */
	e.alu64_reg_reg (Emitter::ALU_ADD, R9, RCX);
	for (k = 0; k < count; k++) {
		const int32_t at = acc[k].fpu ? o.fr + 4 * (int32_t) (acc[k].reg ^ 1) : o.r[acc[k].reg];

		if (store) {
			Reg v;

			if (!acc[k].fpu && rc.peek (acc[k].reg, v))
				e.store32 (R9, acc[k].off, v);
			else {
				e.load32 (RAX, RBX, at);
				e.store32 (R9, acc[k].off, RAX);
			}
		} else {
			e.load32 (RAX, R9, acc[k].off);
			e.store32 (RBX, at, RAX);
		}
	}
	if (cur)
		e.alu_mem_imm (Emitter::ALU_ADD, RBX, o.r[base], cur);
	if (store) {
		/* Counted as sh4_put () would: a store at least, one away
		 * from the stack unless through r15 */
		e.add64_mem_imm (RBX, o.stores, (int32_t) count);
		if (base != 15) {
			e.lea32 (RCX, RCX, lo);
			count_far (e, o);
		}
	}
	rc.forget ();
	e.bind (done);
	cold_path (Cold::GROUP, slow, done, (int32_t) (2 * i), count);
	for (k = 0; k < count; k++)
		if (i + k + 1 < n)
			forced[i + k] = true;
	(void) seq;
	return count;
}

/* T = eax (0 or 1), into SR: an FPU's, whose slow way (its handler, the
 * double's) leaves from before it with the registers as they are after it,
 * so it keeps the cache as it was: its copy of T given up, none taken */
static void
put_t (Emitter &e, const Sh4Offsets &o, Sh4RegCache &rc)
{
	rc.t_drop ();
	e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~1);
	e.alu_mem_reg (Emitter::ALU_OR, RBX, o.sr, RAX);
}

/* The FPU's arithmetic in single precision, as the handlers have it
 * (sh-insns-interp.h), in SSE and in their order: the same bits (no fused
 * multiply-add in the handlers, built without /arch; the same rounding,
 * the host's). FPSCR.PR set (doubles), or SR.FD where the handler looks at
 * it: to slow, the handler then (fallback set), and out of the block. Each
 * tested once in a run of the block's FPU work (pr_ok, fd_ok): only a
 * handler changes them (lds to FPSCR, ldc to SR), after which they are
 * tested again; the tests took two instructions and 16 bytes an FPU
 * instruction. false, nothing written, for the others (FSCA, FSRRA: the
 * handlers) */
static bool
fpu_op (Emitter &e, const Sh4Offsets &o, Sh4RegCache &rc, uint16_t inst, Label &slow, bool &fallback,
        bool &pr_ok, bool &fd_ok)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	unsigned k, r;

	auto fr = [&] (unsigned i) { return o.fr + 4 * (int32_t) (i ^ 1); };
	auto xf = [&] (unsigned i) { return o.xf + 4 * (int32_t) (i ^ 1); };
	/* FPSCR.PR clear; SR.FD clear */
	auto singles = [&] () {
		if (pr_ok)
			return;
		e.test_mem_imm (RBX, o.fpscr, 0x00080000);
		e.jcc (CC_NE, slow);
		fallback = true;
		pr_ok = true;
	};
	auto enabled = [&] () {
		if (fd_ok)
			return;
		e.test_mem_imm (RBX, o.sr, 0x00008000);
		e.jcc (CC_NE, slow);
		fallback = true;
		fd_ok = true;
	};

	if ((inst & 0xF000) != 0xF000)
		return false;
	if (inst == 0xFBFD) {			/* frchg: FPSCR.FR, the banks swapped */
		e.alu_mem_imm (Emitter::ALU_XOR, RBX, o.fpscr, 0x00200000);
		for (k = 0; k < 64; k += 8) {
			e.load64 (RAX, RBX, o.fr + (int32_t) k);
			e.load64 (RCX, RBX, o.xf + (int32_t) k);
			e.store64 (RBX, o.fr + (int32_t) k, RCX);
			e.store64 (RBX, o.xf + (int32_t) k, RAX);
		}
		return true;
	}
	switch (inst & 0xF00F) {
	case 0xF000:				/* fadd FRm,FRn */
	case 0xF001:				/* fsub FRm,FRn */
	case 0xF002:				/* fmul FRm,FRn */
	case 0xF003: {				/* fdiv FRm,FRn */
		static const Emitter::Sse ops[4] = {
			Emitter::SSE_ADD, Emitter::SSE_SUB, Emitter::SSE_MUL, Emitter::SSE_DIV
		};
		singles ();
		e.movss_load (0, RBX, fr (n));
		e.sse_mem (ops[inst & 3], 0, RBX, fr (m));
		e.movss_store (RBX, fr (n), 0);
		return true;
	}
	case 0xF004: {				/* fcmp/eq FRm,FRn: unordered not */
		Label unordered;

		singles ();
		e.movss_load (0, RBX, fr (n));
		e.ucomiss_mem (0, RBX, fr (m));
		e.mov_imm32 (RAX, 0);
		e.jcc (CC_P, unordered);
		e.setcc (CC_E, RAX);
		e.bind (unordered);
		put_t (e, o, rc);
		return true;
	}
	case 0xF005:				/* fcmp/gt FRm,FRn: FRn > FRm */
		singles ();
		e.movss_load (0, RBX, fr (n));
		e.ucomiss_mem (0, RBX, fr (m));
		e.setcc (CC_A, RAX);
		e.movzx8 (RAX, RAX);
		put_t (e, o, rc);
		return true;
	case 0xF00E:				/* fmac FR0,FRm,FRn */
		singles ();
		e.movss_load (0, RBX, fr (m));
		e.sse_mem (Emitter::SSE_MUL, 0, RBX, fr (0));
		e.movss_load (1, RBX, fr (n));
		e.sse_reg (Emitter::SSE_ADD, 1, 0);
		e.movss_store (RBX, fr (n), 1);
		return true;
	}
	if ((inst & 0xF3FF) == 0xF1FD) {	/* ftrv XMTRX,FVn */
		/* As the handler, four lanes at once: each lane's sum in its
		 * order, ((x0 v0 + x1 v1) + x2 v2) + x3 v3, singles (the
		 * handler leaves PR alone too). The registers in pairs swapped
		 * (FR (), XF ()): XMTRX's column k, the 16 bytes at xf + 16k,
		 * lane j its row j ^ 1, as FVn's 16 bytes, its result */
		const unsigned v = n & 12;

		enabled ();
		for (k = 0; k < 4; k++) {
			e.movups_load (k ? 1 : 0, RBX, o.xf + 16 * (int32_t) k);
			e.movss_load (2, RBX, fr (v + k));
			e.shufps (2, 2, 0);
			e.ps_reg (Emitter::SSE_MUL, k ? 1 : 0, 2);
			if (k)
				e.ps_reg (Emitter::SSE_ADD, 0, 1);
		}
		e.movups_store (RBX, o.fr + 4 * (int32_t) v, 0);
		return true;
	}
	switch (inst & 0xF0FF) {
	case 0xF02D:				/* float FPUL,FRn */
		singles ();
		e.cvtsi2ss_mem (0, RBX, o.fpul);
		e.movss_store (RBX, fr (n), 0);
		return true;
	case 0xF03D:				/* ftrc FRn,FPUL */
		singles ();
		e.cvttss2si_mem (RAX, RBX, fr (n));
		e.store32 (RBX, o.fpul, RAX);
		return true;
	case 0xF04D:				/* fneg FRn */
		singles ();
		e.alu_mem_imm (Emitter::ALU_XOR, RBX, fr (n), (int32_t) 0x80000000u);
		return true;
	case 0xF05D:				/* fabs FRn */
		singles ();
		e.alu_mem_imm (Emitter::ALU_AND, RBX, fr (n), 0x7FFFFFFF);
		return true;
	case 0xF06D:				/* fsqrt FRn */
		enabled ();
		singles ();
		e.sse_mem (Emitter::SSE_SQRT, 0, RBX, fr (n));
		e.movss_store (RBX, fr (n), 0);
		return true;
	case 0xF0ED: {				/* fipr FVm,FVn: FR(n+3) */
		const unsigned v = n & 12, w = (n & 3) << 2;

		enabled ();
		e.movss_load (0, RBX, fr (v));
		e.sse_mem (Emitter::SSE_MUL, 0, RBX, fr (w));
		for (k = 1; k < 4; k++) {
			e.movss_load (1, RBX, fr (v + k));
			e.sse_mem (Emitter::SSE_MUL, 1, RBX, fr (w + k));
			e.sse_reg (Emitter::SSE_ADD, 0, 1);
		}
		e.movss_store (RBX, fr (v + 3), 0);
		return true;
	}
	}
	if ((inst & 0xF3FF) == 0xF1FD) {	/* ftrv XMTRX,FVn: xmm2-xmm5 */
		const unsigned v = n & 12;

		enabled ();
		for (r = 0; r < 4; r++) {
			e.movss_load (2 + r, RBX, xf (r));
			e.sse_mem (Emitter::SSE_MUL, 2 + r, RBX, fr (v));
			for (k = 1; k < 4; k++) {
				e.movss_load (1, RBX, xf (r + 4 * k));
				e.sse_mem (Emitter::SSE_MUL, 1, RBX, fr (v + k));
				e.sse_reg (Emitter::SSE_ADD, 2 + r, 1);
			}
		}
		for (r = 0; r < 4; r++)
			e.movss_store (RBX, fr (v + r), 2 + r);
		return true;
	}
	return false;
}

/* What a delay slot may hold, written out: an ALU instruction, a move to
 * or from memory, the FPU's arithmetic (its tests before the jump), a
 * pref; none relative to PC (the interpreter's PC is the jump's target in
 * the slot), no jump. Tried on a scratch buffer */
bool
Sh4X64::slot_writable (const Sh4Offsets &o, uint16_t inst)
{
	uint8_t scratch[2048];
	Emitter e (scratch, sizeof (scratch));

	size_t was = cold.size ();
	Sh4RegCache kept = rc;
	bool counted = store_counted;
	bool ok;

	if ((inst & 0xF000) == 0xD000 || (inst & 0xF000) == 0x9000 || sh4_may_branch (inst))
		return false;
	ok = native_op (e, o, inst) || memory_op (e, o, inst, 0, 0);
	if (!ok) {
		Label slow;
		bool fallback = false, pr = false, fd = false;

		ok = fpu_op (e, o, rc, inst, slow, fallback, pr, fd);
	}
	if (!ok)
		ok = (inst & 0xF0FF) == 0x0083 && !mmu_mode;
	cold.resize (was);
	rc = kept;
	store_counted = counted;
	return ok;
}

void
Sh4X64::slot_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at)
{
	Label slow;
	bool fallback = false;

	if (native_op (e, o, inst))
		return;
	/* The FPU's arithmetic: FPSCR.PR, SR.FD tested before the jump
	 * (slot_guards ()) */
	if (fpu_op (e, o, rc, inst, slow, fallback, pr_ok, fd_ok)) {
		if (fallback)
			HL_ABORT ("sh4-x64: a delay slot's FPU work untested");
		return;
	}
	if ((inst & 0xF0FF) == 0x0083) {
		/* pref @Rn: at a store queue's address alone, its handler
		 * (it reads Rn, changes none of the registers) */
		Label &none = label ();
		const Reg rn = rc.read (e, (inst >> 8) & 15);

		e.mov32 (RAX, rn);
		e.shr32_imm (RAX, 26);
		e.cmp32_imm (RAX, 0x38);
		e.jcc (CC_NE, none);
		rc.store_dirty (e);
		if (translating_cpu->stats) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) sh4_stats_handler (translating_cpu,
			                                                           translating_pc + (uint32_t) at, inst));
			e.add64_mem_imm (RAX, 0, 1);
		}
		e.mov (RCX, RBX);
		e.mov_imm32 (RDX, inst);
		e.call ((const void *) sh4_handler (inst));
		e.bind (none);
		return;
	}
	in_slot = true;
	memory_op (e, o, inst, at, 0);
	in_slot = false;
}

/* The jumps jump_op () writes out, with their slot */
bool
Sh4X64::jump_writable (const Sh4Offsets &o, const Sh4Block *block, size_t i)
{
	const uint16_t inst = block->ops[i].inst;

	if (i + 1 != block->ops.size () || !block->slot_kept ||
	    !slot_writable (o, block->words[i + 1]))
		return false;
	switch (inst & 0xF000) {
	case 0xA000:				/* bra */
	case 0xB000:				/* bsr */
		return true;
	}
	switch (inst & 0xF0FF) {
	case 0x402B:				/* jmp @Rn */
	case 0x400B:				/* jsr @Rn */
	case 0x0003:				/* bsrf Rn */
	case 0x0023:				/* braf Rn */
		return true;
	}
	return inst == 0x000B;			/* rts */
}

/* The block's last instruction, an unconditional jump, and its delay slot
 * (kept with the block) written out, as the handlers have them
 * (sh-insns-interp.h): the target and PR set before the slot (the slot
 * reads them as they were), the slot, PC the target, out. false, nothing
 * written, for RTE and the others, or a slot not written out */
bool
Sh4X64::jump_op (Emitter &e, const Sh4Offsets &o, const Sh4Block *block, size_t i, Label &out,
                 bool &fixed_to, int32_t &to)
{
	const uint16_t inst = block->ops[i].inst;
	const int32_t at = (int32_t) (2 * i);
	const unsigned rn = (inst >> 8) & 15;
	const int32_t simm12 = (int32_t) ((inst & 0xFFF) ^ 0x800) - 0x800;
	bool fixed = false;

	if (!jump_writable (o, block, i))
		return false;
	switch (inst & 0xF000) {
	case 0xB000:				/* bsr: PR, then as bra */
		e.lea32 (RAX, RSI, at + 4);
		e.store32 (RBX, o.pr, RAX);
		/* fall through */
	case 0xA000:				/* bra */
		fixed = true;
		break;
	default:
		switch (inst & 0xF0FF) {
		case 0x402B:			/* jmp @Rn */
			e.store32 (RBX, o.target, rc.read (e, rn));
			break;
		case 0x400B:			/* jsr @Rn */
			e.store32 (RBX, o.target, rc.read (e, rn));
			e.lea32 (RAX, RSI, at + 4);
			e.store32 (RBX, o.pr, RAX);
			break;
		case 0x0003:			/* bsrf Rn: PR, then as braf */
			e.lea32 (RAX, RSI, at + 4);
			e.store32 (RBX, o.pr, RAX);
			/* fall through */
		case 0x0023:			/* braf Rn */
			e.lea32 (RCX, RSI, at + 4);
			e.store32 (RBX, o.target, RCX);
			e.alu_mem_reg (Emitter::ALU_ADD, RBX, o.target, rc.read (e, rn));
			break;
		default:			/* rts */
			e.load32 (RAX, RBX, o.pr);
			e.store32 (RBX, o.target, RAX);
			break;
		}
	}
	slot_op (e, o, block->words[i + 1], at + 2);
	if (fixed)
		e.lea32 (RAX, RSI, at + 4 + 2 * simm12);
	else
		e.load32 (RAX, RBX, o.target);
	e.store32 (RBX, o.pc, RAX);
	rc.store_dirty (e);
	e.jmp (out);
	fixed_to = fixed;
	to = at + 4 + 2 * simm12;
	return true;
}

Sh4Native
Sh4X64::translate (Sh4 *ctx, Sh4Block *block)
{
	const Sh4Offsets o (ctx);
	const size_t n = block->ops.size ();
	/* A generous bound of the function's size: an FPU move writes both
	 * sizes' accesses, each with its slow path, some 400 bytes; a jump its
	 * slot's too */
	const size_t max = 1024 + n * 768;
	/* The ways out after instruction i: in sequence (the next one's
	 * address to set), after a handler's branch (its target less 2 set),
	 * after a branch written out (its target set); after a jump and its
	 * slot written out (its target set, i + 2 run) */
	Label seq[Sh4Block::MAX_OPS], branched[Sh4Block::MAX_OPS], done[Sh4Block::MAX_OPS],
	      jumped[Sh4Block::MAX_OPS];
	bool used[Sh4Block::MAX_OPS] = {}, jumped_used[Sh4Block::MAX_OPS] = {};
	/* The ways out an access group's slow path takes (written after) */
	bool forced[Sh4Block::MAX_OPS] = {};
	/* Where a branch written out goes, from esi: the ends whose target is
	 * known (bt, bf; bt/s, bf/s, bra, bsr) */
	int32_t to[Sh4Block::MAX_OPS] = {};
	bool fixed[Sh4Block::MAX_OPS] = {};
	Label end, probe, probe_checks, chain, icache_miss, icache_done;
	uint8_t *buf, *body;
	size_t i;

	if (stubs_generation != generation && !emit_stubs (ctx))
		return NULL;
	buf = arena.take (max, 16);
	if (!buf)
		return NULL;
	Emitter e (buf, max);
	translating = ctx->mmap;
	translating_cpu = ctx;
	translating_pc = block->pc;
	stamps = ctx->blocks.stamps ();
	cold.clear ();
	labels_used = 0;
	rc.reset (&o);
	store_counted = false;
	marks.clear ();
	mmu_mode = ctx->mmu_exact;
	virt_mode = mmu_mode && ctx->jit_virtual;
	sq_inline = ctx->sq_buffered;
	hot_addr = ctx->hot_read_addr;
	hot_fn = ctx->hot_read;
	hot_obj = ctx->hot_read_obj;
	ctx->jit_tables[2] = stamps;
	if (backend_kind == PAGES) {
		ctx->jit_tables[0] = ((Sh4X64Pages *) this)->read_table;
		ctx->jit_tables[1] = ((Sh4X64Pages *) this)->write_table;
	}
	/* FPSCR.SZ: the FPU's moves written for the size the block is seen
	 * with as it starts (fschg turns it over), checked by a guard at the
	 * first one and again after a handler (an lds may have changed it);
	 * when it differs, that move's handler, then out: the next block,
	 * from the instruction after it, written for the other size. The
	 * moves took a test and both sizes each (AIRTRIX's slave saves and
	 * restores the FPU's banks in pairs) */
	bool sz = ctx->regs.fpscr.bit.sz != 0, sz_seen = false;
	/* FPSCR.PR, SR.FD seen clear since the last handler (fpu_op ()) */
	pr_ok = fd_ok = false;

	/* A way out of the block, taken on c: the GPRs not in ctx written
	 * there first (apart, after the block) */
	auto exit_if = [&] (Cond c, Label &out) {
		if (rc.spills ().empty ()) {
			e.jcc (c, out);
			return;
		}
		Label &stub = label ();
		e.jcc (c, stub);
		cold_path (Cold::EXIT, stub, out);
	};

	/* The handler of instruction i, as the interpreter's loop calls it:
	 * the GPRs in ctx before, none kept after (it may change any); but an
	 * FPU's (keeps: none of the GPRs), on its slow path, which the fast
	 * one joins */
	auto handler = [&] (size_t i, bool keeps) {
		const Sh4Op &op = block->ops[i];
		const int32_t at = (int32_t) (2 * i);

		/* It may change FPSCR.PR, SR.FD: tested again */
		pr_ok = fd_ok = false;
		if (keeps)
			rc.store_dirty (e);
		else {
			rc.flush (e);
			sz_seen = false;
		}
		if (ctx->stats) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) sh4_stats_handler (ctx, block->pc + (uint32_t) at, op.inst));
			e.add64_mem_imm (RAX, 0, 1);
		}
		e.lea32 (RAX, RSI, at);
		e.store32 (RBX, o.pc, RAX);
		call_index (e, o, at);
		e.mov (RCX, RBX);
		e.mov_imm32 (RDX, op.inst);
		e.call ((const void *) op.handler);
		if (!keeps)
			rc.forget ();
		if (sh4_may_branch (op.inst)) {
			e.lea32 (RAX, RSI, at);
			e.cmp32_mem (RBX, o.pc, RAX);
			e.jcc (CC_NE, branched[i]);
			used[i] = true;
		}
		/* It may have raised an interrupt (a store, SR); changed the
		 * fetches' translation */
		if (i + 1 < n) {
			e.cmp8_mem_imm (RBX, o.irq_ready, 0);
			e.jcc (CC_NE, seq[i]);
			if (virt_mode) {
				e.load32 (RAX, RBX, o.mmu_gen);
				e.alu_reg_mem (Emitter::ALU_CMP, RAX, RBX, o.mmu_gen_entry);
				e.jcc (CC_NE, seq[i]);
			}
			used[i] = true;
		}
	};

	/* A delay slot's FPU work (slot_op ()) from jump i: FPSCR.PR, SR.FD
	 * tested before the jump, else the jump's handler (it runs the slot)
	 * and out; the slot then written without them */
	auto slot_guards = [&] (size_t i, uint16_t slot) {
		uint8_t scratch[2048];
		Emitter s (scratch, sizeof (scratch));
		const size_t was = cold.size ();
		const Sh4RegCache kept = rc;
		const bool counted = store_counted;
		Label slow;
		bool fallback = false, pr = pr_ok, fd = fd_ok;
		const bool fpu = fpu_op (s, o, rc, slot, slow, fallback, pr, fd);

		cold.resize (was);
		rc = kept;
		store_counted = counted;
		if (!fpu || (pr == pr_ok && fd == fd_ok))
			return;

		Label &other = label ();

		if (pr != pr_ok) {
			e.test_mem_imm (RBX, o.fpscr, 0x00080000);
			e.jcc (CC_NE, other);
			pr_ok = true;
		}
		if (fd != fd_ok) {
			e.test_mem_imm (RBX, o.sr, 0x00008000);
			e.jcc (CC_NE, other);
			fd_ok = true;
		}
		cold_path (Cold::BRANCH, other, seq[i], (int32_t) (2 * i), 0, block->ops[i].inst,
		           block->ops[i].handler);
		used[i] = true;
	};

	/* Prologue: as the unwind information says */
	mark (e, "entry: prologue");
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
	e.mov (RSI, RDX);
	body = e.here ();

	/* Its lines in the instruction cache, or fetched: the address known
	 * here, a comparison each; a line missing fetched apart (below) */
	const uint32_t icache_last = block->pc + 2 * block->num_words - 1;
	struct Fetch { uint32_t line; Label *fetch, *back; };
	std::vector<Fetch> fetches;

	mark (e, "entry: icache lines");
	for (uint32_t line = Sh4ICache::tag (block->pc);
	     ctx->icache_on && line <= Sh4ICache::tag (icache_last); line++) {
		Label &fetch = label (), &back = label ();

		e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.icache + (int32_t) (4 * (line & (Sh4ICache::NUM_LINES - 1))),
		               (int32_t) line);
		e.jcc (CC_NE, block->code ? fetch : icache_miss);
		e.bind (back);
		fetches.push_back (Fetch { line, &fetch, &back });
	}
	e.bind (icache_done);

	/* A delay loop, "dt Rn; bf" back to its start (SWRACER's master waits
	 * so 700 times a frame, 26 to 217 turns, each through sh4_chain ()
	 * and the idle loops' look): its turns but the last taken here at
	 * once, as running them would leave it: Rn down by one and two cycles
	 * each, T clear (the last dt sets it again). Nothing else runs
	 * meanwhile, no interrupt comes (no store); each turn with room in the
	 * slice, as a block's entry needs (more cycles than its instructions):
	 * (remaining - n - 1) / 2 of them, up to Rn - 1 (Rn 0: 2^32 turns).
	 * Then the last as written; its way back to the probe, not to the
	 * idle loops' look */
	if (block->mem_loop && !block->patched && !mmu_mode) {
		mark (e, "entry: memory delay loop");
		memory_loop (e, o, block);
	}
	if (block->delay_loop && !block->patched) {
		const unsigned rn = (block->ops[0].inst >> 8) & 15;
		const unsigned len = block->delay_len;
		Label bounded, none, room;

		mark (e, "entry: delay loop");
		e.load32 (RAX, RBX, o.remaining);
		e.alu32_imm (Emitter::ALU_SUB, RAX, (int32_t) n + 1);
		e.jcc (CC_LE, none);
		if (len == 2)
			e.shr32_imm (RAX, 1);
		else {
			/* The turns of len instructions with room */
			e.alu32_reg_reg (Emitter::ALU_XOR, RDX, RDX);
			e.mov_imm32 (RCX, len);
			e.div32 (RCX);
		}
		e.load32 (RCX, RBX, o.r[rn]);
		e.lea32 (RDX, RCX, -1);
		e.alu32_reg_reg (Emitter::ALU_CMP, RDX, RAX);
		e.jcc (CC_AE, bounded);
		e.mov32 (RAX, RDX);
		e.bind (bounded);
		e.test32 (RAX, RAX);
		e.jcc (CC_E, none);
		e.alu32_reg_reg (Emitter::ALU_SUB, RCX, RAX);
		e.store32 (RBX, o.r[rn], RCX);
		if (len == 2)
			e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RAX);
		else {
			e.mov_imm32 (RCX, len);
			e.imul32 (RAX, RCX);
		}
		e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, RAX);
		e.bind (none);
	}

	for (i = 0; i < n; i++) {
		const Sh4Op &op = block->ops[i];
		const int32_t at = (int32_t) (2 * i);
		Label slow;
		bool fallback = false;

		/* The slots written out (bt/s, a jump) take the size seen, or
		 * test it */
		move_sz = sz_seen ? (int) sz : -1;
		mark (e, "op: ALU written out");
		if (unsigned run = division_run (e, o, block, i)) {
			i += run - 1;
			continue;
		}
		if (unsigned run = access_group (e, o, block, i, seq, forced, sz_seen && !sz)) {
			for (size_t k = i; k < i + run; k++)
				used[k] = true;
			i += run - 1;
			continue;
		}
		if (native_op (e, o, op.inst)) {
			if (op.inst == 0xF3FD)	/* fschg */
				sz = !sz;
			continue;
		}

		if ((op.inst & 0xF000) == 0xF000 && (op.inst & 0xF00F) >= 0xF006 &&
		    (op.inst & 0xF00F) <= 0xF00C && !sz_seen) {
			/* The guard: SZ as seen, else the handler and out */
			Label &other = label ();

			mark (e, "op: FPSCR.SZ guard");
			e.test_mem_imm (RBX, o.fpscr, 0x00100000);
			e.jcc (sz ? CC_E : CC_NE, other);
			cold_path (Cold::HANDLER, other, seq[i], at, 0, op.inst, op.handler);
			used[i] = true;
			sz_seen = true;
			move_sz = (int) sz;
		}

		mark (e, "op: FPU");
		if (fpu_op (e, o, rc, op.inst, slow, fallback, pr_ok, fd_ok)) {
			if (fallback) {
				/* PR or FD set: its handler, then out (the next
				 * ones written for PR and FD clear) */
				Label after;

				e.jmp (after);
				e.bind (slow);
				handler (i, true);
				pr_ok = fd_ok = true;
				e.jmp (seq[i]);
				used[i] = true;
				e.bind (after);
			}
			continue;
		}

		mark (e, "memory: address");
		no_device = false;
		if (memory_op (e, o, op.inst, at, block->pc + (uint32_t) at)) {
			/* A device's register may have raised an interrupt */
			mark (e, "memory: interrupt check");
			if (i + 1 < n && !no_device) {
				e.cmp8_mem_imm (RBX, o.irq_ready, 0);
				exit_if (CC_NE, seq[i]);
				if (virt_mode) {
					e.load32 (RAX, RBX, o.mmu_gen);
					e.alu_reg_mem (Emitter::ALU_CMP, RAX, RBX, o.mmu_gen_entry);
					exit_if (CC_NE, seq[i]);
				}
				used[i] = true;
			}
			continue;
		}

		if ((op.inst & 0xFF00) == 0xC700) {
			/* mova @(disp,PC),R0: ((PC + 4) & ~3) + disp * 4 */
			uint32_t pc = block->pc + (uint32_t) at;

			e.lea32 (rc.write (e, 0), RSI, (int32_t) (((pc + 4) & ~3u) - block->pc) +
			                               (int32_t) ((op.inst & 0xFF) * 4));
			continue;
		}

		mark (e, "op: mova, branches");
		if ((op.inst & 0xFD00) == 0x8900) {
			/* bt, bf: to the target, T set or clear */
			Label stay;
			int32_t target = at + 4 + 2 * (int32_t) (int8_t) op.inst;

			{
				const Reg t = rc.read (e, Sh4RegCache::T);

				e.test32 (t, t);
			}
			e.jcc ((op.inst & 0x0200) ? CC_NE : CC_E, stay);
			e.lea32 (RAX, RSI, target);
			e.store32 (RBX, o.pc, RAX);
			rc.store_dirty (e);
			e.jmp (done[i]);
			used[i] = true;
			to[i] = target;
			e.bind (stay);
			continue;
		}

		if ((op.inst & 0xFD00) == 0x8D00 && i + 1 < n &&
		    slot_writable (o, block->ops[i + 1].inst)) {
			/* bt/s, bf/s: taken, the slot (the next instruction)
			 * written out here, then to the target; else on to it */
			Label stay;
			int32_t target = at + 4 + 2 * (int32_t) (int8_t) op.inst;
			Sh4RegCache staying;

			slot_guards (i, block->ops[i + 1].inst);
			{
				const Reg t = rc.read (e, Sh4RegCache::T);

				e.test32 (t, t);
			}
			e.jcc ((op.inst & 0x0200) ? CC_NE : CC_E, stay);
			staying = rc;
			{
				/* The slot again on the way not taken: counted
				 * there as well */
				bool counted = store_counted;

				slot_op (e, o, block->ops[i + 1].inst, at + 2);
				store_counted = counted;
			}
			e.lea32 (RAX, RSI, target);
			e.store32 (RBX, o.pc, RAX);
			rc.store_dirty (e);
			rc = staying;
			e.jmp (jumped[i]);
			jumped_used[i] = true;
			fixed[i] = true;
			to[i] = target;
			e.bind (stay);
			continue;
		}

		{
			bool fixed_to = false;
			int32_t target = 0;

			if (jump_writable (o, block, i))
				slot_guards (i, block->words[i + 1]);
			if (jump_op (e, o, block, i, jumped[i], fixed_to, target)) {
				jumped_used[i] = true;
				fixed[i] = fixed_to;
				to[i] = target;
				continue;
			}
		}

		if ((op.inst & 0xF0FF) == 0x0083 && !mmu_mode) {
			/* pref @Rn: nothing but at a store queue's address
			 * (sh4_interp_pref ()), its handler then */
			Label &none = label ();
			Reg rn;

			mark (e, "op: pref");
			rc.store_dirty (e);
			rn = rc.read (e, (op.inst >> 8) & 15);
			e.mov32 (RAX, rn);
			e.shr32_imm (RAX, 26);
			e.cmp32_imm (RAX, 0x38);
			e.jcc (CC_NE, none);
			{
				/* Neither FPSCR nor SR changed: the FPU's tests
				 * kept */
				const bool pr = pr_ok, fd = fd_ok;

				handler (i, true);
				pr_ok = pr;
				fd_ok = fd;
			}
			e.bind (none);
			continue;
		}

		mark (e, "op: handler call");
		handler (i, false);
	}
	/* The end of the block: on to the next instruction */
	mark (e, "exit: registers back");
	rc.store_dirty (e);
	e.jmp (seq[n - 1]);
	used[n - 1] = true;

	/* The exits: r8d the instructions run; a short way back (last the last
	 * one run, from esi) to the run loop's sh4_chain (), for the idle loops'
	 * look, unless the block stored (end), known here or measured; any
	 * other to the probe */
	auto way_back = [] (int32_t target, int32_t last) {
		return target <= last && last - target < (int32_t) Sh4::IdleLoop::MAX_SPAN;
	};
	auto measured = [&] (int32_t last) {
		e.load32 (RAX, RBX, o.pc);
		e.lea32 (RCX, RSI, last);
		e.alu32_reg_reg (Emitter::ALU_SUB, RCX, RAX);
		e.cmp32_imm (RCX, Sh4::IdleLoop::MAX_SPAN);
		e.jcc (CC_B, end);
		e.jmp (probe);
	};
	/* The chaining's counts for HR_PROFILE alone (two writes a block) */
	const bool counting = hl::Profile::enabled;

	/* Gone to already, or by a slow path written after the block */
	auto referenced = [&] (const Label &l) {
		if (l.num_few || !l.waiting.empty ())
			return true;
		for (const Cold &c : cold)
			if (c.to == &l || (c.kind == Cold::BRANCH && &branched[c.at / 2] == &l))
				return true;
		return false;
	};

	/* A way out to a fixed address, PC set: linked to its block (see
	 * "Links"), else the probe */
	const bool linking = !mmu_mode && ctx->chain.enabled && !ctx->icache_on && stubs[0].link;
	/* r8d the count (link_checks takes it off) */
	auto link_site = [&] () {
		e.call_near (stubs[0].link_checks);
		/* jmp rel32 to link, that form whatever the distance (link
		 * patches its field) */
		e.byte (0xE9);
		e.u32 ((uint32_t) (int32_t) (stubs[0].link - (e.here () + 4)));
	};
	auto link_exit = [&] (uint32_t count) {
		e.mov_imm32 (R8, count);
		if (!linking) {
			e.jmp (probe);
			return;
		}
		if (counting)
			e.add64_mem_imm (RBX, o.chain_ends, 1);
		link_site ();
	};
	/* A loop's way back to a fixed address, PC set: end's looks (below),
	 * then linked where end goes to the probe's checks */
	auto loop_exit = [&] (uint32_t count) {
		e.mov_imm32 (R8, count);
		if (!linking) {
			e.jmp (end);
			return;
		}
		Label &no_store = label (), &go = label (), &to_chain = label ();

		if (counting)
			e.add64_mem_imm (RBX, o.chain_ends, 1);
		e.load32 (RAX, RBX, o.stores);
		e.cmp32_mem (RBX, o.chain_stores, RAX);
		e.jcc (CC_E, no_store);
		e.mov_imm64 (RAX, (uint64_t) (uintptr_t) &block->simulated);
		e.cmp8_mem_imm (RAX, 0, 0);
		e.jcc (CC_NE, to_chain);
		e.jmp (go);
		e.bind (no_store);
		e.mov_imm64 (RAX, (uint64_t) (uintptr_t) &block->quiet);
		e.alu_mem_imm (Emitter::ALU_CMP, RAX, 0, 0);
		e.jcc (CC_LE, to_chain);
		e.add32_mem_imm (RAX, 0, -1);
		e.bind (go);
		link_site ();
		/* To sh4_chain (), the count taken off as end does */
		e.bind (to_chain);
		e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
		e.jmp (chain);
	};
	/* A way out to an address known as it runs, PC set, r8d the count:
	 * a loop's way back to end, else its last address kept (see
	 * link_dyn ()) */
	auto dyn_exit = [&] (int32_t last) {
		if (!linking) {
			measured (last);
			return;
		}
		Label &other = label ();
		uint8_t *cached_at, *jump, *stub;

		e.load32 (RAX, RBX, o.pc);
		e.lea32 (RCX, RSI, last);
		e.alu32_reg_reg (Emitter::ALU_SUB, RCX, RAX);
		e.cmp32_imm (RCX, Sh4::IdleLoop::MAX_SPAN);
		e.jcc (CC_B, end);
		if (counting)
			e.add64_mem_imm (RBX, o.chain_ends, 1);
		e.call_near (stubs[0].link_checks);
		cached_at = e.cmp32_imm_site (RSI, 1);
		e.jcc (CC_NE, other);
		jump = e.jmp_site ();
		e.bind (other);
		e.lea_rip (R9, cached_at);
		e.byte (0xE9);
		stub = e.here ();
		e.u32 ((uint32_t) (int32_t) (stubs[0].link_dyn - (stub + 4)));
		if (!e.overflowed () && (jump - cached_at != DYN_BODY || stub - cached_at != DYN_STUB))
			HL_ABORT ("sh4-x64: a way out's layout (%d, %d)", (int) (jump - cached_at),
			          (int) (stub - cached_at));
	};

	mark (e, "exit: ways out");
	for (i = 0; i < n; i++) {
		const int32_t last = (int32_t) (2 * i);

		/* Each way out written when something goes there (an access's
		 * interrupt check goes to seq[i] alone): they took a quarter of
		 * the translations' bytes */
		if (used[i] && (referenced (seq[i]) || forced[i])) {
			e.bind (seq[i]);
			e.lea32 (RAX, RSI, last + 2);
			e.store32 (RBX, o.pc, RAX);
			if (i + 1 == n)
				link_exit ((uint32_t) (i + 1));
			else {
				e.mov_imm32 (R8, (uint32_t) (i + 1));
				e.jmp (probe);
			}
		}
		if (used[i] && referenced (branched[i])) {
			e.bind (branched[i]);
			e.add32_mem_imm (RBX, o.pc, 2);
			e.mov_imm32 (R8, (uint32_t) (i + 1));
			dyn_exit (last);
		}
		if (used[i] && referenced (done[i])) {
			e.bind (done[i]);
			if (block->poll_loop && i == 2) {
				/* A wait on memory's way back: the slice's
				 * other turns off, then on (sh4_poll_skip ()) */
				e.mov (RCX, RBX);
				e.mov_imm32 (RDX, block->ops[0].inst);
				e.call ((const void *) sh4_poll_skip);
				e.mov_imm32 (R8, (uint32_t) (i + 1));
				e.test32 (RAX, RAX);
				e.jcc (CC_E, end);
				e.jmp (probe);
			} else if (way_back (to[i], last) &&
			           !(block->delay_loop && i == (size_t) block->delay_len - 1)) {
				loop_exit ((uint32_t) (i + 1));
			} else if (way_back (to[i], last)) {
				e.mov_imm32 (R8, (uint32_t) (i + 1));
				e.jmp (probe);
			} else
				link_exit ((uint32_t) (i + 1));
		}
		if (jumped_used[i]) {
			e.bind (jumped[i]);
			if (fixed[i] && !way_back (to[i], last + 2))
				link_exit ((uint32_t) (i + 2));
			else if (fixed[i])
				loop_exit ((uint32_t) (i + 2));
			else {
				e.mov_imm32 (R8, (uint32_t) (i + 2));
				dyn_exit (last + 2);
			}
		}
	}

	/* The probe, its checks, sh4_chain (): the stubs' (emit_stubs ()) */
	mark (e, "exit: probe");
	e.bind (probe);
	e.jmp_abs (stubs[mmu_mode].probe);
	e.bind (probe_checks);
	e.jmp_abs (stubs[mmu_mode].checks);
	e.bind (chain);
	e.jmp_abs (stubs[mmu_mode].chain);

	/* sh4_chain (): the next block's body, or back to the run loop with
	 * the last block's count */
	mark (e, "exit: idle loops' end");
	if (referenced (end)) {
		e.bind (end);
		e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
		if (counting)
			e.add64_mem_imm (RBX, o.chain_ends, 1);
		/* A loop that stored: no idle loop's look (idle_look () lets it go),
		 * the probe (it sends a turn noted on to sh4_chain ()). Its stores'
		 * low half: a block's never wraps */
		if (ctx->chain.enabled) {
			Label &no_store = label ();

			e.load32 (RAX, RBX, o.stores);
			e.cmp32_mem (RBX, o.chain_stores, RAX);
			e.jcc (CC_E, no_store);
			/* But a loop whose turns the idle loops simulate */
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) &block->simulated);
			e.cmp8_mem_imm (RAX, 0, 0);
			e.jcc (CC_NE, chain);
			e.jmp (probe_checks);
			e.bind (no_store);
			/* A loop the idle loops let be: its turns counted down here,
			 * no look (Sh4Block::quiet, idle_look ()) */
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) &block->quiet);
			e.alu_mem_imm (Emitter::ALU_CMP, RAX, 0, 0);
			e.jcc (CC_LE, chain);
			e.add32_mem_imm (RAX, 0, -1);
			e.jmp (probe_checks);
		}
	}
	/* Fetching its lines; nothing of the guest's in the registers yet.
	 * A line as sh4_icache_fill () fetches it, the cache on and the area
	 * cached (P1, P3; P0 and U0 without the MMU): its tag, its words from
	 * where the block's code lies (the same page), then a block run from
	 * a line gone looked at again (line_replaced ()). Else that call for
	 * all of them */
	mark (e, "entry: icache fetch");
	for (const Fetch &f : fetches) {
		if (!block->code)
			break;
		const unsigned index = f.line & (Sh4ICache::NUM_LINES - 1);
		const uint8_t *from = (const uint8_t *) block->code +
		                      (int32_t) ((f.line << Sh4ICache::LINE_BITS) - (block->pc & 0x1FFFFFFF));
		Label &replaced = label ();

		e.bind (*f.fetch);
		e.mov_imm64 (R8, (uint64_t) (uintptr_t) sh4_ccr_at (ctx));
		e.test_mem_imm (R8, 0, 0x100);
		e.jcc (CC_E, icache_miss);
		e.mov32 (RAX, RSI);
		e.shr32_imm (RAX, 29);
		e.cmp32_imm (RAX, 5);
		e.jcc (CC_E, icache_miss);
		e.cmp32_imm (RAX, 7);
		e.jcc (CC_E, icache_miss);
		e.store32_imm (RBX, o.icache + (int32_t) (4 * index), f.line);
		e.store8_imm (RBX, o.icache_held + (int32_t) index, 1);
		e.mov_imm64 (RAX, (uint64_t) (uintptr_t) from);
		for (int32_t k = 0; k < 32; k += 8) {
			e.load64 (RCX, RAX, k);
			e.store64 (RBX, o.icache_data + (int32_t) (32 * index) + k, RCX);
		}
		e.add64_mem_imm (RBX, o.icache_fetched, 1);
		e.load32 (RAX, RBX, o.held_epoch);
		e.alu_reg_mem (Emitter::ALU_CMP, RAX, RBX, o.epoch);
		e.jcc (CC_E, replaced);
		e.jmp (*f.back);
		e.bind (replaced);
		e.mov (RCX, RBX);
		e.mov_imm32 (RDX, index);
		e.call ((const void *) sh4_icache_replaced);
		e.jmp (*f.back);
	}
	mark (e, "entry: icache fill call");
	if (ctx->icache_on) {
		e.bind (icache_miss);
		e.mov (RCX, RBX);
		/* The lines at the block's own (physical) address, the area esi's */
		e.mov32 (RDX, RSI);
		e.alu32_imm (Emitter::ALU_AND, RDX, (int32_t) 0xE0000000);
		e.alu32_imm (Emitter::ALU_OR, RDX, (int32_t) (block->pc & 0x1FFFFFFF));
		e.mov_imm32 (R8, icache_last);
		e.call ((const void *) sh4_icache_fill);
		e.jmp (icache_done);
	}

	/* The slow paths, out of the way */
	mark (e, "slow paths");
	main_bytes += e.length ();
	for (const Cold &c : cold) {
		e.bind (*c.from);
		/* A store queue's word first: it calls nothing, the registers
		 * kept where they are */
		if (c.kind == Cold::WRITE && sq_inline)
			sq_store (e, o, c.size, *c.to);
		for (const auto &r : c.spill)
			if (r.second == o.sr) {
				e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~1);
				e.alu_mem_reg (Emitter::ALU_OR, RBX, o.sr, r.first);
			} else
				e.store32 (RBX, r.second, r.first);
		switch (c.kind) {
		case Cold::READ:
			call_read (e, o, c.at, c.size, c.slot);
			break;
		case Cold::WRITE:
			call_write (e, o, c.at, c.size, c.slot);
			break;
		case Cold::HANDLER:
			if (ctx->stats) {
				e.mov_imm64 (RAX, (uint64_t) (uintptr_t) sh4_stats_handler (ctx, block->pc + (uint32_t) c.at, c.inst));
				e.add64_mem_imm (RAX, 0, 1);
			}
			e.lea32 (RAX, RSI, c.at);
			e.store32 (RBX, o.pc, RAX);
			call_index (e, o, c.at);
			e.mov (RCX, RBX);
			e.mov_imm32 (RDX, c.inst);
			e.call ((const void *) c.fn);
			break;
		case Cold::BRANCH:
			if (ctx->stats) {
				e.mov_imm64 (RAX, (uint64_t) (uintptr_t) sh4_stats_handler (ctx, block->pc + (uint32_t) c.at, c.inst));
				e.add64_mem_imm (RAX, 0, 1);
			}
			e.lea32 (RAX, RSI, c.at);
			e.store32 (RBX, o.pc, RAX);
			call_index (e, o, c.at);
			e.mov (RCX, RBX);
			e.mov_imm32 (RDX, c.inst);
			e.call ((const void *) c.fn);
			e.lea32 (RAX, RSI, c.at);
			e.cmp32_mem (RBX, o.pc, RAX);
			e.jcc (CC_NE, branched[c.at / 2]);
			break;
		case Cold::GROUP:
			for (unsigned k = 0; k < c.size; k++) {
				const size_t op = (size_t) (c.at / 2) + k;
				const int32_t at = (int32_t) (2 * op);

				if (ctx->stats) {
					e.mov_imm64 (RAX, (uint64_t) (uintptr_t) sh4_stats_handler (ctx, block->pc + (uint32_t) at,
					                                                           block->ops[op].inst));
					e.add64_mem_imm (RAX, 0, 1);
				}
				e.lea32 (RAX, RSI, at);
				e.store32 (RBX, o.pc, RAX);
				call_index (e, o, at);
				e.mov (RCX, RBX);
				e.mov_imm32 (RDX, block->ops[op].inst);
				e.call ((const void *) block->ops[op].handler);
				if (op + 1 < n) {
					e.cmp8_mem_imm (RBX, o.irq_ready, 0);
					e.jcc (CC_NE, seq[op]);
				}
			}
			break;
		case Cold::EXIT:
			break;
		}
		e.jmp (*c.to);
	}
	cold.clear ();

	if (e.overflowed ())
		return NULL;
	arena.trim (buf + e.length ());
	code_bytes += e.length ();
	num_translated++;
	block->native_body = body;
	block->native_virtual = virt_mode;
	if (hl::Profile::sampling) {
		hl::Profile::code_written (buf, e.length (), ctx->config.master ? 'M' : 'S', block->pc,
		                           block->words, block->num_words, &marks);
		std::vector<std::pair<uint32_t, const char *>> m (marks);

		std::stable_sort (m.begin (), m.end (), [] (const std::pair<uint32_t, const char *> &a,
		                                            const std::pair<uint32_t, const char *> &b) {
			return a.first < b.first;
		});
		for (size_t k = 0; k < m.size (); k++) {
			const uint32_t to = k + 1 < m.size () ? m[k + 1].first : (uint32_t) e.length ();

			kind_bytes[m[k].second] += to - m[k].first;
		}
	}
	return (Sh4Native) buf;
}

Sh4Native
Sh4X64::get (Sh4 *ctx, Sh4Block *block)
{
	if (block->native && block->native_generation == generation)
		return (Sh4Native) block->native;
	if (block->patched || !arena.base ())
		return NULL;
	auto start = std::chrono::steady_clock::now ();
	/* No memory left for the translator's work (another program took it):
	 * the block interpreted this time */
	try {
		block->native = (void *) translate (ctx, block);
	} catch (const std::bad_alloc &) {
		HL_ERROR_FEW ("sh4: no memory left (another program took it?): a block not translated");
		return NULL;
	}
	if (!block->native) {
		/* The arena is full: everything goes, this block first again */
		clear ();
		block->native = (void *) translate (ctx, block);
		/* Too big for the bound even so: the interpreter's, for good
		 * (the arena not cleared again at each entry) */
		if (!block->native)
			block->patched = true;
	}
	block->native_generation = generation;
	translate_seconds += std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
	return (Sh4Native) block->native;
}
