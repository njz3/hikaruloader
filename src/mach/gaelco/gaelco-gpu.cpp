/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/gaelco/atvtrack.cpp (license BSD-3-Clause,
 * copyright-holders Samuele Zannoli).
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
#include <cstddef>
#include <string>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/machine.h"
#include "hl/state.h"
#include "hl/window.h"

#include "cpu/sh/sh4.h"

#include "mach/gaelco/gaelco.h"
#include "mach/gaelco/gaelco-gpu.h"

namespace gaelco {

/*
 * The Neon 250
 * ============
 *
 * A PowerVR series 2 like the Dreamcast's: the same polygon words and texture
 * formats. Beside it, the PMX1: a processor of its own, whose microcode the
 * slave's driver ("pvrhcl", its strings in the games' RAM) loads at the VRAM's
 * start. Its instruction set is not known; what it is asked to do is, from
 * what the driver writes, and is done here without it.
 *
 * The Registers
 * -------------
 *
 *  0000	Written non-zero to start the chip; the games then wait for 1
 *		at VRAM+1350, which the PMX would answer
 *  0070	Interrupts pending; written, clears those bits: 7 a frame drawn, 8 the
 *		vertical blank (the driver counts them at shared RAM +1DC). The
 *		driver leaves 15 unmasked but drops it from what it reads and never
 *		clears it (ATV Track @8C00B8D4): not raised, as it would hold the
 *		slave in the interrupt
 *  0074	Interrupts masked (all at reset)
 *  00B0	0001: commands in the ring, read them; interrupt 7 at once
 *		8001: the ring is set up; the games wait for 1 at VRAM+814804
 *  00C0	The ring's write pointer again (44100000 | pointer)
 *  4xxx	The PCI configuration
 *
 * Its interrupt is the slave's IRL pins at 14 (level 1).
 *
 * The Rings
 * ---------
 *
 * Both in the VRAM, of 64-byte commands (CCBs): word 0 their size, word 1 the
 * microcode's address of what to run, then its arguments.
 *
 *  800000	The commands, 64 KB; written up to VRAM+811020
 *  812000	The frames to draw, 8 KB, word 6 set when one is there:
 *		180080CC  100  region array  chip state  status  1
 *
 * The first ring's "draw" command takes the next frame from the second, which
 * the game fills while the chip draws the one before.
 *
 * A Frame
 * -------
 *
 * The region array:
 *
 *  0	800000xx | tiles across - 1 << 24 | tiles down - 1 << 16 | 20
 *  1-5	Its sizes
 *  6-	The lists, four words each: flags (bit 31 the last; bit 9 translucent),
 *	0, its table's size in 1 KB a page (0: the list is not used), the VRAM's
 *	address of its table of pages (their addresses >> 12)
 *
 * A list is a stream of 32-bit words, in pages, each ended by a zero or its end:
 *
 *  31	A polygon header follows this word, and applies from this strip on:
 *	vertices' page, the page they go on in, the ISP/TSP word, the TSP word,
 *	the texture word (as the Dreamcast's)
 *  30-24 The strip's triangles, one bit each, from bit 30
 *  19-0 The first vertex: its offset, in words, from the vertices' page
 *
 * A vertex is x, y (pixels), 1/w, then u, v when textured (one word when they
 * are 16-bit), the colour, the offset colour when asked for.
 */

enum {
	REG_START	= 0x0000,
	REG_PENDING	= 0x0070,
	REG_MASK	= 0x0074,
	REG_RENDER	= 0x00B0,
};

enum {
	VRAM_MASK	= 0x01FFFFFF,
	RING		= 0x800000,
	RING_MASK	= 0xFFFF,
	RING_WRITE	= 0x811020,
	LIST_RING	= 0x812000,
	LIST_RING_MASK	= 0x1FFF,
};

/* The IRL pins the Neon 250 drives on the slave */
static const unsigned NEON_IRL_PINS = 14;

Neon250::Neon250 (hl::Machine *mach, hl::Buffer *vram) :
	hl::Device (mach),
	vram (vram)
{
}

void
Neon250::reset (hl::ResetType type)
{
	(void) type;
	frames = 0;
	pending = 0;
	mask = 0xFFFF;
	ring_read = 0;
	list_read = 0;
	ready.reset ();
	memset (regs, 0, sizeof (regs));
}

void
Neon250::update_irq ()
{
	Gaelco *gaelco = (Gaelco *) mach;
	gaelco->set_irl (gaelco->sh_s, NEON_IRL_PINS, (pending & ~mask) != 0, this);
}

void
Neon250::raise (unsigned bit)
{
	pending |= 1 << bit;
	update_irq ();
}

/*
 * The Frame's Lists
 */

static inline uint32_t
vram_u32 (hl::Buffer *vram, uint32_t addr)
{
	uint32_t val;
	memcpy (&val, vram->ptr (addr & VRAM_MASK & ~3u), 4);
	return val;
}

/* The words of a list. Its table holds count pages, the first ones: those after
 * them, of a frame with more, are there still, their lists out of date. A zero
 * ends a page, not the list, which goes on in the next one with the same header;
 * a page's unused ones are all zeros */
class ListReader {
public:
	ListReader (hl::Buffer *vram, uint32_t table, unsigned count) : vram (vram)
	{
		unsigned i;
		for (i = 0; i < count; i++) {
			uint32_t entry = vram_u32 (vram, table + i * 4);
			if (entry < 0x18000 || entry >= 0x1A000)
				break;
			pages.push_back ((entry << 12) & VRAM_MASK);
		}
	}

	/* 0 at the list's end */
	uint32_t next ()
	{
		while (page < pages.size ()) {
			uint32_t val = (word < 1024) ? vram_u32 (vram, pages[page] + word++ * 4) : 0;
			if (val)
				return val;
			page++;
			word = 0;
		}
		return 0;
	}

private:
	hl::Buffer *vram;
	std::vector<uint32_t> pages;
	size_t page = 0;
	uint32_t word = 0;
};

/* A vertex's words, from the polygon's ISP/TSP word */
static inline unsigned
vertex_words (uint32_t isp)
{
	bool textured = (isp >> 25) & 1, offset = (isp >> 24) & 1, uv16 = (isp >> 22) & 1;
	return 3 + (textured ? (uv16 ? 1 : 2) : 0) + 1 + ((textured && offset) ? 1 : 0);
}

static inline float
u32_to_f (uint32_t val)
{
	float f;
	memcpy (&f, &val, 4);
	return f;
}

/* A strip's vertices can go on past their 4 KB page, in the next one the header
 * gives, which need not follow it */
struct VertexPages {
	uint32_t page[2];

	uint32_t word (hl::Buffer *vram, uint32_t index) const
	{
		return vram_u32 (vram, page[index >= 1024 ? 1 : 0] + (index & 1023) * 4);
	}
};

static Vertex
read_vertex (hl::Buffer *vram, const VertexPages &pages, uint32_t index, uint32_t isp)
{
	bool textured = (isp >> 25) & 1, offset = (isp >> 24) & 1, uv16 = (isp >> 22) & 1;
	Vertex v = {};

	v.x = u32_to_f (pages.word (vram, index));
	v.y = u32_to_f (pages.word (vram, index + 1));
	v.z = u32_to_f (pages.word (vram, index + 2));
	index += 3;
	if (textured) {
		if (uv16) {
			uint32_t uv = pages.word (vram, index);
			v.u = u32_to_f (uv & 0xFFFF0000);
			v.v = u32_to_f (uv << 16);
			index += 1;
		} else {
			v.u = u32_to_f (pages.word (vram, index));
			v.v = u32_to_f (pages.word (vram, index + 1));
			index += 2;
		}
	}
	v.base = pages.word (vram, index);
	if (textured && offset)
		v.offset = pages.word (vram, index + 1);
	return v;
}

static void
add_list (Frame *frame, hl::Buffer *vram, uint32_t table, unsigned pages_used, bool translucent)
{
	ListReader list (vram, table, pages_used);
	VertexPages pages = {};
	uint32_t isp = 0, tsp = 0, tcw = 0;
	bool have_header = false;
	/* A list that long is a bad one */
	unsigned left = 1024 * 1024;

	while (left--) {
		uint32_t word = list.next ();
		if (!word)
			break;
		if (word >> 31) {
			uint32_t header[5], i;
			for (i = 0; i < 5; i++)
				header[i] = list.next ();
			pages.page[0] = header[0];
			pages.page[1] = header[1];
			isp = header[2];
			tsp = header[3];
			tcw = header[4];
			have_header = true;

			if (frame->batches.empty () || frame->batches.back ().isp != isp ||
			    frame->batches.back ().tsp != tsp || frame->batches.back ().tcw != tcw ||
			    frame->batches.back ().translucent != translucent) {
				Batch batch = { isp, tsp, tcw, (uint32_t) frame->vertices.size (), 0, translucent };
				frame->batches.push_back (batch);
			}
		}
		if (!have_header)
			continue;

		unsigned triangles = (word >> 24) & 0x7F, words = vertex_words (isp), t;
		unsigned cull = (isp >> 27) & 3;
		uint32_t first = word & 0xFFFFF;

		for (t = 0; t < 7; t++) {
			if (!(triangles & (0x40 >> t)))
				continue;
			Vertex v0 = read_vertex (vram, pages, first + t * words, isp);
			Vertex v1 = read_vertex (vram, pages, first + (t + 1) * words, isp);
			Vertex v2 = read_vertex (vram, pages, first + (t + 2) * words, isp);
			/* Flat: the colours of the triangle's last vertex */
			if (!((isp >> 23) & 1)) {
				v0.base = v1.base = v2.base;
				v0.offset = v1.offset = v2.offset;
			}
			if (t & 1)
				std::swap (v1, v2);
			if (v0.z <= 0.0f || v1.z <= 0.0f || v2.z <= 0.0f)
				continue;
			if (cull >= 2) {
				float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
				if ((cull == 2) ? (area < 0.0f) : (area > 0.0f))
					continue;
			}
			frame->vertices.push_back (v0);
			frame->vertices.push_back (v1);
			frame->vertices.push_back (v2);
			frame->max_z = std::max ({ frame->max_z, v0.z, v1.z, v2.z });
			frame->batches.back ().count += 3;
		}
	}
}

/* The translucent polygons, which the chip sorts itself: from the farthest on,
 * each triangle's middle standing for it; the list's order where it ties (a
 * text over its box) */
static void
sort_translucent (Frame *frame)
{
	struct Triangle {
		float z;
		uint32_t first;
		uint32_t batch;
	};
	std::vector<Triangle> triangles;
	std::vector<Vertex> vertices;
	std::vector<Batch> batches;
	size_t first = 0, b;
	uint32_t base;

	while (first < frame->batches.size () && !frame->batches[first].translucent)
		first++;
	if (first == frame->batches.size ())
		return;
	base = frame->batches[first].first;
	for (b = first; b < frame->batches.size (); b++) {
		const Batch &batch = frame->batches[b];
		uint32_t i;
		for (i = 0; i + 3 <= batch.count; i += 3) {
			const Vertex *v = &frame->vertices[batch.first + i];
			triangles.push_back ({ v[0].z + v[1].z + v[2].z, batch.first + i, (uint32_t) b });
		}
	}
	std::stable_sort (triangles.begin (), triangles.end (),
	                  [] (const Triangle &x, const Triangle &y) { return x.z < y.z; });
	vertices.reserve (triangles.size () * 3);
	for (const Triangle &tri : triangles) {
		const Batch &from = frame->batches[tri.batch];
		if (batches.empty () || batches.back ().isp != from.isp ||
		    batches.back ().tsp != from.tsp || batches.back ().tcw != from.tcw) {
			Batch batch = from;
			batch.first = base + (uint32_t) vertices.size ();
			batch.count = 0;
			batches.push_back (batch);
		}
		vertices.insert (vertices.end (), &frame->vertices[tri.first], &frame->vertices[tri.first] + 3);
		batches.back ().count += 3;
	}
	std::copy (vertices.begin (), vertices.end (), frame->vertices.begin () + base);
	frame->batches.resize (first);
	frame->batches.insert (frame->batches.end (), batches.begin (), batches.end ());
}

void
Neon250::build_frame (uint32_t region, uint32_t state)
{
	std::unique_ptr<Frame> frame (new Frame ());
	unsigned n;

	frame->max_z = 1e-6f;
	frame->vertices.reserve (16384);
	for (n = 0; n < 8; n++) {
		uint32_t flags = vram_u32 (vram, region + (6 + n * 4) * 4);
		uint32_t stride = vram_u32 (vram, region + (8 + n * 4) * 4);
		uint32_t table = vram_u32 (vram, region + (9 + n * 4) * 4);

		/* No pages: a list not used, its table not set */
		if (stride)
			add_list (frame.get (), vram, table, stride / 0x400, (flags >> 9) & 1);
		if (flags >> 31)
			break;
	}
	frame->fog_color = vram_u32 (vram, state + 0xB0);
	frame->fog_density = vram_u32 (vram, state + 0xB8) & 0xFFFF;
	frame->background = vram_u32 (vram, state + 0x418);
	frame->stride = (vram_u32 (vram, state + 0xE4) & 0x1F) * 32;
	for (n = 0; n < 128; n++)
		frame->fog_table[n] = (uint16_t) vram_u32 (vram, state + 0x200 + n * 4);
	ready = std::move (frame);
	frames++;
}

/* A command of the first ring */
void
Neon250::run_ccb (uint32_t ccb)
{
	uint32_t handler = vram_u32 (vram, ccb + 4);

	if (handler == 0x18000DA4) {
		/* The driver's test (Gaelco Football): word 3 written at word 2 */
		vram->put (4, vram_u32 (vram, ccb + 8) & VRAM_MASK, vram_u32 (vram, ccb + 12));
	} else if (handler == 0x18008590) {
		/* Draw: the next frame of the second ring */
		uint32_t slot = LIST_RING + list_read;

		if (vram_u32 (vram, slot + 24) == 1) {
			vram->put (4, slot + 24, 0);
			build_frame (vram_u32 (vram, slot + 12), vram_u32 (vram, slot + 16));
			list_read = (list_read + 64) & LIST_RING_MASK;
		}
	}
}

void
Neon250::kick ()
{
	uint32_t write = (uint32_t) vram->get (4, RING_WRITE) & RING_MASK;

	while (ring_read != write) {
		run_ccb (RING + ring_read);
		ring_read = (ring_read + 64) & RING_MASK;
	}
}

void
Neon250::vblank ()
{
	raise (8);
}

int
Neon250::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t offs = addr & 0x7FFF;
	uint32_t data;

	switch (offs) {
	case REG_PENDING:
		data = pending;
		break;
	case REG_MASK:
		data = mask;
		break;
	default:
		/* 0, as the games accept */
		data = 0;
		if (!(seen_r[offs / 4 / 8] & (1 << ((offs / 4) & 7)))) {
			seen_r[offs / 4 / 8] |= 1 << ((offs / 4) & 7);
			HL_LOG ("neon250: frame %llu: first read of %04X",
			        (unsigned long long) mach->frame, offs);
		}
		break;
	}
	set_ptr (val, size, data);
	return 0;
}

int
Neon250::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0x7FFF;
	uint32_t data = (uint32_t) val;

	(void) size;
	if (offs < sizeof (regs))
		regs[offs / 4] = data;
	/* What the games use, for the reverse engineering */
	if (offs < sizeof (regs) && !(seen_w[offs / 4 / 8] & (1 << ((offs / 4) & 7)))) {
		seen_w[offs / 4 / 8] |= 1 << ((offs / 4) & 7);
		HL_LOG ("neon250: frame %llu: first write of %04X = %08X",
		        (unsigned long long) mach->frame, offs, data);
	}

	switch (offs) {
	case REG_START:
		if (data)
			vram->put (1, 0x1350, 1);
		break;
	case REG_PENDING:
		pending &= ~data;
		update_irq ();
		break;
	case REG_MASK:
		mask = data;
		update_irq ();
		break;
	case REG_RENDER:
		if (data == 0x0001) {
			kick ();
			raise (7);
		} else if (data == 0x8001)
			vram->put (1, 0x814804, 1);
		break;
	default:
		break;
	}
	return 0;
}

const char *
Neon250::debug_string ()
{
	static char out[64];
	snprintf (out, sizeof (out), "NEON pending %04X mask %04X frames %u",
	          pending, mask, frames);
	return out;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Neon250::save_state (hl::State *state)
{
	int ret = 0;
	SAVE (frames);
	SAVE (pending);
	SAVE (mask);
	SAVE (regs);
	SAVE (ring_read);
	SAVE (list_read);
	return ret;
}

int
Neon250::load_state (hl::State *state)
{
	int ret = 0;
	LOAD (frames);
	LOAD (pending);
	LOAD (mask);
	LOAD (regs);
	LOAD (ring_read);
	LOAD (list_read);
	return ret;
}

#undef SAVE
#undef LOAD

/*
 * The Textures
 * ============
 *
 * The texture word:
 *
 *  31	 Mipmapped: the smaller levels first, the largest last
 *  30	 VQ: a 2048-byte code book of four texels each, then a byte per 2x2;
 *	 with bit 26, a byte per texel, the entry's first texel (Smashing Drive)
 *  29-27 0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 YUV422, 4 bump, 5 ARGB8888 (the
 *	 Dreamcast's: paletted)
 *  26	 The lines in order (else twiddled: x and y's bits interleaved)
 *  25	 Those lines as long as the chip state's stride (+E4, in 32 texels)
 *  20-0 Its address in the VRAM, in 32 bytes (the Dreamcast's: in 8)
 *
 * The TSP word's bits 5-3 and 2-0: 8 << the width, and the height.
 */

/* x and y's bits interleaved, y's first, while both have some: then the other's
 * alone (a rectangular texture) */
static inline uint32_t
twiddle (uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t out = 0;
	unsigned bit = 0;

	for (w >>= 1, h >>= 1; w || h; ) {
		if (h) {
			out |= (y & 1) << bit++;
			y >>= 1;
			h >>= 1;
		}
		if (w) {
			out |= (x & 1) << bit++;
			x >>= 1;
			w >>= 1;
		}
	}
	return out;
}

/* A 16-bit texel as RGBA8 */
static inline uint32_t
texel_rgba (unsigned format, uint16_t c)
{
	uint32_t r, g, b, a;

	switch (format) {
	case 0:
		a = (c >> 15) ? 255 : 0;
		r = ((c >> 10) & 31) * 255 / 31;
		g = ((c >> 5) & 31) * 255 / 31;
		b = (c & 31) * 255 / 31;
		break;
	case 1:
		a = 255;
		r = ((c >> 11) & 31) * 255 / 31;
		g = ((c >> 5) & 63) * 255 / 63;
		b = (c & 31) * 255 / 31;
		break;
	case 2:
		a = ((c >> 12) & 15) * 17;
		r = ((c >> 8) & 15) * 17;
		g = ((c >> 4) & 15) * 17;
		b = (c & 15) * 17;
		break;
	default:
		a = 255;
		r = g = b = c >> 8;
		break;
	}
	return r | (g << 8) | (b << 16) | (a << 24);
}

/* The texels of the levels smaller than a w x w one: 1x1 + 2x2 + ... */
static inline uint32_t
mip_texels (unsigned w)
{
	uint32_t n = 1, s;
	for (s = 2; s < w; s <<= 1)
		n += s * s;
	return n;
}

/* Before the largest level: 16-bit texels (and 6 bytes before them), VQ's
 * bytes of 2x2 (and one before them) */
static inline uint32_t
mip_bytes (uint32_t tcw, unsigned w)
{
	if (!(tcw >> 31))
		return 0;
	return ((tcw >> 30) & 1) ? mip_texels (w / 2) + 1 : mip_texels (w) * 2 + 6;
}

/* A line's texels, the lines being in order */
static inline unsigned
scan_width (uint32_t tcw, unsigned w, unsigned stride)
{
	return (((tcw >> 25) & 1) && stride) ? stride : w;
}

static uint32_t
texture_bytes (uint32_t tcw, unsigned w, unsigned h, unsigned stride)
{
	if (((tcw >> 26) & 1) && !(tcw >> 31)) {
		unsigned line = scan_width (tcw, w, stride);
		if ((tcw >> 30) & 1)
			return 2048 + line * h;
		return line * h * (((tcw >> 27) & 7) == 5 ? 4 : 2);
	}
	if ((tcw >> 30) & 1)
		return 2048 + mip_bytes (tcw, w) + (w / 2) * (h / 2);
	return mip_bytes (tcw, w) + w * h * (((tcw >> 27) & 7) == 5 ? 4 : 2);
}

static void
decode_texture (std::vector<uint32_t> &out, hl::Buffer *vram, uint32_t tcw, unsigned w, unsigned h,
                unsigned stride)
{
	uint32_t addr = (tcw & 0x1FFFFF) << 5;
	bool vq = (tcw >> 30) & 1, scan = (tcw >> 26) & 1;
	unsigned format = (tcw >> 27) & 7, x, y;

	out.resize (w * h);
	if (addr + texture_bytes (tcw, w, h, stride) > vram->size ()) {
		std::fill (out.begin (), out.end (), 0xFFFF00FFu);
		return;
	}
	const uint8_t *data = (const uint8_t *) vram->ptr (addr);
	unsigned line = scan_width (tcw, w, stride);

	if (vq && scan) {
		const uint16_t *book = (const uint16_t *) data;
		const uint8_t *index = data + 2048;
		for (y = 0; y < h; y++)
			for (x = 0; x < w && x < line; x++)
				out[y * w + x] = texel_rgba (format, book[index[y * line + x] * 4]);
	} else if (vq) {
		const uint16_t *book = (const uint16_t *) data;
		const uint8_t *index = data + 2048 + mip_bytes (tcw, w);
		for (y = 0; y < h; y += 2) {
			for (x = 0; x < w; x += 2) {
				const uint16_t *e = &book[index[twiddle (x / 2, y / 2, w / 2, h / 2)] * 4];
				out[y * w + x] = texel_rgba (format, e[0]);
				out[(y + 1) * w + x] = texel_rgba (format, e[1]);
				out[y * w + x + 1] = texel_rgba (format, e[2]);
				out[(y + 1) * w + x + 1] = texel_rgba (format, e[3]);
			}
		}
	} else if (format == 5) {
		const uint32_t *t = (const uint32_t *) (data + mip_bytes (tcw, w) * 2);
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				uint32_t c = t[scan ? y * line + x % line : twiddle (x, y, w, h)];
				out[y * w + x] = (c & 0xFF00FF00) | ((c >> 16) & 255) | ((c & 255) << 16);
			}
		}
	} else if (!scan) {
		const uint16_t *t = (const uint16_t *) (data + mip_bytes (tcw, w));
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				out[y * w + x] = texel_rgba (format, t[twiddle (x, y, w, h)]);
	} else {
		const uint16_t *t = (const uint16_t *) data;
		for (y = 0; y < h; y++)
			for (x = 0; x < w && x < line; x++)
				out[y * w + x] = texel_rgba (format, t[y * line + x]);
	}
}

GLuint
Renderer::texture (uint32_t tcw, uint32_t tsp)
{
	unsigned w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
	uint64_t key = ((uint64_t) tcw << 32) | (tsp & 0x3F);
	Texture &t = textures[key];

	if (t.id && t.seen == frame_count)
		return t.id;
	t.seen = frame_count;

	/* Its data can change at any time: a hash of it */
	uint32_t addr = (tcw & 0x1FFFFF) << 5;
	uint32_t bytes = texture_bytes (tcw, w, h, frame->stride), i;
	uint64_t hash = (0xCBF29CE484222325ull ^ frame->stride) * 0x100000001B3ull;
	if (addr + bytes <= gpu->vram->size ()) {
		const uint32_t *p = (const uint32_t *) gpu->vram->ptr (addr);
		for (i = 0; i < bytes / 4; i++)
			hash = (hash ^ p[i]) * 0x100000001B3ull;
	}
	if (t.id && t.hash == hash)
		return t.id;
	t.hash = hash;

	std::vector<uint32_t> rgba;
	decode_texture (rgba, gpu->vram, tcw, w, h, frame->stride);
	if (!t.id)
		glGenTextures (1, &t.id);
	glBindTexture (GL_TEXTURE_2D, t.id);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
	glGenerateMipmap (GL_TEXTURE_2D);
	return t.id;
}

/*
 * The Renderer
 */

static const char *vs_src = R"(#version 430
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_base;
layout (location = 3) in vec4 in_offset;
out vec2 v_uv;
out vec4 v_base;
out vec4 v_offset;
noperspective out float v_z;
void main ()
{
	float w = 1.0 / in_pos.z;
	gl_Position = vec4 (in_pos.x / 320.0 - 1.0, 1.0 - in_pos.y / 240.0, 0.0, 1.0) * w;
	v_uv = in_uv;
	v_base = in_base;
	v_offset = in_offset;
	v_z = in_pos.z;
}
)";

/* u_flags: bits 0-1 the shading, 2 textured, 3 the base's alpha used, 4 the
 * texture's alpha ignored, 5 the offset added, 7-8 the fog */
static const char *fs_src = R"(#version 430
in vec2 v_uv;
in vec4 v_base;
in vec4 v_offset;
noperspective in float v_z;
out vec4 out_color;
uniform sampler2D u_tex;
uniform usampler1D u_fog;
uniform int u_flags;
uniform float u_zscale;
uniform float u_fog_density;
uniform vec3 u_fog_color;
#ifdef COLLECT
layout (binding = 0, offset = 0) uniform atomic_uint u_count;
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;
layout (std430, binding = 0) buffer Nodes { uvec4 nodes[]; };
uniform uint u_max_nodes;
uniform uint u_tag;
uniform sampler2D u_opaque_depth;
#endif
float fog_table (float w)
{
	float z = clamp (w * u_fog_density, 1.0, 255.9999);
	float e = floor (log2 (z));
	float m = z * 16.0 / pow (2.0, e) - 16.0;
	int i = int (floor (m) + e * 16.0);
	uint entry = texelFetch (u_fog, clamp (i, 0, 127), 0).r;
	float a = float (entry >> 8u) / 255.0, b = float (entry & 255u) / 255.0;
	return mix (a, b, fract (m));
}
void main ()
{
	vec4 color = v_base;
	if ((u_flags & 8) == 0)
		color.a = 1.0;
	if ((u_flags & 4) != 0) {
		vec4 t = texture (u_tex, v_uv);
		if ((u_flags & 16) != 0)
			t.a = 1.0;
		switch (u_flags & 3) {
		case 0:
			color = t;
			break;
		case 1:
			color = vec4 (color.rgb * t.rgb, t.a);
			break;
		case 2:
			color = vec4 (mix (color.rgb, t.rgb, t.a), color.a);
			break;
		default:
			color *= t;
			break;
		}
		if ((u_flags & 32) != 0)
			color.rgb += v_offset.rgb;
	}
	int fog = (u_flags >> 7) & 3;
	if (fog == 0)
		color.rgb = mix (color.rgb, u_fog_color, fog_table (1.0 / v_z));
	else if (fog == 1)
		color.rgb = mix (color.rgb, u_fog_color, v_offset.a);
	float depth = v_z * u_zscale;
#ifdef COLLECT
	/* Into its pixel's list, unless the opaque polygons hide it: always
	 * "greater or equal" there, whatever the polygon's own test */
	ivec2 pixel = ivec2 (gl_FragCoord.xy);
	if (depth < texelFetch (u_opaque_depth, pixel, 0).r)
		discard;
	uint node = atomicCounterIncrement (u_count);
	if (node < u_max_nodes) {
		uint next = imageAtomicExchange (u_heads, pixel, node + 1u);
		nodes[node] = uvec4 (packUnorm4x8 (clamp (color, 0.0, 1.0)), floatBitsToUint (depth),
		                     next, u_tag + (uint (gl_PrimitiveID) << 6));
	}
#else
	out_color = color;
	gl_FragDepth = depth;
#endif
}
)";

/*
 * Translucency
 * ============
 *
 * The chip sorts the translucent polygons itself, at each pixel ("auto-sort",
 * as the Dreamcast's): it blends their pixels from the farthest, comparing
 * them with the opaque ones' always as "greater or equal", whatever their own
 * depth test; of two at the same depth, the one sent first is the farther.
 * With SORT_TRANSLUCENT_PIXELS, so here:
 *
 *  - collected: each fragment shaded as usual, kept when the opaque depth
 *    (a copy) does not hide it, in its pixel's list (a storage buffer of
 *    uvec4: colour, depth, next + 1, the blending's source and destination
 *    instructions and the triangle's place in the lists << 6; an r32ui image
 *    holds each pixel's last + 1);
 *  - resolved: a pass over the picture blends each pixel's list over a copy
 *    of what is drawn, the farthest first, with the TSP word's two blending
 *    instructions as the chip has them (the other's colour, the source's or
 *    the destination's alpha), each result rounded to 8 bits. Eight
 *    fragments are put in order at a time, again until the list is done.
 *    The pass empties the heads for the next frame.
 *
 * The buffer, 2 fragments a pixel at first, grows when a frame fills it.
 * Without it, each triangle is sorted whole (sort_translucent ()).
 */

static const char *resolve_vs_src = R"(#version 430
void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char *resolve_fs_src = R"(#version 430
#define KEPT 8
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;
layout (std430, binding = 0) readonly buffer Nodes { uvec4 nodes[]; };
uniform sampler2D u_below;
out vec4 out_color;
/* Whether a fragment (depth, place) is blended before another: the farther,
 * then the first sent */
bool before (float d, uint o, float d2, uint o2)
{
	return d < d2 || (d == d2 && o < o2);
}
vec4 factor (uint instr, vec4 src, vec4 dst, bool is_src)
{
	vec4 other = is_src ? dst : src;
	switch (instr) {
	case 0u: return vec4 (0.0);
	case 1u: return vec4 (1.0);
	case 2u: return other;
	case 3u: return vec4 (1.0) - other;
	case 4u: return vec4 (src.a);
	case 5u: return vec4 (1.0 - src.a);
	case 6u: return vec4 (dst.a);
	default: return vec4 (1.0 - dst.a);
	}
}
void main ()
{
	ivec2 pixel = ivec2 (gl_FragCoord.xy);
	uint head = imageAtomicExchange (u_heads, pixel, 0u);
	float depth[KEPT], last_d = -1.0;
	uint order[KEPT], index[KEPT], last_o = 0u, i;
	int num, j;
	bool more;
	vec4 color;

	if (head == 0u)
		discard;
	color = texelFetch (u_below, pixel, 0);
	for (;;) {
		/* The next KEPT in order, after the last blended */
		num = 0;
		more = false;
		for (i = head; i != 0u; i = nodes[i - 1u].z) {
			float d = uintBitsToFloat (nodes[i - 1u].y);
			uint o = nodes[i - 1u].w >> 6;

			if (!before (last_d, last_o, d, o))
				continue;
			if (num < KEPT) {
				j = num++;
			} else {
				more = true;
				if (!before (d, o, depth[KEPT - 1], order[KEPT - 1]))
					continue;
				j = KEPT - 1;
			}
			for (; j > 0 && before (d, o, depth[j - 1], order[j - 1]); j--) {
				depth[j] = depth[j - 1];
				order[j] = order[j - 1];
				index[j] = index[j - 1];
			}
			depth[j] = d;
			order[j] = o;
			index[j] = i;
		}
		for (j = 0; j < num; j++) {
			uvec4 n = nodes[index[j] - 1u];
			vec4 src = unpackUnorm4x8 (n.x);
			vec4 c = src * factor (n.w & 7u, src, color, true) +
			         color * factor ((n.w >> 3) & 7u, src, color, false);
			color = floor (clamp (c, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
		}
		if (!more)
			break;
		last_d = depth[KEPT - 1];
		last_o = order[KEPT - 1];
	}
	out_color = color;
}
)";

#define OIT_MAX_NODES	(16u << 20)
#define OIT_DEPTH_UNIT	2
#define OIT_BELOW_UNIT	3

static const char *blit_vs_src = R"(#version 430
out vec2 v_uv;
void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = p;
	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char *blit_fs_src = R"(#version 430
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_tex;
void main ()
{
	out_color = vec4 (texture (u_tex, v_uv).rgb, 1.0);
}
)";

Renderer::Renderer (Neon250 *gpu) :
	hl::Renderer (640, 480),
	gpu (gpu)
{
	setup ();
}

Renderer::~Renderer ()
{
	for (auto &it : textures)
		glDeleteTextures (1, &it.second.id);
	destroy_oit ();
}

static void
locate (GLuint id, GLint *u_flags, GLint *u_tex, GLint *u_fog, GLint *u_zscale,
        GLint *u_fog_color, GLint *u_fog_density)
{
	*u_flags = glGetUniformLocation (id, "u_flags");
	*u_tex = glGetUniformLocation (id, "u_tex");
	*u_fog = glGetUniformLocation (id, "u_fog");
	*u_zscale = glGetUniformLocation (id, "u_zscale");
	*u_fog_color = glGetUniformLocation (id, "u_fog_color");
	*u_fog_density = glGetUniformLocation (id, "u_fog_density");
}

void
Renderer::setup ()
{
	presentation.reset (new hl::Presentation ());

	program.id = hl::gl::compile_program (vs_src, fs_src);
	locate (program.id, &program.u_flags, &program.u_tex, &program.u_fog, &program.u_zscale,
	        &program.u_fog_color, &program.u_fog_density);
	program.u_tag = program.u_max_nodes = program.u_opaque_depth = -1;

	/* The same, collecting the translucent fragments */
	{
		std::string src = fs_src;
		src.insert (src.find ('\n') + 1, "#define COLLECT 1\n");
		collect.id = hl::gl::compile_program (vs_src, src.c_str ());
		locate (collect.id, &collect.u_flags, &collect.u_tex, &collect.u_fog, &collect.u_zscale,
		        &collect.u_fog_color, &collect.u_fog_density);
		collect.u_tag = glGetUniformLocation (collect.id, "u_tag");
		collect.u_max_nodes = glGetUniformLocation (collect.id, "u_max_nodes");
		collect.u_opaque_depth = glGetUniformLocation (collect.id, "u_opaque_depth");
		oit.resolve = hl::gl::compile_program (resolve_vs_src, resolve_fs_src);
		glGenVertexArrays (1, &oit.vao);
		if (!collect.id || !oit.resolve) {
			HL_ERROR ("gaelco: no translucency at each pixel; each triangle sorted whole");
			oit.failed = true;
		}
	}
	blit_program = hl::gl::compile_program (blit_vs_src, blit_fs_src);
	u_blit_tex = glGetUniformLocation (blit_program, "u_tex");

	glGenVertexArrays (1, &vao);
	glGenVertexArrays (1, &blit_vao);
	glGenBuffers (1, &vbo);
	glBindVertexArray (vao);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (Vertex), (void *) offsetof (Vertex, x));
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex), (void *) offsetof (Vertex, u));
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (Vertex), (void *) offsetof (Vertex, base));
	glEnableVertexAttribArray (3);
	glVertexAttribPointer (3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (Vertex), (void *) offsetof (Vertex, offset));
	glBindVertexArray (0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);

	glGenTextures (1, &fog_tex);
	glBindTexture (GL_TEXTURE_1D, fog_tex);
	glTexParameteri (GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage1D (GL_TEXTURE_1D, 0, GL_R16UI, 128, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
	glBindTexture (GL_TEXTURE_1D, 0);
}

static GLenum
blend_factor (unsigned instr, bool src)
{
	switch (instr & 7) {
	case 0: return GL_ZERO;
	case 1: return GL_ONE;
	case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
	case 3: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
	case 4: return GL_SRC_ALPHA;
	case 5: return GL_ONE_MINUS_SRC_ALPHA;
	case 6: return GL_DST_ALPHA;
	default: return GL_ONE_MINUS_DST_ALPHA;
	}
}

static GLenum
depth_func (unsigned mode)
{
	static const GLenum funcs[8] = {
		GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
	};
	return funcs[mode & 7];
}

void
Renderer::draw_scene ()
{
	unsigned scale = (unsigned) std::max (1, hl::Config::get ()->render_scale);
	unsigned w = 640 * scale, h = 480 * scale;

	if (!fb || fb_w != w || fb_h != h) {
		if (!fb) {
			glGenFramebuffers (1, &fb);
			glGenTextures (1, &fb_color);
			glGenTextures (1, &fb_depth);
		}
		glBindTexture (GL_TEXTURE_2D, fb_color);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture (GL_TEXTURE_2D, fb_depth);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindFramebuffer (GL_FRAMEBUFFER, fb);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_color, 0);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, fb_depth, 0);
		fb_w = w;
		fb_h = h;
	}

	glBindFramebuffer (GL_FRAMEBUFFER, fb);
	glViewport (0, 0, w, h);
	glDisable (GL_SCISSOR_TEST);
	glDepthMask (GL_TRUE);
	if (frame)
		glClearColor (((frame->background >> 16) & 255) / 255.0f,
		              ((frame->background >> 8) & 255) / 255.0f,
		              (frame->background & 255) / 255.0f, 1.0f);
	else
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepth (0.0);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glClearDepth (1.0);

	if (!frame || frame->vertices.empty ())
		return;

	bool per_pixel = hl::Config::get ()->sort_translucent_pixels && !oit.failed && oit_buffers (w, h);
	if (!per_pixel && !frame_sorted)
		sort_translucent (frame.get ());
	frame_sorted |= !per_pixel;

	glUseProgram (program.id);
	glBindVertexArray (vao);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
	glBufferData (GL_ARRAY_BUFFER, frame->vertices.size () * sizeof (Vertex),
	              frame->vertices.data (), GL_STREAM_DRAW);
	set_scene_uniforms (program);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_1D, fog_tex);
	glTexSubImage1D (GL_TEXTURE_1D, 0, 0, 128, GL_RED_INTEGER, GL_UNSIGNED_SHORT, frame->fog_table);
	glActiveTexture (GL_TEXTURE0);

	glEnable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);

	/* The opaque polygons, then the translucent ones (in their lists'
	 * order, the chip's) */
	for (const Batch &b : frame->batches)
		if (!per_pixel || !b.translucent)
			draw_batch (program, b);
	if (per_pixel)
		draw_translucent_per_pixel ();

	glDisable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_TRUE);
	glDepthFunc (GL_LESS);
	glBindVertexArray (0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	glUseProgram (0);
}

void
Renderer::set_scene_uniforms (const Program &p)
{
	glUniform1i (p.u_tex, 0);
	glUniform1i (p.u_fog, 1);
	glUniform1f (p.u_zscale, 1.0f / frame->max_z);
	glUniform3f (p.u_fog_color,
	             ((frame->fog_color >> 16) & 255) / 255.0f,
	             ((frame->fog_color >> 8) & 255) / 255.0f,
	             (frame->fog_color & 255) / 255.0f);
	{
		/* A byte of mantissa (1.7), a byte of signed exponent */
		int e = (int8_t) (frame->fog_density & 255);
		float m = ((frame->fog_density >> 8) & 255) / 128.0f;
		glUniform1f (p.u_fog_density, m * powf (2.0f, (float) e));
	}
}

/* A batch with p: the polygon program, or the collecting one (no blending, no
 * depth test there: the shader's) */
void
Renderer::draw_batch (const Program &p, const Batch &b)
{
	bool textured = (b.isp >> 25) & 1;
	int flags = 0;

	if (!b.count)
		return;
	flags |= (b.tsp >> 6) & 3;
	flags |= textured ? 4 : 0;
	flags |= ((b.tsp >> 20) & 1) ? 8 : 0;
	flags |= ((b.tsp >> 19) & 1) ? 16 : 0;
	flags |= ((b.isp >> 24) & 1) ? 32 : 0;
	flags |= ((b.tsp >> 22) & 3) << 7;
	glUniform1i (p.u_flags, flags);

	if (textured) {
		unsigned filter = (b.tsp >> 13) & 3;
		bool mip = b.tcw >> 31;
		bool clamp_u = (b.tsp >> 16) & 1, clamp_v = (b.tsp >> 15) & 1;
		bool flip_u = (b.tsp >> 18) & 1, flip_v = (b.tsp >> 17) & 1;

		glBindTexture (GL_TEXTURE_2D, texture (b.tcw, b.tsp));
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		                 mip ? (filter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) :
		                 (filter ? GL_LINEAR : GL_NEAREST));
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		                 clamp_u ? GL_CLAMP_TO_EDGE : flip_u ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		                 clamp_v ? GL_CLAMP_TO_EDGE : flip_v ? GL_MIRRORED_REPEAT : GL_REPEAT);
		{
			/* Mipmap D adjust, in quarters */
			unsigned d = (b.tsp >> 8) & 15;
			glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, d ? log2f (d / 4.0f) : 0.0f);
		}
	}

	if (p.u_tag >= 0) {
		glUniform1ui (p.u_tag, ((b.tsp >> 29) & 7) | (((b.tsp >> 26) & 7) << 3) |
		                       ((b.first / 3) << 6));
	} else {
		glBlendFunc (blend_factor (b.tsp >> 29, true), blend_factor (b.tsp >> 26, false));
		glDepthFunc (depth_func (b.isp >> 29));
		glDepthMask ((b.translucent || ((b.isp >> 26) & 1)) ? GL_FALSE : GL_TRUE);
	}
	glDrawArrays (GL_TRIANGLES, b.first, b.count);
}

void
Renderer::destroy_oit ()
{
	GLuint textures_[3] = { oit.heads, oit.depth_tex, oit.color_tex };
	GLuint fbs[2] = { oit.depth_fb, oit.color_fb };
	GLuint buffers[2] = { oit.nodes, oit.counter };

	glDeleteTextures (3, textures_);
	glDeleteFramebuffers (2, fbs);
	glDeleteBuffers (2, buffers);
	oit.heads = oit.depth_tex = oit.color_tex = 0;
	oit.depth_fb = oit.color_fb = oit.nodes = oit.counter = 0;
	oit.w = oit.h = 0;
}

/* The lists' buffers at the scene's size: true when ready */
bool
Renderer::oit_buffers (unsigned w, unsigned h)
{
	uint32_t zero = 0;

	bool same = oit.w == w && oit.h == h;

	if (oit.heads && same)
		return true;
	/* A new size: as many fragments as at first; else those it grew to */
	destroy_oit ();
	if (!oit.max_nodes || !same)
		oit.max_nodes = std::min ((uint32_t) (w * h * 2), OIT_MAX_NODES);

	std::vector<uint32_t> zeros ((size_t) w * h, 0);
	glGenTextures (1, &oit.heads);
	glBindTexture (GL_TEXTURE_2D, oit.heads);
	glTexStorage2D (GL_TEXTURE_2D, 1, GL_R32UI, w, h);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data ());

	/* As the scene's, for the blits */
	glGenTextures (1, &oit.depth_tex);
	glBindTexture (GL_TEXTURE_2D, oit.depth_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glGenTextures (1, &oit.color_tex);
	glBindTexture (GL_TEXTURE_2D, oit.color_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture (GL_TEXTURE_2D, 0);

	glGenFramebuffers (1, &oit.depth_fb);
	glBindFramebuffer (GL_FRAMEBUFFER, oit.depth_fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, oit.depth_tex, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	bool ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glGenFramebuffers (1, &oit.color_fb);
	glBindFramebuffer (GL_FRAMEBUFFER, oit.color_fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, oit.color_tex, 0);
	ok = ok && glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer (GL_FRAMEBUFFER, fb);

	glGenBuffers (1, &oit.nodes);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, oit.nodes);
	glBufferData (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr) oit.max_nodes * 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
	glGenBuffers (1, &oit.counter);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, oit.counter);
	glBufferData (GL_ATOMIC_COUNTER_BUFFER, sizeof (zero), &zero, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
	if (!ok || glGetError () != GL_NO_ERROR) {
		HL_ERROR ("gaelco: no room for the translucency's %u fragments; each triangle sorted whole",
		          oit.max_nodes);
		destroy_oit ();
		oit.failed = true;
		return false;
	}
	oit.w = w;
	oit.h = h;
	HL_PRINT ("gaelco: translucency sorted at each pixel: %ux%u, %u fragments (%u MB)",
	          w, h, oit.max_nodes, (unsigned) ((uint64_t) oit.max_nodes * 16 >> 20));
	return true;
}

/* The translucent batches, sorted at each pixel (see "Translucency") */
void
Renderer::draw_translucent_per_pixel ()
{
	uint32_t zero = 0;

	/* The opaque polygons' depth, for the collecting shader */
	glBindFramebuffer (GL_READ_FRAMEBUFFER, fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, oit.depth_fb);
	glBlitFramebuffer (0, 0, oit.w, oit.h, 0, 0, oit.w, oit.h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, fb);

	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, oit.counter);
	glBufferSubData (GL_ATOMIC_COUNTER_BUFFER, 0, sizeof (zero), &zero);
	glBindBufferBase (GL_ATOMIC_COUNTER_BUFFER, 0, oit.counter);
	glBindBufferBase (GL_SHADER_STORAGE_BUFFER, 0, oit.nodes);
	glBindImageTexture (0, oit.heads, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
	glActiveTexture (GL_TEXTURE0 + OIT_DEPTH_UNIT);
	glBindTexture (GL_TEXTURE_2D, oit.depth_tex);
	glActiveTexture (GL_TEXTURE0);

	/* Collected: no colour, no depth */
	glUseProgram (collect.id);
	set_scene_uniforms (collect);
	glUniform1i (collect.u_opaque_depth, OIT_DEPTH_UNIT);
	glUniform1ui (collect.u_max_nodes, oit.max_nodes);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask (GL_FALSE);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	for (const Batch &b : frame->batches)
		if (b.translucent)
			draw_batch (collect, b);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* The picture drawn so far, which the lists are blended over */
	glBindFramebuffer (GL_READ_FRAMEBUFFER, fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, oit.color_fb);
	glBlitFramebuffer (0, 0, oit.w, oit.h, 0, 0, oit.w, oit.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, fb);
	glActiveTexture (GL_TEXTURE0 + OIT_BELOW_UNIT);
	glBindTexture (GL_TEXTURE_2D, oit.color_tex);
	glActiveTexture (GL_TEXTURE0);

	/* Resolved over the whole picture */
	glUseProgram (oit.resolve);
	glUniform1i (glGetUniformLocation (oit.resolve, "u_below"), OIT_BELOW_UNIT);
	glBindVertexArray (oit.vao);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glMemoryBarrier (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glBindVertexArray (vao);

	/* Now and then, whether a frame filled the buffer: then more, from
	 * the next frame (it stalls, so not every frame) */
	if (++oit.frames % 120 == 0 && oit.max_nodes < OIT_MAX_NODES) {
		uint32_t used = 0;

		glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, oit.counter);
		glGetBufferSubData (GL_ATOMIC_COUNTER_BUFFER, 0, sizeof (used), &used);
		if (used > oit.max_nodes) {
			HL_PRINT ("gaelco: translucency: %u fragments for %u places, more made", used, oit.max_nodes);
			unsigned w = oit.w, h = oit.h;
			destroy_oit ();
			oit.max_nodes = std::min (std::max (oit.max_nodes * 2, used + used / 4), OIT_MAX_NODES);
			oit.w = w;
			oit.h = h;
		}
	}
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
}

void
Renderer::on_end_frame ()
{
	std::unique_ptr<Frame> next = gpu->take_frame ();
	hl::ScreenLayout layout;
	int dw, dh;
	GLuint crt_fb;

	if (next) {
		frame = std::move (next);
		frame_sorted = false;
	}
	frame_count++;
	draw_scene ();

	/* Too many old ones: made again when needed */
	if (textures.size () > 2048) {
		for (auto &it : textures)
			glDeleteTextures (1, &it.second.id);
		textures.clear ();
	}

	presentation->copy_view ();
	crt_fb = presentation->begin (640, 480);
	hl::Window::drawable_size (&dw, &dh);
	hl::Window::layout (&layout);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	hl::Window::set_output_viewport (layout, dw, dh);
	if (crt_fb) {
		glDisable (GL_SCISSOR_TEST);
		glBindFramebuffer (GL_FRAMEBUFFER, crt_fb);
		glViewport (0, 0, 640, 480);
	}
	glUseProgram (blit_program);
	glBindVertexArray (blit_vao);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, fb_color);
	glUniform1i (u_blit_tex, 0);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	glUseProgram (0);
	glBindTexture (GL_TEXTURE_2D, 0);
	presentation->end (0, dw, dh, layout);
}

} /* namespace gaelco */
