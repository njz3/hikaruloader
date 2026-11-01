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
	~Mmap () { set_fastmem (false); }

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
	bool	 use_pages = false;
	std::vector<uint8_t *> read_pages, write_pages;

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
