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

#ifndef __HL_POINTERS_H__
#define __HL_POINTERS_H__

#include <string>
#include <vector>

struct SDL_Window;

namespace hl {

/* The mice and the light guns, each its own: on Windows from Raw Input, which
 * names the device behind each packet, numbered by their names so that the
 * same ones keep their numbers; elsewhere the system's one pointer, number 0.
 * A mouse moves its pointer from where it was, within the picture; a light
 * gun, a device that sends absolute positions, aims at a place of the screen
 * and may be off the picture. x and y go from -1 (left, top) to 1 over the
 * game's 4:3 picture, in the middle of the window. */
class Pointers {
public:
	enum { MOST = 4, BUTTONS = 5 };
	enum Button { LEFT, RIGHT, MIDDLE, X1, X2 };

	struct State {
		double	 x, y;
		bool	 off_screen;
		unsigned buttons;	/* 1 << Button */
	};

	/* For the window; they are read once enabled */
	static void	 init (SDL_Window *window);
	/* In a game, and on their page of the settings: their packets, a
	 * thousand a second, delay the system's pointer in the menus */
	static void	 enable (bool on);
	static State	 get (unsigned index);
	/* Their names, by number: the product's, as the device tells it */
	static std::vector<std::string> names ();
	/* Set by hand: the agent's, the tests' */
	static void	 inject (unsigned index, const State &state);
};

} /* namespace hl */

#endif /* __HL_POINTERS_H__ */
