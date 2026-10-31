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

#include <memory>
#include <vector>

#include "hl/core.h"

class Sh4;

/* The SH-4's code in blocks: the dynamic recompiler's first stage.
 *
 * A block is a straight run of instructions, decoded once: up to MAX_OPS,
 * to the first unconditional jump (its delay slot is the jump's own: the
 * handler runs it), never past a 4 KB page. A conditional branch not taken
 * goes on in the block; any other way, the block is left, and the next
 * one looked up by its address, in a table of pages: no search.
 *
 * Code is rewritten (loaded from ROM, by the CPUs, by DMA): a block keeps
 * the words it was decoded from, and is decoded again when the memory no
 * longer holds them. No write needs watching.
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
	/* Its first instruction's address, 29 bits */
	uint32_t		 pc;
	/* Where its code lies, in a RAM or ROM, and what it was */
	const uint16_t		*code;
	std::vector<uint16_t>	 words;
	std::vector<Sh4Op>	 ops;
	/* Its x86-64 translation (sh4-x64.h), of a generation of the
	 * backend's code; none yet when 0 */
	void			*native = NULL;
	unsigned		 native_generation = 0;
	/* An instruction the machine patches: not translated */
	bool			 patched = false;

	/* The memory still holds the code it was decoded from */
	bool			 current () const
	{
		return !memcmp (code, words.data (), words.size () * sizeof (words[0]));
	}
};

class Sh4BlockCache {
public:
	static const unsigned MAX_OPS = 32;

	Sh4BlockCache () = default;
	Sh4BlockCache (const Sh4BlockCache &) = delete;
	Sh4BlockCache &operator= (const Sh4BlockCache &) = delete;

	/* The block at pc, decoded if new or its code changed; NULL where the
	 * code is not in a RAM or ROM (run () then fetches it) */
	Sh4Block		*get (Sh4 *ctx, uint32_t pc);
	void			 clear ();
	unsigned		 decoded () const { return num_decoded; }

private:
	static const unsigned PAGE_BITS = 12;
	static const unsigned NUM_PAGES = 1u << (29 - PAGE_BITS);

	/* A page's blocks, one an instruction address */
	struct Page {
		std::unique_ptr<Sh4Block> blocks[1u << (PAGE_BITS - 1)];
	};
	std::unique_ptr<Page>	 pages[NUM_PAGES];
	unsigned		 num_decoded = 0;

	bool			 decode (Sh4 *ctx, uint32_t pc, Sh4Block *block);
};

/* The interpreter's handler for an instruction (sh4.cpp) */
Sh4Handler	 sh4_handler (uint16_t inst);
/* The instruction may change the program counter (a branch, a jump) */
bool		 sh4_may_branch (uint16_t inst);
/* A 32-bit load and store as the handlers make them (sh4.cpp): on-chip
 * registers, store queues, the memory map, the stores counted, the write
 * hooks: the translations' way when they cannot go straight */
uint32_t	 sh4_read32 (Sh4 *ctx, uint32_t addr);
void		 sh4_write32 (Sh4 *ctx, uint32_t addr, uint32_t val);

#endif /* __HL_SH4_BLOCKS_H__ */
