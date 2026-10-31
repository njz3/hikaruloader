/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/main.c, Copyright (C) 2011, Stefano Teso.
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

#ifndef __HL_EMULATOR_H__
#define __HL_EMULATOR_H__

#include <map>
#include <string>

#include "hl/core.h"
#include "hl/games.h"
#include "hl/machine.h"

namespace hl {

class Shell;
class Agent;
class Audio;

/* The emulator: the game list, the game and the machine running it, and
 * the main loop, with its pause and frame rate limiter; and what steps it:
 * the Lua shell, the agent. */
class Emulator {
public:
	/* What stopped a frame */
	enum Stop {
		STOP_NONE,
		/* A CPU reached one of its breakpoints */
		STOP_BREAK,
		/* HL_ABORT, recovered (the shell is open): the machine stays as
		 * the abort left it */
		STOP_ABORT
	};

	Emulator ();
	/* Deletes the agent, the shell, the machine, then the game and the
	 * game list */
	~Emulator ();

	Emulator (const Emulator &) = delete;
	Emulator &operator= (const Emulator &) = delete;

	/* data/games.json (Valkyrie's vk-games.json), looked for beside the
	 * program (base) first */
	bool	 load_game_list (const char *base);

	/* Loads the game named name from rom_path, or the one the picker
	 * returns when name is empty; back to the picker when it cannot be
	 * loaded. False when the player quits, or the named game fails. */
	bool	 load_game (const char *rom_path, const char *name, const char *log_path);
	/* Frees the machine and its game, for the picker's next one */
	void	 unload_game ();

	/* <game>.hlstate (hl::Paths: in the write directory's <game>/) */
	int	 load_or_save_state (bool load);
	/* Loading a state clears an abort */
	int	 load_state (const char *path);
	int	 save_state (const char *path);
	/* Clears an abort too */
	void	 reset (ResetType type);

	/* Opens the Lua shell, listening on 127.0.0.1:port (not at all when
	 * 0); with a port, aborts stop the machine, not the program */
	bool	 open_shell (unsigned port);
	/* The agent on stdin and stdout, paused: frames run on its commands;
	 * aborts stop the machine, not the program. Needs the shell */
	void	 open_agent ();

	/* Runs until the player quits, or for num_frames when > 0 */
	void	 run (int num_frames);
	/* One frame of the machine, drawn and presented; a breakpoint or a
	 * recovered abort pauses the emulator, stop_message says which */
	Stop	 run_frame ();
	/* Runs frames now, paused or not, from the breakpoints the CPUs
	 * stopped at; stops after the frame where one reaches a breakpoint or
	 * aborts. Returns the frames run, or -1 when the machine has aborted */
	int	 step (int frames);
	/* Unpauses, from the breakpoints the CPUs stopped at; false when the
	 * machine has aborted */
	bool	 resume ();
	/* An abort recovered outside a frame, in a shell command */
	void	 aborted (const char *what);

	/* Saves a state every `every` frames (never when 0) in dir, as
	 * <game>-<frame>.hlstate, keeping the last `keep` ones */
	void	 set_snapshots (unsigned every, unsigned keep, const char *dir);
	/* Goes to frame target: from the last snapshot before it (or from a
	 * hard reset), running the frames up to it. False when a breakpoint
	 * or an abort stopped it on the way */
	bool	 seek (uint64_t target);

	bool		 paused;
	/* The machine aborted: it runs again once reset, or a state loaded */
	bool		 crashed;
	/* emu.quit () */
	bool		 quit;
	/* The game came from the picker; Escape left it for the picker */
	bool		 from_picker;
	bool		 back_to_picker;
	/* In run_frame (), its hooks included; and the machine running in it,
	 * when the shell's commands that run the machine cannot */
	bool		 in_frame;
	bool		 in_machine;
	/* What stopped the last frame, and how: "break: ..." or "abort: ..." */
	Stop		 last_stop;
	char		 stop_message[1024];
	/* The snapshots: their frames, and files */
	unsigned	 snapshot_every;
	unsigned	 snapshot_keep;
	std::string	 snapshot_dir;
	std::map<uint64_t, std::string> snapshots;
	GameList	*game_list;
	Game		*game;
	Machine		*mach;
	Shell		*shell;
	Agent		*agent;
	/* The machine's sound, played after each frame */
	Audio		*audio;

private:
	/* True when the player quits */
	bool	 process_events ();
	void	 limit_frame_rate ();
	Machine	*create_machine ();
	void	 take_snapshot ();

	Uint64	 next_frame;
	char	 rom_name[64];
	/* The settings' CPU core the machine's CPUs were told of; -1: not
	 * yet (a new machine) */
	int	 cpu_core = -1;
	/* Likewise, the settings' memory access */
	int	 memory_access = -1;
};

} /* namespace hl */

#endif /* __HL_EMULATOR_H__ */
