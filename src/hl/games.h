/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/games.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_GAMES_H__
#define __HL_GAMES_H__

#include <cstdint>
#include <string>
#include <vector>

#include "hl/buffer.h"

namespace hl {

/* A game of data/games.json: its MAME set, and the sections of its ROMs */
struct GameEntry {
	char name[32];
	char mach[32];
	char parent[32];	/* MAME parent set, empty for a parent */
	char bios[32];		/* MAME BIOS set */
	char description[128];
	char year[8];
	char manufacturer[64];
	/* The cabinet: linked to others through the communication board (its
	 * 68000 not run, the board emulated from what it gives the games:
	 * src/mach/hikaru/hikaru-comm.cpp, the ring over TCP); force feedback
	 * from a drive board (SGNASCAR's: its Z80 running Sega Rally 2's EPROM,
	 * src/hl/driveboard-z80.cpp, or without it simulated,
	 * src/hl/driveboard.cpp). From the games' test menus */
	bool link;
	bool ffb;
	/* The cabinet's speakers, as the game's sound boards feed them:
	 * "stereo" (the main board's), "stereo_mix" (both boards' into one
	 * pair), "quad" (the main board's at the front, the option board's
	 * at the rear); its native sound, which the player's sound card then
	 * gets mixed for (src/hl/audio.h) */
	char audio[16];
	json_t *root;
};

/* The game list, data/games.json */
class GameList {
public:
	/* NULL when the file cannot be read */
	static GameList	*load (const char *path);
	~GameList ();

	GameEntry	*find (const char *name);

	/* Counts the ROM files of a game found under path, without loading
	 * them: a game can run when found == total. */
	void		 check (unsigned index, const char *path,
			        unsigned *found, unsigned *total);

	/* The games, for -L: title, set, version, year, maker, the MAME
	 * sets, the ROM files and their CRC32s, the main ROM's, how many of
	 * them are under path */
	void		 print (FILE *fp, const char *path, const char *find = NULL);
	/* --find: those whose set, parent, title or maker hold each word of
	 * find, whatever the case, one a line */
	void		 print_found (FILE *fp, const char *find);
	static bool	 matches (const GameEntry &entry, const char *find);

	std::vector<GameEntry> entries;
	json_t		*root;
};

/* A section of a game's ROMs: the BIOS, the EPROMs, the mask ROMs */
struct GameSection {
	char name[32];
	Buffer *buffer;
	/* Of an "alternative" section: the file it holds, in the list */
	int chosen = -1;
};

/* A game, loaded from its ROM set */
class Game {
public:
	/* NULL when the game is not in the list, or its files cannot be
	 * loaded */
	static Game	*create (GameList *list, const char *path, const char *name,
	                         const char *archive = NULL);
	~Game ();

	Buffer		*section_data (const char *name);
	/* An "alternative" section of files with a "region" (J, U, E, K, A: bits
	 * 0 to 4 of mask): the first present of a region of mask, in place of
	 * one of another region. True when it changed */
	bool		 use_region (const char *section, unsigned mask);

	char name[32];
	char mach[32];
	char parent[32];
	/* GameEntry's */
	char audio[16];
	/* The NAOMI's: its cartridge board ("rom", "m1", "m2", "m4"), that
	 * board's key and id (MAME's parameters), a NAOMI 2 */
	char board[8] = {};
	uint32_t key = 0;
	uint32_t id = 0;
	bool naomi2 = false;
	/* A NAOMI game's GD-ROM: its CHD's name */
	char gdrom[32] = "";
	/* Its DIMM board is a Net DIMM (games.json "net_dimm") */
	bool net_dimm = false;
	/* A motion cabinet whose PC the Net DIMM talks to (games.json "motion_pc") */
	bool motion_pc = false;
	/* The JVS I/O boards of its cabinet (games.json "io_boards"), the one
	 * nearest the main board first, by their Sega numbers; none listed: the
	 * machine's usual board */
	std::vector<std::string> io_boards;
	/* The board on the G2 bus's outside (games.json "g2_board"), by its
	 * Sega number; "": none */
	char g2_board[16] = "";
	/* The boards of its cabinet, linked (multiboard); 0: one */
	unsigned boards = 0;
	/* Complete systems on a ring of communication boards in one cabinet
	 * (Driving Simulator: three), each with its cartridge; 0: none */
	unsigned network_boards = 0;
	/* Those with a monitor, numbered from 1, left to right (F355 Twin: its
	 * slave alone); none listed: all */
	unsigned screens[4] = { 0, 0, 0, 0 };
	unsigned num_screens = 0;
	std::vector<GameSection> sections;

private:
	GameEntry	*entry = NULL;
	std::string	 path, archive;
};

} /* namespace hl */

#endif /* __HL_GAMES_H__ */
