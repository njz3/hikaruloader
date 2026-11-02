/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Supermodel's Src/Graphics/AspectRatio (njz3's fork,
 * GPL v3 or later).
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

/* The frame and the picture's place in it (see aspect.h). The bent edges,
 * along the cropped axis, in frame units: a crop keeps a part k of the
 * picture (k = frame / picture), so the picture is 1/k frames long. The
 * undistorted middle (BARREL_CENTER) shows the middle of the crop window at
 * the crop's scale. On each side, L of the picture is left to show in an
 * edge D = (1 - middle) / 2 wide: a point x into the edge (0 at the middle,
 * D at the frame's border) shows the arc s = L * asin (sin (t) * x / D) / t,
 * t = min (L / R, pi/2) and R the radius (BARREL_RADIUS / 100): the picture
 * wrapped on a cylinder seen from the front. L = D: a plain copy.
 * CROP_POSITION moves the crop window, so the middle, and each edge has its
 * own L. The final pass's shader does the same per pixel
 * (hikaru-renderer.cpp). */

#include <algorithm>
#include <cmath>

#include "hl/aspect.h"
#include "hl/config.h"

namespace hl {

enum Fit {
	FIT_ORIGINAL,	/* the frame has the picture's shape */
	FIT_LETTERBOX,	/* the whole picture, black bars where the shapes differ */
	FIT_STRETCH,	/* the picture fills the frame, deformed */
	FIT_CROP	/* the picture fills the frame at its shape, the rest cut */
};

/* In ASPECT_RATIO's order (config.h) */
static const struct {
	Fit	 fit;
	float	 shape;
} kinds[NUM_ASPECTS] = {
	{ FIT_ORIGINAL,  0.0f },
	{ FIT_LETTERBOX, 4.0f / 3.0f },
	{ FIT_LETTERBOX, 16.0f / 9.0f },
	{ FIT_STRETCH,   4.0f / 3.0f },
	{ FIT_STRETCH,   16.0f / 9.0f },
	{ FIT_CROP,      4.0f / 3.0f },
	{ FIT_CROP,      16.0f / 9.0f },
};

/* A rectangle (origin at the top left) to the target's pixels (origin at
 * the bottom left); within a pixel of the target's edges, all of it: 854x480
 * is a hair wider than 16:9, and left a black row on a 16:9 screen */
static void
to_pixels (int r[4], float x, float y, float w, float h, int tw, int th)
{
	int x0 = (int) lroundf (x), x1 = (int) lroundf (x + w);
	int y0 = (int) lroundf (y), y1 = (int) lroundf (y + h);

	if (x0 >= 0 && x0 <= 1 && x1 >= tw - 1 && x1 <= tw)
		x0 = 0, x1 = tw;
	if (y0 >= 0 && y0 <= 1 && y1 >= th - 1 && y1 <= th)
		y0 = 0, y1 = th;
	r[0] = x0;
	r[1] = th - y1;
	r[2] = MAX2 (1, x1 - x0);
	r[3] = MAX2 (1, y1 - y0);
}

void
screen_layout (ScreenLayout *l, int tw, int th, int pw, int ph)
{
	Config *cfg = Config::get ();
	int aspect = std::clamp (cfg->aspect, 0, NUM_ASPECTS - 1);
	Fit fit = kinds[aspect].fit;
	float picture = (float) MAX2 (pw, 1) / MAX2 (ph, 1);
	float shape = fit == FIT_ORIGINAL ? picture : kinds[aspect].shape;
	float W = (float) MAX2 (tw, 1), H = (float) MAX2 (th, 1);
	float fw, fh, fx, fy, iw = 1.0f, ih = 1.0f, ix, iy, place;
	float image[4];

	/* The frame: the whole screen when stretched, else the largest of its
	 * shape in the middle */
	fw = W;
	fh = H;
	if (!cfg->display_stretch) {
		if (W / H > shape)
			fw = H * shape;
		else
			fh = W / shape;
	}
	fx = (W - fw) / 2.0f;
	fy = (H - fh) / 2.0f;

	/* The picture in frame units (the frame 1 x 1 whatever its shape) */
	if (fit == FIT_LETTERBOX) {
		if (picture < shape)
			iw = picture / shape;
		else
			ih = shape / picture;
	} else if (fit == FIT_CROP) {
		if (picture < shape)
			ih = shape / picture;
		else
			iw = picture / shape;
	}
	place = fit == FIT_CROP ? (std::clamp (cfg->crop_position, -100, 100) + 100) / 200.0f : 0.5f;
	ix = (1.0f - iw) * place;
	iy = (1.0f - ih) * place;
	image[0] = fx + ix * fw;
	image[1] = fy + iy * fh;
	image[2] = iw * fw;
	image[3] = ih * fh;

	/* INTEGER_SCALE: whole multiples of the picture, of square pixels,
	 * where it is whole and undeformed; smaller than 1x, as it was */
	if (cfg->integer_scale && (fit == FIT_ORIGINAL || fit == FIT_LETTERBOX) &&
	    !cfg->display_stretch) {
		int k = (int) floorf (MIN2 (image[2] / MAX2 (pw, 1), image[3] / MAX2 (ph, 1)) + 0.001f);
		if (k >= 1) {
			image[0] += (image[2] - (float) pw * k) / 2.0f;
			image[1] += (image[3] - (float) ph * k) / 2.0f;
			image[2] = (float) pw * k;
			image[3] = (float) ph * k;
		}
	}

	/* Bent edges along the cropped axis */
	l->barrel_axis = -1;
	if (fit == FIT_CROP && cfg->crop_barrel) {
		int axis = iw > 1.0001f ? 0 : (ih > 1.0001f ? 1 : -1);
		float center = std::clamp (cfg->barrel_center, 0, 100) / 100.0f;
		if (axis >= 0 && center < 1.0f) {
			float len = axis == 0 ? iw : ih;
			float radius = MAX2 (cfg->barrel_radius, 1) / 100.0f;
			l->barrel_axis = axis;
			l->center = center;
			l->edge = (1.0f - center) / 2.0f;
			l->start = -(axis == 0 ? ix : iy) + l->edge;
			l->scale = 1.0f / len;
			l->length[0] = l->start;
			l->length[1] = len - (l->start + center);
			for (int i = 0; i < 2; i++)
				l->angle[i] = MIN2 (l->length[i] / radius, 1.57079633f);
		}
	}

	to_pixels (l->frame, fx, fy, fw, fh, tw, th);
	to_pixels (l->image, image[0], image[1], image[2], image[3], tw, th);
}

} /* namespace hl */
