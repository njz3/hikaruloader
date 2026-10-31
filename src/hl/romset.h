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

#ifndef __HL_ROMSET_H__
#define __HL_ROMSET_H__

#include <string>
#include <vector>

#include <miniz.h>

#include "hl/core.h"
#include "hl/buffer.h"

namespace hl {

/* The ROM files of a game, looked for in MAME-style sets under the games
 * folders: the zips <name>.zip of the game, of its parent and of its BIOS
 * (merged, split or non-merged alike), then the folders <dir>/<name>/ of the
 * older layout. Several folders, separated by ';', are looked in in their
 * order, a set's zip in each that has it: the BIOS can be in one, the games
 * in another.
 *
 * As in sm2-emu, a file is identified by its CRC32 first and by its name
 * second: the clones of a merged set, which MAME keeps in sub-folders of the
 * parent's zip, are found that way, and a renamed file still loads. */
class RomSet {
public:
	RomSet (const char *path, const char * const *names, unsigned num_names);

	/* The folders of a path list, separated by ';', in order: each one
	 * trimmed of its spaces, the empty ones left out */
	static std::vector<std::string> split_folders (const char *path);
	~RomSet ();

	RomSet (const RomSet &) = delete;
	RomSet &operator= (const RomSet &) = delete;

	/* crc is 0 when unknown; alias is an alternative file name for the
	 * folders, or NULL */
	bool	 has (const char *name, const char *alias, uint32_t crc, unsigned size);
	Buffer	*load (const char *name, const char *alias, uint32_t crc, unsigned size);

private:
	static const unsigned MAX_DIRS = 4;
	/* A set's zip in each of several folders */
	static const unsigned MAX_ZIPS = 16;

	bool	 find_in_zips (const char *name, uint32_t crc, unsigned size,
		               unsigned *zip, unsigned *index);
	bool	 find_in_dirs (const char *name, const char *alias, unsigned size,
		               char *found, size_t found_size);

	/* The games folders, in order */
	std::vector<std::string> roots;
	std::vector<std::string> dirs;
	mz_zip_archive zips[MAX_ZIPS];
	std::string zip_names[MAX_ZIPS];
	unsigned num_zips;
};

} /* namespace hl */

#endif /* __HL_ROMSET_H__ */
