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
#include <cmath>
#include <fstream>

#include <SDL.h>

#include "hl/controls.h"
#include "hl/config.h"
#include "hl/input.h"

namespace hl {

/* A new controls.ini: our keys as they were in hikaruloader.ini's [Controls],
 * the controllers' as linuxloader and windowsloader have them (the pedals on
 * half an axis: a game controller's stick at rest is not half a throttle) */
static const char *default_file =
	"[Config]\n"
	"Steer_DeadZone = 500\n"
	"Gas_DeadZone = 500\n"
	"Brake_DeadZone = 500\n"
	"\n"
	"[Common]\n"
	"Test = KEY_F5\n"
	"P1_Service = KEY_F6\n"
	"P1_Coin = KEY_5\n"
	"P2_Coin = KEY_6\n"
	"P1_Start = KEY_Return, GC0_BUTTON_START\n"
	"ExitGame = KEY_F9\n"
	"\n"
	"[Digital]\n"
	"P1_Up = KEY_Up, GC0_BUTTON_DPUP, GC0_AXIS_LEFTY_NEGATIVE, JOY0_HAT0_UP\n"
	"P1_Down = KEY_Down, GC0_BUTTON_DPDOWN, GC0_AXIS_LEFTY_POSITIVE, JOY0_HAT0_DOWN\n"
	"P1_Left = KEY_Left, GC0_BUTTON_DPLEFT, GC0_AXIS_LEFTX_NEGATIVE, JOY0_HAT0_LEFT\n"
	"P1_Right = KEY_Right, GC0_BUTTON_DPRIGHT, GC0_AXIS_LEFTX_POSITIVE, JOY0_HAT0_RIGHT\n"
	"P1_Button1 = KEY_LeftCtrl, GC0_BUTTON_A\n"
	"P1_Button2 = KEY_LeftAlt, GC0_BUTTON_B\n"
	"P1_Button3 = KEY_LeftShift, GC0_BUTTON_X\n"
	"P1_Button4 = KEY_Z, GC0_BUTTON_Y\n"
	"\n"
	"[Driving]\n"
	"P1_Steer = GC0_AXIS_LEFTX, JOY0_AXIS_0\n"
	"P1_Gas = GC0_AXIS_RIGHTTRIGGER, JOY0_AXIS_1_NEGATIVE_SIDE\n"
	"P1_Brake = GC0_AXIS_LEFTTRIGGER, JOY0_AXIS_2_NEGATIVE_SIDE\n"
	"P1_Steer_Left = KEY_Left\n"
	"P1_Steer_Right = KEY_Right\n"
	"P1_Gas_Digital = KEY_Up\n"
	"P1_Brake_Digital = KEY_Down\n"
	"\n"
	"[Emulator]\n"
	"Pause = KEY_Space\n"
	"SaveState = KEY_F4\n"
	"LoadState = KEY_F1\n"
	"Menu = KEY_Tab, GC0_BUTTON_BACK\n"
	"ShowFps = KEY_F12\n"
	"\n"
	"[ControllerGUIDs]\n";

/* The file's comments, written again at each save: the syntax, then above
 * each section and each row what it is (and the row's default) */
static const char *file_notes =
	"# Hikaruloader's controls, in linuxloader's syntax (windowsloader's too).\n"
	"# Written by the Controls page (Tab, Controls): each change writes the whole\n"
	"# file again, these comments with it; comments of your own would be lost.\n"
	"# A line left out takes its default, given above it.\n"
	"#\n"
	"# ACTION = BINDING, BINDING...    any of them; A + B: both held together\n"
	"#   KEY_<name>                    a key, by SDL's name: KEY_F5, KEY_Return...\n"
	"#   JOY<n>_BUTTON_<b>             button b of controller n, from 0 as SDL\n"
	"#                                 counts (the page can show them from 1)\n"
	"#   JOY<n>_HAT<h>_<UP|DOWN|LEFT|RIGHT>\n"
	"#   JOY<n>_AXIS_<a>               an axis\n"
	"#   GC<n>_BUTTON_<name>           a game controller's button: A, B, X, Y,\n"
	"#                                 START, BACK, DPUP, LEFTSHOULDER...\n"
	"#   GC<n>_AXIS_<name>             its axis: LEFTX, LEFTY, RIGHTX, RIGHTY,\n"
	"#                                 LEFTTRIGGER, RIGHTTRIGGER\n"
	"# After an axis: _POSITIVE or _NEGATIVE, half the axis as a button;\n"
	"#   _POSITIVE_SIDE or _NEGATIVE_SIDE, half the axis as a pedal (at rest in\n"
	"#   the middle); _INVERTED, the axis the other way round.\n"
	"# <n> is the player's controller, from 0: JOY0 is [ControllerGUIDs] P1_GUID's\n"
	"#   (GC0 too when SDL knows it as a game controller); the others follow in\n"
	"#   SDL's order.\n";

static const struct {
	const char *section, *key, *note;
} notes[] = {
	{ "Config", NULL,
	  "Dead zones, 0 to 32767: the part of the wheel's travel around its middle,\n"
	  "or of a pedal's from its rest, read as nothing" },
	{ "Config", "Steer_DeadZone", "The steering wheel's dead zone" },
	{ "Config", "Gas_DeadZone", "The accelerator's dead zone" },
	{ "Config", "Brake_DeadZone", "The brake's dead zone" },
	{ "Common", NULL, "The cabinet's switches, in every game" },
	{ "Common", "Test", "The Test button" },
	{ "Common", "P1_Service", "The Service button" },
	{ "Common", "P1_Coin", "Coin 1" },
	{ "Common", "P2_Coin", "Coin 2" },
	{ "Common", "P1_Start", "Start" },
	{ "Common", "ExitGame", "Quits the emulator" },
	{ "Digital", NULL, "Player 1's stick and buttons" },
	{ "Digital", "P1_Up", "Up" },
	{ "Digital", "P1_Down", "Down" },
	{ "Digital", "P1_Left", "Left" },
	{ "Digital", "P1_Right", "Right" },
	{ "Digital", "P1_Button1", "Button 1" },
	{ "Digital", "P1_Button2", "Button 2" },
	{ "Digital", "P1_Button3", "Button 3" },
	{ "Digital", "P1_Button4", "Button 4" },
	{ "Driving", NULL,
	  "The wheel and the pedals: the I/O board's analog channels 0, 1 and 2" },
	{ "Driving", "P1_Steer", "The steering wheel: an axis, to the left negative" },
	{ "Driving", "P1_Gas", "The accelerator: a trigger, an axis or half of one" },
	{ "Driving", "P1_Brake", "The brake: a trigger, an axis or half of one" },
	{ "Driving", "P1_Steer_Left", "The wheel turned all the way to the left" },
	{ "Driving", "P1_Steer_Right", "The wheel turned all the way to the right" },
	{ "Driving", "P1_Gas_Digital", "The accelerator pressed all the way" },
	{ "Driving", "P1_Brake_Digital", "The brake pressed all the way" },
	{ "Emulator", NULL,
	  "The emulator's commands: Hikaruloader's own section, the other loaders\n"
	  "leave it alone" },
	{ "Emulator", "Pause", "Pauses and resumes the game" },
	{ "Emulator", "SaveState", "Saves the state" },
	{ "Emulator", "LoadState", "Loads the state" },
	{ "Emulator", "Menu", "Opens and closes the settings" },
	{ "Emulator", "ShowFps", "Shows and hides the frame rate" },
	{ "ControllerGUIDs", NULL,
	  "P<n>_GUID = an SDL joystick GUID: that controller is player n's, JOY<n-1>,\n"
	  "whatever order Windows lists them in (hikaruloader.log gives the GUIDs)" },
};

static const char *
note_of (const std::string &section, const char *key)
{
	for (const auto &n : notes)
		if (!strcasecmp (n.section, section.c_str ()) &&
		    (key ? n.key && !strcasecmp (n.key, key) : !n.key))
			return n.note;
	return NULL;
}

/* "# " before each line of text */
static void
write_note (FILE *fp, const std::string &text)
{
	size_t start = 0;

	while (start < text.size ()) {
		size_t end = text.find ('\n', start);
		if (end == std::string::npos)
			end = text.size ();
		fprintf (fp, "# %s\n", text.substr (start, end - start).c_str ());
		start = end + 1;
	}
}

/****************************************************************************
 Bindings
****************************************************************************/

enum SourceKind {
	SRC_KEY,
	SRC_JOY_BUTTON,
	SRC_JOY_HAT,
	SRC_JOY_AXIS,
	SRC_GC_BUTTON,
	SRC_GC_AXIS
};

enum AxisPart {
	PART_FULL,
	PART_POSITIVE,		/* half an axis as a button */
	PART_NEGATIVE,
	PART_POSITIVE_HALF,	/* half an axis as a pedal: 0 at the middle */
	PART_NEGATIVE_HALF
};

struct Controls::Source {
	SourceKind kind;
	int player;
	int index;	/* a key's scancode, a button's, axis's, hat's number */
	int hat_dir;
	AxisPart part;
	bool inverted;
};

/* Its alternatives, each of up to three inputs held together ("A + B") */
struct Controls::Binding {
	std::vector<std::vector<Source>> alternatives;
};

static std::string
trim (const std::string &s)
{
	size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
	return (a == std::string::npos) ? "" : s.substr (a, b - a + 1);
}

/* "Left Shift", "left_shift" and "LEFTSHIFT" are the same name */
static std::string
normalize (const std::string &s)
{
	std::string out;
	for (char c : s)
		if (c != ' ' && c != '_')
			out += (char) toupper ((unsigned char) c);
	return out;
}

static bool
ends_with (std::string &s, const char *suffix)
{
	size_t n = strlen (suffix);
	if (s.size () <= n || strcasecmp (s.c_str () + s.size () - n, suffix))
		return false;
	s.resize (s.size () - n);
	return true;
}

/* A key's name, as linuxloader and SDL write it, to its scancode */
static int
key_scancode (const std::string &name)
{
	static std::map<std::string, int> names;
	static const struct { const char *alias, *name; } aliases[] = {
		{ "ESC", "ESCAPE" }, { "ENTER", "RETURN" }, { "ARROWLEFT", "LEFT" },
		{ "ARROWRIGHT", "RIGHT" }, { "ARROWUP", "UP" }, { "ARROWDOWN", "DOWN" },
		{ "LSHIFT", "LEFTSHIFT" }, { "RSHIFT", "RIGHTSHIFT" }, { "LCTRL", "LEFTCTRL" },
		{ "RCTRL", "RIGHTCTRL" }, { "LALT", "LEFTALT" }, { "RALT", "RIGHTALT" },
		{ "COMMA", "," }, { "PERIOD", "." }, { "PAGEUP", "PAGEUP" },
	};
	std::string n = normalize (name);
	int sc;

	/* The keys' names in the keyboard's layout first (KEY_A is the key
	 * marked A, on an AZERTY keyboard too), then the US names left */
	if (names.empty ()) {
		for (sc = 1; sc < SDL_NUM_SCANCODES; sc++) {
			SDL_Keycode key = SDL_GetKeyFromScancode ((SDL_Scancode) sc);
			const char *kn = key != SDLK_UNKNOWN ? SDL_GetKeyName (key) : "";
			if (kn[0] && !names.count (normalize (kn)))
				names[normalize (kn)] = sc;
		}
		for (sc = 1; sc < SDL_NUM_SCANCODES; sc++) {
			const char *kn = SDL_GetScancodeName ((SDL_Scancode) sc);
			if (kn[0] && !names.count (normalize (kn)))
				names[normalize (kn)] = sc;
		}
	}
	for (const auto &a : aliases)
		if (n == a.alias)
			n = a.name;
	if (n.size () > 2 && n.compare (0, 2, "KP") == 0 && isdigit ((unsigned char) n.back ()))
		n = std::string ("KEYPAD") + n.back ();
	auto it = names.find (n);
	return (it != names.end ()) ? it->second : 0;
}

/* One input ("JOY0_AXIS_1_INVERTED"), false when not understood */
static bool
parse_source (const std::string &text, Controls::Source *s)
{
	std::string t = trim (text), p;
	bool gc;

	memset ((void *) s, 0, sizeof (*s));
	if (!strncasecmp (t.c_str (), "KEY_", 4)) {
		s->kind = SRC_KEY;
		s->index = key_scancode (t.substr (4));
		return s->index != 0;
	}

	gc = !strncasecmp (t.c_str (), "GC", 2);
	if (!gc && strncasecmp (t.c_str (), "JOY", 3))
		return false;
	p = t.substr (gc ? 2 : 3);
	s->player = atoi (p.c_str ());
	while (!p.empty () && isdigit ((unsigned char) p[0]))
		p.erase (0, 1);
	if (p.empty () || p[0] != '_')
		return false;
	p.erase (0, 1);

	s->inverted = ends_with (p, "_INVERTED");
	if (ends_with (p, "_POSITIVE_SIDE") || ends_with (p, "_POSITIVE_HALF"))
		s->part = PART_POSITIVE_HALF;
	else if (ends_with (p, "_NEGATIVE_SIDE") || ends_with (p, "_NEGATIVE_HALF"))
		s->part = PART_NEGATIVE_HALF;
	else if (ends_with (p, "_POSITIVE"))
		s->part = PART_POSITIVE;
	else if (ends_with (p, "_NEGATIVE"))
		s->part = PART_NEGATIVE;
	if (!s->inverted)
		s->inverted = ends_with (p, "_INVERTED");

	if (!strncasecmp (p.c_str (), "BUTTON_", 7)) {
		s->kind = gc ? SRC_GC_BUTTON : SRC_JOY_BUTTON;
		if (gc) {
			std::string n = normalize (p.substr (7));
			if (!n.compare (0, 4, "DPAD"))
				n = "DP" + n.substr (4);
			std::transform (n.begin (), n.end (), n.begin (), ::tolower);
			s->index = SDL_GameControllerGetButtonFromString (n.c_str ());
		} else
			s->index = atoi (p.c_str () + 7);
		return s->index >= 0;
	}
	if (!strncasecmp (p.c_str (), "AXIS_", 5)) {
		s->kind = gc ? SRC_GC_AXIS : SRC_JOY_AXIS;
		if (gc) {
			std::string n = normalize (p.substr (5));
			if (n == "TRIGGERLEFT")
				n = "LEFTTRIGGER";
			else if (n == "TRIGGERRIGHT")
				n = "RIGHTTRIGGER";
			std::transform (n.begin (), n.end (), n.begin (), ::tolower);
			s->index = SDL_GameControllerGetAxisFromString (n.c_str ());
		} else
			s->index = atoi (p.c_str () + 5);
		return s->index >= 0;
	}
	if (!gc && !strncasecmp (p.c_str (), "HAT", 3)) {
		size_t us = p.find ('_');
		std::string dir;
		s->kind = SRC_JOY_HAT;
		s->index = atoi (p.c_str () + 3);
		if (us == std::string::npos)
			return false;
		dir = normalize (p.substr (us + 1));
		s->hat_dir = (dir == "UP") ? SDL_HAT_UP : (dir == "DOWN") ? SDL_HAT_DOWN :
		             (dir == "LEFT") ? SDL_HAT_LEFT : (dir == "RIGHT") ? SDL_HAT_RIGHT : 0;
		return s->hat_dir != 0;
	}
	return false;
}

std::vector<std::string>
Controls::alternatives (const std::string &value)
{
	std::vector<std::string> out;
	size_t start = 0;

	while (start <= value.size ()) {
		size_t comma = value.find (',', start);
		std::string t = trim (value.substr (start, comma == std::string::npos ?
		                                    std::string::npos : comma - start));
		if (!t.empty ())
			out.push_back (t);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out;
}

std::string
Controls::join (const std::vector<std::string> &tokens)
{
	std::string out;
	for (const auto &t : tokens)
		out += (out.empty () ? "" : ", ") + t;
	return out;
}

bool
Controls::owns (int player, const std::string &token)
{
	char joy[16], gc[16];

	if (player < 0)
		return !strncasecmp (token.c_str (), "KEY_", 4);
	snprintf (joy, sizeof (joy), "JOY%d_", player);
	snprintf (gc, sizeof (gc), "GC%d_", player);
	return !strncasecmp (token.c_str (), joy, strlen (joy)) ||
	       !strncasecmp (token.c_str (), gc, strlen (gc));
}

/****************************************************************************
 The file
****************************************************************************/

Controls &
Controls::get ()
{
	static Controls controls;
	return controls;
}

Controls::Controls () :
	enabled (true),
	capturing (false),
	capture_started (false),
	capture_player (-1),
	capture_kind (CAPTURE_BUTTON)
{
}

/* The file as sections of rows, the lines but those dropped */
static std::vector<std::pair<std::string, std::vector<Controls::Row>>>
parse_ini (const std::string &text)
{
	std::vector<std::pair<std::string, std::vector<Controls::Row>>> out;
	size_t start = 0;

	while (start < text.size ()) {
		size_t end = text.find ('\n', start);
		std::string line = trim (text.substr (start, end == std::string::npos ?
		                                      std::string::npos : end - start));
		start = (end == std::string::npos) ? text.size () : end + 1;
		if (line.empty () || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[') {
			size_t close = line.find (']');
			out.push_back ({ line.substr (1, close == std::string::npos ? std::string::npos : close - 1), {} });
			continue;
		}
		size_t eq = line.find ('=');
		if (eq == std::string::npos || out.empty ())
			continue;
		out.back ().second.push_back ({ trim (line.substr (0, eq)), trim (line.substr (eq + 1)) });
	}
	return out;
}

void
Controls::load (const char *path, const char *save_to)
{
	auto defaults = parse_ini (default_file);
	bool created = false, added = false;

	file = path;
	{
		std::ifstream in (file, std::ios::binary);
		if (in) {
			std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char> ());
			sections = parse_ini (text);
		} else {
			sections = defaults;
			created = true;
		}
	}
	if (save_to)
		file = save_to;

	/* The default lines it lacks: a newer version's actions */
	for (const auto &ds : defaults) {
		auto it = std::find_if (sections.begin (), sections.end (),
		                        [&] (const auto &s) { return !strcasecmp (s.first.c_str (), ds.first.c_str ()); });
		if (it == sections.end ()) {
			sections.push_back (ds);
			added = true;
			continue;
		}
		for (const auto &row : ds.second)
			if (std::none_of (it->second.begin (), it->second.end (),
			                  [&] (const Row &r) { return !strcasecmp (r.key.c_str (), row.key.c_str ()); })) {
				it->second.push_back (row);
				added = true;
			}
	}

	if (created)
		migrate_old_bindings ();
	parse_all ();
	open_devices ();
	if (created || added) {
		save ();
		HL_PRINT ("controls: %s %s", file.c_str (), created ? "created" : "completed with the default bindings");
	} else
		HL_PRINT ("controls: the bindings from '%s'", path);
}

void
Controls::save () const
{
	FILE *fp = fopen (file.c_str (), "w");

	if (!fp) {
		HL_ERROR ("controls: cannot write '%s'", file.c_str ());
		return;
	}
	fputs (file_notes, fp);
	for (const auto &s : sections) {
		const char *note = note_of (s.first, NULL);
		fprintf (fp, "\n");
		if (note)
			write_note (fp, note);
		fprintf (fp, "[%s]\n", s.first.c_str ());
		for (const auto &row : s.second) {
			std::string text = note_of (s.first, row.key.c_str ()) ? note_of (s.first, row.key.c_str ()) : "";
			std::string def = default_value (s.first.c_str (), row.key.c_str ());
			/* The default on the note's line when it fits, else below */
			if (!def.empty ()) {
				if (text.empty ())
					text = "Default: " + def;
				else if (text.size () + def.size () + 13 <= 79)
					text += "; default: " + def;
				else
					text += "\nDefault: " + def;
			}
			if (!text.empty ())
				fprintf (fp, "\n");
			write_note (fp, text);
			fprintf (fp, "%s = %s\n", row.key.c_str (), row.value.c_str ());
		}
	}
	fclose (fp);
}

std::vector<Controls::Row>
Controls::rows (const char *section) const
{
	for (const auto &s : sections)
		if (!strcasecmp (s.first.c_str (), section))
			return s.second;
	return {};
}

std::string
Controls::value (const char *section, const char *key) const
{
	for (const auto &s : sections)
		if (!strcasecmp (s.first.c_str (), section))
			for (const auto &row : s.second)
				if (!strcasecmp (row.key.c_str (), key))
					return row.value;
	return "";
}

void
Controls::set_value (const char *section, const char *key, const std::string &val)
{
	auto it = std::find_if (sections.begin (), sections.end (),
	                        [&] (const auto &s) { return !strcasecmp (s.first.c_str (), section); });
	if (it == sections.end ()) {
		sections.push_back ({ section, {} });
		it = sections.end () - 1;
	}
	auto row = std::find_if (it->second.begin (), it->second.end (),
	                         [&] (const Row &r) { return !strcasecmp (r.key.c_str (), key); });
	if (row == it->second.end ())
		it->second.push_back ({ key, val });
	else
		row->value = val;
	parse_all ();
	if (!strcasecmp (section, "ControllerGUIDs"))
		open_devices ();
	save ();
}

std::string
Controls::default_value (const char *section, const char *key) const
{
	for (const auto &s : parse_ini (default_file))
		if (!strcasecmp (s.first.c_str (), section))
			for (const auto &row : s.second)
				if (!strcasecmp (row.key.c_str (), key))
					return row.value;
	return "";
}

void
Controls::parse_all ()
{
	for (auto &p : parsed)
		delete p.second;
	parsed.clear ();

	for (const auto &s : sections) {
		if (!strcasecmp (s.first.c_str (), "Config") || !strcasecmp (s.first.c_str (), "ControllerGUIDs"))
			continue;
		for (const auto &row : s.second) {
			Binding *b = new Binding;
			for (const auto &alt : alternatives (row.value)) {
				std::vector<Source> parts;
				size_t start = 0;
				while (start <= alt.size () && parts.size () < 3) {
					size_t plus = alt.find ('+', start);
					Source src;
					std::string part = alt.substr (start, plus == std::string::npos ?
					                               std::string::npos : plus - start);
					if (parse_source (part, &src))
						parts.push_back (src);
					else
						HL_ERROR ("controls: [%s] %s: '%s' not understood",
						          s.first.c_str (), row.key.c_str (), trim (part).c_str ());
					if (plus == std::string::npos)
						break;
					start = plus + 1;
				}
				if (!parts.empty ())
					b->alternatives.push_back (parts);
			}
			parsed[normalize (s.first) + "/" + normalize (row.key)] = b;
		}
	}
}

const Controls::Binding *
Controls::binding (const char *section, const char *key)
{
	auto it = parsed.find (normalize (section) + "/" + normalize (key));
	return (it != parsed.end ()) ? it->second : NULL;
}

/* The old [Controls] of hikaruloader.ini, read by Config: its keys and
 * buttons that differ from the old defaults replace the new file's */
void
Controls::migrate_old_bindings ()
{
	const Config *cfg = Config::get ();
	unsigned i;

	for (i = 0; i < NUM_ACTIONS; i++) {
		const char *section, *key;
		hl::Binding old = cfg->bindings[i], def = Input::default_binding ((Action) i);
		if (!Input::row_of ((Action) i, &section, &key))
			continue;
		if (old.key == def.key && old.button == def.button)
			continue;

		std::vector<std::string> tokens = alternatives (value (section, key)), out;
		bool key_done = false, button_done = false;
		for (const auto &t : tokens) {
			if (!strncasecmp (t.c_str (), "KEY_", 4)) {
				if (old.key != def.key) {
					if (!key_done && old.key != SDLK_UNKNOWN) {
						std::string n = SDL_GetKeyName (old.key);
						n.erase (std::remove (n.begin (), n.end (), ' '), n.end ());
						out.push_back ("KEY_" + n);
					}
					key_done = true;
					continue;
				}
			} else if (!strncasecmp (t.c_str (), "GC0_BUTTON_", 11)) {
				if (old.button != def.button) {
					const char *n = old.button >= 0 ?
						SDL_GameControllerGetStringForButton ((SDL_GameControllerButton) old.button) : NULL;
					if (!button_done && n) {
						std::string u = n;
						std::transform (u.begin (), u.end (), u.begin (), ::toupper);
						out.push_back ("GC0_BUTTON_" + u);
					}
					button_done = true;
					continue;
				}
			}
			out.push_back (t);
		}
		if (!key_done && old.key != def.key && old.key != SDLK_UNKNOWN) {
			std::string n = SDL_GetKeyName (old.key);
			n.erase (std::remove (n.begin (), n.end (), ' '), n.end ());
			out.insert (out.begin (), "KEY_" + n);
		}
		if (!button_done && old.button != def.button && old.button >= 0) {
			std::string u = SDL_GameControllerGetStringForButton ((SDL_GameControllerButton) old.button);
			std::transform (u.begin (), u.end (), u.begin (), ::toupper);
			out.push_back ("GC0_BUTTON_" + u);
		}
		for (auto &s : sections)
			if (!strcasecmp (s.first.c_str (), section))
				for (auto &row : s.second)
					if (!strcasecmp (row.key.c_str (), key))
						row.value = join (out);
		HL_PRINT ("controls: %s from hikaruloader.ini's [Controls]: %s", key, join (out).c_str ());
	}
}

/****************************************************************************
 Devices: player n is the device whose GUID is P<n+1>_GUID, then the others
 in SDL's order
****************************************************************************/

void
Controls::close_devices ()
{
	for (auto &p : players) {
		if (p.controller)
			SDL_GameControllerClose ((SDL_GameController *) p.controller);
		else if (p.joystick)
			SDL_JoystickClose ((SDL_Joystick *) p.joystick);
	}
	players.clear ();
}

void
Controls::open_devices ()
{
	int count = SDL_NumJoysticks (), i, p;
	std::vector<bool> taken (count > 0 ? count : 0, false);
	char guid[64];

	close_devices ();
	if (count <= 0)
		return;
	players.assign (count, Player { -1, NULL, NULL, "" });

	/* The players named by their GUID first */
	for (p = 0; p < count; p++) {
		char key[16];
		snprintf (key, sizeof (key), "P%d_GUID", p + 1);
		std::string want = value ("ControllerGUIDs", key);
		if (want.empty ())
			continue;
		for (i = 0; i < count; i++) {
			SDL_JoystickGetGUIDString (SDL_JoystickGetDeviceGUID (i), guid, sizeof (guid));
			if (!taken[i] && !strcasecmp (guid, want.c_str ())) {
				players[p].instance = i;
				taken[i] = true;
				break;
			}
		}
	}
	for (i = 0, p = 0; i < count; i++) {
		if (taken[i])
			continue;
		while (p < count && players[p].instance >= 0)
			p++;
		if (p == count)
			break;
		players[p].instance = i;
		taken[i] = true;
	}

	/* instance holds the device index until opened */
	for (p = 0; p < count; p++) {
		Player &pl = players[p];
		int index = pl.instance;
		if (index < 0)
			continue;
		if (SDL_IsGameController (index)) {
			pl.controller = SDL_GameControllerOpen (index);
			pl.joystick = pl.controller ? SDL_GameControllerGetJoystick ((SDL_GameController *) pl.controller) : NULL;
		}
		if (!pl.joystick)
			pl.joystick = SDL_JoystickOpen (index);
		pl.instance = pl.joystick ? SDL_JoystickInstanceID ((SDL_Joystick *) pl.joystick) : -1;
		pl.name = pl.joystick ? SDL_JoystickName ((SDL_Joystick *) pl.joystick) : "";
		SDL_JoystickGetGUIDString (SDL_JoystickGetDeviceGUID (index), guid, sizeof (guid));
		HL_PRINT ("controls: player %d = %s (%s, GUID %s)", p + 1, pl.name.c_str (),
		          pl.controller ? "game controller" : "joystick", guid);
	}
}

void
Controls::devices_changed ()
{
	capturing = false;
	open_devices ();
}

std::vector<Controls::Device>
Controls::devices () const
{
	std::vector<Device> out;

	out.push_back ({ -1, false, "" });
	for (size_t p = 0; p < players.size (); p++)
		if (players[p].joystick)
			out.push_back ({ (int) p, players[p].controller != NULL, players[p].name });
	return out;
}

/****************************************************************************
 Reading
****************************************************************************/

static bool
keyboard_focus ()
{
	return SDL_GetKeyboardFocus () != NULL;
}

double
Controls::source_axis (const Source &s)
{
	const Player *p = (s.player >= 0 && s.player < (int) players.size ()) ? &players[s.player] : NULL;
	double a = 0.0;

	if (s.kind == SRC_JOY_AXIS && p && p->joystick)
		a = SDL_JoystickGetAxis ((SDL_Joystick *) p->joystick, s.index) / 32767.0;
	else if (s.kind == SRC_GC_AXIS && p && p->controller)
		a = SDL_GameControllerGetAxis ((SDL_GameController *) p->controller,
		                               (SDL_GameControllerAxis) s.index) / 32767.0;
	if (a < -1.0)
		a = -1.0;
	return s.inverted ? -a : a;
}

bool
Controls::source_pressed (const Source &s)
{
	const Player *p = (s.player >= 0 && s.player < (int) players.size ()) ? &players[s.player] : NULL;

	switch (s.kind) {
	case SRC_KEY: {
		int n = 0;
		const Uint8 *keys = SDL_GetKeyboardState (&n);
		return keyboard_focus () && s.index < n && keys[s.index];
	}
	case SRC_JOY_BUTTON:
		return p && p->joystick && SDL_JoystickGetButton ((SDL_Joystick *) p->joystick, s.index);
	case SRC_JOY_HAT:
		return p && p->joystick && (SDL_JoystickGetHat ((SDL_Joystick *) p->joystick, s.index) & s.hat_dir);
	case SRC_GC_BUTTON:
		return p && p->controller &&
		       SDL_GameControllerGetButton ((SDL_GameController *) p->controller,
		                                    (SDL_GameControllerButton) s.index);
	default: {
		double a = source_axis (s);
		return (s.part == PART_NEGATIVE || s.part == PART_NEGATIVE_HALF) ? a < -0.5 : a > 0.5;
	}
	}
}

/* A pedal 0..1 from one input: a trigger's own range, an axis from end to
 * end, half an axis from its middle */
double
Controls::source_pedal (const Source &s)
{
	if (s.kind != SRC_JOY_AXIS && s.kind != SRC_GC_AXIS)
		return source_pressed (s) ? 1.0 : 0.0;

	double a = source_axis (s);
	switch (s.part) {
	case PART_POSITIVE_HALF:
		return a > 0 ? a : 0;
	case PART_NEGATIVE_HALF:
		return a < 0 ? -a : 0;
	case PART_POSITIVE:
	case PART_NEGATIVE:
		return source_pressed (s) ? 1.0 : 0.0;
	default:
		if (s.kind == SRC_GC_AXIS && (s.index == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
		                              s.index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
			return a > 0 ? a : 0;
		return (a + 1.0) / 2.0;
	}
}

static double
apply_deadzone (double v, double dz)
{
	double m = fabs (v);
	if (m <= dz)
		return 0.0;
	return (v > 0 ? 1.0 : -1.0) * (m - dz) / (1.0 - dz);
}

/* [Config] <Action>_DeadZone, the action without its P<n>_ */
double
Controls::deadzone (const char *key) const
{
	std::string k = key;
	if (k.size () > 3 && (k[0] == 'P' || k[0] == 'p') && isdigit ((unsigned char) k[1]) && k[2] == '_')
		k.erase (0, 3);
	std::string v = value ("Config", (k + "_DeadZone").c_str ());
	double dz = v.empty () ? 0.0 : atoi (v.c_str ()) / 32767.0;
	return dz < 0 ? 0 : dz > 0.95 ? 0.95 : dz;
}

bool
Controls::pressed (const char *section, const char *key)
{
	const Binding *b = enabled ? binding (section, key) : NULL;

	if (!b)
		return false;
	for (const auto &alt : b->alternatives) {
		bool all = true;
		for (const auto &s : alt)
			all = all && source_pressed (s);
		if (all)
			return true;
	}
	return false;
}

/* An axis's value before the event: each axis's last two values, rolled on
 * a new event (the rows asked about one event see the same) */
double
Controls::axis_before (const SDL_Event *event, int player)
{
	bool joy = event->type == SDL_JOYAXISMOTION;
	int axis = joy ? event->jaxis.axis : event->caxis.axis;
	int value = joy ? event->jaxis.value : event->caxis.value;
	AxisMemo &m = axis_memo[(player * 2 + (joy ? 0 : 1)) * 64 + (axis & 63)];

	if (m.stamp != event->common.timestamp || m.value != value) {
		m.before = m.now;
		m.now = value / 32767.0;
		m.stamp = event->common.timestamp;
		m.value = value;
	}
	return m.before;
}

/* A key event's scancode; from its key when it has none (keys sent by
 * another program) */
static int
event_scancode (const SDL_Event *event)
{
	if (event->key.keysym.scancode != SDL_SCANCODE_UNKNOWN)
		return event->key.keysym.scancode;
	return SDL_GetScancodeFromKey (event->key.keysym.sym);
}

bool
Controls::pressed_by (const char *section, const char *key, const SDL_Event *event)
{
	const Binding *b = enabled ? binding (section, key) : NULL;
	int player = -1, which = -1;
	size_t p;

	if (!b)
		return false;
	switch (event->type) {
	case SDL_KEYDOWN:
		if (event->key.repeat)
			return false;
		break;
	case SDL_JOYBUTTONDOWN:	which = event->jbutton.which; break;
	case SDL_JOYHATMOTION:	which = event->jhat.which; break;
	case SDL_JOYAXISMOTION:	which = event->jaxis.which; break;
	case SDL_CONTROLLERBUTTONDOWN: which = event->cbutton.which; break;
	case SDL_CONTROLLERAXISMOTION: which = event->caxis.which; break;
	default:
		return false;
	}
	for (p = 0; p < players.size (); p++)
		if (which >= 0 && players[p].instance == which)
			player = (int) p;

	/* The event's own input is read from the event: SDL may have seen its
	 * release already (a tap within a frame); the others' from their state */
	for (const auto &alt : b->alternatives) {
		bool mine = false, all = true;
		for (const auto &s : alt) {
			bool hit = false;
			switch (event->type) {
			case SDL_KEYDOWN:
				hit = s.kind == SRC_KEY && s.index == event_scancode (event);
				break;
			case SDL_JOYBUTTONDOWN:
				hit = s.kind == SRC_JOY_BUTTON && s.player == player && s.index == event->jbutton.button;
				break;
			case SDL_JOYHATMOTION:
				hit = s.kind == SRC_JOY_HAT && s.player == player && s.index == event->jhat.hat &&
				      (event->jhat.value & s.hat_dir);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				hit = s.kind == SRC_GC_BUTTON && s.player == player && s.index == event->cbutton.button;
				break;
			default: {
				/* Half an axis just past its half way */
				bool joy = event->type == SDL_JOYAXISMOTION;
				int axis = joy ? event->jaxis.axis : event->caxis.axis;
				if (s.kind == (joy ? SRC_JOY_AXIS : SRC_GC_AXIS) && s.player == player && s.index == axis &&
				    (s.part == PART_POSITIVE || s.part == PART_NEGATIVE)) {
					double sign = s.inverted ? -1.0 : 1.0;
					double now = (joy ? event->jaxis.value : event->caxis.value) / 32767.0 * sign;
					double before = axis_before (event, player) * sign;
					if (s.part == PART_NEGATIVE)
						hit = now < -0.5 && !(before < -0.5);
					else
						hit = now > 0.5 && !(before > 0.5);
				}
				break;
			}
			}
			mine |= hit;
			all = all && (hit || source_pressed (s));
		}
		if (mine && all)
			return true;
	}
	return false;
}

double
Controls::axis (const char *section, const char *key)
{
	const Binding *b = enabled ? binding (section, key) : NULL;
	double v = 0.0, dz = deadzone (key);

	/* The input turned furthest wins */
	if (b)
		for (const auto &alt : b->alternatives) {
			const Source &s = alt[0];
			double a;
			if ((s.kind == SRC_JOY_AXIS || s.kind == SRC_GC_AXIS) && s.part == PART_FULL)
				a = apply_deadzone (source_axis (s), dz);
			else
				a = source_pressed (s) ? ((s.part == PART_NEGATIVE) ? -1.0 : 1.0) : 0.0;
			if (fabs (a) > fabs (v))
				v = a;
		}
	if (pressed (section, (std::string (key) + "_Left").c_str ()))
		v = -1.0;
	if (pressed (section, (std::string (key) + "_Right").c_str ()))
		v = 1.0;
	return v;
}

double
Controls::pedal (const char *section, const char *key)
{
	const Binding *b = enabled ? binding (section, key) : NULL;
	double v = pressed (section, (std::string (key) + "_Digital").c_str ()) ? 1.0 : 0.0;
	double dz = deadzone (key);

	if (b)
		for (const auto &alt : b->alternatives) {
			double p = apply_deadzone (source_pedal (alt[0]), dz);
			if (p > v)
				v = p;
		}
	return v;
}

int
Controls::input_state (const std::string &token, bool *on, int *raw)
{
	std::vector<Source> parts;
	size_t start = 0;

	*on = false;
	*raw = 0;
	while (start <= token.size ()) {
		size_t plus = token.find ('+', start);
		Source s;
		if (parse_source (token.substr (start, plus == std::string::npos ?
		                                std::string::npos : plus - start), &s))
			parts.push_back (s);
		if (plus == std::string::npos)
			break;
		start = plus + 1;
	}
	if (parts.empty ())
		return 0;

	const Source &s = parts[0];
	if (s.kind != SRC_KEY) {
		const Player *p = (s.player < (int) players.size ()) ? &players[s.player] : NULL;
		bool gc = s.kind == SRC_GC_AXIS || s.kind == SRC_GC_BUTTON;
		if (!p || !(gc ? p->controller : p->joystick))
			return 0;
	}
	if (parts.size () == 1 && (s.kind == SRC_JOY_AXIS || s.kind == SRC_GC_AXIS) && s.part != PART_POSITIVE &&
	    s.part != PART_NEGATIVE) {
		*raw = (int) lround (source_axis (s) * 32767.0);
		return 2;
	}
	*on = true;
	for (const auto &part : parts)
		*on = *on && source_pressed (part);
	return 1;
}

/****************************************************************************
 Capture
****************************************************************************/

void
Controls::capture (int player, CaptureKind kind)
{
	capturing = true;
	capture_started = false;
	capture_player = player;
	capture_kind = kind;
	capture_found.clear ();
}

/* KEY_<name> of a key, "" for none */
static std::string
key_token (int scancode)
{
	std::string name = SDL_GetKeyName (SDL_GetKeyFromScancode ((SDL_Scancode) scancode));
	name.erase (std::remove (name.begin (), name.end (), ' '), name.end ());
	return name.empty () ? "" : "KEY_" + name;
}

void
Controls::capture_event (const SDL_Event *event)
{
	char found[64];
	int sc;

	if (!capturing || !capture_started || !capture_found.empty ())
		return;
	switch (event->type) {
	case SDL_KEYDOWN:
		sc = event_scancode (event);
		/* Escape cancels, Tab is the menu's */
		if (capture_player < 0 && !event->key.repeat &&
		    sc != SDL_SCANCODE_ESCAPE && sc != SDL_SCANCODE_TAB)
			capture_found = key_token (sc);
		break;
	case SDL_JOYBUTTONDOWN:
		if (capture_player >= 0 && capture_player < (int) players.size () &&
		    players[capture_player].instance == event->jbutton.which && capture_kind == CAPTURE_BUTTON) {
			snprintf (found, sizeof (found), "JOY%d_BUTTON_%d", capture_player, event->jbutton.button);
			capture_found = found;
		}
		break;
	}
}

bool
Controls::capture_step (std::string &token)
{
	static const char *dirs[4] = { "UP", "RIGHT", "DOWN", "LEFT" };
	char found[64] = "";
	int n = 0, p, i;
	const Uint8 *keys = SDL_GetKeyboardState (&n);

	if (!capturing)
		return false;

	/* The state at the start: only a change counts (a pedal held, an
	 * axis off centre are no input) */
	if (!capture_started) {
		capture_started = true;
		base_keys.assign (keys, keys + n);
		base_axes.assign (players.size (), {});
		base_buttons.assign (players.size (), {});
		base_hats.assign (players.size (), {});
		for (p = 0; p < (int) players.size (); p++) {
			SDL_Joystick *j = (SDL_Joystick *) players[p].joystick;
			if (!j)
				continue;
			for (i = 0; i < SDL_JoystickNumAxes (j); i++)
				base_axes[p].push_back (SDL_JoystickGetAxis (j, i) / 32767.0);
			for (i = 0; i < SDL_JoystickNumButtons (j); i++)
				base_buttons[p].push_back (SDL_JoystickGetButton (j, i));
			for (i = 0; i < SDL_JoystickNumHats (j); i++)
				base_hats[p].push_back (SDL_JoystickGetHat (j, i));
		}
		return false;
	}

	/* A press seen by its event (let go within the frame), else a change
	 * of state */
	if (!capture_found.empty ())
		snprintf (found, sizeof (found), "%s", capture_found.c_str ());
	if (capture_player < 0 && keyboard_focus () && !found[0]) {
		for (i = 1; i < n && i < (int) base_keys.size () && !found[0]; i++) {
			/* Escape cancels, Tab is the menu's */
			if (i == SDL_SCANCODE_ESCAPE || i == SDL_SCANCODE_TAB)
				continue;
			if (keys[i] && !base_keys[i])
				snprintf (found, sizeof (found), "%s", key_token (i).c_str ());
			if (!keys[i])
				base_keys[i] = 0;
		}
	}

	p = capture_player;
	if (p >= 0 && p < (int) players.size () && players[p].joystick && !found[0]) {
		SDL_Joystick *j = (SDL_Joystick *) players[p].joystick;
		for (i = 0; i < SDL_JoystickNumButtons (j) && i < (int) base_buttons[p].size () && !found[0]; i++)
			if (SDL_JoystickGetButton (j, i) && !base_buttons[p][i])
				snprintf (found, sizeof (found), "JOY%d_BUTTON_%d", p, i);
		for (i = 0; i < SDL_JoystickNumHats (j) && i < (int) base_hats[p].size () && !found[0]; i++) {
			int now = SDL_JoystickGetHat (j, i) & ~base_hats[p][i];
			for (int d = 0; d < 4 && !found[0]; d++)
				if (now & (1 << d))
					snprintf (found, sizeof (found), "JOY%d_HAT%d_%s", p, i, dirs[d]);
		}
		for (i = 0; i < SDL_JoystickNumAxes (j) && i < (int) base_axes[p].size () && !found[0]; i++) {
			double a0 = base_axes[p][i], a1 = SDL_JoystickGetAxis (j, i) / 32767.0;
			const char *suffix;
			if (fabs (a1 - a0) < 0.5)
				continue;
			if (capture_kind == CAPTURE_STEERING)
				/* Turned to the left: SDL's left is negative */
				suffix = (a1 < a0) ? "" : "_INVERTED";
			else if (capture_kind == CAPTURE_PEDAL)
				/* At rest near an end: the whole axis; in the middle (the
				 * BFF's and vJoy's pedals): the half it went through */
				suffix = (fabs (a0) > 0.8) ? ((a0 > 0) ? "_INVERTED" : "") :
				         ((a1 > a0) ? "_POSITIVE_SIDE" : "_NEGATIVE_SIDE");
			else
				suffix = (a1 > a0) ? "_POSITIVE" : "_NEGATIVE";
			snprintf (found, sizeof (found), "JOY%d_AXIS_%d%s", p, i, suffix);
		}
	}

	if (!found[0])
		return false;
	token = found;
	capturing = false;
	HL_PRINT ("controls: captured %s", found);
	return true;
}

/****************************************************************************
 Names
****************************************************************************/

static std::string
axis_suffix (const std::string &s, const char *(*tr) (const char *))
{
	if (!strcasecmp (s.c_str (), "_POSITIVE"))
		return " +";
	if (!strcasecmp (s.c_str (), "_NEGATIVE"))
		return " -";
	if (!strcasecmp (s.c_str (), "_POSITIVE_HALF") || !strcasecmp (s.c_str (), "_NEGATIVE_HALF"))
		return std::string (s[1] == 'P' || s[1] == 'p' ? " +" : " -") + tr (" (half)");
	if (!strcasecmp (s.c_str (), "_POSITIVE_SIDE") || !strcasecmp (s.c_str (), "_NEGATIVE_SIDE"))
		return std::string (s[1] == 'P' || s[1] == 'p' ? " +" : " -") + tr (" (pedal)");
	if (!strcasecmp (s.c_str (), "_INVERTED"))
		return tr (" (inverted)");
	return s;
}

std::string
Controls::describe (const std::string &token, const char *(*tr) (const char *), bool buttons_from_one)
{
	char out[160], name[32];
	int device = 0, number = 0, length = 0;

	if (token.find ('+') != std::string::npos) {
		std::string all;
		size_t start = 0;
		while (start <= token.size ()) {
			size_t plus = token.find ('+', start);
			std::string part = trim (token.substr (start, plus == std::string::npos ?
			                                       std::string::npos : plus - start));
			all += (all.empty () ? "" : " + ") + describe (part, tr, buttons_from_one);
			if (plus == std::string::npos)
				break;
			start = plus + 1;
		}
		return all;
	}

	const char *t = token.c_str ();
	if (!strncasecmp (t, "KEY_", 4))
		return token.substr (4);
	if (sscanf (t, "JOY%d_BUTTON_%d%n", &device, &number, &length) == 2 && !t[length]) {
		/* Only the name: the file keeps SDL's number */
		snprintf (out, sizeof (out), tr ("Button %d"), number + (buttons_from_one ? 1 : 0));
		return out;
	}
	if (sscanf (t, "JOY%d_AXIS_%d%n", &device, &number, &length) == 2) {
		snprintf (out, sizeof (out), tr ("Axis %d"), number);
		return out + axis_suffix (t + length, tr);
	}
	if (sscanf (t, "JOY%d_HAT%d_%31s", &device, &number, name) == 3) {
		const char *dir = !strcasecmp (name, "UP") ? tr ("up") : !strcasecmp (name, "DOWN") ? tr ("down") :
		                  !strcasecmp (name, "LEFT") ? tr ("left") : tr ("right");
		snprintf (out, sizeof (out), tr ("Hat %d"), number);
		return std::string (out) + " " + dir;
	}
	if (sscanf (t, "GC%d_BUTTON_%31s", &device, name) == 2)
		return name;
	if (sscanf (t, "GC%d_AXIS_%31[^_]%n", &device, name, &length) == 2)
		return name + axis_suffix (t + length, tr);
	return token;
}

} /* namespace hl */
