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

/*
 * The monitors, as the settings name them
 * =======================================
 *
 * Three ways, for a setting that stays right from one start to the next:
 *
 *  2		  The number Windows gives the monitor: its device \\.\DISPLAY2
 *  INDEX:1	  SDL's own number, from 0, which follows the order of the desktop
 *  DELL U4919	  A part of the monitor's name, in any case
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <ctype.h>
#include <string>

#include "hl/window.h"

namespace hl {

#ifdef _WIN32
struct MonitorSearch {
	int left, top;
	int number;
};

static BOOL CALLBACK
monitor_found (HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
	MonitorSearch *search = (MonitorSearch *) data;
	MONITORINFOEXA info;
	const char *digits;

	info.cbSize = sizeof (info);
	if (!GetMonitorInfoA (monitor, &info) ||
	    info.rcMonitor.left != search->left || info.rcMonitor.top != search->top)
		return TRUE;
	digits = strstr (info.szDevice, "DISPLAY");
	if (digits)
		search->number = atoi (digits + 7);
	return FALSE;
}
#endif

/* The number Windows gives that display of SDL's; 0 when not known */
static int
system_number (int display)
{
#ifdef _WIN32
	MonitorSearch search = { 0, 0, 0 };
	SDL_Rect bounds;

	if (SDL_GetDisplayBounds (display, &bounds))
		return 0;
	search.left = bounds.x;
	search.top = bounds.y;
	EnumDisplayMonitors (NULL, NULL, monitor_found, (LPARAM) &search);
	return search.number;
#else
	return display + 1;
#endif
}

static std::string
lower (const char *text)
{
	std::string out (text ? text : "");

	for (char &c : out)
		c = (char) tolower ((unsigned char) c);
	return out;
}

void
Window::log_displays ()
{
	int count = SDL_GetNumVideoDisplays (), i;

	for (i = 0; i < count; i++) {
		SDL_Rect r = { 0, 0, 0, 0 };

		SDL_GetDisplayBounds (i, &r);
		HL_PRINT ("window: monitor %d (INDEX:%d) '%s': %dx%d at %d,%d", system_number (i), i,
		          SDL_GetDisplayName (i), r.w, r.h, r.x, r.y);
	}
}

std::vector<Window::DisplayChoice>
Window::display_choices ()
{
	std::vector<DisplayChoice> list;
	int count = SDL_GetNumVideoDisplays (), i;

	for (i = 0; i < count; i++) {
		const char *name = SDL_GetDisplayName (i);
		SDL_DisplayMode mode = { 0, 0, 0, 0, NULL };
		SDL_Rect r = { 0, 0, 0, 0 };
		DisplayChoice choice;
		char text[256];
		int number = system_number (i);

		SDL_GetCurrentDisplayMode (i, &mode);
		SDL_GetDisplayBounds (i, &r);
		if (number)
			snprintf (text, sizeof (text), "%s - DISPLAY%d - %dx%d, %d Hz - at %d,%d", name ? name : "?",
			          number, r.w, r.h, mode.refresh_rate, r.x, r.y);
		else
			snprintf (text, sizeof (text), "%s - %dx%d, %d Hz - at %d,%d", name ? name : "?",
			          r.w, r.h, mode.refresh_rate, r.x, r.y);
		choice.label = text;
		/* Its name when that finds it: it follows the monitor around */
		if (name && name[0] && !isdigit ((unsigned char) name[0]) && find_display (name) == i)
			choice.value = name;
		else {
			snprintf (text, sizeof (text), "INDEX:%d", i);
			choice.value = text;
		}
		list.push_back (choice);
	}
	return list;
}

int
Window::find_display (const char *name)
{
	int count = SDL_GetNumVideoDisplays (), i;
	std::string wanted = lower (name);
	bool digits = !wanted.empty ();

	if (wanted.empty () || wanted == "auto")
		return -1;
	if (!wanted.compare (0, 6, "index:")) {
		i = atoi (wanted.c_str () + 6);
		return i >= 0 && i < count ? i : -1;
	}
	for (char c : wanted)
		digits = digits && isdigit ((unsigned char) c);
	if (digits) {
		for (i = 0; i < count; i++)
			if (system_number (i) == atoi (wanted.c_str ()))
				return i;
		return -1;
	}
	for (i = 0; i < count; i++)
		if (lower (SDL_GetDisplayName (i)).find (wanted) != std::string::npos)
			return i;
	return -1;
}

} /* namespace hl */
