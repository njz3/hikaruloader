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
#include <cstring>

#include "hl/core.h"
#include "hl/machine.h"
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
};

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
	if (run ((const uint8_t *) s.cmd, sizeof (s.cmd), 0) && !s.dma_lines)
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
	gloss = 0.0f;
	if (s.gmp == NONE || s.gmp > RAM_SIZE - SIZE_GMP)
		return;

	const uint32_t *w = (const uint32_t *) (ram_base + s.gmp);
	unsigned frac = w[1] & 31, exp = (w[1] >> 5) & 7;

	gmp_params = w[2];
	gloss = powf (2.0f, (float) exp - 1.0f) * (1.0f + (float) frac / 32.0f);
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
	light_count = 0;
	use_base_over = false;
	ambient_material_base = ambient_material_offset = false;
	memset (ambient_base, 0, sizeof (ambient_base));
	memset (ambient_offset, 0, sizeof (ambient_offset));
	if (s.light_model == NONE || s.light_model > RAM_SIZE - SIZE_LIGHT)
		return;

	const uint32_t *m = (const uint32_t *) (ram_base + s.light_model);
	uint32_t diffuse_mask = m[2] & 0xFFFF, specular_mask = m[2] >> 16;

	ambient_material_base = (m[1] >> 5) & 1;
	ambient_material_offset = (m[1] >> 6) & 1;
	use_base_over = (m[1] >> 9) & 1;
	unpack (m[3], ambient_base);
	unpack (m[4], ambient_offset);

	for (i = 0; i < MAX_LIGHTS; i++) {
		Light &l = lights[light_count];

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
		light_count++;
	}
}

/*
 * The Lighting
 */

void
Elan::light_vertex (Transformed &v, const float *normal) const
{
	float diffuse[3] = { 0, 0, 0 }, specular[3] = { 0, 0, 0 };
	float diffuse_alpha = 0.0f, specular_alpha = 0.0f;
	float eye[3] = { v.pos[0], v.pos[1], v.pos[2] }, reflected[3], d;
	unsigned i, c;

	normalize3 (eye);
	d = 2.0f * dot3 (normal, eye);
	for (c = 0; c < 3; c++)
		reflected[c] = eye[c] - d * normal[c];

	for (i = 0; i < light_count; i++) {
		const Light &l = lights[i];
		float to_light[3], color[3] = { l.color[0], l.color[1], l.color[2] };

		if (l.parallel) {
			memcpy (to_light, l.direction, sizeof (to_light));
		} else {
			float dist;

			for (c = 0; c < 3; c++)
				to_light[c] = l.position[c] - v.pos[c];
			dist = sqrtf (dot3 (to_light, to_light));
			normalize3 (to_light);
			if (l.dist_a != 1.0f || l.dist_b != 0.0f) {
				float k = clamp01 (l.dist_b * (l.dist_mode ? dist : 1.0f / dist) + l.dist_a);

				for (c = 0; c < 3; c++)
					color[c] *= k;
			}
			if (l.angle_a != 1.0f || l.angle_b != 0.0f) {
				float cosine = 1.0f - std::max (0.0f, dot3 (to_light, l.direction));
				float k = clamp01 (cosine * l.angle_b + l.angle_a);

				for (c = 0; c < 3; c++)
					color[c] *= k;
			}
		}
		if (l.diffuse) {
			float factor = (l.routing & ROUTE_SUB) ? -BASE_FACTOR : BASE_FACTOR;

			if (l.dmode == LMODE_SINGLE_SIDED)
				factor *= std::max (dot3 (normal, to_light), 0.0f);
			else if (l.dmode == LMODE_DOUBLE_SIDED)
				factor *= fabsf (dot3 (normal, to_light));
			if (l.routing & ROUTE_ALPHA) {
				diffuse_alpha += color[0] * factor;
			} else {
				float *to = (l.routing & ROUTE_DIFF_TO_OFFSET) ? specular : diffuse;

				for (c = 0; c < 3; c++)
					to[c] += color[c] * factor * v.base[c];
			}
		}
		if (l.specular) {
			float factor = (l.routing & ROUTE_SUB) ? -BASE_FACTOR : BASE_FACTOR;

			if (l.smode == LMODE_SINGLE_SIDED)
				factor *= clamp01 (powf (std::max (dot3 (to_light, reflected), 0.0f), gloss));
			else if (l.smode == LMODE_DOUBLE_SIDED)
				factor *= clamp01 (powf (fabsf (dot3 (to_light, reflected)), gloss));
			if (l.routing & ROUTE_ALPHA) {
				specular_alpha += color[0] * factor;
			} else {
				float *to = (l.routing & ROUTE_SPEC_TO_OFFSET) ? specular : diffuse;

				for (c = 0; c < 3; c++)
					to[c] += color[c] * factor * v.offset[c];
			}
		}
	}
	for (c = 0; c < 3; c++) {
		diffuse[c] += ambient_base[c] * (ambient_material_base ? v.base[c] : 1.0f);
		specular[c] += ambient_offset[c] * (ambient_material_offset ? v.offset[c] : 1.0f);
		v.base[c] = diffuse[c];
		v.offset[c] = specular[c];
	}
	v.base[3] += diffuse_alpha;
	v.offset[3] += specular_alpha;
	/* What is over 1 in the base goes to the offset */
	if (use_base_over)
		for (c = 0; c < 4; c++)
			v.offset[c] += std::max (v.base[c] - 1.0f, 0.0f);
	for (c = 0; c < 4; c++) {
		v.base[c] = clamp01 (v.base[c]);
		v.offset[c] = clamp01 (v.offset[c]);
	}
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
Elan::clip_point (const Transformed &a, float da, const Transformed &b, float db, Transformed &out) const
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
			holly->elan_triangle (out[0].screen, out[1].screen, out[2].screen);
			if (n == 4)
				holly->elan_triangle (out[0].screen, out[2].screen, out[3].screen);
			return;
		}
	}
	holly->elan_triangle (a.screen, b.screen, c.screen);
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
		else if (!constant)
			light_vertex (v, normal);
		if (!textured)
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

	culling_reversed = open_volume = shadowed_volume = false;
	model_tsp = 0;
	update ();
	return ret;
}

} /* namespace naomi */
