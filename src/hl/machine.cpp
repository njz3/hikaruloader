/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/machine.c, Copyright (C) 2011-2013, Stefano Teso.
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

#include <algorithm>

#include "hl/machine.h"
#include "hl/state.h"
#include "hl/buffer.h"
#include "hl/device.h"
#include "hl/cpu.h"

namespace hl {

Device::Device (Machine *mach) :
	mach (mach),
	name (NULL)
{
	HL_ASSERT (mach);
	mach->register_device (this);
}

int
Device::get (unsigned size, uint32_t addr, void *val)
{
	(void) size;
	(void) addr;
	(void) val;
	return -1;
}

int
Device::put (unsigned size, uint32_t addr, uint64_t val)
{
	(void) size;
	(void) addr;
	(void) val;
	return -1;
}

Cpu::Cpu (Machine *mach, Mmap *mmap) :
	Device (mach),
	mmap (mmap),
	state (CPU_STATE_RUN),
	remaining (0),
	profile (NULL),
	addr_mask (0xFFFFFFFF),
	break_hit (false),
	break_pc (0),
	patch_fn (NULL),
	break_skip (false),
	skip_pc (0)
{
	HL_ASSERT (mmap);
	mach->register_cpu (this);
}

Machine::Machine (Game *game) :
	frame (0),
	game (game),
	renderer (NULL),
	hooks (NULL)
{
	name[0] = '\0';
}

Machine::~Machine ()
{
	destroy (renderer);
	for (Buffer *buf : buffers)
		delete buf;
	for (Device *dev : devices)
		delete dev;
}

void
Machine::register_buffer (Buffer *buf, bool battery)
{
	HL_ASSERT (buf);
	buffers.push_back (buf);
	if (battery)
		batteries.push_back (buf);
	HL_LOG ("machine: registered buffer %p, size=%08X", (void *) buf, buf->size ());
}

void
Machine::register_device (Device *dev)
{
	HL_ASSERT (dev);
	devices.push_back (dev);
	HL_LOG ("machine: registered device %p", (void *) dev);
}

void
Machine::register_cpu (Cpu *cpu)
{
	HL_ASSERT (cpu);
	cpus.push_back (cpu);
	HL_LOG ("machine: registered cpu %p", (void *) cpu);
}

void
Machine::add_memory (const char *name, Buffer *buf)
{
	if (buf)
		memories.push_back (std::make_pair (name, buf));
}

void
Machine::reset (ResetType type)
{
	HL_LOG ("resetting machine %p", (void *) this);

	for (Buffer *buf : buffers) {
		if (std::find (batteries.begin (), batteries.end (), buf) != batteries.end ())
			continue;
		HL_LOG ("resetting buf %p", (void *) buf);
		buf->clear ();
	}

	for (Device *dev : devices) {
		HL_LOG ("resetting dev %p", (void *) dev);
		dev->reset (type);
	}

	on_reset (type);
	if (renderer)
		renderer->reset ();
	if (type == RESET_HARD)
		frame = 0;
}

int
Machine::load_save_state (const char *path, State::Mode mode)
{
	const char *op;
	bool changed = false;
	int ret = 0;

	op = (mode == State::LOAD) ? "load" : "save";

	{
		State state (path, mode);

		/* A file that cannot be read leaves the machine as it is */
		if (!state.ok ()) {
			HL_ERROR ("%s state: cannot %s '%s'", op, (mode == State::LOAD) ? "read" : "write", path);
			return -1;
		}

		if (mode == State::LOAD) {
			reset (RESET_HARD);
			changed = true;
		}

		/* The frame it was saved at, to go on from */
		ret = (mode == State::LOAD) ? state.get (&frame, sizeof (frame)) :
		                              state.put (&frame, sizeof (frame));
		if (ret)
			goto done;

		for (Buffer *buf : buffers) {
			ret = (mode == State::LOAD) ?
			      buf->load_state (&state) :
			      buf->save_state (&state);
			if (ret) {
				HL_ERROR ("%s state: cannot %s buffer", op, op);
				goto done;
			}
		}

		for (Device *dev : devices) {
			ret = (mode == State::LOAD) ?
			      dev->load_state (&state) :
			      dev->save_state (&state);
			if (ret) {
				HL_ERROR ("%s state: cannot %s device", op, op);
				goto done;
			}
		}

		ret = (mode == State::LOAD) ? on_load_state (&state) :
		                              on_save_state (&state);
		if (ret) {
			HL_ERROR ("%s state; cannot %s machine", op, op);
			goto done;
		}

		/* Compressed and written; or all of it read */
		ret = state.finish ();
		if (ret)
			HL_ERROR ("%s state: cannot %s '%s'", op, (mode == State::LOAD) ? "use" : "write", path);
	}
done:
	if (ret && changed) {
		HL_ERROR ("load state: resetting machine");
		reset (RESET_HARD);
	}
	return ret;
}

int
Machine::load_state (const char *path)
{
	return load_save_state (path, State::LOAD);
}

int
Machine::save_state (const char *path)
{
	return load_save_state (path, State::SAVE);
}

} /* namespace hl */
