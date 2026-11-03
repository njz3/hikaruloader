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

#include "hl/gearbox.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/input.h"

namespace hl {

/* The machine's frames a second (the Hikaru's 31 kHz screen) */
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
	current = current_out = crossing = 0;
	up_held = down_held = false;
	last_frame = ~0ull;
}

unsigned
Gearbox::gear (uint64_t frame)
{
	static const Action positions[4] = { ACTION_GEAR1, ACTION_GEAR2, ACTION_GEAR3, ACTION_GEAR4 };
	const Config *cfg = Config::get ();
	bool h_shifter = false, held = false;
	bool up, down;
	unsigned g, from;

	if (frame == last_frame)
		return current_out;
	last_frame = frame;

	/* An H shifter's position held: that gear (neutral too) */
	for (g = 0; g < 4; g++) {
		const char *section, *key;

		if (Input::row_of (positions[g], &section, &key) && !Controls::get ().value (section, key).empty ())
			h_shifter = true;
		if (Input::action (positions[g])) {
			current = g + 1;
			held = true;
		}
	}
	if (Input::action (ACTION_GEAR_NEUTRAL)) {
		current = 0;
		held = true;
	}

	/* The paddles: a gear each press, through neutral between two */
	up = Input::action (ACTION_GEAR_UP);
	down = Input::action (ACTION_GEAR_DOWN);
	from = current;
	if (!held) {
		if (up && !up_held && current < 4)
			current++;
		if (down && !down_held && current > 0)
			current--;
		if (current != from && from && current)
			crossing = (unsigned) (cfg->shift_neutral_ms * FRAME_HZ / 1000.0 + 0.999);
	}
	up_held = up;
	down_held = down;

	if (held) {
		crossing = 0;
		current_out = current;
	} else if (h_shifter && cfg->shifter_neutral) {
		/* The lever in none of its gears */
		current = current_out = crossing = 0;
	} else
		current_out = crossing ? 0 : current;
	if (crossing)
		crossing--;
	if (agent_gear >= 0)
		current_out = (unsigned) agent_gear;
	return current_out;
}

} /* namespace hl */
