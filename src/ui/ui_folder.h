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

// The system's dialog to choose a folder (the games folders of the Picker
// tab): Windows' own; none elsewhere.

#pragma once

#include <string>

namespace ui
{
// Whether this system has the dialog
bool canPickFolder();
// Asks for a folder, the dialog opening in start when given (a path, or
// nullptr); false when cancelled or without the dialog. The path comes in the
// system's code page, as fopen () takes it.
bool pickFolder(const char *title, const char *start, std::string &folder);
} // namespace ui
