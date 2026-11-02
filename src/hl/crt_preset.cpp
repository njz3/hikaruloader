/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/video/picture_preset.cpp,
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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "hl/crt_preset.h"
#include "hl/paths.h"

namespace hl {
namespace crt_preset {

/* Without the spaces, tabs and line ends around it (the presets shipped
 * have Windows' line ends) */
static std::string
trimmed (const std::string &s)
{
	const char *space = " \t\r\n";
	size_t first = s.find_first_not_of (space);

	if (first == std::string::npos)
		return std::string ();
	return s.substr (first, s.find_last_not_of (space) - first + 1);
}

static std::string
key_or (const std::map<std::string, std::string> &keys, const std::string &name, const std::string &otherwise)
{
	auto key = keys.find (name);

	return key == keys.end () ? otherwise : key->second;
}

std::map<std::string, std::string>
read_keys (const std::string &text)
{
	std::map<std::string, std::string> keys;
	size_t at = 0;

	while (at < text.size ()) {
		size_t end = text.find ('\n', at);

		end = end == std::string::npos ? text.size () : end;
		std::string line = trimmed (text.substr (at, end - at));

		at = end + 1;
		if (line.empty () || line[0] == '#' || line[0] == ';')
			continue;
		if (line[0] == '[')
			break;

		size_t name_end = line.find_first_of ("= ");
		std::string name = line.substr (0, name_end);
		std::string value = name_end == std::string::npos ? std::string () : line.substr (name_end);

		value = value.substr (std::min (value.find_first_not_of ("= "), value.size ()));
		value = value.substr (0, value.find ('#'));
		if (!value.empty () && value.front () == '"')
			value.erase (0, 1);
		value = value.substr (0, value.find ('"'));
		value = trimmed (value);
		if (!value.empty ())
			keys.emplace (name, value);
	}
	return keys;
}

bool
key_on (const std::map<std::string, std::string> &keys, const std::string &name)
{
	auto key = keys.find (name);

	if (key == keys.end ())
		return false;
	if (key->second == "true" || key->second == "false")
		return key->second == "true";
	return atoi (key->second.c_str ()) != 0;
}

std::string
part (const std::string &source, const char *which)
{
	static const std::string VERSION = "#version";
	std::string text = source, version;

	/* Blanked, not removed: the driver's line numbers stay the file's */
	for (const char *start : { "#pragma parameter", "#define texture" })
		for (size_t at = text.find (start); at != std::string::npos; at = text.find (start)) {
			size_t end = std::min (text.find ('\n', at), text.size ());

			text.replace (at, end - at, end - at, ' ');
		}
	if (size_t at = text.find (VERSION); at != std::string::npos) {
		size_t end = text.find_first_not_of (" \t", at + VERSION.size ());

		end = text.find_first_of (" \t\r\n", end);
		version = text.substr (at, end - at);
		text.replace (at, end - at, end - at, ' ');
	}
	return version + "\n\n#extension GL_ARB_shading_language_420pack : enable\n#define " + which +
	       "\n#define PARAMETER_UNIFORM\n\n#line 1\n" + text;
}

void
declared (const std::string &source, std::vector<std::pair<std::string, float>> &parameters)
{
	static const char PRAGMA[] = "#pragma parameter";

	for (size_t at = source.find (PRAGMA); at != std::string::npos; at = source.find (PRAGMA, at + 1)) {
		std::string line = source.substr (at, source.find ('\n', at) - at);
		size_t name_at = line.find_first_not_of (" \t", sizeof (PRAGMA) - 1);
		size_t name_end = line.find_first_of (" \t", name_at);
		size_t open = line.find ('"', name_end), close = line.find ('"', open + 1);

		if (name_at == std::string::npos || open == std::string::npos || close == std::string::npos)
			continue;

		std::string name = line.substr (name_at, name_end - name_at);
		size_t described = close - open - 1;
		bool known = false;

		for (const auto &[have, value] : parameters)
			known = known || have == name;
		if (!known && name.size () <= LONGEST_NAME && described > 0 && described <= LONGEST_NAME)
			parameters.emplace_back (name, strtof (line.c_str () + close + 1, NULL));
	}
}

/* What a pass draws: read as the next pass says, with its own wrap; at what
 * scale */
static void
read_drawing (const std::map<std::string, std::string> &keys, int i, Plan::Pass &pass)
{
	std::string n = std::to_string (i);
	std::string wrap = key_or (keys, "wrap_mode" + n, "");

	pass.linear = key_on (keys, "filter_linear" + std::to_string (i + 1));
	pass.is_float = key_on (keys, "float_framebuffer" + n);
	pass.wrap = wrap == "repeat" ? Plan::Wrap::REPEAT : wrap == "mirrored_repeat" ? Plan::Wrap::MIRRORED :
	            wrap == "clamp_to_edge" ? Plan::Wrap::EDGE : Plan::Wrap::BORDER;
	for (size_t axis = 0; axis < 2; axis++) {
		std::string ax = axis ? "y" : "x";
		std::string type = key_or (keys, "scale_type" + n, key_or (keys, "scale_type_" + ax + n, "source"));

		pass.scale_type[axis] = type == "viewport" ? Plan::Scale::VIEWPORT :
		                        type == "absolute" ? Plan::Scale::ABSOLUTE : Plan::Scale::SOURCE;
		pass.scale[axis] = strtof (key_or (keys, "scale" + n, key_or (keys, "scale_" + ax + n, "1.0")).c_str (),
		                           NULL);
	}
}

/* The values the preset sets */
static void
read_values (const std::map<std::string, std::string> &keys, Plan &plan)
{
	for (std::string names = key_or (keys, "parameters", ""); !names.empty ();) {
		size_t end = names.find (';');
		std::string name = names.substr (0, end);

		for (auto &[have, value] : plan.parameters)
			if (have == name)
				value = strtof (key_or (keys, name, "0").c_str (), NULL);
		names = end == std::string::npos ? std::string () : names.substr (end + 1);
	}
}

bool
make_plan (const std::string &text, const std::string &folder,
           bool (* read_file) (const std::string &path, std::string &text),
           Plan *plan, std::string *error)
{
	std::map<std::string, std::string> keys = read_keys (text);
	int count = atoi (key_or (keys, "shaders", "0").c_str ());

	*plan = Plan ();
	if (count <= 0) {
		*error = "no passes";
		return false;
	}
	plan->passes.resize ((size_t) count);
	for (int i = 0; i < count; i++) {
		Plan::Pass &pass = plan->passes[(size_t) i];

		pass.file = (std::filesystem::path (folder) / key_or (keys, "shader" + std::to_string (i), "")).string ();
		if (!read_file (pass.file, pass.source)) {
			*error = "cannot read '" + pass.file + "'";
			return false;
		}
		declared (pass.source, plan->parameters);
		read_drawing (keys, i, pass);
	}

	const Plan::Pass &last = plan->passes.back ();
	if (last.scale_type[0] != Plan::Scale::SOURCE || last.scale_type[1] != Plan::Scale::SOURCE ||
	    last.scale[0] != 1.0f || last.scale[1] != 1.0f) {
		*error = "its last pass has a scale";
		return false;
	}
	read_values (keys, *plan);
	return true;
}

static bool
read_text (const std::string &path, std::string &text)
{
	FILE *fp = fopen (path.c_str (), "rb");
	char buffer[16384];
	size_t n;

	if (!fp)
		return false;
	text.clear ();
	while ((n = fread (buffer, 1, sizeof (buffer), fp)) > 0)
		text.append (buffer, n);
	fclose (fp);
	return true;
}

bool
read_plan (const std::string &preset, Plan *plan, std::string *error)
{
	std::string text;

	if (!read_text (preset, text)) {
		*error = "cannot read it";
		return false;
	}
	return make_plan (text, std::filesystem::path (preset).parent_path ().string (), read_text, plan, error);
}

float
power_of_two (float size)
{
	unsigned whole = (unsigned) size, power = 1;

	while (power < whole)
		power <<= 1;
	return (float) power;
}

std::array<float, 2>
pass_size (const std::array<Plan::Scale, 2> &type, const std::array<float, 2> &scale,
           const std::array<float, 2> &viewport, const std::array<float, 2> &in)
{
	std::array<float, 2> out;

	for (size_t axis = 0; axis < 2; axis++)
		out[axis] = type[axis] == Plan::Scale::VIEWPORT ? viewport[axis] * scale[axis] :
		            type[axis] == Plan::Scale::ABSOLUTE ? scale[axis] : in[axis] * scale[axis];
	return out;
}

std::array<float, 16>
corners (const std::array<float, 2> &size, const std::array<float, 2> &texture)
{
	float u = size[0] / texture[0], v = size[1] / texture[1];

	return { -1, -1, 0, 0,  -1, 1, 0, v,  1, -1, u, 0,  1, 1, u, v };
}

/* The presets' folders: the write directory's data/crt (the user's own),
 * then the program's */
static std::vector<std::filesystem::path>
folders ()
{
	std::vector<std::filesystem::path> dirs;

	if (Paths::has_write_dir ())
		dirs.push_back (std::filesystem::path (Paths::write_dir ()) / "data" / "crt");
	dirs.push_back (std::filesystem::path (Paths::base ()) / "data" / "crt");
	return dirs;
}

std::vector<std::string>
list ()
{
	std::set<std::string> names;

	for (const auto &dir : folders ()) {
		std::error_code error;

		for (auto it = std::filesystem::recursive_directory_iterator (dir, error);
		     !error && it != std::filesystem::recursive_directory_iterator (); it.increment (error)) {
			if (it->path ().extension () != ".glslp")
				continue;
			std::filesystem::path name = std::filesystem::relative (it->path (), dir, error);
			if (!error)
				names.insert (name.replace_extension ().generic_string ());
		}
	}
	return std::vector<std::string> (names.begin (), names.end ());
}

std::string
find (const std::string &name)
{
	std::error_code error;

	if (name.empty ())
		return std::string ();
	if (std::filesystem::path (name).extension () == ".glslp")
		return std::filesystem::is_regular_file (name, error) ? name : std::string ();
	for (const auto &dir : folders ()) {
		std::filesystem::path path = (dir / (name + ".glslp")).make_preferred ();
		if (std::filesystem::is_regular_file (path, error))
			return path.string ();
	}
	return std::string ();
}

} /* namespace crt_preset */
} /* namespace hl */
