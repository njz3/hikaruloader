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

#ifndef __HL_AUDIO_H__
#define __HL_AUDIO_H__

#include <cstdio>
#include <vector>

#include <SDL.h>

#include "hl/core.h"

namespace hl {

/* A stream's rate changed, by a ratio that may change on each call: four-
 * point (Catmull-Rom) interpolation, the stream's position and its last
 * samples kept from a call to the next. Two samples of latency */
class Resampler {
public:
	void	 reset (unsigned channels);
	/* in: frames of channels samples; ratio: the output's frames an
	 * input's; out: the frames made, appended */
	void	 run (const int16_t *in, size_t frames, double ratio, std::vector<float> &out);

private:
	unsigned channels = 0;
	/* The next output's position, in input frames from the history's
	 * first */
	double	 pos = 1.0;
	/* The last three input frames, then the call's */
	std::vector<float> work;
};

/* The output's rate adjusted to keep its queue at a level: the host's
 * clocks (the frame limiter's, the sound card's) drift apart, and with a
 * fixed rate the queue would run dry or overflow. Up to 0.5 % off, a pitch
 * change nobody hears */
class AudioRate {
public:
	/* target: the queue's level, in output frames */
	void	 reset (double target);
	/* queued: the output's frames queued now; nominal: the output's
	 * frames an input's at the expected speed. Returns the ratio to
	 * resample with */
	double	 update (double queued, double nominal);

	double	 target = 0.0;
	/* The queue's level, averaged over some frames */
	double	 level = 0.0;
};

/* The machine's sound to the player: queued to SDL's default output a frame
 * at a time, with some 50 ms of latency, resampled to the sound card's rate
 * (AudioRate, Resampler).
 *
 * The machine's sound is its cabinet's speakers' (games.json's "audio"):
 * the front left and right, then the rear's when it has some (Hikaru's
 * option speakers). It goes to the sound card's speakers: the front to the
 * front, the rear to the rear (quad, 4.1, 5.1, 6.1, 7.1) or, without, mixed
 * into the front; the LFE (2.1, 4.1...) gets all of it, low-passed; the
 * center none. The balance fades the front or the rear.
 *
 * HR_NO_AUDIO=1 opens no output; HR_WAV=<path> also writes all of it, as the
 * machine makes it, to a WAV file (the agent's runs, which go faster than
 * the sound, included). The settings set output, balance and volume. */
class Audio {
public:
	static const unsigned RATE = 44100;

	/* The speakers: the sound card's, or as many channels */
	enum Output {
		OUTPUT_AUTO = 0,
		OUTPUT_MONO = 1,
		OUTPUT_STEREO = 2,
		OUTPUT_2_1 = 3,
		OUTPUT_QUAD = 4,
		OUTPUT_5_1 = 6,
		OUTPUT_7_1 = 8
	};

	Audio ();
	/* Closes the output, completes the WAV file */
	~Audio ();

	Audio (const Audio &) = delete;
	Audio &operator= (const Audio &) = delete;

	/* samples: one frame's worth, channels a sample: front left and
	 * right, then rear left and right; opens the output on the first
	 * call */
	void	 play (const std::vector<int16_t> &samples, unsigned channels);
	/* A rear folded to the front (no rear speakers) passes 16 bits at
	 * times, the two boards' sum: what passes -2.5 dBFS rounded off to
	 * full scale, smoothly (the slope 1 at the knee) */
	static float soft_limit (float x);
	/* Opened again at the next frame (output changed), or tried again
	 * after a failure */
	void	 close ();

	/* Taken when the output opens */
	Output	 output;
	/* 0 the front alone, 100 the rear alone, 50 both at their full
	 * level */
	unsigned balance;
	/* Percents */
	unsigned volume;
	/* The frames a second the frame limiter holds, 0 without: the
	 * machine's sound comes that fast */
	float	 frame_rate;
	/* The queue's level kept, in milliseconds (20 to 200): the sound's
	 * lag behind the picture, against its gaps when frames come late;
	 * taken when the output opens */
	unsigned latency;
	/* Since the output opened: the times it ran dry, the frames of
	 * sound dropped (the frames ran too fast) */
	unsigned underruns, drops;

private:
	bool	 open ();
	void	 write_wav_header ();

	SDL_AudioDeviceID dev;
	/* The output's channels, 1 to 8, in SDL's order, and rate */
	unsigned out_channels;
	unsigned out_rate;
	Resampler resampler;
	AudioRate rate;
	unsigned in_channels;
	std::vector<float> res;
	/* The LFE's low-pass, and its factor at the output's rate */
	float	 lfe, lfe_k;
	/* HR_NO_AUDIO */
	bool	 off;
	/* Opened, or failed to, since the last close () */
	bool	 tried;
	std::vector<int16_t> buf;
	FILE	*wav;
	unsigned wav_channels;
	uint32_t wav_bytes;
};

} /* namespace hl */

#endif /* __HL_AUDIO_H__ */
