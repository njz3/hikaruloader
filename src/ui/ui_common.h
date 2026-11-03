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

// Shared pieces of the game picker and of the Tab settings overlay: the ImGui
// context, the style, the translations and the settings window.
//
// Based on the interface of linuxloader, itself a modified work based on
// sm2-emu's (https://github.com/dmanlfc/sm2-emu) by Daniel Martin (dmanlfc)
// and the sm2-emu contributors, GPL-3.0-or-later. Modified for Hikaruloader; the
// About tab says so.

#pragma once

#include <SDL.h>

namespace ui
{
// ImGui context with the SDL2 platform backend and the OpenGL 3 renderer, for
// the current GL context.
bool initContext(SDL_Window *window);
void shutdownContext();
bool processEvent(const SDL_Event *event);
void newFrame();
void renderFrame(int width, int height);

// The text in the interface's language: English by default, French when
// [Picker] LANGUAGE is fr (or auto on a French Windows).
const char *tr(const char *text);
void setLanguage(const char *language);
// A text of a table, given to tr () when shown: marked for tools/po_update.py,
// as gettext's N_
#define N_(text) text

// The settings window. inGame: the settings that need a restart say so.
void drawSettings(bool *open, bool inGame);
// Set when the games folders changed: the picker scans them again, at once
// for a folder added, the field left, Save or Scan again; for one typed in
// with the field still being edited, once the settings are closed.
bool takeRescanRequest(bool settingsOpen);

// The controls page: the devices' bindings in controls.ini (hl::Controls),
// captured from the devices.
void drawControlsTab();
// While a binding is being captured, the devices' events go nowhere else
// (Escape cancels): true when the event is taken.
bool controlsCaptureEvent(const SDL_Event *event);
} // namespace ui
