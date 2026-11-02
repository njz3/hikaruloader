/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/video/picture_preset.hpp,
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

#ifndef __HL_CRT_PRESET_H__
#define __HL_CRT_PRESET_H__

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

/* CRT_PRESET ([Enhancements]): a libretro GLSL preset (.glslp) and the GLSL
 * files of its passes, read as 86Box reads them, whose OpenGL rules the
 * Lola Retro presets shipped in data/crt/lola-retro are written for. The
 * passes run in hl/crt_filter.h. Pure functions: tests/crt_preset_test.cpp. */
namespace hl {
namespace crt_preset {

/* 86Box takes a parameter whose name and description are 1 to 63
 * characters long */
static const size_t LONGEST_NAME = 63;

/* The keys of a preset as 86Box's reader takes them (utils/ini.c): a line
 * that starts with # or ; is a comment and a [section] ends them; a name
 * ends at = or a space; a value is cut at its first #, loses its quotes and
 * its spaces; the first of two equal names counts. */
std::map<std::string, std::string> read_keys (const std::string &text);

/* A key that is true, false or a number, as 86Box reads it; false when
 * there is none */
bool	 key_on (const std::map<std::string, std::string> &keys, const std::string &name);

/* A part of a pass (VERTEX or FRAGMENT) as 86Box gives it to the driver:
 * its #version line first, then the extension, the part, PARAMETER_UNIFORM
 * and the rest of the file, its #pragma parameter lines blanked */
std::string part (const std::string &source, const char *which);

/* The parameters a pass declares (#pragma parameter NAME "description"
 * default minimum maximum step), added to `parameters` unless there
 * already */
void	 declared (const std::string &source, std::vector<std::pair<std::string, float>> &parameters);

/* A preset read: each pass, its file (from the preset's folder) and its
 * text, how what it draws is read (the next pass's filter_linear, its own
 * wrap_mode) and kept (float_framebuffer), its scale each way; the
 * parameters the passes declare, at the values the preset gives them */
struct Plan {
	enum class Scale { SOURCE, VIEWPORT, ABSOLUTE };
	enum class Wrap { BORDER, EDGE, REPEAT, MIRRORED };
	struct Pass {
		std::string file, source;
		bool linear = false, is_float = false;
		Wrap wrap = Wrap::BORDER;
		std::array<Scale, 2> scale_type { Scale::SOURCE, Scale::SOURCE };
		std::array<float, 2> scale { 1.0f, 1.0f };
	};
	std::vector<Pass> passes;
	std::vector<std::pair<std::string, float>> parameters;
};

/* The plan of a preset's text, its pass files read by read_file (a path
 * from the preset's folder; false when it cannot be read). False, why in
 * *error, when a pass cannot be read, there is none, or the last one has a
 * scale (it draws into the window) */
bool	 make_plan (const std::string &text, const std::string &folder,
	            bool (* read_file) (const std::string &path, std::string &text),
	            Plan *plan, std::string *error);
/* The same from a preset file, its passes read from the disk */
bool	 read_plan (const std::string &preset, Plan *plan, std::string *error);

/* The power of two a size takes, as 86Box rounds the whole number of
 * pixels up */
float	 power_of_two (float size);

/* The texture of a picture of that size */
inline std::array<float, 2>
texture_of (const std::array<float, 2> &size)
{
	return { power_of_two (size[0]), power_of_two (size[1]) };
}

/* The size a pass of that scale draws in a viewport of that size, from the
 * size of its input */
std::array<float, 2> pass_size (const std::array<Plan::Scale, 2> &type, const std::array<float, 2> &scale,
                                const std::array<float, 2> &viewport, const std::array<float, 2> &in);

/* The four corners of a pass, VertexCoord then TexCoord: TexCoord spans the
 * picture of its input in its texture */
std::array<float, 16> corners (const std::array<float, 2> &size, const std::array<float, 2> &texture);

/* The presets in data/crt (the write directory's, then the program's), by
 * their names: their paths from there, without .glslp
 * ("lola-retro/sega-monitor-29"), sorted */
std::vector<std::string> list ();
/* A preset's file from its name (or its own path, a .glslp anywhere);
 * empty when there is none */
std::string find (const std::string &name);

} /* namespace crt_preset */
} /* namespace hl */

#endif /* __HL_CRT_PRESET_H__ */
