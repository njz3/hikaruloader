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

// The games' pictures in the picker, by MAME set name, the parent's for a
// clone without its own:
// - the artwork folder beside the program (or the write directory's), the
//   user's own: <set>.png or .jpg, or MAME's packs, titles/, snap/, flyers/
//   and marquees/ with <set>.png or .jpg in them;
// - when the Picker tab's setting asks (off by default), the Arcade Database's
//   (adb.arcadeitalia.net, by motoschifo), downloaded in the background into
//   cache/artwork/, one game and one connection at a time, as its service
//   asks; a game it does not know is not asked again.
// The download is Windows' (WinINet); elsewhere, the artwork folder alone.

#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace ui
{
struct ArtworkImage
{
    ImTextureID texture = 0;
    int width = 0;
    int height = 0;
};

// The sets the download may fetch, the games found in the games folders: it
// starts once, when the setting asks
void artworkStart(const std::vector<std::string> &sets);
// Each frame, on the main thread: the pictures downloaded since taken in
void artworkPoll();
// The set's pictures: how many (0: none), and the index-th, its texture made
// at the first call (a few a frame); false when not ready yet
int artworkCount(const char *set, const char *parent);
bool artworkGet(const char *set, const char *parent, int index, ArtworkImage *image);
// The download stopped, the textures freed
void artworkShutdown();
} // namespace ui
