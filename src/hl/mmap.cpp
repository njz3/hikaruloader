/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/mmap.c, Copyright (C) 2011, Stefano Teso.
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

#include "hl/mmap.h"
#include "hl/machine.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace hl {

static inline unsigned
get_size_flag_for_size (unsigned size)
{
	switch (size) {
	case 1:
		return HL_REGION_SIZE_8;
	case 2:
		return HL_REGION_SIZE_16;
	case 4:
		return HL_REGION_SIZE_32;
	case 8:
		return HL_REGION_SIZE_64;
	}
	return 0;
}

int
Mmap::add (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
           Buffer *buf, Device *dev, const char *name)
{
	HL_ASSERT (flags & HL_REGION_RW);
	HL_ASSERT (flags & HL_REGION_SIZE_ALL);
	HL_ASSERT (hi > lo);
	HL_ASSERT (mask != 0);
	HL_ASSERT (buf != NULL || dev != NULL);
	HL_ASSERT (name != NULL);

	regions.push_back (Region { lo, hi, mask, flags, buf, dev, name });
	if (use_pages)
		set_page_table (true);
	if (fast_base)
		set_fastmem (true);
	return 0;
}

/* A page goes to the table when the region find () would give for its
 * every byte is one RAM or ROM, in the host's order, unlogged: the regions
 * taken in find ()'s order, a page any earlier one touches is theirs */
void
Mmap::build_pages (std::vector<uint8_t *> &pages, uint32_t flags, uint32_t log_flag)
{
	const uint32_t size = 1u << PAGE_BITS;
	std::vector<bool> taken (NUM_PAGES, false);

	pages.assign (NUM_PAGES, NULL);
	for (const Region &r : regions) {
		uint32_t first = r.lo >> PAGE_BITS, last, page;

		if (!(r.flags & flags) || first >= NUM_PAGES)
			continue;
		last = MIN2 (r.hi >> PAGE_BITS, NUM_PAGES - 1);
		for (page = first; page <= last; page++) {
			uint32_t lo = page << PAGE_BITS, offs = lo & r.mask;

			if (taken[page])
				continue;
			taken[page] = true;
			if (lo < r.lo || lo + (size - 1) > r.hi ||
			    !(r.flags & HL_REGION_DIRECT) || (r.flags & log_flag) ||
			    r.buf->byte_order () == Buffer::BE32 ||
			    (r.mask & (size - 1)) != size - 1 ||
			    offs + size > r.buf->size ())
				continue;
			pages[page] = (uint8_t *) r.buf->ptr (offs);
		}
	}
}

#ifdef _WIN32
/* Windows 10 1803's placeholders, looked up: a reservation views replace
 * piece by piece (VirtualAlloc2, MapViewOfFile3) */
typedef PVOID (WINAPI *VirtualAlloc2Fn) (HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                          MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn) (HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
                                          ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);

static bool
placeholders (VirtualAlloc2Fn *alloc, MapViewOfFile3Fn *map)
{
	HMODULE kernelbase = GetModuleHandleW (L"kernelbase.dll");

	if (!kernelbase)
		return false;
	*alloc = (VirtualAlloc2Fn) (void *) GetProcAddress (kernelbase, "VirtualAlloc2");
	*map = (MapViewOfFile3Fn) (void *) GetProcAddress (kernelbase, "MapViewOfFile3");
	return *alloc && *map;
}
#endif

bool
Mmap::set_fastmem (bool on)
{
#ifdef _WIN32
	const SIZE_T granule = (SIZE_T) 1 << GRANULE_BITS;
	const SIZE_T space = (SIZE_T) NUM_GRANULES << GRANULE_BITS;
	VirtualAlloc2Fn alloc;
	MapViewOfFile3Fn map;
	uint32_t g;

	/* Off: the views go, the placeholders left freed one by one */
	if (fast_base) {
		uint8_t *at = fast_base, *end = fast_base + space;

		for (void *view : fast_views)
			UnmapViewOfFile (view);
		fast_views.clear ();
		while (at < end) {
			MEMORY_BASIC_INFORMATION info;

			if (!VirtualQuery (at, &info, sizeof (info)))
				break;
			if (info.State != MEM_FREE)
				VirtualFree (info.AllocationBase, 0, MEM_RELEASE);
			at = (uint8_t *) info.BaseAddress + info.RegionSize;
		}
		fast_base = NULL;
		memset (fast_read, 0, sizeof (fast_read));
		memset (fast_write, 0, sizeof (fast_write));
	}
	layout_version++;
	if (!on)
		return true;
	if (!placeholders (&alloc, &map))
		return false;

	fast_base = (uint8_t *) alloc (NULL, NULL, space,
	                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
	                               PAGE_NOACCESS, NULL, 0);
	if (!fast_base)
		return false;

	/* Each granule whose bytes find () gives to one RAM or ROM, as for
	 * the page table: the first region touching it decides */
	for (g = 0; g < NUM_GRANULES; g++) {
		uint32_t lo = g << GRANULE_BITS, hi = lo + (uint32_t) granule - 1;
		const Region *reader = NULL, *writer = NULL;
		bool read_set = false, write_set = false;

		for (const Region &r : regions) {
			if (r.hi < lo || r.lo > hi)
				continue;
			if (!read_set && (r.flags & HL_REGION_R)) {
				reader = &r;
				read_set = true;
			}
			if (!write_set && (r.flags & HL_REGION_W)) {
				writer = &r;
				write_set = true;
			}
		}
		if (!reader || reader->lo > lo || reader->hi < hi ||
		    !(reader->flags & HL_REGION_DIRECT) || (reader->flags & HL_REGION_LOG_R) ||
		    reader->buf->byte_order () == Buffer::BE32 || !reader->buf->shared_section () ||
		    (reader->mask & (granule - 1)) != granule - 1 ||
		    (lo & reader->mask) + granule > reader->buf->size ())
			continue;

		{
			bool writable = writer == reader && !(reader->flags & HL_REGION_LOG_W);
			uint8_t *at = fast_base + lo;
			void *view;

			/* Its own placeholder, then the view in its place */
			VirtualFree (at, granule, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
			view = map ((HANDLE) reader->buf->shared_section (), GetCurrentProcess (), at,
			            lo & reader->mask, granule, MEM_REPLACE_PLACEHOLDER,
			            writable ? PAGE_READWRITE : PAGE_READONLY, NULL, 0);
			if (!view) {
				set_fastmem (false);
				return false;
			}
			fast_views.push_back (view);
			fast_read[g >> 3] |= (uint8_t) (1u << (g & 7));
			if (writable)
				fast_write[g >> 3] |= (uint8_t) (1u << (g & 7));
		}
	}
	return true;
#else
	(void) on;
	return !on;
#endif
}

void
Mmap::set_page_table (bool on)
{
	use_pages = on;
	layout_version++;
	if (on) {
		build_pages (read_pages, HL_REGION_R, HL_REGION_LOG_R);
		build_pages (write_pages, HL_REGION_W, HL_REGION_LOG_W);
	} else {
		read_pages.clear ();
		write_pages.clear ();
	}
	if (wide_read)
		fill_wide ();
}

void
Mmap::make_wide () const
{
	const size_t size = sizeof (uint8_t *) << (32 - PAGE_BITS);

	if (wide_read)
		return;
#ifdef _WIN32
	wide_read = (uint8_t **) VirtualAlloc (NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	wide_write = (uint8_t **) VirtualAlloc (NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
	wide_read = (uint8_t **) calloc (1, size);
	wide_write = (uint8_t **) calloc (1, size);
#endif
	if (!wide_read || !wide_write)
		HL_ABORT ("out of memory for the page tables");
	fill_wide ();
}

/* Each page of the tables at its aliases; those that held one before
 * cleared (a table made again), the others left untouched (not committed) */
void
Mmap::fill_wide () const
{
	for (uint32_t page = 0; page < NUM_PAGES; page++)
		for (unsigned k = 0; k < WIDE_ALIASES; k++) {
			const uint32_t at = (k << (29 - PAGE_BITS)) | page;
			uint8_t *r = page < read_pages.size () ? read_pages[page] : NULL;
			uint8_t *w = page < write_pages.size () ? write_pages[page] : NULL;

			if (r || wide_read[at])
				wide_read[at] = r;
			if (w || wide_write[at])
				wide_write[at] = w;
		}
}

Mmap::~Mmap ()
{
	set_fastmem (false);
#ifdef _WIN32
	if (wide_read)
		VirtualFree (wide_read, 0, MEM_RELEASE);
	if (wide_write)
		VirtualFree (wide_write, 0, MEM_RELEASE);
#else
	free (wide_read);
	free (wide_write);
#endif
}

int
Mmap::add_ram (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
               Buffer *buf, const char *name)
{
	flags |= HL_REGION_DIRECT | HL_REGION_RW | HL_REGION_SIZE_ALL;
	return add (lo, hi, mask, flags, buf, NULL, name);
}

int
Mmap::add_rom (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
               Buffer *buf, const char *name)
{
	HL_ASSERT (!(flags & HL_REGION_W));
	flags |= HL_REGION_DIRECT | HL_REGION_R | HL_REGION_SIZE_ALL;
	return add (lo, hi, mask, flags, buf, NULL, name);
}

int
Mmap::add_dev (uint32_t lo, uint32_t hi, uint32_t mask, uint32_t flags,
               Device *dev, const char *name)
{
	HL_ASSERT (!(flags & HL_REGION_DIRECT));
	return add (lo, hi, mask, flags, NULL, dev, name);
}

Mmap::Region *
Mmap::find (uint32_t addr, uint32_t flags)
{
	HL_ASSERT ((flags & ~HL_REGION_RW) == 0);

	for (Region &region : regions)
		if (addr >= region.lo && addr <= region.hi &&
		    (region.flags & flags))
			return &region;
	return NULL;
}

bool
Mmap::window (uint32_t addr, Window *win)
{
	Region *region = find (addr, HL_REGION_R);

	if (!region || !(region->flags & HL_REGION_DIRECT) ||
	    (region->flags & HL_REGION_LOG_R) ||
	    region->buf->byte_order () == Buffer::BE32 ||
	    region->buf->size () <= region->mask)
		return false;
	win->lo = region->lo;
	win->hi = region->hi;
	win->mask = region->mask;
	win->base = (const uint8_t *) region->buf->ptr (0);
	return true;
}

int
Mmap::get (unsigned size, uint32_t addr, void *data)
{
	Region *region;

	HL_ASSERT (data != NULL);
	HL_ASSERT (is_size_valid (size));

	if (fast (fast_read, addr)) {
		memcpy (data, fast_base + addr, size);
		return 0;
	}
	if (use_pages && addr < (NUM_PAGES << PAGE_BITS)) {
		const uint8_t *page = read_pages[addr >> PAGE_BITS];
		if (page) {
			memcpy (data, page + (addr & ((1u << PAGE_BITS) - 1)), size);
			return 0;
		}
	}

	region = find (addr, HL_REGION_R);
	if (!region || !(region->flags & get_size_flag_for_size (size)))
		return -1;

	if (region->flags & HL_REGION_LOG_R)
		HL_MACH_LOG (mach, "%s R%u %08X", region->name.c_str (), size * 8, addr);

	if (region->flags & HL_REGION_DIRECT) {
		uint32_t offs = addr & region->mask;
		set_ptr (data, size, region->buf->get (size, offs));
		return 0;
	}

	return region->dev->get (size, addr, data);
}

int
Mmap::put (unsigned size, uint32_t addr, uint64_t data)
{
	Region *region;

	HL_ASSERT (is_size_valid (size));

	if (fast (fast_write, addr)) {
		memcpy (fast_base + addr, &data, size);
		return 0;
	}
	if (use_pages && addr < (NUM_PAGES << PAGE_BITS)) {
		uint8_t *page = write_pages[addr >> PAGE_BITS];
		if (page) {
			memcpy (page + (addr & ((1u << PAGE_BITS) - 1)), &data, size);
			return 0;
		}
	}

	region = find (addr, HL_REGION_W);
	if (!region)
		return -1;

	if (!(region->flags & get_size_flag_for_size (size)))
		return -1;

	if (region->flags & HL_REGION_LOG_W)
		HL_MACH_LOG (mach, "%s W%u %08X = %llX", region->name.c_str (), size * 8, addr,
		             (unsigned long long) data);

	if (region->flags & HL_REGION_DIRECT) {
		uint32_t offs = addr & region->mask;
		region->buf->put (size, offs, data);
		return 0;
	}

	return region->dev->put (size, addr, data);
}

} /* namespace hl */
