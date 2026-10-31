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

#include <chrono>

#include "hl/core.h"
#include "hl/profile.h"

namespace hl {

bool Profile::enabled = false;
uint64_t Profile::total[NUM_SECTIONS];
uint64_t Profile::calls[NUM_SECTIONS];
uint64_t Profile::units[NUM_SECTIONS];

/* The time stamp counter's rate, from the wall clock between init and report */
static uint64_t start_ticks;
static std::chrono::steady_clock::time_point start_time;

static const struct {
	const char *name;
	int parent;		/* -1: none */
	const char *unit;	/* of count (), NULL: none */
} sections[Profile::NUM_SECTIONS] = {
	{ "frame",		-1,			NULL },
	{ "machine",		Profile::FRAME,		NULL },
	{ "SH-4 master",	Profile::MACHINE,	NULL },
	{ "SH-4 slave",		Profile::MACHINE,	NULL },
	{ "MEMCTL",		Profile::MACHINE,	NULL },
	{ "GPU: CP",		Profile::MACHINE,	NULL },
	{ "GPU: events",	Profile::MACHINE,	NULL },
	{ "MIE, JVS",		Profile::MACHINE,	NULL },
	{ "AICAs",		Profile::MACHINE,	NULL },
	{ "ARM7s",		Profile::AICA,		"ARM cycle" },
	{ "voices",		Profile::AICA,		"channel-sample" },
	{ "DSPs",		Profile::AICA,		"step" },
	{ "comm board",		Profile::MACHINE,	NULL },
	{ "renderer: begin",	Profile::FRAME,		NULL },
	{ "renderer: draw",	Profile::FRAME,		NULL },
	{ "thread awaited",	Profile::RENDER_END,	NULL },
	{ "frame handed over",	Profile::RENDER_END,	NULL },
	{ "frame shown",	Profile::RENDER_END,	NULL },
	{ "interface, swap",	Profile::RENDER_END,	NULL },
	{ "sound output",	Profile::FRAME,		NULL },
};

void
Profile::init ()
{
	enabled = hl_util_get_bool_option ("HR_PROFILE", false);
	start_ticks = now ();
	start_time = std::chrono::steady_clock::now ();
}

/* A section's own time: its total less its children's */
static uint64_t
own (const uint64_t *total, int s)
{
	uint64_t t = total[s], kids = 0;
	int k;

	for (k = 0; k < Profile::NUM_SECTIONS; k++)
		if (sections[k].parent == s)
			kids += total[k];
	return (t > kids) ? t - kids : 0;
}

static int
depth (int s)
{
	int d = 0;

	while (sections[s].parent >= 0) {
		s = sections[s].parent;
		d++;
	}
	return d;
}

static void
print_tree (const uint64_t *total, const uint64_t *units, int parent, double ms_per_tick, uint64_t frames)
{
	double whole = (double) total[Profile::FRAME];
	int s;

	for (s = 0; s < Profile::NUM_SECTIONS; s++) {
		if (sections[s].parent != parent || !total[s])
			continue;
		HL_PRINT ("  %*s%-*s %8.3f ms/frame %5.1f%%", depth (s) * 2, "",
		         22 - depth (s) * 2, sections[s].name,
		         total[s] * ms_per_tick / (double) frames, 100.0 * total[s] / whole);
		if (sections[s].unit && units[s])
			HL_PRINT ("  %*s%.1f ns a %s, %.0f a frame", depth (s) * 2 + 2, "",
			          total[s] * ms_per_tick * 1e6 / (double) units[s], sections[s].unit,
			          (double) units[s] / (double) frames);
		print_tree (total, units, s, ms_per_tick, frames);
		/* The rest of a parent, when it has children */
		if (own (total, s) != total[s])
			HL_PRINT ("  %*s%-*s %8.3f ms/frame %5.1f%%", depth (s) * 2 + 2, "",
			         20 - depth (s) * 2, "(the rest)",
			         own (total, s) * ms_per_tick / (double) frames, 100.0 * own (total, s) / whole);
	}
}

void
Profile::report (uint64_t frames)
{
	double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - start_time).count ();
	double ticks_per_s = (double) (now () - start_ticks) / seconds;
	double ms_per_tick = 1000.0 / ticks_per_s;

	if (!enabled || !frames || !total[FRAME])
		return;
	HL_PRINT ("profile: %llu frames, %.3f ms each (%.1f a second without the limiter), %.2f GHz counter",
	         (unsigned long long) frames, total[FRAME] * ms_per_tick / (double) frames,
	         1000.0 * (double) frames / (total[FRAME] * ms_per_tick), ticks_per_s / 1e9);
	print_tree (total, units, -1, ms_per_tick, frames);
}

} /* namespace hl */
