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

#ifndef __HL_ASPECT_H__
#define __HL_ASPECT_H__

namespace hl {

/* Where the machine's picture goes in the window, after [Display]'s
 * ASPECT_RATIO and its companions, with the names and meanings linuxloader,
 * Supermodel, Flycast and sm2-emu give them: the frame the picture is shown
 * in (its shape, in the middle of the window, or filling it with
 * DISPLAY_MODE's _STRETCH), and how the picture fills it: whole with black
 * bars, stretched, or cropped (CROP_POSITION the part kept), the cropped
 * parts possibly bent onto the edges as on a cylinder seen from the front
 * (CROP_BARREL, BARREL_CENTER, BARREL_RADIUS). Also INTEGER_SCALE, and
 * SCREEN_SHAPE for a CRT's super resolutions. */
struct ScreenLayout {
	/* In the target's pixels, origin at the bottom left (OpenGL's): the
	 * frame, what shows (black around it); the whole picture, larger than
	 * the frame along the axis a crop cuts */
	int	 frame[4];	/* x, y, width, height */
	int	 image[4];
	/* Bent edges along one axis (0 across, 1 up and down, -1 none), in
	 * frame units from its left or top edge (see aspect.cpp) */
	int	 barrel_axis;
	float	 edge;		/* each edge's width: (1 - center) / 2 */
	float	 center;	/* the undistorted middle's width */
	float	 start;		/* the picture at the middle's start */
	float	 scale;		/* frame / picture along the axis */
	float	 length[2];	/* the picture each edge shows */
	float	 angle[2];	/* the arc of the cylinder each edge covers */
};

/* The layout in a target of tw x th pixels for a picture of pw x ph */
void	 screen_layout (ScreenLayout *l, int tw, int th, int pw, int ph);

} /* namespace hl */

#endif /* __HL_ASPECT_H__ */
