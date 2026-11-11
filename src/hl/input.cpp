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
#include "hl/window.h"
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

/* Each command's row in controls.ini */
static const struct {
	const char *section;
	const char *key;
} actions[NUM_ACTIONS] = {
	{ "Emulator",	"Pause" },
	{ "Emulator",	"SaveState" },
	{ "Emulator",	"LoadState" },
	{ "Emulator",	"StateNewer" },
	{ "Emulator",	"StateOlder" },
	{ "Emulator",	"Menu" },
	{ "Emulator",	"ShowFps" },
	{ "Emulator",	"NextScreen" },
	{ "Emulator",	"Quit" },
	{ "Emulator",	"Back" },
};

static bool is_suspended;

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
	return !is_suspended && Controls::get ().pressed (actions[action].section, actions[action].key);
}

int
Input::event_action (const SDL_Event *event)
{
	unsigned i;

	/* A button pressed with Guide held: the hotkeys' until let go */
	Controls::get ().track (event);
	for (i = 0; i < NUM_ACTIONS; i++)
		if (Controls::get ().pressed_by (actions[i].section, actions[i].key, event))
			return i;
	return -1;
}

void
Input::suspend (bool on)
{
	is_suspended = on;
}

bool
Input::suspended ()
{
	return is_suspended;
}

void
Input::release_all ()
{
	memset (keystate, 0, sizeof (keystate));
}

bool
Input::is_alt_enter (const SDL_Event *event)
{
	return (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
	       event->key.keysym.sym == SDLK_RETURN && (event->key.keysym.mod & KMOD_ALT);
}

int
Input::handle_event (const SDL_Event *event)
{
	if (is_alt_enter (event)) {
		if (event->type == SDL_KEYDOWN && !event->key.repeat)
			Window::toggle_fullscreen ();
		return -1;
	}
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
