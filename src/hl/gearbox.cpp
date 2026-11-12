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

#include "hl/gearbox.h"
#include "hl/config.h"
#include "hl/controls.h"

namespace hl {

/* The machine's frames a second */
static const double FRAME_HZ = 60.0;

Gearbox &
Gearbox::get ()
{
	static Gearbox box;
	return box;
}

void
Gearbox::reset ()
{
	current = current_out = 0;
	crossing = 0;
	up_held = down_held = false;
	positions_held = 0;
	started = false;
	last_frame = ~0ull;
}

int
Gearbox::gear (uint64_t frame, const std::vector<int> &gears, bool paddles)
{
	static const struct { const char *key; int gear; } positions[] = {
		{ "GearReverse", -1 }, { "GearNeutral", 0 }, { "Gear1", 1 },
		{ "Gear2", 2 }, { "Gear3", 3 }, { "Gear4", 4 }, { "Gear5", 5 }, { "Gear6", 6 },
	};
	const int NONE = -100;
	const Config *cfg = Config::get ();
	Controls &c = Controls::get ();
	unsigned held_now = 0;
	int pressed = NONE, highest = NONE, from;
	bool neutral, up, down;

	if (gears.empty ())
		return 0;
	if (frame == last_frame)
		return current_out;
	last_frame = frame;
	neutral = std::find (gears.begin (), gears.end (), 0) != gears.end ();

	/* The H shifter's buttons the game has a gear for: those pressed
	 * since the last frame (the highest of them, pressed in the same
	 * frame), the highest held */
	for (unsigned i = 0; i < sizeof (positions) / sizeof (positions[0]); i++) {
		if (std::find (gears.begin (), gears.end (), positions[i].gear) == gears.end () ||
		    !c.pressed ("Driving", positions[i].key))
			continue;
		held_now |= 1u << i;
		highest = std::max (highest, positions[i].gear);
		if (!(positions_held & (1u << i)))
			pressed = std::max (pressed, positions[i].gear);
	}
	up = paddles && c.pressed ("Driving", "ShiftUp");
	down = paddles && c.pressed ("Driving", "ShiftDown");

	if (!started) {
		/* The game's start: the gear of a button already held (the
		 * highest), else neutral, else the first forward gear */
		started = true;
		if (highest != NONE)
			current = highest;
		else
			current = neutral ? 0 : gears.front () == -1 && gears.size () > 1 ? gears[1] : gears.front ();
		crossing = 0;
	} else {
		if (std::find (gears.begin (), gears.end (), current) == gears.end ())
			current = neutral ? 0 : gears.front ();
		from = current;
		if (pressed != NONE)
			/* A button pressed: its gear, kept once let go */
			current = pressed;
		else if (up && !up_held) {
			/* Shift up and down: the next gear, neutral not one */
			for (int g : gears)
				if (g > current && g) {
					current = g;
					break;
				}
		} else if (down && !down_held) {
			for (auto g = gears.rbegin (); g != gears.rend (); ++g)
				if (*g < current && *g) {
					current = *g;
					break;
				}
		}
		/* From a gear to another, through neutral SHIFT_NEUTRAL_MS */
		if (current != from && from && current && neutral)
			crossing = (unsigned) (cfg->shift_neutral_ms * FRAME_HZ / 1000.0 + 0.999);
		else if (current != from)
			crossing = 0;
	}
	positions_held = held_now;
	up_held = up;
	down_held = down;

	current_out = crossing ? 0 : current;
	if (crossing)
		crossing--;
	if (agent_gear != NO_GEAR && std::find (gears.begin (), gears.end (), agent_gear) != gears.end ())
		current_out = agent_gear;
	return current_out;
}

} /* namespace hl */
