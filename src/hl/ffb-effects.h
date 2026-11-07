/*
 * Hikaruloader
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

#ifndef __HL_FFB_EFFECTS_H__
#define __HL_FFB_EFFECTS_H__

#include <cstdint>

#include "hl/telemetry.h"

namespace hl {

/* Force feedback made from a game's telemetry, for a cabinet without a motor
 * (Star Wars: Racer Arcade's pods): on a wheel (hl::ForceFeedback's
 * DirectInput effects) the centring, the turns' weight, the damping, the
 * hits' kicks, a rumble of the engines, the scraping, the bumps and the
 * landings; on player 1's controller the same as its two motors' rumble.
 * [ForceFeedback] in hikaruloader.ini sets each family's strength. Nothing
 * outside a race: the effects stop in the menus and in a pause. */
class FfbEffects {
public:
	/* Each frame decoded */
	void	 frame (const telemetry::Frame &f);
	/* The game paused, or gone */
	void	 stop (uint64_t frame);

private:
	bool	 playing = false;
	float	 corner = 0;		/* the turn's weight, smoothed */
	float	 kick = 0;		/* a hit's kick, decaying (> 0 to the left) */
	float	 jolt = 0;		/* a hit's or a landing's shake, decaying */
	float	 avert_slow = 0;	/* the vertical acceleration, smoothed */
	float	 alat_slow = 0;		/* the lateral one, smoothed */
	bool	 was_wall = false;
	uint64_t last_frame = 0;
};

} /* namespace hl */

#endif /* __HL_FFB_EFFECTS_H__ */
