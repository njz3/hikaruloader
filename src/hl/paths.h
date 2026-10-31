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

#ifndef __HL_PATHS_H__
#define __HL_PATHS_H__

#include <string>

namespace hl {

/* Where Hikaruloader's files are. Without a write directory, as always: the
 * settings (hikaruloader.ini, controls.ini, init.lua, data/games.json) and the
 * log beside the program; each game's nvram (nvram/<game>.nvram), states and
 * dumps in the working directory.
 *
 * With one (--write-dir=<path>, or WRITE_DIR in the hikaruloader.ini beside
 * the program, as Supermodel's -write-dir and Flycast's --write-dir), all
 * that is written goes under it, so that the rest of the disk can be read
 * only (a cabinet under Windows' Unified Write Filter, the games on a share):
 * the settings and the log at its root, each game's files in a folder named
 * after its set, <path>/<game>/ (<path>/sgnascar/nvram/sgnascar.nvram).
 *
 * As the cabinet image has it: the settings and each game's nvram are copied
 * there from their usual places when it has none (the first start, or the
 * write directory wiped), then only its copies are read and written; a file
 * there is never replaced by a usual one. The files only read (the game
 * list, init.lua, the states loaded) are taken from it first, else from
 * their usual places. One that cannot be written to is rejected: the usual
 * files read, nothing saved (set_write_dir). */
class Paths {
public:
	/* The program's folder, with its trailing separator */
	static void		 set_base (const char *base);
	static const std::string &base ();

	/* Relative, to the program's folder; made if needed. Empty: none.
	 * False when it cannot be made or written to: rejected, it stays
	 * where all is written, the writes failing, and nothing is written
	 * beside the program whatever happens (the cabinet's model files, the
	 * owner's rule for all the emulators); the usual files are read */
	static bool		 set_write_dir (const std::string &dir);
	static bool		 has_write_dir ();
	/* With its trailing '/', empty when none */
	static const std::string &write_dir ();
	/* The write directory when rejected, for the notice; else empty */
	static const std::string &unusable_write_dir ();
	/* Why it was rejected */
	static const std::string &write_dir_error ();

	/* The program's own files, file relative to its folder: where to write
	 * them; where to read them, the write directory's when there */
	static std::string	 config_write (const std::string &file);
	static std::string	 config_read (const std::string &file);
	/* A setting's file, read and written there: config_write's, copied
	 * first from beside the program when the write directory lacks it */
	static std::string	 config_file (const std::string &file);
	/* Copies from to to when to is missing and from is there, the folders
	 * made; true when copied */
	static bool		 seed (const std::string &from, const std::string &to);
	/* A game's files, file relative ("nvram/sgnascar.nvram"): where to
	 * write them, the folders made; where to read them */
	static std::string	 game_write (const char *game, const std::string &file);
	static std::string	 game_read (const char *game, const std::string &file);

	/* How many of argv's words from i make the write directory's option,
	 * Flycast's syntax (0 when none): --write-dir=<path>, --write-dir
	 * <path>, or with a single '-'. Its path into dir, when not NULL */
	static int		 write_dir_option (int argc, char **argv, int i, std::string *dir);
};

} /* namespace hl */

#endif /* __HL_PATHS_H__ */
