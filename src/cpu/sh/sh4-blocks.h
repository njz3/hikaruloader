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

#ifndef __HL_SH4_BLOCKS_H__
#define __HL_SH4_BLOCKS_H__

#include <functional>
#include <memory>
#include <vector>

#include "hl/core.h"

class Sh4;

/* The SH-4's code in blocks: the dynamic recompiler's first stage.
 *
 * A block is a straight run of instructions, decoded once: up to MAX_OPS,
 * to the first unconditional jump (its delay slot is the jump's own: the
 * handler runs it, or the translation writes it out), never past a 4 KB
 * page. A conditional branch not taken goes on in the block; any other
 * way, the block is left, and the next one looked up by its address, in a
 * table of pages: no search.
 *
 * Code is rewritten (loaded from ROM, by the CPUs, by DMA): a block keeps
 * the words it was decoded from, and is decoded again when the memory no
 * longer holds them. In the CPU's own RAM and ROM (own ()), they are
 * compared once an epoch, as Odin does: each 256 bytes note the epoch
 * their code was last compared in, and a write there in that epoch, by the
 * CPU or by what the machine tells of (written ()), starts a new one.
 * Elsewhere (a memory others write straight), at each entry. By 4 KB
 * pages, as Odin has them, the BIOS's variables at 0C00123C-0C0012E8, in
 * the page of its code at 0C001000-0C001140, started 30 epochs a frame
 * (SWRACER; each block compared again after each). A store there starts
 * one only when a block compared in this epoch holds a byte it writes
 * (covered ()): SWRACER fills 20 bytes beside its code, 2500 times.
 *
 * The same table watches 4 KB pages for the machine (watch ()): one armed
 * (its stamps ARMED, above any epoch) takes the CPU's first store to the
 * slow path, which tells the machine and leaves the page to the fast one
 * until rearm (). The Hikaru's framebuffer so: the 2D layers' rows copied
 * to the drawing thread those written alone.
 *
 * The instruction cache (Sh4ICache): a store over a line it holds does not
 * reach the code fetched from it, until the line goes or CCR.ICI clears it.
 *
 * Sh4::run_blocks () runs them one instruction at a time, as run () does
 * (interrupts, the slice's cycles, the patches, the idle loops): the very
 * same machine, without the fetches and the decoding. The next stage
 * translates a block to x86-64 code (a backend of the same blocks). */

typedef void (* Sh4Handler) (Sh4 *ctx, uint16_t inst);

struct Sh4Op {
	Sh4Handler	 handler;
	uint16_t	 inst;
	/* The machine's patch looks at its address (Cpu::patches) */
	bool		 patched;
};

class Sh4Block {
public:
	static const unsigned MAX_OPS = 32;

	/* Its first instruction's address, 29 bits */
	uint32_t		 pc;
	/* Where its code lies, in a RAM or ROM, and what it was (here, not
	 * in a vector apart: each entry compares them) */
	const uint16_t		*code;
	unsigned		 num_words = 0;
	uint16_t		 words[MAX_OPS + 1];
	std::vector<Sh4Op>	 ops;
	/* The last instruction an unconditional jump, its delay slot kept in
	 * words after the ops' (words[ops.size ()]), compared with them: in
	 * the page, not patched */
	bool			 slot_kept = false;
	/* Its x86-64 translation (sh4-x64.h), of a generation of the
	 * backend's code; none yet when 0. body: past the function's
	 * prologue, where another translation goes on to it (chaining) */
	void			*native = NULL;
	void			*native_body = NULL;
	unsigned		 native_generation = 0;
	/* An instruction the machine patches: not translated */
	bool			 patched = false;
	/* A delay loop, "dt Rn; bf" back to its start: its translation takes
	 * its turns at once (sh4-x64.cpp) */
	bool			 delay_loop = false;
	/* A wait on memory, "load; compare; bt or bf" back to its start: its
	 * translation's way back takes the slice's other turns off
	 * (sh4_poll_skip ()) */
	bool			 poll_loop = false;
	/* Its way back's turns the idle loops let be, gone on without their
	 * look (its translation counts them down) */
	int32_t			 quiet = 0;
	/* In the CPU's own RAM or ROM: compared once an epoch, the last one
	 * checked */
	bool			 checkable = false;
	uint32_t		 checked = 0;

	/* The memory still holds the code it was decoded from: compared here,
	 * 8 bytes at a time (each block's entry asks) */
	bool			 current () const
	{
		const uint8_t *a = (const uint8_t *) code;
		const uint8_t *b = (const uint8_t *) words;
		size_t n = num_words * sizeof (words[0]), i = 0;

		for (; i + 8 <= n; i += 8) {
			uint64_t x, y;
			memcpy (&x, a + i, 8);
			memcpy (&y, b + i, 8);
			if (x != y)
				return false;
		}
		for (; i < n; i += 2)
			if (*(const uint16_t *) (a + i) != *(const uint16_t *) (b + i))
				return false;
		return true;
	}
};

/* The instruction cache, 8 KB of 32-byte lines, direct mapped. A real one for
 * what its lines hold: code in a line stays as it was fetched while the line
 * is there, whatever is written to the memory behind it (a RAM test running
 * over its own code, the SH-4 manual's reason for CCR.ICI). Each block looks
 * at its lines as it starts: one comparison a line, in its translation.
 *
 * tags[]: the line each index last fetched, its address >> 5, 29 bits. The
 * translations only note it: its bytes are copied here when a store is about
 * to change them (Sh4BlockCache::covered ()), held[] then set, with the cache
 * on (enabled, CCR.ICE). Until then the memory is the same as the line.
 * CCR.IIX's index is not modelled */
class Sh4ICache {
public:
	static const unsigned LINE_BITS = 5, NUM_LINES = 256, LINE_WORDS = 16;
	static const uint32_t NONE = 0xFFFFFFFFu;

	uint32_t		 tags[NUM_LINES];
	uint32_t		 held[NUM_LINES];
	uint16_t		 data[NUM_LINES][LINE_WORDS];
	bool			 enabled = false;

	static unsigned		 index (uint32_t addr) { return (addr >> LINE_BITS) & (NUM_LINES - 1); }
	static uint32_t		 tag (uint32_t addr) { return (addr & 0x1FFFFFFF) >> LINE_BITS; }
	void			 reset ()
	{
		for (unsigned i = 0; i < NUM_LINES; i++) {
			tags[i] = NONE;
			held[i] = 0;
		}
	}
	bool			 present (uint32_t addr) const
	{
		return tags[index (addr)] == tag (addr);
	}
	/* The line's words when the cache holds them, else NULL */
	const uint16_t		*line (uint32_t addr) const
	{
		const unsigned i = index (addr);

		return tags[i] == tag (addr) && held[i] ? data[i] : NULL;
	}
};

class Sh4BlockCache {
public:
	static const unsigned MAX_OPS = Sh4Block::MAX_OPS;

	Sh4ICache		 icache;

	Sh4BlockCache () = default;
	Sh4BlockCache (const Sh4BlockCache &) = delete;
	Sh4BlockCache &operator= (const Sh4BlockCache &) = delete;

	/* The block at pc, decoded if new or its code changed; NULL where the
	 * code is not in a RAM or ROM (run () then fetches it) */
	Sh4Block		*get (Sh4 *ctx, uint32_t pc);
	/* The block at pc as it is, NULL unless decoded and current (in
	 * line: sh4_chain ()'s, at each block it goes on to by itself) */
	Sh4Block		*peek (uint32_t pc)
	{
		const std::unique_ptr<Page> &page = pages[(pc & 0x1FFFFFFF) >> PAGE_BITS];
		Sh4Block *block = page ? page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1].get () : NULL;

		if (!block || (block->checked == epoch && !checking))
			return block;
		return fresh (block) ? block : NULL;
	}
	void			 clear ();
	unsigned		 decoded () const { return num_decoded; }
	/* The instruction cache emptied (CCR.ICI, a line invalidated): the
	 * code is the memory's again */
	void			 icache_changed () { new_epoch (); }
	/* A line replaced: a block that ran from a line gone (its memory
	 * rewritten meanwhile) looked at again */
	void			 line_replaced ()
	{
		if (held_epoch == epoch)
			new_epoch ();
	}
	/* The epoch a block last ran on from a held line, for the translations */
	uint32_t		 held_epoch = 0;

	/* Code epochs (above). The CPU's own RAM or ROM, from lo to hi (29
	 * bits): where nothing writes but the CPU and what the machine tells
	 * of. Each entry compared all the same, and a rewrite missed told,
	 * with HR_SH4_EPOCH_CHECK=1 */
	void			 own (uint32_t lo, uint32_t hi);
	bool			 checking = false;
	uint32_t		 epoch = 1;
	/* The epoch of each 256 bytes (1 << 21 of them, STAMP_BITS), for the
	 * translations' stores: those at this epoch or above (ARMED) take the
	 * slow path */
	static const unsigned STAMP_BITS = 8;
	uint32_t		*stamps () const { return page_epochs.get (); }
	void			 new_epoch ();
	/* A write of size bytes at addr: a new epoch when it falls on code
	 * compared in this one; a watched page's told */
	void			 written (uint32_t addr, unsigned size)
	{
		uint32_t a = (addr & 0x1FFFFFFF) >> STAMP_BITS;
		uint32_t b = ((addr + size - 1) & 0x1FFFFFFF) >> STAMP_BITS;

		if (page_epochs[a] >= epoch)
			hit (a, addr, size);
		if (b != a && page_epochs[b] >= epoch)
			hit (b, addr, size);
	}

	/* The pages from lo to hi watched, told is told the address of each
	 * page first written after an arm; then armed again */
	static const uint32_t ARMED = 0xFFFFFFFFu;
	void			 watch (uint32_t lo, uint32_t hi, std::function<void (uint32_t)> told);
	void			 rearm ();

private:
	static const unsigned PAGE_BITS = 12;
	static const unsigned NUM_PAGES = 1u << (29 - PAGE_BITS);
	static const unsigned NUM_STAMPS = 1u << (29 - STAMP_BITS);
	static const unsigned PAGE_STAMPS = 1u << (PAGE_BITS - STAMP_BITS);

	/* A page's blocks, one an instruction address */
	struct Page {
		std::unique_ptr<Sh4Block> blocks[1u << (PAGE_BITS - 1)];
	};
	std::unique_ptr<Page>	 pages[NUM_PAGES];
	unsigned		 num_decoded = 0;
	std::unique_ptr<uint32_t[]> page_epochs { new uint32_t[NUM_STAMPS] () };
	uint32_t		 own_lo[4], own_hi[4];
	unsigned		 num_own = 0;
	/* The pages watched, those told since rearm () */
	std::vector<uint32_t>	 watched, hits;
	std::function<void (uint32_t)> watch_told;

	/* A store of size bytes at addr, at or above stamp's epoch */
	void			 hit (uint32_t stamp, uint32_t addr, unsigned size);
	/* Its code compared now: the stamps it lies on at this epoch */
	void			 stamp (const Sh4Block *block);
	/* A block compared in this epoch holds a byte of those; the last
	 * answers, while no block is stamped again (stamped counts them) */
	bool			 covered (uint32_t addr, unsigned size);
	uint64_t		 stamped = 0;
	struct Uncovered {
		uint32_t addr = 0, size = 0, epoch = 0;
		uint64_t stamped = ~0ull;
	} uncovered[4];
	unsigned		 next_uncovered = 0;

	bool			 decode (Sh4 *ctx, uint32_t pc, Sh4Block *block);
	/* Its code still the memory's: compared when its epoch is old */
	bool			 fresh (Sh4Block *block);
	/* Its code rewritten behind the lines the instruction cache holds */
	bool			 held_in_cache (const Sh4Block *block) const;
	/* The lines the store of size bytes at addr is about to change, those
	 * the cache has: copied from the block they were fetched for */
	void			 capture (const Sh4Block *block, uint32_t addr, unsigned size);
};

/* The interpreter's handler for an instruction (sh4.cpp) */
Sh4Handler	 sh4_handler (uint16_t inst);
/* The instruction may change the program counter (a branch, a jump) */
bool		 sh4_may_branch (uint16_t inst);
/* The lines of the block at pc, up to the byte at last, fetched (sh4.cpp) */
void		 sh4_icache_fill (Sh4 *ctx, uint32_t pc, uint32_t last);
/* A 32-bit load and store as the handlers make them (sh4.cpp): on-chip
 * registers, store queues, the memory map, the stores counted, the write
 * hooks: the translations' way when they cannot go straight */
uint32_t	 sh4_read32 (Sh4 *ctx, uint32_t addr);
void		 sh4_write32 (Sh4 *ctx, uint32_t addr, uint32_t val);
/* The other sizes: 8 and 16 bits sign extended, as mov.b and mov.w load */
uint32_t	 sh4_read8s (Sh4 *ctx, uint32_t addr);
uint32_t	 sh4_read16s (Sh4 *ctx, uint32_t addr);
uint64_t	 sh4_read64 (Sh4 *ctx, uint32_t addr);
void		 sh4_write8 (Sh4 *ctx, uint32_t addr, uint32_t val);
void		 sh4_write16 (Sh4 *ctx, uint32_t addr, uint32_t val);
void		 sh4_write64 (Sh4 *ctx, uint32_t addr, uint64_t val);

#endif /* __HL_SH4_BLOCKS_H__ */
