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

#ifndef __HL_UI_H__
#define __HL_UI_H__

#include "hl/core.h"
#include "hl/games.h"

namespace ui {

/* The Dear ImGui interface: the game picker, and the Tab settings over a
 * running game. */
class Interface {
public:
	static bool	 init (SDL_Window *window);
	static void	 shutdown ();

	/* Runs the picker until a game is chosen (true, its set name in name)
	 * or the player quits (false). message, when not NULL, is shown on
	 * top: why the last game did not start. */
	static bool	 pick_game (hl::GameList *list, char *name, size_t size,
			            const char *message);

	/* One frame saying the game is being loaded */
	static void	 show_loading (const char *description);

	/* The text in the interface's language */
	static const char *tr (const char *text);

	/* In game. process_event returns true when the event was the
	 * interface's (Tab, or anything while the settings are open). */
	static bool	 process_event (const SDL_Event *event);
	static bool	 settings_open ();
	/* Once after the settings closed: what they changed, applied between
	 * two frames */
	static bool	 take_closed ();
	/* The window's overlay hook: the settings and the frame rate */
	static void	 draw_overlay (int width, int height);
	/* It has something to draw over the frames (the settings, a notice,
	 * the frame rate) */
	static bool	 overlay_wanted ();
};

} /* namespace ui */

#endif /* __HL_UI_H__ */
