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
 *	- any other, its handler:
 *		lea eax, [rsi + 2i]; mov [rbx + PC], eax
 *		mov rcx, rbx; mov edx, inst; call handler
 *		(a branch)	lea eax, [rsi + 2i]; cmp [rbx + PC], eax; jne out_i
 *		(not the last)	cmp byte [rbx + pending], 0; je next
 *				mov rcx, rbx; call irq_ready; test al, al; jnz out_i
 *	out_i:	PC set (the next one's, or a branch's target), the cycles
 *		counted, eax = i + 1; add rsp, 40; pop rsi; pop rbx; ret
 *
 * The branches' handlers leave PC at their target less 2, as for the
 * interpreter's loop: + 2 then. */

#include "cpu/sh/sh4-x64.h"
#include "cpu/sh/sh4.h"
#include "hl/mmap.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace x64;

/* An interrupt is waiting, and the CPU would take it now (as
 * sh4_process_irqs () does) */
static bool
irq_ready (Sh4 *ctx)
{
	return ctx->intc.pending &&
	       ctx->intc.irqs[ctx->intc.index].priority > ctx->regs.sr.bit.i;
}

/* The functions' generations, all backends together: a block keeps one
 * of a backend gone, it is not taken for the new one's */
static unsigned generations = 0;

Sh4X64::Sh4X64 (Kind kind, const hl::Mmap *mmap) :
	backend_kind (kind), layout (mmap->layout ()), arena (ARENA_SIZE),
	generation (++generations)
{
	start_arena ();
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

/* The shared unwind information first: the prologue pushes rbx and rsi,
 * then takes 40 bytes (Windows' "x64 exception handling" UNWIND_INFO) */
bool
Sh4X64::start_arena ()
{
	static const uint8_t unwind[] = {
		0x01,		/* version 1, no flags */
		9,		/* prologue size */
		3,		/* unwind codes */
		0x00,		/* no frame register */
		9, 0x42,	/* at 9: UWOP_ALLOC_SMALL, (40 - 8) / 8 */
		2, 0x60,	/* at 2: UWOP_PUSH_NONVOL rsi */
		1, 0x30,	/* at 1: UWOP_PUSH_NONVOL rbx */
		0, 0,		/* padding to an even count */
	};
	uint8_t *at;

	arena.reset ();
	if (!arena.base ())
		return false;
	at = arena.take (sizeof (unwind), 4);
	memcpy (at, unwind, sizeof (unwind));
	unwind_rva = (uint32_t) (at - arena.base ());
	return true;
}

void
Sh4X64::clear ()
{
#ifdef _WIN32
	for (void *table : tables)
		RtlDeleteFunctionTable ((PRUNTIME_FUNCTION) table);
#endif
	tables.clear ();
	generation = ++generations;
	start_arena ();
}

void
Sh4X64::register_unwind (uint8_t *fn, size_t size)
{
#ifdef _WIN32
	RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *) arena.take (sizeof (RUNTIME_FUNCTION), 4);

	if (!rf)
		return;
	rf->BeginAddress = (DWORD) (fn - arena.base ());
	rf->EndAddress = (DWORD) (fn + size - arena.base ());
	rf->UnwindData = unwind_rva;
	if (RtlAddFunctionTable (rf, 1, (DWORD64) arena.base ()))
		tables.push_back (rf);
#else
	(void) fn;
	(void) size;
#endif
}

Sh4Offsets::Sh4Offsets (Sh4 *ctx)
{
	const uint8_t *base = (const uint8_t *) ctx;

	pc = (int32_t) ((const uint8_t *) &ctx->regs.pc - base);
	sr = (int32_t) ((const uint8_t *) &ctx->regs.sr - base);
	gbr = (int32_t) ((const uint8_t *) &ctx->regs.gbr - base);
	remaining = (int32_t) ((const uint8_t *) &ctx->remaining - base);
	pending = (int32_t) ((const uint8_t *) &ctx->intc.pending - base);
	stores = (int32_t) ((const uint8_t *) &ctx->stores - base);
	far_stores = (int32_t) ((const uint8_t *) &ctx->far_stores - base);
	for (unsigned i = 0; i < 16; i++)
		r[i] = (int32_t) ((const uint8_t *) &ctx->regs.r[i] - base);
	fr = (int32_t) ((const uint8_t *) &ctx->regs.f - base);
	xf = (int32_t) ((const uint8_t *) &ctx->regs.x - base);
	fpscr = (int32_t) ((const uint8_t *) &ctx->regs.fpscr - base);
	fpul = (int32_t) ((const uint8_t *) &ctx->regs.fpul - base);
}

/* The memory accesses */

/* A store away from the stack (ecx its address, beyond 512 bytes of r15),
 * counted as sh4_put () does */
void
Sh4X64::count_far (Emitter &e, const Sh4Offsets &o)
{
	Label on_stack;

	e.mov32 (RAX, RCX);
	e.alu_reg_mem (Emitter::ALU_SUB, RAX, RBX, o.r[15]);
	e.lea32 (RAX, RAX, 0x200);
	e.cmp32_imm (RAX, 0x400);
	e.jcc (CC_B, on_stack);
	e.add64_mem_imm (RBX, o.far_stores, 1);
	e.bind (on_stack);
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
	call_read (e, o, at, size);
}

void
Sh4X64Calls::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
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
	read_table (mmap->read_table ()), write_table (mmap->write_table ())
{
}

/* r9 = the page's host address, from the table; edx the address, to its
 * offset in the page */
void
Sh4X64Pages::lookup (Emitter &e, uint8_t * const *table, Label &slow, unsigned size)
{
	straight_address (e, slow, size);
	e.mov32 (RAX, RDX);
	e.shr32_imm (RAX, hl::Mmap::TABLE_PAGE_BITS);
	e.mov_imm64 (R8, (uint64_t) (uintptr_t) table);
	e.load64_index (R9, R8, RAX, 8);
	e.test64 (R9, R9);
	e.jcc (CC_E, slow);
	e.and32_imm (RDX, (1u << hl::Mmap::TABLE_PAGE_BITS) - 1);
}

void
Sh4X64Pages::load (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label slow, done;

	lookup (e, read_table, slow, size);
	access_load (e, size);
	e.jmp (done);
	e.bind (slow);
	call_read (e, o, at, size);
	e.bind (done);
}

void
Sh4X64Pages::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label slow, done;

	lookup (e, write_table, slow, size);
	access_store (e, size);
	e.add64_mem_imm (RBX, o.stores, 1);
	count_far (e, o);
	e.jmp (done);
	e.bind (slow);
	call_write (e, o, at, size);
	e.bind (done);
}

Sh4X64Fastmem::Sh4X64Fastmem (const hl::Mmap *mmap) :
	Sh4X64 (FASTMEM, mmap),
	base (mmap->fastmem_base ()), readable (mmap->fastmem_readable ()),
	writable (mmap->fastmem_writable ())
{
}

/* edx the address, its 64 KB granule's bit set in the map; r9 the base */
void
Sh4X64Fastmem::check (Emitter &e, const uint8_t *bits, Label &slow, unsigned size)
{
	straight_address (e, slow, size);
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
	Label slow, done;

	check (e, readable, slow, size);
	access_load (e, size);
	e.jmp (done);
	e.bind (slow);
	call_read (e, o, at, size);
	e.bind (done);
}

void
Sh4X64Fastmem::store (Emitter &e, const Sh4Offsets &o, int32_t at, unsigned size)
{
	Label slow, done;

	check (e, writable, slow, size);
	access_store (e, size);
	e.add64_mem_imm (RBX, o.stores, 1);
	count_far (e, o);
	e.jmp (done);
	e.bind (slow);
	call_write (e, o, at, size);
	e.bind (done);
}

/* The FPU's moves (fmov, sh-insns-interp.h): 32 bits, a single, unless
 * FPSCR.SZ, 64 bits, a pair; the singles at n ^ 1 (FR ()), a pair's words
 * swapped (MOVE64), XDn for an odd n. SZ tested as they run */
bool
Sh4X64::fpu_move_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	const int32_t rn = o.r[n], rm = o.r[m], r0 = o.r[0];
	const unsigned kind = inst & 0xF00F;
	Label pair, done;
	unsigned sz;

	if (kind < 0xF006 || kind > 0xF00C)
		return false;

	auto single = [&] (unsigned r) { return o.fr + 4 * (int32_t) (r ^ 1); };
	auto pair_at = [&] (unsigned r) { return ((r & 1) ? o.xf : o.fr) + 8 * (int32_t) (r >> 1); };

	e.test_mem_imm (RBX, o.fpscr, 0x00100000);
	e.jcc (CC_NE, pair);
	for (sz = 0; sz < 2; sz++) {
		const unsigned size = sz ? 8 : 4;

		if (sz)
			e.bind (pair);
		switch (kind) {
		case 0xF008:			/* fmov @Rm,FRn */
		case 0xF009:			/* fmov @Rm+,FRn */
		case 0xF006:			/* fmov @(R0,Rm),FRn */
			e.load32 (RCX, RBX, rm);
			if (kind == 0xF006)
				e.alu_reg_mem (Emitter::ALU_ADD, RCX, RBX, r0);
			load (e, o, at, size);
			if (sz) {
				e.ror64_imm (RAX, 32);
				e.store64 (RBX, pair_at (n), RAX);
			} else
				e.store32 (RBX, single (n), RAX);
			if (kind == 0xF009)
				e.add32_mem_imm (RBX, rm, (int32_t) size);
			break;
		case 0xF00A:			/* fmov FRm,@Rn */
		case 0xF00B:			/* fmov FRm,@-Rn */
		case 0xF007:			/* fmov FRm,@(R0,Rn) */
			e.load32 (RCX, RBX, rn);
			if (kind == 0xF007)
				e.alu_reg_mem (Emitter::ALU_ADD, RCX, RBX, r0);
			if (kind == 0xF00B)
				e.lea32 (RCX, RCX, -(int32_t) size);
			if (sz) {
				e.load64 (R10, RBX, pair_at (m));
				e.ror64_imm (R10, 32);
			} else
				e.load32 (R10, RBX, single (m));
			store (e, o, at, size);
			if (kind == 0xF00B)
				e.add32_mem_imm (RBX, rn, -(int32_t) size);
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
		if (!sz)
			e.jmp (done);
	}
	e.bind (done);
	return true;
}

/* The moves to and from memory, as their handlers have them
 * (sh-insns-interp.h): the address to ecx, the value to r10, the access,
 * the registers after. false for the others */
bool
Sh4X64::memory_op (Emitter &e, const Sh4Offsets &o, uint16_t inst, int32_t at, uint32_t pc)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	const int32_t rn = o.r[n], rm = o.r[m], r0 = o.r[0];
	/* mov.b, mov.w, mov.l: the size from the low bits */
	static const unsigned sizes[4] = { 1, 2, 4, 4 };
	const unsigned size = sizes[inst & 3];

	if ((inst & 0xF000) == 0xF000)
		return fpu_move_op (e, o, inst, at);

	switch (inst & 0xF00F) {
	case 0x6000:				/* mov.b @Rm,Rn */
	case 0x6001:				/* mov.w @Rm,Rn */
	case 0x6002:				/* mov.l @Rm,Rn */
		e.load32 (RCX, RBX, rm);
		load (e, o, at, size);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x6004:				/* mov.b @Rm+,Rn */
	case 0x6005:				/* mov.w @Rm+,Rn */
	case 0x6006:				/* mov.l @Rm+,Rn */
		e.load32 (RCX, RBX, rm);
		load (e, o, at, size);
		e.store32 (RBX, rn, RAX);
		if (n != m)
			e.add32_mem_imm (RBX, rm, (int32_t) size);
		return true;
	case 0x2000:				/* mov.b Rm,@Rn */
	case 0x2001:				/* mov.w Rm,@Rn */
	case 0x2002:				/* mov.l Rm,@Rn */
		e.load32 (RCX, RBX, rn);
		e.load32 (R10, RBX, rm);
		store (e, o, at, size);
		return true;
	case 0x2004:				/* mov.b Rm,@-Rn */
	case 0x2005:				/* mov.w Rm,@-Rn */
	case 0x2006:				/* mov.l Rm,@-Rn */
		e.load32 (RCX, RBX, rn);
		e.lea32 (RCX, RCX, -(int32_t) size);
		e.load32 (R10, RBX, rm);
		store (e, o, at, size);
		e.add32_mem_imm (RBX, rn, -(int32_t) size);
		return true;
	case 0x000C:				/* mov.b @(R0,Rm),Rn */
	case 0x000D:				/* mov.w @(R0,Rm),Rn */
	case 0x000E:				/* mov.l @(R0,Rm),Rn */
		e.load32 (RCX, RBX, rm);
		e.alu_reg_mem (Emitter::ALU_ADD, RCX, RBX, r0);
		load (e, o, at, size);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x0004:				/* mov.b Rm,@(R0,Rn) */
	case 0x0005:				/* mov.w Rm,@(R0,Rn) */
	case 0x0006:				/* mov.l Rm,@(R0,Rn) */
		e.load32 (RCX, RBX, rn);
		e.alu_reg_mem (Emitter::ALU_ADD, RCX, RBX, r0);
		e.load32 (R10, RBX, rm);
		store (e, o, at, size);
		return true;
	}
	switch (inst & 0xF000) {
	case 0x5000:				/* mov.l @(disp,Rm),Rn */
		e.load32 (RCX, RBX, rm);
		e.lea32 (RCX, RCX, (inst & 15) * 4);
		load (e, o, at, 4);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x1000:				/* mov.l Rm,@(disp,Rn) */
		e.load32 (RCX, RBX, rn);
		e.lea32 (RCX, RCX, (inst & 15) * 4);
		e.load32 (R10, RBX, rm);
		store (e, o, at, 4);
		return true;
	case 0xD000: {				/* mov.l @(disp,PC),Rn */
		/* ((PC + 4) & ~3) + disp * 4, PC's low bits the block's own */
		int32_t from_esi = (int32_t) (((pc + 4) & ~3u) - (pc - (uint32_t) at)) +
		                   (int32_t) ((inst & 0xFF) * 4);
		e.lea32 (RCX, RSI, from_esi);
		load (e, o, at, 4);
		e.store32 (RBX, rn, RAX);
		return true;
	}
	case 0x9000:				/* mov.w @(disp,PC),Rn */
		e.lea32 (RCX, RSI, at + 4 + (int32_t) ((inst & 0xFF) * 2));
		load (e, o, at, 2);
		e.store32 (RBX, rn, RAX);
		return true;
	}
	switch (inst & 0xFF00) {
	case 0xC600:				/* mov.l @(disp,GBR),R0 */
	case 0xC500:				/* mov.w @(disp,GBR),R0 */
	case 0xC400: {				/* mov.b @(disp,GBR),R0 */
		const unsigned s = 1u << ((inst >> 8) & 3);
		e.load32 (RCX, RBX, o.gbr);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 0xFF) * s));
		load (e, o, at, s);
		e.store32 (RBX, r0, RAX);
		return true;
	}
	case 0xC200:				/* mov.l R0,@(disp,GBR) */
	case 0xC100:				/* mov.w R0,@(disp,GBR) */
	case 0xC000: {				/* mov.b R0,@(disp,GBR) */
		const unsigned s = 1u << ((inst >> 8) & 3);
		e.load32 (RCX, RBX, o.gbr);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 0xFF) * s));
		e.load32 (R10, RBX, r0);
		store (e, o, at, s);
		return true;
	}
	case 0x8500:				/* mov.w @(disp,Rm),R0 */
	case 0x8400: {				/* mov.b @(disp,Rm),R0 */
		const unsigned s = (inst & 0x0100) ? 2 : 1;
		e.load32 (RCX, RBX, o.r[(inst >> 4) & 15]);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 15) * s));
		load (e, o, at, s);
		e.store32 (RBX, r0, RAX);
		return true;
	}
	case 0x8100:				/* mov.w R0,@(disp,Rn) */
	case 0x8000: {				/* mov.b R0,@(disp,Rn) */
		const unsigned s = (inst & 0x0100) ? 2 : 1;
		e.load32 (RCX, RBX, o.r[(inst >> 4) & 15]);
		e.lea32 (RCX, RCX, (int32_t) ((inst & 15) * s));
		e.load32 (R10, RBX, r0);
		store (e, o, at, s);
		return true;
	}
	}
	if ((inst & 0xF0FF) == 0x00C3) {	/* movca.l R0,@Rn */
		e.load32 (RCX, RBX, rn);
		e.load32 (R10, RBX, r0);
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
 * as the interpreter's handlers (sh-insns-interp.h) have them. They read
 * no memory and touch no interrupt bit: no interrupt to look for after
 * them, the program counter left as is (the next call, the exit, set it).
 * false for the others, which the handlers run */
static bool
native_op (Emitter &e, const Sh4Offsets &o, uint16_t inst)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;
	const int32_t rn = o.r[n], rm = o.r[m], r0 = o.r[0];
	const int32_t simm = (int8_t) inst;
	const uint32_t uimm = (uint8_t) inst;

	if (inst == 0x0009)			/* nop */
		return true;
	switch (inst & 0xF000) {
	case 0xE000:				/* mov #imm,Rn */
		e.store32_imm (RBX, rn, (uint32_t) simm);
		return true;
	case 0x7000:				/* add #imm,Rn */
		e.alu_mem_imm (Emitter::ALU_ADD, RBX, rn, simm);
		return true;
	}
	switch (inst & 0xFF00) {
	case 0xC800:				/* tst #imm,R0 */
		e.test_mem_imm (RBX, r0, uimm);
		set_t (e, o, CC_E);
		return true;
	case 0xC900:				/* and #imm,R0 */
		e.alu_mem_imm (Emitter::ALU_AND, RBX, r0, (int32_t) uimm);
		return true;
	case 0xCA00:				/* xor #imm,R0 */
		e.alu_mem_imm (Emitter::ALU_XOR, RBX, r0, (int32_t) uimm);
		return true;
	case 0xCB00:				/* or #imm,R0 */
		e.alu_mem_imm (Emitter::ALU_OR, RBX, r0, (int32_t) uimm);
		return true;
	case 0x8800:				/* cmp/eq #imm,R0 */
		e.alu_mem_imm (Emitter::ALU_CMP, RBX, r0, simm);
		set_t (e, o, CC_E);
		return true;
	}
	switch (inst & 0xF00F) {
	case 0x6003:				/* mov Rm,Rn */
		e.load32 (RAX, RBX, rm);
		e.store32 (RBX, rn, RAX);
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
		e.load32 (RAX, RBX, rm);
		e.alu_mem_reg (ops[inst & 15], RBX, rn, RAX);
		return true;
	}
	case 0x6007:				/* not Rm,Rn */
		e.load32 (RAX, RBX, rm);
		e.not32 (RAX);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x600C:				/* extu.b Rm,Rn */
		e.movzx8_mem (RAX, RBX, rm);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x600D:				/* extu.w Rm,Rn */
		e.movzx16_mem (RAX, RBX, rm);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x2008:				/* tst Rm,Rn */
		e.load32 (RAX, RBX, rn);
		e.test_mem_reg (RBX, rm, RAX);
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
		e.load32 (RAX, RBX, rn);
		e.alu_reg_mem (Emitter::ALU_CMP, RAX, RBX, rm);
		set_t (e, o, conds[inst & 7]);
		return true;
	}
	case 0x600E:				/* exts.b Rm,Rn */
		e.movsx8_mem (RAX, RBX, rm);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x600F:				/* exts.w Rm,Rn */
		e.movsx16_mem (RAX, RBX, rm);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x600B:				/* neg Rm,Rn */
		e.load32 (RAX, RBX, rm);
		e.neg32 (RAX);
		e.store32 (RBX, rn, RAX);
		return true;
	}
	switch (inst & 0xF0FF) {
	case 0x4010:				/* dt Rn */
		e.alu_mem_imm (Emitter::ALU_SUB, RBX, rn, 1);
		set_t (e, o, CC_E);
		return true;
	case 0x4000:				/* shll Rn */
		e.shl_mem (RBX, rn, 1);
		set_t (e, o, CC_B);
		return true;
	case 0x4001:				/* shlr Rn */
		e.shr_mem (RBX, rn, 1);
		set_t (e, o, CC_B);
		return true;
	case 0x4008: e.shl_mem (RBX, rn, 2); return true;	/* shll2 */
	case 0x4018: e.shl_mem (RBX, rn, 8); return true;	/* shll8 */
	case 0x4028: e.shl_mem (RBX, rn, 16); return true;	/* shll16 */
	case 0x4009: e.shr_mem (RBX, rn, 2); return true;	/* shlr2 */
	case 0x4019: e.shr_mem (RBX, rn, 8); return true;	/* shlr8 */
	case 0x4029: e.shr_mem (RBX, rn, 16); return true;	/* shlr16 */
	case 0x0029:				/* movt Rn */
		e.load32 (RAX, RBX, o.sr);
		e.and32_imm (RAX, 1);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x4015:				/* cmp/pl Rn */
	case 0x4011:				/* cmp/pz Rn */
		e.alu_mem_imm (Emitter::ALU_CMP, RBX, rn, 0);
		set_t (e, o, (inst & 4) ? CC_G : CC_GE);
		return true;
	/* The FPU's: FPUL, the singles at n ^ 1 (sh4.cpp, FR ()) */
	case 0x005A:				/* sts FPUL,Rn */
		e.load32 (RAX, RBX, o.fpul);
		e.store32 (RBX, rn, RAX);
		return true;
	case 0x405A:				/* lds Rm,FPUL */
		e.load32 (RAX, RBX, rn);
		e.store32 (RBX, o.fpul, RAX);
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

Sh4Native
Sh4X64::translate (Sh4 *ctx, Sh4Block *block)
{
	const Sh4Offsets o (ctx);
	const size_t n = block->ops.size ();
	/* A generous bound of the function's size: an FPU move writes both
	 * sizes' accesses, each with its slow path, some 400 bytes */
	const size_t max = 128 + n * 768;
	/* The ways out after instruction i: in sequence (the next one's
	 * address to set), after a handler's branch (its target less 2 set),
	 * after a branch written out (its target set) */
	std::vector<Label> seq (n), branched (n), done (n);
	std::vector<bool> used (n, false);
	uint8_t *buf;
	size_t i;

	buf = arena.take (max, 16);
	if (!buf)
		return NULL;
	Emitter e (buf, max);

	/* Prologue: as the unwind information says */
	e.push (RBX);
	e.push (RSI);
	e.sub64_imm (RSP, 40);
	e.mov (RBX, RCX);
	e.mov (RSI, RDX);

	for (i = 0; i < n; i++) {
		const Sh4Op &op = block->ops[i];
		const int32_t at = (int32_t) (2 * i);

		if (native_op (e, o, op.inst))
			continue;

		if (memory_op (e, o, op.inst, at, block->pc + (uint32_t) at)) {
			/* A device's register may have raised an interrupt */
			if (i + 1 < n) {
				Label next;

				e.cmp8_mem_imm (RBX, o.pending, 0);
				e.jcc (CC_E, next);
				e.mov (RCX, RBX);
				e.call ((const void *) irq_ready);
				e.test8 (RAX, RAX);
				e.jcc (CC_NE, seq[i]);
				used[i] = true;
				e.bind (next);
			}
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
			e.jmp (done[i]);
			used[i] = true;
			e.bind (stay);
			continue;
		}

		/* The handler, as the interpreter's loop calls it */
		e.lea32 (RAX, RSI, at);
		e.store32 (RBX, o.pc, RAX);
		e.mov (RCX, RBX);
		e.mov_imm32 (RDX, op.inst);
		e.call ((const void *) op.handler);
		if (sh4_may_branch (op.inst)) {
			e.lea32 (RAX, RSI, at);
			e.cmp32_mem (RBX, o.pc, RAX);
			e.jcc (CC_NE, branched[i]);
			used[i] = true;
		}
		/* It may have raised an interrupt (a store, SR) */
		if (i + 1 < n) {
			Label next;

			e.cmp8_mem_imm (RBX, o.pending, 0);
			e.jcc (CC_E, next);
			e.mov (RCX, RBX);
			e.call ((const void *) irq_ready);
			e.test8 (RAX, RAX);
			e.jcc (CC_NE, seq[i]);
			used[i] = true;
			e.bind (next);
		}
	}
	/* The end of the block: on to the next instruction */
	e.jmp (seq[n - 1]);
	used[n - 1] = true;

	/* The exits */
	for (i = 0; i < n; i++) {
		if (!used[i])
			continue;
		e.bind (seq[i]);
		e.lea32 (RAX, RSI, (int32_t) (2 * i + 2));
		e.store32 (RBX, o.pc, RAX);
		e.jmp (done[i]);
		e.bind (branched[i]);
		e.add32_mem_imm (RBX, o.pc, 2);
		e.bind (done[i]);
		e.add32_mem_imm (RBX, o.remaining, -(int32_t) (i + 1));
		e.mov_imm32 (RAX, (uint32_t) (i + 1));
		e.add64_imm (RSP, 40);
		e.pop (RSI);
		e.pop (RBX);
		e.ret ();
	}

	if (e.overflowed ())
		return NULL;
	arena.trim (buf + e.length ());
	register_unwind (buf, e.length ());
	num_translated++;
	return (Sh4Native) buf;
}

Sh4Native
Sh4X64::get (Sh4 *ctx, Sh4Block *block)
{
	if (block->native && block->native_generation == generation)
		return (Sh4Native) block->native;
	if (block->patched || !arena.base ())
		return NULL;
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
	return (Sh4Native) block->native;
}
