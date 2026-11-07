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

#include <math.h>

#include "hl/ffb.h"
#include "hl/config.h"
#include "hl/controls.h"

namespace hl {

/* The records kept for the agent, at most */
#define MAX_RECORDS	4096

/* The spring's dead band about the centre, of the game's wheel's travel */
#define SPRING_DEADBAND	0.05

static float current[ForceFeedback::NUM_EFFECTS];
static std::vector<ForceFeedback::Record> records;

/* The SDL device, and its effects (-1: not made yet, or not supported) */
static bool sdl_ready, looked;
static SDL_Joystick *joystick;
static SDL_Haptic *haptic;
static unsigned supported;
static int effect_id[ForceFeedback::NUM_EFFECTS];
static bool running[ForceFeedback::NUM_EFFECTS];

static float
clamp1 (float v, float lo)
{
	return (v < lo) ? lo : (v > 1.0f) ? 1.0f : v;
}

static void
close_device (void)
{
	unsigned i;

	if (haptic)
		SDL_HapticClose (haptic);
	if (joystick)
		SDL_JoystickClose (joystick);
	haptic = NULL;
	joystick = NULL;
	for (i = 0; i < ForceFeedback::NUM_EFFECTS; i++) {
		effect_id[i] = -1;
		running[i] = false;
	}
}

/* The first joystick with force feedback */
static void
open_device (void)
{
	int i;

	looked = true;
	if (!sdl_ready) {
		if (SDL_InitSubSystem (SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
			HL_PRINT ("ffb: no force feedback: %s", SDL_GetError ());
			return;
		}
		sdl_ready = true;
	}
	for (i = 0; i < SDL_NumJoysticks () && !haptic; i++) {
		SDL_Joystick *joy = SDL_JoystickOpen (i);
		if (!joy)
			continue;
		if (SDL_JoystickIsHaptic (joy) == 1)
			haptic = SDL_HapticOpenFromJoystick (joy);
		if (haptic)
			joystick = joy;
		else
			SDL_JoystickClose (joy);
	}
	if (!haptic)
		return;
	supported = SDL_HapticQuery (haptic);
	if (supported & SDL_HAPTIC_GAIN)
		SDL_HapticSetGain (haptic, 100);
	if (supported & SDL_HAPTIC_AUTOCENTER)
		SDL_HapticSetAutocenter (haptic, 0);
	HL_PRINT ("ffb: force feedback on '%s'", SDL_JoystickName (joystick));
	for (i = 0; i < ForceFeedback::NUM_EFFECTS; i++)
		effect_id[i] = -1;
}

/* An effect at force, on the device: made the first time, then updated */
static void
play (ForceFeedback::Effect effect, float force)
{
	static const unsigned types[ForceFeedback::NUM_EFFECTS] = {
		0, SDL_HAPTIC_CONSTANT, SDL_HAPTIC_SPRING, SDL_HAPTIC_FRICTION, SDL_HAPTIC_SINE, 0, 0
	};
	SDL_HapticEffect e;

	if (!haptic || !(supported & types[effect]))
		return;
	memset (&e, 0, sizeof (e));
	e.type = (Uint16) types[effect];
	switch (effect) {
	case ForceFeedback::CONSTANT:
		/* Where the force comes from: the right, so it pushes left
		 * when > 0 */
		e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
		e.constant.direction.dir[0] = 1;
		e.constant.length = SDL_HAPTIC_INFINITY;
		e.constant.level = (Sint16) lrintf (force * 32767.0f);
		break;
	case ForceFeedback::SPRING:
	case ForceFeedback::FRICTION:
		e.condition.direction.type = SDL_HAPTIC_CARTESIAN;
		e.condition.direction.dir[0] = 1;
		e.condition.length = SDL_HAPTIC_INFINITY;
		e.condition.right_sat[0] = e.condition.left_sat[0] = 0xFFFF;
		e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16) lrintf (force * 32767.0f);
		/* The spring on the game's wheel, a part of the device's travel
		 * with WHEEL_ROTATION; the friction follows the speed, not the
		 * position, and the game's own torque the position it reads */
		if (effect == ForceFeedback::SPRING) {
			float coefficient, deadband;
			ForceFeedback::spring_on_wheel (force, Config::get ()->wheel_scale (), &coefficient, &deadband);
			e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16) lrintf (coefficient * 32767.0f);
			e.condition.deadband[0] = (Uint16) (deadband * 0xFFFF);
		}
		break;
	case ForceFeedback::VIBRATE:
		e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
		e.periodic.direction.dir[0] = 1;
		e.periodic.length = SDL_HAPTIC_INFINITY;
		e.periodic.period = 50;
		e.periodic.magnitude = (Sint16) lrintf (force * 32767.0f);
		break;
	default:
		return;
	}
	if (effect_id[effect] < 0)
		effect_id[effect] = SDL_HapticNewEffect (haptic, &e);
	else
		SDL_HapticUpdateEffect (haptic, effect_id[effect], &e);
	if (effect_id[effect] < 0)
		return;
	if (force == 0.0f && running[effect]) {
		SDL_HapticStopEffect (haptic, effect_id[effect]);
		running[effect] = false;
	} else if (force != 0.0f && !running[effect]) {
		SDL_HapticRunEffect (haptic, effect_id[effect], 1);
		running[effect] = true;
	}
}

/* What the device gets: the options applied */
static float
device_force (ForceFeedback::Effect effect, float force)
{
	static int gain = -1;
	static bool invert;

	if (gain < 0) {
		gain = hl_util_get_int_option ("FFB_GAIN", 100);
		invert = hl_util_get_bool_option ("FFB_INVERT", false);
	}
	force = clamp1 (force * gain / 100.0f, (effect == ForceFeedback::CONSTANT) ? -1.0f : 0.0f);
	return (invert && effect == ForceFeedback::CONSTANT) ? -force : force;
}

void
ForceFeedback::set (Effect effect, float force, uint64_t frame)
{
	unsigned i;

	if (effect == STOP) {
		for (i = CONSTANT; i < RUMBLE_LOW; i++)
			set ((Effect) i, 0.0f, frame);
		rumble (0.0f, 0.0f, frame);
		return;
	}
	if (effect == RUMBLE_LOW || effect == RUMBLE_HIGH) {
		rumble (effect == RUMBLE_LOW ? force : current[RUMBLE_LOW],
		        effect == RUMBLE_HIGH ? force : current[RUMBLE_HIGH], frame);
		return;
	}
	force = clamp1 (force, (effect == CONSTANT) ? -1.0f : 0.0f);
	if (force == current[effect])
		return;
	current[effect] = force;

	if (records.size () >= MAX_RECORDS)
		records.erase (records.begin ());
	records.push_back ({ frame, effect, force });

	if (!looked)
		open_device ();
	play (effect, device_force (effect, force));
}

/* The controller's motors: what was last sent, and when */
static uint64_t rumble_sent = ~0ull;

static void
send_rumble (uint64_t frame)
{
	SDL_Joystick *joy = (SDL_Joystick *) Controls::get ().player_joystick (0);
	float gain = hl_util_get_int_option ("FFB_GAIN", 100) / 100.0f;
	auto level = [&] (float x) { return (Uint16) lrintf (clamp1 (x * gain, 0.0f) * 65535.0f); };

	rumble_sent = frame;
	if (joy)
		SDL_JoystickRumble (joy, level (current[ForceFeedback::RUMBLE_LOW]),
		                    level (current[ForceFeedback::RUMBLE_HIGH]), 1000);
}

void
ForceFeedback::rumble (float low, float high, uint64_t frame)
{
	bool changed = false;
	const Effect effects[2] = { RUMBLE_LOW, RUMBLE_HIGH };
	float force[2] = { clamp1 (low, 0.0f), clamp1 (high, 0.0f) };

	for (int i = 0; i < 2; i++) {
		/* Steps of 1/64: a record, and a message to the controller, at
		 * each change that can be felt */
		float f = roundf (force[i] * 64.0f) / 64.0f;
		if (f == current[effects[i]])
			continue;
		current[effects[i]] = f;
		changed = true;
		if (records.size () >= MAX_RECORDS)
			records.erase (records.begin ());
		records.push_back ({ frame, effects[i], f });
	}
	if (changed || (current[RUMBLE_LOW] + current[RUMBLE_HIGH] > 0 &&
	                (rumble_sent == ~0ull || frame - rumble_sent >= 30 || frame < rumble_sent)))
		send_rumble (frame);
}

float
ForceFeedback::get (Effect effect)
{
	return current[effect];
}

std::vector<ForceFeedback::Record>
ForceFeedback::take_records ()
{
	std::vector<Record> taken;

	taken.swap (records);
	return taken;
}

const char *
ForceFeedback::effect_name (Effect effect)
{
	/* The agent protocol's names */
	static const char *const names[NUM_EFFECTS] = {
		"stop", "constant", "selfcenter", "friction", "vibrate", "rumble_low", "rumble_high"
	};
	return names[effect];
}

void
ForceFeedback::devices_changed ()
{
	unsigned i;

	if (!looked)
		return;
	if (haptic && !SDL_JoystickGetAttached (joystick))
		close_device ();
	if (haptic)
		return;
	open_device ();
	/* The effects going on, on the new device */
	for (i = CONSTANT; i < RUMBLE_LOW; i++)
		if (current[i] != 0.0f)
			play ((Effect) i, device_force ((Effect) i, current[i]));
}

void
ForceFeedback::wheel_changed ()
{
	if (haptic && current[SPRING] != 0.0f)
		play (SPRING, device_force (SPRING, current[SPRING]));
}

void
ForceFeedback::spring_on_wheel (float strength, double scale, float *coefficient, float *deadband)
{
	if (scale < 1.0)
		scale = 1.0;
	*coefficient = clamp1 ((float) (strength * scale), 0.0f);
	*deadband = (float) (SPRING_DEADBAND / scale);
}

void
ForceFeedback::shutdown ()
{
	if (haptic)
		SDL_HapticStopAll (haptic);
	close_device ();
}

} /* namespace hl */
