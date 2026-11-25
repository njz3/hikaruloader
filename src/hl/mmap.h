/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/mmap.h, Copyright (C) 2011, Stefano Teso.
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

#ifndef __HL_MMAP_H__
#define __HL_MMAP_H__

#include <functional>
#include <string>
#include <vector>

#include "hl/buffer.h"
#include "hl/device.h"

#define HL_REGION_R		(1 << 0)
#define HL_REGION_W		(1 << 1)
#define HL_REGION_RW		(HL_REGION_R|HL_REGION_W)
#define HL_REGION_DIRECT	(1 << 2)
#define HL_REGION_LOG_R		(1 << 4)
#define HL_REGION_LOG_W		(1 << 5)
#define HL_REGION_LOG_RW	(HL_REGION_LOG_R|HL_REGION_LOG_W)
#define HL_REGION_SIZE_8	(1 << 6)
#define HL_REGION_SIZE_16	(1 << 7)
#define HL_REGION_SIZE_32	(1 << 8)
#define HL_REGION_SIZE_64	(1 << 9)
#define HL_REGION_SIZE_ALL	(HL_REGION_SIZE_8|HL_REGION_SIZE_16|HL_REGION_SIZE_32|HL_REGION_SIZE_64)

namespace hl {

class Machine;

/* A CPU's address space: regions of RAM, ROM (buffers read and written
 * directly) and devices (whose get () and put () are called), each with an
 * address mask, looked up in the order they were added. */
class Mmap {
public:
	explicit Mmap (Machine *mach) : mach (mach) {}

	int	add_ram (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
		         Buffer *buf, const char *name);
	int	add_rom (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
		         Buffer *buf, const char *name);
	int	add_dev (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
		         Device *dev, const char *name);

	/* 0 when an access is handled, -1 otherwise */
	int	get (unsigned size, uint32_t addr, void *data);
	int	put (unsigned size, uint32_t addr, uint64_t data);
	/* n words put from addr on, as n puts of 4 bytes one after the other
	 * (a store queue's burst): the region found once */
	int	put_words (uint32_t addr, const uint32_t *words, unsigned n);
	/* Ranges guarded (guard ()): put_words () tells of a store there */
	bool	guarding () const { return !guard_ranges.empty (); }

	/* The page table: for each 4 KB page of the first 512 MB that one
	 * RAM or ROM holds whole (no device, no log), where it lies in the
	 * host, for reads and for writes (a RAM's); get () and put () look
	 * there first, the regions for the rest. Off: the regions alone, one
	 * after the other, as before. The same accesses either way */
	void	set_page_table (bool on);
	bool	page_table () const { return use_pages; }

	/* Fastmem: 512 MB of the host's address space reserved, each 64 KB
	 * of RAM or ROM (in a section, Buffer) shown there at the address
	 * the CPU sees, a ROM read-only; get () and put () reach it at base +
	 * address, a bit of a map telling which. The page table, then the
	 * regions, for the rest. false when the host cannot (Windows 10
	 * before 1803, other systems): the page table goes on alone */
	bool	set_fastmem (bool on);
	bool	fastmem () const { return fast_base != NULL; }
	~Mmap ();

	/* For the dynamic recompilers, which read them from the code they
	 * write: the tables, the bit maps, the base; layout () changes each
	 * time they are made again (the code is then out of date) */
	uint8_t	* const *read_table () const { return read_pages.data (); }
	uint8_t	* const *write_table () const { return write_pages.data (); }
	const uint8_t *fastmem_readable () const { return fast_read; }
	const uint8_t *fastmem_writable () const { return fast_write; }
	uint8_t	*fastmem_base () const { return fast_base; }
	unsigned layout () const { return layout_version; }
	static const unsigned TABLE_PAGE_BITS = 12;
	/* The tables over the 32-bit addresses as an SH-4 without its MMU
	 * reads them (sh4_get ()): the 512 MB of the table at each of U0's
	 * four, P1, P2; none at P3, P4 (the on-chip registers, the store
	 * queues). The translations' lookups need then no test of the area
	 * and no 29-bit mask; each entry the page's host address less the
	 * address it is at, the access at the entry plus the address (no mask
	 * of the offset either). Made at the first call (8 MB each, reserved:
	 * only the pages written count), kept with the tables after */
	static const unsigned WIDE_ALIASES = 6;
	const intptr_t *wide_read_table () const { make_wide (); return wide_read; }
	const intptr_t *wide_write_table () const { make_wide (); return wide_write; }
	/* The same with the code pages in it (code_page () leaves them out
	 * of the other): a store there tests the code's stamp itself */
	const intptr_t *wide_write_all_table () const { make_wide (); return wide_write_all; }
	/* Traps: 4 GB of the host's address space reserved, the SH-4's 32-bit
	 * addresses as it sees them without its MMU (the 512 MB at each of
	 * U0's four, P1, P2; nothing at P3 and P4): each 64 KB the page table
	 * reads straight from one RAM or ROM in a section (Buffer) shown there,
	 * writable where it writes there too; each page as the tables say from
	 * then on (none where they read it no more, a guarded page's or a
	 * device's bank shown; read-only where they write it no more, a code
	 * page's). A translation's access goes to base + address unchecked;
	 * one that faults goes through the tables for good (sh4-x64.cpp,
	 * "Traps"). Made at the first call, again with the page table; NULL
	 * where the host cannot (no page table, fastmem, Windows 10 before
	 * 1803, other systems) */
	uint8_t	*traps () const;
	static const uint64_t TRAP_SPACE = 1ull << 32;
	/* Where the first one goes if it can: an access's displacement */
	static const uint64_t TRAP_LOW = 0x7FFF0000ull;
	/* A 4 KB page (29-bit address >> 12) holding an SH-4's code compared
	 * in its epoch (Sh4BlockCache): its stores through the wide write table
	 * to the slow path (sh4_write () looks at the code there), the other
	 * pages' straight (the translations' stores test no code epoch); on
	 * false, back in the table */
	void	 code_page (uint32_t page, bool on);

	/* Where addr lies in the host when get () or put () would go straight
	 * there (fastmem, then the page table), an access not crossing a 4 KB
	 * page; NULL otherwise. For the CPUs' fast paths */
	uint8_t	*direct (uint32_t addr, bool write) const
	{
		if (fast (write ? fast_write : fast_read, addr))
			return fast_base + addr;
		if (use_pages && addr < (NUM_PAGES << PAGE_BITS)) {
			uint8_t *page = (write ? write_pages : read_pages)[addr >> PAGE_BITS];
			if (page)
				return page + (addr & ((1u << PAGE_BITS) - 1));
		}
		return NULL;
	}

	/* A device's 4 KB page at addr read in place, at host, from now on:
	 * a bank it switches in that holds plain memory, read as the device
	 * would give it (the page table only; the code written reads the
	 * table as it runs). NULL: through the device again */
	void	show (uint32_t addr, const uint8_t *host)
	{
		if (use_pages && addr < (NUM_PAGES << PAGE_BITS)) {
			read_pages[addr >> PAGE_BITS] = (uint8_t *) host;
			if (trap_base)
				trap_page (addr >> PAGE_BITS);
			if (wide_read)
				for (unsigned k = 0; k < WIDE_ALIASES; k++) {
					const uint32_t at = (k << (29 - PAGE_BITS)) | (addr >> PAGE_BITS);

					wide_read[at] = wide_bias (host, at);
				}
		}
	}

	/* The page table's pages from lo to hi left to the slow path, hit
	 * called at the first access there through the map (it may unguard
	 * ()): a buffer a device writes behind the CPU's back, which its
	 * accesses wait for. False where the table is not used (fastmem,
	 * regions alone) */
	bool	guard (uint32_t lo, uint32_t hi, std::function<void ()> hit);
	/* The pages guarded back in the table */
	void	unguard ();

	/* A RAM or ROM that holds addr, readable in place (little endian):
	 * the CPUs fetch their code from it without a lookup each time */
	struct Window {
		uint32_t	 lo, hi, mask;
		const uint8_t	*base;
	};
	bool	window (uint32_t addr, Window *win);

	Machine *mach;

private:
	struct Region {
		uint32_t lo;
		uint32_t hi;
		uint32_t mask;
		uint32_t flags;
		Buffer *buf;
		Device *dev;
		std::string name;
		/* No region before it overlaps it with a read, a write: any
		 * address in it finds it first (find ()'s cache) */
		bool first_r, first_w;
	};

	int	 add (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
		      Buffer *buf, Device *dev, const char *name);
	Region	*find (uint32_t addr, uint32_t flags);
	void	 build_pages (std::vector<uint8_t *> &pages, uint32_t flags,
			      uint32_t log_flag);

	static const unsigned PAGE_BITS = 12;
	static const unsigned NUM_PAGES = 1u << (29 - PAGE_BITS);
	static const unsigned GRANULE_BITS = 16;
	static const unsigned NUM_GRANULES = 1u << (29 - GRANULE_BITS);

	std::vector<Region> regions;
	/* find ()'s last answers, read and write, when first there */
	Region	*last_r = NULL, *last_w = NULL;
	/* find ()'s answer for each granule, read and write: the region (its
	 * index) found first at all of its addresses; none there; or several
	 * (looked for) */
	static const int32_t GRANULE_NONE = -2, GRANULE_MIXED = -1;
	std::vector<int32_t> granule_r, granule_w;
	bool	 use_pages = false;
	std::vector<uint8_t *> read_pages, write_pages;
	/* Each page's host address less its own (at its alias's address):
	 * the host's address of an access, the entry plus the address; 0 for
	 * none (a page whose difference is 0 as none: the slow path) */
	mutable intptr_t *wide_read = NULL, *wide_write = NULL, *wide_write_all = NULL;
	static intptr_t wide_bias (const uint8_t *host, uint32_t page)
	{
		return host ? (intptr_t) host - (intptr_t) ((uintptr_t) page << PAGE_BITS) : 0;
	}
	void	 make_wide () const;
	void	 fill_wide () const;
	/* code_page ()'s, a byte a page; a page's entry at a wide table's
	 * place for writes */
	std::vector<uint8_t> code_pages;
	bool	 is_code_page (uint32_t page) const { return page < code_pages.size () && code_pages[page]; }
	intptr_t wide_write_entry (const uint8_t *w, uint32_t page, uint32_t at) const
	{
		return is_code_page (page) ? 0 : wide_bias (w, at);
	}

	/* guard ()'s: the pages taken out of the table (theirs to put back),
	 * the ranges, the call */
	struct Guarded {
		uint32_t page;
		uint8_t	*read, *write;
	};
	std::vector<Guarded> guarded;
	std::vector<std::pair<uint32_t, uint32_t>> guard_ranges;
	std::function<void ()> guard_hit;
	void	 guard_check (uint32_t addr)
	{
		for (const auto &r : guard_ranges)
			if (addr >= r.first && addr <= r.second) {
				std::function<void ()> hit = guard_hit;

				hit ();
				return;
			}
	}

	/* traps ()'s: the reservation, its views; the granules shown, those
	 * writable (a bit each) */
	mutable uint8_t *trap_base = NULL;
	mutable std::vector<void *> trap_views;
	mutable uint8_t trap_shown[NUM_GRANULES / 8] = {}, trap_writable[NUM_GRANULES / 8] = {};
	void	 build_traps () const;
	void	 free_traps () const;
	/* A page of the traps' views as the tables say now */
	void	 trap_page (uint32_t page) const;

	uint8_t	*fast_base = NULL;
	unsigned layout_version = 0;
	/* The granules shown, readable, writable: a bit each */
	uint8_t	 fast_read[NUM_GRANULES / 8] = {}, fast_write[NUM_GRANULES / 8] = {};
	std::vector<void *> fast_views;

	bool	 fast (const uint8_t *bits, uint32_t addr) const
	{
		return fast_base && addr < (NUM_GRANULES << GRANULE_BITS) &&
		       ((bits[addr >> (GRANULE_BITS + 3)] >> ((addr >> GRANULE_BITS) & 7)) & 1);
	}
};

} /* namespace hl */

#endif /* __HL_MMAP_H__ */
