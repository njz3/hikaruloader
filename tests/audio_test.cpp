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

/* The sound output's tests: the resampler (its latency, its error on a
 * sine, its continuity when the ratio changes) and the rate control against
 * a simulated sound card, whose clock drifts from the frame limiter's. Run
 * by ctest, or alone: audiotest. */

#include <cmath>
#include <vector>

#include "hl/audio.h"

static int failures, checks;

#define CHECK(name_, ok_, fmt_, ...) \
	do { \
		checks++; \
		if (!(ok_)) { \
			failures++; \
			fprintf (stderr, "FAIL %s: " fmt_ "\n", name_, ##__VA_ARGS__); \
		} \
	} while (0)

static const double PI = 3.14159265358979323846;
static const unsigned IN_RATE = 44100, FRAME = 735;

/* A frame of a sine, both channels, from input sample `at` */
static void
sine_frame (std::vector<int16_t> &v, size_t at, double hz, double amp)
{
	v.resize (FRAME * 2);
	for (size_t i = 0; i < FRAME; i++) {
		double x = amp * sin (2.0 * PI * hz * (double) (at + i) / IN_RATE);
		v[2 * i] = v[2 * i + 1] = (int16_t) lrint (x);
	}
}

/* At ratio 1, the input two samples late, exactly */
static void
test_identity ()
{
	hl::Resampler rs;
	std::vector<int16_t> in;
	std::vector<float> out;
	size_t i, bad = 0;

	rs.reset (2);
	for (i = 0; i < 20; i++) {
		sine_frame (in, i * FRAME, 997.0, 20000.0);
		rs.run (in.data (), FRAME, 1.0, out);
	}
	CHECK ("identity count", out.size () == 20 * FRAME * 2, "%zu", out.size () / 2);
	for (i = 2; i < out.size () / 2; i++) {
		double want = lrint (20000.0 * sin (2.0 * PI * 997.0 * (double) (i - 2) / IN_RATE));
		if (out[2 * i] != (float) want || out[2 * i + 1] != (float) want)
			bad++;
	}
	CHECK ("identity values", bad == 0, "%zu samples differ", bad);
}

/* Varying ratios, a frame each, around 48 kHz: each output against the
 * sine at its own position (tracked here as the resampler does). Returns
 * the error's level under the sine, in dB */
static double
sine_error (double hz, const std::vector<double> &ratios)
{
	hl::Resampler rs;
	std::vector<int16_t> in;
	std::vector<float> out;
	double pos = 1.0, err = 0.0, sig = 0.0;
	size_t n = 0, f;

	rs.reset (2);
	for (f = 0; f < ratios.size (); f++) {
		double step = 1.0 / ratios[f];

		out.clear ();
		sine_frame (in, f * FRAME, hz, 16000.0);
		rs.run (in.data (), FRAME, ratios[f], out);
		for (size_t k = 0; k < out.size () / 2; k++) {
			/* pos counts from the history's first: 3 before the frame */
			double t = (double) (f * FRAME) + pos - 3.0;
			double want = 16000.0 * sin (2.0 * PI * hz * t / IN_RATE);

			if (t > 4.0) {
				err += (out[2 * k] - want) * (out[2 * k] - want);
				sig += want * want;
				n++;
			}
			pos += step;
		}
		pos -= FRAME;
	}
	return (n && sig > 0.0) ? 10.0 * log10 (err / sig) : 0.0;
}

static void
test_sine ()
{
	std::vector<double> fixed (200, 48000.0 / IN_RATE), wobbly;
	double e;

	for (size_t i = 0; i < 200; i++)
		wobbly.push_back (48000.0 / IN_RATE * (1.0 + 0.005 * ((i % 3) - 1.0)));
	e = sine_error (1000.0, fixed);
	printf ("  1 kHz to 48 kHz: error %.1f dB\n", e);
	CHECK ("1 kHz to 48 kHz", e < -60.0, "error %.1f dB", e);
	e = sine_error (5000.0, fixed);
	printf ("  5 kHz to 48 kHz: error %.1f dB\n", e);
	CHECK ("5 kHz to 48 kHz", e < -38.0, "error %.1f dB", e);
	e = sine_error (1000.0, wobbly);
	printf ("  1 kHz, ratio changing each frame: error %.1f dB\n", e);
	CHECK ("1 kHz, ratio changing each frame", e < -60.0, "error %.1f dB", e);
}

/* The output's queue in time: the frames come every 1 / fps seconds (plus
 * the limiter's jitter) and are resampled as Audio::play does; the card
 * takes card frames each card / (rate * (1 + drift)) seconds (512, fewer for
 * a short latency, as Audio::open asks). Ten minutes: after the first
 * seconds, it never runs dry nor overflows, and the queue stays near its
 * target, the latency */
static void
test_rate (const char *name, unsigned out_rate, double fps, double drift, double jitter,
           double latency = 0.05, double card = 512.0)
{
	hl::Resampler rs;
	hl::AudioRate rate;
	std::vector<int16_t> in (FRAME * 2, 0);
	std::vector<float> out;
	const double lead = latency * out_rate, max = 4.0 * lead;
	double queued = lead, next_pull = card / (out_rate * (1.0 + drift));
	double lo = 1e9, hi = 0.0;
	unsigned dry = 0, dropped = 0, f, frames = (unsigned) (600 * fps);
	unsigned seed = 12345;

	rs.reset (2);
	rate.reset (lead);
	for (f = 0; f < frames; f++) {
		double now, nominal, ratio;

		/* The limiter's wake-up, a few ms off */
		seed = seed * 1103515245u + 12345u;
		now = (f + 1) / fps + jitter * (((seed >> 16) & 0x7FFF) / 32767.0 - 0.5);
		while (next_pull <= now) {
			if (queued < card) {
				dry++;
				queued = lead;
				rate.level = lead;
			} else
				queued -= card;
			next_pull += card / (out_rate * (1.0 + drift));
		}
		if (queued > max) {
			dropped++;
			continue;
		}
		nominal = (double) out_rate / (FRAME * fps);
		ratio = rate.update (queued, nominal);
		out.clear ();
		rs.run (in.data (), FRAME, ratio, out);
		queued += out.size () / 2;
		if (f > 10 * fps) {
			lo = (queued < lo) ? queued : lo;
			hi = (queued > hi) ? queued : hi;
		}
	}
	printf ("  %s: queue %.1f-%.1f ms (target %.0f)\n", name, 1000.0 * lo / out_rate, 1000.0 * hi / out_rate, 1000.0 * lead / out_rate);
	CHECK (name, dry == 0 && dropped == 0 && lo > 0.2 * lead && hi < 3.0 * lead,
	       "dry %u, dropped %u, queue %.0f-%.0f (target %.0f)", dry, dropped, lo, hi, lead);
}

/* The fold's limiter: untouched under its knee, under full scale above,
 * in order, and without a step at the knee */
static void
test_soft_limit (void)
{
	float x, prev = -1e9f, y;
	bool order = true, under = true, kept = true;

	for (x = -200000.0f; x <= 200000.0f; x += 7.0f) {
		y = hl::Audio::soft_limit (x);
		if (y < prev)
			order = false;
		if (fabsf (y) > 32767.0f)
			under = false;
		if (fabsf (x) <= 24576.0f && y != x)
			kept = false;
		prev = y;
	}
	CHECK ("soft limit: kept", kept, "a value under the knee changed");
	CHECK ("soft limit: under", under, "a value over full scale");
	CHECK ("soft limit: order", order, "out of order");
	y = hl::Audio::soft_limit (24577.0f) - hl::Audio::soft_limit (24576.0f);
	CHECK ("soft limit: knee", y > 0.99f && y <= 1.0f, "slope %f at the knee", y);
	CHECK ("soft limit: odd", hl::Audio::soft_limit (-30000.0f) == -hl::Audio::soft_limit (30000.0f),
	       "not symmetric");
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;

	test_identity ();
	test_sine ();
	test_soft_limit ();
	test_rate ("48 kHz, 60 fps", 48000, 60.0, 0.0, 0.004);
	test_rate ("48 kHz, card 0.3 % fast", 48000, 60.0, 0.003, 0.004);
	test_rate ("48 kHz, card 0.3 % slow", 48000, 60.0, -0.003, 0.004);
	test_rate ("44.1 kHz, card 0.1 % fast", 44100, 60.0, 0.001, 0.004);
	test_rate ("96 kHz, limiter at 57.5 fps", 96000, 57.5, 0.0, 0.004);
	test_rate ("48 kHz, 30 ms, card 0.3 % fast", 48000, 60.0, 0.003, 0.004, 0.030, 256.0);
	test_rate ("48 kHz, 20 ms, card 0.3 % slow", 48000, 60.0, -0.003, 0.004, 0.020, 128.0);
	test_rate ("48 kHz, 200 ms, card 0.3 % fast", 48000, 60.0, 0.003, 0.004, 0.200, 512.0);
	printf ("audiotest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
