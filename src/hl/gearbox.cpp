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
	last_frame = ~0ull;
}

int
Gearbox::gear (uint64_t frame, const std::vector<int> &gears)
{
	static const struct { const char *key; int gear; } positions[] = {
		{ "GearReverse", -1 }, { "GearNeutral", 0 }, { "Gear1", 1 },
		{ "Gear2", 2 }, { "Gear3", 3 }, { "Gear4", 4 },
	};
	const Config *cfg = Config::get ();
	Controls &c = Controls::get ();
	bool h_shifter = false, held = false, neutral;
	bool up, down;
	int from;

	if (gears.empty ())
		return 0;
	if (frame == last_frame)
		return current_out;
	last_frame = frame;
	neutral = std::find (gears.begin (), gears.end (), 0) != gears.end ();
	if (std::find (gears.begin (), gears.end (), current) == gears.end ())
		current = neutral ? 0 : gears.front () == -1 && gears.size () > 1 ? gears[1] : gears.front ();

	/* An H shifter's position held: that gear */
	for (const auto &p : positions) {
		if (std::find (gears.begin (), gears.end (), p.gear) == gears.end ())
			continue;
		if (p.gear && !c.value ("Driving", p.key).empty ())
			h_shifter = true;
		if (c.pressed ("Driving", p.key)) {
			current = p.gear;
			held = true;
		}
	}

	/* The paddles: a position each press, through neutral between two */
	up = c.pressed ("Driving", "ShiftUp");
	down = c.pressed ("Driving", "ShiftDown");
	from = current;
	if (!held) {
		size_t at = std::find (gears.begin (), gears.end (), current) - gears.begin ();

		if (up && !up_held && at + 1 < gears.size ())
			current = gears[at + 1];
		if (down && !down_held && at > 0)
			current = gears[at - 1];
		if (current != from && from && current && neutral)
			crossing = (unsigned) (cfg->shift_neutral_ms * FRAME_HZ / 1000.0 + 0.999);
	}
	up_held = up;
	down_held = down;

	if (held) {
		crossing = 0;
		current_out = current;
	} else if (h_shifter && neutral && cfg->shifter_neutral) {
		/* The lever in none of its gears */
		current = current_out = 0;
		crossing = 0;
	} else
		current_out = crossing ? 0 : current;
	if (crossing)
		crossing--;
	if (agent_gear != NO_GEAR && std::find (gears.begin (), gears.end (), agent_gear) != gears.end ())
		current_out = agent_gear;
	return current_out;
}

} /* namespace hl */
