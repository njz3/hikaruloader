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

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <SDL.h>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/pointers.h"

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#pragma comment (lib, "hid.lib")
#endif

namespace hl {

static std::mutex lock;
static Pointers::State states[Pointers::MOST];
static bool injected[Pointers::MOST];
static SDL_Window *the_window;
static bool captured;

void
Pointers::capture (bool on)
{
	if (on == captured || !the_window)
		return;
	captured = on;
	SDL_SetWindowGrab (the_window, on ? SDL_TRUE : SDL_FALSE);
	SDL_ShowCursor (on || Config::get ()->hide_cursor ? SDL_DISABLE : SDL_ENABLE);
	HL_PRINT ("pointer: the mouse %s", on ? "captured (Tab, Alt or the Windows key let it go)" : "let go");
}

void
Pointers::inject (unsigned index, const State &state)
{
	if (index >= MOST)
		return;
	std::lock_guard<std::mutex> guard (lock);
	states[index] = state;
	injected[index] = true;
}

#ifdef _WIN32

/* The devices, sorted by their names; the handles Windows gives them */
static std::vector<std::pair<std::string, HANDLE>> devices;
/* Their products' names, asked of the devices once (the Controls tab wants
 * them at each frame) */
static std::vector<std::string> products;

/* What the device calls itself, its VID and PID when it does not say */
static std::string
product_name (const std::string &path)
{
	HANDLE file = CreateFileA (path.c_str (), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	wchar_t wide[127] = L"";
	char text[256] = "";

	if (file != INVALID_HANDLE_VALUE) {
		if (HidD_GetProductString (file, wide, sizeof (wide)) && wide[0])
			WideCharToMultiByte (CP_UTF8, 0, wide, -1, text, sizeof (text), NULL, NULL);
		CloseHandle (file);
	}
	if (text[0])
		return text;
	size_t at = path.find ("VID_");
	if (at == std::string::npos)
		at = path.find ("vid_");
	return at != std::string::npos ? path.substr (at, 17) : "Mouse";
}

std::vector<std::string>
Pointers::names ()
{
	std::vector<std::string> out (products);

	if (out.size () > MOST)
		out.resize (MOST);
	return out;
}

static void
list_devices (void)
{
	UINT count = 0;
	std::vector<RAWINPUTDEVICELIST> list;

	devices.clear ();
	products.clear ();
	if (GetRawInputDeviceList (NULL, &count, sizeof (RAWINPUTDEVICELIST)) != 0 || !count)
		return;
	list.resize (count);
	if (GetRawInputDeviceList (list.data (), &count, sizeof (RAWINPUTDEVICELIST)) == (UINT) -1)
		return;
	for (UINT i = 0; i < count; i++) {
		char name[512] = "";
		UINT size = sizeof (name);

		if (list[i].dwType != RIM_TYPEMOUSE ||
		    GetRawInputDeviceInfoA (list[i].hDevice, RIDI_DEVICENAME, name, &size) == (UINT) -1)
			continue;
		/* A remote desktop's */
		if (strstr (name, "RDP_MOU"))
			continue;
		devices.push_back ({ name, list[i].hDevice });
	}
	std::sort (devices.begin (), devices.end ());
	for (unsigned i = 0; i < devices.size (); i++) {
		products.push_back (product_name (devices[i].first));
		HL_PRINT ("pointer: MOUSE%u is %s (%s)", i, products[i].c_str (), devices[i].first.c_str ());
	}
}

static int
device_index (HANDLE handle)
{
	for (int pass = 0; pass < 2; pass++) {
		for (unsigned i = 0; i < devices.size (); i++)
			if (devices[i].second == handle)
				return i < Pointers::MOST ? (int) i : -1;
		/* One plugged in since */
		if (pass == 0)
			list_devices ();
	}
	return -1;
}

/* The game's 4:3 picture, in the middle of the window: where -1 to 1 go */
static void
picture_of (HWND hwnd, double *left, double *top, double *w, double *h)
{
	RECT rect = { 0, 0, 1, 1 };
	POINT origin = { 0, 0 };

	GetClientRect (hwnd, &rect);
	ClientToScreen (hwnd, &origin);
	double cw = std::max (1L, rect.right), ch = std::max (1L, rect.bottom);
	*w = std::min (cw, ch * 4.0 / 3.0);
	*h = std::min (ch, cw * 3.0 / 4.0);
	*left = origin.x + (cw - *w) * 0.5;
	*top = origin.y + (ch - *h) * 0.5;
}

static void
packet (HWND hwnd, const RAWMOUSE &m, HANDLE handle)
{
	static const struct { USHORT down, up; } flags[Pointers::BUTTONS] = {
		{ RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP },
		{ RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP },
		{ RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP },
		{ RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP },
		{ RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP },
	};
	int index = device_index (handle);
	double left, top, w, h;

	if (index < 0)
		return;
	picture_of (hwnd, &left, &top, &w, &h);

	std::lock_guard<std::mutex> guard (lock);
	Pointers::State &s = states[index];

	if (injected[index])
		return;
	if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
		bool desktop = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
		double sx = desktop ? GetSystemMetrics (SM_XVIRTUALSCREEN) : 0;
		double sy = desktop ? GetSystemMetrics (SM_YVIRTUALSCREEN) : 0;
		double sw = GetSystemMetrics (desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
		double sh = GetSystemMetrics (desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
		double x = ((sx + m.lLastX / 65535.0 * sw) - left) / w * 2.0 - 1.0;
		double y = ((sy + m.lLastY / 65535.0 * sh) - top) / h * 2.0 - 1.0;

		s.off_screen = x < -1.0 || x > 1.0 || y < -1.0 || y > 1.0;
		s.x = std::clamp (x, -1.0, 1.0);
		s.y = std::clamp (y, -1.0, 1.0);
	} else {
		s.x = std::clamp (s.x + m.lLastX / w * 2.0, -1.0, 1.0);
		s.y = std::clamp (s.y + m.lLastY / h * 2.0, -1.0, 1.0);
		s.off_screen = false;
	}
	for (unsigned b = 0; b < Pointers::BUTTONS; b++) {
		if (m.usButtonFlags & flags[b].down)
			s.buttons |= 1u << b;
		else if (m.usButtonFlags & flags[b].up)
			s.buttons &= ~(1u << b);
	}
}

/* Inside SDL's event pump, on the window's thread */
static void SDLCALL
hook (void *, void *hwnd, unsigned int message, Uint64, Sint64 lparam)
{
	if (message == WM_KILLFOCUS) {
		std::lock_guard<std::mutex> guard (lock);
		for (unsigned i = 0; i < Pointers::MOST; i++)
			if (!injected[i])
				states[i].buttons = 0;
		return;
	}
	if (message != WM_INPUT)
		return;

	RAWINPUT raw;
	UINT size = sizeof (raw);

	if (GetRawInputData ((HRAWINPUT) lparam, RID_INPUT, &raw, &size, sizeof (RAWINPUTHEADER)) == (UINT) -1 ||
	    raw.header.dwType != RIM_TYPEMOUSE || !raw.header.hDevice)
		return;
	packet ((HWND) hwnd, raw.data.mouse, raw.header.hDevice);
}

void
Pointers::init (SDL_Window *window)
{
	the_window = window;
	list_devices ();
	SDL_SetWindowsMessageHook (hook, NULL);
}

void
Pointers::enable (bool on)
{
	static bool enabled;
	RAWINPUTDEVICE device;

	if (on == enabled)
		return;
	device.usUsagePage = 0x01;	/* generic desktop */
	device.usUsage = 0x02;		/* mouse */
	device.dwFlags = on ? 0 : RIDEV_REMOVE;	/* with the focus, the system's messages kept */
	device.hwndTarget = NULL;
	if (!RegisterRawInputDevices (&device, 1, sizeof (device))) {
		HL_ERROR ("pointer: no raw input (error %lu)", GetLastError ());
		return;
	}
	enabled = on;
	if (!on) {
		std::lock_guard<std::mutex> guard (lock);
		for (unsigned i = 0; i < MOST; i++)
			if (!injected[i])
				states[i].buttons = 0;
	}
}

Pointers::State
Pointers::get (unsigned index)
{
	if (index >= MOST)
		return State ();
	std::lock_guard<std::mutex> guard (lock);
	return states[index];
}

#else /* !_WIN32 */

void
Pointers::enable (bool)
{
}

std::vector<std::string>
Pointers::names ()
{
	return { "Mouse" };
}

void
Pointers::init (SDL_Window *window)
{
	the_window = window;
}

Pointers::State
Pointers::get (unsigned index)
{
	State s = State ();
	int x = 0, y = 0, cw = 1, ch = 1;
	Uint32 pressed;

	if (index >= MOST)
		return s;
	{
		std::lock_guard<std::mutex> guard (lock);
		if (injected[index])
			return states[index];
	}
	if (index != 0 || !the_window)
		return s;
	pressed = SDL_GetMouseState (&x, &y);
	SDL_GetWindowSize (the_window, &cw, &ch);
	double w = std::min ((double) cw, ch * 4.0 / 3.0), h = std::min ((double) ch, cw * 3.0 / 4.0);
	s.x = std::clamp ((x - (cw - w) * 0.5) / w * 2.0 - 1.0, -1.0, 1.0);
	s.y = std::clamp ((y - (ch - h) * 0.5) / h * 2.0 - 1.0, -1.0, 1.0);
	if (pressed & SDL_BUTTON_LMASK)
		s.buttons |= 1u << LEFT;
	if (pressed & SDL_BUTTON_RMASK)
		s.buttons |= 1u << RIGHT;
	if (pressed & SDL_BUTTON_MMASK)
		s.buttons |= 1u << MIDDLE;
	if (pressed & SDL_BUTTON_X1MASK)
		s.buttons |= 1u << X1;
	if (pressed & SDL_BUTTON_X2MASK)
		s.buttons |= 1u << X2;
	return s;
}

#endif /* _WIN32 */

} /* namespace hl */
