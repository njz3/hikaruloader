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

RomSet::RomSet (const char *path, const char * const *names, unsigned num_names) :
	path (path),
	num_zips (0)
{
	unsigned i;

	memset (zips, 0, sizeof (zips));
	for (i = 0; i < num_names && dirs.size () < MAX_DIRS; i++) {
		std::string zip_path;

		if (!names[i] || !names[i][0])
			continue;
		if (std::find (dirs.begin (), dirs.end (), names[i]) != dirs.end ())
			continue;
		dirs.push_back (names[i]);

		zip_path = std::string (path) + "/" + names[i] + ".zip";
		if (mz_zip_reader_init_file (&zips[num_zips], zip_path.c_str (), 0))
			zip_names[num_zips++] = zip_path;
	}
}

RomSet::~RomSet ()
{
	unsigned i;
	for (i = 0; i < num_zips; i++)
		mz_zip_reader_end (&zips[i]);
}

/* Finds the member holding the file: by CRC when known, else by name. */
bool
RomSet::find_in_zips (const char *name, uint32_t crc, unsigned size,
                      unsigned *zip, unsigned *index)
{
	unsigned pass, i, j;

	for (pass = crc ? 0 : 1; pass < 2; pass++)
		for (i = 0; i < num_zips; i++) {
			unsigned n = mz_zip_reader_get_num_files (&zips[i]);
			for (j = 0; j < n; j++) {
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat (&zips[i], j, &stat) ||
				    stat.m_is_directory ||
				    stat.m_uncomp_size != size)
					continue;
				if ((pass == 0 && stat.m_crc32 == crc) ||
				    (pass == 1 && !strcasecmp (base_name (stat.m_filename), name))) {
					*zip = i;
					*index = j;
					return true;
				}
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
		for (k = 0; k < 2; k++) {
			FILE *fp;
			long len;

			if (!candidates[k])
				continue;
			snprintf (found, found_size, "%s/%s/%s", path.c_str (), dir.c_str (), candidates[k]);
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
	unsigned zip, index;

	return find_in_zips (name, crc, size, &zip, &index) ||
	       find_in_dirs (name, alias, size, found, sizeof (found));
}

Buffer *
RomSet::load (const char *name, const char *alias, uint32_t crc, unsigned size)
{
	char found[1024];
	unsigned zip, index;

	if (find_in_zips (name, crc, size, &zip, &index)) {
		mz_zip_archive_file_stat stat;
		Buffer *buffer = Buffer::create (size, 0);

		if (!buffer)
			return NULL;
		mz_zip_reader_file_stat (&zips[zip], index, &stat);
		HL_PRINT ("Loading %u bytes of '%s' from '%s' [%s]",
		          size, name, zip_names[zip].c_str (), stat.m_filename);
		if (!mz_zip_reader_extract_to_mem (&zips[zip], index, buffer->ptr (0), size, 0)) {
			HL_ERROR ("could not extract '%s' from '%s': %s", stat.m_filename,
			          zip_names[zip].c_str (),
			          mz_zip_get_error_string (mz_zip_get_last_error (&zips[zip])));
			destroy (buffer);
		}
		return buffer;
	}

	if (find_in_dirs (name, alias, size, found, sizeof (found))) {
		HL_PRINT ("Loading %u bytes of '%s' from '%s'", size, name, found);
		return Buffer::from_file (found, size);
	}

	return NULL;
}

} /* namespace hl */
