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

#ifndef __HL_AGENT_H__
#define __HL_AGENT_H__

#include <memory>
#include <string>

#include "hl/core.h"

namespace hl {

class Emulator;

/* The agent mode: a program drives the emulator through stdin and stdout,
 * one command a line and one answer each, frames running only on its "step".
 * The commands Supermodel's and sm2-emu's agent modes share, with the same
 * syntax and answers, so that one client drives the three: ready, step, set,
 * setraw, get, release, inputs, obs, shot, save, load, ffb, servo, settle,
 * frame, ram, mem, tel, quit; Supermodel's poke; and lua, a command of the
 * Lua shell. See README.md. */
class Agent {
public:
	explicit Agent (Emulator *emu);
	~Agent ();

	Agent (const Agent &) = delete;
	Agent &operator= (const Agent &) = delete;

	/* Reads stdin from now on, and says "ready GAME W H" */
	void	 start ();
	/* Runs the commands received, waiting up to timeout_ms for the first;
	 * true when there was one */
	bool	 poll (unsigned timeout_ms);

	struct Queue;

private:
	void	 run (const std::string &line);
	void	 reply (const char *fmt, ...);
	/* A header line, then size raw bytes */
	void	 reply_data (const char *header, const void *data, size_t size);
	void	 step (const char *args);
	void	 obs (const char *args);
	void	 read_memory (const char *name, const char *args);
	void	 lua (const char *chunk);

	Emulator			*emu;
	/* Shared with the thread reading stdin, which may outlive the agent */
	std::shared_ptr<Queue>		 queue;
	/* Waited before each frame */
	unsigned			 settle_ms;
};

} /* namespace hl */

#endif /* __HL_AGENT_H__ */
