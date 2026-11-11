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
 * "gearbox" has (-1 reverse, 0 neutral, 1 to 4: GameControls::gears), from
 * the player's controls, as sm2-emu has them. An H shifter's positions
 * (controls.ini's GearReverse, GearNeutral, Gear1 to Gear4), a button held
 * each while the lever is there; paddles (ShiftUp, ShiftDown) a position up
 * or down each press, in the gears' order. A position held wins. With none
 * held, an H shifter's lever is in neutral when the game has one
 * (SHIFTER_NEUTRAL), else the last gear stays; a paddle's shift from a gear
 * to another goes through neutral SHIFT_NEUTRAL_MS. Looked at once a frame
 * of the machine's, as the game polls its switches. */
class Gearbox {
public:
	static Gearbox	&get ();
	/* The gear engaged in that frame, among gears (empty: 0) */
	int		 gear (uint64_t frame, const std::vector<int> &gears);
	/* The one of the last frame looked at, for the settings' page */
	int		 engaged () const { return current_out; }
	/* In neutral (or the first gear without one), as a game starts */
	void		 reset ();
	/* The agent's gear over the controls; NO_GEAR the controls' again */
	enum { NO_GEAR = -100 };
	void		 hold (int gear) { agent_gear = gear; }
	int		 held () const { return agent_gear; }

private:
	Gearbox () = default;

	int		 current = 0;		/* the paddles' gear */
	int		 current_out = 0;	/* the gear the game got */
	unsigned	 crossing = 0;		/* frames left in neutral */
	bool		 up_held = false, down_held = false;
	uint64_t	 last_frame = ~0ull;
	int		 agent_gear = NO_GEAR;
};

} /* namespace hl */

#endif /* __HL_GEARBOX_H__ */
