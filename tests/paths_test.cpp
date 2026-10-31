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

/* The write directory's tests (hl::Paths): its option, the paths without and
 * with it, the overlay of the files read. Run by ctest, or alone: pathstest. */

#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <string>

#include "hl/paths.h"

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

static void
check_str (const char *name, const std::string &got, const std::string &want)
{
	checks++;
	if (got != want) {
		failures++;
		fprintf (stderr, "FAIL %s: '%s', expected '%s'\n", name, got.c_str (), want.c_str ());
	}
}

static void
touch (const std::string &path)
{
	std::error_code error;
	FILE *fp;

	std::filesystem::create_directories (std::filesystem::path (path).parent_path (), error);
	fp = fopen (path.c_str (), "wb");
	if (fp)
		fclose (fp);
}

static void
test_option (void)
{
	char a0[] = "hikaruloader", a1[] = "--write-dir=C:/wd", a2[] = "-write-dir", a3[] = "rel/wd",
	     a4[] = "--write-dir", a5[] = "-write-dirx", a6[] = "-r";
	char *argv[] = { a0, a1, a2, a3, a4, a5, a6 };
	std::string dir;

	check ("not the option", hl::Paths::write_dir_option (7, argv, 0, &dir) == 0);
	check ("--write-dir=", hl::Paths::write_dir_option (7, argv, 1, &dir) == 1);
	check_str ("--write-dir='s path", dir, "C:/wd");
	check ("-write-dir <path>", hl::Paths::write_dir_option (7, argv, 2, &dir) == 2);
	check_str ("-write-dir <path>'s path", dir, "rel/wd");
	check ("--write-dir as the last word", hl::Paths::write_dir_option (5, argv, 4, &dir) == 0);
	check ("-write-dirx", hl::Paths::write_dir_option (7, argv, 5, &dir) == 0);
	check ("-r", hl::Paths::write_dir_option (7, argv, 6, NULL) == 0);
}

static void
test_paths (const std::string &base)
{
	std::string wd = base + "wd/";

	/* None: as always */
	check ("none", hl::Paths::set_write_dir (""));
	check ("none, has", !hl::Paths::has_write_dir ());
	check_str ("none, config write", hl::Paths::config_write ("hikaruloader.ini"), base + "hikaruloader.ini");
	check_str ("none, config read", hl::Paths::config_read ("hikaruloader.ini"), base + "hikaruloader.ini");
	check_str ("none, game write", hl::Paths::game_write ("sgnascar", "nvram/sgnascar.nvram"), "nvram/sgnascar.nvram");
	check_str ("none, game read", hl::Paths::game_read ("sgnascar", "sgnascar.hlstate"), "sgnascar.hlstate");

	/* Relative: to the program's folder, made */
	check ("set", hl::Paths::set_write_dir ("wd"));
	check ("has", hl::Paths::has_write_dir ());
	check_str ("its path", hl::Paths::write_dir (), wd);
	check ("made", std::filesystem::is_directory (wd));
	check_str ("config write", hl::Paths::config_write ("controls.ini"), wd + "controls.ini");

	/* The overlay: the write directory's when there, else the usual */
	touch (base + "controls.ini");
	check_str ("config read, the usual", hl::Paths::config_read ("controls.ini"), base + "controls.ini");
	touch (wd + "controls.ini");
	check_str ("config read, the write dir's", hl::Paths::config_read ("controls.ini"), wd + "controls.ini");

	check_str ("game write", hl::Paths::game_write ("sgnascar", "nvram/sgnascar.nvram"),
	           wd + "sgnascar/nvram/sgnascar.nvram");
	check ("game write's folders made", std::filesystem::is_directory (wd + "sgnascar/nvram"));
	check_str ("game read, the usual", hl::Paths::game_read ("sgnascar", "nvram/sgnascar.nvram"),
	           "nvram/sgnascar.nvram");
	touch (wd + "sgnascar/nvram/sgnascar.nvram");
	check_str ("game read, the write dir's", hl::Paths::game_read ("sgnascar", "nvram/sgnascar.nvram"),
	           wd + "sgnascar/nvram/sgnascar.nvram");
	check_str ("a set's odd characters", hl::Paths::game_write ("a b:c", "x"), wd + "a_b_c/x");

	/* The settings: copied from beside the program when the write
	 * directory lacks them, never replaced */
	{
		FILE *fp = fopen ((base + "hikaruloader.ini").c_str (), "wb");
		fputs ("usual", fp);
		fclose (fp);
	}
	check_str ("config file", hl::Paths::config_file ("hikaruloader.ini"), wd + "hikaruloader.ini");
	check ("config file copied", std::filesystem::file_size (wd + "hikaruloader.ini") == 5);
	std::filesystem::resize_file (wd + "hikaruloader.ini", 2);
	hl::Paths::config_file ("hikaruloader.ini");
	check ("config file kept", std::filesystem::file_size (wd + "hikaruloader.ini") == 2);
	check_str ("config file, none anywhere", hl::Paths::config_file ("none.ini"), wd + "none.ini");
	check ("config file, none made", !std::filesystem::exists (wd + "none.ini"));

	/* A game's: seed copies into its folder, made, only when missing */
	touch (base + "usual.nvram");
	check ("seed", hl::Paths::seed (base + "usual.nvram", wd + "g/nvram/g.nvram"));
	check ("seeded", std::filesystem::exists (wd + "g/nvram/g.nvram"));
	check ("seed, there already", !hl::Paths::seed (base + "usual.nvram", wd + "g/nvram/g.nvram"));
	check ("seed, no source", !hl::Paths::seed (base + "missing.nvram", wd + "h/h.nvram"));
	check ("seed, no source, nothing made", !std::filesystem::exists (wd + "h/h.nvram"));

	/* One that cannot be made (under a file): rejected, the writes still
	 * aimed at it (failing), never beside the program; the usual files
	 * read (the owner's rule for all the emulators) */
	touch (base + "afile");
	check ("rejected", !hl::Paths::set_write_dir (base + "afile/wd"));
	check ("rejected, still there", hl::Paths::has_write_dir ());
	check_str ("rejected, for the notice", hl::Paths::unusable_write_dir (), base + "afile/wd/");
	check_str ("rejected, why", hl::Paths::write_dir_error (), "it cannot be made");
	check_str ("rejected, config file", hl::Paths::config_file ("hikaruloader.ini"),
	           base + "afile/wd/hikaruloader.ini");
	check_str ("rejected, config read", hl::Paths::config_read ("hikaruloader.ini"),
	           base + "hikaruloader.ini");
	check_str ("rejected, game write", hl::Paths::game_write ("g", "nvram/g.nvram"),
	           base + "afile/wd/g/nvram/g.nvram");
	check_str ("rejected, game read", hl::Paths::game_read ("g", "nvram/g.nvram"), "nvram/g.nvram");
	check ("rejected, no seed", !hl::Paths::seed (base + "hikaruloader.ini", base + "afile/wd/x.ini"));
	check ("a usable one again", hl::Paths::set_write_dir ("wd") && hl::Paths::unusable_write_dir ().empty ()
	       && hl::Paths::write_dir_error ().empty ());
	check ("no test file left", !std::filesystem::exists (wd + "hikaruloader-write-test.tmp"));

	/* Absolute: as given */
	check ("absolute", hl::Paths::set_write_dir (base + "abs"));
	check_str ("absolute's path", hl::Paths::write_dir (), base + "abs/");
	hl::Paths::set_write_dir ("");
}

int
main (int argc, char **argv)
{
	std::filesystem::path dir = std::filesystem::absolute ("pathstest.dir");
	std::string base = dir.generic_string () + "/";
	std::error_code error;

	(void) argc;
	(void) argv;
	std::filesystem::remove_all (dir, error);
	std::filesystem::create_directories (dir, error);
	hl::Paths::set_base (base.c_str ());
	check_str ("base", hl::Paths::base (), base);

	test_option ();
	test_paths (base);

	std::filesystem::remove_all (dir, error);
	printf ("pathstest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
