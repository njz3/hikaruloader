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

#ifndef __HL_GAMECONTROLS_H__
#define __HL_GAMECONTROLS_H__

#include <cstdint>
#include <string>
#include <vector>

#include "hl/controls.h"

namespace hl {

class GameList;

/* The running game's controls, the same way on every machine: each a role
 * (a row of controls.ini: P1_Button1, Steering, ViewChange, P1_GunX...)
 * placed on its board's inputs by data/games.json.
 *
 *	"layouts" : { "<name>" : { "<role>" : PLACE, ... }, ... }
 *		a board's usual places (jvs, atomiswave, gaelco_...);
 *	a game's "layout" : "<name>" (by default its machine's: jvs for the
 *		Hikaru and the NAOMI, atomiswave), "controls" : { "<role>" :
 *		PLACE, ... } over it (null: not there; a place it lists is
 *		taken from the layout's role), its parent's when it has none;
 *	PLACE : "<port>:<n>", or { "at" : "<port>:<n>" (or a list of
 *		them), "label" : its INPUT TEST's name, "invert" : true,
 *		"range" : [ "<at rest or left>", "<pressed or right>" ] (16-bit
 *		hex), "combined" : { "thrust" : "<role>", "turn" : "<role>",
 *		"side" : "left" or "right" } }, or a list of places;
 *	"gearbox" : { "<gear>" : [ "<port>:<n>", ... ], ... }: a gear lever
 *		held in a position, each gear ("R", "N", "1" to "6") the
 *		switches it closes;
 *	"fixed" : [ "<port>:<n>", ... ]: switches always closed.
 *
 * A port is a board's: switches by bit ("sys", "p1", "p2" of a JVS I/O
 * board, bit 15 START; the Atomiswave's pads "p1" to "p4"; Gaelco's port
 * selects "7000", "e800"...), the coin counters ("coin"), the analog
 * channels by number ("an"; "a1" to "a4" an Atomiswave pad's). A switch
 * with an analog role is closed past half its travel; several roles on a
 * place add up. A "combined" place is one of a pair of levers (Star Wars:
 * Racer Arcade's thrust levers): with TWIN_LEVERS = COMBINED, its role's
 * own row is not read but the thrust of both levers and the turn their
 * difference, the lever on the side away from the turn pushing harder. */
class GameControls {
public:
	static GameControls &get ();

	/* The game's places (NULL, or a game not in the list: none) */
	void		 set_game (GameList *list, const char *name);
	const char	*game () const { return game_name.c_str (); }

	struct Place {
		const Controls::RowDef *row;	/* its role */
		std::string	 port;
		unsigned	 index = 0;
		std::string	 label;		/* the game's name, "" none */
		uint16_t	 lo = 0x0000, hi = 0xFFFF;
		/* "combined": its pair's thrust and turn, side 1 the left
		 * lever's, -1 the right's, 0 none */
		const Controls::RowDef *thrust = NULL, *turn = NULL;
		int		 side = 0;
	};
	const std::vector<Place> &places () const { return all; }
	/* The role is on one of the game's places (or its gear lever's) */
	bool		 has (const char *role) const;
	/* On one of its places (not the gear lever's) */
	bool		 placed (const char *role) const;
	/* It has a pair of levers TWIN_LEVERS combines ("combined") */
	bool		 combined () const;

	/* A role now: a switch held, an axis -1 (left, up) to 1, a pedal or
	 * a lever 0 to 1 (a sight's -1 to 1); over the held values of the
	 * agent and the shell (Controls::hold) */
	bool		 pressed (const char *role);
	double		 value (const char *role);

	/* The port's switches closed now, bit n for n; the gear lever's
	 * looked at once a frame */
	uint32_t	 switches (const char *port, uint64_t frame);
	/* A channel now, 16 bits; false when the game has none there */
	bool		 analog (const char *port, unsigned index, uint16_t *value);

	/* The gear lever: the gears it has, by number (-1 reverse, 0
	 * neutral, 1 to 6), in their order; moved by ShiftUp and ShiftDown
	 * unless the cabinet has them on paddles of its own */
	const std::vector<int> &gears () const { return gear_list; }
	bool		 paddles () const { return !placed ("ShiftUp") && !placed ("ShiftDown"); }

private:
	GameControls () = default;

	bool		 place_value (const Place &p, uint16_t *value);
	double		 combined_value (const Place &p);
	std::string	 game_name;
	std::vector<Place> all;
	struct Gear {
		int		 gear;
		std::vector<std::pair<std::string, unsigned>> closed;
	};
	std::vector<Gear> gear_table;
	std::vector<int> gear_list;
	std::vector<std::pair<std::string, unsigned>> fixed;
};

} /* namespace hl */

#endif /* __HL_GAMECONTROLS_H__ */
