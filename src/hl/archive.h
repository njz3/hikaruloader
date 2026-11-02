/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/app/rom_files.cpp, Copyright (C) 2026,
 * Réda Chérif-Touil.
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

#ifndef __HL_ARCHIVE_H__
#define __HL_ARCHIVE_H__

#include <string>
#include <vector>

#include "hl/core.h"

namespace hl {

/* An archive holding a romset: a zip, read with miniz, or a 7z, read with
 * the LZMA SDK (3rdparty/lzma, public domain). Its files are listed when it
 * is opened, from its headers alone (the picker opens every set's), and each
 * one is read whole. A 7z is usually solid: its files packed one after the
 * other in a block, which is unpacked whole to reach any of them; the last
 * block unpacked is kept until the archive is closed, so that the files of a
 * set, read one after the other, unpack it once. */
class Archive {
public:
	struct Member {
		std::string	 name;		/* as stored: a clone's files may be in a
						 * sub-folder, '/' or '\' between them */
		uint64_t	 size;
		uint32_t	 crc;
		bool		 has_crc;	/* a 7z can leave it out */
		uint32_t	 index;		/* in the archive, folders included */
	};

	/* The archive at path, by its extension (.zip or .7z, any case); NULL
	 * when there is none, or it cannot be read */
	static Archive	*open (const std::string &path);
	/* The extensions open () knows, in the order the sets are looked for */
	static const char *const extensions[];
	static const unsigned num_extensions;

	virtual ~Archive () {}

	const std::string &path () const { return file; }
	/* Its files, the folders left out */
	const std::vector<Member> &members () const { return list; }
	/* A file's bytes, all of member.size, into dst; false when it cannot
	 * be read or its CRC is wrong (the reason in the log) */
	virtual bool	 read (const Member &member, void *dst) = 0;

protected:
	std::string	 file;
	std::vector<Member> list;
};

} /* namespace hl */

#endif /* __HL_ARCHIVE_H__ */
