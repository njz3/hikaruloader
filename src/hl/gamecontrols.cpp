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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jansson.h>

#include "hl/gamecontrols.h"
#include "hl/config.h"
#include "hl/core.h"
#include "hl/games.h"
#include "hl/gearbox.h"

namespace hl {

GameControls &
GameControls::get ()
{
	static GameControls controls;
	return controls;
}

/* "p1:12", "an:0": a port and a number */
static bool
parse_at (const char *text, std::string &port, unsigned &index)
{
	const char *colon = text ? strchr (text, ':') : NULL;
	char *end;

	if (!colon || colon == text)
		return false;
	index = (unsigned) strtoul (colon + 1, &end, 10);
	if (end == colon + 1 || *end)
		return false;
	port.assign (text, colon - text);
	for (char &ch : port)
		ch = (char) tolower ((unsigned char) ch);
	return true;
}

static uint16_t
parse_hex16 (json_t *v, uint16_t def)
{
	if (json_is_integer (v))
		return (uint16_t) json_integer_value (v);
	if (json_is_string (v))
		return (uint16_t) strtoul (json_string_value (v), NULL, 16);
	return def;
}

/* A role's places from its PLACE (see the class), added to out */
static void
read_places (const char *game, const Controls::RowDef *row, json_t *at,
             std::vector<GameControls::Place> &out)
{
	if (json_is_array (at)) {
		size_t k;
		json_t *one;

		json_array_foreach (at, k, one)
			read_places (game, row, one, out);
		return;
	}
	GameControls::Place p;
	json_t *where = at;

	p.row = row;
	if (json_is_object (at)) {
		json_t *range = json_object_get (at, "range");
		const char *label = json_string_value (json_object_get (at, "label"));

		where = json_object_get (at, "at");
		if (label)
			p.label = label;
		if (json_array_size (range) == 2) {
			p.lo = parse_hex16 (json_array_get (range, 0), p.lo);
			p.hi = parse_hex16 (json_array_get (range, 1), p.hi);
		}
		if (json_is_true (json_object_get (at, "invert")))
			std::swap (p.lo, p.hi);
		if (json_is_array (where)) {
			size_t k;
			json_t *one;

			json_array_foreach (where, k, one) {
				GameControls::Place q = p;

				if (parse_at (json_string_value (one), q.port, q.index))
					out.push_back (q);
				else
					HL_PRINT ("controls: %s: %s's place not understood", game, row->key);
			}
			return;
		}
	}
	if (parse_at (json_string_value (where), p.port, p.index))
		out.push_back (p);
	else
		HL_PRINT ("controls: %s: %s's place not understood", game, row->key);
}

static const GameEntry *
parent_of (GameList *list, const GameEntry *entry)
{
	return entry->parent[0] ? list->find (entry->parent) : NULL;
}

/* A game's field, else its parent's */
static json_t *
field (GameList *list, const GameEntry *entry, const char *name)
{
	json_t *v = json_object_get (entry->root, name);
	const GameEntry *parent;

	if (!v && (parent = parent_of (list, entry)))
		v = json_object_get (parent->root, name);
	return v;
}

void
GameControls::set_game (GameList *list, const char *name)
{
	GameEntry *entry = (list && name) ? list->find (name) : NULL;
	json_t *layout_name, *layout, *controls, *gearbox, *fixed_list;
	const char *role, *layout_text;
	json_t *at;

	game_name = name ? name : "";
	all.clear ();
	gear_table.clear ();
	gear_list.clear ();
	fixed.clear ();
	Gearbox::get ().reset ();
	Gearbox::get ().hold (Gearbox::NO_GEAR);
	if (!entry)
		return;

	/* The board's usual places, then the game's over them */
	layout_name = field (list, entry, "layout");
	layout_text = json_string_value (layout_name);
	if (!layout_text)
		layout_text = !strcmp (entry->mach, "naomi") && !strcmp (json_string_value (json_object_get (entry->root, "board")) ?
		              json_string_value (json_object_get (entry->root, "board")) : "", "aw") ? "atomiswave" : "jvs";
	layout = json_object_get (json_object_get (list->root, "layouts"), layout_text);
	if (!layout)
		HL_PRINT ("controls: %s: no layout '%s'", game_name.c_str (), layout_text);
	/* The game's places take theirs from the layout's roles: a role it
	 * lists, a place it puts another role on */
	controls = field (list, entry, "controls");
	std::vector<Place> own, usual;
	json_object_foreach (controls, role, at) {
		const Controls::RowDef *row = Controls::row_def (role);

		if (!row)
			HL_PRINT ("controls: %s: no role '%s'", game_name.c_str (), role);
		else if (!json_is_null (at))
			read_places (game_name.c_str (), row, at, own);
	}
	json_object_foreach (layout, role, at) {
		const Controls::RowDef *row = Controls::row_def (role);

		if (!row)
			HL_PRINT ("controls: layout %s: no role '%s'", layout_text, role);
		else if (!json_object_get (controls, role))
			read_places (game_name.c_str (), row, at, usual);
	}
	for (const Place &p : usual)
		if (std::none_of (own.begin (), own.end (), [&] (const Place &o) { return o.port == p.port && o.index == p.index; }))
			all.push_back (p);
	all.insert (all.end (), own.begin (), own.end ());

	/* The gear lever's positions, in their order */
	gearbox = field (list, entry, "gearbox");
	json_object_foreach (gearbox, role, at) {
		Gear g;
		size_t k;
		json_t *one;

		g.gear = !strcmp (role, "R") ? -1 : !strcmp (role, "N") ? 0 : atoi (role);
		json_array_foreach (at, k, one) {
			std::pair<std::string, unsigned> p;

			if (parse_at (json_string_value (one), p.first, p.second))
				g.closed.push_back (p);
		}
		gear_table.push_back (g);
	}
	std::sort (gear_table.begin (), gear_table.end (), [] (const Gear &a, const Gear &b) { return a.gear < b.gear; });
	for (const Gear &g : gear_table)
		gear_list.push_back (g.gear);

	fixed_list = field (list, entry, "fixed");
	{
		size_t k;
		json_t *one;

		json_array_foreach (fixed_list, k, one) {
			std::pair<std::string, unsigned> p;

			if (parse_at (json_string_value (one), p.first, p.second))
				fixed.push_back (p);
		}
	}
	HL_PRINT ("controls: %s: layout %s, %u places%s", game_name.c_str (), layout_text,
	          (unsigned) all.size (), gear_list.empty () ? "" : ", a gear lever");
}

bool
GameControls::has (const char *role) const
{
	for (const Place &p : all)
		if (!strcasecmp (p.row->key, role))
			return true;
	if (!gear_list.empty ())
		for (const char *r : { "ShiftUp", "ShiftDown", "GearReverse", "GearNeutral", "Gear1", "Gear2", "Gear3", "Gear4" })
			if (!strcasecmp (r, role))
				return true;
	return false;
}

/* Star Wars: Racer Arcade's levers, 0 (at rest) to 1 (pushed all the way):
 * an axis each, or (TWIN_LEVERS = COMBINED) the accelerator the thrust of
 * both and the wheel the difference around it, the engine on the side away
 * from the turn pushing harder (the pod turns right with the left engine's
 * thrust alone, measured) */
static double
twin_lever (bool right)
{
	Controls &c = Controls::get ();

	if (Config::get ()->twin_levers == TWIN_LEVERS_ORIGINAL)
		return c.pedal ("Driving", right ? "RightLever" : "LeftLever");

	double thrust = c.pedal ("Driving", "Accelerator"), steer = c.axis ("Driving", "Steering");
	return std::clamp (thrust * (right ? 1.0 - steer : 1.0 + steer), 0.0, 1.0);
}

double
GameControls::value (const char *role)
{
	const Controls::RowDef *row = Controls::row_def (role);
	Controls &c = Controls::get ();

	if (!row)
		return 0.0;
	if (!strcmp (row->key, "LeftLever") || !strcmp (row->key, "RightLever"))
		return twin_lever (row->key[0] == 'R');
	switch (row->kind) {
	case Controls::ROW_AXIS: {
		double v = c.axis (row->section, row->key);

		/* INVERT_STICK_Y: a stick's up and down the other way */
		if (Config::get ()->invert_stick_y && strstr (row->key, "StickY"))
			v = -v;
		return v;
	}
	case Controls::ROW_LEVER:
		return c.aim (row->section, row->key);
	case Controls::ROW_PEDAL:
		return c.pedal (row->section, row->key);
	default:
		return c.pressed (row->section, row->key) ? 1.0 : 0.0;
	}
}

bool
GameControls::pressed (const char *role)
{
	const Controls::RowDef *row = Controls::row_def (role);

	if (!row)
		return false;
	if (row->kind == Controls::ROW_SWITCH)
		return Controls::get ().pressed (row->section, row->key);
	return value (role) > 0.5;
}

uint32_t
GameControls::switches (const char *port, uint64_t frame)
{
	uint32_t bits = 0;

	for (const Place &p : all)
		if (p.index < 32 && p.port == port && pressed (p.row->key))
			bits |= 1u << p.index;
	if (!gear_list.empty ()) {
		int g = Gearbox::get ().gear (frame, gear_list);

		for (const Gear &gear : gear_table)
			if (gear.gear == g)
				for (const auto &c : gear.closed)
					if (c.second < 32 && c.first == port)
						bits |= 1u << c.second;
	}
	for (const auto &f : fixed)
		if (f.second < 32 && f.first == port)
			bits |= 1u << f.second;
	return bits;
}

bool
GameControls::place_value (const Place &p, uint16_t *out)
{
	double v = value (p.row->key), t;

	switch (p.row->kind) {
	case Controls::ROW_AXIS:
	case Controls::ROW_LEVER:
		t = (v + 1.0) / 2.0;
		break;
	default:
		t = v;
		break;
	}
	t = std::clamp (t, 0.0, 1.0);
	*out = (uint16_t) lround (p.lo + ((double) p.hi - p.lo) * t);
	return true;
}

bool
GameControls::analog (const char *port, unsigned index, uint16_t *out)
{
	for (const Place &p : all)
		if (p.index == index && p.port == port)
			return place_value (p, out);
	return false;
}

} /* namespace hl */
