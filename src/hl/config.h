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

#include "hl/core.h"
#include "hl/input.h"

namespace hl {

enum DisplayMode {
	DISPLAY_WINDOW,
	DISPLAY_FULLSCREEN,
	DISPLAY_BORDERLESS,

	NUM_DISPLAY_MODES
};

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

/* The screen's own shape, which the aspect ratio places the picture by:
 * AUTO, square pixels (its resolution's shape); 4:3 or 16:9 whatever its
 * resolution (a CRT's super resolutions, 1920x240) */
enum ScreenShape {
	SCREEN_AUTO,
	SCREEN_4_3,
	SCREEN_16_9,

	NUM_SCREEN_SHAPES
};

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

	/* The games folders in use: games_dir (one, or several separated by
	 * ';'), or "roms" beside the program */
	void		 games_dir_in_use (char *dst, size_t size) const;

	/* [Display] */
	int		display_mode;
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
	/* The frames drawn by a thread of their own, a frame later; taken at
	 * the game's start */
	bool		threaded_rendering;
	/* The 3D at 16:9 (854x480), the 2D layers as widescreen_2d says;
	 * taken at the game's start */
	bool		widescreen;
	int		widescreen_2d;
	int		screen_shape;
	int		scale_filter;
	/* The 3D scenery's distance, 1 to 4 times the game's: its levels of
	 * detail and its fog that much farther */
	float		draw_distance;

	/* [Enhancements]: beyond the board's own picture, all off by default,
	 * taken at once */
	int		msaa;		/* its samples a pixel: 0 (none), 2, 4 or 8 */
	int		anisotropy;	/* the textures' filtering: 1 (none) ... 16 */
	int		texture_scale;	/* 1, or 2 and 4 by Scale2x */
	bool		fxaa;		/* the 3D's edges smoothed */
	int		bloom;		/* 0 ... 100 %: a glow around the bright */
	int		crt_filter;	/* 0 ... 100 %: a CRT's scanlines and mask */

	/* [Picker] */
	/* Empty: "roms" beside the program; several separated by ';', looked
	 * in in that order (hl::RomSet) */
	char		games_dir[2048];
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
	bool		mie_hack;	/* fakes the I/O board, as MIE_HACK=1 */
	bool		dynarec;	/* the CPUs' dynamic recompilers, or
					 * their interpreters; from the next
					 * frame on */
	MemoryAccess	memory_access;	/* how the CPUs find their memory;
					 * from the next frame on */
	unsigned	sh4_clock;	/* the SH-4s' clock in MHz, 20 to 200
					 * (the board's); at the game's start */
	/* The one in use, in its range */
	static unsigned	 sh4_clock_mhz ();
	int		region;		/* the board's country: -1 the EEPROM's
					 * own, else 0 Japan, 1 USA, 2 Export,
					 * 3 Australia, 4 Korea; at the game's
					 * start */

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
	unsigned	port_in;	/* the previous cabinet comes in there */
	char		address_out[64];/* the next cabinet, and its port */
	unsigned	port_out;

	/* [Outputs]: the lamps and the drive board's commands, for the tools
	 * that drive real ones (hl::Outputs); at once */
	bool		outputs_network;	/* MAME's network protocol */
	unsigned	outputs_port;		/* its TCP port */
	unsigned	outputs_announce_port;	/* the UDP announcement's, 0
						 * none */

	/* [Input] */
	bool		buttons_from_one; /* a joystick's buttons shown from 1,
					   * as Windows and vJoy number them;
					   * controls.ini keeps SDL's, from 0 */

	/* The old [Controls], read but no longer written: carried over into a
	 * new controls.ini (hl::Controls), which holds the controls now */
	Binding		bindings[NUM_ACTIONS];

private:
	void		 set_value (const char *section, const char *key, const char *value);
};

} /* namespace hl */

#endif /* __HL_CONFIG_H__ */
