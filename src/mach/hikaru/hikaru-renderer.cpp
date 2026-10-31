/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-renderer.c, Copyright (C) 2011-2013, Stefano Teso.
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
#include <mutex>
#include <string>
#include <thread>

#include "hl/config.h"
#include "hl/input.h"
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
	if (is_texhead_eq (hr, th, &cached->th) && cached->gen == writes)
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
};										\n \
										\n \
uniform light_t		u_lights[4];						\n \
uniform vec3		u_ambient;						\n \
uniform sampler2D	u_texture;						\n \
uniform vec2		u_fog;							\n \
uniform vec3		u_fog_color;						\n \
uniform vec2		u_alpha_range;						\n \
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
             in int type,							\n \
             in int att_type,							\n \
             in int has_specular)						\n \
{										\n \
	vec3 light_direction;							\n \
	float distance, attenuation, intensity;					\n \
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
	vec3 normal = normalize (p_normal);					\n \
										\n \
#if HAS_TWO_SIDED_LIGHTING							\n \
	intensity = abs (dot (normal, light_direction));			\n \
#else										\n \
	intensity = max (dot (normal, light_direction), 0.0);			\n \
#endif										\n \
	/* A spot's direction goes from the light to the scene (AIRTRIX's	\n \
	 * skater: lights above it pointing down), unlike a directional	\n \
	 * light's, which points to the light. Its light rises from the	\n \
	 * edge of its cone over a width (451) */				\n \
	if (type == 2) {							\n \
		vec3 spot_direction = normalize (light.direction);		\n \
		float c = dot (spot_direction, -light_direction);		\n \
		attenuation *= clamp ((c - light.cone.x) * light.cone.y, 0.0, 1.0);	\n \
	}									\n \
										\n \
	/* No material diffuse here: it is the base colour (see main) */	\n \
	diffuse += attenuation * intensity * light.diffuse;			\n \
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
	/* The material's ambient (691), lit by the scene's in mode 1 */	\n \
#if HAS_FIXED_AMBIENT								\n \
	vec3 ambient  = p_unknown;						\n \
#else										\n \
	vec3 ambient  = u_ambient * p_unknown;					\n \
#endif										\n \
										\n \
#if HAS_LIGHT0									\n \
	apply_light (diffuse, specular, u_lights[0], LIGHT0_TYPE, LIGHT0_ATT_TYPE, HAS_LIGHT0_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT1									\n \
	apply_light (diffuse, specular, u_lights[1], LIGHT1_TYPE, LIGHT1_ATT_TYPE, HAS_LIGHT1_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT2									\n \
	apply_light (diffuse, specular, u_lights[2], LIGHT2_TYPE, LIGHT2_ATT_TYPE, HAS_LIGHT2_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT3									\n \
	apply_light (diffuse, specular, u_lights[3], LIGHT3_TYPE, LIGHT3_ATT_TYPE, HAS_LIGHT3_SPECULAR);		\n \
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
	color = clamp (color, 0.0, 1.0);					\n \
										\n \
#if HAS_ALPHA_TEST								\n \
	/* Punch-through: in its range, opaque; out, not drawn */		\n \
	if (color.a < u_alpha_range.x || color.a > u_alpha_range.y)		\n \
		discard;							\n \
	color.a = 1.0;								\n \
#endif										\n \
										\n \
#if HAS_FOG									\n \
	/* The viewport's fog (621), linear in 1/z: clear up to 1/x, its	\n \
	 * colour from 1/y on */						\n \
	float w = 1.0 / max (abs (p_position.z), 1e-6);				\n \
	float clear = clamp ((w - u_fog.y) / max (u_fog.x - u_fog.y, 1e-12),	\n \
	                     0.0, 1.0);						\n \
	frag_color = vec4 (mix (u_fog_color, color.rgb, clear), color.a);	\n \
#else										\n \
	frag_color = color;							\n \
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
		HL_ERROR ("recalling light with no direction nor position!");
	if (get_light_attenuation_type (lit) == HIKARU_LIGHT_ATT_INF)
		return HIKARU_LIGHT_TYPE_DIRECTIONAL;
	/* A spot has its cone (451); without, a light at a point */
	else if (lit->has_direction && lit->has_position && lit->has_specular)
		return HIKARU_LIGHT_TYPE_SPOT;
	else if (lit->has_position)
		return HIKARU_LIGHT_TYPE_POSITIONAL;
	return HIKARU_LIGHT_TYPE_DIRECTIONAL;
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

#define MAX_PROGRAMS	256

static struct {
	hikaru_glsl_variant_t variant;
	GLuint program;
} program_cache[MAX_PROGRAMS];
unsigned num_programs;

static void
upload_glsl_program (Renderer *hr, hikaru_mesh_t *mesh)
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
	"#define HAS_FOG %d\n";

	hikaru_glsl_variant_t variant;
	char *definitions, *vs_source, *fs_source;
	int ret, i;

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
	                variant.has_fog);
	HL_ASSERT (ret >= 0);

	ret = asprintf (&vs_source, mesh_vs_source, definitions);
	HL_ASSERT (ret >= 0);

	ret = asprintf (&fs_source, mesh_fs_source, definitions);
	HL_ASSERT (ret >= 0);

	hr->meshes.variant = variant;
	hr->meshes.program = hl::gl::compile_program (vs_source, fs_source);

	free (definitions);
	free (vs_source);
	free (fs_source);

	if (0) {
		hl::gl::print_uniforms (hr->meshes.program);
		HL_ASSERT_NO_GL_ERROR ();
	}

	program_cache[num_programs].variant.full = variant.full;
	program_cache[num_programs].program = hr->meshes.program;
	num_programs++;
	HL_ASSERT (num_programs < MAX_PROGRAMS);

update_locations:

	glUseProgram (hr->meshes.program);
	HL_ASSERT_NO_GL_ERROR ();

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
	HL_ASSERT_NO_GL_ERROR ();
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

	if (!isnonnegative (vp->clip.l) || !isnonnegative (vp->clip.r) ||
	    !isnonnegative (vp->clip.b) || !isnonnegative (vp->clip.t) ||
	    !isnonnegative (vp->clip.f) || !isnonnegative (vp->clip.n)) {
		HL_ERROR ("non-positive viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if ((vp->clip.l >= vp->clip.r) ||
	    (vp->clip.b >= vp->clip.t) ||
	    (vp->clip.n >= vp->clip.f)) {
		HL_ERROR ("inverted viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if (!isnonnegative (vp->offset.x) || (vp->offset.x >= 640.0f) ||
	    !isnonnegative (vp->offset.y) || (vp->offset.y >= 480.0f)) {
		HL_ERROR ("invalid viewport offset: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	LOG ("vp  = %s : [w=%f h=%f dcx=%f dcy=%f]",
	     get_viewport_str (vp), w, h, dcx, dcy);

	hl::frustum (projection, left, right, bottom, top, vp->clip.n, 1e5);

	glUniformMatrix4fv (hr->meshes.locs.u_projection, 1, GL_FALSE,
	                    (const GLfloat *) projection);

	glViewport (vp->clip.l + margin - extra,
	            clip_b,
	            vp->clip.r - vp->clip.l + 2.0f * extra,
	            clip_t - clip_b);

	if (hr->meshes.variant.has_fog) {
		vec2f_t fog;
		vec3f_t fog_color;

		/* In 1/z: the draw distance's factor takes it that much
		 * farther */
		fog[0] = vp->depth.density / hr->view.distance;
		fog[1] = vp->depth.bias / hr->view.distance;
		glUniform2fv (hr->meshes.locs.u_fog, 1, fog);

		fog_color[0] = vp->depth.mask[0] * INV255;
		fog_color[1] = vp->depth.mask[1] * INV255;
		fog_color[2] = vp->depth.mask[2] * INV255;
		glUniform3fv (hr->meshes.locs.u_fog_color, 1, fog_color);
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

	glActiveTexture (GL_TEXTURE0 + 0);
	HL_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, tex ? tex->id : 0);
	HL_ASSERT_NO_GL_ERROR ();

	/* The texhead's wrap: textures of the same slot share a GL one */
	if (tex)
		set_texture_wrap (&hr->tex_list[mesh->tex_index]);

	glUniform1i (hr->meshes.locs.u_texture, 0);
	HL_ASSERT_NO_GL_ERROR ();
}

static void
upload_alpha_range (Renderer *hr, hikaru_mesh_t *mesh)
{
	if (!hr->meshes.variant.has_alpha_test)
		return;

	glUniform2fv (hr->meshes.locs.u_alpha_range, 1, mesh->alpha_thresh);
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

		if (hr->push.tmp[2].info.twosided &&
		    !hr->push.tmp[2].info.nocull)
			HL_ERROR ("got a vertex with culling and two-sided lighting!");

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

	mesh->vp_index = hr->num_vps - 1;
	mesh->mat_index = hr->num_mats - 1;
	mesh->tex_index = hr->num_texs - 1;
	mesh->ls_index = hr->num_lss - 1;

	/* The alpha range of the material's entry (C81) in the table (154):
	 * 7F..FF in AIRTRIX; the high byte only (the others are FFFF or 0) */
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

	glPolygonOffset (0.0f, -mesh->depth_bias);

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
 * the meshes draw from it. The vertex array keeps it bound. */
static void
upload_frame_vertices (Renderer *hr)
{
	std::vector<hikaru_vertex_body_t> &verts = hr->meshes.verts;

	if (!hr->meshes.vao)
		glGenVertexArrays (1, &hr->meshes.vao);
	if (!hr->meshes.vbo)
		glGenBuffers (1, &hr->meshes.vbo);
	glBindVertexArray (hr->meshes.vao);
	glBindBuffer (GL_ARRAY_BUFFER, hr->meshes.vbo);
	glBufferData (GL_ARRAY_BUFFER, sizeof (verts[0]) * verts.size (),
	              verts.data (), GL_STREAM_DRAW);
	HL_ASSERT_NO_GL_ERROR ();

	VAP (0, 3, GL_FLOAT,          position,  GL_FALSE);
	VAP (1, 3, GL_FLOAT,          normal,    GL_FALSE);
	VAP (2, 3, GL_UNSIGNED_BYTE,  diffuse,   GL_TRUE);
	VAP (3, 3, GL_UNSIGNED_BYTE,  ambient,   GL_TRUE);
	VAP (4, 4, GL_UNSIGNED_BYTE,  specular,  GL_TRUE);
	VAP (5, 3, GL_UNSIGNED_SHORT, unknown,   GL_FALSE);
	VAP (6, 2, GL_FLOAT,          texcoords, GL_FALSE);
	VAP (7, 1, GL_UNSIGNED_BYTE,  alpha,     GL_TRUE);
	VAP (8, 1, GL_UNSIGNED_BYTE,  link,      GL_FALSE);
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
 * blending mode (A81). The scene's alpha is not blended. */
static void
set_blending (Renderer *hr, hikaru_mesh_t *mesh)
{
	unsigned mode = 0;

	if (mesh->mat_index != ~0u)
		mode = hr->mat_list[mesh->mat_index].blending_mode;

	switch (mode) {
	case 0:	/* by its alpha */
		glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		                     GL_ONE, GL_ZERO);
		break;
	case 1:	/* times the scene */
		glBlendFuncSeparate (GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
		break;
	case 2:	/* added, by its alpha: AIRTRIX's trick trails, lights */
		glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		break;
	case 3:	/* added, the scene dimmed by its alpha */
		glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		                     GL_ONE, GL_ZERO);
		break;
	}
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

	switch (polytype) {
	case HIKARU_POLYTYPE_TRANSPARENT:
		/* Punch-through: alpha-tested in the shader, opaque */
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
		glEnable (GL_POLYGON_OFFSET_FILL);
		break;
	case HIKARU_POLYTYPE_TRANSLUCENT:
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
		glEnable (GL_POLYGON_OFFSET_FILL);
		break;
	case HIKARU_POLYTYPE_BACKGROUND:
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		break;
	default:
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		break;
	}
	
	/* The translucent meshes from back to front, as the hardware draws
	 * them: AIRTRIX's OHOSOHO sign is a panel with its letters cut out,
	 * then a magenta quad behind it, which in the list's order covered
	 * the panel (these meshes write no depth) */
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

	upload_frame_vertices (hr);

	/* Note that "the pixel ownership test, the scissor test, dithering,
	 * and the buffer writemasks affect the operation of glClear". */
	glDepthMask (GL_TRUE);
	glDisable (GL_SCISSOR_TEST);
	glClearColor (hr->frame_gpu.fb_config.clear[0] * INV255,
	              hr->frame_gpu.fb_config.clear[1] * INV255,
	              hr->frame_gpu.fb_config.clear[2] * INV255,
	              hr->frame_gpu.fb_config.clear[3] * INV255);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* The scene's 640x480 (854 in widescreen): the box a context starts
	 * with is its first window's, 16x16 for the drawing thread's
	 * (THREADED_RENDERING) */
	glEnable (GL_SCISSOR_TEST);
	glScissor (0, 0, hr->scene_w, 480);

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
	glViewport (0, 0, hr->scene_w, 480);
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

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			free (hr->mesh_list[vpi][i]);

	hl::gl::destroy_program (hr->meshes.program);
	HL_ASSERT_NO_GL_ERROR ();

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

	if (!hr->vp_list || !hr->mv_list || !hr->mat_list || !hr->tex_list || !hr->ls_list)
		return -1;

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
"uniform int u_invert_f = 0;							\n"
"uniform int u_invert_b = 0;							\n"
"/* Where the 2D layers are across the scene: their left edge, their width	\n"
" * (widescreen: 4:3 in the middle, or stretched) */				\n"
"uniform vec2 u_layer_x = vec2 (0.0, 1.0);					\n"
"										\n"
"in vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	vec2 p_texcoords_inv = vec2 ((p_texcoords.s - u_layer_x.x) / u_layer_x.y,	\n"
"	                             1 - p_texcoords.t);				\n"
"	float inside = (p_texcoords_inv.s >= 0.0 && p_texcoords_inv.s <= 1.0) ?	\n"
"	               1.0 : 0.0;							\n"
"	vec4 texel_f, texel_b;							\n"
"										\n"
"	if (u_invert_f == 0)							\n"
"		texel_f = texture (u_front, p_texcoords);			\n"
"	else									\n"
"		texel_f = texture (u_front, p_texcoords_inv) * inside;		\n"
"	texel_f = texel_f * u_mult_f;						\n"
"										\n"
"	if (u_invert_b == 0)							\n"
"		texel_b = texture (u_back,  p_texcoords);			\n"
"	else									\n"
"		texel_b = texture (u_back,  p_texcoords_inv) * inside;		\n"
"	texel_b = texel_b * u_mult_b;						\n"
"										\n"
"	vec4 texel_1 = texture (u_layer1, p_texcoords_inv) * inside;		\n"
"	vec4 texel_2 = texture (u_layer2, p_texcoords_inv) * inside;		\n"
"										\n"
"	/* The layers go over the scene where their alpha is set: elsewhere	\n"
"	 * they are transparent, whatever their colour (SGNASCAR's texts). The	\n"
"	 * multiplier scales the alpha too (A2BGR10: PHARRIER's warning). A	\n"
"	 * multiplier of 0 means no layer there. Layer 1 over layer 2:		\n"
"	 * AIRTRIX's logos, on layer 1, over the white of layer 2. */		\n"
"	frag_color = mix (texel_f, texel_b, u_factor);				\n"
"	if (u_mult_2 > 0.0)							\n"
"		frag_color.rgb = mix (frag_color.rgb, texel_2.rgb * u_mult_2,	\n"
"		                      clamp (texel_2.a * u_mult_2, 0.0, 1.0));	\n"
"	if (u_mult_1 > 0.0)							\n"
"		frag_color.rgb = mix (frag_color.rgb, texel_1.rgb * u_mult_1,	\n"
"		                      clamp (texel_1.a * u_mult_1, 0.0, 1.0));	\n"
"}\n";

#define NUM_FRAMEBUFFERS 2

#define OFFSET(member_) \
	((const GLvoid *) offsetof (std::remove_reference_t<decltype (fb_vbo_data[0])>, member_))

static int
build_fb_state (Renderer *hr)
{
	static const GLenum targets[1] = { GL_COLOR_ATTACHMENT0 };
	unsigned i;

	/* Compile the framebuffer GLSL program and retrieve the uniforms. */
	hr->framebuffer.program =
		hl::gl::compile_program (fb_vs_source, fb_fs_source);
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
	hr->framebuffer.locs.u_invert_f =
		get_uniform_loc (hr->framebuffer.program, "u_invert_f");
	hr->framebuffer.locs.u_invert_b =
		get_uniform_loc (hr->framebuffer.program, "u_invert_b");
	hr->framebuffer.locs.u_layer_x =
		get_uniform_loc (hr->framebuffer.program, "u_layer_x");

	/* Generate two framebuffers (the front and the back buffers), each
	 * associated with a colorbuffer (texture) and a depth buffer
	 * (renderbuffer). */
	glGenFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glGenTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glGenRenderbuffers (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	HL_ASSERT_NO_GL_ERROR ();

	for (i = 0; i < NUM_FRAMEBUFFERS; i++) {

		/* Bind the i'th framebuffer */
		glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[i]);
		HL_ASSERT_NO_GL_ERROR ();

		/* Create and attach a texture as color buffer. */
		glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[i]);
		HL_ASSERT_NO_GL_ERROR ();
		glTexStorage2D (GL_TEXTURE_2D, 1, GL_RGBA8, hr->scene_w, 480);
		HL_ASSERT_NO_GL_ERROR ();
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, hr->scene_w, 480,
		                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		HL_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		HL_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		HL_ASSERT_NO_GL_ERROR ();

		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, hr->framebuffer.cb[i], 0);
		HL_ASSERT_NO_GL_ERROR ();

		glDrawBuffers (1, targets);
		HL_ASSERT_NO_GL_ERROR ();

		/* Create and attach a renderbuffer as depth buffer. */
		glBindRenderbuffer (GL_RENDERBUFFER, hr->framebuffer.db[i]);
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT, hr->scene_w, 480);
		HL_ASSERT_NO_GL_ERROR ();

		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                           GL_RENDERBUFFER, hr->framebuffer.db[i]);
		HL_ASSERT_NO_GL_ERROR ();

		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return -1;
	}

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
	HL_ASSERT_NO_GL_ERROR ();

	hl::gl::destroy_program (hr->framebuffer.program);
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
	 * THREADED_RENDERING) */
	if (!hr->framebuffer.layers[i])
		glGenTextures (1, &hr->framebuffer.layers[i]);
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

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	switch (layer->format) {
	case HIKARU_FORMAT_ABGR1555: {
		/* Two pixels a 32-bit word, the left one in its high half (see
		 * hikaru-gpu.cpp, "Framebuffer Configuration"): each pair the
		 * other way round for GL */
		static uint16_t pixels[640 * 480];
		const uint16_t *src = (const uint16_t *) data;
		unsigned x, y;

		for (y = 0; y < 480; y++)
			for (x = 0; x < 640; x++)
				pixels[y * 640 + x] = src[y * 2048 + (x ^ 1)];
		data = pixels;
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 640);

		glTexImage2D (GL_TEXTURE_2D, 0,
		              GL_RGB5_A1,
		              640, 480, 0,
		              GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
		              data);

		*mult = 1.0f;
		break;
	}
	case HIKARU_FORMAT_A2BGR10:
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 1024);

		glTexImage2D (GL_TEXTURE_2D, 0,
		              GL_RGB10_A2,
		              640, 480, 0,
		              GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
		              data);

		*mult = 4.0f;
		break;
	default:
		HL_ASSERT (0);
	}
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	HL_ASSERT_NO_GL_ERROR ();
}

/* TODO make use of the framebuffer configuration in the GPU:1A registers,
 * right now it only uses the information in the 781/181 commands. */
/* The picture's place, the bars around it cleared: in the window, or in
 * the drawing thread's texture at the window's size */
static void
set_output_viewport (Renderer *hr)
{
	if (!hr->target_fb) {
		hl::Window::set_output_viewport ();
		return;
	}
	glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, hr->target_width, hr->target_height);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glViewport (hr->target_rect[0], hr->target_rect[1],
	            hr->target_rect[2], hr->target_rect[3]);
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
}

/* What draw () reads of the GPU's, at the frame's end */
static void
copy_frame_gpu (Renderer *hr, Gpu *gpu)
{
	hr->frame_gpu.fb_config = gpu->fb_config;
	hr->frame_gpu.layers = LAYERS;
	hr->frame_gpu.lists_begun = gpu->lists_begun;
}

static void
draw (Renderer *hr)
{
	auto *gpu = &hr->frame_gpu;
	GLuint n1, n2, f, b, layer1 = 0, layer2 = 0;
	float factor, mult1 = 0.0f, mult2 = 0.0f;
	bool on1, on2, drawn = false;

	/* Figure out the GPU framebuffer assignments */
	HL_PRINT ("FRAMEBUFFER 781=%08X 181=%08X", gpu->fb_config._781, gpu->fb_config._181);

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
		hr->framebuffer.lists_drawn = gpu->lists_begun;

		glBindFramebuffer (GL_FRAMEBUFFER,
		                   (n1 < 2) ? hr->framebuffer.fb[n1] : hr->target_fb);
		HL_ASSERT_NO_GL_ERROR ();

		draw_scene (hr);
		HL_ASSERT_NO_GL_ERROR ();
		drawn = true;

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
			glViewport (0, 0, hr->scene_w, 480);
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
			HL_ASSERT_NO_GL_ERROR ();

			/* front = back * factor + front * (1 - factor) */
			glEnable (GL_BLEND);
			glBlendColor (0.0f, 0.0f, 0.0f, factor);
			glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
			glBindVertexArray (hr->framebuffer.vao);
			glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
			glBindVertexArray (0);
			glDisable (GL_BLEND);
			HL_ASSERT_NO_GL_ERROR ();

			glBindFramebuffer (GL_FRAMEBUFFER, hr->target_fb);
			HL_ASSERT_NO_GL_ERROR ();
		}
		factor = 0.0f;
	}

	/* Where the picture goes in the window, after the aspect ratio */
	set_output_viewport (hr);

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
	glUniform1i (hr->framebuffer.locs.u_invert_f, n1 >= 2);
	glUniform1i (hr->framebuffer.locs.u_invert_b, n2 >= 2);
	if (hr->scene_w == 640 || hr->view.stretch_2d)
		glUniform2f (hr->framebuffer.locs.u_layer_x, 0.0f, 1.0f);
	else
		glUniform2f (hr->framebuffer.locs.u_layer_x,
		             (hr->scene_w - 640) * 0.5f / hr->scene_w, 640.0f / hr->scene_w);
	HL_ASSERT_NO_GL_ERROR ();

	/* The scaling filter, on the four: the scene's buffers go back to
	 * NEAREST after, for the blending of the next frame */
	set_filter (0, hr->view.linear ? GL_LINEAR : GL_NEAREST);
	set_filter (1, hr->view.linear ? GL_LINEAR : GL_NEAREST);
	set_filter (2, hr->view.linear ? GL_LINEAR : GL_NEAREST);
	set_filter (3, hr->view.linear ? GL_LINEAR : GL_NEAREST);

	glBindVertexArray (hr->framebuffer.vao);
	HL_ASSERT_NO_GL_ERROR ();

	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	HL_ASSERT_NO_GL_ERROR ();

	if (hr->view.linear) {
		set_filter (0, GL_NEAREST);
		set_filter (1, GL_NEAREST);
	}

	glBindVertexArray (0);
	glUseProgram (0);
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
	if (hl::Window::make_current (t->context)) {
		try {
			t->drawer = new Renderer (owner);
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
			if (t->drawer)
				owner->draw_job ();
		} catch (const hl::Abort &e) {
			t->error = e.what ();
		}
	}

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
	 * bytes at most) */
	for (i = 0; i < 2; i++) {
		hikaru_layer_t *layer = &d->frame_gpu.layers.layer[0][i];
		unsigned num = 2 + i;

		if (!(layer->enabled || d->frame_gpu.fb_config.num_1 == num ||
		      d->frame_gpu.fb_config.num_2 == num) ||
		    d->debug.flags[i ? HR_DEBUG_NO_LAYER2 : HR_DEBUG_NO_LAYER1])
			continue;
		if ((uint64_t) (layer->y0 + 479) * 4096 + (layer->x0 + 640) * 4 >
		    fb_src->size ())
			continue;
		for (row = 0; row < 480; row++) {
			uint32_t offs = (layer->y0 + row) * 4096 + layer->x0 * 4;
			memcpy (fb_shadow->ptr (offs), fb_src->ptr (offs), 640 * 4);
		}
	}

	/* The window's size, the picture's place in it */
	hl::Window::drawable_size (&d->target_width, &d->target_height);
	hl::Window::output_rect (&d->target_rect[0], &d->target_rect[1],
	                         &d->target_rect[2], &d->target_rect[3]);
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
	d->meshes.variant.full = ~0;
	draw (d);
	HL_ASSERT_NO_GL_ERROR ();
	print_stats (d);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);

	/* Never shown (the window waited for none): no one waits on it */
	if (o.drawn)
		glDeleteSync (o.drawn);
	o.drawn = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	/* The fence on its way, for the window's context to wait on */
	glFlush ();
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

	hr->meshes.variant.full = ~0;

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
		{
			std::lock_guard<std::mutex> lock (t->lock);
			t->busy = true;
			t->job = true;
		}
		t->wake.notify_one ();
		if (before) {
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

	/* WIDESCREEN: the scene at 854x480, the window's picture too */
	scene_w = width;
	hl::Window::set_native_size (width, height);
	copy_view (hr);

	HL_ASSERT_NO_GL_ERROR ();

	memset ((void *) program_cache, 0, sizeof (program_cache));
	num_programs = 0;
	meshes.vao = 0;
	meshes.vbo = 0;
	/* Not the GPU's count: the first frame clears the buffers */
	framebuffer.lists_drawn = ~0u;
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
	texram_src[0] = owner->texram_shadow[0];
	texram_src[1] = owner->texram_shadow[1];
	fb_src = owner->fb_shadow;
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
