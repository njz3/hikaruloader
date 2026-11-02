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

/* CRT_PRESET's reader (src/hl/crt_preset.cpp): the keys of a .glslp as 86Box
 * reads them, a pass's parameters and parts, the plan of a preset and its
 * passes' sizes; then, given the folder data/crt, each Lola Retro preset
 * shipped. Run by ctest, or alone: crtpresettest [data/crt]. */

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>

#include "hl/crt_preset.h"

namespace cp = hl::crt_preset;

static int failures, checks;

static void
check (bool ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf (stderr, "FAIL %s\n", what);
	}
}

/* Within a ten-thousandth of b (8192 has no closer floats) */
static bool
near (float a, float b)
{
	float d = a > b ? a - b : b - a;

	return d <= 1e-4f * (b > 1.0f ? b : 1.0f);
}

/* The pass files of the made-up presets, by their paths */
static std::map<std::string, std::string> files;

static bool
read_file (const std::string &path, std::string &text)
{
	auto file = files.find (std::filesystem::path (path).generic_string ());

	if (file == files.end ())
		return false;
	text = file->second;
	return true;
}

static float
parameter (const cp::Plan &plan, const char *name)
{
	for (const auto &[have, value] : plan.parameters)
		if (have == name)
			return value;
	return -1.0f;
}

static void
test_keys (void)
{
	auto keys = cp::read_keys ("# a comment\r\n"
	                           "; another\n"
	                           "shaders = 2\r\n"
	                           "shader0 = \"shaders/a.glsl\"   # cut here\n"
	                           "filter_linear1=true\n"
	                           "shaders = 9\n"
	                           "empty =\n"
	                           "[section]\n"
	                           "after = 1\n");

	check (keys["shaders"] == "2", "the first of two equal names counts");
	check (keys["shader0"] == "shaders/a.glsl", "a value loses its quotes and its comment");
	check (keys["filter_linear1"] == "true", "a value without spaces");
	check (!keys.count ("empty"), "an empty value is left out");
	check (!keys.count ("after"), "a section ends the keys");
	check (cp::key_on (keys, "filter_linear1"), "true is on");
	check (cp::key_on (keys, "shaders"), "a number other than 0 is on");
	check (!cp::key_on (keys, "missing"), "a missing key is off");
}

static void
test_pass_text (void)
{
	const std::string source =
		"// a pass\n"
		"#version 150\n"
		"#pragma parameter LR_A \"Strength\" 0.5 0 1 0.05\n"
		"#pragma parameter LR_B \"Again\" 2 0 4 1\n"
		"#pragma parameter LR_A \"Twice\" 9 0 9 1\n"
		"#pragma parameter LR_LONG \"" + std::string (64, 'x') + "\" 1 0 1 1\n"
		"uniform float LR_A;\n";
	std::vector<std::pair<std::string, float>> parameters;
	std::string vertex = cp::part (source, "VERTEX");

	cp::declared (source, parameters);
	check (parameters.size () == 2, "two parameters: one declared twice, one described too long");
	check (parameters.size () == 2 && parameters[0].first == "LR_A" && near (parameters[0].second, 0.5f),
	       "the first declaration's default");
	check (parameters.size () == 2 && parameters[1].first == "LR_B" && near (parameters[1].second, 2.0f),
	       "the second parameter");
	check (vertex.rfind ("#version 150\n", 0) == 0, "the part starts with the #version line");
	check (vertex.find ("#define VERTEX\n") != std::string::npos, "the part is defined");
	check (vertex.find ("#pragma parameter") == std::string::npos, "the #pragma parameter lines blanked");
	check (vertex.find ("uniform float LR_A;") != std::string::npos, "the rest kept");
	check (std::count (vertex.begin (), vertex.end (), '\n') ==
	       std::count (source.begin (), source.end (), '\n') + 7, "the file's lines kept, behind #line 1");
}

static void
test_plan (void)
{
	cp::Plan plan;
	std::string error;

	files["shaders/a.glsl"] = "#pragma parameter LR_A \"A\" 1 0 2 0.1\n#pragma parameter LR_B \"B\" 3 0 9 1\n";
	files["shaders/b.glsl"] = "#pragma parameter LR_C \"C\" 5 0 9 1\n";
	check (cp::make_plan ("shaders = 2\n"
	                      "shader0 = shaders/a.glsl\n"
	                      "wrap_mode0 = clamp_to_edge\n"
	                      "float_framebuffer0 = true\n"
	                      "scale_type_x0 = absolute\n"
	                      "scale_x0 = 8192\n"
	                      "scale_type_y0 = source\n"
	                      "scale_y0 = 2.05\n"
	                      "shader1 = shaders/b.glsl\n"
	                      "filter_linear1 = true\n"
	                      "parameters = \"LR_A;LR_C;LR_UNKNOWN\"\n"
	                      "LR_A = 1.5\n"
	                      "LR_C = 7\n", "", read_file, &plan, &error), "a plan of two passes");
	check (plan.passes.size () == 2, "its two passes");
	if (plan.passes.size () == 2) {
		const cp::Plan::Pass &a = plan.passes[0], &b = plan.passes[1];

		check (a.linear, "what pass 0 draws read as filter_linear1 says");
		check (!b.linear, "the last pass's own: no next pass");
		check (a.is_float && !b.is_float, "float_framebuffer");
		check (a.wrap == cp::Plan::Wrap::EDGE && b.wrap == cp::Plan::Wrap::BORDER, "wrap_mode, border by default");
		check (a.scale_type[0] == cp::Plan::Scale::ABSOLUTE && a.scale_type[1] == cp::Plan::Scale::SOURCE,
		       "a scale type each way");
		check (near (a.scale[0], 8192.0f) && near (a.scale[1], 2.05f), "a scale each way");
		check (b.source == files["shaders/b.glsl"], "a pass's text");
	}
	check (near (parameter (plan, "LR_A"), 1.5f), "a value the preset sets");
	check (near (parameter (plan, "LR_B"), 3.0f), "a default the preset keeps");
	check (near (parameter (plan, "LR_C"), 7.0f), "a later pass's parameter set");

	check (!cp::make_plan ("shaders = 0\n", "", read_file, &plan, &error), "no passes: no plan");
	check (!cp::make_plan ("shaders = 1\nshader0 = shaders/missing.glsl\n", "", read_file, &plan, &error),
	       "a pass not found: no plan");
	check (!cp::make_plan ("shaders = 1\nshader0 = shaders/a.glsl\nscale_type0 = viewport\nscale0 = 2\n", "",
	                       read_file, &plan, &error), "the last pass with a scale: no plan");
}

static void
test_sizes (void)
{
	using S = cp::Plan::Scale;
	std::array<float, 2> out;
	std::array<float, 16> quad;

	check (cp::power_of_two (640.0f) == 1024.0f && cp::power_of_two (480.0f) == 512.0f &&
	       cp::power_of_two (512.0f) == 512.0f && cp::power_of_two (1.0f) == 1.0f, "powers of two");
	out = cp::pass_size ({ S::ABSOLUTE, S::SOURCE }, { 8192.0f, 2.05f }, { 1440.0f, 1080.0f }, { 640.0f, 480.0f });
	check (near (out[0], 8192.0f) && near (out[1], 984.0f), "an absolute width, a source height");
	out = cp::pass_size ({ S::VIEWPORT, S::VIEWPORT }, { 1.0f, 1.0f }, { 1440.0f, 1080.0f }, { 640.0f, 480.0f });
	check (near (out[0], 1440.0f) && near (out[1], 1080.0f), "the viewport's size");
	quad = cp::corners ({ 640.0f, 480.0f }, cp::texture_of ({ 640.0f, 480.0f }));
	check (near (quad[14], 0.625f) && near (quad[15], 0.9375f), "TexCoord spans the picture in its texture");
}

/* Each Lola Retro preset shipped: seven passes, their files read, the last
 * one into the viewport */
static void
test_shipped (const char *dir)
{
	unsigned count = 0;
	std::error_code error;

	for (const auto &file : std::filesystem::directory_iterator (std::filesystem::path (dir) / "lola-retro", error)) {
		cp::Plan plan;
		std::string why, name = file.path ().filename ().string ();

		if (file.path ().extension () != ".glslp")
			continue;
		count++;
		check (cp::read_plan (file.path ().string (), &plan, &why), ("reads " + name + ": " + why).c_str ());
		check (plan.passes.size () == 7, (name + " has seven passes").c_str ());
		check (plan.parameters.size () > 10, (name + " declares its parameters").c_str ());
		if (name == "sega-monitor-29.glslp")
			check (near (parameter (plan, "LR_MON_SIZE"), 28.0f), "sega-monitor-29's tube is 28 inches");
	}
	check (count == 12, "twelve presets shipped");
	check (std::filesystem::exists (std::filesystem::path (dir) / "lola-retro" / "LICENSE", error),
	       "with their licence");
}

int
main (int argc, char **argv)
{
	test_keys ();
	test_pass_text ();
	test_plan ();
	test_sizes ();
	if (argc > 1)
		test_shipped (argv[1]);

	printf ("crt_preset: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
