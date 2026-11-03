/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-renderer.c, Copyright (C) 2011-2013, Stefano Teso.
 * The 501 dot patterns (PATTERNS) from Odin, Copyright (C) 2026, Réda Chérif-Touil.
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
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hl/config.h"
#include "hl/input.h"
#include "hl/paths.h"
#include "hl/presentation.h"
#include "hl/profile.h"

#include "mach/hikaru/hikaru-renderer.h"
#include "mach/hikaru/hikaru-renderer-private.h"

namespace hikaru {

#define VP0	VP.scratch
#define MAT0	MAT.scratch
#define TEX0	TEX.scratch
#define LS0	LIT.scratchset

/* A frame's states, a mesh each: more meshes since a recalled material or
 * texhead splits a mesh, and with the finest LODs */
#define MAX_VIEWPORTS	16384
#define MAX_MODELVIEWS	16384
#define MAX_MATERIALS	16384
#define MAX_TEXHEADS	16384
#define MAX_LIGHTSETS	16384
#define MAX_MESHES	16384

#define INV255	(1.0f / 255.0f)

#define isnonnegative(x_) \
	(isfinite(x_) && (x_) >= 0.0)

/****************************************************************************
 Debug
****************************************************************************/

static const struct {
	int32_t min, max;
	uint32_t key;
} debug_controls[] = {
	{  0, 1,     ~0 },
	{  0, 1, SDLK_1 },
	{  0, 1, SDLK_2 },
	{  0, 1, SDLK_3 },
	{ -1, 7, SDLK_v },
	{  0, 1, SDLK_t },
	{  0, 1, SDLK_u },
	{ -1, 7, SDLK_p },
	{  0, 1, SDLK_i },
	{  0, 3, SDLK_j }, 
	{  0, 1, SDLK_l },
	{  0, 1, SDLK_a },
	{  0, 1, SDLK_d },
	{  0, 1, SDLK_s },
	{  0, 1, SDLK_g },
};

static void
init_debug_flags (Renderer *hr)
{
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++)
		hr->debug.flags[i] = debug_controls[i].min;

	hr->debug.flags[HR_DEBUG_LOG] =
		hl_util_get_bool_option ("HR_LOG", false) ? 1 : 0;
	hr->debug.no_draw = hl_util_get_bool_option ("HR_NO_DRAW", false);
	hr->debug.stats = hl_util_get_bool_option ("HR_STATS", false);
	hr->debug.sync = hl_util_get_bool_option ("HR_SYNC", false);
	hr->debug.draws = hr->debug.tris = 0;
	hr->debug.max_tris = hr->debug.max_instances = hr->debug.max_tris_mesh = 0;
	hr->debug.slow_mesh = hr->debug.uploads = 0;
	hr->debug.gpu_ms = hr->debug.slow_ms = 0.0;
}

static void
update_debug_flags (Renderer *hr)
{
	char *msg = hr->message;
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++) {
		uint32_t key = debug_controls[i].key;
		if (key != ~0 && hl::Input::key (key)) {
			hr->debug.flags[i] += 1;
			if (i == HR_DEBUG_NO_MIPMAPS)
			if (hr->debug.flags[i] > debug_controls[i].max)
				hr->debug.flags[i] = debug_controls[i].min;
		}
		msg += sprintf (msg, "%d|", hr->debug.flags[i]);
	}
}

/****************************************************************************
 Utils
****************************************************************************/

static GLuint
get_uniform_loc (GLuint prog, const char *id)
{
	GLuint loc;

	loc = glGetUniformLocation (prog, id);
	HL_ASSERT_NO_GL_ERROR ();
	if (loc == (GLuint) -1)
		HL_ABORT ("uniform '%s' has no location!", id);

	return loc;
}

/****************************************************************************
 Textures
****************************************************************************/

static bool
is_texhead_eq (Renderer *hr,
               hikaru_texhead_t *a, hikaru_texhead_t *b)
{
	return (a->format == b->format) &&
	       (a->logw == b->logw) &&
	       (a->logh == b->logh) &&
	       (a->bank == b->bank) &&
	       (a->slotx == b->slotx) &&
	       (a->sloty == b->sloty);
}

static void
destroy_texture (hikaru_texture_t *tex)
{
	if (tex->id) {
		glDeleteTextures (1, &tex->id);
		HL_ASSERT_NO_GL_ERROR ();
	}
	memset ((void *) tex, 0, sizeof (hikaru_texture_t));
}

static uint16_t
abgr1111_to_rgba4444 (uint32_t texel)
{
	static const uint16_t table[16] = {
		0x0000, 0xF000, 0x0F00, 0xFF00,
		0x00F0, 0xF0F0, 0x0FF0, 0xFFF0,
		0x000F, 0xF00F, 0x0F0F, 0xFF0F,
		0x00FF, 0xF0FF, 0x0FFF, 0xFFFF,
	};
	return table[texel & 15];
}

#define PUT16(x, y, t) \
	*(uint16_t *) &(data[(y)*stride*2 + (x)*2]) = (t)

/* w x h halfwords at (basex, basey), as the IDMA put them, each a block of
 * 2x2 texels: a texture of 2w x 2h. Valkyrie read half of each row only,
 * the texture w wide: the right half of SGNASCAR's font was missing, its
 * glyphs drawn mirrored from the left half */
static void *
decode_texture_abgr1111 (hl::Buffer *texram,
                         uint32_t w, uint32_t h,
                         uint32_t basex, uint32_t basey)
{
	uint32_t x, y, stride = w * 2;
	uint8_t *data;

	data = (uint8_t *) malloc ((w * 2) * (h * 2) * 2);
	if (!data)
		return NULL;

	for (y = 0; y < h; y++) {
		/* Two halfwords at a time, x the first's */
		for (x = 0; x < w; x += 2) {
			uint32_t offs = (basey + y) * 4096 + (basex + x) * 2;
			uint32_t texels = texram->get (4, offs);
			uint32_t tx = x * 2;
			PUT16 (tx + 2, y*2 + 0, abgr1111_to_rgba4444 (texels >> 28));
			PUT16 (tx + 3, y*2 + 0, abgr1111_to_rgba4444 (texels >> 24));
			PUT16 (tx + 2, y*2 + 1, abgr1111_to_rgba4444 (texels >> 20));
			PUT16 (tx + 3, y*2 + 1, abgr1111_to_rgba4444 (texels >> 16));
			PUT16 (tx + 0, y*2 + 0, abgr1111_to_rgba4444 (texels >> 12));
			PUT16 (tx + 1, y*2 + 0, abgr1111_to_rgba4444 (texels >> 8));
			PUT16 (tx + 0, y*2 + 1, abgr1111_to_rgba4444 (texels >> 4));
			PUT16 (tx + 1, y*2 + 1, abgr1111_to_rgba4444 (texels >> 0));
		}
	}
	return (void *) data;
}

#undef PUT16

/* The bound texture's wrap, as the texhead's 2C1 has it: U wraps (bit 22)
 * or is clamped, mirrored (bit 24) or repeated; V likewise (bits 23, 25).
 * Valkyrie mirrored them all: BRAVEFF's "INSERT COIN(S)" came out
 * backwards. The other order smears AIRTRIX's walls */
static void
set_texture_wrap (hikaru_texhead_t *th)
{
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
	                 !th->wrapu ? GL_CLAMP_TO_EDGE :
	                 th->repeatu ? GL_MIRRORED_REPEAT : GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
	                 !th->wrapv ? GL_CLAMP_TO_EDGE :
	                 th->repeatv ? GL_MIRRORED_REPEAT : GL_REPEAT);
	HL_ASSERT_NO_GL_ERROR ();
}

/* TEXTURE_SCALE: a level as RGBA8, the bytes R, G, B, A in a word, as the
 * GL calls of upload_texture read it: w x h texels, 2w x 2h for ABGR1111 */
static bool
decode_level_rgba8 (hl::Buffer *texram, unsigned format, uint32_t w, uint32_t h,
                    uint32_t basex, uint32_t basey, std::vector<uint32_t> &out,
                    uint32_t *out_w, uint32_t *out_h)
{
	auto expand4 = [] (uint32_t v) { return (v & 15) * 17; };
	auto expand5 = [] (uint32_t v) { v &= 31; return (v << 3) | (v >> 2); };
	auto from4444 = [&] (uint32_t t) {
		return expand4 (t) | (expand4 (t >> 4) << 8) |
		       (expand4 (t >> 8) << 16) | (expand4 (t >> 12) << 24);
	};
	const uint8_t *base = (const uint8_t *) texram->ptr (0);
	uint32_t x, y, i;

	if (format == HIKARU_FORMAT_ABGR1111) {
		uint16_t *data = (uint16_t *) decode_texture_abgr1111 (texram, w, h, basex, basey);
		if (!data)
			return false;
		out.resize ((size_t) w * h * 4);
		for (i = 0; i < w * h * 4; i++)
			out[i] = from4444 (data[i]);
		free (data);
		*out_w = w * 2;
		*out_h = h * 2;
		return true;
	}

	out.resize ((size_t) w * h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint16_t t;
			uint32_t c;

			memcpy (&t, base + (basey + y) * 4096 + (basex + x) * 2, 2);
			switch (format) {
			case HIKARU_FORMAT_ABGR1555:
				c = expand5 (t) | (expand5 (t >> 5) << 8) | (expand5 (t >> 10) << 16) |
				    ((t & 0x8000) ? 0xFF000000u : 0);
				break;
			case HIKARU_FORMAT_ABGR4444:
				c = from4444 (t);
				break;
			case HIKARU_FORMAT_LA8:
				/* GL_RG's swizzle: L, L, L, A */
				c = (t & 0xFF) * 0x010101u | ((uint32_t) (t >> 8) << 24);
				break;
			default:
				return false;
			}
			out[y * w + x] = c;
		}
	*out_w = w;
	*out_h = h;
	return true;
}

/* Scale2x (AdvMAME2x): each texel four, those at an edge between flat
 * colours taking its neighbours' colour, the edge left sharp, not blurred;
 * the borders clamped */
static void
scale2x (const std::vector<uint32_t> &src, uint32_t w, uint32_t h,
         std::vector<uint32_t> &dst)
{
	uint32_t x, y;

	dst.resize ((size_t) w * h * 4);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint32_t b = src[(y ? y - 1 : 0) * w + x];
			uint32_t d = src[y * w + (x ? x - 1 : 0)];
			uint32_t e = src[y * w + x];
			uint32_t f = src[y * w + MIN2 (x + 1, w - 1)];
			uint32_t hh = src[MIN2 (y + 1, h - 1) * w + x];
			uint32_t *o = &dst[(size_t) y * 2 * w * 2 + x * 2];

			if (b != hh && d != f) {
				o[0] = (d == b) ? d : e;
				o[1] = (b == f) ? f : e;
				o[w * 2] = (d == hh) ? d : e;
				o[w * 2 + 1] = (hh == f) ? f : e;
			} else
				o[0] = o[1] = o[w * 2] = o[w * 2 + 1] = e;
		}
}

/* TEXTURE_SCALE: the bound texture's levels, enlarged s times. Level 0 is
 * the game's level 0 through Scale2x log2 (s) times, the levels below it
 * fewer times; then the game's levels, s times smaller than the texture
 * now: the same level of detail at each distance as without */
static bool
upload_scaled_levels (Renderer *hr, hikaru_texhead_t *th, uint32_t w, uint32_t h,
                      uint32_t num_levels, uint32_t basex, uint32_t basey,
                      uint32_t bank)
{
	const uint32_t log2s = (hr->view.texture_scale >= 4) ? 2 : 1;
	std::vector<uint32_t> pix, up[2];
	uint32_t level, pw, ph, j;

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels - 1 + log2s);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	for (level = 0; level < num_levels; level++) {
		if (!decode_level_rgba8 (hr->texram_src[bank], th->format, w, h,
		                         basex, basey, pix, &pw, &ph))
			return false;
		if (level == 0) {
			scale2x (pix, pw, ph, up[0]);
			if (log2s == 2)
				scale2x (up[0], pw * 2, ph * 2, up[1]);
			for (j = 0; j < log2s; j++) {
				uint32_t k = log2s - 1 - j;	/* up[k]: 2 << k times */
				glTexImage2D (GL_TEXTURE_2D, j, GL_RGBA8,
				              pw << (k + 1), ph << (k + 1), 0,
				              GL_RGBA, GL_UNSIGNED_BYTE, up[k].data ());
			}
		}
		glTexImage2D (GL_TEXTURE_2D, level + log2s, GL_RGBA8, pw, ph, 0,
		              GL_RGBA, GL_UNSIGNED_BYTE, pix.data ());
		HL_ASSERT_NO_GL_ERROR ();

		w >>= 1;
		h >>= 1;
		HL_ASSERT (w && h);

		basex += (2048 - basex) / 2;
		basey += (1024 - basey) / 2;
		bank ^= 1;
	}
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	return true;
}

static GLuint
upload_texture (Renderer *hr, hikaru_texhead_t *th)
{
	static const GLint la8_swizzle[4] = { GL_RED, GL_RED, GL_RED, GL_GREEN };

	uint32_t w, h, num_levels, level, basex, basey, bank;
	GLuint id;

	w = 16 << th->logw;
	h = 16 << th->logh;
	num_levels = hr->debug.flags[HR_DEBUG_NO_MIPMAPS] ? 1 :
	             MIN2 (th->logw, th->logh) + 4;

	get_texhead_coords (&basex, &basey, th);
	bank = th->bank;

	/* Each level within the 2048x1024 TEXRAM: GL reads it through a
	 * pointer, and a texture past the end had the driver read past the
	 * buffer (BRAVEFF: a crash in the driver, then a GPU reset) */
	{
		uint32_t x = basex, y = basey, lw = w, lh = h;
		for (level = 0; level < num_levels; level++) {
			if (x + lw > 2048 || y + lh > 1024) {
				static unsigned logged;
				if (logged++ < 20)
					HL_ERROR ("texture past the TEXRAM, not drawn: %s",
					          get_texhead_str (th));
				return 0;
			}
			lw >>= 1;
			lh >>= 1;
			x += (2048 - x) / 2;
			y += (1024 - y) / 2;
		}
	}

	glGenTextures (1, &id);
	HL_ASSERT_NO_GL_ERROR ();
	hr->debug.uploads++;

	glActiveTexture (GL_TEXTURE0 + 0);
	HL_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, id);
	HL_ASSERT_NO_GL_ERROR ();

	set_texture_wrap (th);

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	HL_ASSERT_NO_GL_ERROR ();

	/* XXX hack to make textures slightly less blurry. */
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -1);
	HL_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels - 1);
	HL_ASSERT_NO_GL_ERROR ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	HL_ASSERT_NO_GL_ERROR ();

	/* TEXTURE_SCALE: its levels decoded, enlarged by Scale2x */
	if (hr->view.texture_scale > 1) {
		if (!upload_scaled_levels (hr, th, w, h, num_levels, basex, basey, bank))
			goto fail;
		return id;
	}

	for (level = 0; level < num_levels; level++) {
		void *data = hr->texram_src[bank]->ptr (0);

		switch (th->format) {
		case HIKARU_FORMAT_ABGR1555:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			HL_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGB5_A1,
			              w, h, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
			              data);
			HL_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR4444:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			HL_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA4,
			              w, h, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV,
			              data);
			HL_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_LA8:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			HL_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA8,
			              w, h, 0,
			              GL_RG, GL_UNSIGNED_BYTE,
			              data);
			HL_ASSERT_NO_GL_ERROR ();

			glTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA,
			                  la8_swizzle);
			HL_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR1111:
			data = decode_texture_abgr1111 (hr->texram_src[bank],
			                                w, h, basex, basey);
			if (!data)
				goto fail;

			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA4,
			              w * 2, h * 2, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV,
			              data);
			HL_ASSERT_NO_GL_ERROR ();

			free (data);
			break;
		default:
			goto fail;
		}

		w >>= 1;
		h >>= 1;
		HL_ASSERT (w && h);

		basex += (2048 - basex) / 2;
		basey += (1024 - basey) / 2;
		bank ^= 1;
	}

	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	HL_ASSERT_NO_GL_ERROR ();

	return id;

fail:
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	HL_ASSERT_NO_GL_ERROR ();

	glDeleteTextures (1, &id);
	HL_ASSERT_NO_GL_ERROR ();

	return 0;
}

/* The 16x16 slots of the TEXRAM a texture covers, num_levels of its levels
 * (each in the other bank, where upload_texture reads it): fn (bank, x, y) */
template <typename F> static void
for_each_slot (hikaru_texhead_t *th, unsigned num_levels, F fn)
{
	uint32_t w = 16 << th->logw, h = 16 << th->logh;
	uint32_t x, y, bank = th->bank, level, sx, sy;

	get_texhead_coords (&x, &y, th);
	for (level = 0; level < num_levels && w && h; level++) {
		for (sy = y / 16; sy <= (y + h - 1) / 16 && sy < 0x40; sy++)
			for (sx = x / 16; sx <= (x + w - 1) / 16 && sx < 0x80; sx++)
				fn (bank, sx, sy);
		w >>= 1;
		h >>= 1;
		x += (2048 - x) / 2;
		y += (1024 - y) / 2;
		bank ^= 1;
	}
}

/* The writes to a texture's slots, summed: it changes with any of them */
static uint32_t
get_texture_writes (Renderer *hr, hikaru_texhead_t *th)
{
	unsigned num_levels = hr->debug.flags[HR_DEBUG_NO_MIPMAPS] ? 1 :
	                      MIN2 (th->logw, th->logh) + 4;
	uint32_t sum = 0;

	for_each_slot (th, num_levels, [&] (uint32_t bank, uint32_t x, uint32_t y) {
		sum += hr->textures.slot_writes[bank][y][x];
	});
	return sum;
}

hikaru_texture_t *
get_texture (Renderer *hr, hikaru_texhead_t *th)
{
	hikaru_texture_t *cached;
	uint32_t bank, slotx, sloty, writes;
	GLuint id;

	bank  = th->bank;
	slotx = th->slotx;
	sloty = th->sloty;

	if (slotx < 0x80 || sloty < 0xC0)
		return NULL;

	slotx -= 0x80;
	sloty -= 0xC0;

	writes = get_texture_writes (hr, th);
	cached = &hr->textures.cache[bank][sloty][slotx];
	if (is_texhead_eq (hr, th, &cached->th) && cached->gen == writes &&
	    cached->scale == hr->view.texture_scale)
		return cached;

	destroy_texture (cached);

	id = upload_texture (hr, th);
	if (!id) {
		destroy_texture (cached);
		return NULL;
	}

	cached->th = *th;
	cached->id = id;
	cached->gen = writes;
	cached->scale = hr->view.texture_scale;
	cached->wrap = 0xFF;

	hr->textures.is_clear[bank] = false;
	return cached;
}

static void
clear_texcache_bank (Renderer *hr, unsigned bank)
{
	unsigned x, y;

	if (hr->textures.is_clear[bank])
		return;
	hr->textures.is_clear[bank] = true;

	/* Free all allocated surfaces. */
	for (y = 0; y < 0x40; y++)
		for (x = 0; x < 0x80; x++)
			destroy_texture (&hr->textures.cache[bank][y][x]);

	/* Zero out the cache itself, to avoid spurious hits. Note that
	 * texture RAM origin is (80,C0), so (slotx, sloty) will never match
	 * a zeroed out cache entries. */
	memset ((void *) &hr->textures.cache[bank], 0, sizeof (hr->textures.cache[bank]));
}

void
Renderer::invalidate_texcache (hikaru_texhead_t *th)
{
	hl::Renderer *rend = this;
	Renderer *hr = (Renderer *) rend;

	HL_ASSERT (hr);

	if (th == NULL) {
		clear_texcache_bank (hr, 0);
		clear_texcache_bank (hr, 1);
		return;
	}

	/* The slots the IDMA wrote, all the levels it may have copied: the
	 * textures there are uploaded again at their next use */
	for_each_slot (th, 16, [&] (uint32_t bank, uint32_t x, uint32_t y) {
		hr->textures.slot_writes[bank][y][x]++;
		if (hr->thread)
			hr->texram_dirty[bank][y][x] = true;
	});
}

/****************************************************************************
 State
****************************************************************************/

/* The scene's far plane, past the viewports' own (DRAW_DISTANCE draws
 * farther than the games) */
#define SCENE_FAR	1e5f

static const char *mesh_vs_source =
"#version 140									\n \
										\n \
#extension GL_ARB_explicit_attrib_location : require				\n \
										\n \
%s										\n \
										\n \
uniform mat4 u_projection;							\n \
uniform mat4 u_modelview;							\n \
uniform mat3 u_normal;								\n \
uniform mat4 u_link_modelview;							\n \
uniform mat3 u_link_normal;							\n \
										\n \
layout(location = 0) in vec3 i_position;					\n \
layout(location = 1) in vec3 i_normal;						\n \
layout(location = 2) in vec3 i_diffuse;						\n \
layout(location = 3) in vec3 i_ambient;						\n \
layout(location = 4) in vec4 i_specular;					\n \
layout(location = 5) in vec3 i_unknown;						\n \
layout(location = 6) in vec2 i_texcoords;					\n \
layout(location = 7) in float i_alpha;						\n \
layout(location = 8) in float i_link;						\n \
										\n \
out vec4 p_position;								\n \
out vec3 p_normal;								\n \
out vec3 p_diffuse;								\n \
out vec4 p_specular;								\n \
out vec3 p_ambient;								\n \
out vec3 p_unknown;								\n \
out vec2 p_texcoords;								\n \
out float p_alpha;								\n \
										\n \
void main (void) {								\n \
	/* The vertices with S take the matrix pushed before (161) */		\n \
	bool linked = i_link > 0.5;						\n \
	mat4 modelview = linked ? u_link_modelview : u_modelview;		\n \
	mat3 normal = linked ? u_link_normal : u_normal;			\n \
										\n \
	p_position = modelview * vec4 (i_position, 1.0);			\n \
	gl_Position = u_projection * p_position;				\n \
										\n \
	p_normal = normal * i_normal;						\n \
										\n \
	p_diffuse = i_diffuse;							\n \
	p_ambient = i_ambient;							\n \
	p_specular = i_specular;						\n \
	p_unknown = i_unknown / 255.0;						\n \
	p_texcoords = i_texcoords;						\n \
	p_alpha = i_alpha;							\n \
}";

static const char *mesh_fs_source =
"#version 140									\n \
										\n \
out vec4 frag_color;								\n \
										\n \
%s										\n \
										\n \
struct light_t {								\n \
	vec3 position;								\n \
	vec3 direction;								\n \
	vec3 diffuse;								\n \
	vec2 cone;	/* a spot's: the edge's cosine, 1 / the rise */	\n \
	vec2 extents;								\n \
	float ambient;	/* 051's level / 16 */				\n \
	float table[16];	/* a spot's, from its cone's edge (194) */	\n \
};										\n \
										\n \
uniform light_t		u_lights[4];						\n \
uniform vec3		u_ambient;						\n \
uniform sampler2D	u_texture;						\n \
uniform vec2		u_fog;							\n \
uniform vec3		u_fog_color;						\n \
uniform vec2		u_alpha_range;						\n \
uniform vec3		u_depth;						\n \
uniform ivec2		u_pattern;						\n \
									\n \
#if COLLECT									\n \
/* Translucency: the fragments in their pixels' lists (#version 430) */	\n \
layout (binding = 0, offset = 0) uniform atomic_uint u_count;			\n \
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;		\n \
layout (std430, binding = 0) buffer Nodes { uvec4 nodes[]; };		\n \
uniform sampler2D	u_opaque_depth;						\n \
uniform uint		u_max_nodes;						\n \
uniform uint		u_tag;							\n \
#endif										\n \
										\n \
in vec4 p_position;								\n \
in vec3 p_normal;								\n \
in vec3 p_diffuse;								\n \
in vec4 p_specular;								\n \
in vec3 p_ambient;								\n \
in vec3 p_unknown;								\n \
in vec2 p_texcoords;								\n \
in float p_alpha;								\n \
										\n \
void										\n \
apply_light (inout vec3 diffuse,						\n \
             inout vec3 specular,						\n \
             in light_t light,							\n \
             in vec3 normal,							\n \
             in int type,							\n \
             in int att_type,							\n \
             in int has_specular)						\n \
{										\n \
	vec3 light_direction;							\n \
	float distance, attenuation, intensity;					\n \
									\n \
	/* A parallel light without a direction: its whole colour, every	\n \
	 * side */								\n \
	if (type == 3) {							\n \
		diffuse += light.diffuse;					\n \
		return;								\n \
	}									\n \
										\n \
	if (type == 0) {							\n \
		light_direction = normalize (light.direction);			\n \
		distance = 0.001;						\n \
	} else {								\n \
		vec3 delta = light.position - p_position.xyz;			\n \
		distance = length (delta);					\n \
		light_direction = normalize (delta);				\n \
	}									\n \
										\n \
	if (att_type == 1)							\n \
		distance = distance*distance;					\n \
	else if (att_type == 2)							\n \
		distance = 1.0 / distance;					\n \
	else if (att_type == 3)							\n \
		distance = 1.0 / (distance*distance);				\n \
	attenuation = light.extents.x * (light.extents.y + distance);		\n \
	attenuation = clamp (attenuation, 0.0, 1.0);				\n \
										\n \
	intensity = max (dot (normal, light_direction), 0.0);			\n \
	/* A spot's direction goes from the light to the scene (AIRTRIX's	\n \
	 * skater: lights above it pointing down), unlike a directional	\n \
	 * light's, which points to the light. Its light rises from the	\n \
	 * edge of its cone over a width (451) */				\n \
	if (type == 2) {							\n \
		vec3 spot_direction = normalize (light.direction);		\n \
		float c = dot (spot_direction, -light_direction);		\n \
		/* Over the rise, the spot's table (194, US patent 6,333,742 of	\n \
		 * SEGA): sixteen points from the cone's edge to its full light,	\n \
		 * straight lines between them, nothing outside the edge */	\n \
		float where = 15.0 * clamp ((c - light.cone.x) * light.cone.y, 0.0, 1.0);	\n \
		int point = min (int (where), 14);				\n \
		attenuation *= (where > 0.0) ?					\n \
		               mix (light.table[point], light.table[point + 1], where - float (point)) : 0.0;	\n \
	}									\n \
										\n \
	/* No material diffuse here: it is the base colour (see main). The	\n \
	 * light's ambient level (051) adds a share of its colour, facing it	\n \
	 * or not, cut by the distance and the cone as the rest */		\n \
	diffuse += attenuation * (intensity + light.ambient) * light.diffuse;	\n \
										\n \
	/* The light's own colour, if the material shines */			\n \
	if (p_specular.a > 0.0 && intensity > 0.0) {						\n \
		vec3 view_direction = normalize (-p_position.xyz);				\n \
		vec3 reflect_direction = reflect (-light_direction, normal);			\n \
		float angle = max (dot (view_direction, reflect_direction), 0.0);		\n \
		specular += attenuation * light.diffuse * pow (angle, 255.0 * p_specular.a);	\n \
	}											\n \
}										\n \
										\n \
void										\n \
main (void)									\n \
{										\n \
	vec4 color;								\n \
	vec3 base;								\n \
	float alpha = p_alpha;							\n \
										\n \
	/* The slope of 1/z across a pixel, for the depth bias's slope	\n \
	 * mode: taken here, before any discard */				\n \
	float inv_z = 1.0 / max (abs (p_position.z), 1e-6);			\n \
	float slope = max (abs (dFdx (inv_z)), abs (dFdy (inv_z)));		\n \
										\n \
	/* The normal lit. Two-sided lighting lights a face seen from behind	\n \
	 * on its back: its normal turned to the eye when the triangle's own	\n \
	 * (from the position's slopes, here too before any discard) faces	\n \
	 * away. The side away from the light stays dark: abs () lit both	\n \
	 * sides, and PHARRIER's asteroids (culled, two-sided) showed no	\n \
	 * shade, half as bright again as the reference's */			\n \
	vec3 normal = normalize (p_normal);					\n \
#if HAS_TWO_SIDED_LIGHTING							\n \
	vec3 face = cross (dFdx (p_position.xyz), dFdy (p_position.xyz));	\n \
	if (dot (face, p_position.xyz) > 0.0)					\n \
		face = -face;							\n \
	if (dot (normal, face) < 0.0)						\n \
		normal = -normal;						\n \
#endif										\n \
										\n \
	/* The base colour: the texture's, by 2C1's mode, with the diffuse	\n \
	 * (091) and the ambient (291) */					\n \
#if HAS_TEXTURE									\n \
	vec4 texel = texture (u_texture, p_texcoords);				\n \
#if HAS_ALPHA_MODE								\n \
	base = mix (p_diffuse, texel.rgb, alpha);				\n \
	alpha = texel.a;							\n \
#elif TEX_MODE == 1								\n \
	base = mix (p_diffuse, texel.rgb, texel.a);				\n \
#elif TEX_MODE == 2								\n \
	base = p_ambient * texel.rgb;						\n \
	alpha *= texel.a;							\n \
#elif TEX_MODE == 4								\n \
	base = p_diffuse;							\n \
	alpha *= texel.a;							\n \
#elif TEX_MODE == 5								\n \
	base = mix (p_diffuse, p_ambient, texel.rgb);				\n \
	alpha *= texel.a;							\n \
#else										\n \
	base = texel.rgb;							\n \
	alpha *= texel.a;							\n \
#endif										\n \
#elif HAS_ALPHA_MODE								\n \
	base = mix (p_diffuse, p_ambient, alpha);				\n \
	alpha = 1.0;								\n \
#else										\n \
	base = p_diffuse;							\n \
#endif										\n \
										\n \
#if HAS_LIGHTING								\n \
	vec3 diffuse  = vec3 (0.0);						\n \
	vec3 specular = vec3 (0.0);						\n \
	/* The ambient: in mode 1 the scene's (011) times the material's	\n \
	 * ambient colour (291) and reflectance (691); in mode 2 the 691	\n \
	 * alone (PHARRIER's flames, orange: magenta with the 291). Without	\n \
	 * the 291, PHARRIER's asteroids (291 olive, 691 FF) were twice as	\n \
	 * bright as the board's and their shade grey, the board's olive */	\n \
#if HAS_FIXED_AMBIENT								\n \
	vec3 ambient  = p_unknown;						\n \
#else										\n \
	vec3 ambient  = u_ambient * p_ambient * p_unknown;			\n \
#endif										\n \
										\n \
#if HAS_LIGHT0									\n \
	apply_light (diffuse, specular, u_lights[0], normal, LIGHT0_TYPE, LIGHT0_ATT_TYPE, HAS_LIGHT0_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT1									\n \
	apply_light (diffuse, specular, u_lights[1], normal, LIGHT1_TYPE, LIGHT1_ATT_TYPE, HAS_LIGHT1_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT2									\n \
	apply_light (diffuse, specular, u_lights[2], normal, LIGHT2_TYPE, LIGHT2_ATT_TYPE, HAS_LIGHT2_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT3									\n \
	apply_light (diffuse, specular, u_lights[3], normal, LIGHT3_TYPE, LIGHT3_ATT_TYPE, HAS_LIGHT3_SPECULAR);		\n \
#endif										\n \
										\n \
	/* The specular colour has 6 bits (491: 3F at most) */			\n \
	color = vec4 (base * (ambient + diffuse) +				\n \
	              p_specular.rgb * (255.0 / 64.0) * specular, alpha);	\n \
#else										\n \
	/* Unlit: the base colour (AIRTRIX's shadows, black, a pink ambient;	\n \
	 * its OHOSOHO sign, magenta, a white one) */				\n \
	color = vec4 (base, alpha);						\n \
#endif										\n \
#if HAS_WEIGHT									\n \
	/* Blended by its alpha: weighted by it before the clamp (at the	\n \
	 * end), so that a lit colour above 1 keeps its excess: the veils of	\n \
	 * PHARRIER's logo galaxy burn to white on the board */		\n \
	color.a = clamp (color.a, 0.0, 1.0);					\n \
#else										\n \
	color = clamp (color, 0.0, 1.0);					\n \
#endif										\n \
										\n \
#if HAS_PATTERN									\n \
	/* The dot pattern (501): the pixels of each 4 x 4 square of the	\n \
	 * board's screen its bits leave out are not drawn, bit 0 at the	\n \
	 * top left; u_pattern.y is the render scale, so that the dots keep	\n \
	 * the board's size (the scene's 480 rows, a multiple of 4, end on a	\n \
	 * square's top row) */							\n \
	ivec2 dot = ivec2 (gl_FragCoord.xy) / u_pattern.y & 3;			\n \
	if (((u_pattern.x >> ((3 - dot.y) * 4 + dot.x)) & 1) == 0)		\n \
		discard;							\n \
#endif										\n \
									\n \
#if HAS_ALPHA_TEST								\n \
	/* Punch-through: in its range, opaque; out, not drawn */		\n \
	if (color.a < u_alpha_range.x || color.a > u_alpha_range.y)		\n \
		discard;							\n \
	color.a = 1.0;								\n \
#endif										\n \
										\n \
	/* The depth, logarithmic in the eye's distance. The projection's,	\n \
	 * the viewports' near plane at 0.01, stepped by z^2 / near / 2^24 in	\n \
	 * a 24-bit buffer, 0.015 at 50 units: SWRACER's pod engines		\n \
	 * z-fought. This one steps by about z / 2^20. The mesh's depth bias	\n \
	 * moves it nearer: 1/z up by a part of itself (u_depth.y), or by	\n \
	 * its slope across a pixel times u_depth.z */				\n \
	float z = abs (p_position.z);						\n \
	z = z / (1.0 + u_depth.y + u_depth.z * slope * z);			\n \
	float frag_depth = log2 (1.0 + z) * u_depth.x;				\n \
	gl_FragDepth = frag_depth;						\n \
										\n \
#if HAS_FOG									\n \
	/* The viewport's fog (621): linear in the eye's depth, clear up to	\n \
	 * u_fog.x, its colour whole 1 / u_fog.y farther (fog_params) */	\n \
	float fog = clamp ((abs (p_position.z) - u_fog.x) * u_fog.y, 0.0, 1.0);	\n \
	frag_color = vec4 (mix (color.rgb, u_fog_color, fog), color.a);	\n \
#else										\n \
	frag_color = color;							\n \
#endif										\n \
									\n \
#if HAS_CUE									\n \
	/* The viewport's depth queue without a colour (621 D): the pixel	\n \
	 * fades into what is behind it, linearly in the eye's depth from	\n \
	 * u_fog.x on, over 1 / u_fog.y; the blending takes its alpha */	\n \
	float cue = clamp ((abs (p_position.z) - u_fog.x) * u_fog.y, 0.0, 1.0);	\n \
	if (cue >= 1.0)								\n \
		discard;							\n \
	frag_color.a = 1.0 - cue;						\n \
#endif										\n \
#if HAS_WEIGHT									\n \
	frag_color.rgb = clamp (frag_color.rgb * frag_color.a, 0.0, 1.0);	\n \
#endif										\n \
									\n \
#if COLLECT									\n \
	/* Translucency: the fragment into its pixel's list, unless the	\n \
	 * opaque meshes hide it (the depth test is the shader's own here:	\n \
	 * the buffer's runs after it, which would keep what is hidden) */	\n \
	ivec2 pixel = ivec2 (gl_FragCoord.xy);					\n \
	if (frag_depth > texelFetch (u_opaque_depth, pixel, 0).r + 1e-7)	\n \
		discard;							\n \
	uint node = atomicCounterIncrement (u_count);				\n \
	if (node < u_max_nodes) {						\n \
		uint next = imageAtomicExchange (u_heads, pixel, node + 1u);	\n \
		nodes[node] = uvec4 (packUnorm4x8 (clamp (frag_color, 0.0, 1.0)),	\n \
		                     floatBitsToUint (frag_depth), next,		\n \
		                     u_tag + (uint (gl_PrimitiveID) << 2));		\n \
	}									\n \
#endif										\n \
}";

static hikaru_light_att_t
get_light_attenuation_type (hikaru_light_t *lit)
{
	if (lit->att_type == 0 &&
	    lit->attenuation[0] == 1.0f &&
	    lit->attenuation[1] == 1.0f)
		return HIKARU_LIGHT_ATT_INF;
	return (hikaru_light_att_t) lit->att_type;
}

static hikaru_light_type_t
get_light_type (hikaru_light_t *lit)
{
	/* Violated by BRAVEFF, in the intro sequence; it recalls a lightset
	 * at the wrong base, and poof. */
	if (!lit->has_direction && !lit->has_position)
		HL_ERROR_FEW ("recalling light with no direction nor position!");
	/* A parallel light sent with a null direction lights every side, as
	 * Odin's author, Réda Chérif-Touil, saw on a BRAVEFF cabinet (the
	 * light its stages add to every lightset); normalized, the shader's
	 * direction was no number */
	if (get_light_attenuation_type (lit) == HIKARU_LIGHT_ATT_INF)
		return (lit->direction[0] == 0.0f && lit->direction[1] == 0.0f &&
		        lit->direction[2] == 0.0f) ?
		       HIKARU_LIGHT_TYPE_UNDIRECTED : HIKARU_LIGHT_TYPE_DIRECTIONAL;
	/* A spot has its cone (451); without, a light at a point */
	else if (lit->has_direction && lit->has_position && lit->has_specular)
		return HIKARU_LIGHT_TYPE_SPOT;
	else if (lit->has_position)
		return HIKARU_LIGHT_TYPE_POSITIONAL;
	return HIKARU_LIGHT_TYPE_DIRECTIONAL;
}

/* A depth queue without a colour (621 D, type 0, its start and range
 * given) fades the meshes that do not blend into what is behind them: the
 * background is drawn first. PHARRIER's library builds it (0C015A3E); its
 * demo play loses the far scenery and keeps the mountains, translucent, as
 * Odin's author, Réda Chérif-Touil, saw on a cabinet */
static bool
is_faded_by_depth (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_viewport_t *vp;

	if (mesh->vp_index == ~0u || hr->debug.flags[HR_DEBUG_FOG])
		return false;
	vp = &hr->vp_list[mesh->vp_index];
	return !vp->depth.q_enabled && vp->depth.q_type == 0 &&
	       vp->depth.density > 0.0f && vp->depth.bias > 0.0f &&
	       mesh->polytype != HIKARU_POLYTYPE_BACKGROUND &&
	       mesh->polytype != HIKARU_POLYTYPE_TRANSLUCENT;
}

/* The blended meshes (background, translucent) of the modes that weight the
 * source by its alpha (set_blending's 0 and 2): the shader does it before
 * the clamp, the blending takes the source as it is, as Odin's author found
 * the board does */
static bool
weighs_by_alpha (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned mode;

	if (mesh->polytype != HIKARU_POLYTYPE_BACKGROUND &&
	    mesh->polytype != HIKARU_POLYTYPE_TRANSLUCENT)
		return false;
	mode = mesh->mat_index != ~0u ? hr->mat_list[mesh->mat_index].blending_mode : 0;
	return mode == 0 || mode == 2;
}

static hikaru_glsl_variant_t
get_glsl_variant (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_material_t *mat	= &hr->mat_list[mesh->mat_index];
	hikaru_lightset_t *ls	= &hr->ls_list[mesh->ls_index];
	hikaru_glsl_variant_t variant;

	HL_ASSERT (mesh->vp_index != ~0);

	variant.full = 0;

	if (mesh->mat_index == ~0) {
		HL_ERROR ("no material.");
		return variant;
	}
	if (mesh->tex_index == ~0) {
		HL_ERROR ("no texhead.");
		return variant;
	}
	if (mesh->ls_index == ~0) {
		HL_ERROR ("no lightset.");
		return variant;
	}

	variant.has_texture	= mat->has_texture &&
	                          !hr->debug.flags[HR_DEBUG_NO_TEXTURES];

	if (variant.has_texture)
		variant.tex_mode = hr->tex_list[mesh->tex_index].mode;
	variant.has_alpha_mode	= mat->has_alpha;
	variant.has_alpha_test	= mesh->has_alpha_test;
	/* 0 and 16 are every pixel */
	variant.has_pattern	= (mesh->pattern & 15) != 0;
	variant.has_cue		= is_faded_by_depth (hr, mesh);
	variant.collect		= hr->oit.collecting;
	variant.has_weight	= weighs_by_alpha (hr, mesh);
	/* With all its lights off, a lit material still takes the ambient */
	variant.has_lighting	= mat->shading_mode != 0 &&
	                          !hr->debug.flags[HR_DEBUG_NO_LIGHTING];

	/* The viewport's fog, on the materials that take it (881 z clear);
	 * the debug key turns it off */
	variant.has_fog		= hr->vp_list[mesh->vp_index].depth.q_enabled &&
	                          mat->depth_blend == 0 &&
	                          !hr->debug.flags[HR_DEBUG_FOG];

	if (!variant.has_lighting)
		return variant;

	variant.has_fixed_ambient	= mat->shading_mode == 2;

	variant.has_light0		= !(ls->mask & (1 << 0));
	variant.light0_type		= get_light_type (&ls->lights[0]);
	variant.light0_att_type		= get_light_attenuation_type (&ls->lights[0]);
	variant.has_light0_specular	= ls->lights[0].has_specular;

	variant.has_light1		= !(ls->mask & (1 << 1));
	variant.light1_type		= get_light_type (&ls->lights[1]);
	variant.light1_att_type		= get_light_attenuation_type (&ls->lights[1]);
	variant.has_light1_specular	= ls->lights[1].has_specular;

	variant.has_light2		= !(ls->mask & (1 << 2));
	variant.light2_type		= get_light_type (&ls->lights[2]);
	variant.light2_att_type		= get_light_attenuation_type (&ls->lights[2]);
	variant.has_light2_specular	= ls->lights[2].has_specular;

	variant.has_light3		= !(ls->mask & (1 << 3));
	variant.light3_type		= get_light_type (&ls->lights[3]);
	variant.light3_att_type		= get_light_attenuation_type (&ls->lights[3]);
	variant.has_light3_specular	= ls->lights[3].has_specular;

	if (mesh->has_two_sided_lighting)
		variant.has_two_sided_lighting = 1;

	return variant;
}

#define MAX_PROGRAMS	512
/* Translucency's copy of the opaque depth, for the collecting shaders */
#define OIT_DEPTH_UNIT	7

/* What a program's uniforms hold, as the upload_* functions below sent
 * them (their inputs): the same not sent again. BRAVEFF draws up to 1900
 * meshes a frame, some thirty uniforms each: the driver's work for them took
 * the drawing thread's whole frame */
struct MeshSent {
	bool viewport, lightset, modelview, alpha, depth, pattern, texture;
	struct {
		hikaru_viewport_t vp;
		float scene_w, distance;
		unsigned scale;
	} viewport_was;
	struct {
		hikaru_lightset_t ls;
		uint16_t tables[4][16];
		float ambient[3];
		bool no_ambient, no_diffuse;
	} lightset_was;
	struct {
		mtx4x4f_t mtx, link;
	} modelview_was;
	float alpha_was[2];
	float depth_was[3];
	GLint pattern_was[2];
};

/* True when now differs from what the program holds (then kept) */
template <typename T> static bool
differs (bool &known, T &held, const T &now)
{
	if (known && !memcmp ((const void *) &held, (const void *) &now, sizeof (T)))
		return false;
	memcpy ((void *) &held, (const void *) &now, sizeof (T));
	known = true;
	return true;
}

static struct {
	hikaru_glsl_variant_t variant;
	GLuint program;
	/* Its uniforms' locations, looked up at its first use only: some
	 * forty lookups by name at each change of program took most of the
	 * drawing thread's time (AIRTRIX) */
	bool has_locs;
	decltype (((Renderer *) NULL)->meshes.locs) locs;
	MeshSent sent;
} program_cache[MAX_PROGRAMS];
unsigned num_programs;

static GLuint
compile_mesh_program (hikaru_glsl_variant_t variant)
{
	static const char *definitions_template =
	"#define HAS_TEXTURE %d\n"
	"#define HAS_LIGHTING %d\n"
	"#define HAS_TWO_SIDED_LIGHTING %d\n"
	"#define HAS_FIXED_AMBIENT %d\n"
	"#define TEX_MODE %d\n"
	"#define HAS_ALPHA_MODE %d\n"
	"#define HAS_ALPHA_TEST %d\n"
	"#define HAS_LIGHT0 %d\n"
	"#define LIGHT0_TYPE %d\n"
	"#define LIGHT0_ATT_TYPE %d\n"
	"#define HAS_LIGHT0_SPECULAR %d\n"
	"#define HAS_LIGHT1 %d\n"
	"#define LIGHT1_TYPE %d\n"
	"#define LIGHT1_ATT_TYPE %d\n"
	"#define HAS_LIGHT1_SPECULAR %d\n"
	"#define HAS_LIGHT2 %d\n"
	"#define LIGHT2_TYPE %d\n"
	"#define LIGHT2_ATT_TYPE %d\n"
	"#define HAS_LIGHT2_SPECULAR %d\n"
	"#define HAS_LIGHT3 %d\n"
	"#define LIGHT3_TYPE %d\n"
	"#define LIGHT3_ATT_TYPE %d\n"
	"#define HAS_LIGHT3_SPECULAR %d\n"
	"#define HAS_FOG %d\n"
	"#define HAS_PATTERN %d\n"
	"#define HAS_CUE %d\n"
	"#define COLLECT %d\n"
	"#define HAS_WEIGHT %d\n";

	char *definitions, *vs_source, *fs_source;
	GLuint program;
	int ret;

	HL_LOG ("compiling shader for variant %lX", variant.full);

	ret = asprintf (&definitions, definitions_template,
	                variant.has_texture,
	                variant.has_lighting,
	                variant.has_two_sided_lighting,
	                variant.has_fixed_ambient,
	                variant.tex_mode,
	                variant.has_alpha_mode,
	                variant.has_alpha_test,
	                variant.has_light0,
	                variant.light0_type,
	                variant.light0_att_type,
	                variant.has_light0_specular,
	                variant.has_light1,
	                variant.light1_type,
	                variant.light1_att_type,
	                variant.has_light1_specular,
	                variant.has_light2,
	                variant.light2_type,
	                variant.light2_att_type,
	                variant.has_light2_specular,
	                variant.has_light3,
	                variant.light3_type,
	                variant.light3_att_type,
	                variant.has_light3_specular,
	                variant.has_fog,
	                variant.has_pattern,
	                variant.has_cue,
	                variant.collect,
	                variant.has_weight);
	HL_ASSERT (ret >= 0);

	ret = asprintf (&vs_source, mesh_vs_source, definitions);
	HL_ASSERT (ret >= 0);

	ret = asprintf (&fs_source, mesh_fs_source, definitions);
	HL_ASSERT (ret >= 0);

	if (variant.collect) {
		/* Storage buffers, images and atomics: GLSL 4.30 */
		std::string vs (vs_source), fs (fs_source);

		vs.replace (vs.find ("#version 140"), 12, "#version 430");
		fs.replace (fs.find ("#version 140"), 12, "#version 430");
		program = hl::gl::compile_program (vs.c_str (), fs.c_str ());
	} else
		program = hl::gl::compile_program (vs_source, fs_source);

	free (definitions);
	free (vs_source);
	free (fs_source);
	return program;
}

/*
 * The programs' cache
 * ===================
 *
 * A variant met for the first time is compiled there and then, which stops
 * the game for a moment (a stutter the first time an effect shows). So each
 * game's variants are listed in cache/shaders/<game>.txt as they are met (hex,
 * one a line), and compiled before its first frame is drawn; and each
 * program's binary, as the driver gives it (program binaries, OpenGL 4.1),
 * kept in cache/shaders/<variant>.bin with the driver's name, so that the next
 * start reads it instead of compiling. A binary of another driver or version
 * is compiled again. The program ships a list for each game too, in
 * data/shaders/<game>.txt (a clone takes its parent's): its first run
 * compiles them as it starts, not as the effects show (SWRACER's first race
 * stopped the drawing thread 80 ms, the machine waiting). As Odin does (its
 * lists of a game's programs, the driver's cache).
 */

static std::string shader_game, shader_parent;
static bool programs_warmed;

void
Renderer::set_game (const char *name, const char *parent)
{
	shader_game = name ? name : "";
	shader_parent = parent ? parent : "";
}

static std::string
shader_cache_file (const std::string &name)
{
	std::string path = hl::Paths::config_write ("cache/shaders/" + name);
	std::error_code ec;

	std::filesystem::create_directories (std::filesystem::path (path).parent_path (), ec);
	return path;
}

/* The driver the binaries were made by */
static std::string
driver_name ()
{
	const char *v = (const char *) glGetString (GL_VENDOR);
	const char *r = (const char *) glGetString (GL_RENDERER);
	const char *g = (const char *) glGetString (GL_VERSION);

	return std::string (v ? v : "") + "|" + (r ? r : "") + "|" + (g ? g : "");
}

static bool
binaries_supported ()
{
	GLint formats = 0;

	if (!GLEW_VERSION_4_1 && !GLEW_ARB_get_program_binary)
		return false;
	glGetIntegerv (GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
	return formats > 0;
}

static std::string
binary_name (uint64_t variant)
{
	char name[32];

	snprintf (name, sizeof (name), "%016llX.bin", (unsigned long long) variant);
	return name;
}

/* The program of the variant from its binary: 0 when there is none, or of
 * another driver, or the driver refuses it */
static GLuint
load_program_binary (uint64_t variant)
{
	FILE *fp = fopen (shader_cache_file (binary_name (variant)).c_str (), "rb");
	std::string want = driver_name (), have;
	std::vector<uint8_t> data;
	uint32_t len = 0, format = 0, size = 0;
	GLint status = GL_FALSE;
	GLuint program;
	bool ok;

	if (!fp)
		return 0;
	ok = fread (&len, 4, 1, fp) == 1 && len < 4096;
	if (ok) {
		have.resize (len);
		ok = fread (&have[0], 1, len, fp) == len && have == want;
	}
	ok = ok && fread (&format, 4, 1, fp) == 1 && fread (&size, 4, 1, fp) == 1 &&
	     size > 0 && size < (64u << 20);
	if (ok) {
		data.resize (size);
		ok = fread (data.data (), 1, size, fp) == size;
	}
	fclose (fp);
	if (!ok)
		return 0;
	program = glCreateProgram ();
	glProgramBinary (program, (GLenum) format, data.data (), (GLsizei) size);
	glGetProgramiv (program, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		glDeleteProgram (program);
		hl::gl::clear_errors ();
		return 0;
	}
	return program;
}

static void
save_program_binary (GLuint program, uint64_t variant)
{
	std::string name = driver_name ();
	std::vector<uint8_t> data;
	GLint length = 0;
	GLsizei size = 0;
	GLenum format = 0;
	uint32_t len = (uint32_t) name.size ();
	FILE *fp;

	glGetProgramiv (program, GL_PROGRAM_BINARY_LENGTH, &length);
	if (length <= 0)
		return;
	data.resize ((size_t) length);
	glGetProgramBinary (program, length, &size, &format, data.data ());
	if (size <= 0) {
		hl::gl::clear_errors ();
		return;
	}
	fp = fopen (shader_cache_file (binary_name (variant)).c_str (), "wb");
	if (!fp)
		return;
	uint32_t f = format, n = (uint32_t) size;
	fwrite (&len, 4, 1, fp);
	fwrite (name.data (), 1, len, fp);
	fwrite (&f, 4, 1, fp);
	fwrite (&n, 4, 1, fp);
	fwrite (data.data (), 1, (size_t) size, fp);
	fclose (fp);
}

/* The variant's program, from its binary or compiled (its binary then
 * kept); met in a game for the first time, added to the game's list */
static GLuint
mesh_program (hikaru_glsl_variant_t variant, bool met)
{
	bool binaries = binaries_supported ();
	GLuint program = binaries ? load_program_binary (variant.full) : 0;
	FILE *fp;

	if (!program) {
		program = compile_mesh_program (variant);
		if (binaries)
			save_program_binary (program, variant.full);
	}
	if (met && !shader_game.empty ()) {
		fp = fopen (shader_cache_file (shader_game + ".txt").c_str (), "a");
		if (fp) {
			fprintf (fp, "%016llX\n", (unsigned long long) variant.full);
			fclose (fp);
		}
	}
	return program;
}

/* Before the game's first frame: the programs of its lists, its runs' and
 * the program's (above). Those that need OpenGL 4.3 (translucency's) only
 * when the card has it */
static void
warm_up_programs ()
{
	auto start = std::chrono::steady_clock::now ();
	const std::filesystem::path shipped = std::filesystem::path (hl::Paths::base ()) / "data" / "shaders";
	std::vector<std::string> lists;
	unsigned count = 0;
	char line[64];
	FILE *fp;

	if (programs_warmed || shader_game.empty ())
		return;
	programs_warmed = true;
	lists.push_back (shader_cache_file (shader_game + ".txt"));
	lists.push_back ((shipped / (shader_game + ".txt")).string ());
	if (!shader_parent.empty ())
		lists.push_back ((shipped / (shader_parent + ".txt")).string ());
	for (const std::string &list : lists) {
		fp = fopen (list.c_str (), "r");
		if (!fp)
			continue;
		while (fgets (line, sizeof (line), fp) && num_programs < MAX_PROGRAMS - 1) {
			hikaru_glsl_variant_t variant;
			unsigned i;

			variant.full = strtoull (line, NULL, 16);
			if (!variant.full || (variant.collect && !GLEW_VERSION_4_3))
				continue;
			for (i = 0; i < num_programs; i++)
				if (program_cache[i].variant.full == variant.full)
					break;
			if (i < num_programs)
				continue;
			program_cache[num_programs].variant.full = variant.full;
			program_cache[num_programs].program = mesh_program (variant, false);
			num_programs++;
			count++;
		}
		fclose (fp);
	}
	if (count)
		HL_PRINT ("HR %u shader programs of %s ready in %.1f s", count, shader_game.c_str (),
		          std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ());
}

static void
upload_glsl_program (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_glsl_variant_t variant;
	int i, slot;

	variant = get_glsl_variant (hr, mesh);
	if (hr->meshes.variant.full == variant.full)
		return;

	hr->meshes.variant.full = variant.full;

	for (i = 0; i < num_programs; i++) {
		if (program_cache[i].variant.full == variant.full) {
			hr->meshes.program = program_cache[i].program;
			goto update_locations;
		}
	}

	hr->meshes.variant = variant;
	hr->meshes.program = mesh_program (variant, true);

	if (0) {
		hl::gl::print_uniforms (hr->meshes.program);
		HL_ASSERT_NO_GL_ERROR ();
	}

	program_cache[num_programs].variant.full = variant.full;
	program_cache[num_programs].program = hr->meshes.program;
	num_programs++;
	HL_ASSERT (num_programs < MAX_PROGRAMS);

update_locations:
	/* Its entry (i counts the lights below) */
	slot = i;
	hr->meshes.slot = slot;
	glUseProgram (hr->meshes.program);
	HL_ASSERT_NO_GL_ERROR ();

	if (program_cache[slot].has_locs) {
		hr->meshes.locs = program_cache[slot].locs;
		if (hr->meshes.variant.collect)
			glUniform1i (hr->meshes.locs.u_opaque_depth, OIT_DEPTH_UNIT);
		return;
	}

	hr->meshes.locs.u_projection =
		glGetUniformLocation (hr->meshes.program, "u_projection");
	hr->meshes.locs.u_modelview =
		glGetUniformLocation (hr->meshes.program, "u_modelview");
	hr->meshes.locs.u_normal =
		glGetUniformLocation (hr->meshes.program, "u_normal");
	hr->meshes.locs.u_link_modelview =
		glGetUniformLocation (hr->meshes.program, "u_link_modelview");
	hr->meshes.locs.u_link_normal =
		glGetUniformLocation (hr->meshes.program, "u_link_normal");
	for (i = 0; i < 4; i++) {
		char temp[64];

		sprintf (temp, "u_lights[%d].position", i);
		hr->meshes.locs.u_lights[i].position =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].direction", i);
		hr->meshes.locs.u_lights[i].direction =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].diffuse", i);
		hr->meshes.locs.u_lights[i].diffuse =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].cone", i);
		hr->meshes.locs.u_lights[i].cone =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].extents", i);
		hr->meshes.locs.u_lights[i].extents =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].ambient", i);
		hr->meshes.locs.u_lights[i].ambient =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].table", i);
		hr->meshes.locs.u_lights[i].table =
			glGetUniformLocation (hr->meshes.program, temp);
	}
	hr->meshes.locs.u_ambient =
		glGetUniformLocation (hr->meshes.program, "u_ambient");
	hr->meshes.locs.u_texture =
		glGetUniformLocation (hr->meshes.program, "u_texture");
	hr->meshes.locs.u_fog =
		glGetUniformLocation (hr->meshes.program, "u_fog");
	hr->meshes.locs.u_fog_color =
		glGetUniformLocation (hr->meshes.program, "u_fog_color");
	hr->meshes.locs.u_alpha_range =
		glGetUniformLocation (hr->meshes.program, "u_alpha_range");
	hr->meshes.locs.u_depth =
		glGetUniformLocation (hr->meshes.program, "u_depth");
	hr->meshes.locs.u_pattern =
		glGetUniformLocation (hr->meshes.program, "u_pattern");
	hr->meshes.locs.u_max_nodes =
		glGetUniformLocation (hr->meshes.program, "u_max_nodes");
	hr->meshes.locs.u_tag =
		glGetUniformLocation (hr->meshes.program, "u_tag");
	hr->meshes.locs.u_opaque_depth =
		glGetUniformLocation (hr->meshes.program, "u_opaque_depth");
	program_cache[slot].locs = hr->meshes.locs;
	program_cache[slot].has_locs = true;
	if (hr->meshes.variant.collect)
		glUniform1i (hr->meshes.locs.u_opaque_depth, OIT_DEPTH_UNIT);
	HL_ASSERT_NO_GL_ERROR ();
}

/* Other drawing comes (or came): the next mesh sets its program, viewport
 * and texture again */
static void
forget_mesh_state (Renderer *hr)
{
	hr->meshes.variant.full = ~0ull;
	hr->meshes.viewport_known = false;
	hr->meshes.texture_known = false;
}

static void
upload_viewport (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_viewport_t *vp = &hr->vp_list[mesh->vp_index];
	const float h = vp->clip.t - vp->clip.b;
	const float w = vp->clip.r - vp->clip.l;
	const float n_over_f = vp->clip.n / vp->clip.f;
	const float dcx = (vp->offset.x - (w / 2.0f));
	const float dcy = (vp->offset.y - (h / 2.0f));
	/* The projection's centre is the offset, in pixels from the screen's
	 * top left; f the focal length in pixels. Most viewports have it at
	 * their middle; SGNASCAR's test menu at 0,0, its text in the bottom
	 * right corner when Valkyrie put the centre at the middle */
	const float centre_y = 480.0f - vp->offset.y;
	/* The clip rectangle counts its rows from the top as well: b and t
	 * are its top and bottom rows. SGNASCAR's driver cards (90 to 480)
	 * reach row 400; read from the bottom, they stopped at 390 */
	const float clip_b = 480.0f - vp->clip.t;
	const float clip_t = 480.0f - vp->clip.b;
	/* Widescreen: a viewport as wide as the screen sees wider, its middle
	 * 640 pixels as before (the HUDs drawn in 3D stay put); the others
	 * keep their projection, moved to the middle */
	const float margin = (hr->scene_w - 640) * 0.5f;
	const float extra = (margin > 0.0f && vp->clip.l <= 1.0f && vp->clip.r >= 639.0f) ?
	                    margin : 0.0f;
	const float left = (vp->clip.l - extra - vp->offset.x) * n_over_f;
	const float right = (vp->clip.r + extra - vp->offset.x) * n_over_f;
	const float bottom = (clip_b - centre_y) * n_over_f;
	const float top = (clip_t - centre_y) * n_over_f;
	mtx4x4f_t projection;

	HL_ASSERT (mesh->vp_index != ~0);

	/* In the scene's buffers, RENDER_SCALE times its pixels */
	const float s = (float) hr->framebuffer.scale;
	const GLint rect[4] = {
		(GLint) ((vp->clip.l + margin - extra) * s), (GLint) (clip_b * s),
		(GLint) ((vp->clip.r - vp->clip.l + 2.0f * extra) * s), (GLint) ((clip_t - clip_b) * s)
	};
	if (!hr->meshes.viewport_known || memcmp (rect, hr->meshes.viewport, sizeof (rect))) {
		glViewport (rect[0], rect[1], rect[2], rect[3]);
		memcpy (hr->meshes.viewport, rect, sizeof (rect));
		hr->meshes.viewport_known = true;
	}

	/* The program's projection and fog as they were: nothing sent */
	MeshSent &sent = program_cache[hr->meshes.slot].sent;
	decltype (sent.viewport_was) was;

	memset ((void *) &was, 0, sizeof (was));
	was.vp = *vp;
	was.scene_w = hr->scene_w;
	was.distance = hr->view.distance;
	was.scale = hr->framebuffer.scale;
	if (!differs (sent.viewport, sent.viewport_was, was))
		return;

	/* Negative planes and offsets outside the screen are the games' own
	 * (VON4's, BRAVEFF's, at each frame), drawn as they say: logged with
	 * HR_LOG only */
	if (!isnonnegative (vp->clip.l) || !isnonnegative (vp->clip.r) ||
	    !isnonnegative (vp->clip.b) || !isnonnegative (vp->clip.t) ||
	    !isnonnegative (vp->clip.f) || !isnonnegative (vp->clip.n))
		LOG ("non-positive viewport clipping planes: %s", get_viewport_str (vp));

	if ((vp->clip.l >= vp->clip.r) ||
	    (vp->clip.b >= vp->clip.t) ||
	    (vp->clip.n >= vp->clip.f)) {
		HL_ERROR_FEW ("inverted viewport clipping planes: %s",
		              get_viewport_str (vp));
		/* continue anyway */
	}

	if (!isnonnegative (vp->offset.x) || (vp->offset.x >= 640.0f) ||
	    !isnonnegative (vp->offset.y) || (vp->offset.y >= 480.0f))
		LOG ("viewport offset outside the screen: %s", get_viewport_str (vp));

	LOG ("vp  = %s : [w=%f h=%f dcx=%f dcy=%f]",
	     get_viewport_str (vp), w, h, dcx, dcy);

	hl::frustum (projection, left, right, bottom, top, vp->clip.n, SCENE_FAR);

	glUniformMatrix4fv (hr->meshes.locs.u_projection, 1, GL_FALSE,
	                    (const GLfloat *) projection);

	if (hr->meshes.variant.has_fog) {
		vec2f_t fog;
		vec3f_t fog_color;

		/* P is 1 / (end - start) and Q near / end, in the eye's
		 * depth (the VON4 library builds them so, 0C716320; the X360
		 * port fogs linearly from start to end), as Odin's author
		 * found them. Taken before as 1/z where the fog starts and
		 * where it is whole: SGNASCAR's (P 0.0002, Q 0.00001, near
		 * 0.1) then went from 5000 to 100000, now 5000 to 10000. The
		 * draw distance's factor takes it that much farther */
		if (vp->depth.density > 0.0f && vp->depth.bias > 0.0f) {
			fog[0] = (vp->clip.n / vp->depth.bias - 1.0f / vp->depth.density) *
			         hr->view.distance;
			fog[1] = vp->depth.density / hr->view.distance;
		} else {
			fog[0] = 1e30f;
			fog[1] = 0.0f;
		}
		glUniform2fv (hr->meshes.locs.u_fog, 1, fog);

		fog_color[0] = vp->depth.mask[0] * INV255;
		fog_color[1] = vp->depth.mask[1] * INV255;
		fog_color[2] = vp->depth.mask[2] * INV255;
		glUniform3fv (hr->meshes.locs.u_fog_color, 1, fog_color);
	} else if (hr->meshes.variant.has_cue) {
		vec2f_t cue;

		/* Without a colour, P is 1 / (end - start) and Q near / start
		 * in the eye's depth (PHARRIER's library, 0C015A3E), as Odin's
		 * author found them; the draw distance's factor takes both
		 * farther */
		cue[0] = vp->clip.n / vp->depth.bias * hr->view.distance;
		cue[1] = vp->depth.density / hr->view.distance;
		glUniform2fv (hr->meshes.locs.u_fog, 1, cue);
	}
}

static void
upload_modelview (Renderer *hr, hikaru_mesh_t *mesh, unsigned i)
{
	hikaru_modelview_t *mv = &hr->mv_list[mesh->mv_index + i];
	mtx3x3f_t normal;

	if (mesh->mv_index == ~0) {
		static const hikaru_modelview_t identity_mv = {
			.mtx = {
				{ 1.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f, 0.0f },
				{ 0.0f, 0.0f, 0.0f, 1.0f }
			}
		};

		HL_ERROR ("attempting to draw with no modelview!");

		/* Attempt to render something anyway. */
		mv = (hikaru_modelview_t *) &identity_mv;
	}

	/* The program's matrices as they were: nothing sent */
	{
		MeshSent &sent = program_cache[hr->meshes.slot].sent;
		decltype (sent.modelview_was) was;
		const hikaru_modelview_t *link = mesh->has_link ? &hr->mv_list[mesh->mv_index + 1] : mv;

		memcpy (was.mtx, mv->mtx, sizeof (was.mtx));
		memcpy (was.link, link->mtx, sizeof (was.link));
		if (!differs (sent.modelview, sent.modelview_was, was))
			return;
	}

	hl::compute_normal_matrix (normal, mv->mtx);

	LOG ("mv  = [%u+%u] %s", mesh->mv_index, i, get_modelview_str (mv));
	glUniformMatrix4fv (hr->meshes.locs.u_modelview, 1, GL_FALSE,
	                    (const GLfloat *) mv->mtx);

	glUniformMatrix3fv (hr->meshes.locs.u_normal, 1, GL_FALSE,
	                    (const GLfloat *) normal);

	/* The vertices with S: the matrix pushed before, if any */
	if (mesh->has_link) {
		mv = &hr->mv_list[mesh->mv_index + 1];
		hl::compute_normal_matrix (normal, mv->mtx);
	}
	glUniformMatrix4fv (hr->meshes.locs.u_link_modelview, 1, GL_FALSE,
	                    (const GLfloat *) mv->mtx);
	glUniformMatrix3fv (hr->meshes.locs.u_link_normal, 1, GL_FALSE,
	                    (const GLfloat *) normal);
}

static void
upload_material_texhead (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_texture_t *tex;

	if (!hr->meshes.variant.has_texture)
		return;

	tex = get_texture (hr, &hr->tex_list[mesh->tex_index]);

	/* Unit 0 bound to it, unless it was (the other drawing leaves unit 0
	 * active) */
	if (!hr->meshes.texture_known || hr->meshes.texture != (tex ? tex->id : 0)) {
		glActiveTexture (GL_TEXTURE0 + 0);
		HL_ASSERT_NO_GL_ERROR ();

		glBindTexture (GL_TEXTURE_2D, tex ? tex->id : 0);
		HL_ASSERT_NO_GL_ERROR ();
		hr->meshes.texture = tex ? tex->id : 0;
		hr->meshes.texture_known = true;
	}

	/* The texhead's wrap: textures of the same slot share a GL one */
	if (tex) {
		const hikaru_texhead_t *th = &hr->tex_list[mesh->tex_index];
		uint8_t wrap = (uint8_t) (th->wrapu | (th->repeatu << 1) | (th->wrapv << 2) | (th->repeatv << 3));

		if (tex->wrap != wrap) {
			set_texture_wrap (&hr->tex_list[mesh->tex_index]);
			tex->wrap = wrap;
		}
	}

	/* ANISOTROPY, when the card has it: set again when it changes */
	if (tex && tex->aniso != hr->view.anisotropy && hr->framebuffer.max_aniso > 1.0f) {
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
		                 MIN2 ((float) hr->view.anisotropy, hr->framebuffer.max_aniso));
		HL_ASSERT_NO_GL_ERROR ();
		tex->aniso = hr->view.anisotropy;
	}

	if (!program_cache[hr->meshes.slot].sent.texture) {
		glUniform1i (hr->meshes.locs.u_texture, 0);
		HL_ASSERT_NO_GL_ERROR ();
		program_cache[hr->meshes.slot].sent.texture = true;
	}
}

static void
upload_alpha_range (Renderer *hr, hikaru_mesh_t *mesh)
{
	MeshSent &sent = program_cache[hr->meshes.slot].sent;
	float now[2] = { mesh->alpha_thresh[0], mesh->alpha_thresh[1] };

	if (!hr->meshes.variant.has_alpha_test || !differs (sent.alpha, sent.alpha_was, now))
		return;

	glUniform2fv (hr->meshes.locs.u_alpha_range, 1, mesh->alpha_thresh);
	HL_ASSERT_NO_GL_ERROR ();
}

/* The fragments' depth (the shader's u_depth): its scale, and the mesh's
 * depth bias (101), which moves it nearer. Its low 10 bits are the amount;
 * bit 10 picks the mode, as a 32-bit float depth buffer's bias:
 *  - clear, a constant: amount * 2000 units of the depth's last bit, 2^-23
 *    of its power of 2. 1/z goes up by that part of itself, between
 *    amount * 2000 / 2^24 and twice that: here the middle, / ln 2;
 *  - set, a slope: 1/z up by its slope across a pixel times amount * 6 /
 *    256, at the native resolution.
 * PHARRIER's translucent meshes take 0x400, nothing, and 0x500; AIRTRIX's,
 * constants up to 982. All the polytypes take it */
static void
upload_depth (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned amount = mesh->depth_bias & 0x3FF;
	float depth[3];

	depth[0] = 1.0f / log2f (1.0f + SCENE_FAR);
	depth[1] = 0.0f;
	depth[2] = 0.0f;
	if (mesh->depth_bias & 0x400)
		depth[2] = amount * 6.0f / 256.0f * hr->framebuffer.scale;
	else
		depth[1] = amount * 2000.0f / 16777216.0f / logf (2.0f);
	if (differs (program_cache[hr->meshes.slot].sent.depth, program_cache[hr->meshes.slot].sent.depth_was, depth))
		glUniform3fv (hr->meshes.locs.u_depth, 1, depth);
}

/* The 32 dot patterns of 501 (see hikaru-gpu-cp.cpp), a bit a pixel of a
 * 4 x 4 square, bit 0 at the top left: those of the Xbox 360 port, as Odin
 * (Réda Chérif-Touil, GPL v3+) found them */
static const uint16_t PATTERNS[32] = {
	0xFFFF, 0xFBFE, 0xFAFA, 0x7ADA, 0x5A5A, 0x5258, 0x5050, 0x1040,
	0x0000, 0x0401, 0x0505, 0x8525, 0xA5A5, 0xADA7, 0xAFAF, 0xEFBF,
	0xFFFF, 0xFDF7, 0xF5F5, 0xE5B5, 0xA5A5, 0xA4A1, 0xA0A0, 0x8020,
	0x0000, 0x0208, 0x0A0A, 0x1A4A, 0x5A5A, 0x5B5E, 0x5F5F, 0x7FDF,
};

/* The mesh's dot pattern (the shader's u_pattern), and the render scale,
 * so that its dots keep the board's pixels */
static void
upload_pattern (Renderer *hr, hikaru_mesh_t *mesh)
{
	MeshSent &sent = program_cache[hr->meshes.slot].sent;
	GLint now[2] = { PATTERNS[mesh->pattern & 31], (GLint) MAX2 (1u, hr->framebuffer.scale) };

	if (!hr->meshes.variant.has_pattern || !differs (sent.pattern, sent.pattern_was, now))
		return;

	glUniform2i (hr->meshes.locs.u_pattern, now[0], now[1]);
	HL_ASSERT_NO_GL_ERROR ();
}

static void
get_light_ambient (Renderer *hr, hikaru_mesh_t *mesh, float *out)
{
	hikaru_viewport_t *vp =
		(mesh->vp_index == ~0) ? NULL : &hr->vp_list[mesh->vp_index];

	if (hr->debug.flags[HR_DEBUG_NO_AMBIENT] || !vp)
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = vp->color.ambient[0] * INV255;
		out[1] = vp->color.ambient[1] * INV255;
		out[2] = vp->color.ambient[2] * INV255;
	}
}

static void
get_light_diffuse (Renderer *hr, hikaru_light_t *lit, float *out)
{
	if (hr->debug.flags[HR_DEBUG_NO_DIFFUSE])
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = lit->diffuse[0] * INV255;
		out[1] = lit->diffuse[1] * INV255;
		out[2] = lit->diffuse[2] * INV255;
	}
}

/* A spot's cone, from its 451 (kept in the specular bytes): the cosine of its
 * edge (1.15), and over what the light rises from it, eeeemmmm: (32 - m) / 32
 * x 2^-e (AIRTRIX's skater's spots: 22.5 degrees, rising over 0.078) */
static void
get_light_cone (hikaru_light_t *lit, float *out)
{
	unsigned cosine = lit->specular[0] | (lit->specular[1] << 8);
	unsigned e = lit->specular[2] >> 4, m = lit->specular[2] & 15;

	out[0] = cosine * (1.0f / 32768.0f);
	out[1] = ldexpf (32.0f / (32 - m), (int) e);
}

static void
upload_lightset (Renderer *hr, hikaru_mesh_t *mesh)
{
	hikaru_lightset_t *ls = &hr->ls_list[mesh->ls_index];
	float tmp[4];
	unsigned i;

	if (!hr->meshes.variant.has_lighting)
		return;

	/* The program's lights as they were: nothing sent */
	{
		MeshSent &sent = program_cache[hr->meshes.slot].sent;
		decltype (sent.lightset_was) was;

		memset ((void *) &was, 0, sizeof (was));
		was.ls = *ls;
		memcpy (was.tables, hr->ls_tables[mesh->ls_index].points, sizeof (was.tables));
		get_light_ambient (hr, mesh, was.ambient);
		was.no_ambient = hr->debug.flags[HR_DEBUG_NO_AMBIENT] != 0;
		was.no_diffuse = hr->debug.flags[HR_DEBUG_NO_DIFFUSE] != 0;
		if (!differs (sent.lightset, sent.lightset_was, was))
			return;
	}

	LOG ("lightset = %s", get_lightset_str (ls));

	get_light_ambient (hr, mesh, tmp);
	glUniform3fv (hr->meshes.locs.u_ambient, 1, (const GLfloat *) tmp);
	HL_ASSERT_NO_GL_ERROR ();

	for (i = 0; i < 4; i++) {
		hikaru_light_t *lt = &ls->lights[i];

		if (ls->mask & (1 << i))
			continue;

		glUniform3fv (hr->meshes.locs.u_lights[i].position, 1,
		              lt->position);
		HL_ASSERT_NO_GL_ERROR ();

		glUniform3fv (hr->meshes.locs.u_lights[i].direction, 1,
		              lt->direction);
		HL_ASSERT_NO_GL_ERROR ();

		get_light_diffuse (hr, lt, tmp);
		glUniform3fv (hr->meshes.locs.u_lights[i].diffuse, 1, tmp);
		HL_ASSERT_NO_GL_ERROR ();

		get_light_cone (lt, tmp);
		glUniform2fv (hr->meshes.locs.u_lights[i].cone, 1, tmp);
		HL_ASSERT_NO_GL_ERROR ();

		glUniform2fv (hr->meshes.locs.u_lights[i].extents, 1,
		              lt->attenuation);
		glUniform1f (hr->meshes.locs.u_lights[i].ambient,
		             lt->ambient_level * (1.0f / 16.0f));

		/* A spot's table, 0x400 for 1.0; none loaded (all 0), the
		 * straight rise it had before */
		if (hr->meshes.variant.light0_type == HIKARU_LIGHT_TYPE_SPOT ||
		    hr->meshes.variant.light1_type == HIKARU_LIGHT_TYPE_SPOT ||
		    hr->meshes.variant.light2_type == HIKARU_LIGHT_TYPE_SPOT ||
		    hr->meshes.variant.light3_type == HIKARU_LIGHT_TYPE_SPOT) {
			const uint16_t *points = hr->ls_tables[mesh->ls_index].points[i];
			float table[16];
			bool loaded = false;
			unsigned k;

			for (k = 0; k < 16; k++) {
				table[k] = points[k] * (1.0f / 1024.0f);
				loaded |= points[k] != 0;
			}
			if (!loaded)
				for (k = 0; k < 16; k++)
					table[k] = k / 15.0f;
			glUniform1fv (hr->meshes.locs.u_lights[i].table, 16, table);
		}
	}
}

/****************************************************************************
 Meshes
****************************************************************************/

#define HL_COPY_VEC2F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
	} while (0)

#define HL_COPY_VEC3F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
		dst_[2] = src_[2]; \
	} while (0)

static float
clampf (float x, float min_, float max_)
{
	return (min_ > x) ? min_ :
	       (max_ < x) ? max_ : x;
}

static void
copy_colors (Renderer *hr, hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	Gpu *gpu = hr->gpu;
	hikaru_material_t *mat = &MAT.scratch;
	float alpha;
	unsigned i;

	for (i = 0; i < 3; i++)
		dst->body.diffuse[i] = mat->diffuse[i];

	for (i = 0; i < 3; i++)
		dst->body.ambient[i] = mat->ambient[i];

	for (i = 0; i < 4; i++)
		dst->body.specular[i] = mat->specular[i];

	for (i = 0; i < 3; i++)
		dst->body.unknown[i] = mat->unknown[i];

	/* Patch diffuse alpha depending on poly type. NOTE: transparent
	 * polygons also have an alpha, with unknown meaning (it seems to have
	 * opposite sign w.r.t. translucent alpha though). */
	alpha = 1.0f;
	if (POLY.type == HIKARU_POLYTYPE_TRANSLUCENT) {
		float p_alpha = POLY.alpha;
		float v_alpha = src->info.alpha * INV255;
		alpha = clampf (p_alpha * v_alpha, 0.0f, 1.0f);
	}
	dst->body.alpha = (uint8_t) (alpha * 255.0f);
}

static void
copy_texcoords (Renderer *hr,
                hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	Gpu *gpu = hr->gpu;
	hikaru_texhead_t *th = &TEX.scratch;
	float w = 16 << th->logw;
	float h = 16 << th->logh;
	/* Pushed as 12.4; whole texels with this bit (2C1 bit 14) */
	float unit = th->texel_uv ? 16.0f : 1.0f;

	/* 2x2 texels a halfword */
	if (th->format == HIKARU_FORMAT_ABGR1111) {
		w *= 2;
		h *= 2;
	}

	/* 0D1's panning, for the texheads with 2C1 bit 15 only: PHARRIER's
	 * (its sky's clouds move); BRAVEFF's texts, without it, stay where
	 * they are (its panning, set once at the start of the list, sent them
	 * to the wrong texels) */
	float pan_x = th->panned ? 0.5f * gpu->texoffset_x : 0.0f;
	float pan_y = th->panned ? 0.5f * gpu->texoffset_y : 0.0f;

	dst->body.texcoords[0] = (src->body.texcoords[0] * unit + pan_x) / w;
	dst->body.texcoords[1] = (src->body.texcoords[1] * unit + pan_y) / h;
}

static void
add_triangle (Renderer *hr)
{
	if (hr->push.num_verts >= 3) {
		uint32_t index = hr->push.num_tris * 3;
		hikaru_vertex_body_t *dst = &hr->push.all[index];

		HL_ASSERT ((index + 2) < MAX_VERTICES_PER_MESH);

		/* Culled and lit on two sides: PHARRIER's asteroids, some 50 a
		 * frame; the shader lights the side seen (mesh_fs_source) */
		if (hr->push.tmp[2].info.twosided &&
		    !hr->push.tmp[2].info.nocull)
			LOG ("a vertex with culling and two-sided lighting");

		if (hr->push.tmp[2].info.twosided)
			hr->meshes.current->has_two_sided_lighting = 1;

		if (hr->push.tmp[2].info.nocull) {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[2].body;
			dst[2] = hr->push.tmp[1].body;
			dst[3] = hr->push.tmp[0].body;
			dst[4] = hr->push.tmp[1].body;
			dst[5] = hr->push.tmp[2].body;
			hr->push.num_tris += 1;
		} else if (hr->push.tmp[2].info.winding) {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[2].body;
			dst[2] = hr->push.tmp[1].body;
		} else {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[1].body;
			dst[2] = hr->push.tmp[2].body;
		}
		hr->push.num_tris += 1;
	}
}

void
Renderer::push_vertices (hikaru_vertex_t *v, uint32_t flags, unsigned num)
{
	hl::Renderer *rend = this;
	Renderer *hr = (Renderer *) rend;
	Gpu *gpu = hr->gpu;
	unsigned i, vp_index = VP0.depth.func;

	HL_ASSERT (hr);
	HL_ASSERT (v);
	HL_ASSERT (num == 1 || num == 3);
	HL_ASSERT (v->info.tricap == 0 || v->info.tricap == 7);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] != vp_index)
		return;

	switch (num) {

	case 1:
		/* Note that VTX(2) always points to the last pushed vertex,
		 * which for instructions 12x, 1Ax and 1Bx means the vertex
		 * pushed by the instruction itself, and for instructions 1Ex
		 * and 15x the vertex pushed by the previous "push"
		 * instruction.
		 */

		/* If the incoming vertex includes the position, push it
		 * in the temporary buffer, updating it according to the
		 * p(osition)pivot bit. */
		if (flags & HR_PUSH_POS) {

			/* Do not change the pivot if it is not required */
			if (!v->info.ppivot)
				hr->push.tmp[0] = hr->push.tmp[1];
			hr->push.tmp[1] = hr->push.tmp[2];
			memset ((void *) &hr->push.tmp[2], 0, sizeof (hikaru_vertex_t));

			/* Set the position, colors and alpha. */
			HL_COPY_VEC3F (hr->push.tmp[2].body.position, v->body.position);
			hr->push.tmp[2].body.link = v->info.link;
			copy_colors (hr, &hr->push.tmp[2], v);

			/* Account for the added vertex. */
			hr->push.num_verts += 1;
			HL_ASSERT (hr->push.num_verts < MAX_VERTICES_PER_MESH);
		}

		/* Set the normal. */
		if (flags & HR_PUSH_NRM)
			HL_COPY_VEC3F (hr->push.tmp[2].body.normal, v->body.normal);

		/* Set the texcoords. */
		if (flags & HR_PUSH_TXC)
			copy_texcoords (hr, &hr->push.tmp[2], v);
		break;

	case 3:
		HL_ASSERT (flags == HR_PUSH_TXC);

		if (hr->push.num_verts < 3)
			return;

		for (i = 0; i < 3; i++)
			copy_texcoords (hr, &hr->push.tmp[2 - i], &v[i]);
		break;

	default:
		HL_ASSERT (!"num is neither 1 nor 3");
		break;
	}

	/* Finish the previous triangle. */
	if (v[0].info.tricap == 7) {
		hr->push.tmp[2].info.full = v[0].info.full;
		add_triangle (hr);
	}
}

static void
print_rendstate (Renderer *hr, hikaru_mesh_t *mesh, const char *prefix)
{
	LOG ("RENDSTATE %s @%p #instances = %u", prefix, mesh, mesh->num_instances);
	if (mesh->vp_index < MAX_VIEWPORTS)
		LOG ("RENDSTATE %s %u vp:  %s", prefix, mesh->num,
		     get_viewport_str (&hr->vp_list[mesh->vp_index]));
	if (mesh->mv_index < MAX_MODELVIEWS)
		LOG ("RENDSTATE %s %u mv:  %s", prefix, mesh->num,
		     get_modelview_str (&hr->mv_list[mesh->mv_index]));
	if (mesh->mat_index < MAX_MATERIALS)
		LOG ("RENDSTATE %s %u mat: %s", prefix, mesh->num,
		     get_material_str (&hr->mat_list[mesh->mat_index]));
	if (mesh->tex_index < MAX_TEXHEADS)
		LOG ("RENDSTATE %s %u tex: %s", prefix, mesh->num,
		     get_texhead_str (&hr->tex_list[mesh->tex_index]));
	if (mesh->ls_index < MAX_LIGHTSETS)
		LOG ("RENDSTATE %s %u ls:  %s", prefix, mesh->num,
		     get_lightset_str (&hr->ls_list[mesh->ls_index]));
}

/* TODO check boundary conditions when nothing is uploaded in a frame. */
static void
update_and_set_rendstate (Renderer *hr, hikaru_mesh_t *mesh)
{
	Gpu *gpu = hr->gpu;
	unsigned i;

	memset ((void *) mesh, 0, sizeof (hikaru_mesh_t));

	LOG ("RENDSTATE updating vp %u/%u", hr->num_vps, MAX_VIEWPORTS);
	hr->vp_list[hr->num_vps++] = VP0;
	HL_ASSERT (hr->num_vps < MAX_VIEWPORTS);

	LOG ("RENDSTATE updating mat %u/%u", hr->num_mats, MAX_MATERIALS);
	hr->mat_list[hr->num_mats++] = MAT0;
	HL_ASSERT (hr->num_mats < MAX_MATERIALS);

	LOG ("RENDSTATE updating tex %u/%u", hr->num_texs, MAX_TEXHEADS);
	hr->tex_list[hr->num_texs++] = TEX0;
	HL_ASSERT (hr->num_texs < MAX_TEXHEADS);

	LOG ("RENDSTATE updating ls %u/%u", hr->num_lss, MAX_LIGHTSETS);
	/* With its lights' spot tables: light i takes the first of its four
	 * (LTABLE[i], words 0 to 7, the even point in a word's upper half),
	 * as Odin does; the games load the same ramp in all four */
	for (unsigned i = 0; i < 4; i++)
		for (unsigned j = 0; j < 8; j++) {
			uint32_t word = LTABLE[i][j].full;

			hr->ls_tables[hr->num_lss].points[i][j * 2] = (uint16_t) (word >> 16);
			hr->ls_tables[hr->num_lss].points[i][j * 2 + 1] = (uint16_t) word;
		}
	hr->ls_list[hr->num_lss++] = LS0;
	HL_ASSERT (hr->num_lss < MAX_LIGHTSETS);

	/* Copy the per-instance modelviews from last to first. */
	/* TODO optimize by setting MV.total to 0 (and fix the fallback). */
	if (MV.total == ~0) {
		LOG ("RENDSTATE adding no mvs %u/%u [#instances=%u]",
		     hr->num_mvs, MAX_MODELVIEWS, hr->num_instances);

		mesh->mv_index = hr->num_mvs - 1;
		mesh->num_instances = hr->num_instances;
		mesh->has_link = false;
	} else {
		MV.total = 1;

		mesh->mv_index = hr->num_mvs;
		mesh->num_instances = MV.total;
		hr->num_instances = MV.total;

		for (i = 0; i < MV.total; i++, hr->num_mvs++) {
			LOG ("RENDSTATE adding mv %u/%u [#instances=%u]",
			     hr->num_mvs, MAX_MODELVIEWS, MV.total);
	
			HL_ASSERT (hr->num_mvs < MAX_MODELVIEWS);
			hr->mv_list[hr->num_mvs] = MV.table[i];
		}

		/* The matrix pushed before, which the vertices with S take */
		mesh->has_link = gpu->link.valid;
		if (mesh->has_link) {
			HL_ASSERT (hr->num_mvs < MAX_MODELVIEWS);
			hr->mv_list[hr->num_mvs++] = gpu->link.pushed;
		}

		MV.total = 0;
		MV.depth = 0;
	}

	mesh->depth_bias = gpu->depth_bias;
	mesh->pattern = gpu->pattern & 0x1F;

	mesh->vp_index = hr->num_vps - 1;
	mesh->mat_index = hr->num_mats - 1;
	mesh->tex_index = hr->num_texs - 1;
	mesh->ls_index = hr->num_lss - 1;

	/* The alpha range of the material's entry (C81) in the table (154):
	 * 7F..FF in AIRTRIX; the high byte only (the others are FFFF or 0) */
	mesh->polytype = POLY.type;
	mesh->has_alpha_test = POLY.type == HIKARU_POLYTYPE_TRANSPARENT;
	mesh->alpha_thresh[0] = ATABLE[MAT0.alpha_index].part.lo * INV255;
	mesh->alpha_thresh[1] = (ATABLE[MAT0.alpha_index].part.hi & 0xFF) * INV255;

	mesh->num = hr->total_meshes++;
	print_rendstate (hr, mesh, "U");
	LOG ("RENDSTATE U %u alpha: type=%u poly_alpha=%.3f index=%u lo=%X hi=%X", mesh->num,
	     POLY.type, POLY.alpha, MAT0.alpha_index,
	     ATABLE[MAT0.alpha_index].part.lo, ATABLE[MAT0.alpha_index].part.hi);
}

#define OFFSET(member_) \
	((const GLvoid *) offsetof (hikaru_vertex_body_t, member_))

#define VAP(loc_, num_, type_, member_, normalize_) \
	if (loc_ != (GLuint) -1) { \
		glVertexAttribPointer (loc_, num_, type_, normalize_, \
		                       sizeof (hikaru_vertex_body_t), \
		                       OFFSET (member_)); \
		HL_ASSERT_NO_GL_ERROR (); \
		\
		glEnableVertexAttribArray (loc_); \
		HL_ASSERT_NO_GL_ERROR (); \
	}

static void
draw_mesh (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned i;

	HL_ASSERT (mesh);

	LOG ("==== DRAWING MESH @%p (#vertices=%u #instances=%u) ====",
	     mesh, mesh->num_tris * 3, mesh->num_instances);

	print_rendstate (hr, mesh, "D");

	upload_glsl_program (hr, mesh);
	HL_ASSERT_NO_GL_ERROR ();

	upload_viewport (hr, mesh);
	upload_material_texhead (hr, mesh);
	upload_alpha_range (hr, mesh);
	upload_lightset (hr, mesh);
	HL_ASSERT_NO_GL_ERROR ();

	upload_depth (hr, mesh);
	upload_pattern (hr, mesh);
	if (hr->meshes.variant.collect) {
		glUniform1ui (hr->meshes.locs.u_tag, hr->oit.tag);
		glUniform1ui (hr->meshes.locs.u_max_nodes, hr->oit.max_nodes);
	}

	hr->debug.draws += mesh->num_instances;
	hr->debug.tris += (uint64_t) mesh->num_tris * mesh->num_instances;
	if (mesh->num_tris > hr->debug.max_tris) {
		hr->debug.max_tris = mesh->num_tris;
		hr->debug.max_tris_mesh = mesh->num;
	}
	if (mesh->num_instances > hr->debug.max_instances)
		hr->debug.max_instances = mesh->num_instances;
	if (hr->debug.no_draw)
		return;

	/* The work to the GPU in batches: Windows resets the driver when one
	 * submission runs over 2 s, and BRAVEFF's demo draws up to 3400
	 * meshes a frame (its second loop reset it on an Intel UHD) */
	if ((mesh->num & 255) == 255)
		glFlush ();

	/* HR_SYNC: the GPU done with each mesh before the next, timed */
	std::chrono::steady_clock::time_point t0;
	if (hr->debug.sync) {
		glFinish ();
		t0 = std::chrono::steady_clock::now ();
	}

	if (hr->debug.flags[HR_DEBUG_NO_INSTANCING]) {
		unsigned i = MIN2 (hr->debug.flags[HR_DEBUG_SELECT_INSTANCE],
		                   mesh->num_instances - 1);
		upload_modelview (hr, mesh, i);
		glDrawArrays (GL_TRIANGLES, mesh->first, mesh->num_tris * 3);
	} else {
		for (i = 0; i < mesh->num_instances; i++) {
			upload_modelview (hr, mesh, i);
			glDrawArrays (GL_TRIANGLES, mesh->first, mesh->num_tris * 3);
		}
	}

	if (hr->debug.sync) {
		glFinish ();
		double ms = std::chrono::duration<double, std::milli> (
			std::chrono::steady_clock::now () - t0).count ();
		hr->debug.gpu_ms += ms;
		if (ms > hr->debug.slow_ms) {
			hr->debug.slow_ms = ms;
			hr->debug.slow_mesh = mesh->num;
		}
		if (ms > 20.0)
			HL_PRINT ("HR SLOW mesh %u: %.1f ms, %u tris x %u, vp %u, mat %s, tex %s, mv %s",
			          mesh->num, ms, mesh->num_tris, mesh->num_instances,
			          mesh->vp_index,
			          get_material_str (&hr->mat_list[mesh->mat_index]),
			          get_texhead_str (&hr->tex_list[mesh->tex_index]),
			          get_modelview_str (&hr->mv_list[mesh->mv_index]));
	}
}

/* The frame's vertices to the buffer, once, and where each attribute is:
 * the meshes draw from it. The vertex array keeps it bound. The buffer's
 * storage has the largest frame's size, given back to the driver at each
 * frame (the same size, which it takes back once the GPU is done with it)
 * and written into: a storage of each frame's size, a new one a frame, ran
 * an Intel UHD out of memory now and then (AIRTRIX, BRAVEFF, after
 * thousands of frames). False when the GPU has no memory for them: the
 * frame's 3D is not drawn */
static bool
upload_frame_vertices (Renderer *hr)
{
	std::vector<hikaru_vertex_body_t> &verts = hr->meshes.verts;
	size_t bytes = sizeof (verts[0]) * verts.size ();
	GLenum error;

	if (!hr->meshes.vao)
		glGenVertexArrays (1, &hr->meshes.vao);
	if (!hr->meshes.vbo)
		glGenBuffers (1, &hr->meshes.vbo);
	glBindVertexArray (hr->meshes.vao);
	glBindBuffer (GL_ARRAY_BUFFER, hr->meshes.vbo);
	if (bytes > hr->meshes.vbo_size)
		hr->meshes.vbo_size = bytes + bytes / 4;
	glBufferData (GL_ARRAY_BUFFER, hr->meshes.vbo_size, NULL, GL_STREAM_DRAW);
	if (bytes)
		glBufferSubData (GL_ARRAY_BUFFER, 0, bytes, verts.data ());
	/* Checked in every build (the release's has no HL_ASSERT_NO_GL_ERROR):
	 * a failed storage is not drawn from */
	error = hl::gl::check_error (__FILE__, __LINE__);
	if (error == GL_OUT_OF_MEMORY) {
		/* Its size again at the next frame */
		hr->meshes.vbo_size = 0;
		return false;
	}

	VAP (0, 3, GL_FLOAT,          position,  GL_FALSE);
	VAP (1, 3, GL_FLOAT,          normal,    GL_FALSE);
	VAP (2, 3, GL_UNSIGNED_BYTE,  diffuse,   GL_TRUE);
	VAP (3, 3, GL_UNSIGNED_BYTE,  ambient,   GL_TRUE);
	VAP (4, 4, GL_UNSIGNED_BYTE,  specular,  GL_TRUE);
	VAP (5, 3, GL_UNSIGNED_SHORT, unknown,   GL_FALSE);
	VAP (6, 2, GL_FLOAT,          texcoords, GL_FALSE);
	VAP (7, 1, GL_UNSIGNED_BYTE,  alpha,     GL_TRUE);
	VAP (8, 1, GL_UNSIGNED_BYTE,  link,      GL_FALSE);
	return true;
}

#undef OFFSET

static void
upload_vertex_data (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned i;

	HL_ASSERT (mesh);

	mesh->num_tris = hr->push.num_tris;

	if (hr->debug.flags[HR_DEBUG_LOG])
		for (i = 0; i < mesh->num_tris * 3; i++) {
			hikaru_vertex_body_t *v = &hr->push.all[i];
			LOG ("vertex %u: pos=(%.1f %.1f %.1f) uv=(%.3f %.3f) diffuse=%02X%02X%02X alpha=%02X",
			     i, v->position[0], v->position[1], v->position[2],
			     v->texcoords[0], v->texcoords[1],
			     v->diffuse[0], v->diffuse[1], v->diffuse[2], v->alpha);
		}

	/* The middle of the vertices' box */
	if (mesh->num_tris) {
		vec3f_t lo, hi;
		unsigned j;

		for (j = 0; j < 3; j++)
			lo[j] = hi[j] = hr->push.all[0].position[j];
		for (i = 1; i < mesh->num_tris * 3; i++)
			for (j = 0; j < 3; j++) {
				float x = hr->push.all[i].position[j];
				lo[j] = (x < lo[j]) ? x : lo[j];
				hi[j] = (x > hi[j]) ? x : hi[j];
			}
		for (j = 0; j < 3; j++)
			mesh->center[j] = (lo[j] + hi[j]) * 0.5f;
	}

	/* To the frame's vertices, uploaded before drawing */
	mesh->first = (uint32_t) hr->meshes.verts.size ();
	hr->meshes.verts.insert (hr->meshes.verts.end (),
	                         hr->push.all, hr->push.all + mesh->num_tris * 3);
}

void
Renderer::begin_mesh (uint32_t addr, bool is_static)
{
	hl::Renderer *rend = this;
	Renderer *hr = (Renderer *) rend;
	Gpu *gpu = hr->gpu;
	unsigned vp_index = VP0.depth.func;
	unsigned polytype = POLY.type;
	unsigned mesh_index = hr->num_meshes[vp_index][polytype];
	hikaru_mesh_t *mesh;

	HL_ASSERT (hr);
	HL_ASSERT (!hr->meshes.current);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	/* Create a new mesh. */
	mesh = &hr->mesh_list[vp_index][polytype][mesh_index++];
	HL_ASSERT (mesh_index < MAX_MESHES);

	hr->num_meshes[vp_index][polytype] = mesh_index;

	/* Make the mesh current and set the rendering state. */
	hr->meshes.current = mesh;
	update_and_set_rendstate (hr, mesh);
	mesh->addr[0] = addr;

	/* Clear the push buffer. */
	hr->push.num_verts = 0;
	hr->push.num_tris = 0;
}

void
Renderer::end_mesh (uint32_t addr)
{
	hl::Renderer *rend = this;
	Renderer *hr = (Renderer *) rend;

	HL_ASSERT (hr);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	HL_ASSERT (hr->meshes.current);

	/* Upload the pushed vertex data. */
	upload_vertex_data (hr, hr->meshes.current);
	hr->meshes.current->addr[1] = addr;

	/* Make sure there is no current mesh bound. */
	hr->meshes.current = NULL;
}

/* A material or a texhead recalled in the middle of a mesh: the triangles
 * after it go to a new mesh, with that state (PHARRIER's shoes: their
 * soles' texture comes after the uppers', in the same mesh). The strip goes
 * on: its last two vertices stay. */
void
Renderer::split_mesh (uint32_t addr)
{
	hl::Renderer *rend = this;
	Renderer *hr = (Renderer *) rend;
	unsigned num_verts;

	HL_ASSERT (hr);

	if (hr->debug.flags[HR_DEBUG_NO_3D] || !hr->meshes.current)
		return;

	/* No triangle yet (the material, then the texhead): the mesh takes
	 * the new state itself, in its own entries */
	if (!hr->push.num_tris) {
		hikaru_mesh_t *mesh = hr->meshes.current;

		hr->mat_list[mesh->mat_index] = MAT0;
		hr->tex_list[mesh->tex_index] = TEX0;
		mesh->alpha_thresh[0] = ATABLE[MAT0.alpha_index].part.lo * INV255;
		mesh->alpha_thresh[1] = (ATABLE[MAT0.alpha_index].part.hi & 0xFF) * INV255;
		return;
	}

	num_verts = hr->push.num_verts;
	end_mesh (addr);
	begin_mesh (addr, false);
	hr->push.num_verts = MIN2 (num_verts, 2u);
}

/* How a mesh of poly type 5 or 6 blends with the scene: its material's
 * blending mode (A81). The scene's alpha says where a mesh was drawn (the
 * layers' presence test, 1A0001BC): 1 there, whatever the mesh's own. */
static void
set_blending (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned mode = 0;

	if (mesh->mat_index != ~0u)
		mode = hr->mat_list[mesh->mat_index].blending_mode;

	/* Modes 0 and 2 weight the source by its alpha: the shader has done it
	 * (weighs_by_alpha) */
	switch (mode) {
	case 0:	/* by its alpha */
		glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		                     GL_CONSTANT_ALPHA, GL_ONE);
		break;
	case 1:	/* times the scene */
		glBlendFuncSeparate (GL_DST_COLOR, GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE);
		break;
	case 2:	/* added, by its alpha: AIRTRIX's trick trails, lights */
		glBlendFuncSeparate (GL_ONE, GL_ONE, GL_CONSTANT_ALPHA, GL_ONE);
		break;
	case 3:	/* added, the scene dimmed by its alpha */
		glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		                     GL_CONSTANT_ALPHA, GL_ONE);
		break;
	}
}

/*
 * Translucency
 * ============
 *
 * The board blends the translucent polygons from the farthest at each pixel,
 * so that two crossing meshes each cover the other where it is nearer.
 * Sorting whole meshes by their middle gets it wrong where they cross or
 * overlap in depth. With OpenGL 4.3 (and without MSAA), the translucent
 * meshes of a viewport are drawn twice:
 *
 *  - collected: each fragment shaded as usual, then tested against a copy of
 *    the opaque meshes' depth by the shader itself, and kept in its pixel's
 *    list (a storage buffer of uvec4: colour, depth, next + 1, the mesh's
 *    blending mode and order; an r32ui image holds each pixel's last + 1);
 *  - resolved: a pass over the whole picture blends each pixel's list over
 *    a copy of the picture drawn so far, the farthest first, equal depths in
 *    the order sent (the mesh's, then its triangle's), with set_blending's
 *    four modes, in the arithmetic of an 8-bit colour buffer: each result
 *    rounded to the nearest before the next. Eight fragments are put in
 *    order at a time, again until the list is done: none is lost. The pass
 *    empties the heads for the next list.
 *
 * The buffer, 4 fragments a pixel at first, grows when a frame fills it. As
 * Odin's author, Réda Chérif-Touil, blends them.
 */

#define OIT_MAX_NODES		(16u << 20)
#define OIT_BELOW_UNIT		6

static GLuint make_color_texture (unsigned w, unsigned h, GLint filter);

static const char *oit_vs_source =
"#version 430									\n \
										\n \
void										\n \
main (void)									\n \
{										\n \
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);		\n \
	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);				\n \
}";

static const char *oit_fs_source =
"#version 430									\n \
										\n \
#define KEPT 8									\n \
										\n \
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;		\n \
layout (std430, binding = 0) readonly buffer Nodes { uvec4 nodes[]; };	\n \
uniform sampler2D u_below;							\n \
out vec4 frag_color;								\n \
										\n \
/* Whether a fragment (depth, order, node) is blended before another: the	\n \
 * farther, then the first sent; the node only tells two apart */		\n \
bool										\n \
before (float d, uint o, uint i, float d2, uint o2, uint i2)		\n \
{										\n \
	return d > d2 || (d == d2 && (o < o2 || (o == o2 && i < i2)));	\n \
}										\n \
										\n \
void										\n \
main (void)									\n \
{										\n \
	ivec2 pixel = ivec2 (gl_FragCoord.xy);					\n \
	uint head = imageAtomicExchange (u_heads, pixel, 0u);			\n \
	float depth[KEPT], last_d = 2.0;					\n \
	uint order[KEPT], index[KEPT], last_o = 0u, last_i = 0u, i;		\n \
	int num, j;								\n \
	bool more;								\n \
	vec3 color;								\n \
										\n \
	if (head == 0u)								\n \
		discard;							\n \
	color = floor (texelFetch (u_below, pixel, 0).rgb * 255.0 + 0.5);	\n \
	for (;;) {								\n \
		/* The next KEPT in order, after the last blended */		\n \
		num = 0;							\n \
		more = false;							\n \
		for (i = head; i != 0u; i = nodes[i - 1u].z) {			\n \
			float d = uintBitsToFloat (nodes[i - 1u].y);		\n \
			uint o = nodes[i - 1u].w >> 2;				\n \
										\n \
			if (!before (last_d, last_o, last_i, d, o, i))		\n \
				continue;					\n \
			if (num < KEPT) {					\n \
				j = num++;					\n \
			} else {						\n \
				more = true;					\n \
				if (!before (d, o, i, depth[KEPT - 1], order[KEPT - 1], index[KEPT - 1]))	\n \
					continue;				\n \
				j = KEPT - 1;					\n \
			}							\n \
			for (; j > 0 && before (d, o, i, depth[j - 1], order[j - 1], index[j - 1]); j--) {	\n \
				depth[j] = depth[j - 1];			\n \
				order[j] = order[j - 1];			\n \
				index[j] = index[j - 1];			\n \
			}							\n \
			depth[j] = d;						\n \
			order[j] = o;						\n \
			index[j] = i;						\n \
		}								\n \
		/* Blended in turn, as an 8-bit buffer would (set_blending's	\n \
		 * modes; 0 and 2 come weighted by their alpha) */		\n \
		for (j = 0; j < num; j++) {					\n \
			uvec4 n = nodes[index[j] - 1u];				\n \
			vec4 c = unpackUnorm4x8 (n.x);				\n \
			uint mode = n.w & 3u;					\n \
										\n \
			if (mode == 1u)						\n \
				color = c.rgb * color;				\n \
			else if (mode == 2u)					\n \
				color = c.rgb * 255.0 + color;			\n \
			else							\n \
				color = c.rgb * 255.0 + color * (1.0 - c.a);	\n \
			color = floor (clamp (color, 0.0, 255.0) + 0.5);	\n \
		}								\n \
		if (!more)							\n \
			break;							\n \
		last_d = depth[KEPT - 1];					\n \
		last_o = order[KEPT - 1];					\n \
		last_i = index[KEPT - 1];					\n \
	}									\n \
	frag_color = vec4 (color / 255.0, 1.0);					\n \
}";

static void
destroy_oit_buffers (Renderer *hr)
{
	auto &o = hr->oit;

	if (o.heads)
		glDeleteTextures (1, &o.heads);
	if (o.depth_tex)
		glDeleteTextures (1, &o.depth_tex);
	if (o.depth_fb)
		glDeleteFramebuffers (1, &o.depth_fb);
	if (o.color_tex)
		glDeleteTextures (1, &o.color_tex);
	if (o.color_fb)
		glDeleteFramebuffers (1, &o.color_fb);
	o.color_tex = o.color_fb = 0;
	if (o.nodes)
		glDeleteBuffers (1, &o.nodes);
	if (o.counter)
		glDeleteBuffers (1, &o.counter);
	o.heads = o.depth_tex = o.depth_fb = o.nodes = o.counter = 0;
	o.w = o.h = 0;
}

/* The lists' buffers at the scene's size (and the nodes' count): 0 when
 * ready */
static int
build_oit_buffers (Renderer *hr)
{
	auto &o = hr->oit;
	unsigned w = hr->framebuffer.w, h = hr->framebuffer.h;
	uint32_t zero = 0;
	GLint bound = 0;

	if (o.heads && o.w == w && o.h == h && o.nodes)
		return 0;
	destroy_oit_buffers (hr);
	if (!o.max_nodes)
		o.max_nodes = MIN2 ((uint32_t) (w * h * 4), OIT_MAX_NODES);

	std::vector<uint32_t> zeros ((size_t) w * h, 0);
	glGetIntegerv (GL_FRAMEBUFFER_BINDING, &bound);
	glGenTextures (1, &o.heads);
	glBindTexture (GL_TEXTURE_2D, o.heads);
	glTexStorage2D (GL_TEXTURE_2D, 1, GL_R32UI, w, h);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, zeros.data ());

	glGenTextures (1, &o.depth_tex);
	glBindTexture (GL_TEXTURE_2D, o.depth_tex);
	glTexStorage2D (GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, w, h);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glBindTexture (GL_TEXTURE_2D, 0);
	glGenFramebuffers (1, &o.depth_fb);
	glBindFramebuffer (GL_FRAMEBUFFER, o.depth_fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, o.depth_tex, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		HL_ERROR ("HR translucency: no depth copy; each mesh sorted whole");
		glBindFramebuffer (GL_FRAMEBUFFER, bound);
		destroy_oit_buffers (hr);
		o.available = false;
		return -1;
	}
	o.color_tex = make_color_texture (w, h, GL_NEAREST);
	glGenFramebuffers (1, &o.color_fb);
	glBindFramebuffer (GL_FRAMEBUFFER, o.color_fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o.color_tex, 0);
	if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		HL_ERROR ("HR translucency: no picture copy; each mesh sorted whole");
		glBindFramebuffer (GL_FRAMEBUFFER, bound);
		destroy_oit_buffers (hr);
		o.available = false;
		return -1;
	}
	glBindFramebuffer (GL_FRAMEBUFFER, bound);

	glGenBuffers (1, &o.nodes);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, o.nodes);
	glBufferData (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr) o.max_nodes * 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
	glGenBuffers (1, &o.counter);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, o.counter);
	glBufferData (GL_ATOMIC_COUNTER_BUFFER, sizeof (zero), &zero, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
	if (glGetError () != GL_NO_ERROR) {
		HL_ERROR ("HR translucency: no room for %u fragments; each mesh sorted whole", o.max_nodes);
		destroy_oit_buffers (hr);
		o.available = false;
		return -1;
	}
	o.w = w;
	o.h = h;
	HL_PRINT ("HR translucency sorted at each pixel: %ux%u, %u fragments (%u MB)",
	          w, h, o.max_nodes, (unsigned) ((uint64_t) o.max_nodes * 16 >> 20));
	return 0;
}

/* The translucent meshes of a viewport, sorted at each pixel (see above) */
static void
draw_translucent_per_pixel (Renderer *hr, hikaru_mesh_t *meshes, unsigned num)
{
	auto &o = hr->oit;
	GLint scene_fb = 0;
	uint32_t zero = 0;
	unsigned j;

	/* The opaque meshes' depth, for the collecting shaders */
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &scene_fb);
	glDisable (GL_SCISSOR_TEST);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, scene_fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, o.depth_fb);
	glBlitFramebuffer (0, 0, o.w, o.h, 0, 0, o.w, o.h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
	glEnable (GL_SCISSOR_TEST);

	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, o.counter);
	glBufferSubData (GL_ATOMIC_COUNTER_BUFFER, 0, sizeof (zero), &zero);
	glBindBufferBase (GL_ATOMIC_COUNTER_BUFFER, 0, o.counter);
	glBindBufferBase (GL_SHADER_STORAGE_BUFFER, 0, o.nodes);
	glBindImageTexture (0, o.heads, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
	glActiveTexture (GL_TEXTURE0 + OIT_DEPTH_UNIT);
	glBindTexture (GL_TEXTURE_2D, o.depth_tex);
	glActiveTexture (GL_TEXTURE0);

	/* Collected: no colour, no depth; the shaders test the depth */
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask (GL_FALSE);
	glDisable (GL_DEPTH_TEST);
	o.collecting = true;
	for (j = 0; j < num; j++) {
		unsigned mode = 0;

		if (meshes[j].mat_index != ~0u)
			mode = hr->mat_list[meshes[j].mat_index].blending_mode;
		/* The order: the mesh's in the list, then (the shader) its
		 * triangle's */
		o.tag = (mode & 3) | (j << 18);
		draw_mesh (hr, &meshes[j]);
	}
	o.collecting = false;
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* The picture drawn so far, which the lists are blended over */
	glDisable (GL_SCISSOR_TEST);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, scene_fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, o.color_fb);
	glBlitFramebuffer (0, 0, o.w, o.h, 0, 0, o.w, o.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
	glEnable (GL_SCISSOR_TEST);
	glActiveTexture (GL_TEXTURE0 + OIT_BELOW_UNIT);
	glBindTexture (GL_TEXTURE_2D, o.color_tex);
	glActiveTexture (GL_TEXTURE0);

	/* Resolved, over the whole picture, the pixels with a list written
	 * whole (their scene's alpha 1, as the meshes' blending gave it) */
	glUseProgram (o.resolve);
	glUniform1i (glGetUniformLocation (o.resolve, "u_below"), OIT_BELOW_UNIT);
	forget_mesh_state (hr);	/* the next mesh's program again */
	glBindVertexArray (o.vao);
	glViewport (0, 0, o.w, o.h);
	glDisable (GL_CULL_FACE);
	glDisable (GL_BLEND);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glEnable (GL_BLEND);
	glMemoryBarrier (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glEnable (GL_CULL_FACE);
	glEnable (GL_DEPTH_TEST);
	glBindVertexArray (hr->meshes.vao);
	HL_ASSERT_NO_GL_ERROR ();

	/* Now and then, whether a frame filled the buffer: then twice as
	 * many, from the next frame (it stalls, so not every frame) */
	if (++o.frames % 120 == 0 && o.max_nodes < OIT_MAX_NODES) {
		uint32_t used = 0;

		glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, o.counter);
		glGetBufferSubData (GL_ATOMIC_COUNTER_BUFFER, 0, sizeof (used), &used);
		if (used > o.max_nodes) {
			HL_PRINT ("HR translucency: %u fragments for %u places, more made", used, o.max_nodes);
			o.max_nodes = MIN2 (MAX2 (o.max_nodes * 2, used + used / 4), OIT_MAX_NODES);
			destroy_oit_buffers (hr);
		}
	}
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
}

static void
draw_meshes_for_polytype (Renderer *hr, unsigned vpi, int polytype)
{
	hikaru_mesh_t *meshes = hr->mesh_list[vpi][polytype];
	unsigned num = hr->num_meshes[vpi][polytype];
	int j;

	if (num == 0)
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] != polytype)
		return;

	LOG (" ==== DRAWING VP %u, POLYTYPE %d ====", vpi, polytype);

	/* The constant alpha of 1 the scene's alpha takes (set_blending) */
	glBlendColor (0.0f, 0.0f, 0.0f, 1.0f);

	switch (polytype) {
	case HIKARU_POLYTYPE_TRANSPARENT:
		/* Punch-through: alpha-tested in the shader, opaque */
		glEnable (GL_BLEND);
		glBlendFuncSeparate (GL_ONE, GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE);
		glDepthMask (GL_TRUE);
		break;
	case HIKARU_POLYTYPE_TRANSLUCENT:
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
		break;
	case HIKARU_POLYTYPE_BACKGROUND:
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
		break;
	default:
		/* Opaque: blended only for the scene's alpha */
		glEnable (GL_BLEND);
		glBlendFuncSeparate (GL_ONE, GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE);
		glDepthMask (GL_TRUE);
		break;
	}
	
	/* Sorted at each pixel when the card can (see "Translucency") */
	if (polytype == HIKARU_POLYTYPE_TRANSLUCENT && hr->oit.on) {
		draw_translucent_per_pixel (hr, meshes, num);
		return;
	}
	/* Else the translucent meshes from back to front, as the hardware
	 * draws them: AIRTRIX's OHOSOHO sign is a panel with its letters cut
	 * out, then a magenta quad behind it, which in the list's order
	 * covered the panel (these meshes write no depth) */
	if (polytype == HIKARU_POLYTYPE_TRANSLUCENT) {
		std::vector<std::pair<float, unsigned>> order;

		for (j = 0; j < num; j++) {
			hikaru_mesh_t *mesh = &meshes[j];
			float z = 0.0f;

			if (mesh->mv_index != ~0u) {
				auto &m = hr->mv_list[mesh->mv_index].mtx;
				z = m[0][2] * mesh->center[0] + m[1][2] * mesh->center[1] +
				    m[2][2] * mesh->center[2] + m[3][2];
			}
			order.push_back ({ z, (unsigned) j });
		}
		/* The farthest first: the most negative z; the list's order
		 * among equals */
		std::stable_sort (order.begin (), order.end (),
		                  [] (const std::pair<float, unsigned> &a,
		                      const std::pair<float, unsigned> &b) {
			return a.first < b.first;
		});
		for (auto &o : order) {
			set_blending (hr, &meshes[o.second]);
			draw_mesh (hr, &meshes[o.second]);
		}
		return;
	}

	for (j = 0; j < num; j++) {
		hikaru_mesh_t *mesh = &meshes[j];
		if (polytype == HIKARU_POLYTYPE_BACKGROUND)
			set_blending (hr, mesh);
		else if (is_faded_by_depth (hr, mesh))
			/* Into what is behind, by the shader's alpha (621 D) */
			glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
			                     GL_CONSTANT_ALPHA, GL_ONE);
		else
			glBlendFuncSeparate (GL_ONE, GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE);
		draw_mesh (hr, mesh);
	}
}

static void
draw_scene (Renderer *hr)
{
	/* The punch-through meshes write depth: before the translucent ones,
	 * which the scene behind shows through */
	static const int sorted_polytypes[] = {
		HIKARU_POLYTYPE_BACKGROUND,
		HIKARU_POLYTYPE_SHADOW_A,
		HIKARU_POLYTYPE_SHADOW_B,
		HIKARU_POLYTYPE_OPAQUE,
		HIKARU_POLYTYPE_TRANSPARENT,
		HIKARU_POLYTYPE_TRANSLUCENT,
	};
	unsigned vpi, i;

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	if (!upload_frame_vertices (hr))
		return;

	/* Translucency sorted at each pixel: OpenGL 4.3, the setting, no
	 * MSAA (the lists are a pixel's, not a sample's) */
	if (!hr->oit.checked) {
		hr->oit.checked = true;
		hr->oit.available = GLEW_VERSION_4_3 != 0;
		if (hr->oit.available) {
			hr->oit.resolve = hl::gl::compile_program (oit_vs_source, oit_fs_source);
			glGenVertexArrays (1, &hr->oit.vao);
		}
		HL_PRINT ("HR translucency %s", hr->oit.available ?
		          "sorted at each pixel (OpenGL 4.3)" : "sorted by mesh (no OpenGL 4.3)");
	}
	hr->oit.on = hr->oit.available && hr->view.sort_pixels && !hr->framebuffer.ms_fb &&
	             build_oit_buffers (hr) == 0;
	/* The game's programs, before its first frame (see "The programs'
	 * cache") */
	warm_up_programs ();
	forget_mesh_state (hr);

	/* Note that "the pixel ownership test, the scissor test, dithering,
	 * and the buffer writemasks affect the operation of glClear". */
	glDepthMask (GL_TRUE);
	glDisable (GL_SCISSOR_TEST);
	glClearColor (hr->frame_gpu.fb_config.clear[0] * INV255,
	              hr->frame_gpu.fb_config.clear[1] * INV255,
	              hr->frame_gpu.fb_config.clear[2] * INV255,
	              hr->frame_gpu.fb_config.clear[3] * INV255);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* The scene's 640x480 (854 in widescreen), times RENDER_SCALE: the
	 * box a context starts with is its first window's, 16x16 for the
	 * drawing thread's (THREADED_RENDERING) */
	glEnable (GL_SCISSOR_TEST);
	glScissor (0, 0, hr->framebuffer.w, hr->framebuffer.h);

	glEnable (GL_DEPTH_TEST);
	glDepthFunc (GL_LEQUAL);

	glEnable (GL_CULL_FACE);
	glCullFace (GL_BACK);

	for (vpi = 0; vpi < 8; vpi++) {
		glDepthMask (GL_TRUE);
		glClear (GL_DEPTH_BUFFER_BIT);
		for (i = 0; i < NUMELEM (sorted_polytypes); i++)
			draw_meshes_for_polytype (hr, vpi, sorted_polytypes[i]);
	}
	glBindVertexArray (0);
	hr->meshes.verts.clear ();

	glDepthMask (GL_TRUE);

	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, hr->framebuffer.w, hr->framebuffer.h);
}

static void
destroy_3d_state (Renderer *hr)
{
	unsigned vpi, i;

	free (hr->vp_list);
	free (hr->mv_list);
	free (hr->mat_list);
	free (hr->tex_list);
	free (hr->ls_list);
	free (hr->ls_tables);

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			free (hr->mesh_list[vpi][i]);

	hl::gl::destroy_program (hr->meshes.program);
	HL_ASSERT_NO_GL_ERROR ();

	destroy_oit_buffers (hr);
	hl::gl::destroy_program (hr->oit.resolve);
	if (hr->oit.vao)
		glDeleteVertexArrays (1, &hr->oit.vao);

	if (hr->meshes.vbo)
		glDeleteBuffers (1, &hr->meshes.vbo);
	if (hr->meshes.vao) {
		glBindVertexArray (0);
		glDeleteVertexArrays (1, &hr->meshes.vao);
	}
}

static int
build_3d_state (Renderer *hr)
{
	unsigned vpi, i;

	hr->vp_list = (hikaru_viewport_t *)
			malloc (sizeof (hikaru_viewport_t) * MAX_VIEWPORTS);
	hr->mv_list = (hikaru_modelview_t *)
			malloc (sizeof (hikaru_modelview_t) * MAX_MODELVIEWS);
	hr->mat_list = (hikaru_material_t *)
			malloc (sizeof (hikaru_material_t) * MAX_MATERIALS);
	hr->tex_list = (hikaru_texhead_t *)
			malloc (sizeof (hikaru_texhead_t) * MAX_TEXHEADS);
	hr->ls_list = (hikaru_lightset_t *)
			malloc (sizeof (hikaru_lightset_t) * MAX_LIGHTSETS);
	hr->ls_tables = (Renderer::hikaru_light_tables_t *)
			malloc (sizeof (Renderer::hikaru_light_tables_t) * MAX_LIGHTSETS);

	if (!hr->vp_list || !hr->mv_list || !hr->mat_list || !hr->tex_list || !hr->ls_list ||
	    !hr->ls_tables)
		return -1;
	/* Its GL objects at the first scene, in the context that draws it */
	memset ((void *) &hr->oit, 0, sizeof (hr->oit));

	for (vpi = 0; vpi < 8; vpi++) {
		for (i = 0; i < 8; i++) {
			hr->mesh_list[vpi][i] =
				(hikaru_mesh_t *) malloc (sizeof (hikaru_mesh_t) * MAX_MESHES);
			if (!hr->mesh_list[vpi][i])
				return -1;
		}
	}

	return 0;
}

/****************************************************************************
 Framebuffers
****************************************************************************/

static const struct {
       vec3f_t position;
       vec2f_t texcoords;
} fb_vbo_data[] = {
       { { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
       { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
       { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
       { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
};

static const char *fb_vs_source =
"#version 140									\n"
"										\n"
"#extension GL_ARB_explicit_attrib_location : require				\n"
"										\n"
"uniform mat4 u_projection =							\n"
"	mat4 (vec4 ( 2.0,  0.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0, -2.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0,  0.0, -1.0,  0.0),					\n"
"	      vec4 (-1.0,  1.0,  0.0,  1.0));					\n"
"										\n"
"layout(location = 0) in vec3 i_position;					\n"
"layout(location = 1) in vec2 i_texcoords;					\n"
"										\n"
"out vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	gl_Position = u_projection * vec4 (i_position, 1.0);			\n"
"	p_texcoords = i_texcoords;						\n"
"}										\n";

static const char *fb_fs_source =
"#version 140									\n"
"										\n"
"out vec4 frag_color;								\n"
"										\n"
"uniform sampler2D u_front;							\n"
"uniform sampler2D u_back;							\n"
"uniform sampler2D u_layer1;							\n"
"uniform sampler2D u_layer2;							\n"
"uniform float u_factor = 0.0;							\n"
"uniform float u_mult_f = 1.0;							\n"
"uniform float u_mult_b = 1.0;							\n"
"uniform float u_mult_1 = 1.0;							\n"
"uniform float u_mult_2 = 1.0;							\n"
"uniform int u_order = 0;							\n"
"uniform int u_test = 0;							\n"
"uniform int u_invert_f = 0;							\n"
"uniform int u_invert_b = 0;							\n"
"/* Where the 2D layers are across the scene: their left edge, their width	\n"
" * (widescreen: 4:3 in the middle, or stretched) */				\n"
"uniform vec2 u_layer_x = vec2 (0.0, 1.0);					\n"
"/* RENDER_SCALE: the scene's texels a pixel of the picture covers, a	\n"
" * texel's size; BLOOM's glow and its strength; CRT_FILTER's strength, the	\n"
" * picture's size in pixels */							\n"
"uniform vec2 u_down = vec2 (1.0);						\n"
"uniform vec2 u_texel = vec2 (0.0);						\n"
"uniform sampler2D u_bloom;							\n"
"uniform float u_bloom_k = 0.0;							\n"
"uniform float u_crt = 0.0;							\n"
"uniform vec2 u_out = vec2 (640.0, 480.0);					\n"
"/* CROP_BARREL (hl/aspect.cpp): the parts a crop cuts bent onto the frame's	\n"
" * edges along an axis (0 across, 1 up and down, -1 none); the frame in the	\n"
" * target's pixels, the viewport then; edge, middle, start, scale; the	\n"
" * picture each edge shows, the arc each covers */				\n"
"uniform int u_barrel = -1;							\n"
"uniform vec4 u_frame;								\n"
"uniform vec4 u_barrel1;							\n"
"uniform vec4 u_barrel2;							\n"
"										\n"
"in vec2 p_texcoords;								\n"
"										\n"
"%BOX%										\n"
"										\n"
"/* Along the bent axis: q in the frame (0..1 from its left or top) to the	\n"
" * picture (the same way); a cylinder seen from the front on each edge */	\n"
"float edge_arc (float x, float len, float angle, float edge) {		\n"
"	if (len <= edge * 1.0001 || angle < 1e-4)				\n"
"		return len * x / edge;						\n"
"	return len * asin (sin (angle) * x / edge) / angle;			\n"
"}										\n"
"										\n"
"float barrel (float q) {							\n"
"	float u;								\n"
"	if (q < u_barrel1.x)							\n"
"		u = u_barrel1.z - edge_arc (u_barrel1.x - q, u_barrel2.x,	\n"
"		                            u_barrel2.z, u_barrel1.x);		\n"
"	else if (q > u_barrel1.x + u_barrel1.y)				\n"
"		u = u_barrel1.z + u_barrel1.y + edge_arc (q - u_barrel1.x - u_barrel1.y,	\n"
"		                                          u_barrel2.y, u_barrel2.w, u_barrel1.x);	\n"
"	else									\n"
"		u = u_barrel1.z + q - u_barrel1.x;				\n"
"	return u * u_barrel1.w;							\n"
"}										\n"
"										\n"
"/* A scene's buffer, larger than the picture: the mean of its texels	\n"
" * there */									\n"
"vec4 scene (sampler2D s, vec2 uv) {						\n"
"	if (u_down.x <= 1.0 && u_down.y <= 1.0)					\n"
"		return texture (s, uv);						\n"
"	return box (s, uv, u_texel, u_down);					\n"
"}										\n"
"										\n"
"void main (void) {								\n"
"	/* The picture's point here: the viewport's, or bent */		\n"
"	vec2 tc = p_texcoords;							\n"
"	if (u_barrel >= 0) {							\n"
"		vec2 q = (gl_FragCoord.xy - u_frame.xy) / u_frame.zw;		\n"
"		if (u_barrel == 0)						\n"
"			tc.x = barrel (q.x);					\n"
"		else								\n"
"			tc.y = 1.0 - barrel (1.0 - q.y);			\n"
"	}									\n"
"	vec2 p_texcoords_inv = vec2 ((tc.s - u_layer_x.x) / u_layer_x.y,	\n"
"	                             1 - tc.t);					\n"
"	float inside = (p_texcoords_inv.s >= 0.0 && p_texcoords_inv.s <= 1.0) ?	\n"
"	               1.0 : 0.0;							\n"
"	vec4 texel_f, texel_b;							\n"
"										\n"
"	if (u_invert_f == 0)							\n"
"		texel_f = scene (u_front, tc);					\n"
"	else									\n"
"		texel_f = texture (u_front, p_texcoords_inv) * inside;		\n"
"	texel_f = texel_f * u_mult_f;						\n"
"										\n"
"	if (u_invert_b == 0)							\n"
"		texel_b = scene (u_back,  tc);					\n"
"	else									\n"
"		texel_b = texture (u_back,  p_texcoords_inv) * inside;		\n"
"	texel_b = texel_b * u_mult_b;						\n"
"										\n"
"	vec4 texel_1 = texture (u_layer1, p_texcoords_inv) * inside;		\n"
"	vec4 texel_2 = texture (u_layer2, p_texcoords_inv) * inside;		\n"
"										\n"
"	/* The 3D and the shown layers (multiplier > 0) stacked in the order	\n"
"	 * of 1A0001B8, bottom to top: from the top down, the first there	\n"
"	 * (1A0001BC: by its alpha or its colour) gives the pixel, the bottom	\n"
"	 * one otherwise (see hikaru-gpu.cpp). The multiplier scales the	\n"
"	 * colour (A2BGR10: PHARRIER's warning). The glow is the scene's	\n"
"	 * alone. The scene tested by its alpha goes over what is under it	\n"
"	 * by that alpha, its colour already weighted by it (cleared to 0):	\n"
"	 * the same where it is 0 or 1, smooth on the edges of MSAA and	\n"
"	 * RENDER_SCALE. The scene's alpha goes on: the blending of the	\n"
"	 * frames writes it back to the scene. */				\n"
"	vec4 src[3];								\n"
"	bool on[3], there[3];							\n"
"	src[0] = mix (texel_f, texel_b, u_factor);				\n"
"	if (u_bloom_k > 0.0)							\n"
"		src[0].rgb += texture (u_bloom, tc).rgb * u_bloom_k;		\n"
"	src[1] = clamp (texel_1 * u_mult_1, 0.0, 1.0);				\n"
"	src[2] = clamp (texel_2 * u_mult_2, 0.0, 1.0);				\n"
"	on[0] = true;								\n"
"	on[1] = u_mult_1 > 0.0;							\n"
"	on[2] = u_mult_2 > 0.0;							\n"
"	for (int n = 0; n < 3; n++)						\n"
"		there[n] = (u_test & (1 << n)) != 0 ? src[n].a > 0.0 :		\n"
"		           any (greaterThan (src[n].rgb, vec3 (0.0)));		\n"
"	int order[12] = int[12] (2, 1, 0,  2, 0, 1,  1, 0, 2,  0, 2, 1);	\n"
"	bool bottom = true;							\n"
"	frag_color = vec4 (0.0);						\n"
"	for (int k = 0; k < 3; k++) {						\n"
"		int n = order[u_order * 3 + k];					\n"
"		if (!on[n])							\n"
"			continue;						\n"
"		if (bottom)							\n"
"			frag_color = src[n];					\n"
"		else if (n == 0 && there[0] && (u_test & 1) != 0)		\n"
"			frag_color = vec4 (src[0].rgb +				\n"
"			                   frag_color.rgb * (1.0 - src[0].a),	\n"
"			                   1.0);				\n"
"		else if (there[n])						\n"
"			frag_color = src[n];					\n"
"		bottom = false;							\n"
"	}									\n"
"										\n"
"	/* A CRT's 480 scanlines, dark between, and its aperture grille's	\n"
"	 * stripes, a pixel each: none below twice the rows, where they would	\n"
"	 * beat with the pixels; brighter, for the light they take */		\n"
"	if (u_crt > 0.0) {							\n"
"		float k = u_crt * clamp (u_out.y / 480.0 - 1.0, 0.0, 1.0);	\n"
"		float phase = fract ((1.0 - tc.t) * 480.0);			\n"
"		float beam = 0.5 + 0.5 * cos (6.2831853 * (phase - 0.5));	\n"
"		int m = int (mod (gl_FragCoord.x, 3.0));			\n"
"		vec3 mask = vec3 (m == 0 ? 1.0 : 0.7, m == 1 ? 1.0 : 0.7,	\n"
"		                  m == 2 ? 1.0 : 0.7);				\n"
"		frag_color.rgb *= mix (1.0, 0.3 + 0.7 * beam, k) * (1.0 + 0.4 * k);	\n"
"		frag_color.rgb *= mix (vec3 (1.0), mask, 0.6 * k);		\n"
"	}									\n"
"}\n";

/* The texels of s under a pixel down times smaller than them: up to 8x8
 * taps, each the mean of 2x2 texels (LINEAR), at the texels' centres when
 * down is whole (an exact box) */
static const char *box_source =
"vec4 box (sampler2D s, vec2 uv, vec2 texel, vec2 down) {			\n"
"	ivec2 n = clamp (ivec2 (ceil (down * 0.5 - 0.001)), ivec2 (1), ivec2 (8));	\n"
"	vec2 step = down / vec2 (n) * texel;					\n"
"	vec2 first = uv - 0.5 * down * texel + 0.5 * step;			\n"
"	vec4 sum = vec4 (0.0);							\n"
"	for (int y = 0; y < n.y; y++)						\n"
"		for (int x = 0; x < n.x; x++)					\n"
"			sum += texture (s, first + vec2 (x, y) * step);		\n"
"	return sum / float (n.x * n.y);						\n"
"}										\n";

/* FXAA: the edges' steps blurred along the edge, where the luma changes
 * across it; the rest left alone */
static const char *fxaa_fs_source =
"#version 140									\n"
"										\n"
"out vec4 frag_color;								\n"
"uniform sampler2D u_texture;							\n"
"uniform vec2 u_texel;								\n"
"in vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	const vec3 to_luma = vec3 (0.299, 0.587, 0.114);			\n"
"	vec2 uv = p_texcoords;							\n"
"	vec4 m = texture (u_texture, uv);					\n"
"	/* The four corners' luma, the texture's rows going up */		\n"
"	float bl = dot (texture (u_texture, uv + vec2 (-1.0, -1.0) * u_texel).rgb, to_luma);	\n"
"	float br = dot (texture (u_texture, uv + vec2 ( 1.0, -1.0) * u_texel).rgb, to_luma);	\n"
"	float tl = dot (texture (u_texture, uv + vec2 (-1.0,  1.0) * u_texel).rgb, to_luma);	\n"
"	float tr = dot (texture (u_texture, uv + vec2 ( 1.0,  1.0) * u_texel).rgb, to_luma);	\n"
"	float lm = dot (m.rgb, to_luma);					\n"
"	float lmin = min (lm, min (min (bl, br), min (tl, tr)));		\n"
"	float lmax = max (lm, max (max (bl, br), max (tl, tr)));		\n"
"										\n"
"	/* The luma's change turned a quarter: along the edge, blurred */	\n"
"	vec2 dir = vec2 ((tl + tr) - (bl + br), (bl + tl) - (br + tr));		\n"
"	float reduce = max ((bl + br + tl + tr) * (0.25 / 8.0), 1.0 / 128.0);	\n"
"	float scale = 1.0 / (min (abs (dir.x), abs (dir.y)) + reduce);		\n"
"	dir = clamp (dir * scale, vec2 (-8.0), vec2 (8.0)) * u_texel;		\n"
"										\n"
"	vec3 a = 0.5 * (texture (u_texture, uv - dir / 6.0).rgb +		\n"
"	                texture (u_texture, uv + dir / 6.0).rgb);		\n"
"	vec3 b = 0.5 * a + 0.25 * (texture (u_texture, uv - dir * 0.5).rgb +	\n"
"	                           texture (u_texture, uv + dir * 0.5).rgb);	\n"
"	float lb = dot (b, to_luma);						\n"
"	frag_color = vec4 ((lb < lmin || lb > lmax) ? a : b, m.a);		\n"
"}\n";

/* BLOOM's bright parts, a quarter of the scene's size */
static const char *bright_fs_source =
"#version 140									\n"
"										\n"
"out vec4 frag_color;								\n"
"uniform sampler2D u_texture;							\n"
"uniform vec2 u_texel;								\n"
"uniform vec2 u_down;								\n"
"in vec2 p_texcoords;								\n"
"										\n"
"%BOX%										\n"
"										\n"
"void main (void) {								\n"
"	/* By the luma: a blue sky is not a light */				\n"
"	vec3 c = box (u_texture, p_texcoords, u_texel, u_down).rgb;		\n"
"	float l = dot (c, vec3 (0.299, 0.587, 0.114));				\n"
"	frag_color = vec4 (c * smoothstep (0.6, 0.95, l), 1.0);			\n"
"}\n";

/* BLOOM's blur, along u_texel (one texel across, or down): nine texels'
 * gaussian in five LINEAR taps */
static const char *blur_fs_source =
"#version 140									\n"
"										\n"
"out vec4 frag_color;								\n"
"uniform sampler2D u_texture;							\n"
"uniform vec2 u_texel;								\n"
"in vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	vec2 d1 = 1.3846154 * u_texel, d2 = 3.2307692 * u_texel;		\n"
"	vec3 c = texture (u_texture, p_texcoords).rgb * 0.2270270;		\n"
"	c += (texture (u_texture, p_texcoords + d1).rgb +			\n"
"	      texture (u_texture, p_texcoords - d1).rgb) * 0.3162162;		\n"
"	c += (texture (u_texture, p_texcoords + d2).rgb +			\n"
"	      texture (u_texture, p_texcoords - d2).rgb) * 0.0702703;		\n"
"	frag_color = vec4 (c, 1.0);						\n"
"}\n";

/* A shader's source with box_source in place of its %BOX% */
static std::string
with_box (const char *source)
{
	std::string s = source;
	size_t at = s.find ("%BOX%");

	if (at != std::string::npos)
		s.replace (at, 5, box_source);
	return s;
}

#define NUM_FRAMEBUFFERS 2

#define OFFSET(member_) \
	((const GLvoid *) offsetof (std::remove_reference_t<decltype (fb_vbo_data[0])>, member_))

/* An RGBA8 texture of w x h, clamped, cleared to 0: the scene's buffers'
 * (NEAREST, for the blending of the next frame), the post effects'
 * (LINEAR). Cleared by glClear, not glTexSubImage2D: with no unpack buffer
 * bound, that one reads its data pointer, and NVIDIA's driver read address
 * 0 when it was NULL */
static GLuint
make_color_texture (unsigned w, unsigned h, GLint filter)
{
	GLuint id, fbo;

	glGenTextures (1, &id);
	glBindTexture (GL_TEXTURE_2D, id);
	glTexStorage2D (GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	HL_ASSERT_NO_GL_ERROR ();

	glGenFramebuffers (1, &fbo);
	glBindFramebuffer (GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, id, 0);
	glDisable (GL_SCISSOR_TEST);
	glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glDeleteFramebuffers (1, &fbo);
	HL_ASSERT_NO_GL_ERROR ();
	return id;
}

/* A post effect's picture, w x h, and its framebuffer object: made, or made
 * again at another size */
static void
make_post_target (hikaru_target_t &t, unsigned w, unsigned h)
{
	if (t.fb && t.w == w && t.h == h)
		return;
	if (!t.fb)
		glGenFramebuffers (1, &t.fb);
	if (t.tex)
		glDeleteTextures (1, &t.tex);
	t.tex = make_color_texture (w, h, GL_LINEAR);
	glBindFramebuffer (GL_FRAMEBUFFER, t.fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, t.tex, 0);
	HL_ASSERT_NO_GL_ERROR ();
	t.w = w;
	t.h = h;
}

/* The frames' blending (781/181): the picture drawn in fb[n1] and the one kept
 * in fb[n2], the kept one's share `rate` (0 to FF), as the board's blender
 * draws it (US patent 6,549,209, fig. 11): the rate counts one more from 0x80
 * up, so that FF keeps the kept picture whole, and each product loses its
 * fraction before the sum. A blending in floats rounded each pixel to the
 * nearest: a trail at FC never died out, stuck one step above the new
 * picture. As Odin's author, Réda Chérif-Touil, does it */
static const char *blend_vs_source =
"#version 140									\n"
"										\n"
"void										\n"
"main (void)									\n"
"{										\n"
"	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);		\n"
"	gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);				\n"
"}										\n";

static const char *blend_fs_source =
"#version 140									\n"
"										\n"
"uniform sampler2D u_drawn;							\n"
"uniform sampler2D u_kept;							\n"
"uniform int u_rate;								\n"
"out vec4 frag_color;								\n"
"										\n"
"void										\n"
"main (void)									\n"
"{										\n"
"	ivec2 at = ivec2 (gl_FragCoord.xy);					\n"
"	ivec4 drawn = ivec4 (texelFetch (u_drawn, at, 0) * 255.0 + 0.5);	\n"
"	ivec4 kept = ivec4 (texelFetch (u_kept, at, 0) * 255.0 + 0.5);		\n"
"	int k = u_rate + (u_rate >> 7);						\n"
"										\n"
"	frag_color = vec4 (((kept * k) >> 8) + ((drawn * (256 - k)) >> 8)) / 255.0;	\n"
"}										\n";

static void
blend_frames (Renderer *hr, unsigned n1, unsigned n2, unsigned rate)
{
	auto &fb = hr->framebuffer;

	if (!fb.blend.program) {
		fb.blend.program = hl::gl::compile_program (blend_vs_source, blend_fs_source);
		fb.blend.u_drawn = glGetUniformLocation (fb.blend.program, "u_drawn");
		fb.blend.u_kept = glGetUniformLocation (fb.blend.program, "u_kept");
		fb.blend.u_rate = glGetUniformLocation (fb.blend.program, "u_rate");
	}
	/* The picture drawn, copied: the blending writes over it */
	make_post_target (fb.drawn, fb.w, fb.h);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, fb.fb[n1]);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, fb.drawn.fb);
	glBlitFramebuffer (0, 0, fb.w, fb.h, 0, 0, fb.w, fb.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, fb.fb[n1]);
	glViewport (0, 0, fb.w, fb.h);

	glUseProgram (fb.blend.program);
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, fb.drawn.tex);
	glActiveTexture (GL_TEXTURE0 + 1);
	glBindTexture (GL_TEXTURE_2D, fb.cb[n2]);
	glActiveTexture (GL_TEXTURE0);
	glUniform1i (fb.blend.u_drawn, 0);
	glUniform1i (fb.blend.u_kept, 1);
	glUniform1i (fb.blend.u_rate, (GLint) (rate & 0xFF));
	glDisable (GL_BLEND);
	glBindVertexArray (fb.vao);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	HL_ASSERT_NO_GL_ERROR ();
}

static void
destroy_post_target (hikaru_target_t &t)
{
	if (t.fb)
		glDeleteFramebuffers (1, &t.fb);
	if (t.tex)
		glDeleteTextures (1, &t.tex);
	memset ((void *) &t, 0, sizeof (t));
}

/* The scene's two buffers at RENDER_SCALE's size, and MSAA's: built, or
 * built again when they change, their pictures kept (scaled), for the
 * blending of the next frame and for a frame that draws none */
static int
build_scene_buffers (Renderer *hr)
{
	static const GLenum targets[1] = { GL_COLOR_ATTACHMENT0 };
	auto &fb = hr->framebuffer;
	unsigned scale = MAX2 (1u, MIN2 ((unsigned) hl::MAX_RENDER_SCALE, hr->view.scale));
	unsigned samples = MIN2 (hr->view.msaa, (unsigned) MAX2 (0, fb.max_samples));
	unsigned w = hr->scene_w * scale, h = 480 * scale, i;
	bool resized = !fb.cb[0] || fb.w != w || fb.h != h;
	GLuint old_fb = 0;

	if (samples < 2)
		samples = 0;
	if (!resized && fb.samples == samples)
		return 0;

	if (resized && fb.cb[0])
		glGenFramebuffers (1, &old_fb);

	for (i = 0; i < NUM_FRAMEBUFFERS && resized; i++) {
		GLuint cb = make_color_texture (w, h, GL_NEAREST), db;

		glGenRenderbuffers (1, &db);
		glBindRenderbuffer (GL_RENDERBUFFER, db);
		/* 24 bits, as translucency's copy of it (a blit needs the same) */
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
		HL_ASSERT_NO_GL_ERROR ();

		glBindFramebuffer (GL_FRAMEBUFFER, fb.fb[i]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, cb, 0);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                           GL_RENDERBUFFER, db);
		glDrawBuffers (1, targets);
		HL_ASSERT_NO_GL_ERROR ();
		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return -1;

		/* The old picture, scaled to the new size */
		if (old_fb) {
			glBindFramebuffer (GL_READ_FRAMEBUFFER, old_fb);
			glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                        GL_TEXTURE_2D, fb.cb[i], 0);
			glBlitFramebuffer (0, 0, fb.w, fb.h, 0, 0, w, h,
			                   GL_COLOR_BUFFER_BIT, GL_LINEAR);
			glDeleteTextures (1, &fb.cb[i]);
			glDeleteRenderbuffers (1, &fb.db[i]);
			HL_ASSERT_NO_GL_ERROR ();
		}
		fb.cb[i] = cb;
		fb.db[i] = db;
	}
	if (old_fb)
		glDeleteFramebuffers (1, &old_fb);

	/* MSAA: the scene drawn there, resolved into the buffer */
	if (fb.ms_fb) {
		glDeleteFramebuffers (1, &fb.ms_fb);
		glDeleteRenderbuffers (1, &fb.ms_cb);
		glDeleteRenderbuffers (1, &fb.ms_db);
		fb.ms_fb = fb.ms_cb = fb.ms_db = 0;
	}
	if (samples) {
		glGenFramebuffers (1, &fb.ms_fb);
		glGenRenderbuffers (1, &fb.ms_cb);
		glGenRenderbuffers (1, &fb.ms_db);
		glBindRenderbuffer (GL_RENDERBUFFER, fb.ms_cb);
		glRenderbufferStorageMultisample (GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
		glBindRenderbuffer (GL_RENDERBUFFER, fb.ms_db);
		glRenderbufferStorageMultisample (GL_RENDERBUFFER, samples,
		                                  GL_DEPTH_COMPONENT24, w, h);
		glBindFramebuffer (GL_FRAMEBUFFER, fb.ms_fb);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                           GL_RENDERBUFFER, fb.ms_cb);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                           GL_RENDERBUFFER, fb.ms_db);
		glDrawBuffers (1, targets);
		HL_ASSERT_NO_GL_ERROR ();
		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			HL_ERROR ("MSAA x%u not available: drawn without", samples);
			glDeleteFramebuffers (1, &fb.ms_fb);
			glDeleteRenderbuffers (1, &fb.ms_cb);
			glDeleteRenderbuffers (1, &fb.ms_db);
			fb.ms_fb = fb.ms_cb = fb.ms_db = 0;
		}
	}
	glBindRenderbuffer (GL_RENDERBUFFER, 0);
	glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
	HL_ASSERT_NO_GL_ERROR ();

	if (fb.w)
		HL_PRINT ("HR the scene drawn at %ux%u, MSAA x%u", w, h, samples);
	fb.w = w;
	fb.h = h;
	fb.scale = scale;
	/* What was asked, within the card's: asked again when it changes */
	fb.samples = samples;
	return 0;
}

/* A post effect's program, with its uniforms */
template <typename P> static void
build_post_program (P &p, const char *fs_source)
{
	p.program = hl::gl::compile_program (fb_vs_source, with_box (fs_source).c_str ());
	p.u_texture = glGetUniformLocation (p.program, "u_texture");
	p.u_texel = glGetUniformLocation (p.program, "u_texel");
	p.u_down = glGetUniformLocation (p.program, "u_down");
	HL_ASSERT_NO_GL_ERROR ();
}

static int
build_fb_state (Renderer *hr)
{
	/* Compile the framebuffer GLSL program and retrieve the uniforms. */
	hr->framebuffer.program =
		hl::gl::compile_program (fb_vs_source, with_box (fb_fs_source).c_str ());
	HL_ASSERT_NO_GL_ERROR ();

	hr->framebuffer.locs.u_front =
		get_uniform_loc (hr->framebuffer.program, "u_front");
	hr->framebuffer.locs.u_back =
		get_uniform_loc (hr->framebuffer.program, "u_back");
	hr->framebuffer.locs.u_layer1 =
		get_uniform_loc (hr->framebuffer.program, "u_layer1");
	hr->framebuffer.locs.u_layer2 =
		get_uniform_loc (hr->framebuffer.program, "u_layer2");
	hr->framebuffer.locs.u_factor =
		get_uniform_loc (hr->framebuffer.program, "u_factor");
	hr->framebuffer.locs.u_mult_f =
		get_uniform_loc (hr->framebuffer.program, "u_mult_f");
	hr->framebuffer.locs.u_mult_b =
		get_uniform_loc (hr->framebuffer.program, "u_mult_b");
	hr->framebuffer.locs.u_mult_1 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_1");
	hr->framebuffer.locs.u_mult_2 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_2");
	hr->framebuffer.locs.u_order =
		get_uniform_loc (hr->framebuffer.program, "u_order");
	hr->framebuffer.locs.u_test =
		get_uniform_loc (hr->framebuffer.program, "u_test");
	hr->framebuffer.locs.u_invert_f =
		get_uniform_loc (hr->framebuffer.program, "u_invert_f");
	hr->framebuffer.locs.u_invert_b =
		get_uniform_loc (hr->framebuffer.program, "u_invert_b");
	hr->framebuffer.locs.u_layer_x =
		get_uniform_loc (hr->framebuffer.program, "u_layer_x");
	/* [Enhancements]': -1 if the compiler dropped one, ignored then */
	hr->framebuffer.locs.u_down =
		glGetUniformLocation (hr->framebuffer.program, "u_down");
	hr->framebuffer.locs.u_texel =
		glGetUniformLocation (hr->framebuffer.program, "u_texel");
	hr->framebuffer.locs.u_bloom =
		glGetUniformLocation (hr->framebuffer.program, "u_bloom");
	hr->framebuffer.locs.u_bloom_k =
		glGetUniformLocation (hr->framebuffer.program, "u_bloom_k");
	hr->framebuffer.locs.u_crt =
		glGetUniformLocation (hr->framebuffer.program, "u_crt");
	hr->framebuffer.locs.u_out =
		glGetUniformLocation (hr->framebuffer.program, "u_out");
	hr->framebuffer.locs.u_barrel =
		glGetUniformLocation (hr->framebuffer.program, "u_barrel");
	hr->framebuffer.locs.u_frame =
		glGetUniformLocation (hr->framebuffer.program, "u_frame");
	hr->framebuffer.locs.u_barrel1 =
		glGetUniformLocation (hr->framebuffer.program, "u_barrel1");
	hr->framebuffer.locs.u_barrel2 =
		glGetUniformLocation (hr->framebuffer.program, "u_barrel2");

	build_post_program (hr->framebuffer.fxaa, fxaa_fs_source);
	build_post_program (hr->framebuffer.bright, bright_fs_source);
	build_post_program (hr->framebuffer.blur, blur_fs_source);
	hr->presentation = new hl::Presentation ();

	/* What the card can: MSAA's samples, the anisotropy */
	glGetIntegerv (GL_MAX_SAMPLES, &hr->framebuffer.max_samples);
	hr->framebuffer.max_aniso = 1.0f;
	if (GLEW_EXT_texture_filter_anisotropic || GLEW_ARB_texture_filter_anisotropic)
		glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &hr->framebuffer.max_aniso);
	HL_ASSERT_NO_GL_ERROR ();

	/* Two framebuffers (the front and the back buffers), each with a
	 * colorbuffer (texture) and a depth buffer (renderbuffer), at
	 * RENDER_SCALE's size */
	glGenFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	HL_ASSERT_NO_GL_ERROR ();
	if (build_scene_buffers (hr))
		return -1;

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	HL_ASSERT_NO_GL_ERROR ();

	/* Create the VAO/VBO. */
	glGenVertexArrays (1, &hr->framebuffer.vao);
	glBindVertexArray (hr->framebuffer.vao);
	HL_ASSERT_NO_GL_ERROR ();

	glGenBuffers (1, &hr->framebuffer.vbo);
	glBindBuffer (GL_ARRAY_BUFFER, hr->framebuffer.vbo);
	glBufferData (GL_ARRAY_BUFFER,
	              sizeof (fb_vbo_data), fb_vbo_data, GL_STATIC_DRAW);
	HL_ASSERT_NO_GL_ERROR ();

	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (position));
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (texcoords));
	HL_ASSERT_NO_GL_ERROR ();

	glEnableVertexAttribArray (0);
	glEnableVertexAttribArray (1);
	HL_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (0);
	HL_ASSERT_NO_GL_ERROR ();

	return 0;
}

#undef OFFSET

static void
destroy_fb_state (Renderer *hr)
{
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glDeleteTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glDeleteRenderbuffers (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	glDeleteTextures (2, hr->framebuffer.layers);
	memset (hr->framebuffer.layers, 0, sizeof (hr->framebuffer.layers));
	memset (hr->framebuffer.layer_formats, 0, sizeof (hr->framebuffer.layer_formats));
	if (hr->framebuffer.ms_fb) {
		glDeleteFramebuffers (1, &hr->framebuffer.ms_fb);
		glDeleteRenderbuffers (1, &hr->framebuffer.ms_cb);
		glDeleteRenderbuffers (1, &hr->framebuffer.ms_db);
	}
	destroy_post_target (hr->framebuffer.post);
	destroy_post_target (hr->framebuffer.bloom[0]);
	destroy_post_target (hr->framebuffer.bloom[1]);
	destroy_post_target (hr->framebuffer.drawn);
	hl::gl::destroy_program (hr->framebuffer.blend.program);
	hr->framebuffer.blend.program = 0;
	hl::gl::destroy_program (hr->framebuffer.fxaa.program);
	hl::gl::destroy_program (hr->framebuffer.bright.program);
	hl::gl::destroy_program (hr->framebuffer.blur.program);
	delete hr->presentation;
	hr->presentation = NULL;
	HL_ASSERT_NO_GL_ERROR ();

	hl::gl::destroy_program (hr->framebuffer.program);
}

/* A layer's 640x480 pixels to its texture, bound: its storage made for the
 * format (the GPU's, 1 + format, 0 none), else written into */
static void
layer_pixels (Renderer *hr, unsigned i, unsigned format, GLint internal, GLenum type,
              const void *data)
{
	if (hr->framebuffer.layer_formats[i] != format + 1) {
		glTexImage2D (GL_TEXTURE_2D, 0, internal, 640, 480, 0, GL_RGBA, type, data);
		hr->framebuffer.layer_formats[i] = format + 1;
	} else
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 640, 480, GL_RGBA, type, data);
}

/* Each run of a layer's rows changed, from y to y + h */
template <typename F> static void
each_rows_run (const bool *rows, F &&f)
{
	unsigned y = 0, end;

	while (y < 480) {
		if (!rows[y]) {
			y++;
			continue;
		}
		for (end = y; end < 480 && rows[end]; end++)
			;
		f (y, end - y);
		y = end;
	}
}

static void
upload_layer (Renderer *hr, hikaru_layer_t *layer, unsigned i, GLuint *id, float *mult)
{
	void *data;

	LOG ("uploading LAYER %s", get_layer_str (layer));

	/* Its 480 rows of 640 pixels within the framebuffer (2048 rows of
	 * 4096 bytes): GL reads them through a pointer, as for the textures */
	if ((uint64_t) (layer->y0 + 479) * 4096 + (layer->x0 + 640) * 4 >
	    hr->fb_src->size ()) {
		static unsigned logged;
		if (logged++ < 20)
			HL_ERROR ("layer past the framebuffer, not drawn: %s",
			          get_layer_str (layer));
		*id = 0;
		return;
	}

	data = hr->fb_src->ptr (layer->y0 * 4096 + layer->x0 * 4);

	/* To its texture, made once (a texture made and deleted each frame,
	 * two a frame, is the drivers' least tried path, the more so with
	 * THREADED_RENDERING); its storage made again only when the format
	 * changes, its pixels written into it the other frames (a new storage
	 * each frame ran an Intel UHD out of memory, now and then) */
	if (!hr->framebuffer.layers[i]) {
		glGenTextures (1, &hr->framebuffer.layers[i]);
		hr->framebuffer.layer_formats[i] = 0;
	}
	*id = hr->framebuffer.layers[i];
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, *id);
	HL_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	HL_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	HL_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	HL_ASSERT_NO_GL_ERROR ();

	/* All its rows (a new storage, the layer moved, no drawing thread), or
	 * those the drawing thread's copy changed alone */
	bool all = !hr->layers_tracked || hr->layer_all[i] ||
	           hr->framebuffer.layer_formats[i] != layer->format + 1;

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	switch (layer->format) {
	case HIKARU_FORMAT_ABGR1555: {
		/* Two pixels a 32-bit word, the left one in its high half (see
		 * hikaru-gpu.cpp, "Framebuffer Configuration"): each pair the
		 * other way round for GL */
		static uint16_t pixels[640 * 480];
		const uint16_t *src = (const uint16_t *) data;
		auto convert = [src] (unsigned y0, unsigned h) {
			unsigned x, y;

			for (y = y0; y < y0 + h; y++)
				for (x = 0; x < 640; x++)
					pixels[y * 640 + x] = src[y * 2048 + (x ^ 1)];
		};

		glPixelStorei (GL_UNPACK_ROW_LENGTH, 640);
		if (all) {
			convert (0, 480);
			layer_pixels (hr, i, layer->format, GL_RGB5_A1,
			              GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
		} else
			each_rows_run (hr->layer_rows[i], [&] (unsigned y, unsigned h) {
				convert (y, h);
				glTexSubImage2D (GL_TEXTURE_2D, 0, 0, (GLint) y, 640, (GLsizei) h,
				                 GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels + y * 640);
			});
		*mult = 1.0f;
		break;
	}
	case HIKARU_FORMAT_A2BGR10:
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 1024);
		if (all)
			layer_pixels (hr, i, layer->format, GL_RGB10_A2,
			              GL_UNSIGNED_INT_2_10_10_10_REV, data);
		else
			each_rows_run (hr->layer_rows[i], [&] (unsigned y, unsigned h) {
				glTexSubImage2D (GL_TEXTURE_2D, 0, 0, (GLint) y, 640, (GLsizei) h,
				                 GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
				                 (const uint8_t *) data + y * 4096);
			});
		*mult = 4.0f;
		break;
	default:
		HL_ASSERT (0);
	}
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	HL_ASSERT_NO_GL_ERROR ();
	if (hr->layers_tracked) {
		memset (hr->layer_rows[i], 0, sizeof (hr->layer_rows[i]));
		hr->layer_all[i] = false;
	}
}

/* TODO make use of the framebuffer configuration in the GPU:1A registers,
 * right now it only uses the information in the 781/181 commands. */
/* The picture's place, the bars around it cleared: in the window, or in
 * the drawing thread's texture at the window's size */
static void
set_output_viewport (Renderer *hr)
{
	if (!hr->target_fb) {
		hl::Window::drawable_size (&hr->target_width, &hr->target_height);
		hl::Window::layout (&hr->target_layout);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
	hl::Window::set_output_viewport (hr->target_layout, hr->target_width, hr->target_height);
}

/* The texture on a unit, if any, sampled so */
static void
set_filter (unsigned unit, GLint filter)
{
	GLint id;

	glActiveTexture (GL_TEXTURE0 + unit);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &id);
	if (!id)
		return;
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

/* The settings draw () follows, at the frame's end */
static void
copy_view (Renderer *hr)
{
	hl::Config *cfg = hl::Config::get ();

	hr->view.stretch_2d = cfg->widescreen_2d == hl::WIDESCREEN_2D_STRETCHED;
	hr->view.linear = cfg->scale_filter == hl::FILTER_LINEAR;
	hr->view.distance = MAX2 (1.0f, cfg->draw_distance);
	hr->view.scale = cfg->render_scale;
	hr->view.msaa = cfg->msaa;
	hr->view.anisotropy = cfg->anisotropy;
	hr->view.texture_scale = cfg->texture_scale;
	hr->view.fxaa = cfg->fxaa;
	hr->view.sort_pixels = cfg->sort_translucent_pixels;
	hr->view.bloom = cfg->bloom * 0.01f;
	hr->view.crt = cfg->crt_filter * 0.01f;
	if (hr->presentation)
		hr->presentation->copy_view ();
}

/* A post effect's pass: its program on src, into dst; LINEAR on src while
 * it reads it */
template <typename P> static void
draw_post_pass (Renderer *hr, P &p, GLuint src, unsigned sw, unsigned sh,
                hikaru_target_t &dst, float dx, float dy)
{
	glBindFramebuffer (GL_FRAMEBUFFER, dst.fb);
	glViewport (0, 0, dst.w, dst.h);
	glUseProgram (p.program);
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, src);
	set_filter (0, GL_LINEAR);
	glUniform1i (p.u_texture, 0);
	if (dx || dy)	/* the blur's: along its direction */
		glUniform2f (p.u_texel, dx, dy);
	else
		glUniform2f (p.u_texel, 1.0f / sw, 1.0f / sh);
	if (p.u_down != (GLuint) -1)
		glUniform2f (p.u_down, (float) sw / dst.w, (float) sh / dst.h);
	glBindVertexArray (hr->framebuffer.vao);
	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray (0);
	HL_ASSERT_NO_GL_ERROR ();
}

/* FXAA, BLOOM: the scene's picture smoothed, its glow drawn apart. Returns
 * the picture to show, the glow's texture in *bloom (0: none) */
static GLuint
draw_post_effects (Renderer *hr, GLuint scene, GLuint *bloom)
{
	auto &fb = hr->framebuffer;
	GLuint picture = scene;

	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);

	if (hr->view.fxaa) {
		make_post_target (fb.post, fb.w, fb.h);
		draw_post_pass (hr, fb.fxaa, scene, fb.w, fb.h, fb.post, 0.0f, 0.0f);
		picture = fb.post.tex;
	}

	/* The bright parts at a quarter of the scene's size, whatever
	 * RENDER_SCALE's: the same glow at each; blurred twice across, twice
	 * down */
	if (hr->view.bloom > 0.0f) {
		unsigned i;

		make_post_target (fb.bloom[0], hr->scene_w / 4, 480 / 4);
		make_post_target (fb.bloom[1], hr->scene_w / 4, 480 / 4);
		draw_post_pass (hr, fb.bright, picture, fb.w, fb.h, fb.bloom[0], 0.0f, 0.0f);
		for (i = 0; i < 2; i++) {
			draw_post_pass (hr, fb.blur, fb.bloom[0].tex, fb.bloom[0].w, fb.bloom[0].h,
			                fb.bloom[1], 1.0f / fb.bloom[0].w, 0.0f);
			draw_post_pass (hr, fb.blur, fb.bloom[1].tex, fb.bloom[1].w, fb.bloom[1].h,
			                fb.bloom[0], 0.0f, 1.0f / fb.bloom[1].h);
		}
		*bloom = fb.bloom[0].tex;
	}

	/* The scene's buffer back to NEAREST, for the blending of the next
	 * frame */
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, scene);
	set_filter (0, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
	HL_ASSERT_NO_GL_ERROR ();
	return picture;
}

/* What draw () reads of the GPU's, at the frame's end */
static void
copy_frame_gpu (Renderer *hr, Gpu *gpu)
{
	hr->frame_gpu.fb_config = gpu->fb_config;
	hr->frame_gpu.layers = LAYERS;
	hr->frame_gpu.layer_order = REG1AUNIT (0, 0x38) & 3;
	hr->frame_gpu.layer_test = REG1AUNIT (0, 0x3C) & 7;
	hr->frame_gpu.lists_begun = gpu->lists_begun;
}

static void
draw (Renderer *hr)
{
	auto *gpu = &hr->frame_gpu;
	GLuint n1, n2, f, b, layer1 = 0, layer2 = 0, bloom = 0, crt_fb;
	float factor, mult1 = 0.0f, mult2 = 0.0f;
	bool on1, on2, drawn = false, minified;
	GLint out[4], filter, scene_filter;

	/* RENDER_SCALE, MSAA: the scene's buffers at their size */
	if (build_scene_buffers (hr))
		HL_ABORT ("could not build the scene's buffers");

	/* Figure out the GPU framebuffer assignments */
	LOG ("FRAMEBUFFER 781=%08X 181=%08X", gpu->fb_config._781, gpu->fb_config._181);

	n1 = gpu->fb_config.num_1;
	n2 = gpu->fb_config.num_2;

	factor = 0.0f;
	if (gpu->fb_config.blend)
		factor = gpu->fb_config.alpha * (1.0f / 255);


	/* We only care about unit 0 for now. (Unit 1 is probably only used
	 * for the dual-monitor case.) */
	if ((gpu->layers.layer[0][0].enabled || n1 == 2 || n2 == 2) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER1])
		upload_layer (hr, &gpu->layers.layer[0][0], 0, &layer1, &mult1);

	if ((gpu->layers.layer[0][1].enabled || n1 == 3 || n2 == 3) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER2])
		upload_layer (hr, &gpu->layers.layer[0][1], 1, &layer2, &mult2);

	/* After the uploads: a layer may be the front or the back buffer. */
	f = (n1 < 2) ? hr->framebuffer.cb[n1] : (n1 == 2) ? layer1 : layer2;
	b = (n2 < 2) ? hr->framebuffer.cb[n2] : (n2 == 2) ? layer1 : layer2;


	/* Draw the 3D scene to the front buffer. f is its color texture: bind
	 * the framebuffer object holding it, whose name differs as soon as
	 * other textures exist (the interface's). A layer has no framebuffer
	 * object: the scene then goes to the window, cleared afterwards. */
	/* Only when the CP began a list this frame: a game drawing one frame
	 * in two (PHARRIER's demo, at 30 frames a second) shows its last
	 * picture in between, not a cleared buffer (a blue flicker) */
	if (gpu->lists_begun != hr->framebuffer.lists_drawn || n1 >= 2) {
		/* MSAA: the scene drawn there, resolved into the front buffer */
		bool ms = hr->framebuffer.ms_fb && n1 < 2;

		hr->framebuffer.lists_drawn = gpu->lists_begun;

		glBindFramebuffer (GL_FRAMEBUFFER,
		                   ms ? hr->framebuffer.ms_fb :
		                   (n1 < 2) ? hr->framebuffer.fb[n1] : hr->target_fb);
		HL_ASSERT_NO_GL_ERROR ();

		/* The windows' contexts have it off (hl::Window): on, or each
		 * pixel's samples would all take the same value */
		if (ms)
			glEnable (GL_MULTISAMPLE);
		draw_scene (hr);
		HL_ASSERT_NO_GL_ERROR ();
		drawn = true;

		if (ms) {
			glDisable (GL_MULTISAMPLE);
			glBindFramebuffer (GL_READ_FRAMEBUFFER, hr->framebuffer.ms_fb);
			glBindFramebuffer (GL_DRAW_FRAMEBUFFER, hr->framebuffer.fb[n1]);
			glBlitFramebuffer (0, 0, hr->framebuffer.w, hr->framebuffer.h,
			                   0, 0, hr->framebuffer.w, hr->framebuffer.h,
			                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
			HL_ASSERT_NO_GL_ERROR ();
		}

		glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
		HL_ASSERT_NO_GL_ERROR ();
	}


	/* Setup the GL state for 2D drawing. */
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_POLYGON_OFFSET_FILL);
	HL_ASSERT_NO_GL_ERROR ();


	/* Composite all the buffers (front, back, layers). */
	glUseProgram (hr->framebuffer.program);
	HL_ASSERT_NO_GL_ERROR ();

	/* Blending the front and the back buffers, which swap every frame:
	 * the picture stays in the front buffer, the next frame's back, so
	 * that the old pictures fade out over several frames: the motion blur
	 * of PHARRIER's intro, of BRAVEFF's party scene; FF holds the picture.
	 * The factor is the old picture's share. Only the frame that drew
	 * the scene blends; the picture is then the front buffer alone. */
	if (factor > 0.0f && n1 < 2 && n2 < 2 && n1 != n2) {
		if (drawn) {
			glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[n1]);
			glViewport (0, 0, hr->framebuffer.w, hr->framebuffer.h);
			HL_ASSERT_NO_GL_ERROR ();

			glActiveTexture (GL_TEXTURE0 + 0);
			glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[n2]);
			glActiveTexture (GL_TEXTURE0 + 1);
			glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[n2]);
			glUniform1i (hr->framebuffer.locs.u_front,  0);
			glUniform1i (hr->framebuffer.locs.u_back,   1);
			glUniform1f (hr->framebuffer.locs.u_factor, 0.0f);
			glUniform1f (hr->framebuffer.locs.u_mult_f, 1.0f);
			glUniform1f (hr->framebuffer.locs.u_mult_b, 1.0f);
			glUniform1f (hr->framebuffer.locs.u_mult_1, 0.0f);
			glUniform1f (hr->framebuffer.locs.u_mult_2, 0.0f);
			glUniform1i (hr->framebuffer.locs.u_invert_f, 0);
			glUniform1i (hr->framebuffer.locs.u_invert_b, 0);
			glUniform2f (hr->framebuffer.locs.u_down, 1.0f, 1.0f);
			glUniform1f (hr->framebuffer.locs.u_bloom_k, 0.0f);
			glUniform1f (hr->framebuffer.locs.u_crt, 0.0f);
			glUniform1i (hr->framebuffer.locs.u_barrel, -1);
			HL_ASSERT_NO_GL_ERROR ();

			/* front = back * factor + front * (1 - factor), as the
			 * board's blender does it (blend_frames) */
			blend_frames (hr, n1, n2, gpu->fb_config.alpha);
			glUseProgram (hr->framebuffer.program);
			HL_ASSERT_NO_GL_ERROR ();

			glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
			HL_ASSERT_NO_GL_ERROR ();
		}
		factor = 0.0f;
	}

	/* FXAA, BLOOM: on the scene alone, at its buffers' size */
	if (n1 < 2 && (hr->view.fxaa || hr->view.bloom > 0.0f)) {
		f = draw_post_effects (hr, f, &bloom);
		glUseProgram (hr->framebuffer.program);
		HL_ASSERT_NO_GL_ERROR ();
	}

	/* CRT_PRESET: the picture composed at its own size, for the preset's
	 * passes (hl/presentation.h) */
	crt_fb = hr->presentation->begin (hr->scene_w, 480);

	/* Where the picture goes in the window, after the aspect ratio; out,
	 * its whole size there (larger than the window where cropped) */
	set_output_viewport (hr);
	memcpy (out, hr->target_layout.image, sizeof (out));
	if (crt_fb) {
		glDisable (GL_SCISSOR_TEST);
		glBindFramebuffer (GL_FRAMEBUFFER, crt_fb);
		glViewport (0, 0, hr->scene_w, 480);
		out[0] = out[1] = 0;
		out[2] = hr->scene_w;
		out[3] = 480;
	}

	/* Figure out the GPU framebuffer assignments. */
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, f);
	HL_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 1);
	glBindTexture (GL_TEXTURE_2D, b);
	HL_ASSERT_NO_GL_ERROR ();

	/* A layer is drawn over the buffers unless it is one of them. */
	on1 = layer1 && f != layer1 && b != layer1;
	on2 = layer2 && f != layer2 && b != layer2;

	glActiveTexture (GL_TEXTURE0 + 2);
	glBindTexture (GL_TEXTURE_2D, on1 ? layer1 : 0);
	HL_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 3);
	glBindTexture (GL_TEXTURE_2D, on2 ? layer2 : 0);
	HL_ASSERT_NO_GL_ERROR ();

	glUniform1i (hr->framebuffer.locs.u_front,  0);
	glUniform1i (hr->framebuffer.locs.u_back,   1);
	glUniform1i (hr->framebuffer.locs.u_layer1, 2);
	glUniform1i (hr->framebuffer.locs.u_layer2, 3);
	glUniform1f (hr->framebuffer.locs.u_factor, factor);
	/* All of them, every frame: a program keeps its uniforms, and one
	 * frame without a layer would leave its multiplier at 0. */
	glUniform1f (hr->framebuffer.locs.u_mult_f,
	             (n1 == 2) ? mult1 : (n1 == 3) ? mult2 : 1.0f);
	glUniform1f (hr->framebuffer.locs.u_mult_b,
	             (n2 == 2) ? mult1 : (n2 == 3) ? mult2 : 1.0f);
	glUniform1f (hr->framebuffer.locs.u_mult_1, on1 ? mult1 : 0.0f);
	glUniform1f (hr->framebuffer.locs.u_mult_2, on2 ? mult2 : 0.0f);
	glUniform1i (hr->framebuffer.locs.u_order, gpu->layer_order);
	glUniform1i (hr->framebuffer.locs.u_test, gpu->layer_test);
	glUniform1i (hr->framebuffer.locs.u_invert_f, n1 >= 2);
	glUniform1i (hr->framebuffer.locs.u_invert_b, n2 >= 2);
	if (hr->scene_w == 640 || hr->view.stretch_2d)
		glUniform2f (hr->framebuffer.locs.u_layer_x, 0.0f, 1.0f);
	else
		glUniform2f (hr->framebuffer.locs.u_layer_x,
		             (hr->scene_w - 640) * 0.5f / hr->scene_w, 640.0f / hr->scene_w);
	HL_ASSERT_NO_GL_ERROR ();

	/* RENDER_SCALE: the scene's buffers larger than the picture, the
	 * mean of their texels under each pixel. BLOOM's glow; CRT_FILTER at
	 * the picture's size */
	minified = hr->framebuffer.scale > 1 &&
	           (hr->framebuffer.w > (unsigned) out[2] || hr->framebuffer.h > (unsigned) out[3]);
	if (minified)
		glUniform2f (hr->framebuffer.locs.u_down,
		             (float) hr->framebuffer.w / MAX2 (1, out[2]),
		             (float) hr->framebuffer.h / MAX2 (1, out[3]));
	else
		glUniform2f (hr->framebuffer.locs.u_down, 1.0f, 1.0f);
	glUniform2f (hr->framebuffer.locs.u_texel,
	             1.0f / hr->framebuffer.w, 1.0f / hr->framebuffer.h);
	if (bloom) {
		glActiveTexture (GL_TEXTURE0 + 4);
		glBindTexture (GL_TEXTURE_2D, bloom);
		glUniform1i (hr->framebuffer.locs.u_bloom, 4);
	}
	glUniform1f (hr->framebuffer.locs.u_bloom_k, bloom ? hr->view.bloom : 0.0f);
	/* A CRT preset draws its own tube in CRT_FILTER's place, and its
	 * passes cannot bend the picture's edges */
	glUniform1f (hr->framebuffer.locs.u_crt, crt_fb ? 0.0f : hr->view.crt);
	glUniform2f (hr->framebuffer.locs.u_out, (float) out[2], (float) out[3]);
	{
		const hl::ScreenLayout &l = hr->target_layout;
		glUniform1i (hr->framebuffer.locs.u_barrel, crt_fb ? -1 : l.barrel_axis);
		glUniform4f (hr->framebuffer.locs.u_frame, (float) l.frame[0], (float) l.frame[1],
		             (float) l.frame[2], (float) l.frame[3]);
		glUniform4f (hr->framebuffer.locs.u_barrel1, l.edge, l.center, l.start, l.scale);
		glUniform4f (hr->framebuffer.locs.u_barrel2, l.length[0], l.length[1],
		             l.angle[0], l.angle[1]);
	}
	HL_ASSERT_NO_GL_ERROR ();

	/* The scaling filter, on the four; LINEAR on the scene's buffers
	 * when larger, for their mean. They go back to NEAREST after, for the
	 * blending of the next frame */
	filter = hr->view.linear ? GL_LINEAR : GL_NEAREST;
	scene_filter = minified ? GL_LINEAR : filter;
	set_filter (0, (n1 < 2) ? scene_filter : filter);
	set_filter (1, (n2 < 2) ? scene_filter : filter);
	set_filter (2, filter);
	set_filter (3, filter);

	glBindVertexArray (hr->framebuffer.vao);
	HL_ASSERT_NO_GL_ERROR ();

	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	HL_ASSERT_NO_GL_ERROR ();
	/* The crop's cut (set_output_viewport) */
	glDisable (GL_SCISSOR_TEST);

	/* Not left bound: drawn into again at the next frame */
	if (bloom) {
		glActiveTexture (GL_TEXTURE0 + 4);
		glBindTexture (GL_TEXTURE_2D, 0);
		glActiveTexture (GL_TEXTURE0 + 3);
	}
	if (hr->view.linear || minified) {
		set_filter (0, GL_NEAREST);
		set_filter (1, GL_NEAREST);
	}

	glBindVertexArray (0);
	glUseProgram (0);
	HL_ASSERT_NO_GL_ERROR ();

	/* CRT_PRESET's passes into the window's picture, then BEZEL */
	hr->presentation->end (hr->target_fb, hr->target_width, hr->target_height, hr->target_layout);
	HL_ASSERT_NO_GL_ERROR ();
}

/* A sync of the other context's: its work done on the GPU, then deleted */
static void
client_wait (GLsync sync)
{
	while (glClientWaitSync (sync, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000) == GL_TIMEOUT_EXPIRED)
		;
	glDeleteSync (sync);
}

/* HR_STATS's line, the frame's counts cleared */
static void
print_stats (Renderer *hr)
{
	if (hr->debug.stats)
		HL_PRINT ("HR STATS meshes=%u draws=%llu tris=%llu max_tris=%u (mesh %u) max_instances=%u uploads=%u gpu=%.1f ms slowest=%.1f ms (mesh %u)",
		          hr->total_meshes, (unsigned long long) hr->debug.draws,
		          (unsigned long long) hr->debug.tris, hr->debug.max_tris,
		          hr->debug.max_tris_mesh, hr->debug.max_instances,
		          hr->debug.uploads,
		          hr->debug.gpu_ms, hr->debug.slow_ms, hr->debug.slow_mesh);
	hr->debug.draws = hr->debug.tris = 0;
	hr->debug.max_tris = hr->debug.max_instances = hr->debug.max_tris_mesh = 0;
	hr->debug.slow_mesh = hr->debug.uploads = 0;
	hr->debug.gpu_ms = hr->debug.slow_ms = 0.0;
}

/****************************************************************************
 The drawing thread
****************************************************************************/

/* THREADED_RENDERING: the frames drawn by a thread of their own, in a GL
 * context sharing the window's, while the machine runs the next one. At a
 * frame's end, the renderer the GPU feeds waits for the thread to be done
 * with the frame before, hands its lists over to the thread's renderer (they
 * swap them) with copies of what draw () reads of the machine: the GPU's
 * framebuffer settings and layers, the layers' rows of the framebuffer, the
 * TEXRAM's slots written since (the IDMA's, the MEMCTL's). The thread draws
 * the frame while the window shows the one before: the pictures go to two
 * textures in turn, and come a frame later than without the thread. The
 * screenshots, the agent's "shot" and the pause take the last frame
 * (flush ()). */
struct Renderer::Thread {
	std::thread		 thread;
	std::mutex		 lock;
	std::condition_variable	 wake, idle;
	/* busy: not waiting for a job (starting, drawing) */
	bool			 busy = true, job = false, quit = false;
	/* An HL_ABORT there, thrown again by the machine's frame */
	std::string		 error;
	SDL_GLContext		 context = NULL;
	/* Its renderer: its GL objects are the thread's context's */
	Renderer		*drawer = NULL;
	/* HR_PROFILE: its time drawing the frames (and showing them) */
	double			 drawing_ms = 0.0;
	uint64_t		 drawn = 0;

	struct {
		/* The texture, the thread's framebuffer object on it, the
		 * window's own (each context has its own) */
		GLuint		 texture = 0, fb = 0, read_fb = 0;
		int		 width = 0, height = 0;
		/* The texture made again at another size: the window's
		 * framebuffer object follows */
		unsigned	 gen = 0, read_gen = 0;
		/* The thread done with it; the window done showing it */
		GLsync		 drawn = 0, shown = 0;
	} out[2];
	/* The texture of the next frame, of the last one */
	unsigned		 next = 0, last = 0;
	/* Its context current with the window (it can show its frames); this
	 * job's frame shown by it */
	bool			 on_window = false, present = false;
	/* The frames handed over; the last one shown */
	uint64_t		 jobs = 0, shown_job = 0;
};

bool
Renderer::start_thread (hl::Buffer *fb, hl::Buffer *texram[2])
{
	Thread *t;
	unsigned bank;

	if (!GLEW_ARB_sync) {
		HL_ERROR ("THREADED_RENDERING: no GL_ARB_sync here, the frames drawn without a thread");
		return false;
	}
	t = new Thread ();
	t->context = hl::Window::create_shared_context ();
	if (!t->context) {
		delete t;
		return false;
	}
	for (bank = 0; bank < 2; bank++) {
		texram_shadow[bank] = hl::Buffer::create (texram[bank]->size (), 0, hl::Buffer::LE32);
		memcpy (texram_shadow[bank]->ptr (0), texram[bank]->ptr (0), texram[bank]->size ());
	}
	fb_shadow = hl::Buffer::create (fb->size (), 0, hl::Buffer::LE32);

	thread = t;
	t->thread = std::thread (thread_main, this);
	wait_thread (false);
	if (!t->drawer) {
		HL_ERROR ("THREADED_RENDERING: the drawing thread did not start (%s), the frames drawn without it",
		          t->error.c_str ());
		stop_thread ();
		return false;
	}
	hl::Window::set_flush (flush, this);
	HL_PRINT ("renderer: the frames drawn by a thread of their own (THREADED_RENDERING)");
	return true;
}

void
Renderer::stop_thread ()
{
	Thread *t = thread;
	unsigned i;

	if (!t)
		return;
	hl::Window::set_flush (NULL, NULL);
	if (t->thread.joinable ()) {
		{
			std::lock_guard<std::mutex> lock (t->lock);
			t->quit = true;
		}
		t->wake.notify_one ();
		t->thread.join ();
	}
	for (i = 0; i < 2; i++)
		if (t->out[i].read_fb)
			glDeleteFramebuffers (1, &t->out[i].read_fb);
	hl::Window::delete_context (t->context);
	delete t;
	thread = NULL;
	hl::destroy (texram_shadow[0]);
	hl::destroy (texram_shadow[1]);
	hl::destroy (fb_shadow);
}

void
Renderer::thread_main (Renderer *owner)
{
	Thread *t = owner->thread;
	unsigned i;

	HL_PRINT ("renderer: the drawing thread is thread %lu", hl::Log::thread_id ());
	hl::Profile::sample_this_thread ();
	t->on_window = hl::Window::make_current_on_window (t->context);
	if (t->on_window || hl::Window::make_current (t->context)) {
		try {
			t->drawer = new Renderer (owner);
			/* The game's programs now: the machine waits for this
			 * thread to start (start_thread ()), none compiled in its
			 * frames */
			warm_up_programs ();
		} catch (const hl::Abort &e) {
			t->error = e.what ();
		}
	} else
		t->error = "no GL context";

	for (;;) {
		{
			std::unique_lock<std::mutex> lock (t->lock);
			t->busy = false;
			t->idle.notify_all ();
			t->wake.wait (lock, [t] { return t->job || t->quit; });
			if (t->quit)
				break;
			t->job = false;
		}
		try {
			auto t0 = std::chrono::steady_clock::now ();

			if (t->drawer)
				owner->draw_job ();
			if (hl::Profile::enabled) {
				t->drawing_ms += std::chrono::duration<double, std::milli> (
					std::chrono::steady_clock::now () - t0).count ();
				t->drawn++;
			}
		} catch (const hl::Abort &e) {
			t->error = e.what ();
		}
	}
	if (hl::Profile::enabled && t->drawn)
		HL_PRINT ("renderer: the drawing thread: %llu frames, %.3f ms each",
		          (unsigned long long) t->drawn, t->drawing_ms / (double) t->drawn);

	/* Its GL objects, in its context */
	delete t->drawer;
	t->drawer = NULL;
	for (i = 0; i < 2; i++) {
		auto &o = t->out[i];
		if (o.fb)
			glDeleteFramebuffers (1, &o.fb);
		if (o.texture)
			glDeleteTextures (1, &o.texture);
		if (o.drawn)
			glDeleteSync (o.drawn);
		if (o.shown)
			glDeleteSync (o.shown);
	}
	glFinish ();
	hl::Window::make_current (NULL);
}

/* The thread waiting for a job; its error, thrown again */
void
Renderer::wait_thread (bool rethrow)
{
	Thread *t = thread;
	std::unique_lock<std::mutex> lock (t->lock);

	t->idle.wait (lock, [t] { return !t->busy; });
	if (rethrow && !t->error.empty ()) {
		std::string error = t->error;
		t->error.clear ();
		throw hl::Abort (error);
	}
}

void
Renderer::fb_wrote (uint32_t offs, uint32_t size)
{
	uint32_t p;

	for (p = offs >> 12; p <= (offs + size - 1) >> 12 && p < 2048; p++)
		fb_written[p] = 1;
}

/* The frame to the thread's renderer, the thread waiting */
void
Renderer::hand_over ()
{
	Thread *t = thread;
	Renderer *d = t->drawer;
	unsigned vpi, i, bank, x, y, row;

	/* The lists: the drawer's, drawn, are the next frame's to fill */
	std::swap (vp_list, d->vp_list);
	std::swap (mv_list, d->mv_list);
	std::swap (mat_list, d->mat_list);
	std::swap (tex_list, d->tex_list);
	std::swap (ls_list, d->ls_list);
	std::swap (ls_tables, d->ls_tables);
	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++) {
			hikaru_mesh_t *drawn = mesh_list[vpi][i];

			std::swap (mesh_list[vpi][i], d->mesh_list[vpi][i]);
			d->num_meshes[vpi][i] = num_meshes[vpi][i];
			/* A mesh still open: to the lists filled from now on,
			 * where the CP ends it, as without the thread */
			if (meshes.current >= drawn && meshes.current < drawn + MAX_MESHES) {
				size_t k = meshes.current - drawn;
				mesh_list[vpi][i][k] = *meshes.current;
				meshes.current = &mesh_list[vpi][i][k];
			}
		}
	d->num_vps = num_vps;
	d->num_mvs = num_mvs;
	d->num_instances = num_instances;
	d->num_mats = num_mats;
	d->num_texs = num_texs;
	d->num_lss = num_lss;
	d->total_meshes = total_meshes;
	d->meshes.verts.swap (meshes.verts);
	memcpy (d->textures.slot_writes, textures.slot_writes, sizeof (textures.slot_writes));
	memcpy (d->debug.flags, debug.flags, sizeof (debug.flags));
	copy_frame_gpu (d, gpu);
	copy_view (d);

	/* The TEXRAM's slots written, 16 rows of 32 bytes each */
	for (bank = 0; bank < 2; bank++)
		for (y = 0; y < 0x40; y++)
			for (x = 0; x < 0x80; x++) {
				if (!texram_dirty[bank][y][x])
					continue;
				texram_dirty[bank][y][x] = false;
				for (row = 0; row < 16; row++) {
					uint32_t offs = (y * 16 + row) * 4096 + x * 32;
					memcpy (texram_shadow[bank]->ptr (offs),
					        texram_src[bank]->ptr (offs), 32);
				}
			}

	/* The layers draw () uploads: their 480 rows of 640 pixels (four
	 * bytes at most), copied when they changed (the most often few or
	 * none: the copy of both, 2.4 MB, took a twentieth of the machine's
	 * thread, and the comparison of all as much): those on a page written
	 * since (fb_written), all of a layer that moved or came back; the
	 * drawer told which (its textures written for those alone). Two
	 * layers over the same rows: both whole */
	bool copied[2] = { false, false };

	for (i = 0; i < 2; i++) {
		hikaru_layer_t *layer = &d->frame_gpu.layers.layer[0][i];
		unsigned num = 2 + i;
		uint32_t at;

		if (!(layer->enabled || d->frame_gpu.fb_config.num_1 == num ||
		      d->frame_gpu.fb_config.num_2 == num) ||
		    d->debug.flags[i ? HR_DEBUG_NO_LAYER2 : HR_DEBUG_NO_LAYER1] ||
		    (uint64_t) (layer->y0 + 479) * 4096 + (layer->x0 + 640) * 4 > fb_src->size ()) {
			d->layer_at[i] = ~0u;
			continue;
		}
		at = layer->y0 * 4096 + layer->x0 * 4;
		if (at != d->layer_at[i]) {
			d->layer_at[i] = at;
			d->layer_all[i] = true;
		}
		for (row = 0; row < 480; row++) {
			uint32_t offs = at + row * 4096;

			if (!d->layer_all[i] && !fb_written[offs >> 12] &&
			    !fb_written[(offs + 640 * 4 - 1) >> 12])
				continue;
			if (memcmp (fb_shadow->ptr (offs), fb_src->ptr (offs), 640 * 4)) {
				memcpy (fb_shadow->ptr (offs), fb_src->ptr (offs), 640 * 4);
				d->layer_rows[i][row] = true;
			}
		}
		copied[i] = true;
	}
	if (copied[0] && copied[1]) {
		const hikaru_layer_t *a = &d->frame_gpu.layers.layer[0][0];
		const hikaru_layer_t *b = &d->frame_gpu.layers.layer[0][1];

		if (abs ((int) a->y0 - (int) b->y0) < 480 && abs ((int) a->x0 - (int) b->x0) < 640)
			d->layer_all[0] = d->layer_all[1] = true;
	}
	/* The pages written taken: watched again */
	if (fb_rearm) {
		memset (fb_written, 0, sizeof (fb_written));
		fb_rearm ();
	}

	/* The window's size, the picture's place in it */
	hl::Window::drawable_size (&d->target_width, &d->target_height);
	hl::Window::layout (&d->target_layout);
}

/* On the thread: the frame handed over, drawn into out[last] */
void
Renderer::draw_job ()
{
	Thread *t = thread;
	Renderer *d = t->drawer;
	auto &o = t->out[t->last];

	if (!o.texture || o.width != d->target_width || o.height != d->target_height) {
		if (o.texture)
			glDeleteTextures (1, &o.texture);
		glGenTextures (1, &o.texture);
		glBindTexture (GL_TEXTURE_2D, o.texture);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, d->target_width, d->target_height,
		              0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		if (!o.fb)
			glGenFramebuffers (1, &o.fb);
		glBindFramebuffer (GL_FRAMEBUFFER, o.fb);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, o.texture, 0);
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		o.width = d->target_width;
		o.height = d->target_height;
		o.gen++;
		HL_ASSERT_NO_GL_ERROR ();
	}
	/* The window done showing what it held: waited for here, then the
	 * sync deleted, none left in use (glWaitSync's deleted at once
	 * crashed an Intel driver, now and then) */
	if (o.shown) {
		client_wait (o.shown);
		o.shown = 0;
	}

	d->target_fb = o.fb;
	forget_mesh_state (d);
	draw (d);
	HL_ASSERT_NO_GL_ERROR_FRAME ();
	print_stats (d);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);

	/* Never shown (the window waited for none): no one waits on it */
	if (o.drawn)
		glDeleteSync (o.drawn);
	o.drawn = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	/* The fence on its way, for the window's context to wait on */
	glFlush ();

	/* Shown here, the machine's thread free of it (it took 0.8 ms of its
	 * frame, the driver's present) */
	if (t->present) {
		glBindFramebuffer (GL_READ_FRAMEBUFFER, o.fb);
		glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
		glDisable (GL_SCISSOR_TEST);
		glViewport (0, 0, d->target_width, d->target_height);
		glBlitFramebuffer (0, 0, o.width, o.height, 0, 0, d->target_width, d->target_height,
		                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		hl::Window::swap_from_thread ();
	}
}

/* In the window's context: out[i], drawn, to the window */
void
Renderer::show (unsigned i)
{
	auto &o = thread->out[i];
	int dw, dh;

	if (!o.texture)
		return;
	hl::Window::drawable_size (&dw, &dh);
	if (o.drawn) {
		client_wait (o.drawn);
		o.drawn = 0;
	}
	if (!o.read_fb)
		glGenFramebuffers (1, &o.read_fb);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, o.read_fb);
	if (o.read_gen != o.gen) {
		glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, o.texture, 0);
		o.read_gen = o.gen;
	}
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, dw, dh);
	glBlitFramebuffer (0, 0, o.width, o.height, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT,
	                   (o.width == dw && o.height == dh) ? GL_NEAREST : GL_LINEAR);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);

	/* Shown twice (flush ()): the first one's sync, done with */
	if (o.shown)
		client_wait (o.shown);
	o.shown = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush ();
}

/* Window::set_flush ()'s: the last frame drawn, to the window if it is not
 * there yet */
bool
Renderer::flush (void *data)
{
	Renderer *hr = (Renderer *) data;
	Thread *t = hr->thread;

	hr->wait_thread (false);
	if (!t->jobs || t->shown_job == t->jobs)
		return false;
	hr->show (t->last);
	t->shown_job = t->jobs;
	return true;
}

/****************************************************************************
 Interface
****************************************************************************/

void
Renderer::on_begin_frame ()
{
	hl::Renderer *renderer = this;
	Renderer *hr = (Renderer *) renderer;
	unsigned vpi, i;

	forget_mesh_state (hr);

	hr->num_vps = 0;
	hr->num_mvs = 0;
	hr->num_instances = 0;
	hr->num_mats = 0;
	hr->num_texs = 0;
	hr->num_lss = 0;

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			hr->num_meshes[vpi][i] = 0;
	hr->total_meshes = 0;
	hr->meshes.verts.clear ();

	update_debug_flags (hr);

	HL_ASSERT_NO_GL_ERROR ();
}

void
Renderer::on_end_frame ()
{
	hl::Renderer *renderer = this;
	Renderer *hr = (Renderer *) renderer;

	HL_ASSERT_NO_GL_ERROR ();

	if (hr->thread) {
		Thread *t = hr->thread;
		uint64_t before = t->jobs;
		unsigned shown = t->last;

		/* The frame before drawn: this one to the thread, that one
		 * to the window */
		{
			HL_PROFILE (RENDER_WAIT);
			wait_thread (true);
		}
		{
			HL_PROFILE (RENDER_COPY);
			hand_over ();
		}
		t->last = t->next;
		t->next ^= 1;
		t->jobs++;
		/* Shown by the thread itself, while the interface draws nothing
		 * over it */
		t->present = t->on_window && !hl::Window::overlay_wanted ();
		{
			std::lock_guard<std::mutex> lock (t->lock);
			t->busy = true;
			t->job = true;
		}
		t->wake.notify_one ();
		if (t->present)
			hl::Window::set_presented ();
		else if (before) {
			HL_PROFILE (RENDER_SHOW);
			show (shown);
			t->shown_job = before;
		} else {
			glBindFramebuffer (GL_FRAMEBUFFER, 0);
			glDisable (GL_SCISSOR_TEST);
			glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
			glClear (GL_COLOR_BUFFER_BIT);
		}
		HL_ASSERT_NO_GL_ERROR ();
	} else {
		copy_frame_gpu (hr, hr->gpu);
		copy_view (hr);
		draw (hr);
		HL_ASSERT_NO_GL_ERROR ();
		print_stats (hr);
	}

	LOG (" ==== RENDSTATE STATISTICS ==== ");
	LOG ("  vp  : %u", hr->num_vps);
	LOG ("  mv  : %u", hr->num_mvs);
	LOG ("  mat : %u", hr->num_mats);
	LOG ("  tex : %u", hr->num_texs);
	LOG ("  ls  : %u", hr->num_lss);
}

void
Renderer::invalidate_texram ()
{
	unsigned bank, x, y;

	for (bank = 0; bank < 2; bank++)
		for (y = 0; y < 0x40; y++)
			for (x = 0; x < 0x80; x++) {
				textures.slot_writes[bank][y][x]++;
				texram_dirty[bank][y][x] = thread != NULL;
			}
}

void
Renderer::reset ()
{
	invalidate_texram ();
}

Renderer::~Renderer ()
{
	Renderer *hr = this;

	stop_thread ();
	/* The picker's, after a game in widescreen */
	if (!is_drawer)
		hl::Window::set_native_size (640, 480);
	destroy_3d_state (hr);
	destroy_fb_state (hr);

	invalidate_texcache (NULL);
}

Renderer::Renderer (hl::Buffer *fb, hl::Buffer *texram[2]) :
	hl::Renderer (hl::Config::get ()->widescreen ? 854 : 640, 480)
{
	Renderer *hr = this;

	memset (fb_written, 1, sizeof (fb_written));

	/* WIDESCREEN: the scene at 854x480, the window's picture too */
	scene_w = width;
	hl::Window::set_native_size (width, height);
	copy_view (hr);

	HL_ASSERT_NO_GL_ERROR ();

	memset ((void *) program_cache, 0, sizeof (program_cache));
	programs_warmed = false;
	num_programs = 0;
	meshes.vao = 0;
	meshes.vbo = 0;
	/* Not the GPU's count: the first frame clears the buffers */
	framebuffer.lists_drawn = ~0u;
	memset ((void *) &framebuffer.drawn, 0, sizeof (framebuffer.drawn));
	memset ((void *) &framebuffer.blend, 0, sizeof (framebuffer.blend));
	memset ((void *) &textures, 0, sizeof (textures));
	texram_src[0] = texram[0];
	texram_src[1] = texram[1];
	fb_src = fb;
	target_fb = 0;
	thread = NULL;

	init_debug_flags (hr);

	if (build_3d_state (hr))
		HL_ABORT ("could not build the 3D state");
	HL_ASSERT_NO_GL_ERROR ();

	/* The thread's renderer has the GL objects */
	if (hl::Config::get ()->threaded_rendering && start_thread (fb, texram))
		return;

	if (build_fb_state (hr))
		HL_ABORT ("could not build the framebuffer state");
	HL_ASSERT_NO_GL_ERROR ();
}

/* The drawing thread's, in its context: draws from the copies of the TEXRAM
 * and the framebuffer */
Renderer::Renderer (Renderer *owner) :
	hl::Renderer (owner->width, owner->height)
{
	Renderer *hr = this;

	scene_w = owner->scene_w;
	is_drawer = true;
	copy_view (hr);

	gpu = owner->gpu;
	meshes.vao = 0;
	meshes.vbo = 0;
	framebuffer.lists_drawn = ~0u;
	memset ((void *) &framebuffer.drawn, 0, sizeof (framebuffer.drawn));
	memset ((void *) &framebuffer.blend, 0, sizeof (framebuffer.blend));
	texram_src[0] = owner->texram_shadow[0];
	texram_src[1] = owner->texram_shadow[1];
	fb_src = owner->fb_shadow;
	layers_tracked = true;
	target_fb = 0;
	thread = NULL;

	init_debug_flags (hr);

	if (build_3d_state (hr))
		HL_ABORT ("could not build the 3D state");
	if (build_fb_state (hr))
		HL_ABORT ("could not build the framebuffer state");
	HL_ASSERT_NO_GL_ERROR ();
}

void
Renderer::set_gpu (void *gpu_as_void)
{
	hl::Renderer *renderer = this;
	Renderer *hr = (Renderer *) renderer;
	Gpu *gpu = (Gpu *) gpu_as_void;

	hr->gpu = gpu;
}

} /* namespace hikaru */
