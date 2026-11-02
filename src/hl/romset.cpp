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

#include <algorithm>
#include <cctype>
#include <cstring>

#include "hl/romset.h"

namespace hl {

/* Archive member names may carry a clone's sub-folder: compare the last part
 * only, ignoring case. */
static const char *
base_name (const char *path)
{
	const char *slash = strrchr (path, '/');
	const char *back = strrchr (path, '\\');
	if (back && (!slash || back > slash))
		slash = back;
	return slash ? slash + 1 : path;
}

std::vector<std::string>
RomSet::split_folders (const char *path)
{
	std::vector<std::string> folders;
	const char *p = path;

	while (p && *p) {
		const char *end = strchr (p, ';');
		const char *last = end ? end : p + strlen (p);
		const char *first = p;

		while (first < last && isspace ((unsigned char) *first))
			first++;
		while (last > first && isspace ((unsigned char) last[-1]))
			last--;
		if (last > first)
			folders.push_back (std::string (first, last));
		p = end ? end + 1 : NULL;
	}
	return folders;
}

RomSet::RomSet (const char *path, const char * const *names, unsigned num_names) :
	roots (split_folders (path))
{
	unsigned i, k;

	for (i = 0; i < num_names && dirs.size () < MAX_DIRS; i++) {
		if (!names[i] || !names[i][0])
			continue;
		if (std::find (dirs.begin (), dirs.end (), names[i]) != dirs.end ())
			continue;
		dirs.push_back (names[i]);

		/* In each folder that has them, the first folders first; in a
		 * folder, the zip before the 7z */
		for (const std::string &root : roots)
			for (k = 0; k < Archive::num_extensions; k++) {
				Archive *archive;

				if (archives.size () == MAX_ARCHIVES)
					break;
				archive = Archive::open (root + "/" + names[i] + Archive::extensions[k]);
				if (archive)
					archives.push_back (archive);
			}
	}
}

RomSet::~RomSet ()
{
	for (Archive *archive : archives)
		delete archive;
}

/* Finds the member holding the file: by CRC when known, else by name. */
bool
RomSet::find_in_archives (const char *name, uint32_t crc, unsigned size,
                          Archive **archive, const Archive::Member **member)
{
	unsigned pass;

	for (pass = crc ? 0 : 1; pass < 2; pass++)
		for (Archive *a : archives)
			for (const Archive::Member &m : a->members ()) {
				if (m.size != size)
					continue;
				if ((pass == 0 && m.has_crc && m.crc == crc) ||
				    (pass == 1 && !strcasecmp (base_name (m.name.c_str ()), name))) {
					*archive = a;
					*member = &m;
					return true;
				}
			}
	return false;
}

/* Finds the file in the folders of the older layout, under name or alias. */
bool
RomSet::find_in_dirs (const char *name, const char *alias, unsigned size,
                      char *found, size_t found_size)
{
	const char *candidates[2] = { name, alias };
	unsigned k;

	for (const std::string &dir : dirs)
		for (const std::string &root : roots)
		for (k = 0; k < 2; k++) {
			FILE *fp;
			long len;

			if (!candidates[k])
				continue;
			snprintf (found, found_size, "%s/%s/%s", root.c_str (), dir.c_str (), candidates[k]);
			fp = fopen (found, "rb");
			if (!fp)
				continue;
			fseek (fp, 0, SEEK_END);
			len = ftell (fp);
			fclose (fp);
			if (len == (long) size)
				return true;
		}
	return false;
}

bool
RomSet::has (const char *name, const char *alias, uint32_t crc, unsigned size)
{
	char found[1024];
	Archive *archive;
	const Archive::Member *member;

	return find_in_archives (name, crc, size, &archive, &member) ||
	       find_in_dirs (name, alias, size, found, sizeof (found));
}

Buffer *
RomSet::load (const char *name, const char *alias, uint32_t crc, unsigned size)
{
	char found[1024];
	Archive *archive;
	const Archive::Member *member;

	if (find_in_archives (name, crc, size, &archive, &member)) {
		Buffer *buffer = Buffer::create (size, 0);

		if (!buffer)
			return NULL;
		HL_PRINT ("Loading %u bytes of '%s' from '%s' [%s]",
		          size, name, archive->path ().c_str (), member->name.c_str ());
		/* The reason, in the log */
		if (!archive->read (*member, buffer->ptr (0)))
			destroy (buffer);
		return buffer;
	}

	if (find_in_dirs (name, alias, size, found, sizeof (found))) {
		HL_PRINT ("Loading %u bytes of '%s' from '%s'", size, name, found);
		return Buffer::from_file (found, size);
	}

	return NULL;
}

} /* namespace hl */
