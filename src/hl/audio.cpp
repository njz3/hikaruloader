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

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hl/audio.h"

namespace hl {

/****************************************************************************
 Resampler
****************************************************************************/

void
Resampler::reset (unsigned ch)
{
	channels = ch;
	pos = 1.0;
	work.assign (3 * ch, 0.0f);
}

void
Resampler::run (const int16_t *in, size_t frames, double ratio, std::vector<float> &out)
{
	double step = 1.0 / ratio, end = (double) frames + 1.0;
	size_t i, c;

	if (!channels || !frames)
		return;
	/* After the last three frames, the call's: the outputs from 1 to
	 * frames + 1 have their four */
	work.resize ((3 + frames) * channels);
	for (i = 0; i < frames * channels; i++)
		work[3 * channels + i] = (float) in[i];
	for (; pos < end; pos += step) {
		size_t k = (size_t) pos;
		float f = (float) (pos - (double) k);
		const float *xm1 = &work[(k - 1) * channels];
		const float *x0 = xm1 + channels, *x1 = x0 + channels, *x2 = x1 + channels;

		for (c = 0; c < channels; c++)
			out.push_back (x0[c] + 0.5f * f * (x1[c] - xm1[c] +
			               f * (2.0f * xm1[c] - 5.0f * x0[c] + 4.0f * x1[c] - x2[c] +
			               f * (3.0f * (x0[c] - x1[c]) + x2[c] - xm1[c]))));
	}
	pos -= (double) frames;
	memmove (work.data (), &work[frames * channels], 3 * channels * sizeof (float));
	work.resize (3 * channels);
}

/****************************************************************************
 AudioRate
****************************************************************************/

/* The deviation from the nominal rate: 1 % a queue's target off, at most
 * 0.5 %; the level's average over some ten frames */
static const double RATE_GAIN = 0.01;
static const double RATE_MAX_DEV = 0.005;
static const double LEVEL_SMOOTHING = 0.1;

void
AudioRate::reset (double t)
{
	target = t;
	level = t;
}

double
AudioRate::update (double queued, double nominal)
{
	double dev;

	level += LEVEL_SMOOTHING * (queued - level);
	dev = RATE_GAIN * (target - level) / target;
	dev = (dev > RATE_MAX_DEV) ? RATE_MAX_DEV : (dev < -RATE_MAX_DEV) ? -RATE_MAX_DEV : dev;
	return nominal * (1.0 + dev);
}

/****************************************************************************
 Audio
****************************************************************************/

/* The output's queue: its level kept is the latency (the silence queued
 * ahead at the start and when it runs dry: a pause, slow frames), the most,
 * beyond which a frame's sound is dropped (frames faster than the sound:
 * the agent's, the limiter off), four times that: 50 and 200 ms by
 * default */
static const unsigned LATENCY_MIN = 20, LATENCY_MAX = 200;
static const double AUDIO_MAX_LEADS = 4.0;

Audio::Audio () :
	output (OUTPUT_AUTO),
	balance (50),
	volume (100),
	frame_rate (60.0f),
	latency (50),
	underruns (0),
	drops (0),
	dev (0),
	out_channels (0),
	out_rate (RATE),
	in_channels (0),
	lfe (0.0f),
	lfe_k (0.0f),
	off (false),
	tried (false),
	wav (NULL),
	wav_channels (0),
	wav_bytes (0)
{
	const char *path = getenv ("HR_WAV");
	const char *no = getenv ("HR_NO_AUDIO");

	if (no && *no && strcmp (no, "0"))
		off = true;
	if (path && *path) {
		wav = fopen (path, "wb");
		if (!wav)
			HL_ERROR ("audio: cannot write '%s'", path);
	}
}

Audio::~Audio ()
{
	close ();
	if (wav) {
		write_wav_header ();
		fclose (wav);
	}
}

void
Audio::close ()
{
	if (dev) {
		SDL_CloseAudioDevice (dev);
		if (underruns || drops)
			HL_PRINT ("audio: ran dry %u times, %u frames of sound dropped", underruns, drops);
	}
	dev = 0;
	tried = false;
}

/* The speakers of SDL's channel orders: where the front, rear and LFE
 * channels are (-1: none). 1: mono; 2: FL FR; 3: FL FR LFE; 4: FL FR BL BR;
 * 5: FL FR LFE BL BR; 6: FL FR FC LFE BL BR; 7: FL FR FC LFE BC SL SR (the
 * sides for the rear); 8: FL FR FC LFE BL BR SL SR */
static const struct {
	int front, rear, lfe;
	const char *name;
} layouts[9] = {
	{ -1, -1, -1, "none" },
	{ -1, -1, -1, "mono" },
	{ 0, -1, -1, "stereo" },
	{ 0, -1, 2, "2.1" },
	{ 0, 2, -1, "quad" },
	{ 0, 3, 2, "4.1" },
	{ 0, 4, 3, "5.1" },
	{ 0, 5, 3, "6.1" },
	{ 0, 4, 3, "7.1" }
};

bool
Audio::open ()
{
	SDL_AudioSpec want, have;
	unsigned lead_ms = MAX2 (LATENCY_MIN, MIN2 (LATENCY_MAX, latency)), samples = 512;

	tried = true;
	if (!SDL_WasInit (SDL_INIT_AUDIO) && SDL_InitSubSystem (SDL_INIT_AUDIO)) {
		HL_ERROR ("audio: %s", SDL_GetError ());
		return false;
	}
	/* The card's own buffer, a quarter of the latency at most (512 from
	 * 47 ms, 128 at 20) */
	while (samples > 128 && samples * 4 > lead_ms * RATE / 1000)
		samples /= 2;
	memset (&want, 0, sizeof (want));
	want.freq = RATE;
	want.format = AUDIO_S16SYS;
	want.samples = (Uint16) samples;

	/* The sound card's own rate, which the resampler makes; its own
	 * speakers, or those asked for, which SDL converts to the card's */
	want.channels = (Uint8) ((output == OUTPUT_AUTO) ? 8 : output);
	dev = SDL_OpenAudioDevice (NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
	                           ((output == OUTPUT_AUTO) ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0));
	if (dev && (have.channels < 1 || have.channels > 8 || have.freq < 8000 || have.freq > 384000)) {
		SDL_CloseAudioDevice (dev);
		want.channels = 2;
		dev = SDL_OpenAudioDevice (NULL, 0, &want, &have, 0);
	}
	if (!dev) {
		HL_ERROR ("audio: %s", SDL_GetError ());
		return false;
	}
	out_channels = have.channels;
	out_rate = (unsigned) have.freq;
	lfe_k = 1.0f - expf (-2.0f * 3.14159265f * 120.0f / (float) out_rate);
	underruns = drops = 0;
	in_channels = 0;
	HL_PRINT ("audio: %s, %u Hz, %u ms of latency", layouts[out_channels].name, out_rate, lead_ms);

	/* The queue's lead, at once */
	rate.reset (lead_ms / 1000.0 * out_rate);
	buf.assign ((size_t) rate.target * out_channels, 0);
	SDL_QueueAudio (dev, buf.data (), (Uint32) (buf.size () * 2));
	SDL_PauseAudioDevice (dev, 0);
	return true;
}

static void
put_le (uint8_t *p, uint32_t val, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++)
		p[i] = (uint8_t) (val >> (8 * i));
}

/* RIFF, 16-bit PCM; the sizes, once more at the end */
void
Audio::write_wav_header ()
{
	uint8_t h[44];

	memcpy (h, "RIFF", 4);
	put_le (h + 4, 36 + wav_bytes, 4);
	memcpy (h + 8, "WAVEfmt ", 8);
	put_le (h + 16, 16, 4);
	put_le (h + 20, 1, 2);				/* PCM */
	put_le (h + 22, wav_channels, 2);
	put_le (h + 24, RATE, 4);
	put_le (h + 28, RATE * wav_channels * 2, 4);	/* bytes a second */
	put_le (h + 32, wav_channels * 2, 2);		/* a frame */
	put_le (h + 34, 16, 2);				/* bits a sample */
	memcpy (h + 36, "data", 4);
	put_le (h + 40, wav_bytes, 4);
	fseek (wav, 0, SEEK_SET);
	fwrite (h, 1, sizeof (h), wav);
	fseek (wav, 0, SEEK_END);
}

/* The rear's share of a fold to the front: -3 dB, as the surround downmixes
 * have it. The board never sums its two sound boards: at full gain the folds
 * clipped some 2000 samples in 90 s of PHARRIER's, SWRACER's, SGNASCAR's play */
static const float FOLD_REAR = 0.70710678f;

float
Audio::soft_limit (float x)
{
	const float knee = 24576.0f, room = 32767.0f - knee;
	float m = fabsf (x);

	if (m <= knee)
		return x;
	m = knee + room * tanhf ((m - knee) / room);
	return (x < 0.0f) ? -m : m;
}

static inline int16_t
clamp16 (int32_t v)
{
	return (int16_t) ((v > 32767) ? 32767 : (v < -32768) ? -32768 : v);
}

void
Audio::play (const std::vector<int16_t> &samples, unsigned channels)
{
	size_t frames = samples.size () / channels, i;
	/* The front/rear balance, each side fading from the middle */
	float vol = (float) volume / 100.0f;
	float front = (balance <= 50) ? vol : vol * (float) (100 - balance) / 50.0f;
	float back = (balance >= 50) ? vol : vol * (float) balance / 50.0f;
	double queued, nominal, ratio;
	size_t n;
	int f, r, l;

	if (!frames)
		return;
	if (wav) {
		/* Little-endian, as the hosts are */
		if (!wav_channels) {
			wav_channels = channels;
			write_wav_header ();
		}
		if (channels == wav_channels) {
			fwrite (samples.data (), 2, frames * channels, wav);
			wav_bytes += (uint32_t) (frames * channels * 2);
		}
	}
	if (off)
		return;
	/* Once a close (), not each frame */
	if (!dev && (tried || !open ()))
		return;

	if (in_channels != channels) {
		in_channels = channels;
		resampler.reset (channels);
	}

	/* The queue's level: dry, its lead again; too full, the frame's
	 * sound dropped */
	queued = (double) SDL_GetQueuedAudioSize (dev) / (out_channels * 2);
	if (queued > AUDIO_MAX_LEADS * rate.target) {
		drops++;
		return;
	}
	if (queued == 0.0) {
		underruns++;
		buf.assign ((size_t) rate.target * out_channels, 0);
		SDL_QueueAudio (dev, buf.data (), (Uint32) (buf.size () * 2));
		rate.level = queued = rate.target;
	}

	/* To the output's rate, the frames coming as fast as the limiter
	 * lets them */
	nominal = (double) out_rate / ((frame_rate > 0.0f) ? (double) frames * frame_rate : (double) RATE);
	ratio = rate.update (queued, nominal);
	res.clear ();
	resampler.run (samples.data (), frames, ratio, res);
	n = res.size () / channels;

	f = layouts[out_channels].front;
	r = layouts[out_channels].rear;
	l = layouts[out_channels].lfe;
	buf.assign (n * out_channels, 0);
	for (i = 0; i < n; i++) {
		const float *in = &res[i * channels];
		int16_t *o = &buf[i * out_channels];
		float in_l = in[0], in_r = in[(channels > 1) ? 1 : 0];
		float in_rl = (channels >= 4) ? in[2] : 0.0f;
		float in_rr = (channels >= 4) ? in[3] : 0.0f;
		float fl = in_l * front, fr = in_r * front;
		float rl = in_rl * back, rr = in_rr * back;

		if (out_channels == 1) {
			o[0] = clamp16 ((int32_t) ((fl + fr + rl + rr) * 0.5f));
			continue;
		}
		/* Without rear speakers, the rear's sound at the front */
		if (r < 0) {
			if (channels >= 4) {
				fl = soft_limit (fl + rl * FOLD_REAR);
				fr = soft_limit (fr + rr * FOLD_REAR);
			}
		} else {
			o[r] = clamp16 ((int32_t) rl);
			o[r + 1] = clamp16 ((int32_t) rr);
		}
		o[f] = clamp16 ((int32_t) fl);
		o[f + 1] = clamp16 ((int32_t) fr);
		if (l >= 0) {
			lfe += lfe_k * ((in_l + in_r + in_rl + in_rr) * vol * 0.5f - lfe);
			o[l] = clamp16 ((int32_t) lfe);
		}
	}
	SDL_QueueAudio (dev, buf.data (), (Uint32) (buf.size () * 2));
}

} /* namespace hl */
