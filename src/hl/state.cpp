/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/state.c, Copyright (C) 2011-2013, Stefano Teso.
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

#include <filesystem>
#include <system_error>

#include <miniz.h>

#include "hl/state.h"

/* 0 until the first release: the layout changes as the machine does, and a
 * state from another build may not load. Valkyrie's states, "valkyrie state
 * 00000001", do not. */
#define HL_STATE_VERSION	0

namespace hl {

/* A line, padded to 32 bytes */
static void
make_header (char header[32])
{
	memset (header, 0, 32);
	snprintf (header, 32, "hikaruloader state %08X\n", HL_STATE_VERSION);
}

State::State (const char *path, Mode mode) :
	path (path),
	state_mode (mode),
	valid (false),
	offset (0)
{
	char header[32], expected[32];
	std::vector<uint8_t> packed;
	uint64_t size;
	mz_ulong length;
	size_t got;
	FILE *fp;

	HL_ASSERT (path);

	if (mode == SAVE) {
		valid = true;
		return;
	}

	fp = fopen (path, "rb");
	if (!fp)
		return;
	make_header (expected);
	if (fread (header, 1, sizeof (header), fp) != sizeof (header) ||
	    memcmp (header, expected, sizeof (header))) {
		if (!strncmp (header, "valkyrie state ", 15))
			HL_ERROR ("state: '%s' is a Valkyrie state, which Hikaruloader cannot load", path);
		else
			HL_ERROR ("state: '%s' is not a Hikaruloader state of version %u",
			          path, HL_STATE_VERSION);
		goto done;
	}
	if (fread (&size, sizeof (size), 1, fp) != 1 || size > 0xFFFFFFFFull)
		goto done;

	/* The rest of the file */
	do {
		packed.resize (packed.size () + 16*MB);
		got = fread (packed.data () + packed.size () - 16*MB, 1, 16*MB, fp);
		packed.resize (packed.size () - 16*MB + got);
	} while (got == 16*MB);

	data.resize ((size_t) size);
	length = (mz_ulong) size;
	if (mz_uncompress (data.data (), &length, packed.data (), (mz_ulong) packed.size ()) != MZ_OK ||
	    length != size) {
		HL_ERROR ("state: '%s' is damaged", path);
		data.clear ();
		goto done;
	}
	valid = true;
done:
	fclose (fp);
}

int
State::put (const void *src, uint32_t size)
{
	if (state_mode != SAVE || !valid)
		return -1;

	HL_LOG ("state: W%08X %p", size, src);

	data.insert (data.end (), (const uint8_t *) src, (const uint8_t *) src + size);
	return 0;
}

int
State::get (void *dst, uint32_t size)
{
	if (state_mode != LOAD || !valid || offset + size > data.size ())
		return -1;

	HL_LOG ("state: R%08X %p", size, dst);

	memcpy (dst, data.data () + offset, size);
	offset += size;
	return 0;
}

int
State::finish ()
{
	std::vector<uint8_t> packed;
	char header[32];
	uint64_t size = data.size ();
	mz_ulong length;
	bool ok;
	FILE *fp;

	if (!valid)
		return -1;

	if (state_mode == LOAD) {
		if (offset == data.size ())
			return 0;
		HL_ERROR ("state: '%s' holds %u bytes more than this machine reads",
		          path.c_str (), (unsigned) (data.size () - offset));
		return -1;
	}

	/* The fastest level: the RAMs are mostly empty */
	length = mz_compressBound ((mz_ulong) size);
	packed.resize (length);
	if (mz_compress2 (packed.data (), &length, data.data (), (mz_ulong) size, 1) != MZ_OK)
		return -1;

	/* Through a temporary file: a disk full leaves no half state, and the
	 * state of that name as it was */
	std::string temp = path + ".tmp";
	std::error_code error;
	fp = fopen (temp.c_str (), "wb");
	if (!fp)
		return -1;
	make_header (header);
	ok = fwrite (header, 1, sizeof (header), fp) == sizeof (header) &&
	     fwrite (&size, sizeof (size), 1, fp) == 1 &&
	     fwrite (packed.data (), 1, length, fp) == length;
	ok = (fclose (fp) == 0) && ok;
	if (ok)
		std::filesystem::rename (temp, path, error);
	if (!ok || error) {
		std::filesystem::remove (temp, error);
		return -1;
	}
	return 0;
}

} /* namespace hl */
