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
	int32_t pc, sr, gbr, remaining, pending, stores, far_stores, r[16];
	/* The FPU's: the banks' first words (FR's, XF's), FPSCR, FPUL */
	int32_t fr, xf, fpscr, fpul;

	explicit Sh4Offsets (Sh4 *ctx);
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
	/* Forgets every translation (the arena full, a reset) */
	void		 clear ();
	unsigned	 translated () const { return num_translated; }

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

private:
	static const size_t ARENA_SIZE = 32u << 20;

	Kind		 backend_kind;
	unsigned	 layout;
	x64::CodeArena	 arena;
	/* Of all backends: a block's function is of this generation */
	unsigned	 generation;
	unsigned	 num_translated = 0;
	/* The unwind information all functions share, its offset in the
	 * arena; the function tables registered */
	uint32_t	 unwind_rva = 0;
	std::vector<void *> tables;

	bool		 start_arena ();
	Sh4Native	 translate (Sh4 *ctx, Sh4Block *block);
	bool		 memory_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			            int32_t at, uint32_t pc);
	bool		 fpu_move_op (x64::Emitter &e, const Sh4Offsets &o, uint16_t inst,
			              int32_t at);
	void		 register_unwind (uint8_t *fn, size_t size);
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
	void		 lookup (x64::Emitter &e, uint8_t * const *table, x64::Label &slow,
			         unsigned size);
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
	void		 check (x64::Emitter &e, const uint8_t *bits, x64::Label &slow,
			        unsigned size);
};

#endif /* __HL_SH4_X64_H__ */
