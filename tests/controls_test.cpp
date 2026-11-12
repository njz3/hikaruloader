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
 * a player would press them; the gear lever (hl::Gearbox) and the view
 * change button, Star Wars: Racer Arcade's levers combined from data/
 * games.json (hl::GameControls), the players 3 and 4, the devices'
 * profiles (hl::DeviceProfiles). Run by ctest, or alone: controlstest
 * <data/games.json>. */

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
#include "hl/deviceprofiles.h"
#include "hl/ffb.h"
#include "hl/gamecontrols.h"
#include "hl/games.h"
#include "hl/gearbox.h"
#include "hl/input.h"
#include "hl/pointers.h"

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
		"Steering_DeadZone = 0\n"
		"Accelerator_DeadZone = 0\n"
		"[Driving]\n"
		"Steering = GC0_AXIS_LEFTX, JOY1_AXIS_0\n"
		"Accelerator = JOY1_AXIS_1_NEGATIVE_SIDE\n"
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
	check ("Start: the game's", !has (a, hl::ACTION_QUIT));
	check ("Start: held", c.pressed ("Player1", "P1_Start"));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();

	/* Guide + Start: quit, the game sees no Start, not even once Guide is
	 * let go first */
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
	a = pump ();
	check ("Guide alone: nothing", a.empty ());
	button (pad, SDL_CONTROLLER_BUTTON_START, true);
	a = pump ();
	check ("Guide + Start: quit", has (a, hl::ACTION_QUIT));
	check ("Guide + Start: no Start for the game", !c.pressed ("Player1", "P1_Start"));
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
	pump ();
	check ("Guide let go first: still no Start", !c.pressed ("Player1", "P1_Start"));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();
	button (pad, SDL_CONTROLLER_BUTTON_START, true);
	pump ();
	check ("Start again: the game's", c.pressed ("Player1", "P1_Start"));
	button (pad, SDL_CONTROLLER_BUTTON_START, false);
	pump ();

	/* The other hotkeys, each with Guide held: the emulator's command, not
	 * the game's button under it */
	static const struct {
		SDL_GameControllerButton button;
		hl::Action hotkey;
		const char *game, *name;
	} hotkeys[] = {
		{ SDL_CONTROLLER_BUTTON_BACK, hl::ACTION_SETTINGS, NULL, "Guide + Back: the settings" },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, hl::ACTION_SAVE_STATE, "P1_Button6", "Guide + R1: save" },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, hl::ACTION_LOAD_STATE, "P1_Button5", "Guide + L1: load" },
		{ SDL_CONTROLLER_BUTTON_B, hl::ACTION_BACK, "P1_Button2", "Guide + B: back" },
		{ SDL_CONTROLLER_BUTTON_X, hl::ACTION_PAUSE, "P1_Button3", "Guide + X: pause" },
	};
	for (const auto &h : hotkeys) {
		button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
		pump ();
		button (pad, h.button, true);
		a = pump ();
		check (h.name, has (a, h.hotkey));
		if (h.game)
			check (h.name, !c.pressed ("Player1", h.game));
		button (pad, h.button, false);
		button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
		pump ();
	}
	/* The card's reader polls it: Guide + Y, not button 4 */
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, true);
	button (pad, SDL_CONTROLLER_BUTTON_Y, true);
	pump ();
	check ("Guide + Y: the card held", c.pressed ("Common", "Card") && !c.pressed ("Player1", "P1_Button4"));
	button (pad, SDL_CONTROLLER_BUTTON_Y, false);
	button (pad, SDL_CONTROLLER_BUTTON_GUIDE, false);
	pump ();

	/* Without Guide: the game's buttons, Back still the settings */
	button (pad, SDL_CONTROLLER_BUTTON_X, true);
	a = pump ();
	check ("X alone: button 3", !has (a, hl::ACTION_PAUSE) && c.pressed ("Player1", "P1_Button3"));
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
	check ("wheel: the whole turn", near (c.axis ("Driving", "Steering"), 3277 / 32767.0));
	cfg->wheel_rotation = 270;
	check ("wheel: 270 of 900", near (c.axis ("Driving", "Steering"), 3277 / 32767.0 * 900 / 270));
	SDL_JoystickSetVirtualAxis (wheel, 0, -16384);
	pump ();
	check ("wheel: held at the left end", c.axis ("Driving", "Steering") == -1.0);
	SDL_JoystickSetVirtualAxis (wheel, 0, 0);
	SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTX, 6554);
	pump ();
	check ("pad's stick: its own way", near (c.axis ("Driving", "Steering"), 6554 / 32767.0));
	cfg->wheel_rotation = 0;
	SDL_JoystickSetVirtualAxis (pad, SDL_CONTROLLER_AXIS_LEFTX, 0);

	SDL_JoystickClose (pad);
	SDL_JoystickClose (wheel);
	SDL_JoystickDetachVirtual (wheel_index);
	SDL_JoystickDetachVirtual (pad_index);
}

/* Star Wars: Racer Arcade's levers, combined from the accelerator and the
 * wheel by its games.json places ("combined"), or an axis each */
static void
test_levers (hl::GameList *list)
{
	hl::GameControls &g = hl::GameControls::get ();
	hl::Controls &c = hl::Controls::get ();
	hl::Config *cfg = hl::Config::get ();
	uint16_t left = 0, right = 0;

	if (!list) {
		check ("levers: the game list", false);
		return;
	}
	g.set_game (list, "swracer");
	check ("levers: a combined pair", g.combined ());
	cfg->twin_levers = hl::TWIN_LEVERS_COMBINED;
	c.hold ("Driving", "Accelerator", 1.0);
	c.hold ("Driving", "Steering", 0.0);
	g.analog ("an", 0, &left);
	g.analog ("an", 1, &right);
	check ("levers: the accelerator's thrust on both", left == 0x4000 && right == 0x4000);
	c.hold ("Driving", "Steering", 0.5);
	g.analog ("an", 0, &left);
	g.analog ("an", 1, &right);
	check ("levers: to the right, the right one eased", left == 0x4000 && right == 0x8000);
	c.hold ("Driving", "Accelerator", 0.0);
	g.analog ("an", 0, &left);
	check ("levers: no accelerator, no thrust", left == 0xC000);
	cfg->twin_levers = hl::TWIN_LEVERS_ORIGINAL;
	c.hold ("Driving", "Accelerator", 1.0);
	c.hold ("Driving", "LeftLever", 0.5);
	g.analog ("an", 0, &left);
	g.analog ("an", 1, &right);
	check ("levers: an axis each", left == 0x8000 && right == 0xC000);
	cfg->twin_levers = hl::TWIN_LEVERS_COMBINED;
	c.release_held ("Driving", NULL);
	g.set_game (NULL, NULL);
}

/* The gear lever, as sm2-emu's: the paddles a gear each press, through
 * neutral between two gears SHIFT_NEUTRAL_MS; an H shifter's positions,
 * its lever in none of them in neutral (SHIFTER_NEUTRAL); the agent's gear
 * over the controls; looked at once a frame */
static void
test_gearbox (void)
{
	hl::Gearbox &g = hl::Gearbox::get ();
	hl::Config *cfg = hl::Config::get ();
	hl::Controls &c = hl::Controls::get ();
	const std::vector<int> four = { 0, 1, 2, 3, 4 }, trucker = { -1, 0, 1, 2 };
	uint64_t frame = 1;
	unsigned gear, neutral;
	int i;

	/* A row held (or let go) as a player would, then the frame's gear */
	auto press = [&] (const char *key, bool on, const std::vector<int> &gears = { 0, 1, 2, 3, 4 }) {
		if (on)
			c.hold ("Driving", key, 1.0);
		else
			c.release_held ("Driving", key);
		return g.gear (frame++, gears);
	};

	g.reset ();
	cfg->shift_neutral_ms = 0;
	cfg->shifter_neutral = true;
	check ("gears: neutral at first", g.gear (frame++, four) == 0);
	check ("gears: a gear up, 1", press ("ShiftUp", true) == 1);
	check ("gears: held, still 1", press ("ShiftUp", true) == 1);
	press ("ShiftUp", false);
	for (i = 0; i < 5; i++) {
		press ("ShiftUp", true);
		press ("ShiftUp", false);
	}
	check ("gears: 4 at most", g.gear (frame++, four) == 4);
	check ("gears: a gear down, 3", press ("ShiftDown", true) == 3);
	c.release_held ("Driving", "ShiftDown");
	check ("gears: a frame looked at once", g.gear (frame - 1, four) == 3);

	/* 100 ms through neutral: six frames */
	cfg->shift_neutral_ms = 100;
	gear = press ("ShiftUp", true);
	c.release_held ("Driving", "ShiftUp");
	for (neutral = gear ? 0 : 1; neutral < 20 && (gear = g.gear (frame++, four)) == 0; neutral++)
		;
	check ("gears: through neutral six frames", neutral == 6 && gear == 4);
	cfg->shift_neutral_ms = 0;

	/* An H shifter bound and never moved: the paddles' gears stay (a
	 * cabinet's controls.ini for both panels) */
	c.set_value ("Driving", "Gear1", "KEY_1");
	g.reset ();
	press ("ShiftUp", true);
	press ("ShiftUp", false);
	press ("ShiftUp", true);
	check ("H unused: the paddles' 2", press ("ShiftUp", false) == 2);
	press ("ShiftDown", true);
	check ("H unused: the paddles' 1", press ("ShiftDown", false) == 1);

	/* An H shifter */
	check ("H: gear 3 held", press ("Gear3", true) == 3);
	check ("H: let go, neutral", press ("Gear3", false) == 0);
	press ("ShiftUp", true);
	check ("H then the paddles: 1 from neutral", press ("ShiftUp", false) == 1);
	check ("H then the paddles: 1 kept", g.gear (frame++, four) == 1);
	cfg->shifter_neutral = false;
	press ("Gear2", true);
	check ("H: let go, the gear kept", press ("Gear2", false) == 2);
	check ("H: neutral pressed", press ("GearNeutral", true) == 0);
	press ("GearNeutral", false);
	c.set_value ("Driving", "Gear1", "");
	cfg->shifter_neutral = true;

	/* 18 Wheeler's: reverse below neutral, an H shifter's reverse */
	g.reset ();
	check ("reverse: neutral at first", g.gear (frame++, trucker) == 0);
	check ("reverse: a gear down", press ("ShiftDown", true, trucker) == -1);
	press ("ShiftDown", false, trucker);
	check ("reverse: its position held", press ("GearReverse", true, trucker) == -1);
	press ("GearReverse", false, trucker);

	/* The agent's */
	g.hold (4);
	check ("agent: its gear", g.gear (frame++, four) == 4);
	g.hold (hl::Gearbox::NO_GEAR);
	g.reset ();
	check ("reset: neutral", g.gear (frame++, four) == 0);
}

/* NASCAR Arcade's view change: linuxloader's ViewChange, in [Driving], V and
 * a controller's Y by default */
static void
test_view_change (void)
{
	const char *section, *key;

	const hl::Controls::RowDef *row = hl::Controls::row_def ("ViewChange");

	section = row ? row->section : "";
	key = row ? row->key : "";
	check ("view: its row", !strcmp (section, "Driving") && !strcmp (key, "ViewChange"));
	check ("view: V and Y by default",
	       hl::Controls::get ().value ("Driving", "ViewChange") == "KEY_V, GC0_BUTTON_Y");
}

/* Player 2: MAME's keys, the second controller; players 3 and 4: the third
 * and fourth controllers */
static void
test_players (void)
{
	hl::Controls &c = hl::Controls::get ();

	check ("player 2: start", c.value ("Player2", "P2_Start") == "KEY_2, GC1_BUTTON_START");
	check ("player 2: up", c.value ("Player2", "P2_Up").rfind ("KEY_R, GC1_BUTTON_DPUP", 0) == 0);
	check ("player 2: button 3", c.value ("Player2", "P2_Button3") == "KEY_Q, GC1_BUTTON_X");
	check ("player 3: start", c.value ("Player3", "P3_Start") == "GC2_BUTTON_START");
	check ("player 4: button 6", c.value ("Player4", "P4_Button6") == "GC3_BUTTON_RIGHTSHOULDER");
	check ("coin 3: 7", c.value ("Common", "Coin3") == "KEY_7");
}

/* A mouse or a light gun, each its own */
static void
test_pointers (void)
{
	hl::Controls &c = hl::Controls::get ();

	hl::Pointers::inject (0, { 0.5, -0.25, false, 1u << hl::Pointers::LEFT });
	hl::Pointers::inject (1, { -0.75, 0.125, true, 1u << hl::Pointers::RIGHT });
	check ("mouse 0: its X", fabs (c.axis ("Pointer", "P1_GunX") - 0.5) < 1e-6);
	check ("mouse 0: its Y", fabs (c.axis ("Pointer", "P1_GunY") + 0.25) < 1e-6);
	check ("mouse 0: left, the trigger", c.pressed ("Pointer", "P1_Trigger"));
	check ("mouse 0: not right", !c.pressed ("Pointer", "P1_Fire2"));
	check ("mouse 0: on the picture", !c.pressed ("Pointer", "P1_Reload"));
	check ("mouse 1: its X", fabs (c.axis ("Pointer", "P2_GunX") + 0.75) < 1e-6);
	check ("mouse 1: right, the second button", c.pressed ("Pointer", "P2_Fire2"));
	check ("mouse 1: not left", !c.pressed ("Pointer", "P2_Trigger"));
	check ("mouse 1: off the picture", c.pressed ("Pointer", "P2_Reload"));
	check ("mouse 0: the stick too", fabs (c.axis ("Pointer", "P1_StickX") - 0.5) < 1e-6);
	check ("mouse 0: the trigger again", c.pressed ("Pointer", "P1_Trigger"));
	c.aim ("Pointer", "P2_GunY");
	hl::Pointers::inject (1, { -0.75, 0.25, true, 1u << hl::Pointers::RIGHT });
	check ("a sight: where the mouse moved it", fabs (c.aim ("Pointer", "P2_GunY") - 0.25) < 1e-6);
	check ("a sight: it stays", fabs (c.aim ("Pointer", "P2_GunY") - 0.25) < 1e-3);
	check ("a wheel's default at its number", hl::Controls::default_for ("GC0_AXIS_LEFTX, JOY0_AXIS_0", 1, false) ==
	                                          "GC0_AXIS_LEFTX, JOY1_AXIS_0");
	check ("the first joystick's default as it is", hl::Controls::default_for ("JOY0_AXIS_0", 0, false) == "JOY0_AXIS_0");
	check ("a game controller's default as it is", hl::Controls::default_for ("JOY0_AXIS_0", 1, true) == "JOY0_AXIS_0");
	check ("mouse 0: its inputs", hl::Controls::owns (hl::Controls::MOUSE_PLAYER, "MOUSE0_X"));
	check ("mouse 0: not mouse 1's", !hl::Controls::owns (hl::Controls::MOUSE_PLAYER, "MOUSE1_X"));
	check ("a controller: not a mouse's", !hl::Controls::owns (0, "MOUSE0_X"));

	std::string token;

	hl::Pointers::inject (0, { 0.0, 0.0, false, 1u << hl::Pointers::LEFT });
	c.capture (hl::Controls::MOUSE_PLAYER, hl::Controls::CAPTURE_BUTTON);
	check ("mouse capture: held at the start, nothing", !c.capture_step (token));
	check ("mouse capture: nothing new", !c.capture_step (token));
	hl::Pointers::inject (0, { 0.0, 0.0, false, (1u << hl::Pointers::LEFT) | (1u << hl::Pointers::RIGHT) });
	check ("mouse capture: right pressed", c.capture_step (token) && token == "MOUSE0_BUTTON_RIGHT");
	hl::Pointers::inject (1, { 0.0, 0.0, false, 0 });
	c.capture (hl::Controls::MOUSE_PLAYER + 1, hl::Controls::CAPTURE_STEERING);
	c.capture_step (token);
	hl::Pointers::inject (1, { 0.05, 0.5, false, 0 });
	check ("mouse capture: the axis it moved along", c.capture_step (token) && token == "MOUSE1_Y");
	hl::Pointers::inject (0, { 0.0, 0.0, false, 0 });
	hl::Pointers::inject (1, { 0.0, 0.0, false, 0 });
}

/* A device's profile: its inputs to a player's and back, found by its name
 * or USB ids, put on its player's bindings the first time, kept then, the
 * device's defaults, saved from its bindings */
static void
test_profiles (const std::string &dir)
{
	using hl::DeviceProfiles;
	hl::Controls &c = hl::Controls::get ();
	SDL_VirtualJoystickDesc desc;
	SDL_Joystick *wheel;
	int index, p = -1;
	std::string joy, path;

	check ("profile: to player 2", DeviceProfiles::to_player ("JOY_AXIS_0, GC_BUTTON_GUIDE + GC_BUTTON_A", 1) ==
	                               "JOY1_AXIS_0, GC1_BUTTON_GUIDE + GC1_BUTTON_A");
	check ("profile: a key as it is", DeviceProfiles::to_player ("KEY_A", 2) == "KEY_A");
	check ("profile: back from player 2", DeviceProfiles::from_player ("JOY1_AXIS_0,GC1_BUTTON_START", 1) ==
	                                      "JOY_AXIS_0, GC_BUTTON_START");
	check ("profile: another player's as it is", DeviceProfiles::from_player ("JOY0_AXIS_0", 1) == "JOY0_AXIS_0");

	/* A wheel with its own name and USB ids, its profile in devices/ */
	std::filesystem::create_directories (dir + "/devices");
	write_text (dir + "/devices/wheel.ini",
		"[Device]\nName = controlstest wheel\n"
		"[Driving]\nSteering = JOY_AXIS_0\nAccelerator = JOY_AXIS_2_INVERTED\nShiftUp = JOY_BUTTON_7\n"
		"[Player1]\nP1_Start = JOY_BUTTON_0\n");
	write_text (dir + "/profiles.ini", "[Config]\nSteering_DeadZone = 0\nAccelerator_DeadZone = 0\n");
	memset (&desc, 0, sizeof (desc));
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_WHEEL;
	desc.naxes = 3;
	desc.nbuttons = 8;
	desc.vendor_id = 0x1234;
	desc.product_id = 0x5678;
	desc.name = "controlstest wheel";
	index = SDL_JoystickAttachVirtualEx (&desc);
	wheel = index >= 0 ? SDL_JoystickOpen (index) : NULL;
	check ("profile: the wheel", wheel != NULL);
	if (!wheel)
		return;
	SDL_JoystickSetVirtualAxis (wheel, 2, -32767);
	pump ();
	c.load ((dir + "/profiles.ini").c_str ());
	for (const auto &d : c.devices ())
		if (d.name == "controlstest wheel") {
			p = d.player;
			check ("profile: found by its name", !d.profile.empty ());
		}
	check ("profile: its player", p >= 0);
	if (p < 0)
		return;
	joy = "JOY" + std::to_string (p) + "_";
	check ("profile: put on its steering", c.value ("Driving", "Steering").find (joy + "AXIS_0") != std::string::npos);
	check ("profile: its start", c.value ("Player1", "P1_Start").find (joy + "BUTTON_0") != std::string::npos);
	check ("profile: its player's other defaults gone", c.value ("Player1", "P1_Up").find (joy) == std::string::npos);
	check ("profile: the keys kept", c.value ("Player1", "P1_Start").rfind ("KEY_Return", 0) == 0);
	check ("profile: done once", !c.value ("ControllerGUIDs", ("P" + std::to_string (p + 1) + "_Profiled").c_str ()).empty ());
	SDL_JoystickSetVirtualButton (wheel, 7, 1);
	pump ();
	check ("profile: its paddle", c.pressed ("Driving", "ShiftUp"));
	SDL_JoystickSetVirtualButton (wheel, 7, 0);
	pump ();
	check ("profile: its pedal at rest", c.pedal ("Driving", "Accelerator") > 0.99);
	check ("profile: the device's default", c.device_default ("Driving", "ShiftUp", p, false) == joy + "BUTTON_7");

	/* The user's change kept at the next start */
	c.set_value ("Driving", "ShiftDown", "KEY_PageDown, " + joy + "BUTTON_6");
	c.load ((dir + "/profiles.ini").c_str ());
	check ("profile: not put again", c.value ("Driving", "ShiftDown").find (joy + "BUTTON_6") != std::string::npos);

	/* Saved: its bindings by its own inputs, found by its USB ids first */
	path = c.save_profile (p);
	check ("profile: saved", !path.empty () && std::filesystem::exists (path));
	{
		std::ifstream in (path);
		std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char> ());
		check ("profile: its name", text.find ("Name = controlstest wheel") != std::string::npos);
		check ("profile: its USB ids", text.find ("VidPid = 1234:5678") != std::string::npos);
		check ("profile: the user's change", text.find ("ShiftDown = JOY_BUTTON_6") != std::string::npos);
		check ("profile: no player's number", text.find (joy) == std::string::npos);
	}
	const hl::DeviceProfile *found = DeviceProfiles::get ().match ("another name", 0x1234, 0x5678);
	check ("profile: found by its USB ids", found && found->user && found->file == path);

	SDL_JoystickClose (wheel);
	SDL_JoystickDetachVirtual (index);
	pump ();
}

int
main (int argc, char **argv)
{
	std::filesystem::path dir = std::filesystem::absolute ("controlstest.dir");
	std::error_code error;

	hl::GameList *list = argc > 1 ? hl::GameList::load (argv[1]) : NULL;

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
		test_levers (list);
		test_gearbox ();
		test_view_change ();
		test_players ();
		test_pointers ();
		test_profiles (dir.generic_string ());
		/* Before any effect, as the Controls tab asks; what is plugged
		 * in here, for whoever reads the output */
		for (const std::string &name : hl::ForceFeedback::device_names ())
			printf ("controlstest: force feedback device: %s\n", name.c_str ());
		SDL_Quit ();
	}

	std::filesystem::remove_all (dir, error);
	printf ("controlstest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
