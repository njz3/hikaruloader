/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/input.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_INPUT_H__
#define __HL_INPUT_H__

#include "hl/core.h"

namespace hl {

/* What the player can bind on the controls page: the cabinet's switches and
 * controls the machine reads (the main board's, the JVS I/O board's), and the
 * emulator's own commands. */
enum Action {
	ACTION_TEST,
	ACTION_SERVICE,
	ACTION_COIN1,
	ACTION_COIN2,
	ACTION_START1,
	ACTION_UP,
	ACTION_DOWN,
	ACTION_LEFT,
	ACTION_RIGHT,
	ACTION_BUTTON1,
	ACTION_BUTTON2,
	ACTION_BUTTON3,
	ACTION_BUTTON4,
	ACTION_PAUSE,
	ACTION_SAVE_STATE,
	ACTION_LOAD_STATE,
	ACTION_SETTINGS,
	ACTION_SHOW_FPS,
	ACTION_QUIT,

	NUM_ACTIONS
};

/* hikaruloader.ini's old [Controls]: a keyboard key (SDL_Keycode) and a game
 * controller button (SDL_GameControllerButton), SDLK_UNKNOWN and -1 when
 * unbound. Read once, into the new controls.ini (hl::Controls). */
struct Binding {
	int key;
	int button;
};

/* The actions' state: their inputs in controls.ini (hl::Controls), or the Lua
 * shell's and the agent's. */
class Input {
public:
	/* The raw keys the renderers' debug controls read */
	static bool		 key (unsigned key);
	static void		 set_key (unsigned key, bool state);

	/* The name the shell and the agent use, e.g. "TEST" */
	static const char	*action_name (Action action);
	/* The old [Controls]'s default, to tell the bindings the user changed */
	static Binding		 default_binding (Action action);
	/* The action's row in controls.ini, e.g. "Common" and "Test" */
	static bool		 row_of (Action action, const char **section, const char **key);

	/* True while one of the action's inputs is held */
	static bool		 action (Action action);
	/* The action the SDL event presses (not a key repeat), or -1 */
	static int		 event_action (const SDL_Event *event);

	/* Feeds an SDL event to the debug keys and the devices (a controller
	 * plugged in or out). Returns event_action (event). */
	static int		 handle_event (const SDL_Event *event);

	/* The game sees no input while the settings window is open */
	static void		 suspend (bool on);
	/* Forgets the keys held, and the shell's inputs */
	static void		 release_all ();

	/* Holds or releases an action for the Lua shell, as if its input
	 * were */
	static void		 set_action (Action action, bool state);
	/* The action named name ("TEST"), or -1 */
	static int		 find_action (const char *name);
};

} /* namespace hl */

#endif /* __HL_INPUT_H__ */
