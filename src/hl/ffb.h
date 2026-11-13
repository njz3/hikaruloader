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
 * way; [ForceFeedback] GAIN multiplies FFB_GAIN. The servo board's spring is the one effect the device works out from
 * the wheel's position: with [Input] WHEEL_ROTATION, the game's wheel is
 * shorter than the device's, and the spring as much stiffer. */
class ForceFeedback {
public:
	enum Effect {
		STOP,
		CONSTANT,
		SPRING,
		FRICTION,
		VIBRATE,
		/* A game controller's motors, the low (big) one and the high
		 * (small) one, 0 to 1: player 1's controller, rumbled through
		 * SDL (XInput), whether or not it has DirectInput's effects */
		RUMBLE_LOW,
		RUMBLE_HIGH,

		NUM_EFFECTS
	};

	struct Record {
		uint64_t frame;
		Effect	 effect;
		float	 force;
	};

	/* frame: the machine's, for the records */
	static void	 set (Effect effect, float force, uint64_t frame);
	/* VIBRATE's period, ms (10 to 1000; 50, 20 Hz, by default, and again
	 * at STOP), a drive board's road effect's: the vibration playing takes
	 * it at once */
	static void	 sine_period (unsigned ms);
	static float	 get (Effect effect);
	/* The spring's centre, -1 to 1 of the game's wheel's travel, > 0 to
	 * the right; 0 again at STOP */
	static void	 spring_offset (float offset);
	/* What the device is told now, the options applied, for the HUD: the
	 * spring's coefficient, its offset and dead band, fractions of the
	 * device's full scale; the rumble is not sent to the wheel that has
	 * the forces (rumble_on_wheel) */
	struct Status {
		const char *device;
		float	 spring, spring_offset, spring_deadband;
		float	 friction, constant;
		float	 sine, sine_hz;
		float	 rumble_low, rumble_high;
		bool	 rumble_on_wheel;
	};
	static Status	 status ();
	/* Player 1's controller's motors, each frame while they turn: sent
	 * again every half second, as SDL stops them after a while */
	static void	 rumble (float low, float high, uint64_t frame);
	/* The rumble goes to these players' controllers, whatever the device
	 * chosen; -1: back to the usual one. A controller left out stops */
	static void	 rumble_for (int player, int second = -1);
	/* That player's device is the one taking the forces */
	static bool	 is_force_device (int player);
	/* The records since the last call */
	static std::vector<Record> take_records ();
	static const char *effect_name (Effect effect);

	/* A joystick plugged in or out: looks for a device again */
	static void	 devices_changed ();
	/* The names of the devices that can take effects, for [ForceFeedback]
	 * DEVICE */
	static std::vector<std::string> device_names ();
	/* [ForceFeedback] DEVICE changed */
	static void	 device_changed ();
	/* [Input] WHEEL_ROTATION or WHEEL_RANGE changed: the spring again */
	static void	 wheel_changed ();
	/* [ForceFeedback] GAIN changed: the forces going on again */
	static void	 gain_changed ();
	static void	 shutdown ();

	/* The spring of the given strength (0 to 1) on a device whose travel
	 * is scale times the game's wheel's (Config::wheel_scale): its
	 * coefficient, scale times the strength, 1 at most, and its dead band
	 * about the centre, scale times narrower; both fractions of the
	 * device's full scale */
	static void	 spring_on_wheel (float strength, double scale,
			                  float *coefficient, float *deadband);
};

} /* namespace hl */

#endif /* __HL_FFB_H__ */
