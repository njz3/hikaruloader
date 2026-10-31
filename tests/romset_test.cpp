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
 * ';', a set's zip in each that has it, the first folders first. Run by
 * ctest, or alone: romsettest. */

#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <string>
#include <vector>

#include <miniz.h>

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

	std::filesystem::remove_all (dir, error);
	printf ("romsettest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
