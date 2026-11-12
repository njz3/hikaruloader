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

#ifndef __HL_GEARBOX_H__
#define __HL_GEARBOX_H__

#include <cstdint>
#include <vector>

namespace hl {

/* A cabinet's gear lever, held in its positions: the gears the game's
 * "gearbox" has (-1 reverse, 0 neutral, 1 to 6: GameControls::gears), from
 * the player's controls, as SPECS.md has them ("The gear lever"). Events,
 * not levels: an H shifter's button (controls.ini's GearReverse,
 * GearNeutral, Gear1 to Gear6) pressed engages its gear, kept once let go;
 * held, it gives no other; of several, the last pressed (the highest, in
 * the same frame). Shift up and Shift down engage the next gear up or down
 * at each press, neutral not one of them, unless the cabinet has paddles of
 * its own (paddles false: F355's). Neutral only by its button, or for
 * SHIFT_NEUTRAL_MS from a gear to another. At the game's start, the gear of
 * a button already held (the highest), else neutral, else the first
 * forward gear. Looked at once a frame of the machine's, as the game polls
 * its switches. */
class Gearbox {
public:
	static Gearbox	&get ();
	/* The gear engaged in that frame, among gears (empty: 0) */
	int		 gear (uint64_t frame, const std::vector<int> &gears, bool paddles = true);
	/* The one of the last frame looked at, for the settings' page */
	int		 engaged () const { return current_out; }
	/* As a game starts: its first frame looks at the buttons held */
	void		 reset ();
	/* The agent's gear over the controls; NO_GEAR the controls' again */
	enum { NO_GEAR = -100 };
	void		 hold (int gear) { agent_gear = gear; }
	int		 held () const { return agent_gear; }

private:
	Gearbox () = default;

	int		 current = 0;		/* the gear engaged */
	int		 current_out = 0;	/* the gear the game got */
	unsigned	 crossing = 0;		/* frames left in neutral */
	bool		 up_held = false, down_held = false;
	unsigned	 positions_held = 0;	/* the H buttons held at the last frame */
	bool		 started = false;	/* a frame looked at since reset () */
	uint64_t	 last_frame = ~0ull;
	int		 agent_gear = NO_GEAR;
};

} /* namespace hl */

#endif /* __HL_GEARBOX_H__ */
