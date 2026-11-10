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

static const char *multi_screen_names[NUM_MULTI_SCREEN] = {
	"ONE", "SIDE_BY_SIDE", "WINDOWS"
};

static const char *display_names[NUM_DISPLAY_MODES] = {
	"WINDOW", "FULLSCREEN", "BORDERLESS"
};

static const char *graphics_names[NUM_GRAPHICS_APIS] = {
	"OPENGL", "VULKAN", "D3D11"
};

static const char *memory_names[NUM_MEMORY_ACCESSES] = {
	"REGIONS", "PAGE_TABLE", "FASTMEM"
};

/* Linuxloader's, Supermodel's, Flycast's and sm2-emu's */
static const char *aspect_names[NUM_ASPECTS] = {
	"ORIGINAL", "4:3", "16:9", "4:3_STRETCH", "16:9_STRETCH", "4:3_CROP", "16:9_CROP"
};

static const char *widescreen_2d_names[NUM_WIDESCREEN_2D] = {
	"CENTERED", "STRETCHED"
};

static const char *filter_names[NUM_FILTERS] = {
	"NEAREST", "LINEAR"
};

static const char *bezel_names[NUM_BEZEL_MODES] = {
	"OFF", "BLUR", "IMAGE"
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

/* The communication board (CommBoard) */
static const char *twin_levers_names[NUM_TWIN_LEVERS] = {
	"ORIGINAL", "COMBINED"
};

static const char *comm_board_names[NUM_COMM_BOARDS] = {
	"SIMULATED", "PROGRAM"
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
	cfg->multi_screen = MULTI_ONE;
	snprintf (cfg->screen_order, sizeof (cfg->screen_order), "AUTO");
	for (int i = 0; i < MAX_SCREENS; i++)
		snprintf (cfg->screen_display[i], sizeof (cfg->screen_display[i]), "AUTO");
	cfg->display_stretch = false;
	cfg->window_scale = 1;	/* the Hikaru's own 640 x 480 */
	cfg->aspect = ASPECT_ORIGINAL;
	cfg->crop_position = 0;
	cfg->crop_barrel = false;
	/* As linuxloader's */
	cfg->barrel_center = 60;
	cfg->barrel_radius = 50;
	cfg->integer_scale = false;
	cfg->graphics_api = GRAPHICS_OPENGL;
	cfg->vsync = false;
	cfg->fps_limiter = true;
	cfg->fps_target = 60.0f;
	cfg->fps_overlay = false;
	cfg->title_frame = false;
	cfg->hide_cursor = true;
	cfg->threaded_rendering = true;
	cfg->sort_translucent_pixels = true;
	cfg->widescreen = false;
	cfg->widescreen_2d = WIDESCREEN_2D_CENTERED;
	cfg->scale_filter = FILTER_NEAREST;
	cfg->draw_distance = 1.0f;
	/* Black around the picture, as before */
	cfg->bezel = BEZEL_OFF;
	cfg->render_scale = 1;
	cfg->msaa = 0;
	cfg->anisotropy = 1;
	cfg->texture_scale = 1;
	cfg->fxaa = false;
	cfg->bloom = 0;
	cfg->crt_filter = 0;
	cfg->crt_preset[0] = '\0';
	strcpy (cfg->language, "auto");
	cfg->log_file = true;
	cfg->artwork_download = false;
	/* The MIE's Z80 runs, and drives the JVS bus */
	cfg->dynarec = true;
	cfg->memory_access = MEMORY_PAGE_TABLE;
	/* Below the board's 200 MHz: the waits the games poll cost the host
	 * its time (see hikaru.cpp). 100 at least for AIRTRIX, whose slave
	 * poses the skater before the GPU draws him (57 before November 2026) */
	cfg->sh4_clock = 150;
	cfg->naomi_sh4_clock = 200;
	/* The country the board's EEPROM holds */
	cfg->region = 2;	/* Export */
	/* The simulations, which every game plays with */
	cfg->io_board_program = false;
	cfg->security_chips = true;
	/* The speakers the sound card has, all at their full level */
	cfg->sound_output = Audio::OUTPUT_AUTO;
	cfg->sound_balance = 50;
	cfg->sound_volume = 100;
	/* The attract's sound on: a new board's EEPROM has it off */
	cfg->attract_sound = 1;
	cfg->sound_latency = 50;
	cfg->sound_optimized = true;
	/* The link the board's EEPROM holds; a cabinet alone, its own next */
	cfg->link = -1;
	/* Two cabinets, as before November 2026; PROGRAM links more */
	cfg->comm_board = COMM_BOARD_SIMULATED;
	cfg->port_in = 1970;
	strcpy (cfg->address_out, "127.0.0.1");
	cfg->port_out = 1970;
	/* Off, as in MAME, Supermodel and Flycast: a server on the PC's
	 * addresses; their ports */
	cfg->outputs_network = false;
	cfg->outputs_port = 8000;
	cfg->outputs_announce_port = 8001;
	/* Off, as in Supermodel and sm2-emu */
	cfg->telemetry_port = 0;
	cfg->telemetry_forza[0] = 0;
	cfg->ffb_effects = true;
	cfg->ffb_steering = cfg->ffb_engines = cfg->ffb_hits = cfg->ffb_terrain = cfg->ffb_rumble = 100;
	cfg->ffb_wheel_spring = cfg->ffb_wheel_friction = cfg->ffb_wheel_turn = cfg->ffb_wheel_kick = 100;
	cfg->ffb_wheel_boost = cfg->ffb_wheel_scrape = cfg->ffb_wheel_impact = cfg->ffb_wheel_bumps = 100;
	cfg->ffb_wheel_engines = 70;
	cfg->ffb_shots = cfg->ffb_missiles = cfg->ffb_bombs = cfg->ffb_nozzle = 100;
	cfg->ffb_device[0] = '\0';
	cfg->ffb_gain = 100;
	cfg->ffb_hud = false;
	/* As windowsloader and linuxloader */
	cfg->buttons_from_one = true;
	/* The wheel's whole turn for the game's, as before November 2026; the
	 * factory turn of most wheels (Logitech's), Odin's choice too */
	cfg->wheel_rotation = 0;
	cfg->wheel_range = 900;
	/* sm2-emu's defaults */
	cfg->shifter_neutral = true;
	cfg->shift_neutral_ms = 0;
	/* A wheel and its pedals, or a game controller's stick and trigger,
	 * drive the pod at once */
	cfg->twin_levers = TWIN_LEVERS_COMBINED;
	cfg->background_input = false;
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

unsigned
Config::naomi_sh4_clock_mhz ()
{
	return MAX2 (20u, MIN2 (200u, get ()->naomi_sh4_clock));
}

double
Config::wheel_scale (int rotation, int range)
{
	if (rotation <= 0 || range <= rotation)
		return 1.0;
	return (double) range / rotation;
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
		if (!strcasecmp (key, "DISPLAY_MODE")) {
			/* WINDOW, FULLSCREEN or BORDERLESS, each also _STRETCH */
			char mode[32];
			size_t len = strlen (value);
			snprintf (mode, sizeof (mode), "%s", value);
			cfg->display_stretch = len > 8 && len < sizeof (mode) &&
			                       !strcasecmp (mode + len - 8, "_STRETCH");
			if (cfg->display_stretch)
				mode[len - 8] = '\0';
			cfg->display_mode = parse_name (mode, display_names, NUM_DISPLAY_MODES, cfg->display_mode);
		} else if (!strcasecmp (key, "MULTI_SCREEN"))
			cfg->multi_screen = parse_name (value, multi_screen_names, NUM_MULTI_SCREEN, cfg->multi_screen);
		else if (!strcasecmp (key, "SCREEN_ORDER"))
			snprintf (cfg->screen_order, sizeof (cfg->screen_order), "%s", value);
		else if (!strncasecmp (key, "SCREEN_", 7) && key[7] >= '1' && key[7] < '1' + MAX_SCREENS &&
		         !strcasecmp (key + 8, "_DISPLAY"))
			snprintf (cfg->screen_display[key[7] - '1'], sizeof (cfg->screen_display[0]), "%s", value);
		else if (!strcasecmp (key, "WINDOW_SCALE"))
			cfg->window_scale = MAX2 (1, MIN2 (4, atoi (value)));
		else if (!strcasecmp (key, "ASPECT_RATIO")) {
			/* Before November 2026: INTEGER (whole multiples) and
			 * STRETCH (the picture filling the window); 4:3_LETTERBOX
			 * is 4:3, as in linuxloader */
			if (!strcasecmp (value, "INTEGER")) {
				cfg->aspect = ASPECT_ORIGINAL;
				cfg->integer_scale = true;
			} else if (!strcasecmp (value, "STRETCH")) {
				cfg->aspect = ASPECT_ORIGINAL;
				cfg->display_stretch = true;
			} else if (!strcasecmp (value, "4:3_LETTERBOX"))
				cfg->aspect = ASPECT_4_3;
			else
				cfg->aspect = parse_name (value, aspect_names, NUM_ASPECTS, cfg->aspect);
		} else if (!strcasecmp (key, "CROP_POSITION"))
			cfg->crop_position = MAX2 (-100, MIN2 (100, atoi (value)));
		else if (!strcasecmp (key, "CROP_BARREL"))
			cfg->crop_barrel = parse_bool (value);
		else if (!strcasecmp (key, "BARREL_CENTER"))
			cfg->barrel_center = MAX2 (0, MIN2 (100, atoi (value)));
		else if (!strcasecmp (key, "BARREL_RADIUS"))
			cfg->barrel_radius = MAX2 (1, MIN2 (1000, atoi (value)));
		else if (!strcasecmp (key, "INTEGER_SCALE"))
			cfg->integer_scale = parse_bool (value);
		else if (!strcasecmp (key, "RENDER_SCALE"))
			cfg->render_scale = MAX2 (1, MIN2 (MAX_RENDER_SCALE, atoi (value)));
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
		else if (!strcasecmp (key, "SORT_TRANSLUCENT_PIXELS"))
			cfg->sort_translucent_pixels = parse_bool (value);
		else if (!strcasecmp (key, "WIDESCREEN"))
			cfg->widescreen = parse_bool (value);
		else if (!strcasecmp (key, "WIDESCREEN_2D"))
			cfg->widescreen_2d = parse_name (value, widescreen_2d_names, NUM_WIDESCREEN_2D, cfg->widescreen_2d);
		/* Linuxloader's name; SCALE_FILTER before November 2026 */
		else if (!strcasecmp (key, "SCALING_FILTER") || !strcasecmp (key, "SCALE_FILTER"))
			cfg->scale_filter = parse_name (value, filter_names, NUM_FILTERS, cfg->scale_filter);
		else if (!strcasecmp (key, "DRAW_DISTANCE"))
			cfg->draw_distance = MAX2 (1.0f, MIN2 (4.0f, (float) atof (value)));
		else if (!strcasecmp (key, "BEZEL"))
			cfg->bezel = parse_name (value, bezel_names, NUM_BEZEL_MODES, cfg->bezel);
	} else if (!strcasecmp (section, "Enhancements")) {
		/* In [Display] since November 2026 */
		if (!strcasecmp (key, "RENDER_SCALE"))
			cfg->render_scale = MAX2 (1, MIN2 (MAX_RENDER_SCALE, atoi (value)));
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
		else if (!strcasecmp (key, "CRT_PRESET"))
			snprintf (cfg->crt_preset, sizeof (cfg->crt_preset), "%s",
			          strcasecmp (value, "NONE") ? value : "");
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
		if (!strcasecmp (key, "CPU_CORE"))
			cfg->dynarec = strcasecmp (value, "INTERPRETER") != 0;
		else if (!strcasecmp (key, "MEMORY_ACCESS"))
			cfg->memory_access = (MemoryAccess) parse_name (value, memory_names, NUM_MEMORY_ACCESSES, cfg->memory_access);
		else if (!strcasecmp (key, "SH4_CLOCK"))
			cfg->sh4_clock = (unsigned) MAX2 (20, MIN2 (200, atoi (value)));
		else if (!strcasecmp (key, "NAOMI_SH4_CLOCK"))
			cfg->naomi_sh4_clock = (unsigned) MAX2 (20, MIN2 (200, atoi (value)));
		else if (!strcasecmp (key, "REGION"))
			cfg->region = parse_name (value, region_names, (int) NUMELEM (region_names), cfg->region + 1) - 1;
		else if (!strcasecmp (key, "IO_BOARD_PROGRAM"))
			cfg->io_board_program = parse_bool (value);
		else if (!strcasecmp (key, "SECURITY_CHIPS"))
			cfg->security_chips = parse_bool (value);
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
		else if (!strcasecmp (key, "COMM_BOARD"))
			cfg->comm_board = parse_name (value, comm_board_names, NUM_COMM_BOARDS, cfg->comm_board);
		else if (!strcasecmp (key, "PORT_IN"))
			cfg->port_in = (unsigned) MAX2 (1, MIN2 (65535, atoi (value)));
		else if (!strcasecmp (key, "ADDRESS_OUT"))
			snprintf (cfg->address_out, sizeof (cfg->address_out), "%s", value);
		else if (!strcasecmp (key, "PORT_OUT"))
			cfg->port_out = (unsigned) MAX2 (1, MIN2 (65535, atoi (value)));
	} else if (!strcasecmp (section, "Outputs")) {
		if (!strcasecmp (key, "NETWORK"))
			cfg->outputs_network = parse_bool (value);
		else if (!strcasecmp (key, "PORT"))
			cfg->outputs_port = (unsigned) MAX2 (1, MIN2 (65535, atoi (value)));
		else if (!strcasecmp (key, "ANNOUNCE_PORT"))
			cfg->outputs_announce_port = (unsigned) MAX2 (0, MIN2 (65535, atoi (value)));
	} else if (!strcasecmp (section, "Telemetry")) {
		if (!strcasecmp (key, "PORT"))
			cfg->telemetry_port = (unsigned) MAX2 (0, MIN2 (65535, atoi (value)));
		else if (!strcasecmp (key, "FORZA"))
			snprintf (cfg->telemetry_forza, sizeof (cfg->telemetry_forza), "%s", value);
	} else if (!strcasecmp (section, "ForceFeedback")) {
		unsigned pct = (unsigned) MAX2 (0, MIN2 (200, atoi (value)));
		if (!strcasecmp (key, "GAIN"))
			cfg->ffb_gain = (unsigned) MAX2 (0, MIN2 (300, atoi (value)));
		else if (!strcasecmp (key, "HUD"))
			cfg->ffb_hud = parse_bool (value);
		else if (!strcasecmp (key, "EFFECTS"))
			cfg->ffb_effects = parse_bool (value);
		else if (!strcasecmp (key, "STEERING"))
			cfg->ffb_steering = pct;
		else if (!strcasecmp (key, "ENGINES"))
			cfg->ffb_engines = pct;
		else if (!strcasecmp (key, "HITS"))
			cfg->ffb_hits = pct;
		else if (!strcasecmp (key, "TERRAIN"))
			cfg->ffb_terrain = pct;
		else if (!strcasecmp (key, "RUMBLE"))
			cfg->ffb_rumble = pct;
		else if (!strcasecmp (key, "DEVICE"))
			snprintf (cfg->ffb_device, sizeof (cfg->ffb_device), "%s",
			          strcasecmp (value, "AUTO") ? value : "");
		else if (!strcasecmp (key, "SHOTS"))
			cfg->ffb_shots = pct;
		else if (!strcasecmp (key, "MISSILES"))
			cfg->ffb_missiles = pct;
		else if (!strcasecmp (key, "BOMBS"))
			cfg->ffb_bombs = pct;
		else if (!strcasecmp (key, "NOZZLE"))
			cfg->ffb_nozzle = pct;
		else if (!strcasecmp (key, "WHEEL_SPRING"))
			cfg->ffb_wheel_spring = pct;
		else if (!strcasecmp (key, "WHEEL_FRICTION"))
			cfg->ffb_wheel_friction = pct;
		else if (!strcasecmp (key, "WHEEL_TURN"))
			cfg->ffb_wheel_turn = pct;
		else if (!strcasecmp (key, "WHEEL_KICK"))
			cfg->ffb_wheel_kick = pct;
		else if (!strcasecmp (key, "WHEEL_ENGINES"))
			cfg->ffb_wheel_engines = pct;
		else if (!strcasecmp (key, "WHEEL_BOOST"))
			cfg->ffb_wheel_boost = pct;
		else if (!strcasecmp (key, "WHEEL_SCRAPE"))
			cfg->ffb_wheel_scrape = pct;
		else if (!strcasecmp (key, "WHEEL_IMPACT"))
			cfg->ffb_wheel_impact = pct;
		else if (!strcasecmp (key, "WHEEL_BUMPS"))
			cfg->ffb_wheel_bumps = pct;
	} else if (!strcasecmp (section, "Input")) {
		if (!strcasecmp (key, "BUTTONS_FROM_ONE"))
			cfg->buttons_from_one = parse_bool (value);
		else if (!strcasecmp (key, "WHEEL_ROTATION")) {
			/* Any number of degrees, not only the page's choices */
			int degrees = atoi (value);
			cfg->wheel_rotation = (degrees <= 0) ? 0 : MAX2 (90, MIN2 (3600, degrees));
		} else if (!strcasecmp (key, "WHEEL_RANGE"))
			cfg->wheel_range = MAX2 (90, MIN2 (3600, atoi (value)));
		else if (!strcasecmp (key, "SHIFTER_NEUTRAL"))
			cfg->shifter_neutral = parse_bool (value);
		else if (!strcasecmp (key, "SHIFT_NEUTRAL_MS"))
			cfg->shift_neutral_ms = MAX2 (0, MIN2 (500, atoi (value)));
		else if (!strcasecmp (key, "TWIN_LEVERS"))
			cfg->twin_levers = (TwinLevers) parse_name (value, twin_levers_names, NUM_TWIN_LEVERS,
			                                             cfg->twin_levers);
		else if (!strcasecmp (key, "BACKGROUND_INPUT"))
			cfg->background_input = parse_bool (value);
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
Config::override_display_mode (const char *name)
{
	char mode[32];
	size_t len = strlen (name);
	bool stretch;
	int i;

	if (len >= sizeof (mode))
		return false;
	snprintf (mode, sizeof (mode), "%s", name);
	stretch = len > 8 && !strcasecmp (mode + len - 8, "_STRETCH");
	if (stretch)
		mode[len - 8] = '\0';
	for (i = 0; i < NUM_DISPLAY_MODES; i++)
		if (!strcasecmp (mode, display_names[i]))
			break;
	if (i == NUM_DISPLAY_MODES)
		return false;
	display_mode_file = display_mode;
	display_stretch_file = display_stretch;
	display_mode = display_mode_cli = i;
	display_stretch = display_stretch_cli = stretch;
	display_overridden = true;
	return true;
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
	         "\n# The 3D drawn at 1 to %d times the board's 640x480 (854x480 in\n"
	         "# widescreen): 2 gives 1280x960, then fitted to the window. The 2D\n"
	         "# layers (texts, logos) keep theirs. Taken at once. Default: %d\n",
	         MAX_RENDER_SCALE, def.render_scale);
	fprintf (fp, "RENDER_SCALE = %d\n", cfg->render_scale);
	fprintf (fp,
	         "\n# The frame the picture is shown in, and how the picture fills it, as in\n"
	         "# linuxloader, Supermodel, Flycast and sm2-emu: ORIGINAL, the picture's own\n"
	         "# shape (4:3, 16:9 in widescreen); 4:3 or 16:9, that frame with the whole\n"
	         "# picture in it, black bars where the shapes differ; 4:3_STRETCH or\n"
	         "# 16:9_STRETCH, the picture fills the frame, deformed; 4:3_CROP or\n"
	         "# 16:9_CROP, it fills it at its own shape, cut at the sides or above and\n"
	         "# below. DISPLAY_MODE then fits the frame to the window or the screen,\n"
	         "# or stretches it. Taken at once. Default: %s\n", aspect_names[def.aspect]);
	fprintf (fp, "ASPECT_RATIO = %s\n", aspect_names[cfg->aspect]);
	fprintf (fp,
	         "\n# The part a cropped ASPECT_RATIO keeps: -100 (left or top) to 100 (right\n"
	         "# or bottom), 0 the middle. Default: %d\n", def.crop_position);
	fprintf (fp, "CROP_POSITION = %d\n", cfg->crop_position);
	fprintf (fp,
	         "\n# A cropped ASPECT_RATIO: true bends the parts it would cut onto the\n"
	         "# frame's edges, as on a cylinder seen from the front, so the whole\n"
	         "# picture stays visible. Default: %s\n", bool_name (def.crop_barrel));
	fprintf (fp, "CROP_BARREL = %s\n", bool_name (cfg->crop_barrel));
	fprintf (fp,
	         "\n# With CROP_BARREL: the middle shown undistorted, in %% of the frame.\n"
	         "# Default: %d\n", def.barrel_center);
	fprintf (fp, "BARREL_CENTER = %d\n", cfg->barrel_center);
	fprintf (fp,
	         "\n# With CROP_BARREL: the cylinder's radius in %% of the frame (small bends\n"
	         "# hard at the edge, large compresses evenly). Default: %d\n", def.barrel_radius);
	fprintf (fp, "BARREL_RADIUS = %d\n", cfg->barrel_radius);
	fprintf (fp,
	         "\n# WINDOW; FULLSCREEN, the whole screen at the desktop's resolution;\n"
	         "# BORDERLESS, a window without borders covering the screen: the frame\n"
	         "# at its shape in the middle. Each also _STRETCH (WINDOW_STRETCH...): the\n"
	         "# frame fills the window or the screen. On a CRT at a super resolution\n"
	         "# (1920x240), the screen's real shape as ASPECT_RATIO (4:3 or 16:9) and a\n"
	         "# _STRETCH mode. Default: %s\n", display_names[def.display_mode]);
	/* --display-mode is this start's: the file keeps its own, unless the
	 * setting was changed since */
	bool kept = cfg->display_overridden && cfg->display_mode == cfg->display_mode_cli &&
	            cfg->display_stretch == cfg->display_stretch_cli;
	int saved_mode = kept ? cfg->display_mode_file : cfg->display_mode;
	bool saved_stretch = kept ? cfg->display_stretch_file : cfg->display_stretch;
	fprintf (fp, "DISPLAY_MODE = %s%s\n", display_names[saved_mode],
	         saved_stretch ? "_STRETCH" : "");
	fprintf (fp,
	         "\n# The window's size, 1 to 4 times the Hikaru's 640x480 (it can also be\n"
	         "# resized with the mouse). Default: %d\n", def.window_scale);
	fprintf (fp, "WINDOW_SCALE = %d\n", cfg->window_scale);
	fprintf (fp,
	         "\n# NAOMI cabinets of several boards, read when a game starts: ONE shows one\n"
	         "# board's picture (NextScreen, F8, goes to the next board), SIDE_BY_SIDE\n"
	         "# puts them in a row in the window, for a very wide monitor, and WINDOWS\n"
	         "# gives each its window. Default: %s\n", multi_screen_names[def.multi_screen]);
	fprintf (fp, "MULTI_SCREEN = %s\n", multi_screen_names[cfg->multi_screen]);
	fprintf (fp,
	         "\n# The boards with a monitor, left to right, numbered from 1 (\"2,1,3\").\n"
	         "# AUTO: the game's own. Default: AUTO\n");
	fprintf (fp, "SCREEN_ORDER = %s\n", cfg->screen_order[0] ? cfg->screen_order : "AUTO");
	fprintf (fp,
	         "\n# With WINDOWS, the monitor of the windows, left to right: the number\n"
	         "# Windows gives it (1, 2, 3), INDEX:<n> for SDL's numbering from 0, or a\n"
	         "# part of the monitor's name. The log lists them at its start. AUTO: the\n"
	         "# next monitor there is. Default: AUTO\n");
	for (int i = 0; i < MAX_SCREENS; i++)
		fprintf (fp, "SCREEN_%d_DISPLAY = %s\n", i + 1, cfg->screen_display[i][0] ? cfg->screen_display[i] : "AUTO");
	fprintf (fp,
	         "\n# The picture at whole multiples of its size only (1x, 2x, 3x...), for\n"
	         "# even pixels, the frame's bars larger: true or false. Not with a\n"
	         "# stretched or cropped picture, nor a stretched DISPLAY_MODE. Default: %s\n",
	         bool_name (def.integer_scale));
	fprintf (fp, "INTEGER_SCALE = %s\n", bool_name (cfg->integer_scale));
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
	         "\n# The translucent polygons sorted at each pixel, from the farthest,\n"
	         "# as the board blends them: right where they cross. It needs OpenGL\n"
	         "# 4.3 and no MSAA; false sorts each mesh as a whole, lighter for a\n"
	         "# weak graphics card. true or false. Default: %s\n",
	         bool_name (def.sort_translucent_pixels));
	fprintf (fp, "SORT_TRANSLUCENT_PIXELS = %s\n", bool_name (cfg->sort_translucent_pixels));
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
	         "\n# The picture scaled to the window: NEAREST, sharp pixels (even at whole\n"
	         "# multiples only); LINEAR, smooth (down to 240 rows, each the mean of two\n"
	         "# instead of one in two dropped). Default: %s\n", filter_names[def.scale_filter]);
	fprintf (fp, "SCALING_FILTER = %s\n", filter_names[cfg->scale_filter]);
	fprintf (fp,
	         "\n# The 3D scenery's distance, 1.0 to 4.0 times the game's: the detailed\n"
	         "# models and the fog that much farther (not what the game itself leaves\n"
	         "# out). Default: %.1f\n", def.draw_distance);
	fprintf (fp, "DRAW_DISTANCE = %.1f\n", cfg->draw_distance);
	fprintf (fp,
	         "\n# Around the picture where the window is wider than its frame: OFF,\n"
	         "# black; BLUR, the picture's own edges blurred and dimmed; IMAGE, the\n"
	         "# game's bezel picture in Batocera's format, artwork/bezels/<set>.png\n"
	         "# and <set>.info (the screen's place in it; without, where the picture\n"
	         "# is see-through), a clone taking its parent's, else\n"
	         "# artwork/bezels/default.png; black without any. The game's picture goes\n"
	         "# in the bezel's screen, as ASPECT_RATIO places it. Taken at once.\n"
	         "# Default: %s\n", bezel_names[def.bezel]);
	fprintf (fp, "BEZEL = %s\n", bezel_names[cfg->bezel]);

	fprintf (fp, "\n[Enhancements]\n");
	fprintf (fp,
	         "\n# Beyond the board's own picture, all off by default, each taken at\n"
	         "# once. The 3D's edges antialiased, by multisampling: 0 (none), 2, 4 or 8\n"
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
	         "# 0 (none) to 100 %%; best from 960 rows up. Not drawn with a CRT_PRESET,\n"
	         "# whose mask it would beat with. Default: %d\n", def.crt_filter);
	fprintf (fp, "CRT_FILTER = %d\n", cfg->crt_filter);
	fprintf (fp,
	         "\n# A CRT's look by a libretro shader preset (.glslp, as 86Box runs\n"
	         "# them), over the whole picture, in CRT_FILTER's place: NONE, or its\n"
	         "# name in data/crt beside the program: Lola Retro's (MIT, by Zoki),\n"
	         "# lola-retro/sega-monitor-29 and lola-retro/sega-projector-50 (the\n"
	         "# cabinets' screens), lola-retro/pc-monitor-vga (also -good, -premium,\n"
	         "# -premium-plus, -extreme, each also -flat: PC monitors); or a .glslp\n"
	         "# file's path. Their tube stays 4:3. Taken at once. Default: NONE\n");
	fprintf (fp, "CRT_PRESET = %s\n", cfg->crt_preset[0] ? cfg->crt_preset : "NONE");

	fprintf (fp, "\n[Picker]\n");
	fprintf (fp,
	         "\n# The folder of the MAME romsets (<game>.zip or .7z, its parent's,\n"
	         "# hikaru's), or several separated by ';', looked in in that order\n"
	         "# (D:\\roms;E:\\mame\\roms).\n"
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
	         "# frames a second). Air Trix needs 100 at least: below, its skater's\n"
	         "# body is missing. From the game's start. Default: %u\n", def.sh4_clock);
	fprintf (fp, "SH4_CLOCK = %u\n", cfg->sh4_clock);
	fprintf (fp,
	         "\n# The NAOMI's, NAOMI 2's and Atomiswave's SH-4 clock in MHz, 20 to 200\n"
	         "# (the board's). Their games unpack their pictures by what time each frame\n"
	         "# leaves: slower, pictures show before they are complete. From the game's\n"
	         "# start. Default: %u\n", def.naomi_sh4_clock);
	fprintf (fp, "NAOMI_SH4_CLOCK = %u\n", cfg->naomi_sh4_clock);
	fprintf (fp,
	         "\n# The board's country, which the games read for their language and\n"
	         "# texts: AUTO, the one its EEPROM holds (the BIOS's COUNTRY SETTING);\n"
	         "# JAPAN, USA, EXPORT, AUSTRALIA or KOREA, written to the game's EEPROM\n"
	         "# at its start. Default: %s\n", region_names[def.region + 1]);
	fprintf (fp, "REGION = %s\n", region_names[cfg->region + 1]);
	fprintf (fp,
	         "\n# The JVS I/O board's own CPU running its program, as on the cabinet,\n"
	         "# instead of its simulation: the 837-13551's TMP90PH44 running 315-6215\n"
	         "# (MAME's jvs13551 set), NASCAR Arcade's 837-13741 running epr-21868\n"
	         "# (the 18wheelr set). Without the file, the simulation, said in the log.\n"
	         "# From the game's start: true or false. Default: %s\n", bool_name (def.io_board_program));
	fprintf (fp, "IO_BOARD_PROGRAM = %s\n", bool_name (cfg->io_board_program));
	fprintf (fp,
	         "\n# The security chips answering as the boards' do: the ROM board's X76F100\n"
	         "# secure EEPROM and the main board's 93C46 from their images (x76f100.ic85\n"
	         "# and 93c46.ic115, in the newer MAME hikaru sets), the BOOTROM's check of\n"
	         "# them run instead of skipped; NASCAR Arcade's Actel FPGA as the chip.\n"
	         "# From the game's start: true or false. Default: %s\n", bool_name (def.security_chips));
	fprintf (fp, "SECURITY_CHIPS = %s\n", bool_name (cfg->security_chips));

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
	         "\n# The communication board: SIMULATED, its program's work done by\n"
	         "# Hikaruloader, two cabinets at most; PROGRAM, its 68000 running the\n"
	         "# program the game gives it, as on the board: up to 4 cabinets for Star\n"
	         "# Wars Racer Arcade and Virtual-On, 8 for NASCAR Arcade. The same in all\n"
	         "# the cabinets of a ring. From the game's start. Default: %s\n",
	         comm_board_names[def.comm_board]);
	fprintf (fp, "COMM_BOARD = %s\n", comm_board_names[cfg->comm_board]);
	fprintf (fp,
	         "\n# The ring, over TCP: the previous cabinet connects to this one's PORT_IN,\n"
	         "# this one to the next cabinet's ADDRESS_OUT and PORT_OUT, the last one to\n"
	         "# the master. A master alone is its own next (127.0.0.1 and its PORT_IN).\n"
	         "# Defaults: %u, %s, %u\n", def.port_in, def.address_out, def.port_out);
	fprintf (fp, "PORT_IN = %u\n", cfg->port_in);
	fprintf (fp, "ADDRESS_OUT = %s\n", cfg->address_out);
	fprintf (fp, "PORT_OUT = %u\n", cfg->port_out);

	fprintf (fp, "\n[Outputs]\n");
	fprintf (fp,
	         "\n# The cabinet's lamps and NASCAR Arcade's drive board commands, sent to the\n"
	         "# tools that drive real ones (MAMEHooker, BackForceFeeder, DOFLinx...) in\n"
	         "# MAME's network protocol, as Supermodel and sm2-emu send them: a TCP\n"
	         "# server on PORT, one \"name = value\" line at each change. Default: %s\n",
	         bool_name (def.outputs_network));
	fprintf (fp, "NETWORK = %s\n", bool_name (cfg->outputs_network));
	fprintf (fp,
	         "\n# The server's TCP port (MAME's 8000; another one for a second instance).\n"
	         "# Default: %u\n", def.outputs_port);
	fprintf (fp, "PORT = %u\n", cfg->outputs_port);
	fprintf (fp,
	         "\n# When a game starts, \"mame_start = <set>\" and the TCP port go once in a\n"
	         "# UDP datagram to this port on 127.0.0.1, as Supermodel sends them\n"
	         "# (BackForceFeeder waits for it); 0 sends none. Default: %u\n", def.outputs_announce_port);
	fprintf (fp, "ANNOUNCE_PORT = %u\n", cfg->outputs_announce_port);

	fprintf (fp, "\n[Telemetry]\n");
	fprintf (fp,
	         "\n# The racing games' telemetry, for motion simulators, dashboards and your\n"
	         "# own tools, as Supermodel and sm2-emu send it: one JSON line a frame to\n"
	         "# each client of a TCP server on this port; 0, none. Default: %u\n", def.telemetry_port);
	fprintf (fp, "PORT = %u\n", cfg->telemetry_port);
	fprintf (fp,
	         "\n# Forza Motorsport 7's \"Data Out\" datagrams (the Dash format) to HOST:PORT,\n"
	         "# which SimHub, FlyPT Mover, SimTools, Sim Racing Studio read (choose Forza\n"
	         "# Motorsport 7 there); empty, none. Default: (empty)\n");
	fprintf (fp, "FORZA = %s\n", cfg->telemetry_forza);

	fprintf (fp, "\n[ForceFeedback]\n");
	fprintf (fp,
	         "\n# All the forces, of every game, on a wheel and on a controller's motors, in\n"
	         "# percent (0 to 300); above 100 the strongest stop at what the device can\n"
	         "# do. Default: %u\n", def.ffb_gain);
	fprintf (fp, "GAIN = %u\n", cfg->ffb_gain);
	fprintf (fp,
	         "\n# For debugging: the effects' values at the top left, over the game.\n"
	         "# Default: %s\n", bool_name (def.ffb_hud));
	fprintf (fp, "HUD = %s\n", bool_name (cfg->ffb_hud));
	fprintf (fp,
	         "\n# Star Wars: Racer Arcade's cabinet has no motor: force feedback is made from\n"
	         "# what the game does (its engines, its hits, its turns, the ground), on a\n"
	         "# wheel (DirectInput) and on player 1's controller's motors. Default: %s\n",
	         bool_name (def.ffb_effects));
	fprintf (fp, "EFFECTS = %s\n", bool_name (cfg->ffb_effects));
	fprintf (fp,
	         "\n# Each family's strength, in percent (0 to 200): STEERING the centring, the\n"
	         "# weight of the turns and the damping; ENGINES the engines' rumble and the\n"
	         "# boost; HITS the walls and the pods hit; TERRAIN the bumps and the landings;\n"
	         "# RUMBLE a controller's motors as a whole. Default: %u\n", def.ffb_steering);
	fprintf (fp, "STEERING = %u\n", cfg->ffb_steering);
	fprintf (fp, "ENGINES = %u\n", cfg->ffb_engines);
	fprintf (fp, "HITS = %u\n", cfg->ffb_hits);
	fprintf (fp, "TERRAIN = %u\n", cfg->ffb_terrain);
	fprintf (fp, "RUMBLE = %u\n", cfg->ffb_rumble);
	fprintf (fp,
	         "\n# Vibrations the other cabinets did not make, in percent (0 to 200), on a\n"
	         "# controller and a wheel: in Planet Harriers, SHOTS a short jolt at each\n"
	         "# bullet, MISSILES a heavy one, BOMBS at a bomb and its explosion; NOZZLE, a gentle one\n"
	         "# while Brave Firefighters' nozzle sprays. RUMBLE and EFFECTS apply to them\n"
	         "# too. Default: %u\n", def.ffb_shots);
	fprintf (fp, "SHOTS = %u\n", cfg->ffb_shots);
	fprintf (fp, "MISSILES = %u\n", cfg->ffb_missiles);
	fprintf (fp, "BOMBS = %u\n", cfg->ffb_bombs);
	fprintf (fp, "NOZZLE = %u\n", cfg->ffb_nozzle);
	fprintf (fp,
	         "\n# The device that gets the effects, by its name as the Controls tab lists it:\n"
	         "# a wheel takes the forces, a controller its motors. AUTO: the first device\n"
	         "# with force feedback, and the motors of player 1's controller.\n");
	fprintf (fp, "DEVICE = %s\n", cfg->ffb_device[0] ? cfg->ffb_device : "AUTO");
	fprintf (fp,
	         "\n# Each effect's strength on a wheel alone, in percent (0 to 200), on top of\n"
	         "# its family's: WHEEL_SPRING the centring, WHEEL_FRICTION the damping,\n"
	         "# WHEEL_TURN the weight of the turns, WHEEL_KICK a hit's kick, WHEEL_ENGINES\n"
	         "# the engines' vibration, WHEEL_BOOST the boost's, WHEEL_SCRAPE a wall's,\n"
	         "# WHEEL_IMPACT a hit's, WHEEL_BUMPS the bumps' and the landings'.\n"
	         "# Default: %u, WHEEL_ENGINES %u\n", def.ffb_wheel_spring, def.ffb_wheel_engines);
	fprintf (fp, "WHEEL_SPRING = %u\n", cfg->ffb_wheel_spring);
	fprintf (fp, "WHEEL_FRICTION = %u\n", cfg->ffb_wheel_friction);
	fprintf (fp, "WHEEL_TURN = %u\n", cfg->ffb_wheel_turn);
	fprintf (fp, "WHEEL_KICK = %u\n", cfg->ffb_wheel_kick);
	fprintf (fp, "WHEEL_ENGINES = %u\n", cfg->ffb_wheel_engines);
	fprintf (fp, "WHEEL_BOOST = %u\n", cfg->ffb_wheel_boost);
	fprintf (fp, "WHEEL_SCRAPE = %u\n", cfg->ffb_wheel_scrape);
	fprintf (fp, "WHEEL_IMPACT = %u\n", cfg->ffb_wheel_impact);
	fprintf (fp, "WHEEL_BUMPS = %u\n", cfg->ffb_wheel_bumps);

	fprintf (fp, "\n[Input]\n");
	fprintf (fp,
	         "\n# The Controls page numbers a joystick's buttons from 1, as Windows and\n"
	         "# vJoy do (true), or from 0, as SDL and controls.ini do (false): the file\n"
	         "# keeps SDL's numbers either way. Default: %s\n", bool_name (def.buttons_from_one));
	fprintf (fp, "BUTTONS_FROM_ONE = %s\n", bool_name (cfg->buttons_from_one));
	fprintf (fp,
	         "\n# How far a wheel turns from one end to the other for the game's wheel\n"
	         "# (NASCAR Arcade): 0, its whole turn; 270, 360, 540, 900 or 1080 degrees,\n"
	         "# the game's wheel then at its end before the wheel's. A wheel's or a\n"
	         "# joystick's axis (JOY<n>_AXIS_<a>), not a game controller's stick. The\n"
	         "# force feedback's centring spring follows. Taken at once. Default: %d\n",
	         def.wheel_rotation);
	fprintf (fp, "WHEEL_ROTATION = %d\n", cfg->wheel_rotation);
	fprintf (fp,
	         "\n# The degrees the wheel turns from one end to the other, as its driver is\n"
	         "# set (Logitech G HUB, Thrustmaster's or Fanatec's control panel): what\n"
	         "# WHEEL_ROTATION is taken from, 90 to 3600. Default: %d\n", def.wheel_range);
	fprintf (fp, "WHEEL_RANGE = %d\n", cfg->wheel_range);
	fprintf (fp,
	         "\n# NASCAR Arcade's gear lever, an H shifter (controls.ini's Gear1 to Gear4):\n"
	         "# true, its lever in none of the gears is in neutral, as on the cabinet;\n"
	         "# false, the last gear stays until another or GearNeutral. Default: %s\n",
	         bool_name (def.shifter_neutral));
	fprintf (fp, "SHIFTER_NEUTRAL = %s\n", bool_name (cfg->shifter_neutral));
	fprintf (fp,
	         "\n# GearUp and GearDown from a gear to another go through neutral that\n"
	         "# long, as a real lever does, 0 to 500 ms. Default: %d\n", def.shift_neutral_ms);
	fprintf (fp, "SHIFT_NEUTRAL_MS = %d\n", cfg->shift_neutral_ms);
	fprintf (fp,
	         "\n# Star Wars: Racer Arcade's two levers, a pod engine's thrust each:\n"
	         "# ORIGINAL, an axis each, as on the cabinet (controls.ini's P1_LeftLever\n"
	         "# and P1_RightLever); COMBINED, the accelerator (P1_Gas) the thrust of both\n"
	         "# and the wheel (P1_Steer) the difference, the pod turning to the side\n"
	         "# steered. Default: %s\n", twin_levers_names[def.twin_levers]);
	fprintf (fp, "TWIN_LEVERS = %s\n", twin_levers_names[cfg->twin_levers]);
	fprintf (fp,
	         "\n# true, the joysticks, wheels and controllers go on working while another\n"
	         "# window has the focus, their hotkeys too; the keyboard only ever works\n"
	         "# with the focus. Default: %s\n", bool_name (def.background_input));
	fprintf (fp, "BACKGROUND_INPUT = %s\n", bool_name (cfg->background_input));

	/* The controls are controls.ini's (hl::Controls): the old [Controls]
	 * is read once, into it, and no longer written */
	return fclose (fp) == 0;
}

} /* namespace hl */
