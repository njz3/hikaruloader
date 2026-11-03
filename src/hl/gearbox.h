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

namespace hl {

/* A cabinet's gear lever, four gears and neutral (NASCAR Arcade's), from the
 * player's controls, as sm2-emu has it: an H shifter's positions (Gear1 to
 * Gear4, GearNeutral in controls.ini), a button held each while the lever is
 * there; paddles (GearUp, GearDown) a gear up or down each press. A position
 * held wins. With none held, an H shifter's lever is in neutral
 * (SHIFTER_NEUTRAL), else the last gear stays; a paddle's shift from a gear
 * to another goes through neutral SHIFT_NEUTRAL_MS. Looked at once a frame
 * of the machine's, as the game polls its switches. */
class Gearbox {
public:
	static Gearbox	&get ();

	/* The gear engaged in that frame, 0 (neutral) to 4 */
	unsigned	 gear (uint64_t frame);
	/* The one of the last frame looked at, for the settings' page */
	unsigned	 engaged () const { return current_out; }
	/* In neutral, as a game starts */
	void		 reset ();
	/* The agent's gear, 0 to 4, over the controls; -1 the controls' again */
	void		 hold (int gear) { agent_gear = gear; }
	int		 held () const { return agent_gear; }
	/* A cabinet with one, its I/O board's (the agent's "Gear" input) */
	void		 attach (bool on) { present = on; }
	bool		 attached () const { return present; }

private:
	Gearbox () = default;

	unsigned	 current = 0;		/* the paddles' gear */
	unsigned	 current_out = 0;	/* the gear the game got */
	unsigned	 crossing = 0;		/* frames left in neutral */
	bool		 up_held = false, down_held = false;
	uint64_t	 last_frame = ~0ull;
	int		 agent_gear = -1;
	bool		 present = false;
};

} /* namespace hl */

#endif /* __HL_GEARBOX_H__ */
