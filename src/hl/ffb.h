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

#ifndef __HL_FFB_H__
#define __HL_FFB_H__

#include <vector>

#include "hl/core.h"

namespace hl {

/* Force feedback to the player's wheel: the effects of the cabinet's motor,
 * played on the first SDL joystick that can (DirectInput on Windows), and
 * kept for the agent's "ffb". A constant force > 0 pushes to the left, the
 * trigonometric way, as the agent protocol has it; the others are strengths,
 * 0 to 1. Each effect plays until changed; STOP stops them all. Options:
 * FFB_GAIN, a percentage (100), FFB_INVERT, the constant force the other
 * way. */
class ForceFeedback {
public:
	enum Effect {
		STOP,
		CONSTANT,
		SPRING,
		FRICTION,
		VIBRATE,

		NUM_EFFECTS
	};

	struct Record {
		uint64_t frame;
		Effect	 effect;
		float	 force;
	};

	/* frame: the machine's, for the records */
	static void	 set (Effect effect, float force, uint64_t frame);
	static float	 get (Effect effect);
	/* The records since the last call */
	static std::vector<Record> take_records ();
	static const char *effect_name (Effect effect);

	/* A joystick plugged in or out: looks for a device again */
	static void	 devices_changed ();
	static void	 shutdown ();
};

} /* namespace hl */

#endif /* __HL_FFB_H__ */
