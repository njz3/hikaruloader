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
#include <exception>

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
	patch_match (NULL),
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
Machine::register_buffer (Buffer *buf, bool battery, const char *name)
{
	HL_ASSERT (buf);
	buffers.push_back (buf);
	buffer_names.push_back (name ? name : "");
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
Machine::hashed_memories (std::vector<Hashed> &out)
{
	size_t i;

	for (i = 0; i < buffers.size (); i++) {
		std::string name = i < buffer_names.size () ? buffer_names[i] : "";

		for (const auto &m : memories)
			if (name.empty () && m.second == buffers[i])
				name = m.first;
		if (name.empty ())
			name = "buf" + std::to_string (i);
		out.push_back ({ name, buffers[i]->ptr (0), buffers[i]->size () });
	}
	/* The named ones the machine does not own: the CPUs' registers */
	for (const auto &m : memories)
		if (std::find (buffers.begin (), buffers.end (), m.second) == buffers.end ())
			out.push_back ({ m.first, m.second->ptr (0), m.second->size () });
	hashed_extra (out);
}

/* Four lanes of 64 bits (the multiplies in parallel), folded with the size */
static uint64_t
hash_bytes (const void *data, size_t size)
{
	const uint8_t *p = (const uint8_t *) data;
	uint64_t h[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
	size_t i = 0;
	int k;

	for (; i + 32 <= size; i += 32)
		for (k = 0; k < 4; k++) {
			uint64_t w;

			memcpy (&w, p + i + 8 * k, 8);
			h[k] = (h[k] ^ w) * 0x100000001B3ull;
			h[k] ^= h[k] >> 29;
		}
	for (; i < size; i++)
		h[0] = (h[0] ^ p[i]) * 0x100000001B3ull;
	return (h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ size) * 0x9E3779B97F4A7C15ull;
}

void
Machine::write_hashes (FILE *fp, bool blocks)
{
	std::vector<Hashed> mems;

	if (renderer)
		renderer->sync ();
	hashed_memories (mems);
	fprintf (fp, "F %llu", (unsigned long long) frame);
	for (const Hashed &m : mems)
		fprintf (fp, " %s=%016llX", m.name.c_str (), (unsigned long long) hash_bytes (m.data, m.size));
	fputc ('\n', fp);
	if (blocks)
		for (const Hashed &m : mems)
			for (size_t o = 0; o < m.size; o += 0x10000)
				fprintf (fp, "B %llu %s %08zX %016llX\n", (unsigned long long) frame, m.name.c_str (), o,
				         (unsigned long long) hash_bytes ((const uint8_t *) m.data + o,
				                                          std::min<size_t> (0x10000, m.size - o)));
	fflush (fp);
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
	if (renderer)
		renderer->sync ();

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

/* A memory short (the page file cannot grow on a full disk) throws from
 * the buffers: an error, the machine reset when it was loading */
int
Machine::guarded_state (const char *path, State::Mode mode)
{
	try {
		return load_save_state (path, mode);
	} catch (const std::exception &e) {
		HL_ERROR ("%s state: '%s': %s", (mode == State::LOAD) ? "load" : "save", path, e.what ());
		if (mode == State::LOAD) {
			HL_ERROR ("load state: resetting machine");
			reset (RESET_HARD);
		}
		return -1;
	}
}

int
Machine::load_state (const char *path)
{
	return guarded_state (path, State::LOAD);
}

int
Machine::save_state (const char *path)
{
	return guarded_state (path, State::SAVE);
}

} /* namespace hl */
