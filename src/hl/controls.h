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

#ifndef __HL_CONTROLS_H__
#define __HL_CONTROLS_H__

#include <map>
#include <string>
#include <vector>

#include "hl/core.h"
#include "hl/games.h"

union SDL_Event;

namespace hl {

/* The player's keyboard and controllers, bound to the actions in controls.ini,
 * a file of its own beside hikaruloader.ini, in linuxloader's syntax (the
 * other loaders read the same file):
 *
 *	[Common], [Digital], [Driving], [Emulator]
 *		ACTION = BINDING, BINDING + BINDING, ...
 *	KEY_<name>		a key, read while the window has the focus
 *	JOY<n>_BUTTON_<b>, JOY<n>_HAT<h>_<UP|DOWN|LEFT|RIGHT>, JOY<n>_AXIS_<a>[suffix]
 *	GC<n>_BUTTON_<name>, GC<n>_AXIS_<name>[suffix]
 *	MOUSE<n>_X, MOUSE<n>_Y[_INVERTED], MOUSE<n>_BUTTON_<LEFT|RIGHT|MIDDLE|X1|X2>,
 *	MOUSE<n>_OFFSCREEN	a mouse's or a light gun's (hl::Pointers); <n> its
 *				own number, not a player's
 *		SDL game controllers' names: A, B, START, DPUP, LEFTX, RIGHTTRIGGER...
 *	suffixes: _POSITIVE, _NEGATIVE (half an axis as a button),
 *		_POSITIVE_SIDE, _NEGATIVE_SIDE (half an axis as a pedal; _HALF too),
 *		_INVERTED
 *	[Config]		<Action>_DeadZone = 0..32767
 *	[ControllerGUIDs]	P<n>_GUID = an SDL joystick GUID
 *
 * <n> is the player: P1_GUID's device is JOY0 (and GC0 for a game controller),
 * the other devices follow in SDL's order. [Emulator] is ours: the other
 * loaders leave it alone.
 *
 * The hotkeys, RetroArch's way: GC<n>_BUTTON_GUIDE + GC<n>_BUTTON_<name>.
 * While a game controller's Guide is held, its buttons and its hat (JOY<n>_
 * as well) are the hotkeys' alone, the other bindings see them let go; a
 * button pressed meanwhile stays so until let go, Guide let go first. */
class Controls {
public:
	static Controls &get ();

	/* Reads path, written with the defaults when missing (the bindings of
	 * the old [Controls] of hikaruloader.ini carried over); a file without
	 * a newer default line gets it, the user's lines untouched. Saved to
	 * save_to when not NULL: the write directory's (hl::Paths), the file
	 * read beside the program when it is rejected */
	void		 load (const char *path, const char *save_to = NULL);
	const char	*path () const { return file.c_str (); }

	/* The game running (NULL: none) and its own controls (Game::analogs
	 * and switches), shown first in the Controls tab under its names: its
	 * file, controls/<set>.ini beside controls.ini, read instead when
	 * there is one */
	void		 set_game (const char *name, const std::vector<GameInput> &analogs,
		                   const std::vector<GameInput> &switches);
	const char	*game () const { return game_name.c_str (); }
	const std::vector<GameInput> &game_inputs () const { return inputs_of_game; }
	/* That file's path ("" without a game), in use or not */
	std::string	 game_file () const;
	bool		 game_file_in_use () const { return !game_name.empty () && file == game_file (); }
	/* The bindings written to the game's file, used from now on for it */
	bool		 save_for_game ();
	/* The game's file removed, the common bindings back */
	void		 use_common ();

	/* Not in agent mode: the agent alone drives the game */
	void		 set_enabled (bool on) { enabled = on; }
	bool		 is_enabled () const { return enabled || !held.empty (); }

	/* A row held at a value by the agent (its Steering, Accelerator,
	 * Brake, ShiftUp, the game's own controls...), over the bindings: an
	 * axis's, a pedal's, a switch's (pressed from 0.5); release_held gives
	 * them back to the bindings (key NULL: all) */
	void		 hold (const char *section, const char *key, double value);
	void		 release_held (const char *section, const char *key);

	/* The rows of a section, in the file's order */
	struct Row {
		std::string key;
		std::string value;
	};
	std::vector<Row> rows (const char *section) const;
	std::string	 value (const char *section, const char *key) const;
	/* Changes a row and saves the file */
	void		 set_value (const char *section, const char *key, const std::string &value);
	/* The row a new file has, "" for none */
	std::string	 default_value (const char *section, const char *key) const;

	/* The keyboard (player -1), the controllers connected, then the mice
	 * and light guns (player MOUSE_PLAYER + their number) */
	enum { MOUSE_PLAYER = 1000 };
	struct Device {
		int		 player;
		bool		 game_controller;
		std::string	 name;
	};
	std::vector<Device> devices () const;
	/* Player n's SDL_Joystick (from 0), NULL when none is connected */
	void		*player_joystick (int player) const
	{
		return (player >= 0 && player < (int) players.size ()) ? players[player].joystick : NULL;
	}
	/* SDL saw a controller come or go: the players again */
	void		 devices_changed ();

	/* A switch's row: one of its bindings held */
	bool		 pressed (const char *section, const char *key);
	/* Who holds a switch's row: NOBODY, -1 the keyboard, a controller's
	 * player, MOUSE_PLAYER + a mouse's number */
	enum { NOBODY = -2 };
	int		 holder (const char *section, const char *key);
	/* The SDL event presses the row: a key, a button, a hat, half an axis
	 * of one of its bindings, the binding's other inputs held */
	bool		 pressed_by (const char *section, const char *key, const SDL_Event *event);
	/* An axis's row: -1 (left) ... +1 (right), its dead zone applied; its
	 * _Left and _Right rows add the keys' full turns. [Driving] P1_Steer's
	 * joystick axes (a wheel's) go Config::wheel_scale () times further */
	/* The board running: its section ([Naomi]) is looked at first for
	 * Digital, Common and Driving's rows; "" none */
	void		 set_platform (const char *section) { platform = section; }
	double		 axis (const char *section, const char *key);
	/* A sight's row, -1 ... +1: a mouse or a light gun puts it where it
	 * points, a stick and the row's _Left and _Right keys move it, faster
	 * the further the stick goes, and it stays where it is left
	 * ([Config] Aim_DeadZone, Aim_Speed) */
	double		 aim (const char *section, const char *key);
	/* A wheel's axis, -1 ... +1, as the game's wheel: scale times further,
	 * held at its ends */
	static double	 steer (double axis, double scale);
	/* Each event, before the actions it presses are looked for: the
	 * buttons pressed while their controller's Guide is held */
	void		 track (const SDL_Event *event);
	/* A pedal's row: 0 (up) ... 1 (pressed); its _Digital row adds a key */
	double		 pedal (const char *section, const char *key);

	/* One binding now, for the controls page: 0 not there, 1 a switch
	 * (held or not in *on), 2 an axis (-32768..32767 in *raw) */
	int		 input_state (const std::string &token, bool *on, int *raw);

	/* The capture of one input of a device (player, -1 the keyboard):
	 * only a change from the state at its start counts */
	enum CaptureKind {
		CAPTURE_BUTTON,		/* a key, a button, a hat, half an axis */
		CAPTURE_STEERING,	/* an axis turned to the left */
		CAPTURE_PEDAL		/* an axis pressed from its rest */
	};
	void		 capture (int player, CaptureKind kind);
	void		 capture_cancel () { capturing = false; }
	bool		 is_capturing () const { return capturing; }
	/* Each frame while capturing: true and the binding when found */
	bool		 capture_step (std::string &token);
	/* The events while capturing: a key or a button let go within a
	 * frame counts too */
	void		 capture_event (const SDL_Event *event);

	/* "Bouton 3", "Axe 1 - (pédale)", "A + B"... as the page shows them;
	 * tr translates the words; buttons_from_one shows JOY<n>_BUTTON_0 as
	 * "Button 1", as Windows and vJoy number them */
	static std::string describe (const std::string &token,
	                             const char *(*tr) (const char *),
	                             bool buttons_from_one);
	/* A row's default for a device: the defaults name the first joystick,
	 * JOY0; a joystick further in the list, a wheel after a controller,
	 * takes them at its own number */
	static std::string default_for (const std::string &value, int player, bool game_controller);
	/* A binding's alternatives ("A, B + C" gives "A" and "B + C") */
	static std::vector<std::string> alternatives (const std::string &value);
	static std::string join (const std::vector<std::string> &tokens);
	/* The alternative is the device's: its first input is a key for the
	 * keyboard (player -1), JOY<player>_ or GC<player>_ for a controller */
	static bool	 owns (int player, const std::string &token);

	/* One input, and a row's inputs: see controls.cpp */
	struct Source;
	struct Binding;

private:
	Controls ();

	void		 save () const;
	void		 parse_all ();
	const Binding	*binding (const char *section, const char *key);
	/* hotkey: for a binding with Guide in it, which sees the buttons
	 * hidden () from the others */
	bool		 source_pressed (const Source &s, bool hotkey = false);
	bool		 hidden (const Source &s) const;
	bool		 guide_held (int player) const;
	int		 player_of (int instance) const;
	std::string	 platform;
	double		 source_axis (const Source &s);
	double		 source_pedal (const Source &s);
	double		 deadzone (const char *key) const;
	double		 axis_before (const SDL_Event *event, int player);
	void		 open_devices ();
	void		 close_devices ();
	void		 migrate_old_bindings ();

	std::string	 file;
	/* controls.ini's paths, read and written; the game's */
	std::string	 base_read, base_save, game_name;
	std::vector<GameInput> inputs_of_game;
	void		 read (const char *path, const char *save_to);
	/* The sections in the file's order, each its rows in order */
	std::vector<std::pair<std::string, std::vector<Row>>> sections;
	std::map<std::string, Binding *> parsed;
	bool		 enabled;
	/* The agent's held values, by "SECTION/KEY" normalized */
	std::map<std::string, double> held;
	const double	*held_value (const char *section, const char *key) const;

	struct Player {
		int		 instance;	/* SDL's joystick instance id, -1 none */
		void		*joystick;	/* SDL_Joystick * */
		void		*controller;	/* SDL_GameController *, or NULL */
		std::string	 name;
		/* The buttons pressed while Guide was held, not let go since:
		 * by their joystick numbers, and their game controller's */
		uint64_t	 hidden_joy;
		uint32_t	 hidden_gc;
	};
	std::vector<Player> players;

	struct AxisMemo {
		uint32_t	 stamp;
		int		 value;
		double		 before, now;
	};
	std::map<int, AxisMemo> axis_memo;

	bool		 capturing;
	bool		 capture_started;
	int		 capture_player;
	CaptureKind	 capture_kind;
	bool		 capture_mouse (std::string &token);
	/* Each sight's row: where it is, the mouse's last place, the last time */
	struct Aim {
		double		 position = 0.0, mouse = 0.0;
		bool		 mouse_seen = false;
		uint64_t	 last = 0;
	};
	std::map<std::string, Aim> aims;
	/* Each axis's keys and buttons: where they have brought it, the last
	 * time ([Config] Axis_Key_Ramp, Axis_Key_Return) */
	struct KeyAxis {
		double		 value = 0.0;
		uint64_t	 last = 0;
	};
	std::map<std::string, KeyAxis> key_axes;
	/* Where the keys and buttons bring the axis now, toward target (-1, 0
	 * or 1) */
	double		 ramp_keys (const char *section, const char *key, double target);
	unsigned	 mouse_buttons;
	double		 mouse_x, mouse_y;
	std::string	 capture_found;
	std::vector<uint8_t> base_keys;
	std::vector<std::vector<double>> base_axes;
	std::vector<std::vector<uint8_t>> base_buttons;
	std::vector<std::vector<uint8_t>> base_hats;
};

} /* namespace hl */

#endif /* __HL_CONTROLS_H__ */
