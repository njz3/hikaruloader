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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "hl/ffb.h"
#include "hl/config.h"
#include "hl/controls.h"

namespace hl {

/* The records kept for the agent, at most */
#define MAX_RECORDS	4096

/* The spring's dead band about the centre, of the game's wheel's travel */
#define SPRING_DEADBAND	0.05

/* The vibration's period */
#define SINE_PERIOD_MS	50

static float current[ForceFeedback::NUM_EFFECTS];
static std::atomic<float> spring_center;
static std::vector<ForceFeedback::Record> records;

/* The SDL device, and its effects (-1: not made yet, or not supported) */
static bool sdl_ready, looked;
static std::atomic<SDL_Joystick *> joystick;
/* The player whose controller rumbles, -1 the usual one */
static std::atomic<int> rumble_player (-1), rumble_second_player (-1);
/* [ForceFeedback] DEVICE, a controller: its motors take the rumble */
static std::atomic<SDL_Joystick *> chosen_motors;
static std::atomic<SDL_Haptic *> haptic;
static std::mutex &device_lock = *new std::mutex;
static unsigned supported;
static char device_name[128];
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
	std::lock_guard<std::mutex> device (device_lock);
	unsigned i;

	if (haptic)
		SDL_HapticClose (haptic);
	if (joystick)
		SDL_JoystickClose (joystick);
	if (chosen_motors)
		SDL_JoystickClose (chosen_motors);
	haptic = NULL;
	joystick = NULL;
	chosen_motors = NULL;
	device_name[0] = '\0';
	for (i = 0; i < ForceFeedback::NUM_EFFECTS; i++) {
		effect_id[i] = -1;
		running[i] = false;
	}
}

/* SDL's joysticks and haptics, once: at the first effect, or for the list of
 * the devices, in the menus before any */
static bool
init_sdl (void)
{
	if (sdl_ready)
		return true;
	if (SDL_InitSubSystem (SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
		HL_PRINT ("ffb: no force feedback: %s", SDL_GetError ());
		return false;
	}
	sdl_ready = true;
	return true;
}

/* The first joystick with force feedback */
static void
open_device (void)
{
	int i;

	std::lock_guard<std::mutex> device (device_lock);

	looked = true;
	if (!init_sdl ())
		return;
	const char *chosen = Config::get ()->ffb_device;

	for (i = 0; i < SDL_NumJoysticks () && !haptic && !chosen_motors; i++) {
		SDL_Joystick *joy = SDL_JoystickOpen (i);
		if (!joy)
			continue;
		if (chosen[0]) {
			const char *name = SDL_JoystickName (joy);

			if (!name || strcmp (name, chosen)) {
				SDL_JoystickClose (joy);
				continue;
			}
			if (SDL_JoystickHasRumble (joy)) {
				chosen_motors = joy;
				HL_PRINT ("ffb: rumble on '%s'", name);
				snprintf (device_name, sizeof (device_name), "%s", name);
				continue;
			}
		}
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
	snprintf (device_name, sizeof (device_name), "%s", SDL_JoystickName (joystick));
	for (i = 0; i < ForceFeedback::NUM_EFFECTS; i++)
		effect_id[i] = -1;
}

static bool
inverted (void)
{
	static const bool invert = hl_util_get_bool_option ("FFB_INVERT", false);
	return invert;
}

/* The spring's centre on the device, a fraction of its travel: nearer with
 * WHEEL_ROTATION, the game's wheel being shorter than the device's */
static float
device_center (void)
{
	double scale = Config::get ()->wheel_scale ();
	float center = (float) (spring_center / (scale < 1.0 ? 1.0 : scale));
	return inverted () ? -center : center;
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
			e.condition.center[0] = (Sint16) lrintf (device_center () * 32767.0f);
		}
		break;
	case ForceFeedback::VIBRATE:
		e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
		e.periodic.direction.dir[0] = 1;
		e.periodic.length = SDL_HAPTIC_INFINITY;
		e.periodic.period = SINE_PERIOD_MS;
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

/* A write to the device is a USB transfer, a whole frame's time on a wheel
 * that answers slowly: they are made on a thread of their own, for the
 * machine never to wait for them. It sends each effect's last force, a newer
 * one taking the place of one not sent yet. A pause after each one
 * (FFB_SEND_MS), as DirectInput makes the inputs' reading wait for the
 * device's transfer: without it the wheel is always busy, and the frames
 * wait all the same. Never destroyed: the thread may still be in a transfer
 * as the program ends. */
struct Sender {
	std::thread		 thread;
	std::mutex		 lock;
	std::condition_variable	 wake;
	bool			 pending, quit;
	bool			 queued[ForceFeedback::NUM_EFFECTS];
	float			 force[ForceFeedback::NUM_EFFECTS];
	bool			 rumble;
	SDL_JoystickID		 rumble_joystick;
	SDL_JoystickID		 rumble_second;	/* -1 none */
	std::vector<SDL_JoystickID> rumble_stops;
	Uint16			 rumble_low, rumble_high;
};

static Sender &sender = *new Sender ();

static void
send_loop (void)
{
	std::unique_lock<std::mutex> lock (sender.lock);
	bool queued[ForceFeedback::NUM_EFFECTS];
	float force[ForceFeedback::NUM_EFFECTS];
	int interval = hl_util_get_int_option ("FFB_SEND_MS", 16);
	unsigned i;

	while (true) {
		sender.wake.wait (lock, [] { return sender.pending || sender.quit; });
		if (!sender.pending)
			return;
		memcpy (queued, sender.queued, sizeof (queued));
		memcpy (force, sender.force, sizeof (force));
		memset (sender.queued, 0, sizeof (sender.queued));
		bool rumble = sender.rumble;
		SDL_JoystickID id = sender.rumble_joystick, second = sender.rumble_second;
		std::vector<SDL_JoystickID> stops;
		stops.swap (sender.rumble_stops);
		Uint16 low = sender.rumble_low, high = sender.rumble_high;
		sender.pending = sender.rumble = false;
		lock.unlock ();

		for (i = ForceFeedback::CONSTANT; i < ForceFeedback::RUMBLE_LOW; i++)
			if (queued[i]) {
				{
					std::lock_guard<std::mutex> device (device_lock);
					play ((ForceFeedback::Effect) i, force[i]);
				}
				std::this_thread::sleep_for (std::chrono::milliseconds (interval));
			}
		if (rumble) {
			/* Its instance: the player's joystick may be gone */
			SDL_Joystick *joy = SDL_JoystickFromInstanceID (id);
			if (joy)
				SDL_JoystickRumble (joy, low, high, 1000);
			joy = second >= 0 ? SDL_JoystickFromInstanceID (second) : NULL;
			if (joy)
				SDL_JoystickRumble (joy, low, high, 1000);
		}
		for (SDL_JoystickID stop : stops) {
			SDL_Joystick *joy = SDL_JoystickFromInstanceID (stop);
			if (joy)
				SDL_JoystickRumble (joy, 0, 0, 0);
		}
		lock.lock ();
	}
}

/* sender.lock held */
static void
wake_sender (void)
{
	if (!sender.thread.joinable ()) {
		sender.quit = false;
		sender.thread = std::thread (send_loop);
	}
	sender.pending = true;
	sender.wake.notify_one ();
}

static void
send_force (ForceFeedback::Effect effect, float force)
{
	if (!haptic)
		return;
	std::lock_guard<std::mutex> lock (sender.lock);
	sender.queued[effect] = true;
	sender.force[effect] = force;
	wake_sender ();
}

/* What the device gets: the options applied */
static float
device_force (ForceFeedback::Effect effect, float force)
{
	static int gain = -1;

	if (gain < 0)
		gain = hl_util_get_int_option ("FFB_GAIN", 100);
	force = clamp1 (force * gain / 100.0f * Config::get ()->ffb_gain / 100.0f,
	                (effect == ForceFeedback::CONSTANT) ? -1.0f : 0.0f);
	return (inverted () && effect == ForceFeedback::CONSTANT) ? -force : force;
}

void
ForceFeedback::set (Effect effect, float force, uint64_t frame)
{
	unsigned i;

	if (effect == STOP) {
		spring_center = 0.0f;
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
	send_force (effect, device_force (effect, force));
}

/* The controller's motors: what was last sent, and when */
static uint64_t rumble_sent = ~0ull;

static float
rumble_force (float force)
{
	return clamp1 (force * hl_util_get_int_option ("FFB_GAIN", 100) / 100.0f * Config::get ()->ffb_gain / 100.0f, 0.0f);
}

static void
send_rumble (uint64_t frame)
{
	/* The device chosen, or player 1's controller; a wheel chosen: none */
	SDL_Joystick *joy = rumble_player >= 0 ? (SDL_Joystick *) Controls::get ().player_joystick (rumble_player) :
	                    chosen_motors ? (SDL_Joystick *) chosen_motors :
	                    Config::get ()->ffb_device[0] ? NULL :
	                    (SDL_Joystick *) Controls::get ().player_joystick (0);
	auto level = [] (float x) { return (Uint16) lrintf (rumble_force (x) * 65535.0f); };

	rumble_sent = frame;
	/* Not the wheel with the forces: SDL would hold its joysticks' lock,
	 * which the inputs' reading waits for, during the wheel's transfer */
	if (!joy || joy == joystick)
		return;
	std::lock_guard<std::mutex> lock (sender.lock);
	sender.rumble = true;
	sender.rumble_joystick = SDL_JoystickInstanceID (joy);
	{
		SDL_Joystick *other = rumble_second_player >= 0 ?
		                      (SDL_Joystick *) Controls::get ().player_joystick (rumble_second_player) : NULL;
		sender.rumble_second = (other && other != joystick) ? SDL_JoystickInstanceID (other) : -1;
	}
	sender.rumble_low = level (current[ForceFeedback::RUMBLE_LOW]);
	sender.rumble_high = level (current[ForceFeedback::RUMBLE_HIGH]);
	wake_sender ();
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

ForceFeedback::Status
ForceFeedback::status ()
{
	Status s;

	s.device = device_name;
	spring_on_wheel (device_force (SPRING, current[SPRING]), Config::get ()->wheel_scale (),
	                 &s.spring, &s.spring_deadband);
	s.spring_offset = device_center ();
	s.friction = device_force (FRICTION, current[FRICTION]);
	s.constant = device_force (CONSTANT, current[CONSTANT]);
	s.sine = device_force (VIBRATE, current[VIBRATE]);
	s.sine_hz = 1000.0f / SINE_PERIOD_MS;
	s.rumble_low = rumble_force (current[RUMBLE_LOW]);
	s.rumble_high = rumble_force (current[RUMBLE_HIGH]);
	s.rumble_on_wheel = joystick && Controls::get ().player_joystick (0) == joystick;
	return s;
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
	if ((haptic && !SDL_JoystickGetAttached (joystick)) ||
	    (chosen_motors && !SDL_JoystickGetAttached (chosen_motors)))
		close_device ();
	if (haptic || chosen_motors)
		return;
	open_device ();
	/* The effects going on, on the new device */
	for (i = CONSTANT; i < RUMBLE_LOW; i++)
		if (current[i] != 0.0f)
			send_force ((Effect) i, device_force ((Effect) i, current[i]));
}

void
ForceFeedback::rumble_for (int player, int second)
{
	const int before[2] = { rumble_player, rumble_second_player };

	rumble_player = player;
	rumble_second_player = second;
	for (int b : before) {
		if (b < 0 || b == player || b == second)
			continue;
		SDL_Joystick *joy = (SDL_Joystick *) Controls::get ().player_joystick (b);
		if (joy) {
			std::lock_guard<std::mutex> lock (sender.lock);
			sender.rumble_stops.push_back (SDL_JoystickInstanceID (joy));
			wake_sender ();
		}
	}
}

bool
ForceFeedback::is_force_device (int player)
{
	return joystick && Controls::get ().player_joystick (player) == joystick;
}

std::vector<std::string>
ForceFeedback::device_names ()
{
	std::vector<std::string> names;
	int i;

	std::lock_guard<std::mutex> device (device_lock);
	if (!init_sdl ())
		return names;
	for (i = 0; i < SDL_NumJoysticks (); i++) {
		SDL_Joystick *joy = SDL_JoystickOpen (i);
		const char *name;

		if (!joy)
			continue;
		name = SDL_JoystickName (joy);
		if (name && (SDL_JoystickIsHaptic (joy) == 1 || SDL_JoystickHasRumble (joy)) &&
		    std::find (names.begin (), names.end (), name) == names.end ())
			names.push_back (name);
		SDL_JoystickClose (joy);
	}
	return names;
}

void
ForceFeedback::device_changed ()
{
	unsigned i;

	if (!looked)
		return;
	close_device ();
	open_device ();
	for (i = CONSTANT; i < RUMBLE_LOW; i++)
		if (current[i] != 0.0f)
			send_force ((Effect) i, device_force ((Effect) i, current[i]));
	if (current[RUMBLE_LOW] + current[RUMBLE_HIGH] > 0)
		send_rumble (rumble_sent);
}

void
ForceFeedback::wheel_changed ()
{
	if (current[SPRING] != 0.0f)
		send_force (SPRING, device_force (SPRING, current[SPRING]));
}

void
ForceFeedback::spring_offset (float offset)
{
	offset = clamp1 (offset, -1.0f);
	if (offset == spring_center)
		return;
	spring_center = offset;
	if (current[SPRING] != 0.0f)
		send_force (SPRING, device_force (SPRING, current[SPRING]));
}

void
ForceFeedback::gain_changed ()
{
	unsigned i;

	for (i = CONSTANT; i < RUMBLE_LOW; i++)
		if (current[i] != 0.0f)
			send_force ((Effect) i, device_force ((Effect) i, current[i]));
	if (current[RUMBLE_LOW] + current[RUMBLE_HIGH] > 0)
		send_rumble (rumble_sent);
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
	if (sender.thread.joinable ()) {
		{
			std::lock_guard<std::mutex> lock (sender.lock);
			sender.quit = true;
			sender.wake.notify_one ();
		}
		sender.thread.join ();
	}
	if (haptic)
		SDL_HapticStopAll (haptic);
	close_device ();
}

} /* namespace hl */
