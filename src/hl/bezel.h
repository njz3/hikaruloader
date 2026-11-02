/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/video/display_bezel.cpp and picture.hpp,
 * Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __HL_BEZEL_H__
#define __HL_BEZEL_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hl/aspect.h"

namespace hl {

/* BEZEL = IMAGE ([Display]): a picture laid over the window where it is
 * wider than the frame, the game's picture in its screen. Batocera's
 * format: artwork/bezels/<set>.png, and <set>.info beside it giving the
 * screen's place; a clone without its own takes its parent's, a game
 * without any artwork/bezels/default.png. The drawing is
 * hl::Presentation's (hl/presentation.h). */

/* The screen of a bezel's picture, the hole the game shows through: where
 * it is, in shares of the picture's width and height from its top left
 * corner, and the picture's size in pixels; none while w is 0 */
struct BezelHole {
	double	 x = 0.0, y = 0.0, w = 0.0, h = 0.0;
	int	 bezel_w = 0, bezel_h = 0;
};

/* The hole a Batocera .info gives (JSON: width, height, and top, left,
 * bottom, right, what is around the hole on each side, in pixels of a
 * picture of that size), for a picture of bezel_w x bezel_h; false when it
 * gives none */
bool	 bezel_parse_info (const char *json, int bezel_w, int bezel_h, BezelHole *hole);

/* The hole found in the picture itself (RGBA, top row first): the pixels
 * joined to its middle that are not opaque (a screen's glass often is half
 * so), their bounds; false when its middle is opaque */
bool	 bezel_find_hole (const uint8_t *rgba, int w, int h, BezelHole *hole);

/* screen_layout () in a target of tw x th for a picture of pw x ph, then,
 * when the target is wider than its frame and the hole is there, the
 * bezel's picture fitted to the target (as large as it holds it, in the
 * middle) in l->bezel, and the picture laid out in its hole as
 * screen_layout () lays it out in a window of the hole's size. True when
 * the bezel shows */
bool	 bezel_layout (ScreenLayout *l, int tw, int th, int pw, int ph, const BezelHole &hole);

/* A bezel's picture, read once: RGBA, top row first, and its hole. Shared
 * with the drawing thread, never changed once made */
struct BezelImage {
	std::string		 path;
	int			 w = 0, h = 0;
	std::vector<uint8_t>	 rgba;
	BezelHole		 hole;
};

/* The bezel of the game being played. From the thread that runs the
 * machine and the window (the main one) */
class Bezel {
public:
	/* The game started, its set and its parent's (empty for a parent);
	 * NULL: none (the picker) */
	static void		 set_game (const char *set, const char *parent);
	/* Window::layout ()'s: screen_layout (), in the bezel's screen when
	 * BEZEL = IMAGE and the game has a picture (bezel_layout ()) */
	static void		 layout (ScreenLayout *l, int tw, int th, int pw, int ph);
	/* The game's bezel picture, read at the first call: NULL when it has
	 * none, or it cannot be read */
	static std::shared_ptr<const BezelImage> image ();
	/* Its file: artwork/bezels/<set>.png, the parent's, default.png (the
	 * write directory's first); empty when none */
	static std::string	 find (const char *set, const char *parent);
};

} /* namespace hl */

#endif /* __HL_BEZEL_H__ */
