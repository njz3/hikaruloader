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

/* The LZMA SDK's headers first: on Windows they bring windows.h, which must
 * come before GLEW's (hl/core.h) */
#include <7z.h>
#include <7zAlloc.h>
#include <7zCrc.h>
#include <7zFile.h>

#include <cctype>
#include <cstring>

#include <miniz.h>

#include "hl/archive.h"

namespace hl {

const char *const Archive::extensions[] = { ".zip", ".7z" };
const unsigned Archive::num_extensions = 2;

/****************************************************************************
 Zip
****************************************************************************/

namespace {

class ZipArchive : public Archive {
public:
	ZipArchive () : opened (false)
	{
		memset (&zip, 0, sizeof (zip));
	}

	~ZipArchive () override
	{
		if (opened)
			mz_zip_reader_end (&zip);
	}

	bool
	init (const std::string &path)
	{
		mz_uint i, n;

		if (!mz_zip_reader_init_file (&zip, path.c_str (), 0))
			return false;
		opened = true;
		file = path;
		n = mz_zip_reader_get_num_files (&zip);
		for (i = 0; i < n; i++) {
			mz_zip_archive_file_stat stat;
			if (!mz_zip_reader_file_stat (&zip, i, &stat) || stat.m_is_directory)
				continue;
			list.push_back ({ stat.m_filename, stat.m_uncomp_size, stat.m_crc32, true, i });
		}
		return true;
	}

	/* miniz checks the CRC itself */
	bool
	read (const Member &member, void *dst) override
	{
		if (mz_zip_reader_extract_to_mem (&zip, member.index, dst, (size_t) member.size, 0))
			return true;
		HL_ERROR ("could not extract '%s' from '%s': %s", member.name.c_str (), file.c_str (),
		          mz_zip_get_error_string (mz_zip_get_last_error (&zip)));
		return false;
	}

private:
	mz_zip_archive zip;
	bool opened;
};

/****************************************************************************
 7z
****************************************************************************/

const ISzAlloc alloc = { SzAlloc, SzFree };
const ISzAlloc alloc_temp = { SzAllocTemp, SzFreeTemp };

/* What the LZMA SDK reads ahead of the archive at a time, as its 7zMain.c */
const size_t LOOK_AHEAD = 1 << 18;

const char *
sz_error (SRes res)
{
	switch (res) {
	case SZ_ERROR_DATA:		return "damaged data";
	case SZ_ERROR_MEM:		return "out of memory";
	case SZ_ERROR_CRC:		return "wrong CRC";
	case SZ_ERROR_UNSUPPORTED:	return "a compression method not supported";
	case SZ_ERROR_INPUT_EOF:	return "cut short";
	case SZ_ERROR_READ:		return "a read error";
	case SZ_ERROR_ARCHIVE:		return "damaged headers";
	case SZ_ERROR_NO_ARCHIVE:	return "not a 7z archive";
	default:			return "an error";
	}
}

/* A name of the archive, in UTF-16 (up to its 0), as UTF-8: a pair of
 * surrogates is one character */
std::string
utf8_of (const std::vector<UInt16> &utf16)
{
	std::string out;
	size_t i;

	for (i = 0; i < utf16.size () && utf16[i]; i++) {
		uint32_t c = utf16[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < utf16.size () &&
		    utf16[i + 1] >= 0xDC00 && utf16[i + 1] < 0xE000)
			c = 0x10000 + ((c - 0xD800) << 10) + (utf16[++i] - 0xDC00u);
		if (c < 0x80)
			out += (char) c;
		else if (c < 0x800) {
			out += (char) (0xC0 | c >> 6);
			out += (char) (0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out += (char) (0xE0 | c >> 12);
			out += (char) (0x80 | (c >> 6 & 0x3F));
			out += (char) (0x80 | (c & 0x3F));
		} else {
			out += (char) (0xF0 | c >> 18);
			out += (char) (0x80 | (c >> 12 & 0x3F));
			out += (char) (0x80 | (c >> 6 & 0x3F));
			out += (char) (0x80 | (c & 0x3F));
		}
	}
	return out;
}

/* The games folders are UTF-8, as miniz takes them: Windows' wide name, its
 * ANSI one when the path is no UTF-8 */
WRes
open_file (CSzFile *f, const std::string &path)
{
#ifdef _WIN32
	int n = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str (), -1, NULL, 0);
	if (n > 0) {
		std::vector<wchar_t> wide ((size_t) n);
		MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str (), -1, wide.data (), n);
		return InFile_OpenW (f, wide.data ());
	}
#endif
	return InFile_Open (f, path.c_str ());
}

class SevenZipArchive : public Archive {
public:
	SevenZipArchive () :
		stream_open (false),
		block_index (0xFFFFFFFF),
		block (NULL),
		block_size (0)
	{
		memset (&stream, 0, sizeof (stream));
		memset (&look, 0, sizeof (look));
		SzArEx_Init (&db);
	}

	~SevenZipArchive () override
	{
		if (block)
			ISzAlloc_Free (&alloc, block);
		SzArEx_Free (&db, &alloc);
		if (look.buf)
			ISzAlloc_Free (&alloc, look.buf);
		if (stream_open)
			File_Close (&stream.file);
	}

	bool
	init (const std::string &path)
	{
		/* The CRC's tables, once for all */
		static const bool crc_ready = (CrcGenerateTable (), true);
		SRes res;
		UInt32 i;

		(void) crc_ready;
		FileInStream_CreateVTable (&stream);
		File_Construct (&stream.file);
		if (open_file (&stream.file, path) != 0)
			return false;
		stream_open = true;

		LookToRead2_CreateVTable (&look, False);
		look.buf = (Byte *) ISzAlloc_Alloc (&alloc, LOOK_AHEAD);
		if (!look.buf)
			return false;
		look.bufSize = LOOK_AHEAD;
		look.realStream = &stream.vt;
		LookToRead2_INIT (&look);

		res = SzArEx_Open (&db, &look.vt, &alloc, &alloc_temp);
		if (res != SZ_OK) {
			HL_ERROR_FEW ("'%s' cannot be read: %s", path.c_str (), sz_error (res));
			return false;
		}
		file = path;
		for (i = 0; i < db.NumFiles; i++) {
			size_t length;
			bool has_crc;

			if (SzArEx_IsDir (&db, i))
				continue;
			length = SzArEx_GetFileNameUtf16 (&db, i, NULL);
			std::vector<UInt16> name (length ? length : 1, 0);
			SzArEx_GetFileNameUtf16 (&db, i, name.data ());
			has_crc = SzBitWithVals_Check (&db.CRCs, i);
			list.push_back ({ utf8_of (name), SzArEx_GetFileSize (&db, i),
			                  has_crc ? db.CRCs.Vals[i] : 0, has_crc, i });
		}
		return true;
	}

	/* The LZMA SDK unpacks the file's whole block (kept for the next
	 * file), and checks the CRC */
	bool
	read (const Member &member, void *dst) override
	{
		size_t offset = 0, size = 0;
		SRes res;

		res = SzArEx_Extract (&db, &look.vt, member.index, &block_index, &block, &block_size,
		                      &offset, &size, &alloc, &alloc_temp);
		if (res != SZ_OK || size != member.size) {
			HL_ERROR ("could not extract '%s' from '%s': %s", member.name.c_str (), file.c_str (),
			          res != SZ_OK ? sz_error (res) : "its size differs");
			return false;
		}
		memcpy (dst, block + offset, size);
		return true;
	}

private:
	CFileInStream stream;
	bool stream_open;
	CLookToRead2 look;
	CSzArEx db;
	/* The block unpacked last */
	UInt32 block_index;
	Byte *block;
	size_t block_size;
};

/* The path ends with ext, in any case */
bool
has_extension (const std::string &path, const char *ext)
{
	size_t n = strlen (ext);
	return path.size () > n && !strcasecmp (path.c_str () + path.size () - n, ext);
}

} /* namespace */

Archive *
Archive::open (const std::string &path)
{
	if (has_extension (path, ".zip")) {
		ZipArchive *zip = new ZipArchive ();
		if (zip->init (path))
			return zip;
		delete zip;
	} else if (has_extension (path, ".7z")) {
		SevenZipArchive *sz = new SevenZipArchive ();
		if (sz->init (path))
			return sz;
		delete sz;
	}
	return NULL;
}

} /* namespace hl */
