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

#include <stdio.h>
#include <string.h>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

#include "hl/nvram.h"

namespace hl {

static const char MAGIC[4] = { 'H', 'L', 'N', 'V' };

#define HEADER_SIZE	16
#define ENTRY_SIZE	16
#define ALIGNMENT	16
/* Far past what a board keeps: a larger file is not one of these */
#define MAX_FILE	(64L << 20)

static uint32_t
get_u32 (const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void
put_u32 (uint8_t *p, uint32_t val)
{
	p[0] = (uint8_t) val;
	p[1] = (uint8_t) (val >> 8);
	p[2] = (uint8_t) (val >> 16);
	p[3] = (uint8_t) (val >> 24);
}

static size_t
align (size_t size)
{
	return (size + ALIGNMENT - 1) & ~(size_t) (ALIGNMENT - 1);
}

Nvram::Status
Nvram::load (const char *path)
{
	std::vector<uint8_t> file;
	std::vector<Block> found;
	uint32_t count, i;
	long size;
	bool ok;
	FILE *fp;

	fp = fopen (path, "rb");
	if (!fp)
		return MISSING;
	ok = !fseek (fp, 0, SEEK_END) && (size = ftell (fp)) >= 0 &&
	     size <= MAX_FILE && !fseek (fp, 0, SEEK_SET);
	if (ok) {
		file.resize ((size_t) size);
		ok = fread (file.data (), 1, file.size (), fp) == file.size ();
	}
	fclose (fp);
	if (!ok || file.size () < HEADER_SIZE || memcmp (file.data (), MAGIC, 4) ||
	    get_u32 (&file[4]) != VERSION)
		return INVALID;

	count = get_u32 (&file[8]);
	if (count > MAX_BLOCKS || HEADER_SIZE + count * ENTRY_SIZE > file.size ())
		return INVALID;
	for (i = 0; i < count; i++) {
		const uint8_t *entry = &file[HEADER_SIZE + i * ENTRY_SIZE];
		uint32_t offs = get_u32 (entry + 4), len = get_u32 (entry + 8);
		Block block;

		if (offs > file.size () || len > file.size () - offs)
			return INVALID;
		memcpy (block.type, entry, 4);
		block.data.assign (file.begin () + offs, file.begin () + offs + len);
		found.push_back (std::move (block));
	}
	blocks = std::move (found);
	return LOADED;
}

bool
Nvram::save (const char *path) const
{
	std::filesystem::path target (path), temp (path);
	std::vector<uint8_t> file;
	std::error_code error;
	size_t i;
	bool ok;
	FILE *fp;

	/* The header and the directory, then each block's data */
	file.resize (align (HEADER_SIZE + blocks.size () * ENTRY_SIZE));
	memcpy (&file[0], MAGIC, 4);
	put_u32 (&file[4], VERSION);
	put_u32 (&file[8], (uint32_t) blocks.size ());
	for (i = 0; i < blocks.size (); i++) {
		uint8_t *entry = &file[HEADER_SIZE + i * ENTRY_SIZE];

		memcpy (entry, blocks[i].type, 4);
		put_u32 (entry + 4, (uint32_t) file.size ());
		put_u32 (entry + 8, (uint32_t) blocks[i].data.size ());
		file.insert (file.end (), blocks[i].data.begin (), blocks[i].data.end ());
		file.resize (align (file.size ()));
	}

	if (target.has_parent_path ())
		std::filesystem::create_directories (target.parent_path (), error);
	temp += ".tmp";
	fp = fopen (temp.string ().c_str (), "wb");
	if (!fp)
		return false;
	ok = fwrite (file.data (), 1, file.size (), fp) == file.size ();
	ok = (fclose (fp) == 0) && ok;
	/* Another program (an antivirus, the search indexer) may hold the file
	 * just written, or the one it replaces, for a few milliseconds: Windows
	 * then refuses the rename, a few times in a thousand */
	for (int attempt = 0; ok; attempt++) {
		std::filesystem::rename (temp, target, error);
		if (!error || attempt == 50)
			break;
		std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}
	ok = ok && !error;
	if (!ok)
		std::filesystem::remove (temp, error);
	return ok;
}

const std::vector<uint8_t> *
Nvram::get (const char *type) const
{
	for (const Block &block : blocks)
		if (!memcmp (block.type, type, 4))
			return &block.data;
	return NULL;
}

void
Nvram::set (const char *type, const void *data, size_t size)
{
	const uint8_t *bytes = (const uint8_t *) data;
	Block block;

	for (Block &old : blocks)
		if (!memcmp (old.type, type, 4)) {
			old.data.assign (bytes, bytes + size);
			return;
		}
	memcpy (block.type, type, 4);
	block.data.assign (bytes, bytes + size);
	blocks.push_back (std::move (block));
}

} /* namespace hl */
