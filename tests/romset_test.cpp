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

/* The games folders' tests (hl::RomSet): a list of folders separated by
 * ';', a set's zip in each that has it, the first folders first; the sets
 * in 7z (hl::Archive). Run by ctest, or alone: romsettest. */

#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <miniz.h>

#include "hl/archive.h"
#include "hl/romset.h"

static int failures, checks;

static void
check (const char *name, bool ok)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf (stderr, "FAIL %s\n", name);
	}
}

/* A zip of files named as given, each holding its own name, or content when
 * given */
static void
make_zip (const std::string &path, const std::vector<std::string> &files, const char *content = NULL)
{
	mz_zip_archive zip;

	memset (&zip, 0, sizeof (zip));
	if (!mz_zip_writer_init_file (&zip, path.c_str (), 0)) {
		fprintf (stderr, "cannot write %s\n", path.c_str ());
		return;
	}
	for (const std::string &file : files) {
		const char *data = content ? content : file.c_str ();
		mz_zip_writer_add_mem (&zip, file.c_str (), data, strlen (data), MZ_DEFAULT_COMPRESSION);
	}
	mz_zip_writer_finalize_archive (&zip);
	mz_zip_writer_end (&zip);
}

static uint32_t
crc_of (const std::string &text)
{
	return (uint32_t) mz_crc32 (MZ_CRC32_INIT, (const unsigned char *) text.c_str (), text.size ());
}

static bool
has (hl::RomSet &set, const std::string &name, const std::string &content)
{
	return set.has (name.c_str (), NULL, crc_of (content), (unsigned) content.size ());
}

/* A solid 7z made by 7-Zip 26 (7z a -t7z -m0=LZMA2 -ms=on): first.bin and
 * clone\second.bin, seven_content () of each, in one LZMA2 block, the
 * headers LZMA-packed */
static const unsigned char seven_bytes[] = {
	0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x83, 0xfb, 0x1f, 0xd9,
	0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x6b, 0x60, 0xab, 0x69, 0xe0, 0x05, 0x77, 0x00,
	0x40, 0x5d, 0x00, 0x39, 0x99, 0x48, 0x91, 0xb1, 0x69, 0x96, 0x61, 0x36,
	0xa8, 0xde, 0x94, 0xf1, 0x58, 0xdd, 0x2a, 0x0a, 0x1f, 0x90, 0xc5, 0x2e,
	0x5e, 0xef, 0x69, 0xda, 0x0e, 0x21, 0x33, 0xf6, 0x9c, 0x7e, 0xd1, 0x41,
	0x6d, 0x31, 0x07, 0xf0, 0x63, 0x6e, 0xb2, 0x1e, 0x09, 0x6f, 0x14, 0x9d,
	0x1e, 0x87, 0x96, 0xe9, 0xd9, 0x2e, 0x89, 0xa4, 0xce, 0x2b, 0x51, 0x27,
	0xa0, 0x73, 0xc4, 0x01, 0x7e, 0x64, 0x00, 0x00, 0x00, 0x00, 0x81, 0x33,
	0x07, 0xae, 0x0f, 0xd2, 0x5a, 0x35, 0xfd, 0x40, 0xc0, 0x90, 0xd2, 0xff,
	0x7d, 0x69, 0x4d, 0x8f, 0xbe, 0x46, 0x9c, 0x66, 0xb0, 0xb7, 0xbd, 0xd9,
	0x80, 0x81, 0xbc, 0xe8, 0x1d, 0xe3, 0x76, 0x80, 0x81, 0xaf, 0x3c, 0x51,
	0x0c, 0x38, 0x5b, 0xec, 0x10, 0x3a, 0x95, 0x57, 0x5f, 0x14, 0x97, 0xc6,
	0x5d, 0xda, 0xb5, 0xef, 0x59, 0x82, 0x40, 0x65, 0x95, 0x08, 0xb2, 0x4a,
	0xbf, 0x27, 0x51, 0x37, 0x21, 0xc4, 0x86, 0xaa, 0xc1, 0xeb, 0x7a, 0xa3,
	0x0e, 0xf8, 0x7d, 0xfc, 0xd3, 0x4f, 0xb9, 0x46, 0xe3, 0xeb, 0xd6, 0x3c,
	0x4f, 0x3b, 0x39, 0xb2, 0x10, 0x61, 0x2b, 0x13, 0x0b, 0x31, 0x31, 0xad,
	0x5a, 0xa9, 0x76, 0x18, 0xf5, 0x53, 0xd6, 0xda, 0x63, 0x24, 0x8b, 0x81,
	0x21, 0xa3, 0x43, 0x54, 0x6d, 0xa4, 0x17, 0x06, 0x48, 0x01, 0x09, 0x76,
	0x00, 0x07, 0x0b, 0x01, 0x00, 0x01, 0x23, 0x03, 0x01, 0x01, 0x05, 0x5d,
	0x00, 0x10, 0x00, 0x00, 0x0c, 0x80, 0x8e, 0x0a, 0x01, 0x76, 0x92, 0xd7,
	0x0d, 0x00, 0x00,
};

/* What the 7z's files hold: their name's sentence, repeated */
static std::string
seven_content (const char *name)
{
	std::string out;
	bool first = !strcmp (name, "first.bin");
	int i;

	for (i = 0; i < (first ? 20 : 30); i++)
		out += first ? "first.bin, packed by 7-Zip. " : "second.bin, a clone's file. ";
	return out;
}

static void
write_bytes (const std::string &path, const unsigned char *bytes, size_t size)
{
	std::ofstream out (path, std::ios::binary);
	out.write ((const char *) bytes, (std::streamsize) size);
}

static bool
loads (hl::RomSet &set, const char *name, uint32_t crc, const std::string &content)
{
	hl::Buffer *buffer = set.load (name, NULL, crc, (unsigned) content.size ());
	bool ok = buffer && !memcmp (buffer->ptr (0), content.data (), content.size ());

	hl::destroy (buffer);
	return ok;
}

/* The sets in 7z: listed by Archive, found by CRC and by name, read from
 * their solid block; a zip and a 7z of a set together; a damaged 7z */
static void
test_7z (const std::string &dir)
{
	std::string first = seven_content ("first.bin"), second = seven_content ("second.bin");

	write_bytes (dir + "/sevens.7z", seven_bytes, sizeof (seven_bytes));
	{
		hl::Archive *archive = hl::Archive::open (dir + "/sevens.7z");
		check ("7z: opened", archive != NULL);
		if (archive) {
			const auto &m = archive->members ();
			check ("7z: two files", m.size () == 2);
			for (const auto &member : m) {
				bool is_first = member.name == "first.bin";
				const std::string &content = is_first ? first : second;
				check ("7z: a file's name", is_first || member.name == "clone\\second.bin" ||
				                            member.name == "clone/second.bin");
				check ("7z: a file's size", member.size == content.size ());
				check ("7z: a file's CRC", member.has_crc && member.crc == crc_of (content));
			}
			delete archive;
		}
		check ("7z: none of another name", hl::Archive::open (dir + "/sevens.zip") == NULL);
	}
	{
		const char *names[1] = { "sevens" };
		hl::RomSet set (dir.c_str (), names, 1);

		check ("7z: by CRC", has (set, "renamed.bin", first));
		check ("7z: by name, a clone's sub-folder", set.has ("second.bin", NULL, 0, (unsigned) second.size ()));
		check ("7z: not another size", !set.has ("second.bin", NULL, 0, 10));
		check ("7z: first loaded", loads (set, "first.bin", crc_of (first), first));
		check ("7z: second loaded from the same block", loads (set, "second.bin", crc_of (second), second));
		check ("7z: first loaded again", loads (set, "x", crc_of (first), first));
	}

	/* A zip and a 7z of the same set: the files of both */
	write_bytes (dir + "/both.7z", seven_bytes, sizeof (seven_bytes));
	make_zip (dir + "/both.zip", { "zipped.bin" });
	{
		const char *names[1] = { "both" };
		hl::RomSet set (dir.c_str (), names, 1);
		check ("zip and 7z: the zip's file", has (set, "zipped.bin", "zipped.bin"));
		check ("zip and 7z: the 7z's file", has (set, "first.bin", first));
		check ("zip and 7z: the 7z's file loaded", loads (set, "first.bin", crc_of (first), first));
	}

	/* Cut short: no file, and no crash */
	write_bytes (dir + "/broken.7z", seven_bytes, sizeof (seven_bytes) - 40);
	{
		const char *names[1] = { "broken" };
		hl::RomSet set (dir.c_str (), names, 1);
		check ("a damaged 7z: nothing found", !has (set, "first.bin", first));
	}
}

static void
test_split (void)
{
	std::vector<std::string> f;

	f = hl::RomSet::split_folders ("D:\\roms");
	check ("one folder", f.size () == 1 && f[0] == "D:\\roms");
	f = hl::RomSet::split_folders ("  D:\\roms ; ;E:\\mame roms;  ");
	check ("several, spaces and empties", f.size () == 2 && f[0] == "D:\\roms" && f[1] == "E:\\mame roms");
	f = hl::RomSet::split_folders ("");
	check ("none", f.empty ());
	f = hl::RomSet::split_folders (NULL);
	check ("NULL", f.empty ());
}

int
main (int argc, char **argv)
{
	std::filesystem::path dir = std::filesystem::absolute ("romsettest.dir");
	std::string a = dir.generic_string () + "/a", b = dir.generic_string () + "/b";
	std::error_code error;

	(void) argc;
	(void) argv;
	std::filesystem::remove_all (dir, error);
	std::filesystem::create_directories (a, error);
	std::filesystem::create_directories (b, error);

	test_split ();

	/* a: the game's zip, without its second file; b: the game's zip again,
	 * with that file, and the BIOS's */
	make_zip (a + "/game.zip", { "first.bin" });
	make_zip (b + "/game.zip", { "second.bin" });
	make_zip (b + "/bios.zip", { "bios.bin" });

	{
		const char *names[3] = { "game", "", "bios" };
		hl::RomSet both ((a + ";" + b).c_str (), names, 3);
		hl::RomSet only_a (a.c_str (), names, 3);
		hl::RomSet missing ((dir.generic_string () + "/none;" + b).c_str (), names, 3);

		check ("a;b: the game's file in a", has (both, "first.bin", "first.bin"));
		check ("a;b: the game's file in b", has (both, "second.bin", "second.bin"));
		check ("a;b: the BIOS in b", has (both, "bios.bin", "bios.bin"));
		check ("a: not b's files", !has (only_a, "second.bin", "second.bin") && !has (only_a, "bios.bin", "bios.bin"));
		check ("a missing folder skipped", has (missing, "bios.bin", "bios.bin"));

		hl::Buffer *buffer = both.load ("second.bin", NULL, crc_of ("second.bin"), 10);
		check ("a;b: b's file loaded", buffer && !memcmp (buffer->ptr (0), "second.bin", 10));
		hl::destroy (buffer);
	}

	/* The same file name in both, different data: by name (no CRC), the
	 * first folder's; by CRC, the one that matches */
	make_zip (a + "/same.zip", { "x.bin" });
	make_zip (b + "/same.zip", { "x.bin" }, "X.BIN");
	{
		const char *names[1] = { "same" };
		hl::RomSet ab ((a + ";" + b).c_str (), names, 1), ba ((b + ";" + a).c_str (), names, 1);
		hl::Buffer *buffer = ab.load ("x.bin", NULL, 0, 5);
		check ("a;b: a's file first", buffer && !memcmp (buffer->ptr (0), "x.bin", 5));
		hl::destroy (buffer);
		buffer = ba.load ("x.bin", NULL, 0, 5);
		check ("b;a: b's file first", buffer && !memcmp (buffer->ptr (0), "X.BIN", 5));
		hl::destroy (buffer);
		buffer = ab.load ("x.bin", NULL, crc_of ("X.BIN"), 5);
		check ("a;b: b's file by its CRC", buffer && !memcmp (buffer->ptr (0), "X.BIN", 5));
		hl::destroy (buffer);
	}

	test_7z (a);

	std::filesystem::remove_all (dir, error);
	printf ("romsettest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
