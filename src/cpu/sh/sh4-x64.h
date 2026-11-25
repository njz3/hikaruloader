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

#ifndef __HL_SH4_X64_H__
#define __HL_SH4_X64_H__

#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu/x64/x64-emitter.h"
#include "cpu/sh/sh4-blocks.h"

namespace hl { class Mmap; }

/* The dynamic recompiler's backend: a block of sh4-blocks.h translated to
 * one host function, which runs its instructions as the interpreter would,
 * in its order: the program counter set before each handler, the slice's
 * cycles counted, a branch taken or an interrupt to take ending it early.
 * The frequent instructions are written out in x86-64 (the ALU, the
 * branches, the loads and stores of every size, the FPU's moves); the
 * others are direct calls to their interpreter's handlers.
 *
 * The function returns how many of the block's instructions it ran; the
 * program counter is then the next one's, the cycles counted down. It runs
 * only when the slice has room for the whole block (and a delay slot), so
 * the slice's end never falls inside it: the dispatcher (sh4.cpp) runs the
 * last instructions of a slice through the interpreter's loop.
 *
 * The memory accesses differ with the memory map's way (Mmap): one class
 * each, the rest of the translation theirs in common.
 *	Sh4X64Calls	the interpreter's path, a call: any memory map
 *	Sh4X64Pages	the page table, read by the code: RAM and ROM at once
 *	Sh4X64Fastmem	fastmem: a bit of a map tested, then base + address
 * All three go the interpreter's way (sh4_read32, sh4_write32 and the other
 * sizes') for what is not RAM or ROM: the devices, the on-chip registers,
 * the store queues.
 * The same machine whichever. Sh4X64::create () picks the one for a memory
 * map; a new one is made when its tables change (Mmap::layout ()).
 *
 * The handlers may abort (hl::Abort, a C++ exception): on Windows, the
 * functions' unwind information is registered (RtlAddFunctionTable), the
 * exception goes through them. */

typedef int (* Sh4Native) (Sh4 *ctx, uint32_t pc);

/* Where the translation finds the SH-4's state, from ctx (rbx) */
struct Sh4Offsets {
	int32_t pc, pr, sr, gbr, macl, mach, remaining, irq_ready, stores, far_stores, r[16];
	/* The other bank's R0-R7 (Rn_BANK) */
	int32_t rbank;
	/* irq_ready, mmu_exact, the idle loops' noting: one word */
	int32_t chain_stop;
	/* A jump's target across its slot, the last block's count (chaining);
	 * the code epoch (sh4-blocks.h); what the chaining's probe looks at */
	int32_t target, chain_n, epoch, state, mmu_on, noting, chain_stores, chain_ends,
		chain_gone, icache;
	/* The instruction cache's lines held, their words, those fetched;
	 * the epoch a block last ran on held lines (sh4-blocks.h) */
	int32_t icache_held, icache_data, icache_fetched, held_epoch;
	/* Under the MMU: the instruction calling out (Sh4::jit_index) */
	int32_t jit_index, mmu_gen, mmu_gen_entry;
	/* A delay slot run (an exception's SPC its branch's) */
	int32_t in_slot, slot_pc;
	int32_t sq, jit_tables;
	/* The FPU's: the banks' first words (FR's, XF's), FPSCR, FPUL */
	int32_t fr, xf, fpscr, fpul;

	explicit Sh4Offsets (Sh4 *ctx);
};

/* The SH-4's GPRs a translation keeps in host registers through its block
 * (sh4-x64.cpp, "Registers"), as it is written: which each of the six
 * holds, those not yet in ctx */
struct Sh4RegCache {
	static const unsigned NUM = 6;
	/* SR.T kept as one of them too (0 or 1), put back into SR's bit 0 (an
	 * and, an or) where the others are written back: a compare and its
	 * branch no longer go through SR in memory */
	static const unsigned T = 16;

	const Sh4Offsets *o = NULL;
	int8_t		 held[NUM];	/* the GPR, -1 none */
	bool		 dirty[NUM];
	uint32_t	 used[NUM];	/* when last used: the one to give up */
	uint32_t	 clock = 0;

	void		 reset (const Sh4Offsets *offsets);
	/* Rn's host register, loaded; to be written whole; read and written */
	x64::Reg	 read (x64::Emitter &e, unsigned n);
	x64::Reg	 write (x64::Emitter &e, unsigned n);
	x64::Reg	 modify (x64::Emitter &e, unsigned n);
	/* Rn's, when it has one (nothing written) */
	bool		 peek (unsigned n, x64::Reg &r) const;
	/* Those not in ctx written there: as they stay (a way out of the
	 * block, a call that reads them), or then in ctx too */
	void		 store_dirty (x64::Emitter &e) const;
	void		 flush (x64::Emitter &e);
	/* None kept (a handler may have changed any) */
	void		 forget ();
	/* T in SR (put back if changed), kept; T given up (SR written in
	 * memory with a new one) */
	void		 t_to_sr (x64::Emitter &e);
	void		 t_drop ();
	/* Register r's value, n's, written back to ctx (T's into SR: with
	 * rax free, a load and a store, else an and and an or on SR) */
	void		 put_back (x64::Emitter &e, x64::Reg r, int n, bool rax_free = false) const;
	/* Those not in ctx, for a way written apart (a slow path, an exit):
	 * their host registers and places in ctx */
	struct Spills {
		std::pair<x64::Reg, int32_t> r[NUM];
		unsigned n = 0;

		const std::pair<x64::Reg, int32_t> *begin () const { return r; }
		const std::pair<x64::Reg, int32_t> *end () const { return r + n; }
		bool	 empty () const { return n == 0; }
	};
	Spills		 spills () const;
	/* Each of regs loaded and taken as changed (written back at the ways
	 * out): a loop's registers kept through its turns; the same registers
	 * in the same host registers as other */
	void		 preload (x64::Emitter &e, const unsigned *regs, unsigned n);
	bool		 same_places (const Sh4RegCache &other) const;
	bool		 empty () const;

private:
	int		 take (x64::Emitter &e, unsigned n, bool load);
};

/* The FPU's singles (FR0-FR15, the bank FPSCR.FR shows) in xmm6-xmm15,
 * which the calls keep (Windows' convention: the prologue saves them):
 * the same as Sh4RegCache, for the FPU's arithmetic and moves (see "The
 * FPU's Registers" in sh4-x64.cpp). FRn at fr + 4 * (n ^ 1) in ctx */
struct Sh4FpuCache {
	static const unsigned NUM = 10;
	static unsigned	 xmm (unsigned k) { return 6 + k; }

	const Sh4Offsets *o = NULL;
	int8_t		 held[NUM];	/* the FR, -1 none */
	bool		 dirty[NUM];
	uint32_t	 used[NUM];
	uint32_t	 clock = 0;

	void		 reset (const Sh4Offsets *offsets);
	/* FRn's xmm, loaded; to be written whole; read and written */
	unsigned	 read (x64::Emitter &e, unsigned n);
	unsigned	 write (x64::Emitter &e, unsigned n);
	unsigned	 modify (x64::Emitter &e, unsigned n);
	/* As Sh4RegCache's: written back, kept (a way out); written back,
	 * clean (a call that reads them); none kept (a call that may change
	 * them); FRn's alone written back and given up (an access of ctx's
	 * copy) */
	void		 store_dirty (x64::Emitter &e) const;
	void		 flush (x64::Emitter &e);
	void		 forget ();
	void		 give_up (x64::Emitter &e, unsigned n);
	bool		 empty () const;
	struct Spills {
		std::pair<unsigned, int32_t> r[NUM];
		unsigned n = 0;

		const std::pair<unsigned, int32_t> *begin () const { return r; }
		const std::pair<unsigned, int32_t> *end () const { return r + n; }
		bool	 empty () const { return n == 0; }
	};
	Spills		 spills () const;

private:
	int32_t		 at (unsigned n) const;
	int		 take (x64::Emitter &e, unsigned n, bool load);
};

class Sh4X64 {
public:
	enum Kind { CALLS, PAGES, FASTMEM };

	/* The backend for the memory map's way, NULL when the host has none
	 * (another calling convention than Windows') */
	static Sh4X64	*create (hl::Mmap *mmap);
	virtual ~Sh4X64 ();
	Sh4X64 (const Sh4X64 &) = delete;
	Sh4X64 &operator= (const Sh4X64 &) = delete;

	Kind		 kind () const { return backend_kind; }
	/* Still the right one for the memory map */
	bool		 fits (const hl::Mmap *mmap) const;
	/* The block's function, translated if it is not yet; NULL when it
	 * cannot be (a patched instruction, no room left) */
	Sh4Native	 get (Sh4 *ctx, Sh4Block *block);
	/* The block's translation past its prologue, for another to go on to
	 * (chaining); NULL when not translated (or of an older generation) */
	void		*body (const Sh4Block *block) const
	{
		return (block->native && block->native_generation == generation) ?
		       block->native_body : NULL;
	}
	/* The block at pc (its translation, its code compared in this epoch)
	 * in the near table, for the translations' ends to go on to it */
	void		 note (uint32_t pc, const Sh4Block *block, uint32_t epoch, uint32_t gen = 0);
	/* The body the near table holds for pc, compared in epoch (under the
	 * MMU, noted under gen); its instructions in count. NULL when it holds
	 * none */
	void		*near_body (uint32_t pc, uint32_t epoch, bool mmu, uint32_t gen, int *count) const
	{
		const Near &n = near_table[(pc >> 1) & near_mask];

		if (n.pc != pc || n.checked != epoch || (mmu && n.gen != gen) || !n.body)
			return NULL;
		*count = n.count;
		return n.body;
	}
	/* The same, without the MMU: the function, for the run loop (the
	 * block table's look missed the host's caches at each line's first
	 * block) */
	Sh4Native	 near_native (uint32_t pc, uint32_t epoch, int *count) const
	{
		const Near &n = near_table[(pc >> 1) & near_mask];

		if (n.pc != pc || n.checked != epoch || !n.body)
			return NULL;
		*count = n.count;
		return n.native;
	}
	/* Forgets every translation (the arena full, a reset) */
	void		 clear ();
	unsigned	 translated () const { return num_translated; }
	/* For HR_PROFILE: the bytes written, those before the slow paths;
	 * with HR_SAMPLE, the bytes of each kind of code (marks) */
	uint64_t	 code_bytes = 0, main_bytes = 0;
	std::map<std::string, uint64_t> kind_bytes;
	void		 print_kinds (char cpu) const;
	/* The times all were dropped, the arena full */
	unsigned	 clears () const { return num_clears; }
	/* The ways out linked to their next block's body (see "Links" in
	 * sh4-x64.cpp): all put back to their stub, the code epoch moved (the
	 * blocks to be compared again first) */
	void		 unlink ();
	/* The link stub's call: the way out at site (its 12 bytes, see
	 * "Links") goes to body from now on; body */
	static void	*link (Sh4X64 *jit, uint8_t *site, void *body);
	/* The same for a way out with its checks in line: its jump's rel32
	 * field at field, from there to body */
	static void	*link_field (Sh4X64 *jit, uint8_t *field, void *body);
	static void	 site_unlinked (uint8_t *site, const uint8_t *link_stub);
	/* The same for a way out to an address known as it runs, its last
	 * address's field at cached: pc and body kept there, until it went to
	 * too many others (DYN_TRIES): then the plain look for good */
	static void	*link_dyn (Sh4X64 *jit, uint8_t *cached, void *body, uint32_t pc);
	uint64_t	 links_made () const { return num_links; }
	/* Traps (see "Traps" in sh4-x64.cpp): the accesses sent through the
	 * page table by a fault, each written over then */
	uint64_t	 traps_taken () const { return num_traps; }
	/* The fault at rip, reading at addr: one of its loads' (the program
	 * counter then its slow path's, *rip) */
	bool		 trapped (uintptr_t rip, uintptr_t addr, uint64_t *new_rip);
	/* The time taken translating, in seconds */
	double		 translating_time () const { return translate_seconds; }

protected:
	Sh4X64 (Kind kind, const hl::Mmap *mmap);
	/* Traps: the base the loads read at (Sh4X64Pages, hl::Mmap::traps ()),
	 * NULL for none; each load's place by its offset in the arena, its
	 * slow path's and the start of the instructions written over (the
	 * base's mov); those of the translation being written */
	uint8_t		*trap_base = NULL;
	struct TrapSite { uint32_t patch, slow; };
	std::unordered_map<uint32_t, TrapSite> trap_sites;
	struct PendingTrap { uint8_t *patch, *load, *end; x64::Label *slow; };
	std::vector<PendingTrap> pending_traps;
	uint64_t	 num_traps = 0;
	void		 trap_register ();

	/* The stores' code stamps left to the write table's pages
	 * (Sh4BlockCache::code_page, hl::Mmap::code_page) */
	bool		 code_pages_on = false;

	/* A load of size bytes (1, 2, 4, 8): the address in ecx, the value to
	 * eax (8 and 16 bits sign extended) or rax. A store: the address in
	 * ecx, the value in r10. rax, rcx, rdx, r8, r9 and r11 may go. at is
	 * the instruction's place from the block's start, for the program
	 * counter the slow path sets */
	virtual void	 load (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			       unsigned size) = 0;
	virtual void	 store (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			        unsigned size) = 0;

	/* The slow paths, the interpreter's: they set the program counter
	 * first (the logs, an abort) */
	void		 call_read (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			            unsigned size, bool slot = false);
	void		 call_write (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			             unsigned size, bool slot = false);
	/* A store's address (ecx) away from the stack, counted */
	void		 count_far (x64::Emitter &e, const Sh4Offsets &o);
	/* The store's count (Sh4::stores, sh4_put ()'s) and the far one's,
	 * as the store written next needs them: the counts are only compared
	 * (an idle loop's turn, a timer's poll), so the block's first store
	 * alone counts in stores (store_counted: a store written out, and
	 * all those after it then run); one at r15 (@-R15, @(disp,R15),
	 * store_base 15) is never far. A store took both counts and seven
	 * instructions more each */
	void		 count_store (x64::Emitter &e, const Sh4Offsets &o);
	bool		 store_counted = false;
	/* The translation may count a store (Sh4::stores): one written, a
	 * slow path's, a handler's, a group's, a pref's. Without, its ways out
	 * linked leave chain.stores as it is, equal to stores already (each
	 * entry sets it so) */
	bool		 may_store = false;
	int		 store_base = -1;
	/* A way written after the block (translate ()'s end), from a label to
	 * another, the GPRs not in ctx written there first: an exit; an
	 * access's slow path (the interpreter's access, which reads r15,
	 * PC...); a handler, PC set. Plain records: the lambdas they were
	 * (std::function, each with a copy of Sh4Offsets) took a dozen
	 * allocations a block, half of its translation's time */
	struct Cold {
		/* GROUP: the handlers of size ops from at / 2, an interrupt
		 * looked for after each (access_group ()'s slow path).
		 * BRANCH: a jump's handler (it runs its delay slot), out by
		 * the jump's way when it went, else to (a delay slot's FPU
		 * work with FPSCR.PR or SR.FD set, translate ()'s
		 * slot_guards) */
		enum Kind { EXIT, READ, WRITE, HANDLER, GROUP, BRANCH, TABLE_READ, TABLE_WRITE } kind;
		x64::Label	*from, *to;
		Sh4RegCache::Spills spill;
		int32_t		 at;
		unsigned	 size;
		uint16_t	 inst;
		Sh4Handler	 fn;
		/* An access of a delay slot written out (slot_op ()) */
		bool		 slot;
		/* TABLE_READ, TABLE_WRITE: a trap's load or store gone to the
		 * page table's look, from, back at to; its slow path's */
		x64::Label	*slow = nullptr;
		/* The FPU's singles not in ctx (Sh4FpuCache) */
		Sh4FpuCache::Spills fspill;
	};
	void		 cold_path (Cold::Kind kind, x64::Label &from, x64::Label &to,
			            int32_t at = 0, unsigned size = 0, uint16_t inst = 0,
			            Sh4Handler fn = NULL)
	{
		cold.push_back (Cold { kind, &from, &to, rc.spills (), at, size, inst, fn, in_slot });
		cold.back ().fspill = fc.spills ();
		if (kind != Cold::EXIT && kind != Cold::READ && kind != Cold::TABLE_READ)
			may_store = true;
	}
	/* Writing a delay slot out (slot_op ()) */
	bool		 in_slot = false;
	/* Sh4::hot_read's, for the reads' slow ways (call_read ()) */
	uint32_t	 hot_addr = ~0u;
	/* TCNT0's reads straight (HR_SH4_TCNT_DIRECT=0: through sh4_read32 ()) */
	bool		 tcnt_direct = true;
	uint32_t	 (*hot_fn) (void *obj) = nullptr;
	void		*hot_obj = nullptr;
	uint32_t	 hot2_addr = ~0u;
	uint32_t	 (*hot2_fn) (void *obj) = nullptr;
	void		*hot2_obj = nullptr;
	/* Translating under the MMU (see "MMU" in sh4.cpp): the loads and
	 * stores straight to P1 and P2 alone, privileged; before a call out,
	 * the instruction's index kept for an exception; no access written in
	 * a delay slot */
	bool		 mmu_mode = false;
	/* And for code at a translated address (Sh4::jit_virtual): no literal
	 * read here, left once the MMU's state moved (Sh4::mmu_gen) */
	bool		 virt_mode = false;
	void		 call_index (x64::Emitter &e, const Sh4Offsets &o, int32_t at, bool slot = false);
	/* A store's slow path: one to a store queue kept until its pref
	 * (Sh4::sq_buffered) written there at once, as sh4_sq_put () does
	 * (ecx the address, r10 the value), then on to back */
	void		 sq_store (x64::Emitter &e, const Sh4Offsets &o, unsigned size, x64::Label &back);
	/* A store the page table has no page for, ecx the address, r10 the
	 * value: to the NAOMI's 32-bit VRAM path straight (Sh4::vram32_base),
	 * then to done (counted there), else to slow */
	void		 vram32_store (x64::Emitter &e, const Sh4Offsets &o, unsigned size, x64::Label &done,
				       x64::Label &slow);
	uint8_t		*vram32_base = nullptr;
	const bool	*vram32_busy = nullptr;
	bool		 sq_inline = false;
	/* The access just written cannot reach a device (a literal read at
	 * the translation): no interrupt check after it */
	bool		 no_device = false;
	void		 mmu_guard (x64::Emitter &e, const Sh4Offsets &o, x64::Label &slow);
	/* HR_SAMPLE: the kinds of the code being written, each from its
	 * offset on (hl::Profile::code_written ()) */
	std::vector<std::pair<uint32_t, const char *>> marks;
	void		 mark (x64::Emitter &e, const char *what);
	/* An access's slow path, written after the block: bound at slow,
	 * back to back after it */
	void		 slow_path (x64::Label &slow, x64::Label &back, bool write, int32_t at,
			            unsigned size)
	{
		cold_path (write ? Cold::WRITE : Cold::READ, slow, back, at, size);
	}
	/* A trap's access through the page table, from table back to back;
	 * slow its slow path (see "Traps") */
	void		 table_path (x64::Label &table, x64::Label &back, x64::Label &slow, bool write,
			             int32_t at, unsigned size)
	{
		cold_path (write ? Cold::TABLE_WRITE : Cold::TABLE_READ, table, back, at, size);
		cold.back ().slow = &slow;
	}
	/* A label of this translation (those of the ones before used again) */
	x64::Label	&label ()
	{
		if (labels_used == labels.size ())
			labels.emplace_back ();
		x64::Label &l = labels[labels_used++];
		l.reset ();
		return l;
	}
	/* eax a store's stamp (its 256 bytes, Sh4BlockCache::STAMP_BITS): to
	 * slow when code there was compared in this epoch, or its page is
	 * watched (sh4-blocks.h), the CPU's stamps in stamps */
	void		 stamp_check (x64::Emitter &e, const Sh4Offsets &o, x64::Label &slow);
	const uint32_t	*stamps = NULL;
	/* The GPRs of the block being translated in host registers */
	Sh4RegCache	 rc;
	/* The FPU's singles in host registers (HR_SH4_FPU_CACHE=0: none, each
	 * FPU instruction from and to ctx); fpu_cache_on for the block being
	 * translated (not under the MMU, the page table's backend); the
	 * prologue saving xmm6-xmm15 (all translations: one frame) */
	Sh4FpuCache	 fc;
	bool		 fpu_cache_on = false, saves_xmm = false;
	/* The frame the prologue takes below its pushes (the xmm saved at
	 * XMM_AT up) */
	static const int32_t FRAME = 216, XMM_AT = 48;

private:
	/* VON4 filled 32 MB four times in its first minute (29000 blocks),
	 * each block then translated again */
	static const size_t ARENA_SIZE = 128u << 20;

	Kind		 backend_kind;
	unsigned	 layout;
	x64::CodeArena	 arena;
	/* Of all backends: a block's function is of this generation */
	unsigned	 generation;
	unsigned	 num_translated = 0, num_clears = 0;
	double		 translate_seconds = 0;
	/* The unwind information all functions share, its offset in the
	 * arena; the function table of the whole arena (start_arena ()) */
	uint32_t	 unwind_rva = 0;
	void		*table = NULL;

	bool		 start_arena ();
	/* The translations' prologue, its unwind codes (UNWIND_CODE slots,
	 * the last instruction's first) to codes when not NULL; its size.
	 * The way back to the run loop's epilogue, the same undone */
	unsigned	 emit_prologue (x64::Emitter &e, std::vector<uint8_t> *codes) const;
	void		 emit_epilogue (x64::Emitter &e) const;
	/* The ways out the translations share, written in the arena once a
	 * generation (emit_stubs ()), apart under the MMU: the probe (r8d the
	 * count, PC set, esi the block's address), its checks (the count taken
	 * off), sh4_chain (), and back to the run loop. A copy in each
	 * translation made them some 150 bytes longer */
	struct Stubs {
		const uint8_t *probe = NULL, *checks = NULL, *chain = NULL;
		/* The linked ways out's checks (called), the link (none under
		 * the MMU); for an address known as it runs, the link and the
		 * plain look (no link) */
		const uint8_t *link_checks = NULL, *link = NULL, *link_dyn = NULL, *look = NULL,
			      *dyn_checks = NULL;
		/* A way out with its checks in line, not linked yet: r9 its
		 * jump's rel32 field (see "Links") */
		const uint8_t *link_in = NULL;
	};
	/* The rel32 fields of the jumps linked; the last addresses' fields
	 * of the ways out to addresses known as they run, and the times
	 * each went elsewhere */
	std::vector<uint8_t *> linked, cached;
	/* The ways out with their checks in line linked: their jump's rel32
	 * field, the place it went before (unlink ()) */
	std::vector<std::pair<uint8_t *, const uint8_t *>> linked_in;
	std::unordered_map<uint8_t *, unsigned> dyn_tries;
	static const unsigned DYN_TRIES = 8;
	/* Their layout from the cached field: the jump to the body, the jump
	 * to the link stub (cmp esi, imm32; jne; jmp rel32; lea r9; jmp rel32) */
	static const int32_t DYN_BODY = 11, DYN_STUB = 23;
	uint64_t	 num_links = 0;
	Stubs		 stubs[2];
	unsigned	 stubs_generation = 0;
	bool		 emit_stubs (Sh4 *ctx);
	Sh4Native	 translate (Sh4 *ctx, Sh4Block *block);
	bool		 memory_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			            int32_t at, uint32_t pc);
	bool		 native_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst);
	/* T = c's flag, in its host register; where the flags still hold it
	 * (no code written since), the condition */
	void		 set_t_cond (x64::Emitter &e, x64::Cond c);
	uint8_t		*t_flags_at = nullptr;
	x64::Cond	 t_flags_cond = x64::CC_E;
	/* The FPU's arithmetic (sh4-x64.cpp); fc_checked, the FPU's
	 * registers as they stood at its test of PR or FD (its slow way's) */
	bool		 fpu_op (x64::Emitter &e, const Sh4Offsets &o, Sh4RegCache &rc, uint16_t inst,
			         x64::Label &slow, bool &fallback, bool &pr_ok, bool &fd_ok);
	Sh4FpuCache	 fc_checked;
	/* A division's steps from op i, div1 and rotcl on the same registers:
	 * how many written out at once (0 for none) */
	unsigned	 division_run (x64::Emitter &e, const Sh4Offsets &o, const Sh4Block *block, size_t i);
	void		 division_pairs (x64::Emitter &e, x64::Reg a, x64::Reg d, x64::Reg q, unsigned k,
			                 const std::function<void ()> &slow_steps);
	/* A run of 32-bit accesses through one base register from op i (the
	 * pushes and pops of a function, a vector's fmov.s), its alignment
	 * and its page looked at once: the ops taken, 0 for none. seq the
	 * ways out (those its slow path takes in forced) */
	unsigned	 access_group (x64::Emitter &e, const Sh4Offsets &o, const Sh4Block *block, size_t i,
			               x64::Label *seq, bool *forced, bool singles);
	/* A delay loop on a word in memory (Sh4Block::mem_loop): its turns
	 * but the last taken at once, at the block's entry */
	void		 memory_loop (x64::Emitter &e, const Sh4Offsets &o, const Sh4Block *block);
	/* The memory map of the block being translated (a PC-relative load
	 * from RAM or ROM read at its host address) */
	hl::Mmap	*translating = NULL;
	/* Its processor and its address (HR_SH4_STATS' counters) */
	Sh4		*translating_cpu = NULL;
	uint32_t	 translating_pc = 0;
	/* FPSCR.PR, SR.FD seen clear since the last handler (fpu_op ()):
	 * the FPU's work written without its tests */
	bool		 pr_ok = false, fd_ok = false;
	const uint8_t	*literal (uint32_t addr, unsigned size) const;
	bool		 fpu_move_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			              int32_t at);
	/* FPSCR.SZ for the FPU's moves written next: 0 or 1, that size
	 * alone (the block's guard saw it); -1, tested as they run */
	int		 move_sz = -1;
	/* A delay slot's instruction, written out (slot_writable ()); a jump
	 * jump_op () writes out with its slot */
	bool		 slot_writable (const Sh4Offsets &o, uint16_t inst);
	bool		 jump_writable (const Sh4Offsets &o, const Sh4Block *block, size_t i);
	void		 slot_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			          int32_t at);
	bool		 jump_op (x64::Emitter &e, const Sh4Offsets &o,
			          const Sh4Block *block, size_t i, x64::Label &out,
			          bool &fixed, int32_t &to);

	/* The slow paths of the block being translated, and their labels */
	std::vector<Cold> cold;
	std::deque<x64::Label> labels;
	size_t		 labels_used = 0;

	/* The near table (sh4-x64.cpp, the end of translate ()): a flat
	 * table of the blocks gone to, one an address's place (pc >> 1, 16
	 * bits), 32 bytes each */
	struct Near {
		uint32_t	 pc;		/* 29 bits; odd: none */
		uint32_t	 checked;	/* the code epoch it was compared in */
		int32_t		 count;		/* its instructions */
		uint32_t	 gen;		/* the MMU's state's (Sh4::mmu_gen) */
		void		*body;
		/* The function (its prologue), for the run loop */
		Sh4Native	 native;
	};
	static const unsigned NEAR_BITS = 16;
	std::unique_ptr<Near[]> near_table { new Near[1u << NEAR_BITS] };
	static const uint32_t near_mask = (1u << NEAR_BITS) - 1;
	void		 clear_near ();
};

/* The loads and stores through the interpreter's path: any memory map */
class Sh4X64Calls : public Sh4X64 {
public:
	explicit Sh4X64Calls (const hl::Mmap *mmap) : Sh4X64 (CALLS, mmap) {}
protected:
	void		 load (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			       unsigned size) override;
	void		 store (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			        unsigned size) override;
};

/* Through the page table, read by the code */
class Sh4X64Pages : public Sh4X64 {
public:
	explicit Sh4X64Pages (const hl::Mmap *mmap);
protected:
	void		 load (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			       unsigned size) override;
	void		 store (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			        unsigned size) override;
private:
	/* memory_loop () reads the write table too */
	friend class Sh4X64;
	const intptr_t	*read_table, *write_table, *write_all_table;
	void		 lookup (x64::Emitter &e, const Sh4Offsets &o, const intptr_t *table,
			         x64::Label &slow, unsigned size, bool store);
};

/* Through fastmem: base + address, where the bit map says */
class Sh4X64Fastmem : public Sh4X64 {
public:
	explicit Sh4X64Fastmem (const hl::Mmap *mmap);
protected:
	void		 load (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			       unsigned size) override;
	void		 store (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			        unsigned size) override;
private:
	uint8_t		*base;
	const uint8_t	*readable, *writable;
	void		 check (x64::Emitter &e, const Sh4Offsets &o, const uint8_t *bits,
			        x64::Label &slow, unsigned size, bool store);
};

#endif /* __HL_SH4_X64_H__ */
