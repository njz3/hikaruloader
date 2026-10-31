/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/input.c, Copyright (C) 2011, 2012, Stefano Teso.
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

#include "hl/input.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/ffb.h"

namespace hl {

static const unsigned keymap[] = {
	SDLK_F1, SDLK_F2, SDLK_F3, SDLK_F4, SDLK_F5, SDLK_F6, SDLK_F7, SDLK_F8, SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12,
	SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7, SDLK_8, SDLK_9, SDLK_0,
	SDLK_q, SDLK_w, SDLK_e, SDLK_r, SDLK_t, SDLK_y, SDLK_u, SDLK_i, SDLK_o, SDLK_p,
	SDLK_a, SDLK_s, SDLK_d, SDLK_f, SDLK_g, SDLK_h, SDLK_j, SDLK_k, SDLK_l,
	SDLK_z, SDLK_x, SDLK_c, SDLK_v, SDLK_b, SDLK_n, SDLK_m,
};

static bool keystate[NUMELEM (keymap)];

/* Each action's name for the shell and the agent, its row in controls.ini
 * (linuxloader's names), and the old [Controls]'s default */
static const struct {
	const char *name;
	const char *section;
	const char *key;
	Binding binding;
} actions[NUM_ACTIONS] = {
	{ "TEST",	"Common",	"Test",		{ SDLK_F5,	-1 } },
	{ "SERVICE",	"Common",	"P1_Service",	{ SDLK_F6,	-1 } },
	/* MAME's keys, but where the renderer's debug keys are (1, 2, 3...) */
	{ "COIN1",	"Common",	"P1_Coin",	{ SDLK_5,	-1 } },
	{ "COIN2",	"Common",	"P2_Coin",	{ SDLK_6,	-1 } },
	{ "START1",	"Common",	"P1_Start",	{ SDLK_RETURN,	SDL_CONTROLLER_BUTTON_START } },
	{ "UP",		"Digital",	"P1_Up",	{ SDLK_UP,	SDL_CONTROLLER_BUTTON_DPAD_UP } },
	{ "DOWN",	"Digital",	"P1_Down",	{ SDLK_DOWN,	SDL_CONTROLLER_BUTTON_DPAD_DOWN } },
	{ "LEFT",	"Digital",	"P1_Left",	{ SDLK_LEFT,	SDL_CONTROLLER_BUTTON_DPAD_LEFT } },
	{ "RIGHT",	"Digital",	"P1_Right",	{ SDLK_RIGHT,	SDL_CONTROLLER_BUTTON_DPAD_RIGHT } },
	{ "BUTTON1",	"Digital",	"P1_Button1",	{ SDLK_LCTRL,	SDL_CONTROLLER_BUTTON_A } },
	{ "BUTTON2",	"Digital",	"P1_Button2",	{ SDLK_LALT,	SDL_CONTROLLER_BUTTON_B } },
	{ "BUTTON3",	"Digital",	"P1_Button3",	{ SDLK_LSHIFT,	SDL_CONTROLLER_BUTTON_X } },
	{ "BUTTON4",	"Digital",	"P1_Button4",	{ SDLK_z,	SDL_CONTROLLER_BUTTON_Y } },
	{ "PAUSE",	"Emulator",	"Pause",	{ SDLK_SPACE,	-1 } },
	{ "SAVE_STATE",	"Emulator",	"SaveState",	{ SDLK_F4,	-1 } },
	{ "LOAD_STATE",	"Emulator",	"LoadState",	{ SDLK_F1,	-1 } },
	{ "SETTINGS",	"Emulator",	"Menu",		{ SDLK_TAB,	SDL_CONTROLLER_BUTTON_BACK } },
	{ "SHOW_FPS",	"Emulator",	"ShowFps",	{ SDLK_F12,	-1 } },
	{ "QUIT",	"Common",	"ExitGame",	{ SDLK_F9,	-1 } },
};

/* The shell's and the agent's inputs */
static bool shell_held[NUM_ACTIONS];
static bool suspended;

bool
Input::key (unsigned key)
{
	unsigned i;
	for (i = 0; i < NUMELEM (keymap); i++)
		if (keymap[i] == key)
			return keystate[i];
	return false;
}

void
Input::set_key (unsigned key, bool state)
{
	unsigned i;
	for (i = 0; i < NUMELEM (keymap); i++)
		if (keymap[i] == key)
			keystate[i] = state;
}

const char *
Input::action_name (Action action)
{
	return actions[action].name;
}

Binding
Input::default_binding (Action action)
{
	return actions[action].binding;
}

bool
Input::row_of (Action action, const char **section, const char **key)
{
	if ((unsigned) action >= NUM_ACTIONS)
		return false;
	*section = actions[action].section;
	*key = actions[action].key;
	return true;
}

bool
Input::action (Action action)
{
	if (shell_held[action])
		return true;
	return !suspended && Controls::get ().pressed (actions[action].section, actions[action].key);
}

int
Input::event_action (const SDL_Event *event)
{
	unsigned i;

	for (i = 0; i < NUM_ACTIONS; i++)
		if (Controls::get ().pressed_by (actions[i].section, actions[i].key, event))
			return i;
	return -1;
}

void
Input::set_action (Action action, bool state)
{
	shell_held[action] = state;
}

void
Input::suspend (bool on)
{
	suspended = on;
}

int
Input::find_action (const char *name)
{
	unsigned i;
	for (i = 0; i < NUM_ACTIONS; i++)
		if (!strcasecmp (actions[i].name, name))
			return i;
	return -1;
}

void
Input::release_all ()
{
	memset (keystate, 0, sizeof (keystate));
	memset (shell_held, 0, sizeof (shell_held));
}

int
Input::handle_event (const SDL_Event *event)
{
	switch (event->type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		set_key (event->key.keysym.sym, event->type == SDL_KEYDOWN);
		break;
	case SDL_JOYDEVICEADDED:
	case SDL_JOYDEVICEREMOVED:
		/* The players again; a wheel for the force feedback, perhaps */
		Controls::get ().devices_changed ();
		ForceFeedback::devices_changed ();
		break;
	}
	return event_action (event);
}

} /* namespace hl */
