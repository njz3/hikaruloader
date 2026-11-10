/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/renderer.h, Copyright (C) 2011, Stefano Teso.
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

#ifndef __HL_WINDOW_H__
#define __HL_WINDOW_H__

#include <vector>

#include "hl/core.h"
#include "hl/aspect.h"

namespace hl {

/* The program's one window and its graphics context, shared by the picker
 * and the machines' renderers: opened by the first caller, kept until the
 * program ends. It applies the [Display] settings, places the machine's
 * picture after the aspect ratio, draws the interface over it and presents
 * the frames. OpenGL 3.1 today; Vulkan 1.3 and Direct3D 11 are to come. */
class Window {
public:
	static int		 open (unsigned width, unsigned height);
	static SDL_Window	*sdl_window ();
	/* The window closed (its close button, Alt+F4) or the program asked
	 * to quit: SDL_QUIT alone does not come while the drawing thread's
	 * hidden window is open */
	static bool		 closing (const SDL_Event *event);
	static void		 drawable_size (int *width, int *height);
	/* Full screen, window size, v-sync, cursor */
	static void		 apply_display ();
	/* Alt+Enter: a window, or the desktop's full screen; for this start,
	 * the setting left as it is */
	static void		 toggle_fullscreen ();

	/* The monitors, in the log. Which one a setting names: a number, the
	 * one Windows gives it (its DISPLAY<n>); INDEX:<n>, SDL's own from 0;
	 * else a part of its name. -1: none, or AUTO. See displays.cpp */
	static void		 log_displays ();
	static int		 find_display (const char *name);
	/* The monitors, in SDL's order, for a list to choose from: what to
	 * write in the setting (their name, or INDEX:<n> for those a name does
	 * not tell apart) and how to show them */
	struct DisplayChoice {
		std::string	 value, label;
	};
	static std::vector<DisplayChoice> display_choices ();
	/* The window on that display, at x, y in a window (the desktop's
	 * coordinates); display -1: in the middle of the first, as at start */
	static void		 place (int display, int x, int y);
	/* After the game's name in the title */
	static void		 set_title_detail (const char *detail);

	/* The machine's picture, 640x480 (854x480 in widescreen): the
	 * window's size and the aspect ratio follow it. From the main thread */
	static void		 set_native_size (int width, int height);
	/* Where the machine's picture goes in the window, after the aspect
	 * ratio's settings (hl/aspect.h); output_rect, what shows of it, the
	 * bars left out, origin at the bottom left */
	static void		 layout (ScreenLayout *l);
	static void		 output_rect (int *x, int *y, int *w, int *h);
	/* In the bound framebuffer, width x height: the bars cleared, the
	 * viewport on the picture (on its frame with bent edges, the final
	 * pass bending it), what falls outside the frame cut by the scissor:
	 * disable GL_SCISSOR_TEST after the draw */
	static void		 set_output_viewport (const ScreenLayout &l, int width, int height);

	/* Called after each frame, before the buffers are swapped: the
	 * interface; and whether it has something to draw (the settings, a
	 * notice, the frame rate): while it has not, a drawing thread shows
	 * its frames itself */
	static void		 set_overlay (void (* hook) (int width, int height));
	static void		 set_overlay_wanted (bool (* hook) (void));
	static bool		 overlay_wanted ();
	/* The end of a machine's frame (Renderer::end_frame ()) */
	static void		 frame_done (const char *message);
	/* The last frame again (paused), or an empty window, with the overlay */
	static void		 present_paused ();
	static void		 present_blank ();

	static float		 fps ();
	/* The machine's picture in the last frame, as the window showed it but
	 * without the bars: RGB, top row first; false when there is none */
	static bool		 read_frame (std::vector<uint8_t> &rgb, int *width, int *height);
	/* The same, to a 24-bit BMP file; false when it cannot be written */
	static bool		 screenshot (const char *path);
	/* "<name> - <fps> fps - Hikaruloader <version>", or the program and its
	 * version alone when name is NULL */
	static void		 set_title (const char *name);
	/* "(Paused)" in the title, after the game's name */
	static void		 set_paused (bool paused);
	/* The machine's frame number, in the title when the settings ask for
	 * it (to tell which frames look wrong) */
	static void		 set_frame (uint64_t frame);
	/* The machine renderer's last debug message */
	static const char	*renderer_message ();

	/* A drawing thread's (THREADED_RENDERING): a second GL context,
	 * sharing the window's textures, buffers, programs and syncs; NULL
	 * when it cannot be made. make_current () makes it current on the
	 * calling thread (NULL: none there) */
	static SDL_GLContext	 create_shared_context ();
	static bool		 make_current (SDL_GLContext context);
	/* That context current with the window itself, to show its frames
	 * there (false: it cannot be, the window's context shows them); its
	 * buffers swapped from that thread (the settings' swap interval); the
	 * frame shown so: frame_done () shows and swaps nothing, keeps
	 * nothing (flush () gives the screenshots and the pause their frame) */
	static bool		 make_current_on_window (SDL_GLContext context);
	static void		 swap_from_thread ();
	static void		 set_presented ();
	static void		 delete_context (SDL_GLContext context);
	/* A frame drawn by that thread but not shown yet: hook (data) puts
	 * it in the window and returns true. The screenshots and the pause
	 * call it first, for the last frame; NULL: none */
	static void		 set_flush (bool (* hook) (void *data), void *data);
};

/* A further window of the machine, drawn with the main one's context: a
 * cabinet's other screens. After [Display] DISPLAY_MODE as the main one is */
class ExtraWindow {
public:
	ExtraWindow (const char *title, int display, int x, int y, int w, int h);
	~ExtraWindow ();

	ExtraWindow (const ExtraWindow &) = delete;
	ExtraWindow &operator= (const ExtraWindow &) = delete;

	void		 set_title (const char *title);
	/* Drawing goes to it, whose size is returned */
	void		 begin (int *width, int *height);
	/* Shown; drawing goes back to the main window */
	void		 end ();
	Uint32		 id () const { return window ? SDL_GetWindowID (window) : 0; }

private:
	SDL_Window	*window;
};

} /* namespace hl */

#endif /* __HL_WINDOW_H__ */
