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

/* The emulator's own commands, rows of controls.ini's [Emulator] (the
 * game's controls are its roles: hl::GameControls) */
enum Action {
	ACTION_PAUSE,
	ACTION_SAVE_STATE,
	ACTION_LOAD_STATE,
	/* The state ACTION_LOAD_STATE will load */
	ACTION_STATE_NEWER,
	ACTION_STATE_OLDER,
	ACTION_SETTINGS,
	ACTION_SHOW_FPS,
	/* The next board's picture, on the cabinets of several boards */
	ACTION_NEXT_SCREEN,
	ACTION_QUIT,
	/* Back to the picker, or out (Escape) */
	ACTION_BACK,

	NUM_ACTIONS
};

/* The commands' state, and the events' */
class Input {
public:
	/* The raw keys the renderers' debug controls read */
	static bool		 key (unsigned key);
	static void		 set_key (unsigned key, bool state);

	/* The command's row in controls.ini, e.g. "Emulator" and "Pause" */
	static bool		 row_of (Action action, const char **section, const char **key);

	/* True while one of the command's inputs is held */
	static bool		 action (Action action);
	/* The command the SDL event presses (not a key repeat), or -1; with a
	 * controller's Guide held, its hotkey's (hl::Controls) */
	static int		 event_action (const SDL_Event *event);

	/* Feeds an SDL event to the debug keys and the devices (a controller
	 * plugged in or out). Returns event_action (event). */
	static int		 handle_event (const SDL_Event *event);
	/* Alt+Enter, which is the window's: neither the game's Start nor the
	 * interface's Enter */
	static bool		 is_alt_enter (const SDL_Event *event);

	/* The game sees no input while the settings window is open */
	static void		 suspend (bool on);
	static bool		 suspended ();
	/* Forgets the keys held */
	static void		 release_all ();
};

} /* namespace hl */

#endif /* __HL_INPUT_H__ */
