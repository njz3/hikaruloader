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

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include "hl/bezel.h"
#include "hl/config.h"
#include "hl/core.h"
#include "hl/paths.h"

/* The PNG reader, a copy of its own: the picker's (ui_artwork.cpp) is in the
 * program, not in this library the tests link */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace hl {

bool
bezel_parse_info (const char *json, int bezel_w, int bezel_h, BezelHole *hole)
{
	json_error_t error;
	json_t *root = json_loads (json, 0, &error);
	auto number = [root] (const char *key) { return json_number_value (json_object_get (root, key)); };
	bool ok = false;

	*hole = BezelHole ();
	if (!root)
		return false;

	double w = number ("width"), h = number ("height");
	double left = number ("left"), top = number ("top"), right = number ("right"), bottom = number ("bottom");

	if (w > 0.0 && h > 0.0 && left >= 0.0 && top >= 0.0 && right >= 0.0 && bottom >= 0.0 &&
	    left + right < w && top + bottom < h && bezel_w > 0 && bezel_h > 0) {
		hole->x = left / w;
		hole->y = top / h;
		hole->w = (w - left - right) / w;
		hole->h = (h - top - bottom) / h;
		hole->bezel_w = bezel_w;
		hole->bezel_h = bezel_h;
		ok = true;
	}
	json_decref (root);
	return ok;
}

/* Below this alpha a pixel lets the game through */
static const uint8_t SEE_THROUGH = 250;

bool
bezel_find_hole (const uint8_t *rgba, int w, int h, BezelHole *hole)
{
	auto open = [&] (int x, int y) { return rgba[((size_t) y * w + x) * 4 + 3] < SEE_THROUGH; };
	std::vector<uint8_t> seen ((size_t) MAX2 (w, 0) * MAX2 (h, 0));
	std::vector<std::pair<int, int>> next = { { w / 2, h / 2 } };
	int left = w, top = h, right = -1, bottom = -1;

	*hole = BezelHole ();
	if (w < 2 || h < 2 || !open (w / 2, h / 2))
		return false;
	seen[(size_t) (h / 2) * w + w / 2] = 1;
	while (!next.empty ()) {
		auto [x, y] = next.back ();

		next.pop_back ();
		left = MIN2 (left, x);
		right = MAX2 (right, x);
		top = MIN2 (top, y);
		bottom = MAX2 (bottom, y);
		for (const auto &[dx, dy] : { std::pair { 1, 0 }, std::pair { -1, 0 }, std::pair { 0, 1 }, std::pair { 0, -1 } }) {
			int nx = x + dx, ny = y + dy;

			if (nx < 0 || ny < 0 || nx >= w || ny >= h)
				continue;
			size_t at = (size_t) ny * w + nx;
			if (seen[at] || !open (nx, ny))
				continue;
			seen[at] = 1;
			next.push_back ({ nx, ny });
		}
	}
	hole->x = (double) left / w;
	hole->y = (double) top / h;
	hole->w = (double) (right - left + 1) / w;
	hole->h = (double) (bottom - top + 1) / h;
	hole->bezel_w = w;
	hole->bezel_h = h;
	return true;
}

bool
bezel_layout (ScreenLayout *l, int tw, int th, int pw, int ph, const BezelHole &hole)
{
	screen_layout (l, tw, th, pw, ph);
	/* Only beside a frame narrower than the window: a stretched display
	 * mode, or a frame as wide, leaves no room */
	if (hole.w <= 0.0 || hole.h <= 0.0 || hole.bezel_w <= 0 || hole.bezel_h <= 0 || l->frame[2] >= tw)
		return false;

	/* The bezel as large as the target holds it, in its middle; from the
	 * top left, as the .info counts */
	double k = MIN2 ((double) tw / hole.bezel_w, (double) th / hole.bezel_h);
	double bw = hole.bezel_w * k, bh = hole.bezel_h * k;
	double bx = (tw - bw) / 2.0, by = (th - bh) / 2.0;
	int x0 = (int) lround (bx + hole.x * bw), x1 = (int) lround (bx + (hole.x + hole.w) * bw);
	int y0 = (int) lround (by + hole.y * bh), y1 = (int) lround (by + (hole.y + hole.h) * bh);
	int bx0 = (int) lround (bx), bx1 = (int) lround (bx + bw);
	int by0 = (int) lround (by), by1 = (int) lround (by + bh);
	ScreenLayout in;

	if (x1 - x0 < 1 || y1 - y0 < 1)
		return false;

	/* The picture in the hole, as in a window of its size; then to the
	 * target's pixels, from its bottom left */
	screen_layout (&in, x1 - x0, y1 - y0, pw, ph);
	in.frame[0] += x0;
	in.frame[1] += th - y1;
	in.image[0] += x0;
	in.image[1] += th - y1;
	in.bezel[0] = bx0;
	in.bezel[1] = th - by1;
	in.bezel[2] = bx1 - bx0;
	in.bezel[3] = by1 - by0;
	*l = in;
	return true;
}

/****************************************************************************
 The game's bezel
****************************************************************************/

static struct {
	std::string set, parent;
	/* Read once a game: the picture, or none */
	bool looked;
	std::shared_ptr<const BezelImage> image;
} game;

void
Bezel::set_game (const char *set, const char *parent)
{
	game.set = set ? set : "";
	game.parent = parent ? parent : "";
	game.looked = false;
	game.image.reset ();
}

std::string
Bezel::find (const char *set, const char *parent)
{
	std::error_code error;

	for (const char *name : { set, parent, "default" }) {
		if (!name || !name[0])
			continue;

		std::string path = Paths::config_read (std::string ("artwork/bezels/") + name + ".png");
		if (std::filesystem::is_regular_file (path, error))
			return path;
	}
	return std::string ();
}

/* Its picture and its hole: as its .info says, else where it is see-through */
static std::shared_ptr<const BezelImage>
read_image (const std::string &path)
{
	auto image = std::make_shared<BezelImage> ();
	int channels;
	stbi_uc *pixels = stbi_load (path.c_str (), &image->w, &image->h, &channels, 4);
	std::string info = std::filesystem::path (path).replace_extension (".info").string ();
	FILE *fp;

	if (!pixels) {
		HL_ERROR ("BEZEL: cannot read '%s': %s", path.c_str (), stbi_failure_reason ());
		return NULL;
	}
	image->path = path;
	image->rgba.assign (pixels, pixels + (size_t) image->w * image->h * 4);
	stbi_image_free (pixels);

	if ((fp = fopen (info.c_str (), "rb")) != NULL) {
		std::string text;
		char buffer[4096];
		size_t n;

		while ((n = fread (buffer, 1, sizeof (buffer), fp)) > 0)
			text.append (buffer, n);
		fclose (fp);
		if (!bezel_parse_info (text.c_str (), image->w, image->h, &image->hole))
			HL_ERROR ("BEZEL: '%s' gives no screen: found in the picture", info.c_str ());
	}
	if (image->hole.w <= 0.0 &&
	    !bezel_find_hole (image->rgba.data (), image->w, image->h, &image->hole)) {
		HL_ERROR ("BEZEL: '%s' has no screen (no .info, its middle opaque): not shown", path.c_str ());
		return NULL;
	}
	HL_PRINT ("BEZEL: '%s', %dx%d, its screen at %.0f,%.0f %.0fx%.0f", path.c_str (), image->w, image->h,
	          image->hole.x * image->w, image->hole.y * image->h,
	          image->hole.w * image->w, image->hole.h * image->h);
	return image;
}

std::shared_ptr<const BezelImage>
Bezel::image ()
{
	if (!game.looked && !game.set.empty ()) {
		std::string path = find (game.set.c_str (), game.parent.c_str ());

		game.looked = true;
		if (!path.empty ())
			game.image = read_image (path);
		else
			HL_PRINT ("BEZEL: no picture for %s in artwork/bezels", game.set.c_str ());
	}
	return game.image;
}

void
Bezel::layout (ScreenLayout *l, int tw, int th, int pw, int ph)
{
	std::shared_ptr<const BezelImage> picture;

	if (Config::get ()->bezel == BEZEL_IMAGE && (picture = image ()))
		bezel_layout (l, tw, th, pw, ph, picture->hole);
	else
		screen_layout (l, tw, th, pw, ph);
}

} /* namespace hl */
