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

#ifndef __HL_PROFILE_H__
#define __HL_PROFILE_H__

#include <stdint.h>

#if defined (_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace hl {

/* Where the host's time goes, module by module: with HR_PROFILE=1 in the
 * environment, each section's time is counted (the CPU's time stamp counter,
 * a few nanoseconds a read) and the totals are printed at the end. Without
 * it, a section costs a test. The sections nest: a parent's own time is its
 * total less its children's. */
class Profile {
public:
	enum Section {
		FRAME,		/* all of a frame, the limiter's wait aside */
		MACHINE,	/* the machine's frame */
		SH4_MASTER,
		SH4_SLAVE,
		MEMCTL,
		GPU_CP,		/* the command processor: the meshes built */
		GPU_EVENTS,	/* hblank, vblank: the GPU's IRQs and lists */
		MIE,		/* the Z80, the JVS bus, the I/O board */
		AICA,		/* both, all of them */
		AICA_ARM,	/* their ARM7s */
		AICA_VOICES,	/* the 64 channels */
		AICA_DSP,	/* the effects */
		COMM,		/* the communication board */
		RENDER_BEGIN,	/* the renderer, before the machine's frame */
		RENDER_END,	/* the renderer's frame: the 3D drawn, presented */
		RENDER_WAIT,	/* THREADED_RENDERING: its thread awaited, */
		RENDER_COPY,	/* the frame handed over, */
		RENDER_SHOW,	/* the frame before put in the window */
		PRESENT,	/* the interface drawn, the buffers swapped */
		AUDIO,		/* the host's sound output */
		NUM_SECTIONS
	};

	/* From HR_PROFILE, once */
	static void	 init ();
	static bool	 enabled;

	static inline uint64_t now () { return __rdtsc (); }
	static inline void add (Section s, uint64_t ticks) { total[s] += ticks; calls[s]++; }
	/* The section's units of work (the voices' channel-samples, the DSPs'
	 * steps, the ARMs' cycles), for its time per unit */
	static inline void count (Section s, uint64_t n) { if (enabled) units[s] += n; }

	/* frames: the frames run; to the log */
	static void	 report (uint64_t frames);
	/* The frames' own CPU time on their thread (QueryThreadCycleTime:
	 * without the time other programs took the core, which the counter's
	 * times count), at a frame's start and end */
	static void	 frame_cpu (bool start);

	/* HR_SAMPLE: code written at run time, the samples in it told by the
	 * code it runs (the SH-4s' translations: the CPU, the block's address
	 * and its instructions) */
	static bool	 sampling;
	/* With HR_PROFILE too: the section the machine's thread is in, its
	 * samples told apart (the SH-4s') */
	static volatile int current;
	/* HR_SAMPLE: the calling thread sampled too (the drawing thread's),
	 * reported apart */
	static void	 sample_this_thread ();
	static void	 code_written (const void *start, size_t size, char cpu, uint32_t pc,
			               const uint16_t *words, unsigned num_words);

	/* Times its block */
	class Scope {
	public:
		explicit Scope (Section s) : section (s), start (enabled ? now () : 0)
		{
			if (enabled && s == FRAME)
				frame_cpu (true);
			if (sampling) {
				outer = current;
				current = s;
			}
		}
		~Scope ()
		{
			if (enabled && section == FRAME)
				frame_cpu (false);
			if (enabled)
				add (section, now () - start);
			if (sampling)
				current = outer;
		}
	private:
		Section section;
		uint64_t start;
		int outer = -1;
	};

private:
	static uint64_t total[NUM_SECTIONS];
	static uint64_t calls[NUM_SECTIONS];
	static uint64_t units[NUM_SECTIONS];
};

} /* namespace hl */

#define HL_PROFILE(section_) \
	hl::Profile::Scope hl_profile_scope_ (hl::Profile::section_)

#endif /* __HL_PROFILE_H__ */
