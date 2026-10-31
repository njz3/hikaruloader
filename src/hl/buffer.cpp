/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/buffer.c, Copyright (C) 2011, 2012, Stefano Teso.
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

#include "hl/buffer.h"

#ifdef _WIN32
#include <malloc.h>
#define NOMINMAX
#include <windows.h>
#endif

#ifndef HL_LITTLE_ENDIAN
#error "Big endian detected here... Are you sure?"
#endif

namespace hl {

static unsigned
get_file_size (FILE *fp)
{
	unsigned size, offs;

	offs = (unsigned) ftell (fp);
	fseek (fp, 0, SEEK_END);
	size = (unsigned) ftell (fp);
	fseek (fp, offs, SEEK_SET);

	return size;
}

static uint8_t *
allocate (unsigned size, unsigned alignment)
{
	uint8_t *ptr = NULL;

	HL_ASSERT (is_pow2 (alignment));

#ifdef _WIN32
	/* No posix_memalign (); _aligned_malloc () memory can't be passed to
	 * free (), so use it for all buffers. */
	ptr = (uint8_t *) _aligned_malloc (size, alignment ? alignment : 16);
#else
	if (alignment) {
		if (posix_memalign ((void **) &ptr, alignment, size))
			ptr = NULL;
	} else
		ptr = (uint8_t *) malloc (size);
#endif
	return ptr;
}

Buffer *
Buffer::create (unsigned size, unsigned alignment, Order order)
{
	uint8_t *ptr;

#ifdef _WIN32
	/* The memories of 64 KB and more in a section of the paging file, as
	 * much memory as from the heap: the memory maps' fastmem can then show
	 * them a second time, at the addresses the CPUs see (Mmap) */
	if (size >= SECTION_MIN && alignment <= SECTION_MIN) {
		HANDLE section = CreateFileMappingW (INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
		                                     0, size, NULL);
		if (section) {
			ptr = (uint8_t *) MapViewOfFile (section, FILE_MAP_ALL_ACCESS, 0, 0, size);
			if (ptr) {
				Buffer *buffer = new Buffer (ptr, size, order);
				buffer->section = section;
				return buffer;
			}
			CloseHandle (section);
		}
	}
#endif
	ptr = allocate (size, alignment);
	if (!ptr)
		return NULL;
	return new Buffer (ptr, size, order);
}

Buffer *
Buffer::from_file (const char *path, unsigned reqsize)
{
	Buffer *buffer = NULL;
	unsigned size, read_size;
	FILE *fp = NULL;

	fp = fopen (path, "rb");
	if (!fp)
		goto fail;

	size = get_file_size (fp);
	if (size != reqsize && reqsize != ~0u)
		goto fail;

	buffer = create (size, 0);
	if (!buffer)
		goto fail;

	clearerr (fp);
	read_size = (unsigned) fread (buffer->buf_ptr, 1, size, fp);
	if (read_size != size)
		goto fail;
	if (ferror (fp)) {
		perror (NULL);
		goto fail;
	}
	fclose (fp);
	return buffer;

fail:
	destroy (buffer);
	if (fp)
		fclose (fp);
	return NULL;
}

Buffer::~Buffer ()
{
#ifdef _WIN32
	if (section) {
		UnmapViewOfFile (buf_ptr);
		CloseHandle ((HANDLE) section);
		return;
	}
	_aligned_free (buf_ptr);
#else
	free (buf_ptr);
#endif
}

void *
Buffer::ptr (unsigned offs)
{
	if (offs < buf_size)
		return (void *) &buf_ptr[offs];
	return NULL;
}

void
Buffer::clear ()
{
	HL_ASSERT (buf_ptr);
	memset (buf_ptr, 0, buf_size);
}

void
Buffer::print_some (unsigned lo, unsigned hi) const
{
	unsigned i;

	for (i = lo; i < hi; i++)
		printf ("%02X %c\n", buf_ptr[i], buf_ptr[i]);
}

void
Buffer::print () const
{
	print_some (0, buf_size);
}

void
Buffer::dump (const char *path) const
{
	FILE *fp;

	/* swap if required */

	if (!buf_ptr || !buf_size)
		return;

	fp = fopen (path, "wb");
	if (!fp)
		return;

	fwrite (buf_ptr, 1, buf_size, fp);
	fclose (fp);
}

void
Buffer::dumpf (const char *fmt, ...) const
{
	char path[256] = "";
	va_list va;

	va_start (va, fmt);
	vsnprintf (path, sizeof (path), fmt, va);
	va_end (va);

	dump (path);
}

int
Buffer::load_state (State *state)
{
	HL_ASSERT (state);
	return state->get (buf_ptr, buf_size);
}

int
Buffer::save_state (State *state)
{
	HL_ASSERT (state);
	return state->put (buf_ptr, buf_size);
}

} /* namespace hl */
