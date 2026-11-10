/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/cpu.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_CPU_H__
#define __HL_CPU_H__

#include <unordered_map>
#include <vector>

#include "hl/mmap.h"
#include "hl/device.h"
#include "hl/machine.h"

namespace hl {

enum CpuState {
	CPU_STATE_RUN,
	CPU_STATE_STOP,
	CPU_STATE_SLEEP,
	CPU_STATE_STANDBY,

	NUM_CPU_STATES
};

enum IrqState {
	IRQ_STATE_CLEAR,
	IRQ_STATE_RAISED,

	NUM_IRQ_STATES
};

class Cpu;

/* Changes the instruction fetched at pc */
typedef uint32_t (* CpuPatch) (Cpu *cpu, uint32_t pc, uint32_t inst);
typedef bool (* CpuPatchMatch) (Cpu *cpu, uint32_t pc, uint32_t inst);

/* A processor: a device that runs code, reading and writing memory through
 * its own memory map. */
class Cpu : public Device {
public:
	/* Registers itself in mach as a device and as a CPU */
	Cpu (Machine *mach, Mmap *mmap);
	~Cpu () { delete profile; }

	virtual int		 run (int cycles) = 0;
	virtual void		 set_state (CpuState state) = 0;
	/* from is the device that asks, for the hooks (Machine::hooks); NULL
	 * when it is the CPU's own */
	virtual int		 set_irq_state (unsigned num, IrqState state, Device *from = NULL) = 0;
	/* The name of the interrupt source num, e.g. "IRL2" */
	virtual const char	*irq_name (unsigned num) const { (void) num; return "?"; }
	/* "M @PC @PR SR", for the log */
	virtual const char	*debug_string () = 0;

	/* Memory access through the CPU's map */
	int read (unsigned size, uint32_t addr, void *val)
	{
		return mmap->get (size, addr, val);
	}
	int write (unsigned size, uint32_t addr, uint64_t val)
	{
		return mmap->put (size, addr, val);
	}

	/* match says which instructions the patch looks at, from their code:
	 * the others need not call it (the SH-4's blocks, as they are decoded;
	 * the interpreter calls it for all) */
	void install_patch (CpuPatch patch, CpuPatchMatch match)
	{
		HL_ASSERT (patch != NULL && match != NULL);
		patch_fn = patch;
		patch_match = match;
	}
	uint32_t patch (uint32_t pc, uint32_t inst)
	{
		return patch_fn ? patch_fn (this, pc, inst) : inst;
	}
	bool patches (uint32_t pc, uint32_t inst)
	{
		return patch_fn && patch_match (this, pc, inst);
	}

	/* The debugger's view of the registers: their names, in the order the
	 * Lua shell lists them, and their values */
	virtual unsigned	 num_registers () const { return 0; }
	virtual const char	*register_name (unsigned index) const { (void) index; return NULL; }
	virtual uint64_t	 get_register (unsigned index) { (void) index; return 0; }
	virtual void		 set_register (unsigned index, uint64_t val) { (void) index; (void) val; }

	/* Memory access as the CPU's code sees it, on-chip registers included,
	 * for the debugger: the map alone, by default */
	virtual int debug_read (unsigned size, uint32_t addr, void *val)
	{
		return read (size, addr, val);
	}
	virtual int debug_write (unsigned size, uint32_t addr, uint64_t val)
	{
		return write (size, addr, val);
	}

	/* Called by run () before each instruction while there are
	 * breakpoints: true when the CPU must stop at pc */
	bool at_breakpoint (uint32_t pc)
	{
		if (break_skip) {
			break_skip = false;
			if (pc == skip_pc)
				return false;
		}
		for (uint32_t bp : breakpoints)
			if (bp == (pc & addr_mask)) {
				break_hit = true;
				break_pc = pc;
				return true;
			}
		return false;
	}
	/* Leaves the breakpoint the CPU stopped at: its instruction runs
	 * next */
	void resume ()
	{
		if (break_hit) {
			break_hit = false;
			break_skip = true;
			skip_pc = break_pc;
		}
	}

	Mmap		*mmap;
	CpuState	 state;
	int		 remaining;

	/* The debugger's profile, while not NULL: the instructions run at each
	 * address (through addr_mask) */
	std::unordered_map<uint32_t, uint64_t> *profile;

	/* The debugger's watchpoints: the writes to these addresses (through
	 * addr_mask) go to the machine's hooks */
	std::vector<uint32_t> watchpoints;
	bool watched (uint32_t addr, unsigned size) const
	{
		for (uint32_t wp : watchpoints)
			if (wp - (addr & addr_mask) < size)
				return true;
		return false;
	}

	/* The debugger's breakpoints, compared through addr_mask (the SH-4's
	 * top three bits only select a region of the same memory) */
	std::vector<uint32_t> breakpoints;
	uint32_t	 addr_mask;
	/* run () stopped before the instruction at break_pc, and does nothing
	 * until resume () */
	bool		 break_hit;
	uint32_t	 break_pc;

private:
	CpuPatch	 patch_fn;
	CpuPatchMatch	 patch_match;
	bool		 break_skip;
	uint32_t	 skip_pc;
};

} /* namespace hl */

/* The CPU's state at the head of the line: made only when the log is (its
 * debug_string () a sprintf, at each on-chip register's access otherwise) */
#ifdef NDEBUG
#define HL_CPU_LOG(cpu_, fmt_, ...) \
	do { } while (0);
#else
#define HL_CPU_LOG(cpu_, fmt_, ...) \
	do { \
		if (hl_verbosity > 0) { \
			const char *str = (cpu_)->debug_string (); \
			HL_LOG ("%s : " fmt_, str, ##__VA_ARGS__); \
		} \
	} while (0);
#endif

#define HL_CPU_ERROR(cpu_, fmt_, ...) \
	do { \
		const char *str = (cpu_)->debug_string (); \
		HL_ERROR ("%s : " fmt_, str, ##__VA_ARGS__); \
	} while (0);

#define HL_CPU_ABORT(cpu_, fmt_, ...) \
	do { \
		const char *str = (cpu_)->debug_string (); \
		HL_ABORT ("%s : " fmt_, str, ##__VA_ARGS__); \
	} while (0);

#define HL_CPU_ASSERT(cpu_, cond_) \
	do { \
		if (!(cond_)) { \
			const char *str = (cpu_)->debug_string (); \
			HL_ABORT ("%s : assertion failed, aborting", str); \
		} \
	} while (0);

#endif /* __HL_CPU_H__ */
