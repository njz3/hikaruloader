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
 *	  taken), PC its target, then out (the count i + 2);
 *	- any other, its handler:
 *		lea eax, [rsi + 2i]; mov [rbx + PC], eax
 *		mov rcx, rbx; mov edx, inst; call handler
 *		(a branch)	lea eax, [rsi + 2i]; cmp [rbx + PC], eax; jne out_i
 *		(not the last)	cmp byte [rbx + irq_ready], 0; jne out_i
 *	out_i:	PC set (the next one's, or a branch's target), r8d = the
 *		instructions run (i + 1), to the end:
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
	start_arena ();
	clear_near ();
}

void
Sh4X64::clear_near ()
{
	for (unsigned i = 0; i < (1u << NEAR_BITS); i++)
		near_table[i] = Near { 1, 0, 0, 0, NULL, 0 };
}

void
Sh4X64::note (uint32_t pc, const Sh4Block *block, uint32_t epoch)
{
	Near &n = near_table[(pc >> 1) & ((1u << NEAR_BITS) - 1)];

	if (!block->checkable || block->checked != epoch || !body (block))
		return;
	n.pc = pc & 0x1FFFFFFF;
	n.checked = block->checked;
	n.count = (int32_t) block->ops.size ();
	n.body = block->native_body;
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
	sr = (int32_t) ((const uint8_t *) &ctx->regs.sr - base);
	gbr = (int32_t) ((const uint8_t *) &ctx->regs.gbr - base);
	remaining = (int32_t) ((const uint8_t *) &ctx->remaining - base);
	irq_ready = (int32_t) ((const uint8_t *) &ctx->irq_ready - base);
	stores = (int32_t) ((const uint8_t *) &ctx->stores - base);
	far_stores = (int32_t) ((const uint8_t *) &ctx->far_stores - base);
	for (unsigned i = 0; i < 16; i++)
		r[i] = (int32_t) ((const uint8_t *) &ctx->regs.r[i] - base);
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
		e.store32 (RBX, o->r[held[pick]], cached[pick]);
	held[pick] = (int8_t) n;
	dirty[pick] = false;
	used[pick] = ++clock;
	if (load)
		e.load32 (cached[pick], RBX, o->r[n]);
	return (int) pick;
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
			e.store32 (RBX, o->r[held[k]], cached[k]);
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

	for (unsigned k = 0; k < NUM; k++)
		if (held[k] >= 0 && dirty[k])
			v.r[v.n++] = std::make_pair (cached[k], o->r[held[k]]);
	return v;
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
Sh4X64::call_read (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	e.lea32 (R11, RSI, at);
	e.store32 (RBX, o.pc, R11);
	e.mov32 (RDX, RCX);
	e.mov (RCX, RBX);
	switch (size) {
	case 1: e.call ((const void *) sh4_read8s); break;
	case 2: e.call ((const void *) sh4_read16s); break;
	case 4: e.call ((const void *) sh4_read32); break;
	default: e.call ((const void *) sh4_read64); break;
	}
}

void
Sh4X64::call_write (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	e.lea32 (R11, RSI, at);
	e.store32 (RBX, o.pc, R11);
	e.mov (R8, R10);
	e.mov32 (RDX, RCX);
	e.mov (RCX, RBX);
	switch (size) {
	case 1: e.call ((const void *) sh4_write8); break;
	case 2: e.call ((const void *) sh4_write16); break;
	case 4: e.call ((const void *) sh4_write32); break;
	default: e.call ((const void *) sh4_write64); break;
	}
}

void
Sh4X64Calls::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	rc.store_dirty (e);
	call_read (e, o, at, size);
}

void
Sh4X64Calls::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	rc.store_dirty (e);
	call_write (e, o, at, size);
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

/* The access at r9 + rdx: a load to eax (sign extended) or rax, a store
 * of r10 */
static void
access_load (Emitter &e, unsigned size)
{
	switch (size) {
	case 1: e.load8s_index (RAX, R9, RDX, 1); break;
	case 2: e.load16s_index (RAX, R9, RDX, 1); break;
	case 4: e.load32_index (RAX, R9, RDX, 1); break;
	default: e.load64_index (RAX, R9, RDX, 1); break;
	}
}

static void
access_store (Emitter &e, unsigned size)
{
	switch (size) {
	case 1: e.store8_index (R9, RDX, 1, R10); break;
	case 2: e.store16_index (R9, RDX, 1, R10); break;
	case 4: e.store32_index (R9, RDX, 1, R10); break;
	default: e.store64_index (R9, RDX, 1, R10); break;
	}
}

Sh4X64Pages::Sh4X64Pages (const hl::Mmap *mmap) :
	Sh4X64 (PAGES, mmap),
	read_table (mmap->wide_read_table ()), write_table (mmap->wide_write_table ())
{
}

void
Sh4X64::stamp_check (Emitter &e, const Sh4Offsets &o, Label &slow)
{
	e.mov_imm64 (R8, (uint64_t) (uintptr_t) stamps);
	e.load32_index (R11, R8, RAX, 4);
	e.alu_reg_mem (Emitter::ALU_CMP, R11, RBX, o.epoch);
	e.jcc (CC_AE, slow);
}

/* r9 = the page's host address, from the table over the 32-bit addresses
 * (Mmap::wide_read_table (): none at P3 and P4, the aliases the same, so
 * no test of the area and no 29-bit mask, three instructions less an
 * access); edx the address's offset in the page. Aligned (an access across
 * two pages never). A store to code compared in this epoch, the slow path
 * (sh4_write () moves the epoch) */
void
Sh4X64Pages::lookup (Emitter &e, const Sh4Offsets &o, uint8_t * const *table, Label &slow,
                     unsigned size, bool store)
{
	if (size > 1) {
		e.test32_imm (RCX, size - 1);
		e.jcc (CC_NE, slow);
	}
	if (store) {
		e.mov32 (RAX, RCX);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.shr32_imm (RAX, Sh4BlockCache::STAMP_BITS);
		stamp_check (e, o, slow);
	}
	e.mov32 (RAX, RCX);
	e.shr32_imm (RAX, hl::Mmap::TABLE_PAGE_BITS);
	e.mov_imm64 (R8, (uint64_t) (uintptr_t) table);
	e.load64_index (R9, R8, RAX, 8);
	e.test64 (R9, R9);
	e.jcc (CC_E, slow);
	e.mov32 (RDX, RCX);
	e.and32_imm (RDX, (1u << hl::Mmap::TABLE_PAGE_BITS) - 1);
}

void
Sh4X64Pages::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	lookup (e, o, read_table, slow, size, false);
	access_load (e, size);
	e.bind (back);
	slow_path (slow, back, false, at, size);
}

void
Sh4X64Pages::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label &slow = label (), &back = label ();

	lookup (e, o, write_table, slow, size, true);
	access_store (e, size);
	count_store (e, o);
	e.bind (back);
	slow_path (slow, back, true, at, size);
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

	if ((inst & 0xF000) == 0xF000)
		return fpu_move_op (e, o, inst, at);

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
		const uint8_t *host = literal (pc + (uint32_t) from_esi - (uint32_t) at, 4);

		if (host) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) host);
			e.load32 (RAX, RAX, 0);
		} else {
			e.lea32 (RCX, RSI, from_esi);
			load (e, o, at, 4);
		}
		result (n);
		return true;
	}
	case 0x9000: {				/* mov.w @(disp,PC),Rn */
		int32_t from_esi = at + 4 + (int32_t) ((inst & 0xFF) * 2);
		const uint8_t *host = literal (pc + (uint32_t) from_esi - (uint32_t) at, 2);

		if (host) {
			e.mov_imm64 (RAX, (uint64_t) (uintptr_t) host);
			e.movsx16_mem (RAX, RAX, 0);
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
		address (n, -4, false);
		e.load32 (R10, RBX, o.pr);
		store (e, o, at, 4);
		step (n, -4);
		return true;
	case 0x4026:				/* lds.l @Rn+,PR */
		address (n, 0, false);
		load (e, o, at, 4);
		e.store32 (RBX, o.pr, RAX);
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

/* T = the condition, from the flags of the instruction just written */
static void
set_t (Emitter &e, const Sh4Offsets &o, Cond c)
{
	e.setcc (c, RAX);
	e.movzx8 (RAX, RAX);
	e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~1);
	e.alu_mem_reg (Emitter::ALU_OR, RBX, o.sr, RAX);
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

	if (inst == 0x0009)			/* nop */
		return true;
	if (inst == 0x0008 || inst == 0x0018) {	/* clrt, sett */
		if (inst == 0x0008)
			e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~1);
		else
			e.alu_mem_imm (Emitter::ALU_OR, RBX, o.sr, 1);
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
		set_t (e, o, CC_E);
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
		set_t (e, o, CC_E);
		return true;
	}
	switch (inst & 0xF00F) {
	case 0x6003:				/* mov Rm,Rn */
		a = rc.read (e, m);
		d = rc.write (e, n);
		if (d != a)
			e.mov32 (d, a);
		return true;
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
		set_t (e, o, CC_E);
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
		set_t (e, o, conds[inst & 7]);
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
	}
	switch (inst & 0xF0FF) {
	case 0x4010:				/* dt Rn */
		e.alu32_imm (Emitter::ALU_SUB, rc.modify (e, n), 1);
		set_t (e, o, CC_E);
		return true;
	case 0x4000:				/* shll Rn */
		e.shl32_imm (rc.modify (e, n), 1);
		set_t (e, o, CC_B);
		return true;
	case 0x4001:				/* shlr Rn */
		e.shr32_imm (rc.modify (e, n), 1);
		set_t (e, o, CC_B);
		return true;
	case 0x4008: e.shl32_imm (rc.modify (e, n), 2); return true;	/* shll2 */
	case 0x4018: e.shl32_imm (rc.modify (e, n), 8); return true;	/* shll8 */
	case 0x4028: e.shl32_imm (rc.modify (e, n), 16); return true;	/* shll16 */
	case 0x4009: e.shr32_imm (rc.modify (e, n), 2); return true;	/* shlr2 */
	case 0x4019: e.shr32_imm (rc.modify (e, n), 8); return true;	/* shlr8 */
	case 0x4029: e.shr32_imm (rc.modify (e, n), 16); return true;	/* shlr16 */
	case 0x0029:				/* movt Rn */
		d = rc.write (e, n);
		e.load32 (d, RBX, o.sr);
		e.and32_imm (d, 1);
		return true;
	case 0x4024:				/* rotcl Rn: through T */
	case 0x4025:				/* rotcr Rn */
		d = rc.modify (e, n);
		e.bt_mem_imm (RBX, o.sr, 0);
		e.rot32 ((inst & 1) ? Emitter::ROT_RCR : Emitter::ROT_RCL, d, 1);
		set_t (e, o, CC_B);
		return true;
	case 0x4004:				/* rotl Rn: T the bit out */
	case 0x4005:				/* rotr Rn */
	case 0x4021:				/* shar Rn */
		e.rot32 ((inst & 0x20) ? Emitter::ROT_SAR :
		         (inst & 1) ? Emitter::ROT_ROR : Emitter::ROT_ROL, rc.modify (e, n), 1);
		set_t (e, o, CC_B);
		return true;
	case 0x002A:				/* sts PR,Rn */
		e.load32 (rc.write (e, n), RBX, o.pr);
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
		set_t (e, o, (inst & 4) ? CC_G : CC_GE);
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

/* T = eax (0 or 1) */
static void
put_t (Emitter &e, const Sh4Offsets &o)
{
	e.alu_mem_imm (Emitter::ALU_AND, RBX, o.sr, ~1);
	e.alu_mem_reg (Emitter::ALU_OR, RBX, o.sr, RAX);
}

/* The FPU's arithmetic in single precision, as the handlers have it
 * (sh-insns-interp.h), in SSE and in their order: the same bits (no fused
 * multiply-add in the handlers, built without /arch; the same rounding,
 * the host's). FPSCR.PR set (doubles), or SR.FD where the handler looks at
 * it: to slow, the handler then (fallback set). false, nothing written, for
 * the others (FSCA, FSRRA: the handlers) */
static bool
fpu_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, Label &slow, bool &fallback)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	unsigned k, r;

	auto fr = [&] (unsigned i) { return o.fr + 4 * (int32_t) (i ^ 1); };
	auto xf = [&] (unsigned i) { return o.xf + 4 * (int32_t) (i ^ 1); };
	/* FPSCR.PR clear; SR.FD clear */
	auto singles = [&] () {
		e.test_mem_imm (RBX, o.fpscr, 0x00080000);
		e.jcc (CC_NE, slow);
		fallback = true;
	};
	auto enabled = [&] () {
		e.test_mem_imm (RBX, o.sr, 0x00008000);
		e.jcc (CC_NE, slow);
		fallback = true;
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
		put_t (e, o);
		return true;
	}
	case 0xF005:				/* fcmp/gt FRm,FRn: FRn > FRm */
		singles ();
		e.movss_load (0, RBX, fr (n));
		e.ucomiss_mem (0, RBX, fr (m));
		e.setcc (CC_A, RAX);
		e.movzx8 (RAX, RAX);
		put_t (e, o);
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
 * or from memory; none relative to PC (the interpreter's PC is the jump's
 * target in the slot), no jump. Tried on a scratch buffer */
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
	cold.resize (was);
	rc = kept;
	store_counted = counted;
	return ok;
}

void
Sh4X64::slot_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at)
{
	if (!native_op (e, o, inst))
		memory_op (e, o, inst, at, 0);
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

	if (i + 1 != block->ops.size () || !block->slot_kept ||
	    !slot_writable (o, block->words[i + 1]))
		return false;
	switch (inst & 0xF000) {
	case 0xA000:				/* bra */
	case 0xB000:				/* bsr */
		break;
	default:
		switch (inst & 0xF0FF) {
		case 0x402B:			/* jmp @Rn */
		case 0x400B:			/* jsr @Rn */
		case 0x0003:			/* bsrf Rn */
		case 0x0023:			/* braf Rn */
			break;
		default:
			if (inst != 0x000B)	/* rts */
				return false;
		}
	}

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
	/* Where a branch written out goes, from esi: the ends whose target is
	 * known (bt, bf; bt/s, bf/s, bra, bsr) */
	int32_t to[Sh4Block::MAX_OPS] = {};
	bool fixed[Sh4Block::MAX_OPS] = {};
	Label end, probe, probe_checks, chain, leave, icache_miss, icache_done;
	uint8_t *buf, *body;
	size_t i;

	buf = arena.take (max, 16);
	if (!buf)
		return NULL;
	Emitter e (buf, max);
	translating = ctx->mmap;
	stamps = ctx->blocks.stamps ();
	cold.clear ();
	labels_used = 0;
	rc.reset (&o);
	store_counted = false;
	/* FPSCR.SZ: the FPU's moves written for the size the block is seen
	 * with as it starts (fschg turns it over), checked by a guard at the
	 * first one and again after a handler (an lds may have changed it);
	 * when it differs, that move's handler, then out: the next block,
	 * from the instruction after it, written for the other size. The
	 * moves took a test and both sizes each (AIRTRIX's slave saves and
	 * restores the FPU's banks in pairs) */
	bool sz = ctx->regs.fpscr.bit.sz != 0, sz_seen = false;

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

		if (keeps)
			rc.store_dirty (e);
		else {
			rc.flush (e);
			sz_seen = false;
		}
		e.lea32 (RAX, RSI, at);
		e.store32 (RBX, o.pc, RAX);
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
		/* It may have raised an interrupt (a store, SR) */
		if (i + 1 < n) {
			e.cmp8_mem_imm (RBX, o.irq_ready, 0);
			e.jcc (CC_NE, seq[i]);
			used[i] = true;
		}
	};

	/* Prologue: as the unwind information says */
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
	 * here, a comparison each */
	const uint32_t icache_last = block->pc + 2 * block->num_words - 1;
	for (uint32_t line = Sh4ICache::tag (block->pc); line <= Sh4ICache::tag (icache_last); line++) {
		e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.icache + (int32_t) (4 * (line & (Sh4ICache::NUM_LINES - 1))),
		               (int32_t) line);
		e.jcc (CC_NE, icache_miss);
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
	if (block->delay_loop && !block->patched) {
		const unsigned rn = (block->ops[0].inst >> 8) & 15;
		Label bounded, none;

		e.load32 (RAX, RBX, o.remaining);
		e.alu32_imm (Emitter::ALU_SUB, RAX, (int32_t) n + 1);
		e.shr32_imm (RAX, 1);
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
		e.alu32_reg_reg (Emitter::ALU_ADD, RAX, RAX);
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
		if (native_op (e, o, op.inst)) {
			if (op.inst == 0xF3FD)	/* fschg */
				sz = !sz;
			continue;
		}

		if ((op.inst & 0xF000) == 0xF000 && (op.inst & 0xF00F) >= 0xF006 &&
		    (op.inst & 0xF00F) <= 0xF00C && !sz_seen) {
			/* The guard: SZ as seen, else the handler and out */
			Label &other = label ();

			e.test_mem_imm (RBX, o.fpscr, 0x00100000);
			e.jcc (sz ? CC_E : CC_NE, other);
			cold_path (Cold::HANDLER, other, seq[i], at, 0, op.inst, op.handler);
			used[i] = true;
			sz_seen = true;
			move_sz = (int) sz;
		}

		if (fpu_op (e, o, op.inst, slow, fallback)) {
			if (fallback) {
				Label after;

				e.jmp (after);
				e.bind (slow);
				handler (i, true);
				e.bind (after);
			}
			continue;
		}

		if (memory_op (e, o, op.inst, at, block->pc + (uint32_t) at)) {
			/* A device's register may have raised an interrupt */
			if (i + 1 < n) {
				e.cmp8_mem_imm (RBX, o.irq_ready, 0);
				exit_if (CC_NE, seq[i]);
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

		if ((op.inst & 0xFD00) == 0x8900) {
			/* bt, bf: to the target, T set or clear */
			Label stay;
			int32_t target = at + 4 + 2 * (int32_t) (int8_t) op.inst;

			e.test8_mem_imm (RBX, o.sr, 1);
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

			e.test8_mem_imm (RBX, o.sr, 1);
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

			if (jump_op (e, o, block, i, jumped[i], fixed_to, target)) {
				jumped_used[i] = true;
				fixed[i] = fixed_to;
				to[i] = target;
				continue;
			}
		}

		handler (i, false);
	}
	/* The end of the block: on to the next instruction */
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

	for (i = 0; i < n; i++) {
		const int32_t last = (int32_t) (2 * i);

		if (used[i]) {
			e.bind (seq[i]);
			e.lea32 (RAX, RSI, last + 2);
			e.store32 (RBX, o.pc, RAX);
			e.mov_imm32 (R8, (uint32_t) (i + 1));
			e.jmp (probe);
			e.bind (branched[i]);
			e.add32_mem_imm (RBX, o.pc, 2);
			e.mov_imm32 (R8, (uint32_t) (i + 1));
			measured (last);
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
			} else {
				e.mov_imm32 (R8, (uint32_t) (i + 1));
				e.jmp (way_back (to[i], last) && !(block->delay_loop && i == 1) ? end : probe);
			}
		}
		if (jumped_used[i]) {
			e.bind (jumped[i]);
			e.mov_imm32 (R8, (uint32_t) (i + 2));
			if (fixed[i])
				e.jmp (way_back (to[i], last + 2) ? end : probe);
			else
				measured (last + 2);
		}
	}

	/* The probe: the near table's entry for PC, its translation's body
	 * gone on to when the run loop would do no more (sh4_chain ()): its
	 * code compared in this epoch, the slice with room for it, no
	 * interrupt to take, no idle loop noting, the MMU off, the CPU
	 * running. Else sh4_chain () (the near table filled there) */
	e.bind (probe);
	e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
	if (counting)
		e.add64_mem_imm (RBX, o.chain_ends, 1);
	if (ctx->chain.enabled) {
		e.bind (probe_checks);
		e.load32 (RAX, RBX, o.pc);
		e.and32_imm (RAX, 0x1FFFFFFF);
		e.mov32 (RCX, RAX);
		e.shr32_imm (RCX, 1);
		e.and32_imm (RCX, (1u << NEAR_BITS) - 1);
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
		e.cmp8_mem_imm (RBX, o.mmu_on, 0);
		e.jcc (CC_NE, chain);
		e.alu_mem_imm (Emitter::ALU_CMP, RBX, o.state, (int32_t) hl::CPU_STATE_RUN);
		e.jcc (CC_NE, chain);
		e.load64 (RAX, RBX, o.stores);
		e.store64 (RBX, o.chain_stores, RAX);
		if (counting)
			e.add64_mem_imm (RBX, o.chain_gone, 1);
		e.load32 (RSI, RBX, o.pc);
		e.load64 (RAX, RDX, 16);
		e.jmp_reg (RAX);
	}
	e.jmp (chain);

	/* sh4_chain (): the next block's body, or back to the run loop with
	 * the last block's count */
	e.bind (end);
	e.alu_mem_reg (Emitter::ALU_SUB, RBX, o.remaining, R8);
	if (counting)
		e.add64_mem_imm (RBX, o.chain_ends, 1);
	/* A loop that stored: no idle loop's look (idle_look () lets it go),
	 * the probe (it sends a turn noted on to sh4_chain ()). Its stores'
	 * low half: a block's never wraps */
	if (ctx->chain.enabled) {
		e.load32 (RAX, RBX, o.stores);
		e.cmp32_mem (RBX, o.chain_stores, RAX);
		e.jcc (CC_NE, probe_checks);
		/* A loop the idle loops let be: its turns counted down here,
		 * no look (Sh4Block::quiet, idle_look ()) */
		e.mov_imm64 (RAX, (uint64_t) (uintptr_t) &block->quiet);
		e.alu_mem_imm (Emitter::ALU_CMP, RAX, 0, 0);
		e.jcc (CC_LE, chain);
		e.add32_mem_imm (RAX, 0, -1);
		e.jmp (probe_checks);
	}
	e.bind (chain);
	e.mov (RCX, RBX);
	e.mov32 (RDX, RSI);
	e.call ((const void *) sh4_chain);
	e.test64 (RAX, RAX);
	e.jcc (CC_E, leave);
	e.load32 (RSI, RBX, o.pc);
	e.jmp_reg (RAX);
	e.bind (leave);
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

	/* Fetching its lines; nothing of the guest's in the registers yet */
	e.bind (icache_miss);
	e.mov (RCX, RBX);
	e.mov32 (RDX, RSI);
	e.mov_imm32 (R8, icache_last);
	e.call ((const void *) sh4_icache_fill);
	e.jmp (icache_done);

	/* The slow paths, out of the way */
	for (const Cold &c : cold) {
		e.bind (*c.from);
		for (const auto &r : c.spill)
			e.store32 (RBX, r.second, r.first);
		switch (c.kind) {
		case Cold::READ:
			call_read (e, o, c.at, c.size);
			break;
		case Cold::WRITE:
			call_write (e, o, c.at, c.size);
			break;
		case Cold::HANDLER:
			e.lea32 (RAX, RSI, c.at);
			e.store32 (RBX, o.pc, RAX);
			e.mov (RCX, RBX);
			e.mov_imm32 (RDX, c.inst);
			e.call ((const void *) c.fn);
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
	num_translated++;
	block->native_body = body;
	if (hl::Profile::sampling)
		hl::Profile::code_written (buf, e.length (), ctx->config.master ? 'M' : 'S', block->pc,
		                           block->words, block->num_words);
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
	block->native = (void *) translate (ctx, block);
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
