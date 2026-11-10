/*
 * Hikaruloader
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

/* WIDESCREEN: what a game leaves out of view is never sent to the GPU, so the
 * game's own culling has to see the 854 pixels' width. It is computed on the
 * SH-4, from the screen's width, in two ways the machines' games share; both
 * are recognised here from the instructions and the values they work on,
 * wherever the code is, whichever the game:
 *
 * - a view plane normalised: the distance from the centre to a side, an
 *   integer, goes through FPUL to a float, which is squared and added the
 *   focal length's square (lds Rn,FPUL; float FPUL,FRa; fmul; fmac). The
 *   Hikaru games' library and SWRACER. A side 320 pixels from the centre is
 *   brought to 427, as the renderer widens a viewport as wide as the screen.
 *
 * - a view cone's slope: the width, a float read from a table, over twice the
 *   focal length, stored (fmov.s @Rm,FRn; fldi1; fadd; fmul; fdiv; fmov.s).
 *   The Gaelco games' slaves. A width of 640 becomes 854.
 *
 * The heights go through the same code: the values tell them apart. */

#include "cpu/sh/sh4-wide-view.h"
#include "cpu/sh/sh4.h"
#include "hl/config.h"

/* The instructions looked at around one of them, each way */
#define WINDOW 24

static uint16_t
code_at (hl::Cpu *cpu, uint32_t addr)
{
	uint16_t word = 0;

	cpu->read (2, addr, &word);
	return word;
}

/* rts, rte, sleep; jmp, jsr, braf, bsrf; bra, bsr; bt, bf and theirs with a
 * delay slot */
static bool
ends_flow (uint16_t w)
{
	return w == 0x000B || w == 0x002B || w == 0x001B ||
	       (w & 0xF0FF) == 0x402B || (w & 0xF0FF) == 0x400B ||
	       (w & 0xF0FF) == 0x0023 || (w & 0xF0FF) == 0x0003 ||
	       (w >> 12) == 0xA || (w >> 12) == 0xB ||
	       (w & 0xFF00) == 0x8900 || (w & 0xFF00) == 0x8B00 ||
	       (w & 0xFF00) == 0x8D00 || (w & 0xFF00) == 0x8F00;
}

static inline unsigned frn (uint16_t w) { return (w >> 8) & 15; }
static inline unsigned frm (uint16_t w) { return (w >> 4) & 15; }

static inline bool is_lds_fpul (uint16_t w) { return (w & 0xF0FF) == 0x405A; }
static inline bool is_float (uint16_t w) { return (w & 0xF0FF) == 0xF02D; }
static inline bool is_fmov (uint16_t w) { return (w & 0xF00F) == 0xF00C; }
static inline bool is_fmul (uint16_t w) { return (w & 0xF00F) == 0xF002; }
static inline bool is_fadd (uint16_t w) { return (w & 0xF00F) == 0xF000; }
static inline bool is_fdiv (uint16_t w) { return (w & 0xF00F) == 0xF003; }
static inline bool is_fmac (uint16_t w) { return (w & 0xF00F) == 0xF00E; }
static inline bool is_fldi1 (uint16_t w) { return (w & 0xF0FF) == 0xF09D; }

/* fmov.s from memory: @(R0,Rm), @Rm, @Rm+ */
static inline bool
is_fload (uint16_t w)
{
	return (w & 0xF00F) == 0xF006 || (w & 0xF00F) == 0xF008 || (w & 0xF00F) == 0xF009;
}

/* fmov.s to memory: @(R0,Rn), @Rn, @-Rn; the register stored is the m field */
static inline bool
is_fstore (uint16_t w)
{
	return (w & 0xF00F) == 0xF007 || (w & 0xF00F) == 0xF00A || (w & 0xF00F) == 0xF00B;
}

/* Those that leave a result in FRn */
static bool
writes_frn (uint16_t w)
{
	if ((w >> 12) != 0xF)
		return false;
	switch (w & 15) {
	case 0x0: case 0x1: case 0x2: case 0x3:		/* fadd, fsub, fmul, fdiv */
	case 0x6: case 0x8: case 0x9:			/* fmov.s from memory */
	case 0xC: case 0xE:				/* fmov, fmac */
		return true;
	case 0xD:					/* fsts, float, fneg, fabs,
							 * fsqrt, fsrra, fldi0, fldi1 */
		return frm (w) == 0 || (frm (w) >= 2 && frm (w) <= 9 && frm (w) != 3);
	}
	return false;
}

/* The lds at pc starts a view plane's normalisation: its integer goes to a
 * float, which is squared, and something added to the square */
static bool
is_view_plane_distance (hl::Cpu *cpu, uint32_t pc)
{
	uint32_t live = 0, squared = 0;
	bool converted = false;

	for (unsigned i = 1; i < WINDOW; i++) {
		uint16_t w = code_at (cpu, pc + 2 * i);
		unsigned n = frn (w), m = frm (w);

		if (ends_flow (w))
			return false;
		if (is_lds_fpul (w)) {
			/* Another value in FPUL before ours was taken */
			if (!converted)
				return false;
			continue;
		}
		if (is_float (w) && !converted) {
			converted = true;
			live |= 1u << n;
			continue;
		}
		if (!converted || (w >> 12) != 0xF)
			continue;
		if ((is_fmac (w) || is_fadd (w)) && (squared & (1u << n)))
			return true;
		if (is_fmov (w) && (live & (1u << m))) {
			live |= 1u << n;
			squared &= ~(1u << n);
			continue;
		}
		if (is_fmul (w) && (live & (1u << m)) && (live & (1u << n))) {
			live &= ~(1u << n);
			squared |= 1u << n;
			continue;
		}
		if (writes_frn (w)) {
			live &= ~(1u << n);
			squared &= ~(1u << n);
		}
	}
	return false;
}

/* The last instruction before pc to write FRn, its address in *at; false when
 * there is none in the window, or the flow may have come from elsewhere */
static bool
last_write (hl::Cpu *cpu, uint32_t pc, unsigned n, uint16_t *inst, uint32_t *at)
{
	for (unsigned i = 1; i < WINDOW; i++) {
		uint16_t w = code_at (cpu, pc - 2 * i);

		if (ends_flow (w))
			return false;
		if (writes_frn (w) && frn (w) == n) {
			*inst = w;
			*at = pc - 2 * i;
			return true;
		}
	}
	return false;
}

/* The fdiv at pc makes a view cone's slope: its dividend read from memory, its
 * divisor doubled by a two made there (fldi1, added to itself), the quotient
 * stored */
static bool
is_view_cone_slope (hl::Cpu *cpu, uint32_t pc, uint16_t fdiv)
{
	const unsigned n = frn (fdiv), m = frm (fdiv);
	uint16_t w;
	uint32_t at;
	unsigned two;

	if (n == m || !last_write (cpu, pc, n, &w, &at) || !is_fload (w))
		return false;
	if (!last_write (cpu, pc, m, &w, &at) || !is_fmul (w))
		return false;
	two = frm (w);
	if (two == m || !last_write (cpu, at, two, &w, &at) ||
	    !is_fadd (w) || frm (w) != two)
		return false;
	if (!last_write (cpu, at, two, &w, &at) || !is_fldi1 (w))
		return false;
	for (unsigned i = 1; i <= 5; i++) {
		w = code_at (cpu, pc + 2 * i);
		if (ends_flow (w))
			return false;
		if (is_fstore (w) && frm (w) == n)
			return true;
	}
	return false;
}

static void
log_site (hl::Cpu *cpu, uint32_t pc, const char *what)
{
	static uint32_t seen[16];
	static unsigned num_seen;

	for (unsigned i = 0; i < num_seen; i++)
		if (seen[i] == pc)
			return;
	if (num_seen < sizeof (seen) / sizeof (seen[0])) {
		seen[num_seen++] = pc;
		HL_PRINT ("WIDESCREEN: %s widened at %08X (%s)", what, pc, cpu->name);
	}
}

static uint32_t
wide_view_patch (hl::Cpu *cpu, uint32_t pc, uint32_t inst)
{
	Sh4 *ctx = (Sh4 *) cpu;

	/* Switched in the settings during a game: the game's view widened
	 * again when it computes it next */
	if (!hl::Config::get ()->widescreen)
		return inst;
	if (is_lds_fpul (inst)) {
		uint32_t &distance = ctx->regs.r[frn (inst)];

		if (distance == 320 && is_view_plane_distance (cpu, pc)) {
			distance += (854 - 640) / 2;
			log_site (cpu, pc, "the view's plane");
		}
	} else if (is_fdiv (inst) && !ctx->regs.fpscr.bit.pr) {
		float &width = ctx->regs.f.f[frn (inst) ^ 1].f;

		if (width == 640.0f && is_view_cone_slope (cpu, pc, inst)) {
			width = 854.0f;
			log_site (cpu, pc, "the view's cone");
		}
	}
	return inst;
}

/* The instructions wide_view_patch may change: never translated, so that it
 * sees them run. They are few: most of a game's lds and fdiv do not pass */
static bool
wide_view_match (hl::Cpu *cpu, uint32_t pc, uint32_t inst)
{
	if (is_lds_fpul (inst))
		return is_view_plane_distance (cpu, pc);
	if (is_fdiv (inst))
		return is_view_cone_slope (cpu, pc, inst);
	return false;
}

void
sh4_wide_view_install (hl::Cpu *cpu)
{
	cpu->install_patch (wide_view_patch, wide_view_match);
}
