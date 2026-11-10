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
#include <memory>
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
	/* A jump's target across its slot, the last block's count (chaining);
	 * the code epoch (sh4-blocks.h); what the chaining's probe looks at */
	int32_t target, chain_n, epoch, state, mmu_on, noting, chain_stores, chain_ends,
		chain_gone, icache;
	/* The FPU's: the banks' first words (FR's, XF's), FPSCR, FPUL */
	int32_t fr, xf, fpscr, fpul;

	explicit Sh4Offsets (Sh4 *ctx);
};

/* The SH-4's GPRs a translation keeps in host registers through its block
 * (sh4-x64.cpp, "Registers"), as it is written: which each of the six
 * holds, those not yet in ctx */
struct Sh4RegCache {
	static const unsigned NUM = 6;

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

private:
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
	void		 note (uint32_t pc, const Sh4Block *block, uint32_t epoch);
	/* Forgets every translation (the arena full, a reset) */
	void		 clear ();
	unsigned	 translated () const { return num_translated; }
	/* The times all were dropped, the arena full */
	unsigned	 clears () const { return num_clears; }
	/* The time taken translating, in seconds */
	double		 translating_time () const { return translate_seconds; }

protected:
	Sh4X64 (Kind kind, const hl::Mmap *mmap);

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
			            unsigned size);
	void		 call_write (x64::Emitter &e, const Sh4Offsets &o, int32_t at,
			             unsigned size);
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
	int		 store_base = -1;
	/* A way written after the block (translate ()'s end), from a label to
	 * another, the GPRs not in ctx written there first: an exit; an
	 * access's slow path (the interpreter's access, which reads r15,
	 * PC...); a handler, PC set. Plain records: the lambdas they were
	 * (std::function, each with a copy of Sh4Offsets) took a dozen
	 * allocations a block, half of its translation's time */
	struct Cold {
		enum Kind { EXIT, READ, WRITE, HANDLER } kind;
		x64::Label	*from, *to;
		Sh4RegCache::Spills spill;
		int32_t		 at;
		unsigned	 size;
		uint16_t	 inst;
		Sh4Handler	 fn;
	};
	void		 cold_path (Cold::Kind kind, x64::Label &from, x64::Label &to,
			            int32_t at = 0, unsigned size = 0, uint16_t inst = 0,
			            Sh4Handler fn = NULL)
	{
		cold.push_back (Cold { kind, &from, &to, rc.spills (), at, size, inst, fn });
	}
	/* An access's slow path, written after the block: bound at slow,
	 * back to back after it */
	void		 slow_path (x64::Label &slow, x64::Label &back, bool write, int32_t at,
			            unsigned size)
	{
		cold_path (write ? Cold::WRITE : Cold::READ, slow, back, at, size);
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
	Sh4Native	 translate (Sh4 *ctx, Sh4Block *block);
	bool		 memory_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			            int32_t at, uint32_t pc);
	bool		 native_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst);
	/* The memory map of the block being translated (a PC-relative load
	 * from RAM or ROM read at its host address) */
	hl::Mmap	*translating = NULL;
	const uint8_t	*literal (uint32_t addr, unsigned size) const;
	bool		 fpu_move_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			              int32_t at);
	/* FPSCR.SZ for the FPU's moves written next: 0 or 1, that size
	 * alone (the block's guard saw it); -1, tested as they run */
	int		 move_sz = -1;
	/* A delay slot's instruction, written out (slot_writable ()) */
	bool		 slot_writable (const Sh4Offsets &o, uint16_t inst);
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
		uint32_t	 unused;
		void		*body;
		uint64_t	 unused2;
	};
	static const unsigned NEAR_BITS = 16;
	std::unique_ptr<Near[]> near_table { new Near[1u << NEAR_BITS] };
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
	uint8_t * const	*read_table, * const *write_table;
	void		 lookup (x64::Emitter &e, const Sh4Offsets &o, uint8_t * const *table,
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
