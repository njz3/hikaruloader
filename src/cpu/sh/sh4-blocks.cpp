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

#include "cpu/sh/sh4-blocks.h"
#include "cpu/sh/sh4.h"

#define ADDR_MASK	0x1FFFFFFF

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

	/* Within the RAM or ROM, and the page */
	end = MIN2 (win.hi, pc | ((1u << PAGE_BITS) - 1));
	block->pc = pc;
	block->code = (const uint16_t *) (win.base + (pc & win.mask));
	block->words.clear ();
	block->ops.clear ();
	block->native = NULL;
	block->native_generation = 0;
	block->patched = false;
	for (a = pc; a + 1 <= end && block->ops.size () < MAX_OPS; a += 2) {
		uint16_t inst = *(const uint16_t *) (win.base + (a & win.mask));
		Sh4Op op = { sh4_handler (inst), inst, ctx->patches (a) };

		block->words.push_back (inst);
		block->ops.push_back (op);
		block->patched |= op.patched;
		if (ends_block (inst))
			break;
	}
	num_decoded++;
	return !block->ops.empty ();
}

Sh4Block *
Sh4BlockCache::get (Sh4 *ctx, uint32_t pc)
{
	std::unique_ptr<Page> &page = pages[(pc & ADDR_MASK) >> PAGE_BITS];
	std::unique_ptr<Sh4Block> *slot;

	pc &= ADDR_MASK;
	if (!page)
		page.reset (new Page ());
	slot = &page->blocks[(pc & ((1u << PAGE_BITS) - 1)) >> 1];
	if (*slot && (*slot)->current ())
		return slot->get ();
	if (!*slot)
		slot->reset (new Sh4Block ());
	if (!decode (ctx, pc, slot->get ())) {
		slot->reset ();
		return NULL;
	}
	return slot->get ();
}

void
Sh4BlockCache::clear ()
{
	for (std::unique_ptr<Page> &page : pages)
		page.reset ();
	num_decoded = 0;
}
