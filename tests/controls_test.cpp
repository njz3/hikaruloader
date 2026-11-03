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

/* The controls' tests: the wheel's rotation (Config::wheel_scale,
 * Controls::steer, the force feedback's spring), the controllers' hotkeys
 * added to an older controls.ini, and the hotkeys and the wheel read from
 * SDL's virtual joysticks, a game controller with its Guide and a wheel, as
 * a player would press them; NASCAR Arcade's gear lever (hl::Gearbox) and
 * view change button, Star Wars: Racer Arcade's levers. Run by ctest, or
 * alone: controlstest. */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <SDL.h>

#include "hl/config.h"
#include "hl/controls.h"
#include "hl/ffb.h"
#include "hl/gearbox.h"
#include "hl/input.h"
#include "hl/jvs.h"

static int failures, checks;

static void
check (const char *name, bool ok)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf (stderr, "FAIL %s\n", name);
	}
}

static bool
near (double a, double b)
{
	return fabs (a - b) < 1e-3;
}

static void
write_text (const std::string &path, const char *text)
{
	std::ofstream out (path, std::ios::binary);
	out << text;
}

static void
test_wheel_scale (void)
{
	float coefficient, deadband;

	check ("scale: 0 is the whole turn", hl::Config::wheel_scale (0, 900) == 1.0);
	check ("scale: 270 of 900", near (hl::Config::wheel_scale (270, 900), 900.0 / 270.0));
	check ("scale: 540 of 1080", near (hl::Config::wheel_scale (540, 1080), 2.0));
	check ("scale: the wheel's own turn", hl::Config::wheel_scale (900, 900) == 1.0);
	check ("scale: never below 1", hl::Config::wheel_scale (1080, 900) == 1.0);

	check ("steer: as is", near (hl::Controls::steer (0.5, 1.0), 0.5));
	check ("steer: further", near (hl::Controls::steer (0.15, 900.0 / 270.0), 0.5));
	check ("steer: held at the right end", hl::Controls::steer (0.5, 900.0 / 270.0) == 1.0);
	check ("steer: held at the left end", hl::Controls::steer (-0.31, 900.0 / 270.0) == -1.0);

	hl::ForceFeedback::spring_on_wheel (0.3f, 1.0, &coefficient, &deadband);
	check ("spring: as before", near (coefficient, 0.3) && near (deadband, 0.05));
	hl::ForceFeedback::spring_on_wheel (0.1f, 2.0, &coefficient, &deadband);
	check ("spring: twice as stiff, half the dead band", near (coefficient, 0.2) && near (deadband, 0.025));
	hl::ForceFeedback::spring_on_wheel (0.5f, 900.0 / 270.0, &coefficient, &deadband);
	check ("spring: the device's most", coefficient == 1.0f);
}

/* A file from before the hotkeys: its rows kept as they are (the user's
 * file), the rows it lacks with their defaults */
static void
test_upgrade (const std::string &dir)
{
	hl::Controls &c = hl::Controls::get ();
	std::string path = dir + "/old.ini";

	write_text (path,
		"[Config]\n"
		"Steer_DeadZone = 0\n"
		"[Common]\n"
		"Test = KEY_F5\n"
		"ExitGame = KEY_F9\n"
		"[Digital]\n"
		"P1_Card1Insert = KEY_F7\n"
		"[Emulator]\n"
		"Pause = KEY_P\n"
		"Menu = KEY_Tab, GC0_BUTTON_BACK\n");
	c.load (path.c_str ());
	check ("upgrade: Quit kept", c.value ("Common", "ExitGame") == "KEY_F9");
	check ("upgrade: the card kept", c.value ("Digital", "P1_Card1Insert") == "KEY_F7");
	check ("upgrade: the user's key kept", c.value ("Emulator", "Pause") == "KEY_P");
	check ("upgrade: the menu kept", c.value ("Emulator", "Menu") == "KEY_Tab, GC0_BUTTON_BACK");
	check ("upgrade: Back added", c.value ("Emulator", "Back") == c.default_value ("Emulator", "Back"));
	check ("upgrade: a missing row's default", c.value ("Emulator", "SaveState") ==
	                                          "KEY_F4, GC0_BUTTON_GUIDE + GC0_BUTTON_RIGHTSHOULDER");
	check ("upgrade: no hotkey, no change", c.value ("Common", "Test") == "KEY_F5");

	/* Loaded again: still as the user left it */
	c.load (path.c_str ());
	check ("upgrade: kept again", c.value ("Emulator", "Pause") == "KEY_P" &&
	                              c.value ("Common", "ExitGame") == "KEY_F9");
}

/* The actions the events pressed, each event looked at as the emulator's
 * loop does */
static std::vector<int>
pump (void)
{
	std::vector<int> actions;
	SDL_Event event;

	while (SDL_PollEvent (&event)) {
		int a = hl::Input::event_action (&event);
		if (a >= 0)
			actions.push_back (a);
	}
	return actions;
}

static bool
has (const std::vector<int> &actions, hl::Action action)
{
	return std::find (actions.begin (), actions.end (), (int) action) != actions.end ();
}

static void
button (SDL_Joystick *pad, SDL_GameControllerButton b, bool down)
{
	SDL_JoystickSetVirtualButton (pad, b, down ? 1 : 0);
}

static void
test_devices (const std::string &dir)
{
	hl::Controls &c = hl::Controls::get ();
	hl::Config *cfg = hl::Config::get ();
	SDL_VirtualJoystickDesc desc;
	SDL_Joystick *pad, *wheel;
	char pad_guid[64], wheel_guid[64], text[1024];
	int pad_index, wheel_index;
	std::vector<int> a;

	/* A game controller, SDL's buttons and axes in their order; a wheel */
	memset (&desc, 0, sizeof (desc));
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
	desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
	desc.name = "controlstest pad";
	pad_index = SDL_JoystickAttachVirtualEx (&desc);
	wheel_index = SDL_JoystickAttachVirtual (SDL_JOYSTICK_TYPE_WHEEL, 3, 8, 0);
	check ("virtual devices", pad_index >= 0 && wheel_index >= 0);
	if (pad_index < 0 || wheel_index < 0) {
		fprintf (stderr, "no virtual joysticks: %s\n", SDL_GetError ());
		return;
	}
	pad = SDL_JoystickOpen (pad_index);
	wheel = SDL_JoystickOpen (wheel_index);
	check ("the pad is a game controller", SDL_IsGameController (pad_index));
	SDL_JoystickGetGUIDString (SDL_JoystickGetDeviceGUID (pad_index), pad_guid, sizeof (pad_guid));
	SDL_JoystickGetGUIDString (SDL_JoystickGetDeviceGUID (wheel_index), wheel_guid, sizeof (wheel_guid));
	pump ();

	/* Player 1's: the pad, GC0 and JOY0; player 2's: the wheel, JOY1 */
	snprintf (text, sizeof (text),
		"[Config]\n"
		"Steer_DeadZone = 0\n"
		"Gas_DeadZone = 0\n"
		"[Driving]\n"
		"P1_Steer = GC0_AXIS_LEFTX, JOY1_AXIS_0\n"
		"P1_Gas = JOY1_AXIS_1_NEGATIVE_SIDE\n"
		"[Emulator]\n"
		"Back = KEY_Escape, GC0_BUTTON_GUIDE + GC0_BUTTON_B\n"
		"[ControllerGUIDs]\n"
		"P1_GUID = %s\n"
		"P2_GUID = %s\n", pad_guid, wheel_guid);
	write_text (dir + "/devices.ini", text);
	c.load ((dir + "/devices.ini").c_str ());

	/* Escape: the Back row's, no longer the emulator's own */
	{
		SDL_Event key;
		memset (&key, 0, sizeof (key));
		key.type = SDL_KEYDOWN;
		key.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
		key.key.keysym.sym = SDLK_ESCAPE;
		check ("Escape: back", hl::Input::event_action (&key) == hl::ACTION_BACK);
		key.key.repeat = 1;
		check ("Escape repeated: nothing", hl::Input::event_action (&key) == -1);
	}

	/* Start alone: the game's */
	button (pad, SDL_CONTROLLER_BUTTON_START, true);
	a = pump ();
	check ("Start: the game's", has (a, hl::ACTION_START1) && !has (a, hl::ACTION_QUIT));
	check ("Start: held", hl::Input::action (hl::ACTION_START1));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();

	/* Guide + Start: quit, the game sees no Start, not even once Guide is
	 * let go first */
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
	a = pump ();
	check ("Guide alone: nothing", a.empty ());
	button (pad, SDL_CONTROLLER_BUTTON_START, true);
	a = pump ();
	check ("Guide + Start: quit", has (a, hl::ACTION_QUIT) && !has (a, hl::ACTION_START1));
	check ("Guide + Start: no Start for the game", !hl::Input::action (hl::ACTION_START1));
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
	pump ();
	check ("Guide let go first: still no Start", !hl::Input::action (hl::ACTION_START1));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();
	button (pad, SDL_CONTROLLER_BUTTON_START, true);
	pump ();
	check ("Start again: the game's", hl::Input::action (hl::ACTION_START1));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();

	/* The other hotkeys, each with Guide held */
	static const struct {
		SDL_GameControllerButton button;
		hl::Action hotkey, game;
		const char *name;
	} hotkeys[] = {
		{ SDL_CONTROLLER_BUTTON_BACK, hl::ACTION_SETTINGS, hl::NUM_ACTIONS, "Guide + Back: the settings" },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, hl::ACTION_SAVE_STATE, hl::NUM_ACTIONS, "Guide + R1: save" },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, hl::ACTION_LOAD_STATE, hl::NUM_ACTIONS, "Guide + L1: load" },
		{ SDL_CONTROLLER_BUTTON_B, hl::ACTION_BACK, hl::ACTION_BUTTON2, "Guide + B: back" },
		{ SDL_CONTROLLER_BUTTON_X, hl::ACTION_PAUSE, hl::ACTION_BUTTON3, "Guide + X: pause" },
		{ SDL_CONTROLLER_BUTTON_Y, hl::ACTION_CARD, hl::ACTION_BUTTON4, "Guide + Y: the card" },
	};
	for (const auto &h : hotkeys) {
		button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
		pump ();
		button (pad, h.button, true);
		a = pump ();
		check (h.name, has (a, h.hotkey) && (h.game == hl::NUM_ACTIONS || !has (a, h.game)));
		if (h.game != hl::NUM_ACTIONS)
			check (h.name, !hl::Input::action (h.game));
		button (pad, h.button, false);
		button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
		pump ();
	}
	/* The card's reader polls it */
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
	button (pad, SDL_CONTROLLER_BUTTON_Y, true);
	pump ();
	check ("Guide + Y: the card held", hl::Input::action (hl::ACTION_CARD) && !hl::Input::action (hl::ACTION_BUTTON4));
	button (pad, SDL_CONTROLLER_BUTTON_Y, false);
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
	pump ();

	/* Without Guide: the game's buttons, Back still the settings */
	button (pad, SDL_CONTROLLER_BUTTON_X, true);
	a = pump ();
	check ("X alone: button 3", has (a, hl::ACTION_BUTTON3) && !has (a, hl::ACTION_PAUSE) &&
	                            hl::Input::action (hl::ACTION_BUTTON3));
	button (pad, SDL_CONTROLLER_BUTTON_X, false);
	button (pad, SDL_CONTROLLER_BUTTON_BACK, true);
	a = pump ();
	check ("Back alone: the settings", has (a, hl::ACTION_SETTINGS));
	button (pad, SDL_CONTROLLER_BUTTON_BACK, false);
	pump ();

	/* The wheel: its axis that much further with WHEEL_ROTATION, held at
	 * its ends; the pad's stick its own way */
	SDL_JoystickSetVirtualAxis (wheel, 0, 3277);
	pump ();
	cfg->wheel_rotation = 0;
	cfg->wheel_range = 900;
	check ("wheel: the whole turn", near (c.axis ("Driving", "P1_Steer"), 3277 / 32767.0));
	cfg->wheel_rotation = 270;
	check ("wheel: 270 of 900", near (c.axis ("Driving", "P1_Steer"), 3277 / 32767.0 * 900 / 270));
	SDL_JoystickSetVirtualAxis (wheel, 0, -16384);
	pump ();
	check ("wheel: held at the left end", c.axis ("Driving", "P1_Steer") == -1.0);
	SDL_JoystickSetVirtualAxis (wheel, 0, 0);
	SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTX, 6554);
	pump ();
	check ("pad's stick: its own way", near (c.axis ("Driving", "P1_Steer"), 6554 / 32767.0));
	cfg->wheel_rotation = 0;
	SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTX, 0);

	/* Star Wars: Racer Arcade's levers. Combined: the accelerator both,
	 * the wheel the difference, the left engine harder to the right */
	{
		const double half = 16384 / 32767.0;
		double left, right;

		cfg->twin_levers = hl::TWIN_LEVERS_COMBINED;
		SDL_JoystickSetVirtualAxis (wheel, 1, -32767);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: the accelerator's thrust on both", near (left, 1.0) && near (right, 1.0));
		SDL_JoystickSetVirtualAxis (wheel, 0, 16384);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: to the right, the right one eased", near (left, 1.0) && near (right, 1.0 - half));
		SDL_JoystickSetVirtualAxis (wheel, 1, -16384);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: half the accelerator, the same mean",
		       near (left, half * (1.0 + half)) && near (right, half * (1.0 - half)));
		SDL_JoystickSetVirtualAxis (wheel, 0, -32767);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: all to the left, the left one at rest", near (left, 0.0) && near (right, 2.0 * half));
		SDL_JoystickSetVirtualAxis (wheel, 1, 0);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: no accelerator, no thrust", left == 0.0 && right == 0.0);
		SDL_JoystickSetVirtualAxis (wheel, 0, 0);

		/* Original: an axis each (the pad's sticks pushed up) */
		cfg->twin_levers = hl::TWIN_LEVERS_ORIGINAL;
		SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTY, -32767);
		SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_RIGHTY, -16384);
		pump ();
		hl::twin_lever_thrusts (&left, &right);
		check ("levers: an axis each", near (left, 1.0) && near (right, half));
		SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTY, 0);
		SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_RIGHTY, 0);
		pump ();
		cfg->twin_levers = hl::TWIN_LEVERS_COMBINED;
	}

	SDL_JoystickClose (pad);
	SDL_JoystickClose (wheel);
	SDL_JoystickDetachVirtual (wheel_index);
	SDL_JoystickDetachVirtual (pad_index);
}

/* NASCAR Arcade's gear lever, as sm2-emu's: the paddles a gear each press,
 * through neutral between two gears SHIFT_NEUTRAL_MS; an H shifter's
 * positions, its lever in none of them in neutral (SHIFTER_NEUTRAL); the
 * agent's gear over the controls; looked at once a frame */
static void
test_gearbox (void)
{
	hl::Gearbox &g = hl::Gearbox::get ();
	hl::Config *cfg = hl::Config::get ();
	hl::Controls &c = hl::Controls::get ();
	uint64_t frame = 1;
	unsigned gear, neutral;
	int i;

	auto press = [&] (hl::Action a, bool on) {
		hl::Input::set_action (a, on);
		return g.gear (frame++);
	};

	g.reset ();
	cfg->shift_neutral_ms = 0;
	cfg->shifter_neutral = true;
	check ("gears: neutral at first", g.gear (frame++) == 0);
	check ("gears: a gear up, 1", press (hl::ACTION_GEAR_UP, true) == 1);
	check ("gears: held, still 1", press (hl::ACTION_GEAR_UP, true) == 1);
	press (hl::ACTION_GEAR_UP, false);
	for (i = 0; i < 5; i++) {
		press (hl::ACTION_GEAR_UP, true);
		press (hl::ACTION_GEAR_UP, false);
	}
	check ("gears: 4 at most", g.gear (frame++) == 4);
	check ("gears: a gear down, 3", press (hl::ACTION_GEAR_DOWN, true) == 3);
	hl::Input::set_action (hl::ACTION_GEAR_DOWN, false);
	check ("gears: a frame looked at once", g.gear (frame - 1) == 3);

	/* 100 ms through neutral: six frames */
	cfg->shift_neutral_ms = 100;
	gear = press (hl::ACTION_GEAR_UP, true);
	hl::Input::set_action (hl::ACTION_GEAR_UP, false);
	for (neutral = gear ? 0 : 1; neutral < 20 && (gear = g.gear (frame++)) == 0; neutral++)
		;
	check ("gears: through neutral six frames", neutral == 6 && gear == 4);
	cfg->shift_neutral_ms = 0;

	/* An H shifter */
	c.set_value ("Driving", "Gear1", "KEY_1");
	check ("H: gear 3 held", press (hl::ACTION_GEAR3, true) == 3);
	check ("H: let go, neutral", press (hl::ACTION_GEAR3, false) == 0);
	cfg->shifter_neutral = false;
	press (hl::ACTION_GEAR2, true);
	check ("H: let go, the gear kept", press (hl::ACTION_GEAR2, false) == 2);
	check ("H: neutral pressed", press (hl::ACTION_GEAR_NEUTRAL, true) == 0);
	press (hl::ACTION_GEAR_NEUTRAL, false);
	c.set_value ("Driving", "Gear1", "");
	cfg->shifter_neutral = true;

	/* The agent's */
	g.hold (4);
	check ("agent: its gear", g.gear (frame++) == 4);
	g.hold (-1);
	g.reset ();
	check ("reset: neutral", g.gear (frame++) == 0);
}

/* NASCAR Arcade's view change: linuxloader's ViewChange, in [Driving], V and
 * a controller's Y by default */
static void
test_view_change (void)
{
	const char *section, *key;

	check ("view: its row", hl::Input::row_of (hl::ACTION_VIEW_CHANGE, &section, &key) &&
	       !strcmp (section, "Driving") && !strcmp (key, "ViewChange"));
	check ("view: V and Y by default",
	       hl::Controls::get ().value ("Driving", "ViewChange") == "KEY_V, GC0_BUTTON_Y");
}

int
main (int argc, char **argv)
{
	std::filesystem::path dir = std::filesystem::absolute ("controlstest.dir");
	std::error_code error;

	(void) argc;
	(void) argv;
	std::filesystem::remove_all (dir, error);
	std::filesystem::create_directories (dir, error);

	test_wheel_scale ();

	/* No window: the joysticks' events all the same */
	SDL_SetHint (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
		fprintf (stderr, "SDL_Init: %s\n", SDL_GetError ());
		check ("SDL", false);
	} else {
		test_upgrade (dir.generic_string ());
		test_devices (dir.generic_string ());
		test_gearbox ();
		test_view_change ();
		SDL_Quit ();
	}

	std::filesystem::remove_all (dir, error);
	printf ("controlstest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
