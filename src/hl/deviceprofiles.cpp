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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "hl/deviceprofiles.h"
#include "hl/core.h"
#include "hl/paths.h"

namespace hl {

DeviceProfiles &
DeviceProfiles::get ()
{
	static DeviceProfiles profiles;
	return profiles;
}

static std::string
trim (const std::string &s)
{
	size_t a = s.find_first_not_of (" \t\r"), b = s.find_last_not_of (" \t\r");
	return a == std::string::npos ? "" : s.substr (a, b - a + 1);
}

std::string
DeviceProfile::value (const char *section, const char *key) const
{
	for (const auto &s : sections)
		if (!strcasecmp (s.first.c_str (), section))
			for (const auto &row : s.second)
				if (!strcasecmp (row.first.c_str (), key))
					return row.second;
	return "";
}

/* One file: false when it names no device */
static bool
read_profile (const std::filesystem::path &path, DeviceProfile &p)
{
	std::ifstream in (path, std::ios::binary);
	std::string line, section;

	if (!in)
		return false;
	p.file = path.string ();
	while (std::getline (in, line)) {
		line = trim (line);
		if (line.empty () || line[0] == '#' || line[0] == ';')
			continue;
		if (line[0] == '[') {
			size_t close = line.find (']');
			section = line.substr (1, close == std::string::npos ? std::string::npos : close - 1);
			if (strcasecmp (section.c_str (), "Device"))
				p.sections.push_back ({ section, {} });
			continue;
		}
		size_t eq = line.find ('=');
		if (eq == std::string::npos || section.empty ())
			continue;
		std::string key = trim (line.substr (0, eq)), value = trim (line.substr (eq + 1));
		if (!strcasecmp (section.c_str (), "Device")) {
			if (!strcasecmp (key.c_str (), "Name"))
				p.name = value;
			else if (!strcasecmp (key.c_str (), "VidPid")) {
				unsigned vid = 0, pid = 0;

				if (sscanf (value.c_str (), "%x:%x", &vid, &pid) == 2) {
					p.vendor = (uint16_t) vid;
					p.product = (uint16_t) pid;
				}
			}
			continue;
		}
		p.sections.back ().second.push_back ({ key, value });
	}
	return !p.name.empty () || p.vendor || p.product;
}

void
DeviceProfiles::read_dir (const std::string &dir, bool from_user)
{
	std::error_code ec;
	std::vector<std::filesystem::path> files;

	if (dir.empty () || !std::filesystem::is_directory (dir, ec))
		return;
	for (const auto &e : std::filesystem::directory_iterator (dir, ec))
		if (e.is_regular_file (ec) && !strcasecmp (e.path ().extension ().string ().c_str (), ".ini"))
			files.push_back (e.path ());
	std::sort (files.begin (), files.end ());
	for (const auto &f : files) {
		DeviceProfile p;

		p.user = from_user;
		if (read_profile (f, p))
			all.push_back (p);
		else
			HL_PRINT ("controls: '%s' names no device ([Device] Name or VidPid): not a profile", f.string ().c_str ());
	}
}

void
DeviceProfiles::load (const std::string &user_dir)
{
	all.clear ();
	user = user_dir;
	read_dir (Paths::base () + "data/devices", false);
	read_dir (user, true);
	HL_PRINT ("controls: %u device profiles (data/devices, '%s')", (unsigned) all.size (), user.c_str ());
}

const DeviceProfile *
DeviceProfiles::match (const std::string &name, uint16_t vendor, uint16_t product) const
{
	const DeviceProfile *found = NULL;
	int best = 0;

	/* 4 its vendor and product, 2 its name; 1 more the user's */
	for (const DeviceProfile &p : all) {
		int score = 0;

		if ((p.vendor || p.product) && p.vendor == vendor && p.product == product)
			score = 4;
		else if (!p.name.empty () && !strcasecmp (trim (p.name).c_str (), trim (name).c_str ()))
			score = 2;
		if (!score)
			continue;
		score += p.user ? 1 : 0;
		if (score > best) {
			best = score;
			found = &p;
		}
	}
	return found;
}

/* Each input of each alternative changed by change (a token, trimmed),
 * joined again as controls.ini has them */
template <typename F>
static std::string
each_input (const std::string &value, F change)
{
	std::string out;
	size_t start = 0;

	while (start <= value.size ()) {
		size_t comma = value.find (',', start);
		std::string alt = value.substr (start, comma == std::string::npos ? std::string::npos : comma - start);
		std::string parts;
		size_t at = 0;

		while (at <= alt.size ()) {
			size_t plus = alt.find ('+', at);
			std::string part = trim (alt.substr (at, plus == std::string::npos ? std::string::npos : plus - at));

			if (!part.empty ())
				parts += (parts.empty () ? "" : " + ") + change (part);
			if (plus == std::string::npos)
				break;
			at = plus + 1;
		}
		if (!parts.empty ())
			out += (out.empty () ? "" : ", ") + parts;
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}

std::string
DeviceProfiles::to_player (const std::string &value, int player)
{
	const std::string n = std::to_string (player);

	return each_input (value, [&] (const std::string &t) {
		if (!strncasecmp (t.c_str (), "JOY_", 4))
			return "JOY" + n + "_" + t.substr (4);
		if (!strncasecmp (t.c_str (), "GC_", 3))
			return "GC" + n + "_" + t.substr (3);
		return t;
	});
}

std::string
DeviceProfiles::from_player (const std::string &value, int player)
{
	const std::string joy = "JOY" + std::to_string (player) + "_", gc = "GC" + std::to_string (player) + "_";

	return each_input (value, [&] (const std::string &t) {
		if (!strncasecmp (t.c_str (), joy.c_str (), joy.size ()))
			return "JOY_" + t.substr (joy.size ());
		if (!strncasecmp (t.c_str (), gc.c_str (), gc.size ()))
			return "GC_" + t.substr (gc.size ());
		return t;
	});
}

std::string
DeviceProfiles::save (const DeviceProfile &p)
{
	std::error_code ec;
	std::string base;

	if (user.empty ())
		return "";
	/* Its file named after it: letters, digits and '-' kept */
	for (char ch : p.name)
		base += isalnum ((unsigned char) ch) || ch == '-' ? ch : '_';
	while (!base.empty () && base.back () == '_')
		base.pop_back ();
	if (base.empty ())
		base = "device";
	std::filesystem::create_directories (user, ec);
	std::filesystem::path path = std::filesystem::path (user) / (base + ".ini");
	FILE *fp = fopen (path.string ().c_str (), "w");

	if (!fp) {
		HL_ERROR ("controls: cannot write '%s'", path.string ().c_str ());
		return "";
	}
	fprintf (fp, "# Hikaruloader's profile of a device: its bindings, for whoever connects\n"
	             "# one (devices/ beside controls.ini, or the program's data/devices). Its\n"
	             "# inputs are named without a player's number (JOY_ a joystick's, GC_ a\n"
	             "# game controller's): they go to the player the device is, the first time\n"
	             "# it is connected, and to the Controls tab's Default and Reset.\n");
	fprintf (fp, "\n[Device]\n# Its name, as the Controls tab and hikaruloader.log give it\nName = %s\n", p.name.c_str ());
	if (p.vendor || p.product)
		fprintf (fp, "# Its USB vendor and product, found first\nVidPid = %04X:%04X\n", p.vendor, p.product);
	for (const auto &s : p.sections) {
		if (s.second.empty ())
			continue;
		fprintf (fp, "\n[%s]\n", s.first.c_str ());
		for (const auto &row : s.second)
			fprintf (fp, "%s = %s\n", row.first.c_str (), row.second.c_str ());
	}
	fclose (fp);
	HL_PRINT ("controls: the profile of %s in '%s'", p.name.c_str (), path.string ().c_str ());
	load (user);
	return path.string ();
}

} /* namespace hl */
