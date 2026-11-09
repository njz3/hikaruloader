/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/renderer.c, Copyright (C) 2011, Stefano Teso.
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

#include <algorithm>
#include <vector>

#include "hl/window.h"
#include "hl/bezel.h"
#include "hl/renderer.h"
#include "hl/config.h"

namespace hl {

/* TODO change API to avoid leaking the SDL window and GL context. */

static SDL_Window *shared_window = NULL;
static SDL_GLContext shared_context = NULL;
/* A hidden window, the drawing thread's context current on it: not two
 * threads on the window's own surface */
static SDL_Window *thread_window = NULL;

static void (* overlay_hook) (int width, int height) = NULL;
static bool (* flush_hook) (void *data) = NULL;
static void *flush_data = NULL;
static float last_fps = 0.0f;
static void refresh_frame_title (void);
/* The machine's picture: 640x480, 854x480 in widescreen */
static int native_w = 640, native_h = 480;
static char last_message[256];

/* A cabinet's other screens' windows, and where the main one goes (display
 * -1: in the middle of the first) */
static std::vector<const ExtraWindow *> extras;
static int place_display = -1, place_x, place_y;
static bool place_changed;

/* The last frame, redrawn under the settings while the game is paused */
static struct {
	GLuint program, vao, texture;
	int width, height;
	bool valid;
} last_frame;

SDL_Window *
Window::sdl_window ()
{
	return shared_window;
}

bool
Window::closing (const SDL_Event *event)
{
	if (event->type == SDL_QUIT)
		return true;
	if (event->type != SDL_WINDOWEVENT || event->window.event != SDL_WINDOWEVENT_CLOSE)
		return false;
	for (const ExtraWindow *extra : extras)
		if (event->window.windowID == extra->id ())
			return true;
	return shared_window && event->window.windowID == SDL_GetWindowID (shared_window);
}

void
Window::set_overlay (void (* hook) (int width, int height))
{
	overlay_hook = hook;
}

static bool (* overlay_wanted_hook) (void) = NULL;
/* This frame shown by the drawing thread */
static bool presented = false;

void
Window::set_overlay_wanted (bool (* hook) (void))
{
	overlay_wanted_hook = hook;
}

bool
Window::overlay_wanted ()
{
	return overlay_wanted_hook && overlay_wanted_hook ();
}

void
Window::set_presented ()
{
	presented = true;
}

float
Window::fps ()
{
	return last_fps;
}

void
Window::drawable_size (int *width, int *height)
{
	SDL_GL_GetDrawableSize (shared_window, width, height);
}

void
Window::set_native_size (int width, int height)
{
	if (width == native_w && height == native_h)
		return;
	native_w = width;
	native_h = height;
	apply_display ();
}

void
Window::layout (ScreenLayout *l)
{
	int dw, dh;

	Window::drawable_size (&dw, &dh);
	/* In the game's bezel picture's screen under BEZEL = IMAGE */
	Bezel::layout (l, dw, dh, native_w, native_h);
}

void
Window::output_rect (int *x, int *y, int *w, int *h)
{
	ScreenLayout l;
	int x1, y1;

	/* The picture where it is in the frame: no bars, nothing cut */
	Window::layout (&l);
	*x = MAX2 (l.frame[0], l.image[0]);
	*y = MAX2 (l.frame[1], l.image[1]);
	x1 = MIN2 (l.frame[0] + l.frame[2], l.image[0] + l.image[2]);
	y1 = MIN2 (l.frame[1] + l.frame[3], l.image[1] + l.image[3]);
	*w = MAX2 (0, x1 - *x);
	*h = MAX2 (0, y1 - *y);
}

void
Window::set_output_viewport (const ScreenLayout &l, int width, int height)
{
	const int *r = l.barrel_axis >= 0 ? l.frame : l.image;

	/* Black bars around the picture */
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, width, height);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glViewport (r[0], r[1], r[2], r[3]);
	/* A crop: the picture larger than the frame, cut to it */
	glScissor (l.frame[0], l.frame[1], l.frame[2], l.frame[3]);
	glEnable (GL_SCISSOR_TEST);
}

void
Window::apply_display ()
{
	static int applied_mode = -1, applied_scale = -1, applied_width = -1;
	hl::Config *cfg = hl::Config::get ();

	if (!shared_window)
		return;

	if (place_changed) {
		place_changed = false;
		if (cfg->display_mode != hl::DISPLAY_WINDOW) {
			SDL_SetWindowFullscreen (shared_window, 0);
			applied_mode = -1;
		}
		if (place_display < 0)
			SDL_SetWindowPosition (shared_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		else
			SDL_SetWindowPosition (shared_window, place_x, place_y);
	}
	if (cfg->display_mode != applied_mode) {
		Uint32 flags = cfg->display_mode == hl::DISPLAY_FULLSCREEN ? SDL_WINDOW_FULLSCREEN :
		               cfg->display_mode == hl::DISPLAY_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
		if (flags == SDL_WINDOW_FULLSCREEN) {
			/* The desktop's own mode, not a mode change */
			SDL_DisplayMode mode;
			if (!SDL_GetDesktopDisplayMode (SDL_GetWindowDisplayIndex (shared_window), &mode))
				SDL_SetWindowDisplayMode (shared_window, &mode);
		}
		SDL_SetWindowFullscreen (shared_window, flags);
		applied_mode = cfg->display_mode;
		applied_scale = -1;
	}
	if (cfg->display_mode == hl::DISPLAY_WINDOW &&
	    (cfg->window_scale != applied_scale || native_w != applied_width)) {
		SDL_SetWindowSize (shared_window, native_w * cfg->window_scale,
		                   native_h * cfg->window_scale);
		if (place_display < 0)
			SDL_SetWindowPosition (shared_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		else
			SDL_SetWindowPosition (shared_window, place_x, place_y);
		applied_scale = cfg->window_scale;
		applied_width = native_w;
	}

	SDL_GL_SetSwapInterval (cfg->vsync ? 1 : 0);
	SDL_ShowCursor (cfg->hide_cursor ? SDL_DISABLE : SDL_ENABLE);
}

void
Window::place (int display, int x, int y)
{
	if (display == place_display && x == place_x && y == place_y)
		return;
	place_display = display;
	place_x = x;
	place_y = y;
	place_changed = true;
	apply_display ();
}

ExtraWindow::ExtraWindow (const char *title, int display, int x, int y, int w, int h)
{
	Config *cfg = Config::get ();

	/* Several full screen windows: the one losing the focus stays */
	SDL_SetHint (SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	window = SDL_CreateWindow (title, x, y, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!window) {
		HL_ERROR ("could not create a window for a screen: '%s'", SDL_GetError ());
		return;
	}
	if (cfg->display_mode != DISPLAY_WINDOW) {
		SDL_DisplayMode mode;

		if (cfg->display_mode == DISPLAY_FULLSCREEN && !SDL_GetDesktopDisplayMode (display, &mode))
			SDL_SetWindowDisplayMode (window, &mode);
		SDL_SetWindowFullscreen (window, cfg->display_mode == DISPLAY_FULLSCREEN ? SDL_WINDOW_FULLSCREEN
		                                                                         : SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	/* Only the main window's swap waits for the screen, else one frame
	 * would take as many refreshes as there are windows */
	SDL_GL_MakeCurrent (window, shared_context);
	SDL_GL_SetSwapInterval (0);
	SDL_GL_MakeCurrent (shared_window, shared_context);
	extras.push_back (this);
	HL_PRINT ("window: a screen's window on display %d", display);
}

ExtraWindow::~ExtraWindow ()
{
	extras.erase (std::remove (extras.begin (), extras.end (), this), extras.end ());
	if (window)
		SDL_DestroyWindow (window);
	if (extras.empty ())
		SDL_ResetHint (SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS);
}

void
ExtraWindow::set_title (const char *title)
{
	if (window)
		SDL_SetWindowTitle (window, title);
}

void
ExtraWindow::begin (int *width, int *height)
{
	*width = *height = 1;
	if (window) {
		SDL_GL_MakeCurrent (window, shared_context);
		SDL_GL_GetDrawableSize (window, width, height);
	}
}

void
ExtraWindow::end ()
{
	if (window) {
		SDL_GL_SwapWindow (window);
		SDL_GL_MakeCurrent (shared_window, shared_context);
	}
}

static void
capture_last_frame (void)
{
	int dw, dh;

	Window::drawable_size (&dw, &dh);
	if (!last_frame.texture)
		glGenTextures (1, &last_frame.texture);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, last_frame.texture);
	if (dw != last_frame.width || dh != last_frame.height) {
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		last_frame.width = dw;
		last_frame.height = dh;
	}
	glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
	glReadBuffer (GL_BACK);
	glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 0, 0, dw, dh);
	last_frame.valid = true;
	gl::clear_errors ();
}

void
Window::set_flush (bool (* hook) (void *data), void *data)
{
	flush_hook = hook;
	flush_data = data;
}

/* The drawing thread's last frame, if not shown yet, kept as the last one */
static void
flush (void)
{
	if (flush_hook && flush_hook (flush_data))
		capture_last_frame ();
}

bool
Window::read_frame (std::vector<uint8_t> &rgb, int *width, int *height)
{
	std::vector<uint8_t> rgba;
	int x, y, w, h, row, col;

	flush ();
	if (!last_frame.valid)
		return false;

	/* The picture, without the bars, in the frame kept */
	output_rect (&x, &y, &w, &h);
	x = MIN2 (x, last_frame.width);
	y = MIN2 (y, last_frame.height);
	w = MIN2 (w, last_frame.width - x);
	h = MIN2 (h, last_frame.height - y);
	if (w <= 0 || h <= 0)
		return false;

	rgba.resize ((size_t) last_frame.width * last_frame.height * 4);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, last_frame.texture);
	glPixelStorei (GL_PACK_ALIGNMENT, 4);
	glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
	gl::clear_errors ();

	/* OpenGL's rows go up */
	rgb.resize ((size_t) w * h * 3);
	for (row = 0; row < h; row++) {
		const uint8_t *src = &rgba[((size_t) (y + h - 1 - row) * last_frame.width + x) * 4];
		uint8_t *dst = &rgb[(size_t) row * w * 3];
		for (col = 0; col < w; col++, src += 4, dst += 3) {
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
	}
	*width = w;
	*height = h;
	return true;
}

bool
Window::screenshot (const char *path)
{
	std::vector<uint8_t> rgb;
	SDL_Surface *surface;
	int w, h, row;
	bool ok;

	if (!read_frame (rgb, &w, &h))
		return false;
	surface = SDL_CreateRGBSurfaceWithFormat (0, w, h, 24, SDL_PIXELFORMAT_RGB24);
	if (!surface)
		return false;
	for (row = 0; row < h; row++)
		memcpy ((uint8_t *) surface->pixels + row * surface->pitch,
		        &rgb[(size_t) row * w * 3], (size_t) w * 3);
	ok = SDL_SaveBMP (surface, path) == 0;
	SDL_FreeSurface (surface);
	return ok;
}

static const char *blit_vs_source =
"#version 140\n"
"out vec2 p_texcoords;\n"
"void main (void)\n"
"{\n"
"	/* A triangle covering the viewport */\n"
"	vec2 p = vec2 ((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);\n"
"	p_texcoords = p * 0.5 + 0.5;\n"
"	gl_Position = vec4 (p, 0.0, 1.0);\n"
"}\n";

static const char *blit_fs_source =
"#version 140\n"
"uniform sampler2D u_texture;\n"
"in vec2 p_texcoords;\n"
"out vec4 frag_color;\n"
"void main (void)\n"
"{\n"
"	frag_color = texture (u_texture, p_texcoords);\n"
"}\n";

/* The GL errors the window's own drawing left (the interface's, the paused
 * picture's), logged and cleared: the machine's renderer checks for errors
 * at the start of its frame, and stopped on them (BRAVEFF, frame 1977, the
 * settings opened: "invalid enum") */
static void
clear_gl_errors (const char *what)
{
	static unsigned logged;
	GLenum err;
	int n = 0;

	while ((err = glGetError ()) != GL_NO_ERROR && n++ < 16)
		if (logged < 20) {
			logged++;
			HL_ERROR ("GL error 0x%04X after %s (ignored)", err, what);
		}
}

static void
present (void)
{
	int dw, dh;

	Window::drawable_size (&dw, &dh);
	if (overlay_hook) {
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		glViewport (0, 0, dw, dh);
		overlay_hook (dw, dh);
		clear_gl_errors ("the interface");
	}
	SDL_GL_SwapWindow (shared_window);
}

void
Window::present_paused ()
{
	int dw, dh;

	flush ();
	refresh_frame_title ();
	Window::drawable_size (&dw, &dh);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, dw, dh);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);

	if (last_frame.valid) {
		if (!last_frame.program) {
			last_frame.program = gl::compile_program (blit_vs_source, blit_fs_source);
			glGenVertexArrays (1, &last_frame.vao);
		}
		glDisable (GL_DEPTH_TEST);
		glDisable (GL_BLEND);
		glDisable (GL_CULL_FACE);
		glUseProgram (last_frame.program);
		glUniform1i (glGetUniformLocation (last_frame.program, "u_texture"), 0);
		glBindVertexArray (last_frame.vao);
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, last_frame.texture);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		glBindVertexArray (0);
		glUseProgram (0);
	}
	clear_gl_errors ("the paused picture");
	present ();
}

void
Window::present_blank ()
{
	int dw, dh;

	Window::drawable_size (&dw, &dh);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, dw, dh);
	glClearColor (0.06f, 0.07f, 0.09f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	present ();
}

/* The frame rate, averaged over the frames of the last half second */
static struct {
	uint32_t start;
	unsigned frames;
} fps_window;

/* The game part of the window's title: "<title> [<set>]", or empty */
static char title_name[256];
/* After it: the board shown, on a cabinet of several */
static char title_detail[64];
/* The machine's frame, the one the title shows, and when it changed */
static uint64_t title_frame, title_frame_shown;
static uint32_t title_frame_ticks;
static bool title_paused;

static void
update_title (void)
{
	char title[512];
	char paused[96];

	if (!shared_window)
		return;
	snprintf (paused, sizeof (paused), "%s%s", title_detail, title_paused ? " (Paused)" : "");
	title_frame_shown = title_frame;
	if (title_name[0] && Config::get ()->title_frame)
		snprintf (title, sizeof (title), "%s%s - frame %llu - %.1f fps - Hikaruloader %s",
		          title_name, paused, (unsigned long long) title_frame, last_fps,
		          HL_VERSION_FULL);
	else if (title_name[0])
		snprintf (title, sizeof (title), "%s%s - %.1f fps - Hikaruloader %s",
		          title_name, paused, last_fps, HL_VERSION_FULL);
	else
		snprintf (title, sizeof (title), "Hikaruloader %s", HL_VERSION_FULL);
	SDL_SetWindowTitle (shared_window, title);
}

/* What Renderer::end_frame () does once the machine's frame is drawn: keeps
 * it for the pause, draws the overlay, presents it, counts the frame rate */
void
Window::frame_done (const char *message)
{
	uint32_t now;

	/* Shown by the drawing thread; else kept for the pause, before the
	 * settings are drawn over it, and shown */
	if (presented)
		presented = false;
	else {
		if (overlay_hook)
			capture_last_frame ();
		present ();
	}

	now = SDL_GetTicks ();
	fps_window.frames++;
	if (!fps_window.start)
		fps_window.start = now;
	else if (now - fps_window.start >= 500) {
		last_fps = fps_window.frames * 1000.0f / (now - fps_window.start);
		fps_window.start = now;
		fps_window.frames = 0;
		update_title ();
	}

	/* The renderer's debug flags, shown with the frame rate (F12) */
	snprintf (last_message, sizeof (last_message), "%s", message);
}

void
Window::set_paused (bool paused)
{
	if (paused == title_paused)
		return;
	title_paused = paused;
	update_title ();
}

void
Window::set_title_detail (const char *detail)
{
	snprintf (title_detail, sizeof (title_detail), "%s", detail ? detail : "");
	update_title ();
}

void
Window::set_title (const char *name)
{
	snprintf (title_name, sizeof (title_name), "%s", name ? name : "");
	update_title ();
}

/* Paused, the exact frame: the title may lag 100 ms behind while running */
static void
refresh_frame_title (void)
{
	if (Config::get ()->title_frame && title_frame_shown != title_frame)
		update_title ();
}

void
Window::set_frame (uint64_t frame)
{
	uint32_t now;

	title_frame = frame;
	if (!Config::get ()->title_frame)
		return;
	/* Ten times a second: a title each frame costs */
	now = SDL_GetTicks ();
	if (now - title_frame_ticks >= 100) {
		title_frame_ticks = now;
		update_title ();
	}
}

const char *
Window::renderer_message ()
{
	return last_message;
}

/* The driver's own description of a GL error, logged as it happens: the
 * HL_ASSERT_NO_GL_ERROR checks only see it later, elsewhere */
static void GLAPIENTRY
log_gl_message (GLenum source, GLenum type, GLuint id, GLenum severity,
                GLsizei length, const GLchar *message, const void *user)
{
	(void) source;
	(void) length;
	(void) user;
	if (type == GL_DEBUG_TYPE_ERROR ||
	    severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM)
		HL_ERROR ("GL: %s (id %u, type %04X, severity %04X)", message, id, type, severity);
}

SDL_GLContext
Window::create_shared_context ()
{
	SDL_GLContext context;

	if (!shared_window)
		return NULL;
	if (!thread_window)
		thread_window = SDL_CreateWindow ("Hikaruloader", 0, 0, 16, 16,
		                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!thread_window) {
		HL_ERROR ("could not create the drawing thread's window: '%s'", SDL_GetError ());
		return NULL;
	}
	SDL_GL_SetAttribute (SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
	context = SDL_GL_CreateContext (thread_window);
	SDL_GL_SetAttribute (SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
	if (!context)
		HL_ERROR ("could not create a second GL context: '%s'", SDL_GetError ());
	/* SDL made the new one current: the window's again */
	SDL_GL_MakeCurrent (shared_window, shared_context);
	return context;
}

bool
Window::make_current_on_window (SDL_GLContext context)
{
	if (!context || SDL_GL_MakeCurrent (shared_window, context))
		return false;
	if (GLEW_KHR_debug) {
		glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback (log_gl_message, NULL);
	}
	glDisable (GL_MULTISAMPLE);
	gl::clear_errors ();
	return true;
}

void
Window::swap_from_thread ()
{
	static thread_local int interval = -1;
	int want = Config::get ()->vsync ? 1 : 0;

	if (want != interval) {
		SDL_GL_SetSwapInterval (want);
		interval = want;
	}
	SDL_GL_SwapWindow (shared_window);
}

bool
Window::make_current (SDL_GLContext context)
{
	if (SDL_GL_MakeCurrent (context ? thread_window : shared_window, context)) {
		HL_ERROR ("could not make a GL context current: '%s'", SDL_GetError ());
		return false;
	}
	if (context) {
		/* What the window's context has */
		if (GLEW_KHR_debug) {
			glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
			glDebugMessageCallback (log_gl_message, NULL);
		}
		glDisable (GL_MULTISAMPLE);
		gl::clear_errors ();
	}
	return true;
}

void
Window::delete_context (SDL_GLContext context)
{
	if (context)
		SDL_GL_DeleteContext (context);
}

int
Window::open (unsigned width, unsigned height)
{
	int num_ms_buffers, num_samples;

	if (shared_window)
		return 0;

	/* [Input] BACKGROUND_INPUT: SDL's joysticks without the focus; the keys
	 * stay behind it (hl::Controls) */
	SDL_SetHint (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, Config::get ()->background_input ? "1" : "0");

	if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER)) {
		HL_ERROR ("could not initialize SDL: '%s'", SDL_GetError ());
		return -1;
	}

	/* Vulkan 1.3 and Direct3D 11 renderers are planned; until then, the
	 * machines draw with OpenGL whatever the setting */
	if (Config::get ()->graphics_api != GRAPHICS_OPENGL)
		HL_PRINT ("window: the %s renderer is not there yet, using OpenGL",
		          Config::get ()->graphics_api == GRAPHICS_VULKAN ? "Vulkan" : "Direct3D 11");

	SDL_GL_SetAttribute (SDL_GL_RED_SIZE,   8);
	SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE,  8);
	SDL_GL_SetAttribute (SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_BUFFER_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_ACCUM_RED_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_ACCUM_GREEN_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_ACCUM_BLUE_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_ACCUM_ALPHA_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_STEREO, 0);
	SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, 0);
	SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 0);
	SDL_GL_SetAttribute (SDL_GL_ACCELERATED_VISUAL, 1);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 1);
	/* HR_GL_DEBUG=1: a debug context, whose driver says what goes wrong
	 * (log_gl_message): a context without it says nothing on Intel */
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_FLAGS,
	                     SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG |
	                     (hl_util_get_bool_option ("HR_GL_DEBUG", false) ?
	                      SDL_GL_CONTEXT_DEBUG_FLAG : 0));
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK,
	                     SDL_GL_CONTEXT_PROFILE_CORE);

	shared_window = SDL_CreateWindow ("Hikaruloader",
	                                  SDL_WINDOWPOS_CENTERED,
	                                  SDL_WINDOWPOS_CENTERED,
	                                  width, height,
	                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
	                                  SDL_WINDOW_ALLOW_HIGHDPI);
	if (!shared_window) {
		HL_ERROR ("could not create SDL window: '%s'", SDL_GetError ());
		return -1;
	}

	/* OpenGL 4.3 when the card has it, for the translucent meshes sorted
	 * at each pixel (storage buffers and atomics); else 3.1, which drivers
	 * give as asked (NVIDIA's 3.1 has none of it), the meshes then sorted
	 * whole. The drawing thread's context is made with the same */
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 3);
	shared_context = SDL_GL_CreateContext (shared_window);
	if (!shared_context) {
		SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 1);
		shared_context = SDL_GL_CreateContext (shared_window);
	}
	if (!shared_context) {
		HL_ERROR ("could not create GL context: '%s'", SDL_GetError ());
		return -1;
	}

	SDL_GL_SetSwapInterval (0);

	glewExperimental = GL_TRUE;
	if (glewInit () != GLEW_OK) {
		HL_ERROR ("could not initialize glew");
		return -1;
	}

	if (GLEW_KHR_debug) {
		glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback (log_gl_message, NULL);
	}

	/* Attempt to force multisampling off. */
	glDisable (GL_MULTISAMPLE);

	SDL_GL_GetAttribute (SDL_GL_MULTISAMPLEBUFFERS, &num_ms_buffers);
	SDL_GL_GetAttribute (SDL_GL_MULTISAMPLESAMPLES, &num_samples);

	HL_PRINT ("renderer: GL vendor    = %s", glGetString (GL_VENDOR));
	HL_PRINT ("renderer: GL renderer  = %s", glGetString (GL_RENDERER));
	HL_PRINT ("renderer: GL version   = %s", glGetString (GL_VERSION));
	HL_PRINT ("renderer: GLSL version = %s", glGetString (GL_SHADING_LANGUAGE_VERSION));
	HL_PRINT ("renderer: %d samples on %d ms buffers", num_samples, num_ms_buffers);

	glViewport (0, 0, width, height);

	/* The SDL initialization sequence does not clear the (harmless,
	 * hopefully) GL errors it may cause. Let's do it here so we can
	 * catch more serious errors later. */
	gl::clear_errors ();

	Window::log_displays ();
	Window::apply_display ();
	return 0;
}

} /* namespace hl */
