/*
 * Hikaruloader
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

/* The aspect ratio's layouts (src/hl/aspect.cpp): the frame and the picture
 * in a window, letterboxed, stretched, cropped, at whole multiples; a CRT's
 * 1920x240 with a stretched frame. Run by ctest, or alone: aspecttest. */

#include <stdio.h>

#include "hl/aspect.h"
#include "hl/config.h"

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		int g_ = (int) (got_), w_ = (int) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %d, expected %d\n", name_, #got_, g_, w_); \
		} \
	} while (0)

/* A rectangle, origin at the bottom left */
static void
check_rect (const char *name, const int *r, int x, int y, int w, int h)
{
	CHECK (name, r[0], x);
	CHECK (name, r[1], y);
	CHECK (name, r[2], w);
	CHECK (name, r[3], h);
}

/* The settings from their defaults, then an aspect ratio and a stretch */
static hl::Config *
settings (int aspect, bool stretch)
{
	hl::Config *cfg = hl::Config::get ();
	cfg->set_defaults ();
	cfg->aspect = aspect;
	cfg->display_stretch = stretch;
	return cfg;
}

int
main (int argc, char **argv)
{
	hl::ScreenLayout l;
	hl::Config *cfg;

	(void) argc;
	(void) argv;

	/* 4:3 in a 16:9 window: bars at the sides */
	settings (hl::ASPECT_ORIGINAL, false);
	hl::screen_layout (&l, 1280, 720, 640, 480);
	check_rect ("original frame", l.frame, 160, 0, 960, 720);
	check_rect ("original image", l.image, 160, 0, 960, 720);
	CHECK ("original barrel", l.barrel_axis, -1);

	/* Stretched: the frame and the picture fill the window */
	settings (hl::ASPECT_ORIGINAL, true);
	hl::screen_layout (&l, 1280, 720, 640, 480);
	check_rect ("stretch frame", l.frame, 0, 0, 1280, 720);
	check_rect ("stretch image", l.image, 0, 0, 1280, 720);

	/* 16:9 with black bars in a 4:3 window: the frame 640x360, the picture
	 * 480x360 in it */
	settings (hl::ASPECT_16_9, false);
	hl::screen_layout (&l, 640, 480, 640, 480);
	check_rect ("16:9 frame", l.frame, 0, 60, 640, 360);
	check_rect ("16:9 image", l.image, 80, 60, 480, 360);

	/* 16:9 cropped in a 16:9 window: the picture 1280x960, its middle */
	settings (hl::ASPECT_16_9_CROP, false);
	hl::screen_layout (&l, 1280, 720, 640, 480);
	check_rect ("crop frame", l.frame, 0, 0, 1280, 720);
	check_rect ("crop image", l.image, 0, -120, 1280, 960);
	/* Its top kept (-100), the bottom cut */
	cfg = settings (hl::ASPECT_16_9_CROP, false);
	cfg->crop_position = -100;
	hl::screen_layout (&l, 1280, 720, 640, 480);
	check_rect ("crop top", l.image, 0, -240, 1280, 960);
	/* Bent: the vertical axis, the whole picture shown */
	cfg = settings (hl::ASPECT_16_9_CROP, false);
	cfg->crop_barrel = true;
	hl::screen_layout (&l, 1280, 720, 640, 480);
	CHECK ("barrel axis", l.barrel_axis, 1);
	CHECK ("barrel lengths x1000", (l.length[0] + l.length[1] + l.center) * 1000.0f + 0.5f, 1333);

	/* Whole multiples: 1x in a 1000x800 window */
	cfg = settings (hl::ASPECT_ORIGINAL, false);
	cfg->integer_scale = true;
	hl::screen_layout (&l, 1000, 800, 640, 480);
	check_rect ("integer image", l.image, 180, 160, 640, 480);

	/* A 16:9 CRT at 1920x240: 16:9 with bars, stretched over the screen,
	 * the 4:3 picture on three quarters of its width */
	settings (hl::ASPECT_16_9, true);
	hl::screen_layout (&l, 1920, 240, 640, 480);
	check_rect ("crt 16:9 frame", l.frame, 0, 0, 1920, 240);
	check_rect ("crt 16:9 image", l.image, 240, 0, 1440, 240);
	/* A 4:3 CRT: the picture over all of it; widescreen's 854x480 with
	 * bars above and below */
	settings (hl::ASPECT_4_3, true);
	hl::screen_layout (&l, 1920, 240, 640, 480);
	check_rect ("crt 4:3 image", l.image, 0, 0, 1920, 240);
	hl::screen_layout (&l, 1920, 240, 854, 480);
	check_rect ("crt 4:3 wide image", l.image, 0, 30, 1920, 180);

	printf ("aspecttest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
