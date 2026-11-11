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

#ifndef __HL_CONFIG_H__
#define __HL_CONFIG_H__

#include <cstdio>
#include <string>
#include <vector>

#include "hl/core.h"
#include "hl/input.h"

namespace hl {

enum DisplayMode {
	DISPLAY_WINDOW,
	DISPLAY_FULLSCREEN,
	DISPLAY_BORDERLESS,

	NUM_DISPLAY_MODES
};

/* [Display] MULTI_SCREEN: the cabinets of several boards */
enum {
	MULTI_ONE,		/* one board's picture, the key changing it */
	MULTI_SIDE_BY_SIDE,	/* all of them in a row in the window */
	MULTI_WINDOWS,		/* each in a window of its own */
	NUM_MULTI_SCREEN
};

enum { MAX_SCREENS = 3 };

/* ASPECT_RATIO, as linuxloader, Supermodel, Flycast and sm2-emu name it
 * (hl/aspect.h): the frame the picture is shown in, and how it fills it */
enum Aspect {
	ASPECT_ORIGINAL,	/* the picture's own shape */
	ASPECT_4_3,		/* the whole picture, black bars */
	ASPECT_16_9,
	ASPECT_4_3_STRETCH,	/* the picture fills it, deformed */
	ASPECT_16_9_STRETCH,
	ASPECT_4_3_CROP,	/* the picture fills it at its shape, cut */
	ASPECT_16_9_CROP,

	NUM_ASPECTS
};

/* RENDER_SCALE's most: 3840x2880, 4K's class */
static const int MAX_RENDER_SCALE = 6;

/* WHEEL_ROTATION's choices, Odin's: the degrees the player's wheel turns for
 * the game's wheel from one end to the other, 0 its whole turn */
static const int WHEEL_ROTATIONS[] = { 0, 270, 360, 540, 900, 1080 };
/* WHEEL_RANGE's: the turns the wheels' drivers offer (Logitech's 900,
 * Thrustmaster's and Fanatec's 1080, a direct drive's more) */
static const int WHEEL_RANGES[] = { 180, 270, 360, 540, 720, 900, 1080, 1440, 1800, 2520 };

/* The 2D layers in widescreen: 4:3 in the middle, or stretched to 16:9 */
enum Widescreen2d {
	WIDESCREEN_2D_CENTERED,
	WIDESCREEN_2D_STRETCHED,

	NUM_WIDESCREEN_2D
};

/* How the picture is scaled to the window: sharp, or smooth (fewer rows,
 * 240 on a CRT: each one the mean of two instead of one in two dropped) */
enum ScaleFilter {
	FILTER_NEAREST,
	FILTER_LINEAR,

	NUM_FILTERS
};

/* BEZEL: what is around the picture where the window is wider than its
 * frame (hl/bezel.h) */
enum BezelMode {
	BEZEL_OFF,	/* black, as before */
	BEZEL_BLUR,	/* the picture's own edges, blurred and dimmed */
	BEZEL_IMAGE,	/* the game's bezel picture, Batocera's format */

	NUM_BEZEL_MODES
};

/* The graphics API the window and the renderers use. Only OpenGL is there
 * for now; Vulkan 1.3 and Direct3D 11 are planned. */
enum GraphicsApi {
	GRAPHICS_OPENGL,
	GRAPHICS_VULKAN,
	GRAPHICS_D3D11,

	NUM_GRAPHICS_APIS
};

/* How the CPUs find their memory (Mmap): the regions one after the other,
 * a table of pages, or (planned) the RAM mapped in the host's address
 * space, the devices reached through the faults */
enum MemoryAccess {
	MEMORY_REGIONS,
	MEMORY_PAGE_TABLE,
	MEMORY_FASTMEM,

	NUM_MEMORY_ACCESSES
};

/* [Network] COMM_BOARD: the communication board simulated (its program's
 * work done by Hikaruloader, two cabinets), or its 68000 running the
 * program the game uploads (hikaru-commbd.cpp, up to 8) */
enum CommBoard {
	COMM_BOARD_SIMULATED,
	COMM_BOARD_PROGRAM,

	NUM_COMM_BOARDS
};

/* Star Wars: Racer Arcade's two levers, a pod engine's thrust each: an axis
 * of the player's each, as on the cabinet, or worked out from the
 * accelerator (the thrust of both) and the wheel (the difference) */
enum TwinLevers {
	TWIN_LEVERS_ORIGINAL,
	TWIN_LEVERS_COMBINED,

	NUM_TWIN_LEVERS
};

/* The settings of hikaruloader.ini, edited by the settings window */
class Config {
public:
	/* The program's settings, the defaults until load () */
	static Config	*get ();

	/* The file the settings are read from and saved to */
	static const char *path ();
	static void	 set_path (const char *path);

	void		 set_defaults ();
	/* A missing file leaves the defaults; true when the file was read */
	bool		 load ();
	bool		 save () const;

	/* A game's own settings: games/<set>.ini beside hikaruloader.ini (the
	 * write directory's, else the one beside the program), only the values
	 * that differ from the common ones, read over them while it runs.
	 * begin_game () before its machine is made, end_game () after */
	static void	 begin_game (const char *name);
	static void	 end_game ();
	/* The game running ("" none), and whether it has a file of its own */
	static const char *game ();
	static bool	 game_has_file ();
	/* The settings now, those that differ from the common ones, into the
	 * game's file; the common ones then stay as they were */
	static bool	 save_for_game ();
	/* The settings now into hikaruloader.ini, but those the game's file
	 * sets, which keep their common value there */
	static bool	 save_common ();
	/* The game's file removed, the common settings back for it */
	static void	 use_common_settings ();
	/* A setting the game's file sets (section, key) */
	static bool	 set_by_game (const char *section, const char *key);
	/* Those a game cannot have of its own: the folders, the network... */
	static bool	 common_only (const char *section, const char *key);
	/* Every setting as the file has it: section, key, value */
	struct Entry {
		std::string section, key, value;
	};
	std::vector<Entry> entries () const;
	bool		 write (FILE *fp) const;

	/* The games folders in use: games_dir (one, or several separated by
	 * ';'), or "roms" beside the program; rom_archive's folder before them */
	void		 games_dir_in_use (char *dst, size_t size) const;

	/* [Display] */
	int		multi_screen;
	/* The boards with a monitor, left to right ("2,1,3"), for games of
	 * several boards; else the game's own order */
	char		screen_order[32];
	/* Each screen's monitor (hl::Window::find_display); empty or AUTO */
	char		screen_display[MAX_SCREENS][64];
	int		display_mode;
	/* --display-mode: name (WINDOW, FULLSCREEN or BORDERLESS, each also
	 * _STRETCH) in use for this start, in place of the file's, which save ()
	 * keeps unless the setting is changed in the meantime. False for
	 * another name */
	bool		override_display_mode (const char *name);
	bool		display_overridden = false;
	int		display_mode_file = 0, display_mode_cli = 0;
	bool		display_stretch_file = false, display_stretch_cli = false;
	bool		display_stretch; /* DISPLAY_MODE's _STRETCH: the frame
					  * fills the window or the screen */
	int		window_scale;	/* 1 ... 4 times 640x480 */
	int		aspect;
	int		crop_position;	/* the part a crop keeps: -100 left or
					 * top, 0 the middle, 100 right or
					 * bottom */
	bool		crop_barrel;	/* the parts a crop cuts bent onto the
					 * edges */
	int		barrel_center;	/* the middle undistorted, % of the
					 * frame */
	int		barrel_radius;	/* the cylinder's radius, % of the
					 * frame */
	bool		integer_scale;	/* whole multiples of the picture */
	/* The 3D drawn at 1 ... MAX_RENDER_SCALE times its size, at once */
	int		render_scale;
	int		graphics_api;	/* takes effect at the next start */
	bool		vsync;
	bool		fps_limiter;
	float		fps_target;
	bool		fps_overlay;
	bool		title_frame;	/* the frame number in the title */
	bool		hide_cursor;
	/* A click in the game's window keeps the mouse in it, hidden, until
	 * Tab, Alt, the Windows key or the focus lost */
	bool		capture_mouse;
	/* The frames drawn by a thread of their own, a frame later; taken at
	 * the game's start */
	bool		threaded_rendering;
	/* The translucent polygons sorted at each pixel (OpenGL 4.3, without
	 * MSAA), else each mesh as a whole; taken at once */
	bool		sort_translucent_pixels;
	/* The 3D at 16:9 (854x480), the 2D layers as widescreen_2d says;
	 * taken at the game's start */
	bool		widescreen;
	int		widescreen_2d;
	int		scale_filter;
	/* The 3D scenery's distance, 1 to 4 times the game's: its levels of
	 * detail and its fog that much farther */
	float		draw_distance;
	int		bezel;		/* a BezelMode, taken at once */

	/* [Enhancements]: beyond the board's own picture, all off by default,
	 * taken at once */
	int		msaa;		/* its samples a pixel: 0 (none), 2, 4 or 8 */
	int		anisotropy;	/* the textures' filtering: 1 (none) ... 16 */
	int		texture_scale;	/* 1, or 2 and 4 by Scale2x */
	bool		fxaa;		/* the 3D's edges smoothed */
	int		bloom;		/* 0 ... 100 %: a glow around the bright */
	int		crt_filter;	/* 0 ... 100 %: a CRT's scanlines and mask */
	/* A CRT's look by a libretro preset's passes (hl/crt_preset.h): its
	 * name in data/crt ("lola-retro/sega-monitor-29") or a .glslp file;
	 * empty: none. It takes CRT_FILTER's place */
	char		crt_preset[512];

	/* [Picker] */
	/* Empty: "roms" beside the program; several separated by ';', looked
	 * in in that order (hl::RomSet) */
	char		games_dir[2048];
	/* -z: the zip or 7z holding this start's game, whatever its name; never
	 * saved. Its folder is the first looked in, for the BIOS and the CHD */
	char		rom_archive[1024] = "";
	/* All that is written goes there (hl::Paths); empty: none. The
	 * hikaruloader.ini beside the program's alone counts */
	char		write_dir[512];
	char		language[8];	/* "auto", "fr" or "en" */
	/* hikaruloader.log written, at the next start: beside the program,
	 * or at the write directory's root; replaced at each start */
	bool		log_file;
	/* The picker's pictures downloaded from the Arcade Database
	 * (ui/ui_artwork.h); off, the artwork folder's alone */
	bool		artwork_download;
	char		last_game[32];

	/* [Emulation] */
	bool		dynarec;	/* the CPUs' dynamic recompilers, or
					 * their interpreters; from the next
					 * frame on */
	MemoryAccess	memory_access;	/* how the CPUs find their memory;
					 * from the next frame on */
	unsigned	sh4_clock;	/* the SH-4s' clock in MHz, 20 to 200
					 * (the board's); at the game's start */
	/* The one in use, in its range */
	static unsigned	 sh4_clock_mhz ();
	unsigned	naomi_sh4_clock; /* the NAOMI's and Atomiswave's, as it */
	bool		invert_stick_y; /* The analog sticks' up and down */
	static unsigned	 naomi_sh4_clock_mhz ();
	int		region;		/* the board's country: -1 the EEPROM's
					 * own, else 0 Japan, 1 USA, 2 Export,
					 * 3 Australia, 4 Korea; at the game's
					 * start */
	/* The low-level emulations, off by default; at the game's start. The
	 * JVS I/O board's CPU running its own program (315-6215, epr-21868)
	 * instead of its simulation (hl/ioboard.h) */
	bool		io_board_program;
	/* The security chips answering as the boards' do: the ROM board's
	 * X76F100 and the main board's 93C46 from their images, SGNASCAR's
	 * Actel FPGA as the chip (hikaru-memctl.cpp) */
	bool		security_chips;

	/* [Sound] */
	int		sound_output;	/* an Audio::Output: 0 the sound card's
					 * own speakers, else as many channels;
					 * from the next frame on */
	int		sound_balance;	/* 0 the front alone, 100 the rear
					 * alone, 50 both at their full level */
	int		sound_volume;	/* percents */
	int		attract_sound;	/* the attract's sound: -1 the EEPROM's
					 * own, else 0 off, 1 on; at the
					 * game's start */
	int		sound_latency;	/* the output's queue, 20 to 200 ms */
	bool		sound_optimized; /* the AICAs' registers kept decoded
					 * (OPTIMIZED), or decoded at each
					 * sample (INTERPRETED); at once */

	/* [Network] */
	int		link;		/* the BIOS's COMMUNICATION MODE: -1 the
					 * EEPROM's own, else 0 no link, 1
					 * master, 2 slave, 3 relay; at the
					 * game's start */
	unsigned	link_node;	/* a NAOMI slave's number on the ring (1 to 7:
					 * its car, its seat), at the game's start */
	int		comm_board;	/* CommBoard, at the game's start */
	unsigned	port_in;	/* the previous cabinet comes in there */
	char		address_out[64];/* the next cabinet, and its port */
	unsigned	port_out;
	/* The Net DIMM boards' network (naomi::DimmNet), at the game's start:
	 * the board's address, where the addresses the game uses are, what
	 * its bound ports are moved by, whether other machines can reach
	 * it (else 127.0.0.1 alone), the address of the board's TCP service */
	char		dimm_ip[32];
	char		dimm_map[512];
	int		dimm_port_offset;
	bool		dimm_listen;
	char		dimm_service_ip[32];

	/* [Outputs]: the lamps and the drive board's commands, for the tools
	 * that drive real ones (hl::Outputs); at once */
	bool		outputs_network;	/* MAME's network protocol */
	unsigned	outputs_port;		/* its TCP port */
	unsigned	outputs_announce_port;	/* the UDP announcement's, 0
						 * none */

	/* [Telemetry]: the racing games' telemetry (hl::telemetry), for
	 * motion simulators and dashboards; at a game's start */
	unsigned	telemetry_port;		/* its JSON TCP server's, 0 off */
	char		telemetry_forza[128];	/* Forza Data Out's HOST:PORT, ""
						 * off */

	/* [ForceFeedback]: the effects worked out from the telemetry of a
	 * game whose cabinet has no motor (Star Wars: Racer Arcade:
	 * hl::FfbEffects), on a wheel (DirectInput) and a controller's
	 * motors; each family's strength, a percentage */
	bool		ffb_effects;
	unsigned	ffb_steering, ffb_engines, ffb_hits, ffb_terrain, ffb_rumble;
	/* Each effect's strength on a wheel alone, over its family's */
	unsigned	ffb_wheel_spring, ffb_wheel_friction, ffb_wheel_turn, ffb_wheel_kick;
	unsigned	ffb_wheel_engines, ffb_wheel_boost, ffb_wheel_scrape, ffb_wheel_impact, ffb_wheel_bumps;
	/* hl::GameRumble: Planet Harriers' shots, Brave Firefighters' nozzle */
	unsigned	ffb_shots, ffb_missiles, ffb_bombs, ffb_nozzle;
	/* The device to use, by its name; "" the first with force feedback,
	 * and the motors of player 1's controller */
	char		ffb_device[128];
	/* All the forces, every game's, to a wheel and a controller's motors:
	 * a percentage, 0 to 300 (hl::ForceFeedback) */
	unsigned	ffb_gain;
	/* The effects' values over the game, for debugging (ui_overlay) */
	bool		ffb_hud;

	/* [Input] */
	bool		buttons_from_one; /* a joystick's buttons shown from 1,
					   * as Windows and vJoy number them;
					   * controls.ini keeps SDL's, from 0 */
	/* The degrees the player's wheel turns for the game's wheel from one
	 * end to the other: 0 its whole turn, else that many of the
	 * wheel_range degrees it turns end to end, as its driver is set */
	int		wheel_rotation;
	int		wheel_range;
	/* NASCAR Arcade's gear lever (hl::Gearbox): an H shifter in none of
	 * its gears in neutral, else the last gear kept; a shift from a gear
	 * to another through neutral that long, in ms (as sm2-emu) */
	bool		shifter_neutral;
	int		shift_neutral_ms;
	TwinLevers	twin_levers;
	/* The joysticks, wheels and controllers read without the window's
	 * focus; the keyboard never is (hl::Controls) */
	bool		background_input;
	/* How many times further than its axis the game's wheel goes: 1 for
	 * rotation 0, range / rotation else, never below 1 (the game's wheel
	 * always reaches its ends) */
	static double	 wheel_scale (int rotation, int range);
	double		 wheel_scale () const { return wheel_scale (wheel_rotation, wheel_range); }


private:
	void		 set_value (const char *section, const char *key, const char *value);
};

} /* namespace hl */

#endif /* __HL_CONFIG_H__ */
