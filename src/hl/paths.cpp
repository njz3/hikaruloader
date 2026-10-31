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

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <system_error>

#include "hl/core.h"
#include "hl/paths.h"

namespace hl {

static std::string base_dir = "./";
static std::string write_root;		/* empty: none */
/* Rejected: it could not be made or written to; why */
static bool rejected = false;
static std::string rejection;

void
Paths::set_base (const char *base)
{
	base_dir = (base && base[0]) ? base : "./";
	if (base_dir.back () != '/' && base_dir.back () != '\\')
		base_dir += '/';
}

const std::string &
Paths::base ()
{
	return base_dir;
}

bool
Paths::set_write_dir (const std::string &dir)
{
	std::filesystem::path path (dir);
	std::error_code error;

	write_root.clear ();
	rejected = false;
	rejection.clear ();
	if (dir.empty ())
		return true;
	if (path.is_relative ())
		path = std::filesystem::path (base_dir) / path;
	path = path.lexically_normal ();
	write_root = path.generic_string ();
	if (write_root.back () != '/')
		write_root += '/';

	/* Made, then written to (a read-only share, the rights), as Flycast
	 * and Supermodel check it. Else, the owner's rule for all of them: it
	 * stays where the writes go, and they fail, lost; never beside the
	 * program, whose files are the cabinet's models (two emulators started
	 * together, one failing, would break them). The usual files are read */
	std::filesystem::create_directories (path, error);
	if (!std::filesystem::is_directory (path, error)) {
		rejected = true;
		rejection = "it cannot be made";
		return false;
	}
	std::string probe = write_root + "hikaruloader-write-test.tmp";
	FILE *fp = fopen (probe.c_str (), "wb");
	bool ok = fp && fputs ("test", fp) >= 0;

	if (fp)
		ok = (fclose (fp) == 0) && ok;
	ok = std::filesystem::remove (probe, error) && !error && ok;
	if (!ok) {
		rejected = true;
		rejection = "a file cannot be written in it";
	}
	return ok;
}

bool
Paths::has_write_dir ()
{
	return !write_root.empty ();
}

const std::string &
Paths::write_dir ()
{
	return write_root;
}

const std::string &
Paths::unusable_write_dir ()
{
	static const std::string none;
	return rejected ? write_root : none;
}

const std::string &
Paths::write_dir_error ()
{
	return rejection;
}

static bool
exists (const std::string &path)
{
	std::error_code error;
	return std::filesystem::exists (path, error);
}

std::string
Paths::config_write (const std::string &file)
{
	return (write_root.empty () ? base_dir : write_root) + file;
}

std::string
Paths::config_read (const std::string &file)
{
	if (!write_root.empty () && !rejected && exists (write_root + file))
		return write_root + file;
	return base_dir + file;
}

bool
Paths::seed (const std::string &from, const std::string &to)
{
	std::filesystem::path target (to);
	std::error_code error;

	if (rejected || exists (to) || !exists (from))
		return false;
	if (target.has_parent_path ())
		std::filesystem::create_directories (target.parent_path (), error);
	return std::filesystem::copy_file (from, to, error) && !error;
}

std::string
Paths::config_file (const std::string &file)
{
	std::string path = config_write (file);

	if (!write_root.empty () && seed (base_dir + file, path))
		HL_PRINT ("write directory: '%s' copied from '%s'", path.c_str (), (base_dir + file).c_str ());
	return path;
}

/* Named after the set, as Flycast's: its characters but letters, digits,
 * '-', '_' and '.' as '_' */
static std::string
game_dir (const char *game)
{
	std::string name = (game && game[0]) ? game : "unknown";

	for (char &c : name)
		if (!isalnum ((unsigned char) c) && c != '-' && c != '_' && c != '.')
			c = '_';
	return write_root + name + "/";
}

std::string
Paths::game_write (const char *game, const std::string &file)
{
	std::filesystem::path path;
	std::error_code error;

	if (write_root.empty ())
		return file;
	/* Rejected, nothing made in it */
	path = game_dir (game) + file;
	if (!rejected)
		std::filesystem::create_directories (path.parent_path (), error);
	return path.generic_string ();
}

std::string
Paths::game_read (const char *game, const std::string &file)
{
	if (!write_root.empty () && !rejected && exists (game_dir (game) + file))
		return game_dir (game) + file;
	return file;
}

int
Paths::write_dir_option (int argc, char **argv, int i, std::string *dir)
{
	const char *arg = argv[i];

	if (arg[0] == '-' && arg[1] == '-')
		arg++;
	if (strncmp (arg, "-write-dir", 10))
		return 0;
	if (arg[10] == '=') {
		if (dir)
			*dir = arg + 11;
		return 1;
	}
	if (arg[10] == '\0' && i + 1 < argc) {
		if (dir)
			*dir = argv[i + 1];
		return 2;
	}
	return 0;
}

} /* namespace hl */
