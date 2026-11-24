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

/* The SH-4's code in blocks; see sh4-blocks.h */

#include <new>
#include <algorithm>

#include "cpu/sh/sh4-blocks.h"
#include "cpu/sh/sh4.h"

#define ADDR_MASK	0x1FFFFFFF

/* A load whose address its result leaves as it was: mov.x @(disp,GBR),R0,
 * mov.x @Rm,Rn, mov.l @(disp,Rm),Rn, mov.x @(disp,Rm),R0 (Rn not Rm) */
static bool
poll_load (uint16_t inst)
{
	const unsigned n = (inst >> 8) & 15, m = (inst >> 4) & 15;

	switch (inst & 0xFF00) {
	case 0xC400: case 0xC500: case 0xC600:
		return true;
	case 0x8400: case 0x8500:
		return m != 0;
	}
	if ((inst & 0xF00F) == 0x6000 || (inst & 0xF00F) == 0x6001 || (inst & 0xF00F) == 0x6002 ||
	    (inst & 0xF000) == 0x5000)
		return n != m;
	return false;
}

/* A comparison, which sets T alone: cmp/pl, cmp/pz, tst, cmp/str, cmp/eq,
 * cmp/hs, cmp/ge, cmp/hi, cmp/gt, cmp/eq #imm,R0, tst #imm,R0 */
static bool
compares (uint16_t inst)
{
	switch (inst & 0xF0FF) {
	case 0x4015: case 0x4011:
		return true;
	}
	switch (inst & 0xF00F) {
	case 0x2008: case 0x200C:
	case 0x3000: case 0x3002: case 0x3003: case 0x3006: case 0x3007:
		return true;
	}
	return (inst & 0xFF00) == 0x8800 || (inst & 0xFF00) == 0xC800;
}

/* Where a block ends: after a jump that always goes, or a stop */
static bool
ends_block (uint16_t inst)
{
	switch (inst & 0xF000) {
	case 0xA000:	/* bra */
	case 0xB000:	/* bsr */
		return true;
	}
	switch (inst & 0xF0FF) {
	case 0x0023:	/* braf Rn */
	case 0x0003:	/* bsrf Rn */
	case 0x402B:	/* jmp @Rn */
	case 0x400B:	/* jsr @Rn */
		return true;
	}
	switch (inst) {
	case 0x000B:	/* rts */
	case 0x002B:	/* rte */
	case 0x001B:	/* sleep */
		return true;
	}
	return (inst & 0xFF00) == 0xC300;	/* trapa #imm */
}

bool
sh4_may_branch (uint16_t inst)
{
	switch (inst & 0xFF00) {
	case 0x8900:	/* bt */
	case 0x8B00:	/* bf */
	case 0x8D00:	/* bt/s */
	case 0x8F00:	/* bf/s */
		return true;
	}
	return ends_block (inst);
}

bool
Sh4BlockCache::decode (Sh4 *ctx, uint32_t pc, Sh4Block *block)
{
	hl::Mmap::Window win;
	uint32_t end, a;

	if (!ctx->mmap->window (pc, &win))
		return false;

	/* Within the RAM or ROM, and the page; under the MMU, its smallest
	 * (1 KB): a block in one page of a translated address too */
	end = MIN2 (win.hi, pc | ((1u << PAGE_BITS) - 1));
	if (ctx->mmu_exact)
		end = MIN2 (end, pc | 0x3FF);
	block->pc = pc;
	block->code = (const uint16_t *) (win.base + (pc & win.mask));
	block->num_words = 0;
	block->ops.clear ();
	/* Its room at once: grown by doubling, a block's ops took six
	 * allocations */
	block->ops.reserve (MAX_OPS);
	block->native = NULL;
	block->native_body = NULL;
	block->native_generation = 0;
	block->patched = false;
	block->delay_loop = false;
	block->mem_loop = Sh4Block::MEM_LOOP_NONE;
	block->simulated = 0;
	block->poll_loop = false;
	block->quiet = 0;
	block->slot_kept = false;
	block->checked = 0;
	block->checkable = false;
	for (unsigned i = 0; i < num_own; i++)
		if (pc >= own_lo[i] && pc <= own_hi[i])
			block->checkable = true;
	/* From the instruction cache where it holds the line */
	auto word = [&] (uint32_t at) {
		const uint16_t *line = icache.line (at);

		return line ? line[(at >> 1) & (Sh4ICache::LINE_WORDS - 1)] :
		              *(const uint16_t *) (win.base + (at & win.mask));
	};
	for (a = pc; a + 1 <= end && block->ops.size () < MAX_OPS; a += 2) {
		uint16_t inst = word (a);
		Sh4Op op = { sh4_handler (inst), inst, ctx->patches (a, inst) };

		block->words[block->num_words++] = inst;
		block->ops.push_back (op);
		block->patched |= op.patched;
		if (ends_block (inst)) {
			/* A jump's delay slot, after it in the page; not sleep's
			 * or trapa's (none) */
			if (inst != 0x001B && (inst & 0xFF00) != 0xC300 && a + 3 <= end &&
			    !ctx->patches (a + 2, word (a + 2))) {
				block->words[block->num_words++] = word (a + 2);
				block->slot_kept = true;
			}
			break;
		}
	}
	block->delay_loop = false;
	if (block->ops.size () >= 2 && (block->ops[0].inst & 0xF0FF) == 0x4010) {
		unsigned j = 1;

		while (j < block->ops.size () && j < 6 && block->ops[j].inst == 0x0009)
			j++;
		/* bf back to the dt: -(j + 2) */
		if (j < block->ops.size () && block->ops[j].inst == (0x8B00 | ((0u - (j + 2)) & 0xFF))) {
			block->delay_loop = true;
			block->delay_len = (uint8_t) (j + 1);
		}
	}
	block->poll_loop = block->ops.size () >= 3 && poll_load (block->ops[0].inst) &&
	                   compares (block->ops[1].inst) &&
	                   (block->ops[2].inst == 0x89FC || block->ops[2].inst == 0x8BFC);
	if (block->ops.size () >= 6) {
		const uint16_t *w = block->words;
		const unsigned x = (w[0] >> 8) & 15;

		/* "while (n--)" on the stack: n at r15 + 4d */
		if (((w[0] & 0xF0FF) == 0x60F2 || (w[0] & 0xF0F0) == 0x50F0) && x != 15 &&
		    w[1] == (0x70FF | x << 8) &&
		    (((w[0] & 0xF000) == 0x6000 && w[2] == (0x2F02 | x << 4)) ||
		     ((w[0] & 0xF000) == 0x5000 && w[2] == (0x1F00 | x << 4 | (w[0] & 15)))) &&
		    w[3] == (0x7001 | x << 8) && w[4] == (0x2008 | x << 8 | x << 4) && w[5] == 0x8BF9) {
			block->mem_loop = Sh4Block::MEM_LOOP_STACK;
			block->mem_reg = 15;
			block->mem_disp = (w[0] & 0xF000) == 0x5000 ? (uint8_t) (w[0] & 15) : 0;
		}
		/* Down to -1 through Rp, read again in R0 */
		const unsigned p = (w[0] >> 4) & 15;
		if ((w[0] & 0xF00F) == 0x6002 && p != x && p != 0 && w[1] == (0x70FF | x << 8) &&
		    w[2] == (0x2002 | p << 8 | x << 4) && w[3] == (0x6002 | p << 4) && w[4] == 0x88FF &&
		    w[5] == 0x8BF9) {
			block->mem_loop = Sh4Block::MEM_LOOP_EQ;
			block->mem_reg = (uint8_t) p;
			block->mem_disp = 0;
		}
		/* While positive, through a pointer from the literals, the store
		 * in bt/s's slot */
		const unsigned rp = (w[0] >> 8) & 15, rx = (w[1] >> 8) & 15;
		if ((w[0] & 0xF000) == 0xD000 && rp != rx && w[1] == (0x6002 | rx << 8 | rp << 4) &&
		    w[2] == (0x4015 | rx << 8) && w[3] == (0x70FF | rx << 8) && w[4] == 0x8DFA &&
		    w[5] == (0x2002 | rp << 8 | rx << 4)) {
			block->mem_loop = Sh4Block::MEM_LOOP_PL;
			block->mem_reg = (uint8_t) rp;
			block->mem_disp = 0;
		}
		for (unsigned k = 0; k < 6 && block->mem_loop; k++)
			if (block->ops[k].patched)
				block->mem_loop = Sh4Block::MEM_LOOP_NONE;
	}
	num_decoded++;
	return !block->ops.empty ();
}

Sh4Block *
Sh4BlockCache::get (Sh4 *ctx, uint32_t pc)
{
	std::unique_ptr<Page> &page = pages[(pc & ADDR_MASK) >> PAGE_BITS];
	std::unique_ptr<Sh4Block> *slot = NULL;

	pc &= ADDR_MASK;
	/* No memory left for a new block (another program took it): none, the
	 * code fetched an instruction at a time until there is (the run's way
	 * for code outside RAM and ROM), rather than the emulator stopped */
	try {
		if (!page)
			page.reset (new Page ());
		slot = &page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1];
		if (*slot && fresh (slot->get ()))
			return slot->get ();
		if (!*slot)
			slot->reset (new Sh4Block ());
		if (!decode (ctx, pc, slot->get ())) {
			slot->reset ();
			return NULL;
		}
	} catch (const std::bad_alloc &) {
		if (slot)
			slot->reset ();
		HL_ERROR_FEW ("sh4: no memory left (another program took it?): code at %08X interpreted", pc);
		return NULL;
	}
	/* Its code just read */
	if ((*slot)->checkable) {
		(*slot)->checked = epoch;
		stamp (slot->get ());
	}
	return slot->get ();
}

void
Sh4BlockCache::stamp (const Sh4Block *block)
{
	uint32_t a = block->pc >> STAMP_BITS;
	uint32_t b = (block->pc + 2 * block->num_words - 1) >> STAMP_BITS;

	for (; a <= b; a++)
		if (page_epochs[a] != ARMED)
			page_epochs[a] = epoch;
	stamped++;
}

bool
Sh4BlockCache::covered (uint32_t addr, unsigned size)
{
	uint32_t last = addr + size - 1, pc;

	for (const Uncovered &u : uncovered)
		if (u.addr == addr && u.size >= size && u.epoch == epoch && u.stamped == stamped)
			return false;
	/* The blocks that may reach there: from 33 words before, in the page
	 * (none crosses one) */
	for (pc = addr & ~1u; ; pc -= 2) {
		const std::unique_ptr<Page> &page = pages[pc >> PAGE_BITS];
		const Sh4Block *block = page ? page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1].get () : NULL;

		if (block && block->checkable && block->checked == epoch &&
		    block->pc + 2 * block->num_words > addr)
			return true;
		if (!(pc & ((1u << PAGE_BITS) - 1)) || (addr & ~1u) - pc >= 2 * (MAX_OPS + 1))
			break;
	}
	/* Those it writes over (a word, a long, a pair) */
	for (pc = (addr & ~1u) + 2; pc <= last; pc += 2) {
		const std::unique_ptr<Page> &page = pages[pc >> PAGE_BITS];

		if (page && page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1] &&
		    page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1]->checked == epoch)
			return true;
	}
	uncovered[next_uncovered] = Uncovered { addr, size, epoch, stamped };
	next_uncovered = (next_uncovered + 1) & 3;
	return false;
}

bool
Sh4BlockCache::fresh (Sh4Block *block)
{
	if (block->checked == epoch) {
		if (!checking || block->current ())
			return true;
		HL_ERROR ("SH-4 block @%08X rewritten, the code epoch the same", block->pc);
		return false;
	}
	if (!block->current ()) {
		if (!held_in_cache (block))
			return false;
		/* Looked at again at each entry: not compared for the epoch;
		 * its lines' replacement starts another */
		held_epoch = epoch;
		for (uint32_t a = block->pc & ~((1u << Sh4ICache::LINE_BITS) - 1);
		     a < block->pc + 2 * block->num_words; a += 1u << Sh4ICache::LINE_BITS)
			icache.stale[Sh4ICache::index (a)] = true;
		return true;
	}
	if (block->checkable) {
		block->checked = epoch;
		stamp (block);
	}
	return true;
}

bool
Sh4BlockCache::held_in_cache (const Sh4Block *block) const
{
	for (unsigned i = 0; i < block->num_words; i++) {
		const uint32_t at = block->pc + 2 * i;
		const uint16_t *line = icache.line (at);

		if (!line || line[(at >> 1) & (Sh4ICache::LINE_WORDS - 1)] != block->words[i])
			return false;
	}
	return true;
}

/* A store at or above the stamp's epoch: a watched page's first, told and
 * let be (all its stamps); else on code compared in this epoch, when it
 * writes over some */
void
Sh4BlockCache::hit (uint32_t stamp, uint32_t addr, unsigned size)
{
	if (page_epochs[stamp] == ARMED) {
		uint32_t page = stamp >> (PAGE_BITS - STAMP_BITS);

		std::fill_n (&page_epochs[page * PAGE_STAMPS], PAGE_STAMPS, 0u);
		hits.push_back (page);
		watch_told (page << PAGE_BITS);
	} else if (covered (addr & ADDR_MASK, size))
		new_epoch ();
}

void
Sh4BlockCache::written_range (uint32_t addr, uint32_t len)
{
	const uint32_t a = addr & ADDR_MASK;
	uint32_t s, e;

	if (!len)
		return;
	e = (a + len - 1) & ADDR_MASK;
	if (e < a)
		e = ADDR_MASK;
	for (s = a >> STAMP_BITS; s <= e >> STAMP_BITS; s++)
		if (page_epochs[s] >= epoch) {
			const uint32_t lo = MAX2 (a, s << STAMP_BITS);
			const uint32_t hi = MIN2 (e, ((s + 1) << STAMP_BITS) - 1);

			hit (s, lo, hi - lo + 1);
		}
}

void
Sh4BlockCache::watch (uint32_t lo, uint32_t hi, std::function<void (uint32_t)> told)
{
	uint32_t p;

	watch_told = told;
	for (p = (lo & ADDR_MASK) >> PAGE_BITS; p <= (hi & ADDR_MASK) >> PAGE_BITS; p++) {
		std::fill_n (&page_epochs[p * PAGE_STAMPS], PAGE_STAMPS, ARMED);
		watched.push_back (p);
	}
}

void
Sh4BlockCache::rearm ()
{
	for (uint32_t p : hits)
		std::fill_n (&page_epochs[p * PAGE_STAMPS], PAGE_STAMPS, ARMED);
	hits.clear ();
}

void
Sh4BlockCache::own (uint32_t lo, uint32_t hi)
{
	if (num_own < 4) {
		own_lo[num_own] = lo & ADDR_MASK;
		own_hi[num_own] = hi & ADDR_MASK;
		num_own++;
	}
}

/* At the wrap (short of ARMED), every block and page compared again, the
 * watched pages armed */
void
Sh4BlockCache::new_epoch ()
{
	memset (icache.stale, 0, sizeof (icache.stale));
	if (++epoch < ARMED)
		return;
	epoch = 1;
	std::fill (page_epochs.get (), page_epochs.get () + NUM_STAMPS, 0u);
	for (uint32_t p : watched)
		std::fill_n (&page_epochs[p * PAGE_STAMPS], PAGE_STAMPS, ARMED);
	hits.clear ();
	for (std::unique_ptr<Page> &page : pages)
		if (page)
			for (std::unique_ptr<Sh4Block> &block : page->blocks)
				if (block)
					block->checked = 0;
}

void
Sh4BlockCache::clear ()
{
	for (std::unique_ptr<Page> &page : pages)
		page.reset ();
	num_decoded = 0;
	icache.reset ();
	new_epoch ();
}
