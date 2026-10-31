/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/device.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_DEVICE_H__
#define __HL_DEVICE_H__

#include "hl/core.h"
#include "hl/state.h"

namespace hl {

class Machine;

enum ResetType {
	RESET_HARD,
	RESET_SOFT,

	NUM_RESET_TYPES
};

/* A component of a machine: a CPU, a chip, a block of registers mapped in a
 * CPU's address space. The machine it is created for keeps it, resets it,
 * saves and loads its state, and deletes it. */
class Device {
public:
	explicit Device (Machine *mach);
	virtual ~Device () {}

	Device (const Device &) = delete;
	Device &operator= (const Device &) = delete;

	/* Zero-filled, as the C code they come from relied on calloc () */
	static void *operator new (size_t size)
	{
		void *ptr = ::operator new (size);
		memset (ptr, 0, size);
		return ptr;
	}
	static void operator delete (void *ptr) { ::operator delete (ptr); }

	virtual void	reset (ResetType type) { (void) type; }
	/* Runs for some cycles, returns the cycles used: all of them, by
	 * default */
	virtual int	exec (int cycles) { return cycles; }
	/* Register access, through the memory maps; -1 when unhandled */
	virtual int	get (unsigned size, uint32_t addr, void *val);
	virtual int	put (unsigned size, uint32_t addr, uint64_t val);
	virtual int	save_state (State *state) { (void) state; return 0; }
	virtual int	load_state (State *state) { (void) state; return 0; }

	/* Its state in a line, for the log and the Lua shell; NULL when it
	 * has none to tell */
	virtual const char *debug_string () { return NULL; }
	/* A debugging option, e.g. the GPU's "log_cp"; false when the device
	 * has none of that name */
	virtual bool	set_option (const char *name, int value) { (void) name; (void) value; return false; }

	Machine *mach;
	/* Its name in the Lua shell, e.g. "gpu", given by the machine; NULL
	 * when it has none */
	const char *name;
};

} /* namespace hl */

#endif /* __HL_DEVICE_H__ */
