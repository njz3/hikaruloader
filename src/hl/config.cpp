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

#include "hl/config.h"
#include "hl/audio.h"

#include <ctype.h>

namespace hl {

static Config config;
static bool config_ready = false;
static char config_path[1024] = "hikaruloader.ini";

static const char *display_names[NUM_DISPLAY_MODES] = {
	"WINDOW", "FULLSCREEN", "BORDERLESS"
};

static const char *graphics_names[NUM_GRAPHICS_APIS] = {
	"OPENGL", "VULKAN", "D3D11"
};

static const char *memory_names[NUM_MEMORY_ACCESSES] = {
	"REGIONS", "PAGE_TABLE", "FASTMEM"
};

static const char *aspect_names[NUM_ASPECTS] = {
	"ORIGINAL", "INTEGER", "STRETCH"
};

static const char *screen_shape_names[NUM_SCREEN_SHAPES] = {
	"AUTO", "4:3", "16:9"
};

static const char *widescreen_2d_names[NUM_WIDESCREEN_2D] = {
	"CENTERED", "STRETCHED"
};

static const char *filter_names[NUM_FILTERS] = {
	"NEAREST", "LINEAR"
};

/* The BIOS's countries, from -1: the EEPROM's own */
static const char *region_names[] = {
	"AUTO", "JAPAN", "USA", "EXPORT", "AUSTRALIA", "KOREA"
};

/* The BIOS's ADVERTISE SOUND, from -1: the EEPROM's own */
static const char *attract_sound_names[] = {
	"AUTO", "OFF", "ON"
};

/* The BIOS's COMMUNICATION MODE, from -1: the EEPROM's own */
static const char *link_names[] = {
	"AUTO", "NO_LINK", "MASTER", "SLAVE", "RELAY"
};

/* The speakers, by their channels (Audio::Output) */
static const struct {
	int output;
	const char *name;
} sound_outputs[] = {
	{ Audio::OUTPUT_AUTO, "AUTO" },
	{ Audio::OUTPUT_MONO, "MONO" },
	{ Audio::OUTPUT_STEREO, "STEREO" },
	{ Audio::OUTPUT_2_1, "2.1" },
	{ Audio::OUTPUT_QUAD, "QUAD" },
	{ Audio::OUTPUT_5_1, "5.1" },
	{ Audio::OUTPUT_7_1, "7.1" }
};

static const char *
sound_output_name (int output)
{
	for (const auto &o : sound_outputs)
		if (o.output == output)
			return o.name;
	return sound_outputs[0].name;
}

void
Config::set_defaults ()
{
	Config *cfg = this;
	unsigned i;

	memset (cfg, 0, sizeof (*cfg));
	cfg->display_mode = DISPLAY_WINDOW;
	cfg->window_scale = 1;	/* the Hikaru's own 640 x 480 */
	cfg->aspect = ASPECT_ORIGINAL;
	cfg->graphics_api = GRAPHICS_OPENGL;
	cfg->vsync = false;
	cfg->fps_limiter = true;
	cfg->fps_target = 60.0f;
	cfg->fps_overlay = false;
	cfg->title_frame = false;
	cfg->hide_cursor = false;
	cfg->threaded_rendering = true;
	cfg->widescreen = false;
	cfg->widescreen_2d = WIDESCREEN_2D_CENTERED;
	cfg->screen_shape = SCREEN_AUTO;
	cfg->scale_filter = FILTER_NEAREST;
	cfg->draw_distance = 1.0f;
	cfg->render_scale = 1;
	cfg->msaa = 0;
	cfg->anisotropy = 1;
	cfg->texture_scale = 1;
	cfg->fxaa = false;
	cfg->bloom = 0;
	cfg->crt_filter = 0;
	strcpy (cfg->language, "auto");
	cfg->log_file = true;
	cfg->artwork_download = false;
	/* The MIE's Z80 runs, and drives the JVS bus */
	cfg->mie_hack = false;
	cfg->dynarec = true;
	cfg->memory_access = MEMORY_PAGE_TABLE;
	/* Below the board's 200 MHz: the waits the games poll cost the host
	 * its time (see hikaru.cpp) */
	cfg->sh4_clock = 57;
	/* The country the board's EEPROM holds */
	cfg->region = -1;
	/* The speakers the sound card has, all at their full level */
	cfg->sound_output = Audio::OUTPUT_AUTO;
	cfg->sound_balance = 50;
	cfg->sound_volume = 100;
	/* The attract's sound the board's EEPROM holds */
	cfg->attract_sound = -1;
	cfg->sound_latency = 50;
	cfg->sound_optimized = true;
	/* The link the board's EEPROM holds; a cabinet alone, its own next */
	cfg->link = -1;
	cfg->port_in = 1970;
	strcpy (cfg->address_out, "127.0.0.1");
	cfg->port_out = 1970;
	/* As windowsloader and linuxloader */
	cfg->buttons_from_one = true;
	for (i = 0; i < NUM_ACTIONS; i++)
		cfg->bindings[i] = Input::default_binding ((Action) i);
}

Config *
Config::get ()
{
	if (!config_ready) {
		config.set_defaults ();
		config_ready = true;
	}
	return &config;
}

unsigned
Config::sh4_clock_mhz ()
{
	return MAX2 (20u, MIN2 (200u, get ()->sh4_clock));
}

const char *
Config::path ()
{
	return config_path;
}

void
Config::set_path (const char *path)
{
	strncpy (config_path, path, sizeof (config_path) - 1);
}

void
Config::games_dir_in_use (char *dst, size_t size) const
{
	const Config *cfg = this;

	if (cfg->games_dir[0])
		snprintf (dst, size, "%s", cfg->games_dir);
	else {
		char *base = SDL_GetBasePath ();
		snprintf (dst, size, "%sroms", base ? base : "./");
		SDL_free (base);
	}
}

static char *
trim (char *s)
{
	char *end;
	while (isspace ((unsigned char) *s))
		s++;
	end = s + strlen (s);
	while (end > s && isspace ((unsigned char) end[-1]))
		*--end = '\0';
	return s;
}

static bool
parse_bool (const char *value)
{
	return !strcasecmp (value, "true") || !strcmp (value, "1") ||
	       !strcasecmp (value, "yes") || !strcasecmp (value, "on");
}

static int
parse_name (const char *value, const char **names, int count, int fallback)
{
	int i;
	for (i = 0; i < count; i++)
		if (!strcasecmp (value, names[i]))
			return i;
	return fallback;
}

/* The power of two at most value's, from min to max; below min, none (0,
 * or the anisotropy's 1) */
static int
parse_pow2 (const char *value, int none, int min, int max)
{
	int v = atoi (value), p = min;

	if (v < min)
		return none;
	while (p * 2 <= MIN2 (v, max))
		p *= 2;
	return p;
}

void
Config::set_value (const char *section, const char *key, const char *value)
{
	Config *cfg = this;
	unsigned i;

	if (!strcasecmp (section, "Display")) {
		if (!strcasecmp (key, "DISPLAY_MODE"))
			cfg->display_mode = parse_name (value, display_names, NUM_DISPLAY_MODES, cfg->display_mode);
		else if (!strcasecmp (key, "WINDOW_SCALE"))
			cfg->window_scale = MAX2 (1, MIN2 (4, atoi (value)));
		else if (!strcasecmp (key, "ASPECT_RATIO"))
			cfg->aspect = parse_name (value, aspect_names, NUM_ASPECTS, cfg->aspect);
		else if (!strcasecmp (key, "GRAPHICS_API"))
			cfg->graphics_api = parse_name (value, graphics_names, NUM_GRAPHICS_APIS, cfg->graphics_api);
		else if (!strcasecmp (key, "VSYNC"))
			cfg->vsync = parse_bool (value);
		else if (!strcasecmp (key, "FPS_LIMITER_ENABLED"))
			cfg->fps_limiter = parse_bool (value);
		else if (!strcasecmp (key, "FPS_TARGET"))
			cfg->fps_target = MAX2 (10.0f, MIN2 (240.0f, (float) atof (value)));
		else if (!strcasecmp (key, "FPS_OVERLAY_ENABLED"))
			cfg->fps_overlay = parse_bool (value);
		else if (!strcasecmp (key, "TITLE_FRAME_NUMBER"))
			cfg->title_frame = parse_bool (value);
		else if (!strcasecmp (key, "HIDE_CURSOR"))
			cfg->hide_cursor = parse_bool (value);
		else if (!strcasecmp (key, "THREADED_RENDERING"))
			cfg->threaded_rendering = parse_bool (value);
		else if (!strcasecmp (key, "WIDESCREEN"))
			cfg->widescreen = parse_bool (value);
		else if (!strcasecmp (key, "WIDESCREEN_2D"))
			cfg->widescreen_2d = parse_name (value, widescreen_2d_names, NUM_WIDESCREEN_2D, cfg->widescreen_2d);
		else if (!strcasecmp (key, "SCREEN_SHAPE"))
			cfg->screen_shape = parse_name (value, screen_shape_names, NUM_SCREEN_SHAPES, cfg->screen_shape);
		else if (!strcasecmp (key, "SCALE_FILTER"))
			cfg->scale_filter = parse_name (value, filter_names, NUM_FILTERS, cfg->scale_filter);
		else if (!strcasecmp (key, "DRAW_DISTANCE"))
			cfg->draw_distance = MAX2 (1.0f, MIN2 (4.0f, (float) atof (value)));
	} else if (!strcasecmp (section, "Enhancements")) {
		if (!strcasecmp (key, "RENDER_SCALE"))
			cfg->render_scale = MAX2 (1, MIN2 (4, atoi (value)));
		else if (!strcasecmp (key, "MSAA"))
			cfg->msaa = parse_pow2 (value, 0, 2, 8);
		else if (!strcasecmp (key, "ANISOTROPY"))
			cfg->anisotropy = parse_pow2 (value, 1, 1, 16);
		else if (!strcasecmp (key, "TEXTURE_SCALE"))
			cfg->texture_scale = parse_pow2 (value, 1, 1, 4);
		else if (!strcasecmp (key, "FXAA"))
			cfg->fxaa = parse_bool (value);
		else if (!strcasecmp (key, "BLOOM"))
			cfg->bloom = MAX2 (0, MIN2 (100, atoi (value)));
		else if (!strcasecmp (key, "CRT_FILTER"))
			cfg->crt_filter = MAX2 (0, MIN2 (100, atoi (value)));
	} else if (!strcasecmp (section, "Picker")) {
		if (!strcasecmp (key, "GAMES_DIR"))
			snprintf (cfg->games_dir, sizeof (cfg->games_dir), "%s", value);
		else if (!strcasecmp (key, "WRITE_DIR"))
			snprintf (cfg->write_dir, sizeof (cfg->write_dir), "%s", value);
		else if (!strcasecmp (key, "LANGUAGE"))
			snprintf (cfg->language, sizeof (cfg->language), "%s", value);
		else if (!strcasecmp (key, "LOG_FILE"))
			cfg->log_file = parse_bool (value);
		else if (!strcasecmp (key, "ARTWORK_DOWNLOAD"))
			cfg->artwork_download = parse_bool (value);
		else if (!strcasecmp (key, "LAST_GAME"))
			snprintf (cfg->last_game, sizeof (cfg->last_game), "%s", value);
	} else if (!strcasecmp (section, "Emulation")) {
		if (!strcasecmp (key, "MIE_HACK"))
			cfg->mie_hack = parse_bool (value);
		else if (!strcasecmp (key, "CPU_CORE"))
			cfg->dynarec = strcasecmp (value, "INTERPRETER") != 0;
		else if (!strcasecmp (key, "MEMORY_ACCESS"))
			cfg->memory_access = (MemoryAccess) parse_name (value, memory_names, NUM_MEMORY_ACCESSES, cfg->memory_access);
		else if (!strcasecmp (key, "SH4_CLOCK"))
			cfg->sh4_clock = (unsigned) MAX2 (20, MIN2 (200, atoi (value)));
		else if (!strcasecmp (key, "REGION"))
			cfg->region = parse_name (value, region_names, (int) NUMELEM (region_names), cfg->region + 1) - 1;
	} else if (!strcasecmp (section, "Sound")) {
		if (!strcasecmp (key, "OUTPUT")) {
			for (const auto &o : sound_outputs)
				if (!strcasecmp (value, o.name))
					cfg->sound_output = o.output;
		} else if (!strcasecmp (key, "BALANCE"))
			cfg->sound_balance = MAX2 (0, MIN2 (100, atoi (value)));
		else if (!strcasecmp (key, "VOLUME"))
			cfg->sound_volume = MAX2 (0, MIN2 (100, atoi (value)));
		else if (!strcasecmp (key, "ATTRACT_SOUND"))
			cfg->attract_sound = parse_name (value, attract_sound_names, (int) NUMELEM (attract_sound_names), cfg->attract_sound + 1) - 1;
		else if (!strcasecmp (key, "LATENCY"))
			cfg->sound_latency = MAX2 (20, MIN2 (200, atoi (value)));
		else if (!strcasecmp (key, "SOUND_CORE"))
			cfg->sound_optimized = strcasecmp (value, "INTERPRETED") != 0;
	} else if (!strcasecmp (section, "Network")) {
		if (!strcasecmp (key, "LINK"))
			cfg->link = parse_name (value, link_names, (int) NUMELEM (link_names), cfg->link + 1) - 1;
		else if (!strcasecmp (key, "PORT_IN"))
			cfg->port_in = (unsigned) MAX2 (1, MIN2 (65535, atoi (value)));
		else if (!strcasecmp (key, "ADDRESS_OUT"))
			snprintf (cfg->address_out, sizeof (cfg->address_out), "%s", value);
		else if (!strcasecmp (key, "PORT_OUT"))
			cfg->port_out = (unsigned) MAX2 (1, MIN2 (65535, atoi (value)));
	} else if (!strcasecmp (section, "Input")) {
		if (!strcasecmp (key, "BUTTONS_FROM_ONE"))
			cfg->buttons_from_one = parse_bool (value);
	} else if (!strcasecmp (section, "Controls")) {
		for (i = 0; i < NUM_ACTIONS; i++) {
			const char *name = Input::action_name ((Action) i);
			size_t len = strlen (name);
			if (strncasecmp (key, name, len))
				continue;
			if (!key[len])
				cfg->bindings[i].key = value[0] ? SDL_GetKeyFromName (value) : SDLK_UNKNOWN;
			else if (!strcasecmp (key + len, "_PAD"))
				cfg->bindings[i].button = value[0] ? SDL_GameControllerGetButtonFromString (value)
				                                   : SDL_CONTROLLER_BUTTON_INVALID;
		}
	}
}

bool
Config::load ()
{
	Config *cfg = this;
	char line[1024], section[64] = "";
	FILE *fp;

	fp = fopen (config_path, "r");
	if (!fp)
		return false;

	while (fgets (line, sizeof (line), fp)) {
		char *s = trim (line), *eq;
		if (!s[0] || s[0] == '#' || s[0] == ';')
			continue;
		if (s[0] == '[') {
			char *close = strchr (s, ']');
			if (close)
				*close = '\0';
			snprintf (section, sizeof (section), "%s", trim (s + 1));
			continue;
		}
		eq = strchr (s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		set_value (section, trim (s), trim (eq + 1));
	}
	fclose (fp);
	return true;
}

static const char *
bool_name (bool value)
{
	return value ? "true" : "false";
}

bool
Config::save () const
{
	const Config *cfg = this;
	Config def;
	FILE *fp;

	fp = fopen (config_path, "w");
	if (!fp)
		return false;
	/* Each setting's comment gives its default, as set_defaults () has it */
	def.set_defaults ();

	fprintf (fp,
	         "# Hikaruloader settings, written by the settings window (Tab, then Save).\n"
	         "# Each save writes the whole file again, these comments with it: comments\n"
	         "# of your own would be lost. A setting left out takes its default.\n"
	         "# The controls are in controls.ini, beside this file.\n");

	fprintf (fp, "\n[Display]\n");
	fprintf (fp,
	         "\n# WINDOW; FULLSCREEN, the whole screen at the desktop's resolution;\n"
	         "# BORDERLESS, a window without borders covering the screen.\n"
	         "# Default: %s\n", display_names[def.display_mode]);
	fprintf (fp, "DISPLAY_MODE = %s\n", display_names[cfg->display_mode]);
	fprintf (fp,
	         "\n# The window's size, 1 to 4 times the Hikaru's 640x480 (it can also be\n"
	         "# resized with the mouse). Default: %d\n", def.window_scale);
	fprintf (fp, "WINDOW_SCALE = %d\n", cfg->window_scale);
	fprintf (fp,
	         "\n# ORIGINAL, the whole picture at 4:3 with black bars where the window's\n"
	         "# shape differs; INTEGER, the same at whole multiples of 640x480 only, for\n"
	         "# sharp pixels; STRETCH, the picture fills the window, deformed.\n"
	         "# Default: %s\n", aspect_names[def.aspect]);
	fprintf (fp, "ASPECT_RATIO = %s\n", aspect_names[cfg->aspect]);
	fprintf (fp,
	         "\n# OPENGL (3.1); VULKAN and D3D11 are to come. Read at the start.\n"
	         "# Default: %s\n", graphics_names[def.graphics_api]);
	fprintf (fp, "GRAPHICS_API = %s\n", graphics_names[cfg->graphics_api]);
	fprintf (fp,
	         "\n# Vertical sync: true or false. Default: %s\n", bool_name (def.vsync));
	fprintf (fp, "VSYNC = %s\n", bool_name (cfg->vsync));
	fprintf (fp,
	         "\n# Waits between frames to hold FPS_TARGET: true or false.\n"
	         "# Default: %s\n", bool_name (def.fps_limiter));
	fprintf (fp, "FPS_LIMITER_ENABLED = %s\n", bool_name (cfg->fps_limiter));
	fprintf (fp,
	         "\n# The frame rate held, 10 to 240 frames a second. Default: %.1f\n", def.fps_target);
	fprintf (fp, "FPS_TARGET = %.1f\n", cfg->fps_target);
	fprintf (fp,
	         "\n# The frame rate shown over the picture (F12 toggles it): true or false.\n"
	         "# Default: %s\n", bool_name (def.fps_overlay));
	fprintf (fp, "FPS_OVERLAY_ENABLED = %s\n", bool_name (cfg->fps_overlay));
	fprintf (fp,
	         "\n# The frame number in the title bar, to report a glitch's frame: true or\n"
	         "# false. Default: %s\n", bool_name (def.title_frame));
	fprintf (fp, "TITLE_FRAME_NUMBER = %s\n", bool_name (cfg->title_frame));
	fprintf (fp,
	         "\n# The mouse pointer hidden over the window: true or false.\n"
	         "# Default: %s\n", bool_name (def.hide_cursor));
	fprintf (fp, "HIDE_CURSOR = %s\n", bool_name (cfg->hide_cursor));
	fprintf (fp,
	         "\n# The frames drawn by a thread of their own while the next one is\n"
	         "# emulated: faster when the drawing slows the game, but the picture\n"
	         "# comes a frame later. Taken at the game's start: true or false.\n"
	         "# Default: %s\n", bool_name (def.threaded_rendering));
	fprintf (fp, "THREADED_RENDERING = %s\n", bool_name (cfg->threaded_rendering));
	fprintf (fp,
	         "\n# The 3D at 16:9 (854x480): the scene seen wider, its middle as the\n"
	         "# game draws it. The game may leave objects out at the sides. Taken at\n"
	         "# the game's start: true or false. Default: %s\n", bool_name (def.widescreen));
	fprintf (fp, "WIDESCREEN = %s\n", bool_name (cfg->widescreen));
	fprintf (fp,
	         "\n# The 2D layers (texts, logos) in widescreen: CENTERED, at 4:3 in the\n"
	         "# middle; STRETCHED, to the whole width. Default: %s\n",
	         widescreen_2d_names[def.widescreen_2d]);
	fprintf (fp, "WIDESCREEN_2D = %s\n", widescreen_2d_names[cfg->widescreen_2d]);
	fprintf (fp,
	         "\n# The screen's own shape, for ASPECT_RATIO: AUTO, that of its resolution\n"
	         "# (square pixels); 4:3 or 16:9 whatever its resolution, for a CRT's super\n"
	         "# resolutions (1920x240). Default: %s\n", screen_shape_names[def.screen_shape]);
	fprintf (fp, "SCREEN_SHAPE = %s\n", screen_shape_names[cfg->screen_shape]);
	fprintf (fp,
	         "\n# The picture scaled to the window: NEAREST, sharp; LINEAR, smooth (down\n"
	         "# to 240 rows, each the mean of two instead of one in two dropped).\n"
	         "# Default: %s\n", filter_names[def.scale_filter]);
	fprintf (fp, "SCALE_FILTER = %s\n", filter_names[cfg->scale_filter]);
	fprintf (fp,
	         "\n# The 3D scenery's distance, 1.0 to 4.0 times the game's: the detailed\n"
	         "# models and the fog that much farther (not what the game itself leaves\n"
	         "# out). Default: %.1f\n", def.draw_distance);
	fprintf (fp, "DRAW_DISTANCE = %.1f\n", cfg->draw_distance);

	fprintf (fp, "\n[Enhancements]\n");
	fprintf (fp,
	         "\n# Beyond the board's own picture, all off by default, each taken at\n"
	         "# once. The 3D drawn at 1 to 4 times its size, 640x480 (854x480 in\n"
	         "# widescreen): 2 gives 1280x960, then fitted to the window. The 2D\n"
	         "# layers (texts, logos) keep theirs. Default: %d\n", def.render_scale);
	fprintf (fp, "RENDER_SCALE = %d\n", cfg->render_scale);
	fprintf (fp,
	         "\n# The 3D's edges antialiased, by multisampling: 0 (none), 2, 4 or 8\n"
	         "# samples a pixel (the graphics card's most at most). Default: %d\n", def.msaa);
	fprintf (fp, "MSAA = %d\n", cfg->msaa);
	fprintf (fp,
	         "\n# The textures seen at a slant (roads, floors) kept sharp, anisotropic\n"
	         "# filtering: 1 (none), 2, 4, 8 or 16. Default: %d\n", def.anisotropy);
	fprintf (fp, "ANISOTROPY = %d\n", cfg->anisotropy);
	fprintf (fp,
	         "\n# The textures enlarged by Scale2x, the edges of their flat colours\n"
	         "# (texts, signs) kept sharp: 1 (none), 2 or 4 times. More memory, slower\n"
	         "# uploads. Default: %d\n", def.texture_scale);
	fprintf (fp, "TEXTURE_SCALE = %d\n", cfg->texture_scale);
	fprintf (fp,
	         "\n# The 3D's edges smoothed once drawn (FXAA): cheaper than MSAA, a little\n"
	         "# blurrier. true or false. Default: %s\n", bool_name (def.fxaa));
	fprintf (fp, "FXAA = %s\n", bool_name (cfg->fxaa));
	fprintf (fp,
	         "\n# A glow around the 3D's bright parts (bloom): 0 (none) to 100 %%.\n"
	         "# Default: %d\n", def.bloom);
	fprintf (fp, "BLOOM = %d\n", cfg->bloom);
	fprintf (fp,
	         "\n# A CRT's look on a flat screen, its scanlines and its phosphors' mask:\n"
	         "# 0 (none) to 100 %%; best from 960 rows up. Default: %d\n", def.crt_filter);
	fprintf (fp, "CRT_FILTER = %d\n", cfg->crt_filter);

	fprintf (fp, "\n[Picker]\n");
	fprintf (fp,
	         "\n# The folder of the MAME romsets (<game>.zip, its parent's, hikaru.zip),\n"
	         "# or several separated by ';', looked in in that order (D:\\roms;E:\\mame\\roms).\n"
	         "# Default: empty, the roms folder beside the program\n");
	fprintf (fp, "GAMES_DIR = %s\n", cfg->games_dir);
	fprintf (fp,
	         "\n# The folder all that is written goes to, so that the rest of the disk\n"
	         "# can stay read only (a cabinet under Windows' Unified Write Filter):\n"
	         "# these settings, controls.ini and the log at its root, each game's nvram,\n"
	         "# states and dumps in <folder>/<game>/. The settings and a game's nvram\n"
	         "# are copied there from their usual places when it has none, then read\n"
	         "# there only. Relative: to the program's folder. Taken at the start from\n"
	         "# the hikaruloader.ini beside the program only; the --write-dir option\n"
	         "# wins. Default: empty, none\n");
	fprintf (fp, "WRITE_DIR = %s\n", cfg->write_dir);
	fprintf (fp,
	         "\n# The interface's language: auto (Windows'), en, fr, or another\n"
	         "# data/lang/<code>.po. Default: %s\n", def.language);
	fprintf (fp, "LANGUAGE = %s\n", cfg->language);
	fprintf (fp,
	         "\n# The log, hikaruloader.log: what to send with a problem report. Written\n"
	         "# beside the program, or at the write directory's root when there is\n"
	         "# one; the previous start's kept as hikaruloader.old.log. Taken at the\n"
	         "# next start: true or false. Default: %s\n", def.log_file ? "true" : "false");
	fprintf (fp, "LOG_FILE = %s\n", cfg->log_file ? "true" : "false");
	fprintf (fp,
	         "\n# The picker's pictures downloaded from the Arcade Database\n"
	         "# (adb.arcadeitalia.net, by motoschifo) into cache/artwork, one game at a\n"
	         "# time, those of the games found; yours in artwork/ beside the program\n"
	         "# (<set>.png or .jpg, or MAME's titles/, snap/, flyers/, marquees/) show\n"
	         "# either way: true or false. Default: %s\n", def.artwork_download ? "true" : "false");
	fprintf (fp, "ARTWORK_DOWNLOAD = %s\n", cfg->artwork_download ? "true" : "false");
	fprintf (fp,
	         "\n# The game last started, selected when the picker opens. Default: empty\n");
	fprintf (fp, "LAST_GAME = %s\n", cfg->last_game);

	fprintf (fp, "\n[Emulation]\n");
	fprintf (fp,
	         "\n# true: Valkyrie's hack fakes the MIE's answers, the Test and Service\n"
	         "# switches alone; false: the MIE's Z80 runs and drives the JVS I/O board.\n"
	         "# The MIE_HACK environment variable, when set, wins. Read at the start.\n"
	         "# Default: %s\n", bool_name (def.mie_hack));
	fprintf (fp, "MIE_HACK = %s\n", bool_name (cfg->mie_hack));
	fprintf (fp,
	         "\n# The SH-4s' code: DYNAREC, translated to x86-64; INTERPRETER, one\n"
	         "# instruction at a time. The same machine either way. Default: %s\n",
	         def.dynarec ? "DYNAREC" : "INTERPRETER");
	fprintf (fp, "CPU_CORE = %s\n", cfg->dynarec ? "DYNAREC" : "INTERPRETER");
	fprintf (fp,
	         "\n# How the CPUs find their RAM and ROM: PAGE_TABLE, at once; REGIONS,\n"
	         "# searched one after the other; FASTMEM, at their own addresses in the\n"
	         "# PC's address space (512 MB reserved per SH-4). Default: %s\n",
	         memory_names[def.memory_access]);
	fprintf (fp, "MEMORY_ACCESS = %s\n", memory_names[cfg->memory_access]);
	fprintf (fp,
	         "\n# The SH-4s' clock in MHz, 20 to 200 (the board's). Their timers, the\n"
	         "# MIE and the sound keep real time whatever it is; the faster, the more of\n"
	         "# the PC the waits the games poll take (at 200, their boots run below 60\n"
	         "# frames a second). From the game's start. Default: %u\n", def.sh4_clock);
	fprintf (fp, "SH4_CLOCK = %u\n", cfg->sh4_clock);
	fprintf (fp,
	         "\n# The board's country, which the games read for their language and\n"
	         "# texts: AUTO, the one its EEPROM holds (the BIOS's COUNTRY SETTING);\n"
	         "# JAPAN, USA, EXPORT, AUSTRALIA or KOREA, written to the game's EEPROM\n"
	         "# at its start. Default: %s\n", region_names[def.region + 1]);
	fprintf (fp, "REGION = %s\n", region_names[cfg->region + 1]);

	fprintf (fp, "\n[Sound]\n");
	fprintf (fp,
	         "\n# The PC's speakers: AUTO, those the sound card has; MONO, STEREO, 2.1,\n"
	         "# QUAD, 5.1 or 7.1. The cabinet's rear speakers go to the rear ones, or\n"
	         "# into the front without; the subwoofer gets all of the sound, low-passed;\n"
	         "# the center none. Default: %s\n", sound_output_name (def.sound_output));
	fprintf (fp, "OUTPUT = %s\n", sound_output_name (cfg->sound_output));
	fprintf (fp,
	         "\n# The front/rear balance, 0 to 100: 0 the front alone, 100 the rear alone,\n"
	         "# 50 both at their full level. Default: %d\n", def.sound_balance);
	fprintf (fp, "BALANCE = %d\n", cfg->sound_balance);
	fprintf (fp,
	         "\n# The volume, 0 to 100 percent. Default: %d\n", def.sound_volume);
	fprintf (fp, "VOLUME = %d\n", cfg->sound_volume);
	fprintf (fp,
	         "\n# The sound of the game's attract, while nobody plays: AUTO, as its EEPROM\n"
	         "# holds it (the BIOS's ADVERTISE SOUND, off on a new board); OFF or ON,\n"
	         "# written to the game's EEPROM at its start. Default: %s\n",
	         attract_sound_names[def.attract_sound + 1]);
	fprintf (fp, "ATTRACT_SOUND = %s\n", attract_sound_names[cfg->attract_sound + 1]);
	fprintf (fp,
	         "\n# The sound kept ahead in the output, 20 to 200 milliseconds: less, the\n"
	         "# sound closer to the picture, but gaps when a frame comes late; more,\n"
	         "# safer on a slow PC. The drift of the sound card's clock is followed\n"
	         "# either way. Default: %d\n", def.sound_latency);
	fprintf (fp, "LATENCY = %d\n", cfg->sound_latency);
	fprintf (fp,
	         "\n# The sound chips' emulation: OPTIMIZED keeps their channels' registers\n"
	         "# and their effects' program decoded until the game writes them, and runs\n"
	         "# their drivers' scan loops at once; INTERPRETED decodes them at each\n"
	         "# sample and runs one instruction at a time, slower, the reference. The\n"
	         "# same sound either way; changed at once. Default: %s\n",
	         def.sound_optimized ? "OPTIMIZED" : "INTERPRETED");
	fprintf (fp, "SOUND_CORE = %s\n", cfg->sound_optimized ? "OPTIMIZED" : "INTERPRETED");

	fprintf (fp, "\n[Network]\n");
	fprintf (fp,
	         "\n# The cabinets' link (the BIOS's COMMUNICATION MODE): AUTO, as the game's\n"
	         "# EEPROM holds it (NO_LINK on a new board); NO_LINK, MASTER, SLAVE or RELAY,\n"
	         "# written to the EEPROM at the game's start. One master per ring; a relay\n"
	         "# (SGNASCAR's monitor) does not play. Default: %s\n", link_names[def.link + 1]);
	fprintf (fp, "LINK = %s\n", link_names[cfg->link + 1]);
	fprintf (fp,
	         "\n# The ring, over TCP: the previous cabinet connects to this one's PORT_IN,\n"
	         "# this one to the next cabinet's ADDRESS_OUT and PORT_OUT, the last one to\n"
	         "# the master. A master alone is its own next (127.0.0.1 and its PORT_IN).\n"
	         "# Defaults: %u, %s, %u\n", def.port_in, def.address_out, def.port_out);
	fprintf (fp, "PORT_IN = %u\n", cfg->port_in);
	fprintf (fp, "ADDRESS_OUT = %s\n", cfg->address_out);
	fprintf (fp, "PORT_OUT = %u\n", cfg->port_out);

	fprintf (fp, "\n[Input]\n");
	fprintf (fp,
	         "\n# The Controls page numbers a joystick's buttons from 1, as Windows and\n"
	         "# vJoy do (true), or from 0, as SDL and controls.ini do (false): the file\n"
	         "# keeps SDL's numbers either way. Default: %s\n", bool_name (def.buttons_from_one));
	fprintf (fp, "BUTTONS_FROM_ONE = %s\n", bool_name (cfg->buttons_from_one));

	/* The controls are controls.ini's (hl::Controls): the old [Controls]
	 * is read once, into it, and no longer written */
	return fclose (fp) == 0;
}

} /* namespace hl */
