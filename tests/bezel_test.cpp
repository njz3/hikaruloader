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

/* BEZEL's layouts (src/hl/bezel.cpp): a Batocera .info read, a screen found
 * in a picture's see-through pixels, and the game's picture placed in the
 * bezel's screen at the aspect ratio's settings, or no bezel where the
 * window is not wider than the frame. Run by ctest, or alone: bezeltest. */

#include <stdio.h>

#include <vector>

#include "hl/bezel.h"
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

/* A 1920x1080 bezel, its screen left, top, right, bottom away from its
 * edges, as a Batocera .info gives it */
static hl::BezelHole
hole_of (int left, int top, int right, int bottom)
{
	char json[256];
	hl::BezelHole hole;

	snprintf (json, sizeof (json),
	          "{ \"width\": 1920, \"height\": 1080, \"left\": %d, \"top\": %d, \"right\": %d,"
	          " \"bottom\": %d, \"opacity\": 1.0, \"messagex\": 0.22, \"messagey\": 0.12 }",
	          left, top, right, bottom);
	CHECK ("info read", hl::bezel_parse_info (json, 1920, 1080, &hole), 1);
	return hole;
}

static void
test_info (void)
{
	hl::BezelHole hole = hole_of (240, 0, 240, 0);

	CHECK ("info x", hole.x * 1920 + 0.5, 240);
	CHECK ("info w", hole.w * 1920 + 0.5, 1440);
	CHECK ("info h", hole.h * 1080 + 0.5, 1080);
	CHECK ("info size", hole.bezel_w, 1920);
	/* A .info for a 1280x720 copy of the picture: the same shares */
	CHECK ("info scaled", hl::bezel_parse_info ("{\"width\":1280,\"height\":720,\"left\":160,\"top\":0,"
	                                            "\"right\":160,\"bottom\":0}", 1920, 1080, &hole), 1);
	CHECK ("info scaled x", hole.x * 1920 + 0.5, 240);
	CHECK ("info scaled size", hole.bezel_w, 1920);
	CHECK ("info no screen", hl::bezel_parse_info ("{\"width\":100,\"height\":100,\"left\":60,\"right\":40}",
	                                               100, 100, &hole), 0);
	CHECK ("info not json", hl::bezel_parse_info ("width = 100", 100, 100, &hole), 0);
}

static void
test_find (void)
{
	/* 32x18, opaque, a see-through screen from 8,2 to 23,15 and a
	 * see-through corner apart (not joined to the middle) */
	const int w = 32, h = 18;
	std::vector<uint8_t> rgba (w * h * 4, 255);
	hl::BezelHole hole;

	for (int y = 2; y <= 15; y++)
		for (int x = 8; x <= 23; x++)
			rgba[(y * w + x) * 4 + 3] = y == 2 ? 128 : 0;	/* half see-through glass at the top */
	rgba[3] = 0;
	CHECK ("find", hl::bezel_find_hole (rgba.data (), w, h, &hole), 1);
	CHECK ("find x", hole.x * w + 0.5, 8);
	CHECK ("find y", hole.y * h + 0.5, 2);
	CHECK ("find w", hole.w * w + 0.5, 16);
	CHECK ("find h", hole.h * h + 0.5, 14);
	/* The middle opaque: no screen */
	rgba[(9 * w + 16) * 4 + 3] = 255;
	CHECK ("find opaque middle", hl::bezel_find_hole (rgba.data (), w, h, &hole), 0);
}

static void
test_layout (void)
{
	hl::ScreenLayout l;
	hl::Config *cfg;

	/* No bezel: screen_layout's, none drawn */
	settings (hl::ASPECT_ORIGINAL, false);
	hl::screen_layout (&l, 1280, 720, 640, 480);
	CHECK ("plain no bezel", l.bezel[2], 0);

	/* A 4:3 screen in a 16:9 bezel, a 16:9 window of its size: the
	 * picture in the screen, the bezel over the window */
	settings (hl::ASPECT_ORIGINAL, false);
	CHECK ("shown", hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (240, 0, 240, 0)), 1);
	check_rect ("4:3 frame", l.frame, 240, 0, 1440, 1080);
	check_rect ("4:3 image", l.image, 240, 0, 1440, 1080);
	check_rect ("4:3 bezel", l.bezel, 0, 0, 1920, 1080);

	/* Smaller, the window 1280x720: all two thirds as large */
	hl::bezel_layout (&l, 1280, 720, 640, 480, hole_of (240, 0, 240, 0));
	check_rect ("2/3 frame", l.frame, 160, 0, 960, 720);
	check_rect ("2/3 bezel", l.bezel, 0, 0, 1280, 720);

	/* A screen wider than 4:3, off the middle and above it (y from the
	 * top in the .info, from the bottom in the layout): the picture at
	 * its shape in the middle of the screen, bars in it */
	hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (200, 40, 400, 80));
	/* The screen 1320x960 at 200,40 (from the top): 1280x960 in it */
	check_rect ("off frame", l.frame, 220, 80, 1280, 960);
	check_rect ("off image", l.image, 220, 80, 1280, 960);

	/* A 21:9 window: the bezel as tall as it, in its middle */
	hl::bezel_layout (&l, 2560, 1080, 640, 480, hole_of (240, 0, 240, 0));
	check_rect ("21:9 bezel", l.bezel, 320, 0, 1920, 1080);
	check_rect ("21:9 frame", l.frame, 560, 0, 1440, 1080);

	/* Whole multiples in the screen: 2x, 1280x960, in its middle */
	cfg = settings (hl::ASPECT_ORIGINAL, false);
	cfg->integer_scale = true;
	hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (240, 0, 240, 0));
	check_rect ("integer image", l.image, 320, 60, 1280, 960);

	/* No room: a 4:3 window, a stretched display mode, a 16:9 frame in a
	 * 16:9 window, no screen; the plain layout then */
	settings (hl::ASPECT_ORIGINAL, false);
	CHECK ("4:3 window", hl::bezel_layout (&l, 640, 480, 640, 480, hole_of (240, 0, 240, 0)), 0);
	check_rect ("4:3 window frame", l.frame, 0, 0, 640, 480);
	CHECK ("4:3 window no bezel", l.bezel[2], 0);
	settings (hl::ASPECT_ORIGINAL, true);
	CHECK ("stretched mode", hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (240, 0, 240, 0)), 0);
	settings (hl::ASPECT_16_9, false);
	CHECK ("16:9 frame", hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (240, 0, 240, 0)), 0);
	settings (hl::ASPECT_16_9_STRETCH, false);
	CHECK ("16:9 stretched", hl::bezel_layout (&l, 1920, 1080, 640, 480, hole_of (240, 0, 240, 0)), 0);
	check_rect ("16:9 stretched frame", l.frame, 0, 0, 1920, 1080);
	settings (hl::ASPECT_ORIGINAL, false);
	CHECK ("no screen", hl::bezel_layout (&l, 1920, 1080, 640, 480, hl::BezelHole ()), 0);
	check_rect ("no screen frame", l.frame, 240, 0, 1440, 1080);
}

int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;

	test_info ();
	test_find ();
	test_layout ();

	printf ("bezel: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
