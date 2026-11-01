/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/machine.h, Copyright (C) 2011-2013, Stefano Teso.
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

#ifndef __HL_MACH_H__
#define __HL_MACH_H__

#include <vector>

#include "hl/core.h"
#include "hl/games.h"
#include "hl/renderer.h"
#include "hl/state.h"
#include "hl/buffer.h"
#include "hl/device.h"

namespace hl {

class Cpu;

/* What the Lua shell's hooks hear from inside the machine, while
 * Machine::hooks is set */
class Hooks {
public:
	virtual ~Hooks () {}
	/* from asked cpu for an interrupt on its source line (or withdrew it) */
	virtual void	 irq (Device *from, Cpu *cpu, const char *line, bool raised) = 0;
	/* cpu takes an interrupt from line: its handler is at handler */
	virtual void	 interrupt (Cpu *cpu, const char *line, uint32_t handler) = 0;
	/* cpu's code at pc writes val to a watched address */
	virtual void	 write (Cpu *cpu, uint32_t addr, unsigned size, uint64_t val, uint32_t pc) = 0;
};

/* An emulated machine: its game, its memory buffers, its devices (CPUs
 * among them) and its renderer, all of which it owns. */
class Machine {
public:
	explicit Machine (Game *game);
	virtual ~Machine ();

	Machine (const Machine &) = delete;
	Machine &operator= (const Machine &) = delete;

	/* Zero-filled, as the C code they come from relied on calloc () */
	static void *operator new (size_t size)
	{
		void *ptr = ::operator new (size);
		memset (ptr, 0, size);
		return ptr;
	}
	static void operator delete (void *ptr) { ::operator delete (ptr); }

	/* Called by the buffers', devices' and CPUs' creators; a battery's
	 * buffer keeps its contents through the resets */
	void		 register_buffer (Buffer *buf, bool battery = false);
	void		 register_device (Device *dev);
	void		 register_cpu (Cpu *cpu);
	/* Names a memory for the Lua shell: a registered buffer, or one the
	 * machine does not own, such as a ROM of the game */
	void		 add_memory (const char *name, Buffer *buf);

	/* The CPU the next memory accesses come from, when the debugger
	 * reads or runs one */
	virtual void	 set_current_cpu (Cpu *cpu) { (void) cpu; }

	/* Clears the buffers (the batteries' aside), resets the devices, the
	 * machine, the renderer */
	void		 reset (ResetType type);
	virtual int	 run_frame () = 0;
	int		 load_state (const char *path);
	int		 save_state (const char *path);
	/* For the log */
	virtual const char *debug_string () = 0;

	char		 name[64];
	/* The frames run since the last hard reset: the emulator counts them,
	 * the states keep them */
	uint64_t	 frame;
	Game		*game;
	Renderer	*renderer;
	/* The sound of the frame run: 44.1 kHz, audio_channels a sample (the
	 * front left and right, then the rear's); the emulator plays it,
	 * then clears it */
	std::vector<int16_t> audio;
	unsigned	 audio_channels = 2;
	/* NULL but when the shell has hooks on interrupts */
	Hooks		*hooks;
	std::vector<Buffer *> buffers;
	/* Those of buffers a reset leaves */
	std::vector<Buffer *> batteries;
	std::vector<Device *> devices;
	std::vector<Cpu *> cpus;
	std::vector<std::pair<const char *, Buffer *>> memories;

protected:
	virtual void	 on_reset (ResetType type) { (void) type; }
	virtual int	 on_load_state (State *state) { (void) state; return 0; }
	virtual int	 on_save_state (State *state) { (void) state; return 0; }

private:
	int		 load_save_state (const char *path, State::Mode mode);
};

} /* namespace hl */

/* The machine's state at the head of the line: made only when the log is
 * (Hikaru's, two strdup and a sprintf, at each access of a region the
 * memory map logs otherwise: the GPU's, the MIE's) */
#ifdef NDEBUG
#define HL_MACH_LOG(mach_, fmt_, ...) \
	do { } while (0);
#else
#define HL_MACH_LOG(mach_, fmt_, ...) \
	do { \
		if (hl_verbosity > 0) { \
			const char *str = (mach_)->debug_string (); \
			HL_LOG ("%s : " fmt_, str, ##__VA_ARGS__); \
		} \
	} while (0);
#endif

#define HL_MACH_ERROR(mach_, fmt_, ...) \
	do { \
		const char *str = (mach_)->debug_string (); \
		HL_ERROR ("%s : " fmt_, str, ##__VA_ARGS__); \
	} while (0);

#define HL_MACH_ABORT(mach_, fmt_, ...) \
	do { \
		const char *str = (mach_)->debug_string (); \
		HL_ABORT ("%s : " fmt_, str, ##__VA_ARGS__); \
	} while (0);

#define HL_MACH_ASSERT(mach_, cond_) \
	HL_ASSERT (cond_)

#endif /* __HL_MACH_H__ */
