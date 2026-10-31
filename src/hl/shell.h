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

#ifndef __HL_SHELL_H__
#define __HL_SHELL_H__

#include <map>
#include <string>

#include "hl/core.h"
#include "hl/machine.h"
#include "hl/server.h"

struct lua_State;

namespace hl {

class Emulator;

/* The Lua shell: a Lua 5.5 interpreter where the emulator (emu), the machine's
 * CPUs, memories and devices (machine) are objects, to read and write the
 * emulated memory and registers, set breakpoints and step the machine; and
 * where hooks (emu.hook) run at the start and the end of the emulation, of
 * each frame, and on the machine's interrupts. Its clients connect over TCP,
 * to 127.0.0.1 only; one line is one command, or the part of one, and its
 * output ends with the prompt "> " (">> " when the command goes on). */
class Shell : public Hooks {
public:
	/* What the hooks run on */
	enum Event {
		/* Once, before the first frame, and once the emulation ends */
		EVENT_START,
		EVENT_EXIT,
		/* Before and after each frame: its number */
		EVENT_FRAME_BEGIN,
		EVENT_FRAME_END,
		/* What stopped the machine */
		EVENT_BREAK,
		EVENT_ABORT,
		/* A device's interrupt request (from, cpu, line, raised), and an
		 * interrupt a CPU takes (cpu, line, handler) */
		EVENT_IRQ,
		EVENT_INTERRUPT,
		/* A CPU writes a watched address (cpu, addr, size, value, pc) */
		EVENT_WRITE,

		NUM_EVENTS
	};

	explicit Shell (Emulator *emu);
	~Shell ();

	Shell (const Shell &) = delete;
	Shell &operator= (const Shell &) = delete;

	bool	 listen (unsigned port);
	/* Runs the commands the clients sent; the main loop calls it between
	 * two frames */
	void	 poll ();
	/* Tells every client, e.g. that a breakpoint stopped the machine; the
	 * line starts with "*** " */
	void	 notify (const char *text);
	/* Runs a Lua file; its output goes to the log */
	bool	 run_file (const char *path);
	/* Runs a Lua chunk, and returns what it printed and returned */
	std::string run (const char *chunk);

	/* The hooks: Lua functions in the registry (ref) */
	int	 add_hook (Event event, int ref);
	bool	 remove_hook (int id);
	/* Calls the hooks on event; push pushes their arguments, and returns
	 * how many */
	void	 call_hooks (Event event, const std::function<int (lua_State *)> &push = nullptr);
	/* With one argument: a frame's number, a message */
	void	 hook_number (Event event, uint64_t number);
	void	 hook_text (Event event, const char *text);
	void	 irq (Device *from, Cpu *cpu, const char *line, bool raised) override;
	void	 interrupt (Cpu *cpu, const char *line, uint32_t handler) override;
	void	 write (Cpu *cpu, uint32_t addr, unsigned size, uint64_t val, uint32_t pc) override;

	Emulator	*emu;
	/* What print () wrote during the command */
	std::string	 output;

private:
	struct Hook {
		int	 id;
		Event	 event;
		int	 ref;
	};

	/* True when chunk is not complete yet */
	bool	 evaluate (const std::string &chunk);
	void	 run_line (int client, const char *line);

	lua_State			*L;
	Server				 server;
	/* The commands the clients have begun */
	std::map<int, std::string>	 pending;
	/* A command (or a script) runs: what it prints goes to its client */
	int				 depth;
	std::vector<Hook>		 hooks;
	unsigned			 num_hooks[NUM_EVENTS];
	int				 next_hook;
};

} /* namespace hl */

#endif /* __HL_SHELL_H__ */
