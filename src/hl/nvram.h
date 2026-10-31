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

#ifndef __HL_NVRAM_H__
#define __HL_NVRAM_H__

#include <string>
#include <vector>

#include "hl/core.h"

namespace hl {

/* A game's non-volatile memories in one file (nvram/<game>.nvram): a header,
 * a directory of the blocks, then their data. Little endian throughout:
 *
 *   Header, 16 bytes
 *     0  char[4]  "HLNV"
 *     4  u32      the version: 1
 *     8  u32      the directory's entries
 *    12  u32      reserved, 0
 *   Directory, right after: 16 bytes an entry
 *     0  char[4]  the block's type: "EEPR", "BRAM"...
 *     4  u32      its data's offset, from the file's start
 *     8  u32      its data's size, in bytes
 *    12  u32      reserved, 0
 *   Data, each block's at its offset, 16-byte aligned
 *
 * The blocks of the types the machine doesn't know stay, and are written back
 * as they were. */
class Nvram {
public:
	static const uint32_t VERSION = 1;
	static const unsigned MAX_BLOCKS = 64;

	enum Status {
		LOADED,
		MISSING,	/* no file */
		INVALID		/* not one of these, or of a later version */
	};

	/* The file's blocks, in place of those held */
	Status	 load (const char *path);
	/* Through a temporary file, so that a crash leaves the old one; the
	 * directory made if needed */
	bool	 save (const char *path) const;

	/* The data of the block of that type (four characters), NULL without */
	const std::vector<uint8_t> *get (const char *type) const;
	/* Adds the block, or replaces the data of the one of that type */
	void	 set (const char *type, const void *data, size_t size);
	size_t	 count () const { return blocks.size (); }

private:
	struct Block {
		char type[4];
		std::vector<uint8_t> data;
	};
	std::vector<Block> blocks;
};

} /* namespace hl */

#endif /* __HL_NVRAM_H__ */
