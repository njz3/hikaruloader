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

/* The AICA's arithmetic (hikaru-aica-math.h): the DSP's ring buffer floats,
 * exact up to 11 bits and their sign (small negatives once came back about
 * 2048 too low), close and in order above; the ADPCM decoder, symmetric,
 * its value in 16 bits and its step in 7F-6000. Run by ctest, or alone:
 * aicatest. */

#include <stdio.h>
#include <stdlib.h>

#include "mach/hikaru/hikaru-aica-math.h"

using namespace hikaru;

static int failures, checks;

#define CHECK(name_, ok_, fmt_, ...) \
	do { \
		checks++; \
		if (!(ok_)) { \
			failures++; \
			fprintf (stderr, "FAIL %s: " fmt_ "\n", name_, ##__VA_ARGS__); \
		} \
	} while (0)

static int32_t
round_trip (int32_t v)
{
	return aica_dsp_unpack (aica_dsp_pack (v));
}

/* The values under 2048 kept whole, the sign included */
static void
test_small (void)
{
	int32_t v, bad = 0, first = 0;

	for (v = -2048; v < 2048; v++)
		if (round_trip (v) != v && !bad++)
			first = v;
	CHECK ("small", bad == 0, "%d values changed, the first %d -> %d", bad, first,
	       round_trip (first));
	CHECK ("-1000", round_trip (-1000) == -1000, "-1000 -> %d", round_trip (-1000));
	CHECK ("-1", round_trip (-1) == -1, "-1 -> %d", round_trip (-1));
}

/* Above: within the twelve bits kept, in order, and packed again the same */
static void
test_large (void)
{
	int32_t v, prev = -0x800000, errors = 0, disorder = 0, unstable = 0;

	for (v = -0x800000; v < 0x800000; v += 97) {
		int32_t r = round_trip (v);
		int32_t mag = (v < 0) ? -v : v;
		int32_t tol = (mag >> 11) + 1;

		if (abs (r - v) > tol)
			errors++;
		if (r < prev)
			disorder++;
		if (aica_dsp_pack (r) != aica_dsp_pack (v))
			unstable++;
		prev = r;
	}
	CHECK ("large: precision", errors == 0, "%d values off by more than 1/2048", errors);
	CHECK ("large: order", disorder == 0, "%d values out of order", disorder);
	CHECK ("large: again", unstable == 0, "%d values packed otherwise once unpacked", unstable);
	CHECK ("most negative", round_trip (-0x800000) == -0x800000, "-800000 -> %X",
	       round_trip (-0x800000));
	CHECK ("most positive", round_trip (0x7FFFFF) <= 0x7FFFFF &&
	       round_trip (0x7FFFFF) >= 0x7FF000, "7FFFFF -> %X", round_trip (0x7FFFFF));
}

/* ADPCM: a nibble and its negative mirror each other; the value stays in 16
 * bits, the step in its range, over random nibbles and the extremes */
static void
test_adpcm (void)
{
	int32_t a = 0, sa = 0x7F, b = 0, sb = 0x7F, value, step;
	unsigned i, nib, seed = 12345;
	bool bounded = true;

	for (i = 0; i < 1000; i++) {
		nib = (i * 7) & 7;
		aica_adpcm_step (a, sa, nib);
		aica_adpcm_step (b, sb, nib | 8);
		/* But at the ends of 16 bits: 32767 and -32768 */
		if ((a != -b && !(a == 32767 && b == -32768)) || sa != sb)
			break;
	}
	CHECK ("adpcm: mirror", i == 1000, "differ at nibble %u: %d / %d", i, a, b);

	value = 0;
	step = 0x7F;
	for (i = 0; i < 100000; i++) {
		seed = seed * 1103515245u + 12345u;
		aica_adpcm_step (value, step, (seed >> 16) & 0xF);
		if (value < -32768 || value > 32767 || step < 0x7F || step > 0x6000)
			bounded = false;
	}
	CHECK ("adpcm: bounded", bounded, "value %d, step %X", value, step);

	value = 0;
	step = 0x7F;
	for (i = 0; i < 200; i++)
		aica_adpcm_step (value, step, 7);
	CHECK ("adpcm: top", value == 32767 && step == 0x6000, "value %d, step %X", value, step);
	for (i = 0; i < 400; i++)
		aica_adpcm_step (value, step, 0xF);
	CHECK ("adpcm: bottom", value == -32768 && step == 0x6000, "value %d, step %X", value, step);
	for (i = 0; i < 200; i++)
		aica_adpcm_step (value, step, 0);
	CHECK ("adpcm: small steps shrink", step == 0x7F, "step %X", step);

	/* The first step from rest: 7F times 1/8 */
	value = 0;
	step = 0x7F;
	CHECK ("adpcm: first", aica_adpcm_step (value, step, 0) == 15, "value %d", value);
}

int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;
	test_small ();
	test_large ();
	test_adpcm ();
	printf ("aicatest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
