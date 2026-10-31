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
	static void		 drawable_size (int *width, int *height);
	/* Full screen, window size, v-sync, cursor */
	static void		 apply_display ();

	/* The machine's picture, 640x480 (854x480 in widescreen): the
	 * window's size and the aspect ratio follow it. From the main thread */
	static void		 set_native_size (int width, int height);
	/* Where the machine's picture goes after the aspect ratio setting and
	 * the screen's shape; set_output_viewport clears the bars and sets the
	 * viewport there */
	static void		 output_rect (int *x, int *y, int *w, int *h);
	static void		 set_output_viewport ();

	/* Called after each frame, before the buffers are swapped: the
	 * interface */
	static void		 set_overlay (void (* hook) (int width, int height));
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
	static void		 delete_context (SDL_GLContext context);
	/* A frame drawn by that thread but not shown yet: hook (data) puts
	 * it in the window and returns true. The screenshots and the pause
	 * call it first, for the last frame; NULL: none */
	static void		 set_flush (bool (* hook) (void *data), void *data);
};

} /* namespace hl */

#endif /* __HL_WINDOW_H__ */
