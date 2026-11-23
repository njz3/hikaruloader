/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/pvr/elan.cpp (GPL-2.0-or-later, Copyright
 * flyinghead and contributors).
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
#include <cmath>
#include <functional>
#include <emmintrin.h>
#include <cstring>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/machine.h"
#include "hl/profile.h"
#include "hl/state.h"

#include "cpu/sh/sh4.h"
#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Elan
 * ========
 *
 * The NAOMI 2's transform and lighting chip, with its 32 MB of RAM:
 *
 *  08800000-088000FF	Its registers
 *  09000000-09FFFFFF	Its command port: 32 bytes, run at the last word
 *  0A000000-0BFFFFFF	Its RAM
 *
 * It reads commands, 32 bytes or more each, from the port and, following their
 * links, from its RAM. Bit 27 of a command's first word tells them from the
 * parameters of the Tile Accelerator, which go to the Holly as they are:
 *
 *  11-8  the command: 0 nothing, 3 the projection, 4 a matrix or a light, 5 a
 *	  material, 7 a list of vertices, 8 a model, E a wait, F a link
 *
 * Its lists of vertices are transformed, lit and clipped here, one vertex at a
 * time, into the screen's triangles the TA would have made of them.
 */

enum {
	PCW_NAOMI2	= 1 << 27,

	CMD_NULL	= 0,
	CMD_PROJECTION	= 3,
	CMD_MATRIX_LIGHT = 4,
	CMD_GMP		= 5,
	CMD_ICH		= 7,
	CMD_MODEL	= 8,
	CMD_WAIT	= 14,
	CMD_LINK	= 15,

	VTX_V		= 0x002,
	VTX_VU		= 0x00A,
	VTX_VNU		= 0x00E,
	VTX_VR		= 0x042,
	VTX_VUR		= 0x04A,
	VTX_VUB		= 0x10A,

	/* The material's parameters */
	GMP_D0		= 1 << 0,
	GMP_S0		= 1 << 1,
	GMP_B0		= 1 << 9,
	GMP_E0		= 1 << 11,
	GMP_E1		= 1 << 12,

	/* The lights' ways */
	LMODE_SINGLE_SIDED = 0,
	LMODE_DOUBLE_SIDED = 1,

	/* Where a light goes */
	ROUTE_SPEC_TO_OFFSET = 1,
	ROUTE_DIFF_TO_OFFSET = 2,
	ROUTE_ALPHA	= 4,
	ROUTE_SUB	= 8,

	/* The polygon words' fields */
	TSP_USE_ALPHA	= 1 << 20,
	TSP_IGNORE_TEX_A = 1 << 19,
	ISP_CULL_LOW	= 1 << 27,
};

static const uint32_t RAM_SIZE = 32 * MB;
static const uint32_t RAM_MASK = RAM_SIZE - 1;
static const uint32_t SH_RAM_MASK = 0x01FFFFFF;
static const uint32_t VRAM_SIZE = 16 * MB;
static const uint32_t VRAM_MASK = VRAM_SIZE - 1;
static const uint32_t NONE = 0xFFFFFFFF;

static const uint32_t SIZE_GMP = 64;
static const uint32_t SIZE_MATRIX = 160;
static const uint32_t SIZE_LIGHT = 32;

static const uint64_t DMA_BYTES_PER_SECOND = 800000000;	/* 64 bits, 100 MHz */

/* Against a list that links to itself, or garbage */
static const unsigned MAX_DEPTH = 128;
static const uint64_t MAX_COMMANDS = 8000000;

static const float BASE_FACTOR = 2.0f;

static inline float
to_float (uint32_t val)
{
	float f;
	memcpy (&f, &val, 4);
	return f;
}

static inline void
unpack (uint32_t c, float *out)
{
	out[0] = (float) ((c >> 16) & 255) / 255.0f;
	out[1] = (float) ((c >> 8) & 255) / 255.0f;
	out[2] = (float) (c & 255) / 255.0f;
	out[3] = (float) (c >> 24) / 255.0f;
}

static inline uint32_t
to_byte (float f)
{
	return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (uint32_t) (f * 255.0f + 0.5f);
}

static inline uint32_t
pack (const float *c)
{
	return (to_byte (c[3]) << 24) | (to_byte (c[0]) << 16) | (to_byte (c[1]) << 8) | to_byte (c[2]);
}

static inline float
clamp01 (float f)
{
	return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static inline float
dot3 (const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void
normalize3 (float *v)
{
	float len = sqrtf (dot3 (v, v));

	if (len > 0.0f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

/* The 12 bits of a light's axis, the top 8 in the light's word, the low 4 in
 * its command */
static inline float
light_axis (uint32_t top, uint32_t low)
{
	return (float) (((int32_t) (int8_t) top << 4) | (int32_t) (low & 15)) / 2047.0f;
}

/* A float of which only the top 16 bits are kept */
static inline float
short_float (uint32_t v)
{
	return to_float ((v & 0xFFFF) << 16);
}

/* A vertex in the view's space, lit, and where it falls on the screen */
struct Elan::Transformed {
	float pos[3];
	float u, v;
	float base[4], offset[4];
	Vertex screen;
	/* Its screen's colours, and those base and offset stand for, to come
	 * from a vertex lit later (a ColorRef's ref; -1: those here); a flat
	 * triangle's last vertex has the next one's screen colours */
	int ref, fref;
};

std::atomic<bool> gpu_lighting { false };

Elan::Elan (Board *board_, hl::Buffer *ram_) :
	hl::Device (board_->machine),
	board (board_),
	ram (ram_)
{
	ram_base = (const uint8_t *) ram->ptr (0);
	near_plane = 0.001f;
	far_plane = 100000.0f;
	clear_state ();
	s.reg30 = 0x31;
	reset_projection ();
}

void
Elan::reset_projection ()
{
	s.proj[0] = 579.411194f;
	s.proj[1] = -320.0f;
	s.proj[2] = -579.411194f;
	s.proj[3] = -240.0f;
}

void
Elan::clear_state ()
{
	unsigned i;

	s.gmp = s.instance = s.light_model = NONE;
	for (i = 0; i < MAX_LIGHTS; i++)
		s.lights[i] = NONE;
	update ();
}

void
Elan::reset (hl::ResetType type)
{
	s.reg10 = s.reg74 = s.reg78 = 0;
	s.reg30 = 0x31;
	s.dma_lines = 0;
	s.dma_end = NONE;
	memset (s.cmd, 0, sizeof (s.cmd));
	culling_reversed = open_volume = shadowed_volume = false;
	model_tsp = 0;
	drop_lighting ();
	if (type == hl::RESET_HARD) {
		ram->clear ();
		reset_projection ();
	}
	clear_state ();
}

/*
 * The Time It Takes
 *
 * Of the chip's timings only the texture DMA's is known at all: its bus is 64
 * bits at 100 MHz (noted by MAME's 315-6289, the SH-4's channel 2 feeding it),
 * whose bytes are counted here, in the lines the machine advances by. How long
 * the lists of vertices take (and thus when a list's end comes, or the
 * "geometry timeout" bits 2-3 of register 74) is documented nowhere: these run
 * at once, at the write that starts them. Where the output of its interrupt
 * registers (74 status, 78 mask) goes is not known either: it is not wired.
 */

unsigned
Elan::dma_duration (uint32_t bytes) const
{
	uint64_t per_line = DMA_BYTES_PER_SECOND / (60 * std::max (board->frame_lines, 1u));

	return (unsigned) (1 + (bytes ? (bytes - 1) / per_line : 0));
}

void
Elan::line ()
{
	if (s.dma_lines && !--s.dma_lines) {
		s.reg74 |= 1;
		if (s.dma_end != NONE)
			((Sh4 *) board->sh)->ddt_done (2, s.dma_end);
	}
}

/*
 * The Registers
 */

int
Elan::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t data = 0;

	if ((addr & 0x0FFFFFFF) >= 0x09000000) {
		set_ptr (val, size, 0);
		return 0;
	}
	switch (addr & 0xFF) {
	case 0x00: data = 0xE1AD0000; break;	/* its magic number */
	case 0x04: data = 0x10; break;		/* its revision */
	case 0x0C: data = 1; break;		/* the commands waiting */
	case 0x10: data = s.reg10; break;
	case 0x14: data = 0x2029; break;	/* the SDRAM's refresh */
	case 0x1C: data = 0x87320961; break;	/* the SDRAM's configuration */
	case 0x30: data = s.reg30; break;	/* the macro tiler */
	case 0x74: data = s.reg74; break;	/* 0 DMA done, 1 commands done, 4-6 errors */
	case 0x78: data = s.reg78; break;	/* its interrupts' mask */
	default: break;
	}
	set_ptr (val, size, data);
	return 0;
}

int
Elan::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t data = (uint32_t) val;

	if ((addr & 0x0FFFFFFF) >= 0x09000000) {
		command_word ((addr >> 2) & 7, data);
		if (size == 8)
			command_word (((addr >> 2) + 1) & 7, (uint32_t) (val >> 32));
		return 0;
	}
	switch (addr & 0xFF) {
	case 0x08:
		if (data == 0)
			s.reg74 = 0;
		break;
	case 0x10:
		s.reg10 = data;
		break;
	case 0x30:
		s.reg30 = data;
		break;
	case 0x74:
		s.reg74 &= ~data;
		break;
	case 0x78:
		s.reg78 = data;
		break;
	default:
		break;
	}
	return 0;
}

void
Elan::command_word (unsigned index, uint32_t data)
{
	s.cmd[index] = data;
	if (index != 7)
		return;
	commands = 0;
	bool ok;
	{
		HL_PROFILE (ELAN);
		ok = run ((const uint8_t *) s.cmd, sizeof (s.cmd), 0);
	}
	if (ok && !s.dma_lines)
		s.reg74 |= 2;
}

/*
 * What the Commands Leave Set
 */

uint32_t
Elan::ram_offset (const uint8_t *p, uint32_t bytes) const
{
	if (p < ram_base || p > ram_base + RAM_SIZE - bytes)
		return NONE;
	return (uint32_t) (p - ram_base);
}

void
Elan::update ()
{
	update_matrix ();
	update_gmp ();
	lights_changed = true;
}

void
Elan::update_matrix ()
{
	if (s.instance == NONE || s.instance > RAM_SIZE - SIZE_MATRIX) {
		/* None: the vertices as they are */
		static const float view[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
		static const float normals[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

		memcpy (matrix, view, sizeof (matrix));
		memcpy (normal_matrix, normals, sizeof (normal_matrix));
		env_u = env_v = 0.0f;
		return;
	}
	const uint32_t *w = (const uint32_t *) (ram_base + s.instance);

	/* x and z the other way, the view looking down its negative z */
	matrix[0] = -to_float (w[26]); matrix[1] = -to_float (w[29]);
	matrix[2] = -to_float (w[32]); matrix[3] = -to_float (w[35]);
	matrix[4] = to_float (w[27]);  matrix[5] = to_float (w[30]);
	matrix[6] = to_float (w[33]);  matrix[7] = to_float (w[36]);
	matrix[8] = -to_float (w[28]); matrix[9] = -to_float (w[31]);
	matrix[10] = -to_float (w[34]); matrix[11] = -to_float (w[37]);

	normal_matrix[0] = to_float (w[10]); normal_matrix[1] = to_float (w[11]); normal_matrix[2] = to_float (w[12]);
	normal_matrix[3] = to_float (w[13]); normal_matrix[4] = to_float (w[14]); normal_matrix[5] = to_float (w[15]);
	normal_matrix[6] = to_float (w[16]); normal_matrix[7] = to_float (w[17]); normal_matrix[8] = to_float (w[18]);

	near_plane = to_float (w[25]);
	far_plane = to_float (w[38]);
	env_u = to_float (w[9]);
	env_v = to_float (w[19]);
}

void
Elan::update_gmp ()
{
	memset (gmp_diffuse, 0, sizeof (gmp_diffuse));
	memset (gmp_specular, 0, sizeof (gmp_specular));
	gmp_params = 0;
	ls.gloss = 0.0f;
	ls_changes++;
	if (s.gmp == NONE || s.gmp > RAM_SIZE - SIZE_GMP)
		return;

	const uint32_t *w = (const uint32_t *) (ram_base + s.gmp);
	unsigned frac = w[1] & 31, exp = (w[1] >> 5) & 7;

	gmp_params = w[2];
	ls.gloss = powf (2.0f, (float) exp - 1.0f) * (1.0f + (float) frac / 32.0f);
	if (gmp_params & GMP_D0)
		unpack (w[3], gmp_diffuse);
	if (gmp_params & GMP_S0)
		unpack (w[4], gmp_specular);
}

/* The lights of the first volume, read when a polygon first wants them */
void
Elan::update_lights ()
{
	unsigned i;

	if (!lights_changed)
		return;
	lights_changed = false;
	ls_changes++;
	ls.count = 0;
	ls.use_base_over = false;
	ls.ambient_material_base = ls.ambient_material_offset = false;
	memset (ls.ambient_base, 0, sizeof (ls.ambient_base));
	memset (ls.ambient_offset, 0, sizeof (ls.ambient_offset));
	if (s.light_model == NONE || s.light_model > RAM_SIZE - SIZE_LIGHT)
		return;

	const uint32_t *m = (const uint32_t *) (ram_base + s.light_model);
	uint32_t diffuse_mask = m[2] & 0xFFFF, specular_mask = m[2] >> 16;

	ls.ambient_material_base = (m[1] >> 5) & 1;
	ls.ambient_material_offset = (m[1] >> 6) & 1;
	ls.use_base_over = (m[1] >> 9) & 1;
	unpack (m[3], ls.ambient_base);
	unpack (m[4], ls.ambient_offset);

	for (i = 0; i < MAX_LIGHTS; i++) {
		Light &l = ls.lights[ls.count];

		l.diffuse = (diffuse_mask >> i) & 1;
		l.specular = (specular_mask >> i) & 1;
		if ((!l.diffuse && !l.specular) || s.lights[i] == NONE || s.lights[i] > RAM_SIZE - SIZE_LIGHT)
			continue;

		const uint32_t *w = (const uint32_t *) (ram_base + s.lights[i]);
		bool parallel = (w[0] >> 20) & 1;

		l.color[0] = (float) ((w[1] >> 24) & 255) / 255.0f;
		l.color[1] = (float) ((w[1] >> 16) & 255) / 255.0f;
		l.color[2] = (float) ((w[1] >> 8) & 255) / 255.0f;
		l.routing = (w[2] >> 24) & 15;
		/* Pointing to the light */
		l.direction[0] = -light_axis (w[2] >> 16, w[0] >> 16);
		l.direction[1] = -light_axis (w[2] >> 8, w[0] >> 4);
		l.direction[2] = -light_axis (w[2], w[0]);
		l.parallel = parallel;
		if (parallel) {
			l.dmode = (w[2] >> 28) & 3;
			l.smode = LMODE_SINGLE_SIDED;
		} else {
			l.dmode = (w[1] >> 5) & 7;
			l.smode = (w[2] >> 28) & 3;
			/* With no position nor attenuation, they are parallel */
			l.parallel = !w[3] && !w[4] && !w[5] && !w[6] && !w[7];
			l.position[0] = to_float (w[3]);
			l.position[1] = to_float (w[4]);
			l.position[2] = to_float (w[5]);
			l.dist_mode = (w[2] >> 31) & 1;
			l.dist_a = short_float (w[6]);
			l.dist_b = short_float (w[6] >> 16);
			l.angle_a = short_float (w[7]);
			l.angle_b = short_float (w[7] >> 16);
		}
		if (l.parallel)
			normalize3 (l.direction);
		{
			unsigned n = ls.count;
			const float *f[NUM_LS] = {
				&l.direction[0], &l.direction[1], &l.direction[2], &l.position[0], &l.position[1],
				&l.position[2], &l.color[0], &l.color[1], &l.color[2], &l.dist_a, &l.dist_b,
				&l.angle_a, &l.angle_b,
			};

			for (unsigned k = 0; k < NUM_LS; k++)
				ls.soa[k][n] = *f[k];
			ls.masks[LM_PARALLEL][n] = l.parallel ? ~0u : 0;
			ls.masks[LM_DIST_MODE][n] = l.dist_mode ? ~0u : 0;
			ls.masks[LM_HAS_DIST][n] = (l.dist_a != 1.0f || l.dist_b != 0.0f) ? ~0u : 0;
			ls.masks[LM_HAS_ANGLE][n] = (l.angle_a != 1.0f || l.angle_b != 0.0f) ? ~0u : 0;
		}
		ls.count++;
	}
}

/*
 * The Lighting
 *
 * A list's vertices are not lit as they come: each is kept with the lights
 * of its time (a LightSet, once for all those of the same lights), its
 * triangles sent to the Holly with their colours to come, and the clipped
 * edges' points with the vertices they are between. Before the Holly draws a
 * scene or starts another (flush_lighting ()), all of them are lit on several
 * threads, each vertex alone (the same colours whatever the threads), the
 * clipped points worked out from them, and the scenes' vertices given their
 * colours: Club Kart's start, some 57000 vertices of 6 lights a frame in 100
 * lists of 600 or so, took half the frame on one thread. Nothing else of the
 * Holly's reads the colours before it draws them (its batches, its culling,
 * its depths are the positions').
 *
 * With ELAN_LIGHTING GPU, all that is left to the renderer instead, with the
 * scene (Scene::lighting): its compute shaders light the vertices in the same
 * way when it draws the scene, the processor's work only to keep them.
 */

/* A cosine to the gloss's power: none when the light is behind and all
 * when straight without powf, as powf gives them (the gloss above 0) */
static inline float
power01 (float x, float gloss)
{
	if (x <= 0.0f)
		return 0.0f;
	if (x >= 1.0f)
		return 1.0f;
	return powf (x, gloss);
}

/* SSE: f where the mask is set, else g */
static inline __m128
select4 (__m128 mask, __m128 f, __m128 g)
{
	return _mm_or_ps (_mm_and_ps (mask, f), _mm_andnot_ps (mask, g));
}

/* clamp01 (), its comparisons the same (a NaN goes through) */
static inline __m128
clamp4 (__m128 f)
{
	const __m128 zero = _mm_setzero_ps (), one = _mm_set1_ps (1.0f);

	return select4 (_mm_cmplt_ps (f, zero), zero, select4 (_mm_cmpgt_ps (f, one), one, f));
}

/* dot3 (), its sums in the same order */
static inline __m128
dot4 (__m128 ax, __m128 ay, __m128 az, __m128 bx, __m128 by, __m128 bz)
{
	return _mm_add_ps (_mm_add_ps (_mm_mul_ps (ax, bx), _mm_mul_ps (ay, by)), _mm_mul_ps (az, bz));
}

/* Each light's attenuated colour and its cosines to the normal and to the
 * reflected eye, four lights at a time: the same operations as one at a
 * time, in the same order (the sums stay a light at a time, in the lights'
 * order: light ()) */
void
Elan::light_terms (const LightSet &set, const float *pos, const float *normal, const float *reflected,
                   float (*color)[MAX_LIGHTS], float *to_normal, float *to_reflected)
{
	const __m128 zero = _mm_setzero_ps (), one = _mm_set1_ps (1.0f);
	const __m128 vx = _mm_set1_ps (pos[0]), vy = _mm_set1_ps (pos[1]), vz = _mm_set1_ps (pos[2]);
	const __m128 nx = _mm_set1_ps (normal[0]), ny = _mm_set1_ps (normal[1]), nz = _mm_set1_ps (normal[2]);
	const __m128 rx = _mm_set1_ps (reflected[0]), ry = _mm_set1_ps (reflected[1]), rz = _mm_set1_ps (reflected[2]);
	unsigned i;

	for (i = 0; i < set.count; i += 4) {
		auto field = [&] (unsigned f) { return _mm_loadu_ps (&set.soa[f][i]); };
		auto mask = [&] (unsigned m) { return _mm_castsi128_ps (_mm_loadu_si128 ((const __m128i *) &set.masks[m][i])); };
		const __m128 parallel = mask (LM_PARALLEL);
		const __m128 dx = field (LS_DX), dy = field (LS_DY), dz = field (LS_DZ);
		/* To the light: its direction, or from the vertex to it,
		 * normalised (normalize3 ()) */
		__m128 tx = select4 (parallel, dx, _mm_sub_ps (field (LS_PX), vx));
		__m128 ty = select4 (parallel, dy, _mm_sub_ps (field (LS_PY), vy));
		__m128 tz = select4 (parallel, dz, _mm_sub_ps (field (LS_PZ), vz));
		const __m128 dist = _mm_sqrt_ps (dot4 (tx, ty, tz, tx, ty, tz));
		const __m128 norm = _mm_andnot_ps (parallel, _mm_cmpgt_ps (dist, zero));

		tx = select4 (norm, _mm_div_ps (tx, dist), tx);
		ty = select4 (norm, _mm_div_ps (ty, dist), ty);
		tz = select4 (norm, _mm_div_ps (tz, dist), tz);

		/* By the distance, then the angle: a point's light */
		__m128 kd = clamp4 (_mm_add_ps (_mm_mul_ps (field (LS_DIST_B),
		                                           select4 (mask (LM_DIST_MODE), dist, _mm_div_ps (one, dist))),
		                                field (LS_DIST_A)));
		const __m128 to_axis = dot4 (tx, ty, tz, dx, dy, dz);
		const __m128 cosine = _mm_sub_ps (one, select4 (_mm_cmplt_ps (zero, to_axis), to_axis, zero));
		__m128 ka = clamp4 (_mm_add_ps (_mm_mul_ps (cosine, field (LS_ANGLE_B)), field (LS_ANGLE_A)));

		kd = select4 (_mm_andnot_ps (parallel, mask (LM_HAS_DIST)), kd, one);
		ka = select4 (_mm_andnot_ps (parallel, mask (LM_HAS_ANGLE)), ka, one);
		for (unsigned c = 0; c < 3; c++)
			_mm_storeu_ps (&color[c][i], _mm_mul_ps (_mm_mul_ps (field (LS_R + c), kd), ka));
		_mm_storeu_ps (&to_normal[i], dot4 (nx, ny, nz, tx, ty, tz));
		_mm_storeu_ps (&to_reflected[i], dot4 (tx, ty, tz, rx, ry, rz));
	}
}

/* A vertex lit with its lights, then the ambient, the colours clamped and an
 * untextured one's offset added to its base, as the screen takes them */
void
Elan::light (const LightSet &set, LitVertex &v)
{
	float diffuse[3] = { 0, 0, 0 }, specular[3] = { 0, 0, 0 };
	float diffuse_alpha = 0.0f, specular_alpha = 0.0f;
	float eye[3] = { v.pos[0], v.pos[1], v.pos[2] }, reflected[3], d;
	/* light_terms ()'s (the lights four at a time: MAX_LIGHTS a multiple
	 * of 4) */
	alignas (16) float color[3][MAX_LIGHTS], to_normal[MAX_LIGHTS], to_reflected[MAX_LIGHTS];
	unsigned i, c;

	normalize3 (eye);
	d = 2.0f * dot3 (v.normal, eye);
	for (c = 0; c < 3; c++)
		reflected[c] = eye[c] - d * v.normal[c];
	light_terms (set, v.pos, v.normal, reflected, color, to_normal, to_reflected);

	/* No specular colour (a model without one): its terms add zeros, the
	 * sums never -0, so they are left out with their powf (a third of
	 * the lighting's time; a NaN's cosine aside) */
	const bool no_offset = v.offset[0] == 0.0f && v.offset[1] == 0.0f && v.offset[2] == 0.0f;

	for (i = 0; i < set.count; i++) {
		const Light &l = set.lights[i];

		if (l.diffuse) {
			float factor = (l.routing & ROUTE_SUB) ? -BASE_FACTOR : BASE_FACTOR;

			if (l.dmode == LMODE_SINGLE_SIDED)
				factor *= std::max (to_normal[i], 0.0f);
			else if (l.dmode == LMODE_DOUBLE_SIDED)
				factor *= fabsf (to_normal[i]);
			if (l.routing & ROUTE_ALPHA) {
				diffuse_alpha += color[0][i] * factor;
			} else {
				float *to = (l.routing & ROUTE_DIFF_TO_OFFSET) ? specular : diffuse;

				for (c = 0; c < 3; c++)
					to[c] += color[c][i] * factor * v.base[c];
			}
		}
		if (l.specular && !(no_offset && !(l.routing & ROUTE_ALPHA))) {
			float factor = (l.routing & ROUTE_SUB) ? -BASE_FACTOR : BASE_FACTOR;

			if (l.smode == LMODE_SINGLE_SIDED)
				factor *= clamp01 (power01 (std::max (to_reflected[i], 0.0f), set.gloss));
			else if (l.smode == LMODE_DOUBLE_SIDED)
				factor *= clamp01 (power01 (fabsf (to_reflected[i]), set.gloss));
			if (l.routing & ROUTE_ALPHA) {
				specular_alpha += color[0][i] * factor;
			} else {
				float *to = (l.routing & ROUTE_SPEC_TO_OFFSET) ? specular : diffuse;

				for (c = 0; c < 3; c++)
					to[c] += color[c][i] * factor * v.offset[c];
			}
		}
	}
	for (c = 0; c < 3; c++) {
		diffuse[c] += set.ambient_base[c] * (set.ambient_material_base ? v.base[c] : 1.0f);
		specular[c] += set.ambient_offset[c] * (set.ambient_material_offset ? v.offset[c] : 1.0f);
		v.base[c] = diffuse[c];
		v.offset[c] = specular[c];
	}
	v.base[3] += diffuse_alpha;
	v.offset[3] += specular_alpha;
	/* What is over 1 in the base goes to the offset */
	if (set.use_base_over)
		for (c = 0; c < 4; c++)
			v.offset[c] += std::max (v.base[c] - 1.0f, 0.0f);
	for (c = 0; c < 4; c++) {
		v.base[c] = clamp01 (v.base[c]);
		v.offset[c] = clamp01 (v.offset[c]);
	}
	if (v.add)
		for (c = 0; c < 4; c++)
			v.base[c] += v.offset[c];
}

uint32_t
Elan::light_set ()
{
	if (sets.empty () || ls_kept != ls_changes) {
		sets.push_back (ls);
		ls_kept = ls_changes;
	}
	return (uint32_t) sets.size () - 1;
}

/* flush_lighting ()'s threads: the vertices in chunks, each thread taking the
 * next as it is done, the caller too */
struct Elan::Workers {
	std::vector<std::thread> threads;
	std::mutex lock;
	std::condition_variable wake, done;
	std::function<void (size_t, size_t)> work;
	std::atomic<size_t> next { 0 };
	size_t total = 0, chunk = 1;
	unsigned running = 0;
	uint64_t round = 0;
	bool quit = false;

	explicit Workers (unsigned n)
	{
		for (unsigned i = 0; i < n; i++)
			threads.emplace_back ([this] { loop (); });
	}

	~Workers ()
	{
		{
			std::lock_guard<std::mutex> hold (lock);
			quit = true;
		}
		wake.notify_all ();
		for (std::thread &t : threads)
			t.join ();
	}

	void take ()
	{
		for (;;) {
			size_t at = next.fetch_add (chunk);

			if (at >= total)
				return;
			work (at, std::min (total, at + chunk));
		}
	}

	void loop ()
	{
		uint64_t seen = 0;
		std::unique_lock<std::mutex> hold (lock);

		for (;;) {
			wake.wait (hold, [&] { return quit || round != seen; });
			if (quit)
				return;
			seen = round;
			hold.unlock ();
			take ();
			hold.lock ();
			if (--running == 0)
				done.notify_one ();
		}
	}

	void run (size_t n, size_t chunk_, std::function<void (size_t, size_t)> fn)
	{
		{
			std::lock_guard<std::mutex> hold (lock);
			work = std::move (fn);
			total = n;
			chunk = chunk_;
			next = 0;
			running = (unsigned) threads.size ();
			round++;
		}
		wake.notify_all ();
		take ();
		std::unique_lock<std::mutex> hold (lock);
		done.wait (hold, [&] { return running == 0; });
	}
};

Elan::~Elan ()
{
}

void
Elan::gpu_set (const LightSet &set, GpuLightSet &out)
{
	memset (&out, 0, sizeof (out));
	memcpy (out.ambient_base, set.ambient_base, sizeof (out.ambient_base));
	memcpy (out.ambient_offset, set.ambient_offset, sizeof (out.ambient_offset));
	out.gloss = set.gloss;
	out.count = set.count;
	out.flags = (set.use_base_over ? 1 : 0) | (set.ambient_material_base ? 2 : 0) |
	            (set.ambient_material_offset ? 4 : 0);
	for (unsigned i = 0; i < set.count; i++) {
		const Light &l = set.lights[i];
		GpuLight &g = out.lights[i];

		for (unsigned c = 0; c < 3; c++) {
			g.direction[c] = l.direction[c];
			g.position[c] = l.position[c];
			g.color[c] = l.color[c];
		}
		g.direction[3] = l.parallel ? 1.0f : 0.0f;
		g.position[3] = l.dist_mode ? 1.0f : 0.0f;
		g.atten[0] = l.dist_a;
		g.atten[1] = l.dist_b;
		g.atten[2] = l.angle_a;
		g.atten[3] = l.angle_b;
		g.flags = (l.diffuse ? 1 : 0) | (l.specular ? 2 : 0) | (set.masks[LM_HAS_DIST][i] ? 4 : 0) |
		          (set.masks[LM_HAS_ANGLE][i] ? 8 : 0);
		g.routing = (int32_t) l.routing;
		g.dmode = (int32_t) l.dmode;
		g.smode = (int32_t) l.smode;
	}
}

void
Elan::flush_lighting ()
{
	/* Fewer on one thread: waking the others would cost more */
	static const size_t SPREAD_LEAST = 2048, CHUNK = 256;

	if (lit.empty () && patches.empty ())
		return;
	HL_PROFILE (ELAN);
	if (hl::Config::get ()->elan_lighting == hl::ELAN_LIGHTING_GPU && gpu_lighting.load ()) {
		/* Left to the renderer: each scene its vertices to colour, with
		 * all the lit ones and the clipped points (as they are, when the
		 * scene has none yet: the vectors swapped with its empty ones) */
		for (size_t k = 0; k < scenes.size (); k++) {
			SceneLighting &l = scenes[k]->lighting;
			size_t from = scene_from[k], to = k + 1 < scenes.size () ? scene_from[k + 1] : patches.size ();
			uint32_t set_at = (uint32_t) l.sets.size ();
			int32_t lit_at = (int32_t) l.lit.size (), clip_at = (int32_t) l.clips.size ();
			size_t s0 = l.sets.size ();

			l.sets.resize (s0 + sets.size ());
			for (size_t i = 0; i < sets.size (); i++)
				gpu_set (sets[i], l.sets[s0 + i]);
			if (scenes.size () == 1 && l.empty () && l.lit.empty () && l.clips.empty () && set_at == 0) {
				l.lit.swap (lit);
				l.clips.swap (clips);
				l.refs.swap (patches);
				continue;
			}
			for (const LitVertex &v : lit) {
				l.lit.push_back (v);
				l.lit.back ().set += set_at;
			}
			for (const ClipColor &c : clips) {
				l.clips.push_back (c);
				if (c.a >= 0)
					l.clips.back ().a += lit_at;
				if (c.b >= 0)
					l.clips.back ().b += lit_at;
			}
			for (size_t i = from; i < to; i++) {
				ColorRef r = patches[i];

				r.ref = r.ref >= 0 ? r.ref + lit_at : r.ref - clip_at;
				l.refs.push_back (r);
			}
		}
		drop_lighting ();
		return;
	}
	if (lit.size () >= SPREAD_LEAST) {
		if (!workers) {
			/* The machine's thread lights too; the drawing thread and
			 * the others' keep theirs */
			unsigned cores = std::thread::hardware_concurrency ();

			workers.reset (new Workers (cores > 3 ? std::min (cores - 3, 6u) : 0));
		}
		workers->run (lit.size (), CHUNK, [this] (size_t from, size_t to) {
			for (size_t i = from; i < to; i++)
				light (sets[lit[i].set], lit[i]);
		});
	} else
		for (LitVertex &v : lit)
			light (sets[v.set], v);

	/* clip_point ()'s colours, from the lit ones */
	for (ClipColor &k : clips) {
		const float *base[2], *offset[2];
		int ends[2] = { k.a, k.b };
		unsigned c;

		for (int e = 0; e < 2; e++) {
			base[e] = ends[e] >= 0 ? lit[ends[e]].base : k.base[e];
			offset[e] = ends[e] >= 0 ? lit[ends[e]].offset : k.offset[e];
		}
		for (c = 0; c < 4; c++) {
			k.out_base[c] = base[0][c] + (base[1][c] - base[0][c]) * k.t;
			k.out_offset[c] = offset[0][c] + (offset[1][c] - offset[0][c]) * k.t;
		}
	}
	/* The scenes' vertices, each its own too */
	auto colour = [this] (size_t from, size_t to) {
		size_t k = std::upper_bound (scene_from.begin (), scene_from.end (), from) - scene_from.begin () - 1;

		for (size_t i = from; i < to; i++) {
			const ColorRef &p = patches[i];
			const float *base, *offset;

			while (k + 1 < scene_from.size () && scene_from[k + 1] <= i)
				k++;

			Vertex &v = scenes[k]->vertices[p.at];

			if (p.ref >= 0) {
				base = lit[p.ref].base;
				offset = lit[p.ref].offset;
			} else {
				base = clips[-2 - p.ref].out_base;
				offset = clips[-2 - p.ref].out_offset;
			}
			v.base = pack (base);
			v.offset = pack (offset);
		}
	};
	if (workers && patches.size () >= SPREAD_LEAST)
		workers->run (patches.size (), CHUNK * 4, colour);
	else
		colour (0, patches.size ());
	drop_lighting ();
}

void
Elan::drop_lighting ()
{
	sets.clear ();
	lit.clear ();
	clips.clear ();
	patches.clear ();
	scenes.clear ();
	scene_from.clear ();
}

/*
 * The Screen
 */

void
Elan::project (Transformed &v) const
{
	float z = v.pos[2];

	v.screen.x = s.proj[0] * v.pos[0] / z + s.proj[1];
	v.screen.y = -s.proj[2] * v.pos[1] / z + s.proj[3];
	v.screen.z = z != 0.0f ? -1.0f / z : 0.0f;
	v.screen.u = v.u;
	v.screen.v = v.v;
	v.screen.base = pack (v.base);
	v.screen.offset = pack (v.offset);
}

/* Where the edge from a to b, a inside, crosses the near plane */
void
Elan::clip_point (const Transformed &a, float da, const Transformed &b, float db, Transformed &out)
{
	float t = fabsf (da) / (fabsf (da) + fabsf (db));
	unsigned c;

	for (c = 0; c < 3; c++)
		out.pos[c] = a.pos[c] + (b.pos[c] - a.pos[c]) * t;
	out.u = a.u + (b.u - a.u) * t;
	out.v = a.v + (b.v - a.v) * t;
	for (c = 0; c < 4; c++) {
		out.base[c] = a.base[c] + (b.base[c] - a.base[c]) * t;
		out.offset[c] = a.offset[c] + (b.offset[c] - a.offset[c]) * t;
	}
	out.ref = out.fref = -1;
	if (a.fref != -1 || b.fref != -1) {
		/* Between colours to come: worked out with them */
		ClipColor &k = clips.emplace_back ();

		k.a = a.fref;
		k.b = b.fref;
		k.t = t;
		memcpy (k.base[0], a.base, sizeof (k.base[0]));
		memcpy (k.offset[0], a.offset, sizeof (k.offset[0]));
		memcpy (k.base[1], b.base, sizeof (k.base[1]));
		memcpy (k.offset[1], b.offset, sizeof (k.offset[1]));
		out.ref = out.fref = -2 - (int) (clips.size () - 1);
	}
	project (out);
}

void
Elan::triangle (const Transformed &a, const Transformed &b, const Transformed &c, bool clip)
{
	Holly *holly = board->holly;

	if (clip) {
		const Transformed *in[3] = { &a, &b, &c };
		float dist[3];
		Transformed out[4];
		unsigned i, n = 0;

		for (i = 0; i < 3; i++)
			dist[i] = -in[i]->pos[2] - near_plane;
		if (dist[0] < 0.0f || dist[1] < 0.0f || dist[2] < 0.0f) {
			if (dist[0] < 0.0f && dist[1] < 0.0f && dist[2] < 0.0f)
				return;
			for (i = 0; i < 3; i++) {
				unsigned j = (i + 1) % 3;

				if (dist[i] >= 0.0f)
					out[n++] = *in[i];
				if ((dist[i] < 0.0f) != (dist[j] < 0.0f))
					clip_point (*in[i], dist[i], *in[j], dist[j], out[n++]);
			}
			emit (out[0], out[1], out[2]);
			if (n == 4)
				emit (out[0], out[2], out[3]);
			return;
		}
	}
	emit (a, b, c);
}

void
Elan::emit (const Transformed &a, const Transformed &b, const Transformed &c)
{
	Holly *holly = board->holly;
	int at = holly->elan_triangle (a.screen, b.screen, c.screen);

	if (at < 0 || (a.ref == -1 && b.ref == -1 && c.ref == -1))
		return;
	/* Flat, the Holly's three vertices have the last one's colours */
	int refs[3] = { list_gouraud ? a.ref : c.ref, list_gouraud ? b.ref : c.ref, c.ref };
	Scene *scene = holly->elan_scene ().get ();

	if (scenes.empty () || scenes.back ().get () != scene) {
		scenes.push_back (holly->elan_scene ());
		scene_from.push_back (patches.size ());
	}
	for (int k = 0; k < 3; k++)
		if (refs[k] != -1)
			patches.push_back ({ (uint32_t) (at + k), refs[k] });
}

/*
 * The Lists of Vertices
 */

static unsigned
vertex_words (uint32_t flags)
{
	switch (flags) {
	case VTX_V:   return 4;
	case VTX_VU:  return 6;
	case VTX_VNU: return 10;
	case VTX_VR:  return 6;
	case VTX_VUR: return 8;
	case VTX_VUB: return 10;
	default:      return 0;
	}
}

void
Elan::view_position (const uint32_t *w, float *out) const
{
	float x = to_float (w[1]), y = to_float (w[2]), z = to_float (w[3]);

	out[0] = matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3];
	out[1] = matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7];
	out[2] = matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11];
}

/* False when the list is wholly out of the near and far planes; clip, when it
 * crosses the near one */
bool
Elan::in_depth_range (const uint32_t *w, uint32_t count, unsigned words, bool *clip) const
{
	float lo = 1e38f, hi = -1e38f, low[3], high[3], extent, center;
	uint32_t i;
	unsigned c;

	for (c = 0; c < 3; c++) {
		low[c] = 1e38f;
		high[c] = -1e38f;
	}
	for (i = 0; i < count; i++, w += words) {
		for (c = 0; c < 3; c++) {
			float p = to_float (w[c + 1]);

			low[c] = std::min (low[c], p);
			high[c] = std::max (high[c], p);
		}
	}
	/* The depth of the box the list lies in */
	center = matrix[8] * (low[0] + high[0]) * 0.5f + matrix[9] * (low[1] + high[1]) * 0.5f +
	         matrix[10] * (low[2] + high[2]) * 0.5f + matrix[11];
	extent = fabsf (matrix[8]) * (high[0] - low[0]) * 0.5f + fabsf (matrix[9]) * (high[1] - low[1]) * 0.5f +
	         fabsf (matrix[10]) * (high[2] - low[2]) * 0.5f;
	lo = center - extent;
	hi = center + extent;
	if (lo != lo || hi != hi)
		return false;
	if (lo > -near_plane || hi < -far_plane)
		return false;
	*clip = hi > -near_plane;
	return true;
}

/* A modifier volume's triangle, cut along the near plane; what is wholly before
 * it is flattened on it, for the volume to remain closed */
void
Elan::modifier_triangle (const float *a, const float *b, const float *c)
{
	Holly *holly = board->holly;
	const float *in[3] = { a, b, c };
	float dist[3], cut[2][3];
	bool above[3];
	unsigned i, lone = 3;

	for (i = 0; i < 3; i++) {
		dist[i] = -in[i][2] - near_plane;
		above[i] = dist[i] >= 0.0f;
	}
	if (above[0] != above[1] || above[0] != above[2]) {
		if (above[0] == above[2])
			lone = 1;
		else if (above[0] == above[1])
			lone = 2;
		else
			lone = 0;
	}
	auto send = [&] (const float *p, const float *q, const float *r) {
		const float *v[3] = { p, q, r };
		float out[9];
		unsigned n;

		for (n = 0; n < 3; n++) {
			float z = std::min (v[n][2], -0.001f);

			out[n * 3] = s.proj[0] * v[n][0] / z + s.proj[1];
			out[n * 3 + 1] = -s.proj[2] * v[n][1] / z + s.proj[3];
			out[n * 3 + 2] = -1.0f / z;
		}
		holly->elan_mod_triangle (out);
	};
	if (lone == 3) {
		send (a, b, c);
		return;
	}
	{
		const float *v0 = in[lone], *v1 = in[(lone + 1) % 3], *v2 = in[(lone + 2) % 3];
		float d0 = fabsf (dist[lone]), d1 = fabsf (dist[(lone + 1) % 3]), d2 = fabsf (dist[(lone + 2) % 3]);

		for (i = 0; i < 3; i++) {
			cut[0][i] = (v0[i] * d1 + v1[i] * d0) / (d0 + d1);
			cut[1][i] = (v0[i] * d2 + v2[i] * d0) / (d0 + d2);
		}
		send (v0, cut[0], cut[1]);
		send (cut[0], v1, cut[1]);
		send (v2, cut[1], v1);
	}
}

void
Elan::modifier_list (const uint32_t *ich, const uint32_t *w, unsigned words)
{
	Holly *holly = board->holly;
	uint32_t pcw = ich[0], isp = ich[1], count = ich[7], i, first = 0;
	float prev[2][3] = {}, now[3];

	if (!open_volume)
		isp &= ~(3u << 27);
	isp = (isp & ~(1u << 26)) | (((pcw >> 6) & 1) << 26);
	isp &= ~(1u << 31);	/* the volume's mode has two bits */
	holly->elan_modifier ((pcw >> 24) & 7, isp);

	for (i = 0; i < count; i++, w += words) {
		uint32_t n = i - first;

		view_position (w, now);
		if (n >= 2) {
			if (n & 1)
				modifier_triangle (prev[1], prev[0], now);
			else
				modifier_triangle (prev[0], prev[1], now);
		}
		if (w[0] >> 31)
			first = i + 1;
		memcpy (prev[0], prev[1], sizeof (prev[0]));
		memcpy (prev[1], now, sizeof (now));
	}
}

void
Elan::polygon_list (const uint32_t *ich)
{
	Holly *holly = board->holly;
	const uint32_t *w = ich + 8;
	uint32_t pcw = ich[0], isp = ich[1], tsp = ich[2], tcw = ich[3], flags = ich[6], count = ich[7], i;
	unsigned words = vertex_words (flags);
	bool textured, offset = (pcw >> 2) & 1, gouraud = (pcw >> 1) & 1, shadow = (pcw >> 7) & 1;
	bool env0 = false, env_any = false, constant = false, clip = false, bump, has_uv, has_rgb;
	unsigned uv_at = 4, rgb_at = 4;

	/* Not one the chip's lists are known to use */
	if (!words || flags == VTX_VNU) {
		HL_ERROR_FEW ("elan: vertices of format %X", flags);
		return;
	}
	has_uv = flags != VTX_V && flags != VTX_VR;
	has_rgb = flags == VTX_VR || flags == VTX_VUR;
	rgb_at = flags == VTX_VUR ? 6 : 4;

	if (flags == VTX_V || flags == VTX_VU) {
		int list = holly->elan_list ();

		if (list < 0)
			list = (pcw >> 24) & 7;
		if (list & 1) {
			modifier_list (ich, w, words);
			return;
		}
	}
	if (!in_depth_range (w, count, words, &clip))
		return;

	textured = has_uv && ((pcw >> 3) & 1);
	if (flags == VTX_VUB) {
		textured = true;
		offset = false;
	}
	update_lights ();
	if (s.gmp != NONE) {
		constant = gmp_params & GMP_B0;
		if (gmp_params & GMP_E0) {
			textured = true;
			offset = false;
			tsp = (tsp | TSP_USE_ALPHA) & ~TSP_IGNORE_TEX_A;
			env0 = true;
			env_any = true;
		}
		if (gmp_params & GMP_E1)
			env_any = true;
	}
	/* The bump maps' shading is the core's: their colours are left alone */
	bump = textured && ((tcw >> 27) & 7) == 4;

	tsp ^= model_tsp;
	if ((culling_reversed ? 1 : 0) ^ (std::signbit (-s.proj[0]) == std::signbit (s.proj[2]) ? 1 : 0))
		isp ^= ISP_CULL_LOW;
	shadow ^= shadowed_volume;
	isp = (isp & 0xFC3FFFFF) | ((textured ? 1u : 0u) << 25) | ((offset ? 1u : 0u) << 24) | ((gouraud ? 1u : 0u) << 23);

	{
		int list = holly->elan_list ();

		holly->elan_polygon (list < 0 ? (pcw >> 24) & 7 : (unsigned) list, isp, tsp, tcw, shadow);
	}

	list_gouraud = gouraud;
	Transformed center = {}, p = {}, q = {}, v = {};
	unsigned n = 0;
	bool start = true, odd = false;

	for (i = 0; i < count; i++, w += words) {
		uint32_t header = w[0];
		float normal[3], in_n[3], base_in[4] = { 1, 1, 1, 1 };
		unsigned c;

		view_position (w, v.pos);
		v.u = v.v = 0.0f;
		if (env_any) {
			v.u = env_u;
			v.v = env_v;
		} else if (has_uv) {
			v.u = to_float (w[uv_at]);
			v.v = to_float (w[uv_at + 1]);
		}
		if (has_rgb)
			unpack (w[rgb_at], base_in);
		memcpy (v.base, base_in, sizeof (v.base));
		memset (v.offset, 0, sizeof (v.offset));
		if (gmp_params & GMP_D0)
			memcpy (v.base, gmp_diffuse, sizeof (v.base));
		if (gmp_params & GMP_S0)
			memcpy (v.offset, gmp_specular, sizeof (v.offset));

		in_n[0] = (float) (int8_t) header / 127.0f;
		in_n[1] = (float) (int8_t) (header >> 8) / 127.0f;
		in_n[2] = (float) (int8_t) (header >> 16) / 127.0f;
		for (c = 0; c < 3; c++)
			normal[c] = normal_matrix[c * 3] * in_n[0] + normal_matrix[c * 3 + 1] * in_n[1] +
			            normal_matrix[c * 3 + 2] * in_n[2];
		normalize3 (normal);

		if (bump)
			memset (v.offset, 0, sizeof (v.offset));
		v.ref = v.fref = -1;
		if (!bump && !constant) {
			/* Lit later (flush_lighting ()), its offset added then */
			LitVertex &l = lit.emplace_back ();

			memcpy (l.pos, v.pos, sizeof (l.pos));
			memcpy (l.normal, normal, sizeof (l.normal));
			memcpy (l.base, v.base, sizeof (l.base));
			memcpy (l.offset, v.offset, sizeof (l.offset));
			l.set = light_set ();
			l.add = !textured;
			v.ref = v.fref = (int) lit.size () - 1;
		} else if (!textured)
			for (c = 0; c < 4; c++)
				v.base[c] += v.offset[c];
		if (env0) {
			v.u = clamp01 (v.u + normal[0] * 0.5f + 0.5f);
			v.v = clamp01 (v.v + normal[1] * 0.5f + 0.5f);
		}
		project (v);

		if (start) {
			center = v;
			n = 0;
			odd = false;
			start = false;
		}
		if (n >= 1 && !((header >> 29) & 1) && ((header >> 30) & 1)) {
			/* A fan's: around the first vertex */
			triangle (center, q, v, clip);
			p = q;
			q = v;
			odd = true;
		} else {
			if (n >= 2) {
				if (odd) {
					/* Flat shading: the last vertex's colour */
					Transformed last = q;

					if (!gouraud) {
						last.screen.base = v.screen.base;
						last.screen.offset = v.screen.offset;
						last.ref = v.ref;
					}
					triangle (p, v, last, clip);
				} else
					triangle (p, q, v, clip);
				odd = !odd;
			}
			p = q;
			q = v;
		}
		n++;
		if (header >> 31)
			start = true;
	}
}

/*
 * The Commands
 */

bool
Elan::fail ()
{
	s.reg74 |= 0x12;
	HL_ERROR_FEW ("elan: invalid commands");
	return false;
}

bool
Elan::run (const uint8_t *data, int64_t size, unsigned depth)
{
	Holly *holly = board->holly;
	const uint8_t *limit = (data >= ram_base && data < ram_base + RAM_SIZE) ? ram_base + RAM_SIZE :
	                       (const uint8_t *) s.cmd + sizeof (s.cmd);

	while (size >= 32) {
		const uint32_t *w = (const uint32_t *) data;
		uint32_t pcw = w[0];
		uint64_t used = 32;

		if (++commands > MAX_COMMANDS)
			return fail ();
		if (holly->elan_wants_half () || !(pcw & PCW_NAOMI2)) {
			holly->elan_packet (w);
			data += 32;
			size -= 32;
			continue;
		}
		switch ((pcw >> 8) & 15) {
		case CMD_NULL:
			break;
		case CMD_PROJECTION:
			s.proj[0] = to_float (w[2]);
			s.proj[1] = to_float (w[3]);
			s.proj[2] = to_float (w[4]);
			s.proj[3] = to_float (w[5]);
			break;
		case CMD_MATRIX_LIGHT:
			if (w[1] == 0xF && w[2] == 0x7F) {
				s.instance = ram_offset (data, SIZE_MATRIX);
				update_matrix ();
				used = SIZE_MATRIX;
			} else if (w[1] & 0x10) {
				s.light_model = ram_offset (data, SIZE_LIGHT);
				lights_changed = true;
			} else {
				s.lights[w[1] & 15] = ram_offset (data, SIZE_LIGHT);
				lights_changed = true;
			}
			break;
		case CMD_GMP:
			s.gmp = ram_offset (data, SIZE_GMP);
			update_gmp ();
			used = SIZE_GMP;
			break;
		case CMD_ICH: {
			unsigned words = vertex_words (w[6]);

			used = 32 + (uint64_t) words * 4 * w[7];
			if (used > (uint64_t) (limit - data))
				return fail ();
			if (words)
				polygon_list (w);
			break;
		}
		case CMD_MODEL: {
			uint32_t at = w[4] & 0x01FFFFF8;
			uint32_t bytes = std::min (w[6], RAM_SIZE - at);
			bool ok;

			if (depth >= MAX_DEPTH)
				return fail ();
			culling_reversed = !((w[1] >> 27) & 1);
			open_volume = (w[1] >> 28) & 1;
			shadowed_volume = (pcw >> 7) & 1;
			model_tsp = w[2];
			holly->elan_clip_mode ((pcw >> 16) & 3);
			ok = run (ram_base + at, bytes, depth + 1);
			culling_reversed = open_volume = shadowed_volume = false;
			model_tsp = 0;
			if (!ok)
				return false;
			break;
		}
		case CMD_WAIT:
			if (w[1] != NONE && w[3] != 0) {
				/* The bits of the Holly's end of list it waits on */
				switch (w[3]) {
				case 0x80:
				case 0x100:
				case 0x200:
				case 0x400:
				case 0x200000:
					break;
				default:
					return fail ();
				}
				holly->elan_end_of_list (w[3]);
				clear_state ();
			}
			break;
		case CMD_LINK: {
			uint32_t offset = w[1], bytes = w[3];

			if (offset & 0xA0000000) {
				/* A texture for the VRAM, from the SH-4's RAM at the
				 * channel 2 DMA's source, or from the Elan's */
				uint32_t to = w[2] & VRAM_MASK, from;
				const uint8_t *source;

				if (bytes > VRAM_SIZE)
					return fail ();
				bytes = std::min (bytes, VRAM_SIZE - to);
				s.dma_end = NONE;
				if (offset & 0x80000000) {
					uint32_t start = ((Sh4 *) board->sh)->ddt_source (2);

					from = start & SH_RAM_MASK;
					bytes = std::min (bytes, SH_RAM_MASK + 1 - from);
					source = (const uint8_t *) board->ram->ptr (from);
					s.dma_end = start + bytes;
				} else {
					from = offset & RAM_MASK;
					bytes = std::min (bytes, RAM_SIZE - from);
					source = ram_base + from;
				}
				memcpy (holly->vram->ptr (to), source, bytes);
				s.dma_lines = dma_duration (bytes);
			} else {
				uint32_t at = offset & RAM_MASK;

				bytes = std::min (bytes, RAM_SIZE - at);
				if (size == 32) {
					/* The last of its list: no need to come back */
					data = ram_base + at;
					size = bytes;
					limit = ram_base + RAM_SIZE;
					continue;
				}
				if (depth >= MAX_DEPTH)
					return fail ();
				if (!run (ram_base + at, bytes, depth + 1))
					return false;
			}
			break;
		}
		default:
			return fail ();
		}
		data += used;
		size -= (int64_t) used;
	}
	return true;
}

/*
 * The States
 */

int
Elan::save_state (hl::State *state)
{
	return state->put ((void *) &s, sizeof (s));
}

int
Elan::load_state (hl::State *state)
{
	int ret = state->get ((void *) &s, sizeof (s));

	drop_lighting ();
	culling_reversed = open_volume = shadowed_volume = false;
	model_tsp = 0;
	update ();
	return ret;
}

} /* namespace naomi */
