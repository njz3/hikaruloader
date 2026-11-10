/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/gaelco/gaelco-gpu.cpp; the modifier volumes after
 * Flycast's core/rend/gles/gldraw.cpp (GPL-2.0-or-later, copyright-holders
 * Flyinghead and its contributors).
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
#include "hl/window.h"

#include "hl/bezel.h"
#include "mach/naomi/naomi-renderer.h"

namespace naomi {

#define OIT_MAX_NODES	(16u << 20)
#define OIT_DEPTH_UNIT	2
#define OIT_BELOW_UNIT	3

/* The vertices come in the screen's 640x480 already. WIDESCREEN: the scene
 * u_width (854) wide, the 640 in its middle, what the games send beyond
 * their sides seen there (the objects they leave out as out of sight
 * missing); the 2D stretched to the whole width (u_stretch_2d, the
 * WIDESCREEN_2D setting): the HUD's polygons, each at a 1/W of 1 or more
 * (1, 2, 3... one a sprite; the 3D's is 0.2 at most), and the backgrounds at
 * the far plane's 1e-6 (Smashing Drive's sky, the pictures) */
static const char *vs_src = R"(#version 430
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_base;
layout (location = 3) in vec4 in_offset;
out vec2 v_uv;
out vec4 v_base;
out vec4 v_offset;
noperspective out float v_z;
uniform float u_width;
uniform int u_stretch_2d;
uniform vec2 u_scale;
uniform vec2 u_target;
void main ()
{
	float w = 1.0 / in_pos.z;
	bool flat_2d = in_pos.z >= 1.0 || in_pos.z <= 1.0001e-6;
	/* At 640, and the 2D stretched: as before (the uniform's division
	 * rounds otherwise, a few pixels a frame) */
	float px = in_pos.x * u_scale.x, py = in_pos.y * u_scale.y;
	float x = px / (u_target.x * 0.5) - 1.0;
	if (u_width > 640.0 && !(u_stretch_2d != 0 && flat_2d))
		x = (px + (u_width - 640.0) * 0.5) / (u_width * 0.5) - 1.0;
	gl_Position = vec4 (x, 1.0 - py / (u_target.y * 0.5), 0.0, 1.0) * w;
	v_uv = in_uv;
	v_base = in_base;
	v_offset = in_offset;
	v_z = in_pos.z;
}
)";

/* u_flags: bits 0-1 the shading, 2 textured, 3 the base's alpha used, 4 the
 * texture's alpha ignored, 5 the offset added, 7-8 the fog, 9 the colour
 * clamped, 10 punch-through, 11 and 12 tri-linear's passes A and B, 13 inside
 * a modifier volume in Intensity Volume mode, 14 the user clip's "outside" */
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
uniform vec3 u_fog_color_vertex;
uniform vec4 u_clamp_max;
uniform vec4 u_clamp_min;
uniform float u_pt_ref;
uniform float u_lod_bias;
uniform float u_max_lod;
uniform float u_shade_scale;
uniform vec4 u_clip_out;
#ifdef COLLECT
layout (binding = 0, offset = 0) uniform atomic_uint u_count;
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;
layout (std430, binding = 0) buffer Nodes { uvec4 nodes[]; };
uniform uint u_max_nodes;
uniform uint u_tag;
uniform sampler2D u_opaque_depth;
#endif
/* The table's coefficient at a pixel of depth z (the vertices' 1/W): z by
 * FOG_DENSITY, clamped to 1.0-255.9999, is the address (bits 6-4 its
 * exponent, 3-0 the top of its mantissa), the far pixels at the table's
 * start; each entry's high byte its own coefficient, the low one the next
 * address's, between which the rest of the mantissa interpolates (PowerVR
 * 2, "Fog Processing"; ATV Track's and Smashing Drive's tables go from
 * 0x3F-0xD9 at the start to 0 at the end) */
float fog_table (float z_)
{
	float z = clamp (z_ * u_fog_density, 1.0, 255.9999);
	float e = floor (log2 (z));
	float m = z * 16.0 / pow (2.0, e) - 16.0;
	int i = int (floor (m) + e * 16.0);
	uint entry = texelFetch (u_fog, clamp (i, 0, 127), 0).r;
	float a = float (entry >> 8u) / 255.0, b = float (entry & 255u) / 255.0;
	return mix (a, b, fract (m));
}
void main ()
{
	/* The user clip, its "outside" mode: nothing inside its rectangle */
	if ((u_flags & 16384) != 0 && gl_FragCoord.x >= u_clip_out.x && gl_FragCoord.x <= u_clip_out.z &&
	    gl_FragCoord.y >= u_clip_out.y && gl_FragCoord.y <= u_clip_out.w)
		discard;
	vec4 color = v_base;
	vec3 offset = v_offset.rgb;
	float trilinear = 1.0;
	/* Inside an Intensity Volume, before the texture is combined */
	if ((u_flags & 8192) != 0) {
		color.rgb *= u_shade_scale;
		offset *= u_shade_scale;
	}
	if ((u_flags & 8) == 0)
		color.a = 1.0;
	if ((u_flags & 4) != 0) {
		vec4 t;
		if ((u_flags & 6144) != 0) {
			/* Tri-linear: pass A takes the level of the D value, the other
			 * the next smaller one, each by its share of D's decimals */
			float lod = clamp (textureQueryLod (u_tex, v_uv).x + u_lod_bias, 0.0, u_max_lod);
			float level = floor (lod);
			if ((u_flags & 2048) != 0) {
				t = textureLod (u_tex, v_uv, level);
				trilinear = 1.0 - (lod - level);
			} else {
				t = textureLod (u_tex, v_uv, level + 1.0);
				trilinear = lod - level;
			}
		} else
			t = texture (u_tex, v_uv, u_lod_bias);
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
			color.rgb += offset;
	}
	/* Punch-through: under PT_ALPHA_REF, nothing */
	if ((u_flags & 1024) != 0 && color.a < u_pt_ref)
		discard;
	/* The colour clamp, before the fog: each of ARGB between FOG_CLAMP_MIN
	 * and FOG_CLAMP_MAX */
	if ((u_flags & 512) != 0)
		color = min (max (color, u_clamp_min), u_clamp_max);
	int fog = (u_flags >> 7) & 3;
	/* 0 the table; 1 per vertex, the offset colour's alpha toward
	 * FOG_COL_VERT ("no fog" without the offset); 2 none; 3 the table's
	 * "mode 2", the polygon's colour replaced by the table's colour and
	 * its alpha by the coefficient, to fog what is under it once blended
	 * (PowerVR 2, "Fog Control"; the three games use 0 and 2 only) */
	if (fog == 0)
		color.rgb = mix (color.rgb, u_fog_color, fog_table (v_z));
	else if (fog == 1 && (u_flags & 32) != 0)
		color.rgb = mix (color.rgb, u_fog_color_vertex, v_offset.a);
	else if (fog == 3)
		color = vec4 (u_fog_color, fog_table (v_z));
	/* After the fog: the two passes add up to it once */
	color *= trilinear;
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
		                     next, u_tag + (uint (gl_PrimitiveID) << 8));
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
 *    instructions, the SRC and DST selects at bits 6-7, the triangle's place
 *    in the lists << 8; an r32ui image holds each pixel's last + 1);
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
	vec4 color, secondary = vec4 (0.0);

	if (head == 0u)
		discard;
	color = texelFetch (u_below, pixel, 0);
	for (;;) {
		/* The next KEPT in order, after the last blended */
		num = 0;
		more = false;
		for (i = head; i != 0u; i = nodes[i - 1u].z) {
			float d = uintBitsToFloat (nodes[i - 1u].y);
			uint o = nodes[i - 1u].w >> 8;

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
			bool src_sel = (n.w & 64u) != 0u, dst_sel = (n.w & 128u) != 0u;
			/* SRC select: the secondary buffer in place of the polygon's
			 * colour; DST select: the blending goes on with, and to, it */
			vec4 src = src_sel ? secondary : unpackUnorm4x8 (n.x);
			vec4 dst = dst_sel ? secondary : color;
			vec4 c = src * factor (n.w & 7u, src, dst, true) +
			         dst * factor ((n.w >> 3) & 7u, src, dst, false);
			c = floor (clamp (c, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
			if (dst_sel)
				secondary = c;
			else
				color = c;
		}
		if (!more)
			break;
		last_d = depth[KEPT - 1];
		last_o = order[KEPT - 1];
	}
	out_color = color;
}
)";


static const char *blit_vs_src = R"(#version 430
out vec2 v_uv;
uniform int u_flip;
void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = vec2 (p.x, u_flip != 0 ? 1.0 - p.y : p.y);
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


static const char *filter_fs_src = R"(#version 430
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_tex;
uniform float u_line;
uniform vec2 u_coef;
void main ()
{
	vec3 c = texture (u_tex, v_uv - vec2 (0.0, u_line)).rgb * u_coef.x +
	         texture (u_tex, v_uv).rgb * u_coef.y +
	         texture (u_tex, v_uv + vec2 (0.0, u_line)).rgb * u_coef.x;
	out_color = vec4 (c, 1.0);
}
)";

static const char *mod_vs_src = R"(#version 430
layout (location = 0) in vec3 in_pos;
noperspective out float v_z;
uniform vec2 u_scale;
uniform vec2 u_target;
void main ()
{
	float w = 1.0 / in_pos.z;
	gl_Position = vec4 (in_pos.x * u_scale.x / (u_target.x * 0.5) - 1.0,
	                    1.0 - in_pos.y * u_scale.y / (u_target.y * 0.5), 0.0, 1.0) * w;
	v_z = in_pos.z;
}
)";

static const char *mod_fs_src = R"(#version 430
noperspective in float v_z;
out vec4 out_color;
uniform float u_zscale;
void main ()
{
	out_color = vec4 (0.0);
	gl_FragDepth = v_z * u_zscale;
}
)";

/*
 * The Textures
 * ============
 *
 * The texture word:
 *
 *  31	  Mipmapped: the smaller levels first, the largest last
 *  30	  VQ: a 2048-byte code book of four texels each, then a byte per 2x2
 *  29-27 0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 YUV422, 4 bump, 5 4-bit paletted,
 *	  6 8-bit paletted
 *  26	  The lines in order (else twiddled); paletted, 26-21 the palette
 *  25	  Those lines as long as TEXT_CONTROL's stride
 *  20-0  Its address in the VRAM, in 8 bytes
 */

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

static inline uint32_t
rgba_16 (unsigned format, uint16_t c)
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

static inline uint32_t
rgba_yuv (int y, int u, int v)
{
	int r = y + (11 * (v - 128)) / 8;
	int g = y - (11 * (u - 128) + 22 * (v - 128)) / 32;
	int b = y + (55 * (u - 128)) / 32;

	r = std::max (0, std::min (255, r));
	g = std::max (0, std::min (255, g));
	b = std::max (0, std::min (255, b));
	return (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | 0xFF000000u;
}

/* A palette's entry as RGBA8, as PAL_RAM_CTRL has them */
static inline uint32_t
rgba_palette (const Draw &d, unsigned index)
{
	uint32_t c = d.palette[index & 1023];

	switch (d.pal_ctrl & 3) {
	case 0: return rgba_16 (0, (uint16_t) c);
	case 1: return rgba_16 (1, (uint16_t) c);
	case 2: return rgba_16 (2, (uint16_t) c);
	default: return (c & 0xFF00FF00) | ((c >> 16) & 255) | ((c & 255) << 16);
	}
}

/* Where the level of 2^i texels a side starts: in texels after the texture's
 * start (three unused, 1x1, 2x2...), its VQ indices in bytes after the code book
 * (the 1x1's byte, then the 2x2's...) */
static inline uint32_t
mip_texels (unsigned i)
{
	return 3 + ((1u << (2 * i)) - 1) / 3;
}

static inline uint32_t
mip_indices (unsigned i)
{
	return i ? 1 + ((1u << (2 * (i - 1))) - 1) / 3 : 0;
}

static inline unsigned
log2_of (unsigned w)
{
	unsigned i = 0;

	while ((2u << i) <= w)
		i++;
	return i;
}

static inline bool
is_paletted (uint32_t tcw)
{
	unsigned format = (tcw >> 27) & 7;

	return format == 5 || format == 6;
}

static inline unsigned
texel_bits (uint32_t tcw)
{
	unsigned format = (tcw >> 27) & 7;

	return format == 5 ? 4 : format == 6 ? 8 : 16;
}

static inline bool
is_scan (uint32_t tcw)
{
	return !is_paletted (tcw) && ((tcw >> 26) & 1);
}

/* The lines in order have no levels; VQ's are always twiddled */
static inline bool
is_mipmapped (uint32_t tcw)
{
	return (tcw >> 31) && (((tcw >> 30) & 1) || !is_scan (tcw));
}

/* What comes before the largest level, in bytes */
static inline uint32_t
mip_bytes (uint32_t tcw, unsigned w)
{
	if (!is_mipmapped (tcw))
		return 0;
	if ((tcw >> 30) & 1)
		return mip_indices (log2_of (w));
	return mip_texels (log2_of (w)) * texel_bits (tcw) / 8;
}

static inline unsigned
line_width (uint32_t tcw, unsigned w, unsigned stride)
{
	return (is_scan (tcw) && ((tcw >> 25) & 1) && stride) ? stride : w;
}

static uint32_t
texture_bytes (uint32_t tcw, unsigned w, unsigned h, unsigned stride)
{
	if ((tcw >> 30) & 1)
		return 2048 + mip_bytes (tcw, w) + (w / 2) * (h / 2);
	if (is_scan (tcw))
		return line_width (tcw, w, stride) * h * 2;
	return mip_bytes (tcw, w) + w * h * texel_bits (tcw) / 8;
}

/* One level of a texture at data: the largest, or the one of 2^mip texels a
 * side when mipmapped */
static void
decode_level (std::vector<uint32_t> &out, const uint8_t *data, const Draw &d, uint32_t tcw, unsigned w, unsigned h,
              unsigned mip)
{
	bool vq = (tcw >> 30) & 1, scan = is_scan (tcw), mipmapped = is_mipmapped (tcw);
	unsigned format = (tcw >> 27) & 7, stride = (d.text_control & 31) * 32, x, y;
	unsigned line = line_width (tcw, w, stride);
	uint32_t skip = mipmapped ? mip_texels (mip) : 0;

	out.resize (w * h);
	if (vq && mipmapped && mip == 0) {
		/* The 1x1 level, as a 2x2 block: the block's last texel */
		const uint16_t *e = &((const uint16_t *) data)[data[2048] * 4];

		out[0] = rgba_16 (format, e[3]);
	} else if (vq) {
		const uint16_t *book = (const uint16_t *) data;
		const uint8_t *index = data + 2048 + (mipmapped ? mip_indices (mip) : 0);

		for (y = 0; y < h; y += 2) {
			for (x = 0; x < w; x += 2) {
				const uint16_t *e = &book[index[twiddle (x / 2, y / 2, w / 2, h / 2)] * 4];

				out[y * w + x] = rgba_16 (format, e[0]);
				out[(y + 1) * w + x] = rgba_16 (format, e[1]);
				out[y * w + x + 1] = rgba_16 (format, e[2]);
				out[(y + 1) * w + x + 1] = rgba_16 (format, e[3]);
			}
		}
	} else if (format == 5) {
		unsigned selector = ((tcw >> 21) & 63) << 4;

		/* In nibbles: the 1x1 level is the fourth one */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				uint32_t i = skip + twiddle (x, y, w, h);
				unsigned value = (i & 1) ? data[i / 2] >> 4 : data[i / 2] & 15;

				out[y * w + x] = rgba_palette (d, selector + value);
			}
		}
	} else if (format == 6) {
		const uint8_t *t = data + skip;
		unsigned selector = ((tcw >> 25) & 3) << 8;

		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				out[y * w + x] = rgba_palette (d, selector + t[twiddle (x, y, w, h)]);
	} else if (format == 3 && mipmapped && mip == 0) {
		/* A pair of YUV needs two texels: the 1x1 level is RGB565 */
		out[0] = rgba_16 (1, ((const uint16_t *) data)[skip]);
	} else if (format == 3) {
		const uint16_t *t = (const uint16_t *) data + skip;

		for (y = 0; y < h; y++) {
			for (x = 0; x + 1 < w; x += 2) {
				uint16_t a = scan ? t[y * line + x] : t[twiddle (x, y, w, h)];
				uint16_t b = scan ? t[y * line + x + 1] : t[twiddle (x + 1, y, w, h)];

				out[y * w + x] = rgba_yuv (a >> 8, a & 255, b & 255);
				out[y * w + x + 1] = rgba_yuv (b >> 8, a & 255, b & 255);
			}
		}
	} else if (scan) {
		const uint16_t *t = (const uint16_t *) data;

		for (y = 0; y < h; y++)
			for (x = 0; x < w && x < line; x++)
				out[y * w + x] = rgba_16 (format, t[y * line + x]);
	} else {
		const uint16_t *t = (const uint16_t *) data + skip;

		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				out[y * w + x] = rgba_16 (format, t[twiddle (x, y, w, h)]);
	}
}

uint32_t
texture_size (uint32_t tcw, uint32_t tsp, uint32_t text_control)
{
	unsigned w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);

	/* The levels are square: the height is not looked at */
	if (is_mipmapped (tcw))
		h = w;
	return texture_bytes (tcw, w, h, (text_control & 31) * 32);
}

GLuint
BoardRenderer::texture (uint32_t tcw, uint32_t tsp)
{
	unsigned w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
	uint64_t key = texture_key (tcw, tsp);

	if (is_mipmapped (tcw))
		h = w;
	Texture &t = textures[key];

	if (t.id && t.seen == frame_count)
		return t.id;
	t.seen = frame_count;

	/* As it was at the STARTRENDER */
	auto found = draw->textures.find (key);
	const TextureData *kept = found != draw->textures.end () ? found->second.get () : NULL;
	uint64_t hash = kept ? kept->hash : 0x100000001B3ull;
	unsigned i;

	if (is_paletted (tcw)) {
		unsigned first = ((tcw >> 27) & 7) == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
		unsigned count = ((tcw >> 27) & 7) == 5 ? 16 : 256;

		hash = (hash ^ draw->pal_ctrl) * 0x100000001B3ull;
		for (i = 0; i < count; i++)
			hash = (hash ^ draw->palette[(first + i) & 1023]) * 0x100000001B3ull;
	}
	if (t.id && t.hash == hash)
		return t.id;
	t.hash = hash;

	std::vector<uint32_t> rgba;
	const uint8_t *data = kept ? kept->bytes.data () : NULL;

	if (!t.id)
		glGenTextures (1, &t.id);
	glBindTexture (GL_TEXTURE_2D, t.id);
	if (!data) {
		rgba.assign (w * h, 0xFFFF00FFu);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	} else if (is_mipmapped (tcw)) {
		/* As they are in the VRAM: GL's level 0 is the largest */
		unsigned top = log2_of (w), level;

		for (level = 0; level <= top; level++) {
			unsigned side = 1u << level;

			decode_level (rgba, data, *draw, tcw, side, side, level);
			glTexImage2D (GL_TEXTURE_2D, top - level, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			              rgba.data ());
		}
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, top);
	} else {
		decode_level (rgba, data, *draw, tcw, w, h, 0);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	}
	return t.id;
}

/*
 * The Renderer
 */

BoardRenderer::BoardRenderer (Holly *holly_, unsigned width_, unsigned height_) :
	holly (holly_),
	width (width_),
	height (height_)
{
	setup ();
}

BoardRenderer::~BoardRenderer ()
{
	for (auto &it : textures)
		glDeleteTextures (1, &it.second.id);
	destroy_oit ();
}

template <typename P> static void
locate (P &p)
{
	p.u_flags = glGetUniformLocation (p.id, "u_flags");
	p.u_tex = glGetUniformLocation (p.id, "u_tex");
	p.u_fog = glGetUniformLocation (p.id, "u_fog");
	p.u_zscale = glGetUniformLocation (p.id, "u_zscale");
	p.u_fog_color = glGetUniformLocation (p.id, "u_fog_color");
	p.u_fog_color_vertex = glGetUniformLocation (p.id, "u_fog_color_vertex");
	p.u_fog_density = glGetUniformLocation (p.id, "u_fog_density");
	p.u_width = glGetUniformLocation (p.id, "u_width");
	p.u_stretch_2d = glGetUniformLocation (p.id, "u_stretch_2d");
	p.u_clamp_max = glGetUniformLocation (p.id, "u_clamp_max");
	p.u_clamp_min = glGetUniformLocation (p.id, "u_clamp_min");
	p.u_scale = glGetUniformLocation (p.id, "u_scale");
	p.u_target = glGetUniformLocation (p.id, "u_target");
	p.u_pt_ref = glGetUniformLocation (p.id, "u_pt_ref");
	p.u_shade_scale = glGetUniformLocation (p.id, "u_shade_scale");
	p.u_clip_out = glGetUniformLocation (p.id, "u_clip_out");
	p.u_lod_bias = glGetUniformLocation (p.id, "u_lod_bias");
	p.u_max_lod = glGetUniformLocation (p.id, "u_max_lod");
}

void
BoardRenderer::setup ()
{
	program.id = hl::gl::compile_program (vs_src, fs_src);
	locate (program);
	program.u_tag = program.u_max_nodes = program.u_opaque_depth = -1;

	{
		std::string src = fs_src;

		src.insert (src.find ('\n') + 1, "#define COLLECT 1\n");
		collect.id = hl::gl::compile_program (vs_src, src.c_str ());
		locate (collect);
		collect.u_tag = glGetUniformLocation (collect.id, "u_tag");
		collect.u_max_nodes = glGetUniformLocation (collect.id, "u_max_nodes");
		collect.u_opaque_depth = glGetUniformLocation (collect.id, "u_opaque_depth");
		oit.resolve = hl::gl::compile_program (resolve_vs_src, resolve_fs_src);
		glGenVertexArrays (1, &oit.vao);
		if (!collect.id || !oit.resolve) {
			HL_ERROR ("naomi: no translucency at each pixel");
			oit.failed = true;
		}
	}
	blit_program = hl::gl::compile_program (blit_vs_src, blit_fs_src);
	u_blit_tex = glGetUniformLocation (blit_program, "u_tex");
	mod_program = hl::gl::compile_program (mod_vs_src, mod_fs_src);
	u_mod_zscale = glGetUniformLocation (mod_program, "u_zscale");
	u_mod_scale = glGetUniformLocation (mod_program, "u_scale");
	u_mod_target = glGetUniformLocation (mod_program, "u_target");

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

	glGenVertexArrays (1, &mod_vao);
	glGenBuffers (1, &mod_vbo);
	glBindVertexArray (mod_vao);
	glBindBuffer (GL_ARRAY_BUFFER, mod_vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 12, (void *) 0);
	glBindVertexArray (0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);

	glGenTextures (1, &fog_tex);
	glBindTexture (GL_TEXTURE_1D, fog_tex);
	glTexParameteri (GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage1D (GL_TEXTURE_1D, 0, GL_R16UI, 128, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
	glBindTexture (GL_TEXTURE_1D, 0);

	glGenTextures (1, &vram_tex);
	glBindTexture (GL_TEXTURE_2D, vram_tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture (GL_TEXTURE_2D, 0);
}

void
BoardRenderer::make_target (Target &t, unsigned w, unsigned h)
{
	if (t.fb && t.w == w && t.h == h)
		return;
	if (!t.fb) {
		glGenFramebuffers (1, &t.fb);
		glGenTextures (1, &t.color);
		glGenTextures (1, &t.depth);
	}
	glBindTexture (GL_TEXTURE_2D, t.color);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture (GL_TEXTURE_2D, t.depth);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, t.fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
	t.w = w;
	t.h = h;
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
BoardRenderer::set_scene_uniforms (const Program &p)
{
	const Draw &d = *draw;

	glUniform1i (p.u_tex, 0);
	glUniform1i (p.u_fog, 1);
	glUniform1f (p.u_zscale, 1.0f / std::max (d.scene->max_z, d.background_z));
	glUniform1f (p.u_width, d.to_texture ? 640.0f : (float) width);
	glUniform1i (p.u_stretch_2d, hl::Config::get ()->widescreen_2d == hl::WIDESCREEN_2D_STRETCHED);
	glUniform2f (p.u_scale, d.scale_x, d.scale_y);
	glUniform2f (p.u_target, pixel_x, pixel_y);
	glUniform1f (p.u_pt_ref, d.pt_alpha_ref / 255.0f);
	glUniform1f (p.u_shade_scale, (d.shad_scale & 255) / 256.0f);
	glUniform3f (p.u_fog_color, ((d.fog_color >> 16) & 255) / 255.0f, ((d.fog_color >> 8) & 255) / 255.0f,
	             (d.fog_color & 255) / 255.0f);
	glUniform3f (p.u_fog_color_vertex, ((d.fog_color_vertex >> 16) & 255) / 255.0f,
	             ((d.fog_color_vertex >> 8) & 255) / 255.0f, (d.fog_color_vertex & 255) / 255.0f);
	glUniform4f (p.u_clamp_max, ((d.clamp_max >> 16) & 255) / 255.0f, ((d.clamp_max >> 8) & 255) / 255.0f,
	             (d.clamp_max & 255) / 255.0f, (d.clamp_max >> 24) / 255.0f);
	glUniform4f (p.u_clamp_min, ((d.clamp_min >> 16) & 255) / 255.0f, ((d.clamp_min >> 8) & 255) / 255.0f,
	             (d.clamp_min & 255) / 255.0f, (d.clamp_min >> 24) / 255.0f);
	{
		int e = (int8_t) (d.fog_density & 255);
		float m = ((d.fog_density >> 8) & 255) / 128.0f;

		glUniform1f (p.u_fog_density, m * powf (2.0f, (float) e));
	}
}

/* A batch, or its part in one of a modifier volume's two areas: its TSP and
 * texture words, its vertices at first, shadowed or not */
void
BoardRenderer::draw_piece (const Program &p, const Batch &b, uint32_t tsp, uint32_t tcw, uint32_t first, bool shadowed,
                           bool opaque)
{
	bool textured = (b.isp >> 25) & 1;
	int flags = 0;

	flags |= (tsp >> 6) & 3;
	flags |= textured ? 4 : 0;
	flags |= ((tsp >> 20) & 1) ? 8 : 0;
	flags |= ((tsp >> 19) & 1) ? 16 : 0;
	flags |= ((b.isp >> 24) & 1) ? 32 : 0;
	flags |= ((tsp >> 22) & 3) << 7;
	flags |= ((tsp >> 21) & 1) ? 512 : 0;
	flags |= shadowed ? 8192 : 0;
	flags |= b.clip_out ? 16384 : 0;
	{
		/* Tri-linear filtering, the PowerVR's way, in two passes of a same
		 * mipmapped polygon added together (FILTER 2 and 3 in the TSP
		 * word); punch-through polygons have no such thing */
		unsigned filter = (tsp >> 13) & 3;

		if (textured && filter > 1 && !punch_flag && is_mipmapped (tcw))
			flags |= filter == 2 ? 2048 : 4096;
	}
	/* The depth pass needs the textures of the punch-through polygons alone */
	if (depth_pass && !punch_flag) {
		flags &= ~4;
		textured = false;
	}
	glUniform1i (p.u_flags, flags | punch_flag);

	if (textured) {
		unsigned filter = (tsp >> 13) & 3;
		bool mip = is_mipmapped (tcw);
		bool clamp_u = (tsp >> 16) & 1, clamp_v = (tsp >> 15) & 1;
		bool flip_u = (tsp >> 18) & 1, flip_v = (tsp >> 17) & 1;

		glBindTexture (GL_TEXTURE_2D, texture (tcw, tsp));
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
		/* One level at a time; tri-linear's two levels are its passes' */
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		                 mip ? (filter ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST) :
		                 (filter ? GL_LINEAR : GL_NEAREST));
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		                 clamp_u ? GL_CLAMP_TO_EDGE : flip_u ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		                 clamp_v ? GL_CLAMP_TO_EDGE : flip_v ? GL_MIRRORED_REPEAT : GL_REPEAT);
		{
			/* The "MIP-map D adjust", a factor on D of two fractional bits */
			unsigned bias = (tsp >> 8) & 15;

			glUniform1f (p.u_lod_bias, bias ? log2f (bias / 4.0f) : 0.0f);
			glUniform1f (p.u_max_lod, (float) log2_of (8u << ((tsp >> 3) & 7)));
		}
	}

	if (p.u_tag >= 0) {
		glUniform1ui (p.u_tag, ((tsp >> 29) & 7) | (((tsp >> 26) & 7) << 3) | (((tsp >> 25) & 1) << 6) |
		              (((tsp >> 24) & 1) << 7) | ((first / 3) << 8));
	} else if (opaque)
		glDisable (GL_BLEND);
	else {
		glEnable (GL_BLEND);
		glBlendFunc (blend_factor (tsp >> 29, true), blend_factor (tsp >> 26, false));
	}
	glDrawArrays (GL_TRIANGLES, first, b.count);
}

void
BoardRenderer::draw_batch (const Program &p, const Draw &d, const Batch &b, bool opaque, float sx, float sy)
{
	if (!b.count)
		return;
	{
		/* In the target's pixels, its origin at the bottom; the tiles are
		 * those before the X and Y scalers, the wide picture centred */
		float ox = (!d.to_texture && width > 640) ? (width - 640) * 0.5f * sx : 0.0f;
		float x0 = ox + b.x0 * d.scale_x * sx, x1 = ox + b.x1 * d.scale_x * sx;
		float y0 = target->h - b.y1 * d.scale_y * sy, y1 = target->h - b.y0 * d.scale_y * sy;

		if (b.clip_on) {
			glEnable (GL_SCISSOR_TEST);
			glScissor ((GLint) x0, (GLint) y0, (GLsizei) (x1 - x0), (GLsizei) (y1 - y0));
		} else
			glDisable (GL_SCISSOR_TEST);
		if (b.clip_out)
			glUniform4f (p.u_clip_out, x0, y0, x1, y1);
	}

	if (p.u_tag < 0) {
		glDepthFunc (depth_func (b.isp >> 29));
		glDepthMask ((!opaque || ((b.isp >> 26) & 1)) ? GL_FALSE : GL_TRUE);
	}
	if (opaque && volumes_ready && b.shadow) {
		/* The two areas: outside, then inside the volumes */
		glStencilFunc (GL_EQUAL, 0, 1);
		draw_piece (p, b, b.tsp, b.tcw, b.first, false, opaque);
		glStencilFunc (GL_EQUAL, 1, 1);
		if (b.two_volume)
			draw_piece (p, b, b.tsp1, b.tcw1, vertices1_at + b.first1, false, opaque);
		else
			draw_piece (p, b, b.tsp, b.tcw, b.first, (draw->shad_scale & 0x100) != 0, opaque);
	} else {
		if (p.u_tag < 0 && !depth_pass)
			glStencilFunc (GL_ALWAYS, 0, 0);
		draw_piece (p, b, b.tsp, b.tcw, b.first, false, opaque);
	}
}

/* The modifier volumes against the depth the opaque and punch-through polygons
 * left: bit 1 of the stencil counts the faces in front of the depth, bit 0 is
 * what the volume covers, the polygons' "area 1" */
void
BoardRenderer::draw_modifiers (const Draw &d, float sx, float sy)
{
	const Scene &scene = *d.scene;

	(void) sx;
	(void) sy;
	glUseProgram (mod_program);
	glUniform1f (u_mod_zscale, 1.0f / std::max (scene.max_z, d.background_z));
	glUniform2f (u_mod_scale, d.scale_x, d.scale_y);
	glUniform2f (u_mod_target, pixel_x, pixel_y);
	glBindVertexArray (mod_vao);
	glBindBuffer (GL_ARRAY_BUFFER, mod_vbo);
	glBufferData (GL_ARRAY_BUFFER, scene.mod_vertices.size () * sizeof (float), scene.mod_vertices.data (), GL_STREAM_DRAW);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask (GL_FALSE);
	glDisable (GL_BLEND);
	glDisable (GL_SCISSOR_TEST);
	glEnable (GL_STENCIL_TEST);
	int base = -1;

	for (const ModVol &m : scene.mod_volumes) {
		unsigned mode = m.isp >> 29;
		/* OR for an open volume or quads, XOR for a closed one */
		bool either = !((m.isp >> 26) & 1) && mode > 0;

		if (!m.count)
			continue;
		glEnable (GL_DEPTH_TEST);
		glDepthFunc (GL_GREATER);
		glStencilMask (2);
		if (base < 0)
			base = (int) m.first;
		if (!either) {
			glStencilFunc (GL_ALWAYS, 0, 2);
			glStencilOp (GL_KEEP, GL_KEEP, GL_INVERT);
		} else {
			glStencilFunc (GL_ALWAYS, 2, 2);
			glStencilOp (GL_REPLACE, GL_KEEP, GL_REPLACE);
		}
		glDrawArrays (GL_TRIANGLES, m.first, m.count);
		if (mode == 1 || mode == 2) {
			glDisable (GL_DEPTH_TEST);
			glStencilMask (3);
			if (mode == 1) {
				glStencilFunc (GL_LEQUAL, 1, 3);
				glStencilOp (GL_ZERO, GL_ZERO, GL_REPLACE);
			} else {
				glStencilFunc (GL_EQUAL, 1, 3);
				glStencilOp (GL_ZERO, GL_ZERO, GL_KEEP);
			}
			/* The volume's sum, over all its pieces */
			glDrawArrays (GL_TRIANGLES, base, m.first + m.count - base);
			base = -1;
		}
	}
	glDisable (GL_DEPTH_TEST);
	glStencilMask (0xFF);
	glDisable (GL_STENCIL_TEST);
}

/* The background polygon: the plane of its three vertices, the depth and the
 * coordinates and colours by 1/W, at the corners of the picture. When its
 * three vertices do not make a plane, its first vertex's values everywhere */
void
BoardRenderer::background_vertices (const Draw &d, Vertex *quad) const
{
	Vertex v[3] = { d.background[0], d.background[1], d.background[2] };
	float extra = (!d.to_texture && width > 640) ? (width - 640) * 0.5f : 0.0f;
	float xs[2] = { -extra / d.scale_x, (pixel_x + extra) / d.scale_x }, ys[2] = { 0.0f, pixel_y / d.scale_y };
	float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
	bool plane = area != 0.0f && std::isfinite (area);
	bool gouraud = (d.background_isp >> 23) & 1;
	unsigned i, k;

	for (i = 0; i < 3; i++)
		if (!(v[i].z > 0.0f) || !std::isfinite (v[i].z))
			v[0].z = v[1].z = v[2].z = std::max (d.background_z, 1e-11f);
	if (!gouraud) {
		v[0].base = v[1].base = v[2].base;
		v[0].offset = v[1].offset = v[2].offset;
	}

	auto corner = [&] (float x, float y) {
		Vertex out = v[0];
		float l[3] = { 1.0f, 0.0f, 0.0f }, z = 0.0f, u = 0.0f, tv = 0.0f, a[4] = {}, b[4] = {};
		unsigned c;

		if (plane) {
			l[1] = ((x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (y - v[0].y)) / area;
			l[2] = ((v[1].x - v[0].x) * (y - v[0].y) - (x - v[0].x) * (v[1].y - v[0].y)) / area;
			l[0] = 1.0f - l[1] - l[2];
		}
		for (k = 0; k < 3; k++) {
			z += l[k] * v[k].z;
			u += l[k] * v[k].u * v[k].z;
			tv += l[k] * v[k].v * v[k].z;
			for (c = 0; c < 4; c++) {
				a[c] += l[k] * ((v[k].base >> (c * 8)) & 255) * v[k].z;
				b[c] += l[k] * ((v[k].offset >> (c * 8)) & 255) * v[k].z;
			}
		}
		out.x = x;
		out.y = y;
		out.z = z > 0.0f ? z : v[0].z;
		out.u = u / out.z;
		out.v = tv / out.z;
		out.base = out.offset = 0;
		for (c = 0; c < 4; c++) {
			out.base |= (uint32_t) std::max (0.0f, std::min (255.0f, a[c] / out.z)) << (c * 8);
			out.offset |= (uint32_t) std::max (0.0f, std::min (255.0f, b[c] / out.z)) << (c * 8);
		}
		return out;
	};
	Vertex tl = corner (xs[0], ys[0]), tr = corner (xs[1], ys[0]), bl = corner (xs[0], ys[1]), br = corner (xs[1], ys[1]);

	quad[0] = tl;
	quad[1] = tr;
	quad[2] = br;
	quad[3] = tl;
	quad[4] = br;
	quad[5] = bl;
}

/* Whether the opaque or punch-through lists have shadowed polygons and volumes
 * to shadow them */
static bool
has_shadows (const Scene &scene)
{
	bool volumes = false;

	for (const ModVol &m : scene.mod_volumes)
		volumes = volumes || m.count;
	if (!volumes)
		return false;
	for (const Batch &b : scene.batches[LIST_OPAQUE])
		if (b.shadow)
			return true;
	for (const Batch &b : scene.batches[LIST_PUNCH])
		if (b.shadow)
			return true;
	return false;
}

void
BoardRenderer::draw_scene (const Draw &d, Target &t, bool to_texture)
{
	const Scene &scene = *d.scene;
	unsigned w, h, i;
	float sx, sy;

	draw = &d;
	target = &t;
	frame_count++;
	glBindFramebuffer (GL_FRAMEBUFFER, t.fb);
	glViewport (0, 0, t.w, t.h);
	glDisable (GL_SCISSOR_TEST);
	glDepthMask (GL_TRUE);
	glStencilMask (0xFF);
	glClearStencil (0);
	glClearColor (((d.background[0].base >> 16) & 255) / 255.0f, ((d.background[0].base >> 8) & 255) / 255.0f,
	              (d.background[0].base & 255) / 255.0f, 1.0f);
	glClearDepth (std::min (1.0, (double) d.background_z / std::max (scene.max_z, d.background_z)));
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glClearDepth (1.0);

	w = to_texture ? t.w : width;
	h = to_texture ? t.h : height;
	pixel_x = to_texture ? (float) t.w : 640.0f;
	pixel_y = to_texture ? (float) t.h : 480.0f;
	sx = (float) t.w / (to_texture ? (float) w : 640.0f);
	sy = (float) t.h / (to_texture ? (float) h : 480.0f);
	if (!to_texture && width > 640)
		sx = (float) t.w / 854.0f;

	/* The background: its plane over the whole picture, after the others */
	Batch background = {};

	background.isp = d.background_isp;
	background.tsp = d.background_tsp;
	background.tcw = d.background_tcw;
	background.shadow = d.background_shadow;
	background.first = (uint32_t) (scene.vertices.size () + scene.vertices1.size ());
	background.count = 6;

	if (scene.vertices.empty () && !((d.background_isp >> 25) & 1))
		return;

	bool per_pixel =!to_texture && !d.presort && hl::Config::get ()->sort_translucent_pixels &&
	                 !oit.failed && oit_buffers (t.w, t.h);

	glUseProgram (program.id);
	glBindVertexArray (vao);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
	vertices1_at = (uint32_t) scene.vertices.size ();
	glBufferData (GL_ARRAY_BUFFER, (scene.vertices.size () + scene.vertices1.size () + 6) * sizeof (Vertex), NULL,
	              GL_STREAM_DRAW);
	glBufferSubData (GL_ARRAY_BUFFER, 0, scene.vertices.size () * sizeof (Vertex), scene.vertices.data ());
	glBufferSubData (GL_ARRAY_BUFFER, scene.vertices.size () * sizeof (Vertex),
	                 scene.vertices1.size () * sizeof (Vertex), scene.vertices1.data ());
	{
		Vertex quad[6];

		background_vertices (d, quad);
		glBufferSubData (GL_ARRAY_BUFFER, background.first * sizeof (Vertex), sizeof (quad), quad);
	}
	set_scene_uniforms (program);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_1D, fog_tex);
	glTexSubImage1D (GL_TEXTURE_1D, 0, 0, 128, GL_RED_INTEGER, GL_UNSIGNED_SHORT, d.fog_table);
	glActiveTexture (GL_TEXTURE0);

	glEnable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);

	volumes_ready = false;
	if (has_shadows (scene)) {
		/* The volumes come after the lists' depth, and decide how these
		 * polygons are shaded: a pass for the depth alone first (the core
		 * has it per tile before it shades them) */
		depth_pass = true;
		glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		punch_flag = 0;
		for (const Batch &b : scene.batches[LIST_OPAQUE])
			draw_batch (program, d, b, true, sx, sy);
		punch_flag = 1024;
		for (const Batch &b : scene.batches[LIST_PUNCH])
			draw_batch (program, d, b, true, sx, sy);
		punch_flag = 0;
		depth_pass = false;
		glDisable (GL_SCISSOR_TEST);
		draw_modifiers (d, sx, sy);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glUseProgram (program.id);
		glBindVertexArray (vao);
		glBindBuffer (GL_ARRAY_BUFFER, vbo);
		glDepthMask (GL_TRUE);
		glClearDepth (std::min (1.0, (double) d.background_z / std::max (scene.max_z, d.background_z)));
		glClear (GL_DEPTH_BUFFER_BIT);
		glClearDepth (1.0);
		glEnable (GL_DEPTH_TEST);
		glStencilMask (0);
		glEnable (GL_STENCIL_TEST);
		volumes_ready = true;
	}

	/* The background polygon goes under everything, at its own depth */
	glDisable (GL_DEPTH_TEST);
	draw_batch (program, d, background, true, sx, sy);
	glEnable (GL_DEPTH_TEST);

	punch_flag = 0;
	for (const Batch &b : scene.batches[LIST_OPAQUE])
		draw_batch (program, d, b, true, sx, sy);
	punch_flag = 1024;
	for (const Batch &b : scene.batches[LIST_PUNCH])
		draw_batch (program, d, b, true, sx, sy);
	punch_flag = 0;

	volumes_ready = false;
	glDisable (GL_STENCIL_TEST);
	glStencilMask (0xFF);
	glDisable (GL_SCISSOR_TEST);

	if (per_pixel)
		draw_translucent_per_pixel ();
	else {
		for (const Batch &b : scene.batches[LIST_TRANSLUCENT])
			draw_batch (program, d, b, false, sx, sy);
	}
	(void) i;

	glDisable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_SCISSOR_TEST);
	glDepthMask (GL_TRUE);
	glDepthFunc (GL_LESS);
	glBindVertexArray (0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	glUseProgram (0);
}

/* A scene drawn to a texture: its pixels in the VRAM, as FB_W_CTRL packs them */
void
BoardRenderer::write_back (const Draw &d, Target &t)
{
	/* A 4x4 ordered dither, added before the low bits go */
	static const uint8_t dither_order[16] = {
		5, 13, 7, 15,
		9, 1, 11, 3,
		6, 14, 4, 12,
		10, 2, 8, 0,
	};
	std::vector<uint8_t> rgba (t.w * t.h * 4);
	hl::Buffer *vram = holly->vram;
	uint32_t base = d.fb_w_sof1 & 0x00FFFFFF, stride = (d.fb_w_linestride & 0x1FF) * 8;
	unsigned pack = d.fb_w_ctrl & 7, x, y;
	bool dither = (d.fb_w_ctrl >> 3) & 1;
	uint32_t kval = (d.fb_w_ctrl >> 8) & 0xFF, alpha_threshold = (d.fb_w_ctrl >> 16) & 0xFF;
	auto dithered = [&] (uint32_t c, unsigned bits, unsigned px, unsigned py) {
		uint32_t level;

		if (!dither)
			return c;
		level = dither_order[(py & 3) * 4 + (px & 3)] >> (4 - bits);
		return std::min (255u, c + level);
	};

	glBindFramebuffer (GL_FRAMEBUFFER, t.fb);
	glReadPixels (0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
	if (!stride)
		stride = t.w * 2;
	for (y = 0; y < t.h; y++) {
		const uint8_t *p = &rgba[(t.h - 1 - y) * t.w * 4];
		uint32_t at = base + y * stride;

		for (x = 0; x < t.w; x++, p += 4) {
			uint32_t r = p[0], g = p[1], b = p[2], a = p[3];

			if (at + 4 > vram->size ())
				return;
			switch (pack) {
			case 0:	/* K, the top of fb_kval */
				r = dithered (r, 3, x, y);
				g = dithered (g, 3, x, y);
				b = dithered (b, 3, x, y);
				vram->put (2, at, (kval >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
				at += 2;
				break;
			case 1:
				r = dithered (r, 3, x, y);
				g = dithered (g, 2, x, y);
				b = dithered (b, 3, x, y);
				vram->put (2, at, (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
				at += 2;
				break;
			case 2:
				r = dithered (r, 4, x, y);
				g = dithered (g, 4, x, y);
				b = dithered (b, 4, x, y);
				a = dithered (a, 4, x, y);
				vram->put (2, at, (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
				at += 2;
				break;
			case 3:	/* A, the comparison with fb_alpha_threshold */
				r = dithered (r, 3, x, y);
				g = dithered (g, 3, x, y);
				b = dithered (b, 3, x, y);
				vram->put (2, at, (a >= alpha_threshold ? 1u : 0u) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
				at += 2;
				break;
			case 4:
				vram->put (1, at, b);
				vram->put (1, at + 1, g);
				vram->put (1, at + 2, r);
				at += 3;
				break;
			case 5:	/* K, fb_kval */
				vram->put (4, at, kval << 24 | r << 16 | g << 8 | b);
				at += 4;
				break;
			default:
				vram->put (4, at, a << 24 | r << 16 | g << 8 | b);
				at += 4;
				break;
			}
		}
	}
}

/* The Y scaler's filter, three lines through Y_COEFF's coefficients, when the
 * picture is scaled down: returns the texture to show */
GLuint
BoardRenderer::filter_lines (const Draw &d)
{
	float c0 = (d.y_coeff & 255) / 256.0f, c1 = ((d.y_coeff >> 8) & 255) / 256.0f;

	if (d.scale_y >= 1.0f || (c0 == 0.0f && c1 == 0.0f))
		return screen.color;
	if (!filter_program) {
		filter_program = hl::gl::compile_program (blit_vs_src, filter_fs_src);
		u_filter_tex = glGetUniformLocation (filter_program, "u_tex");
		u_filter_line = glGetUniformLocation (filter_program, "u_line");
		u_filter_coef = glGetUniformLocation (filter_program, "u_coef");
		glUseProgram (filter_program);
		glUniform1i (glGetUniformLocation (filter_program, "u_flip"), 0);
		glUseProgram (0);
	}
	make_target (filtered, screen.w, screen.h);
	glBindFramebuffer (GL_FRAMEBUFFER, filtered.fb);
	glViewport (0, 0, filtered.w, filtered.h);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_STENCIL_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_SCISSOR_TEST);
	glUseProgram (filter_program);
	glBindVertexArray (blit_vao);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, screen.color);
	glUniform1i (u_filter_tex, 0);
	/* A line of the accumulation buffer, of the picture's height */
	glUniform1f (u_filter_line, d.scale_y / pixel_y);
	glUniform2f (u_filter_coef, c0, c1);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindTexture (GL_TEXTURE_2D, 0);
	glBindVertexArray (0);
	glUseProgram (0);
	return filtered.color;
}

/* No scene for the screen: the framebuffer the game shows, as it is */
bool
BoardRenderer::show_vram ()
{
	uint32_t ctrl = holly->fb_r_ctrl (), size = holly->fb_r_size ();
	unsigned depth = (ctrl >> 2) & 3;
	unsigned bytes = depth == 0 || depth == 1 ? 2 : depth == 2 ? 3 : 4;
	unsigned line = (size & 0x3FF) + 1, lines = ((size >> 10) & 0x3FF) + 1, modulus = (size >> 20) & 0x3FF;
	unsigned w = line * 4 / bytes, x, y;
	uint32_t at = holly->fb_r_sof1 () & 0x00FFFFFC;
	std::vector<uint32_t> rgba;

	if (!(ctrl & 1) || w > 1024 || lines > 1024)
		return false;
	rgba.resize (w * lines);
	for (y = 0; y < lines; y++) {
		for (x = 0; x < w; x++) {
			uint32_t c = 0, p;
			unsigned i;

			for (i = 0; i < bytes; i++)
				c |= (uint32_t) holly->vram->get (1, Holly::map32 ((at + x * bytes + i) & 0x00FFFFFF)) << (8 * i);
			switch (depth) {
			case 0: p = rgba_16 (0, (uint16_t) c) | 0xFF000000u; break;
			case 1: p = rgba_16 (1, (uint16_t) c); break;
			default: p = ((c >> 16) & 255) | (c & 0xFF00) | ((c & 255) << 16) | 0xFF000000u; break;
			}
			rgba[y * w + x] = p;
		}
		at += line * 4 + (modulus ? (modulus - 1) * 4 : 0);
	}
	glBindTexture (GL_TEXTURE_2D, vram_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, lines, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
	glBindTexture (GL_TEXTURE_2D, 0);
	return true;
}

Picture
BoardRenderer::draw_picture ()
{
	std::vector<Draw> draws = holly->take_draws ();
	unsigned scale = (unsigned) std::max (1, hl::Config::get ()->render_scale);
	const Draw *for_screen = NULL;
	Picture picture;

	make_target (screen, width * scale, height * scale);
	for (const Draw &d : draws) {
		if (d.to_texture) {
			unsigned w = ((d.fb_x_clip >> 16) & 0x7FF) + 1, h = ((d.fb_y_clip >> 16) & 0x3FF) + 1;

			w = std::max (1u, std::min (w, 1024u));
			h = std::max (1u, std::min (h, 1024u));
			make_target (offscreen, w, h);
			draw_scene (d, offscreen, true);
			write_back (d, offscreen);
		} else
			for_screen = &d;
	}
	if (for_screen) {
		draw_scene (*for_screen, screen, false);
		shown_color = filter_lines (*for_screen);
		frames_without = 0;
	} else if (frames_without < 1000)
		frames_without++;

	if (frames_without == 0 || frames_without < 4)
		picture.texture = shown_color;
	else if (show_vram ()) {
		picture.texture = vram_tex;
		picture.flip = true;
	}
	draw = NULL;

	if (textures.size () > 4096) {
		for (auto &it : textures)
			glDeleteTextures (1, &it.second.id);
		textures.clear ();
	}
	return picture;
}

void
BoardRenderer::blit (const Picture &p)
{
	if (!p.texture)
		return;
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glUseProgram (blit_program);
	glBindVertexArray (blit_vao);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, p.texture);
	glUniform1i (u_blit_tex, 0);
	glUniform1i (glGetUniformLocation (blit_program, "u_flip"), p.flip ? 1 : 0);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	glUseProgram (0);
	glBindTexture (GL_TEXTURE_2D, 0);
}

Renderer::Renderer (Naomi *naomi_) :
	hl::Renderer (hl::Config::get ()->widescreen ? 854 : 640, 480),
	naomi (naomi_),
	drawers (naomi_->boards.size ())
{
	unsigned across = naomi->multi_screen == hl::MULTI_SIDE_BY_SIDE ? (unsigned) naomi->screen_order.size () : 1;

	hl::set_layout_screens ((int) across);
	hl::Window::set_native_size ((int) (width * across), (int) height);
	outputs.resize (1);
	outputs[0].presentation.reset (new hl::Presentation ());
	if (naomi->multi_screen == hl::MULTI_WINDOWS)
		open_windows ();
	update_titles ();
}

Renderer::~Renderer ()
{
	outputs.clear ();
	drawers.clear ();
	if (wide_fb)
		glDeleteFramebuffers (1, &wide_fb);
	if (wide_color)
		glDeleteTextures (1, &wide_color);
	hl::set_layout_screens (1);
	hl::Window::place (-1, 0, 0);
	hl::Window::set_title_detail ("");
	hl::Window::set_native_size (640, 480);
}

BoardRenderer *
Renderer::drawer (unsigned board)
{
	if (!drawers[board])
		drawers[board].reset (new BoardRenderer (naomi->boards[board]->holly, width, height));
	return drawers[board].get ();
}

/* A window for each screen: on the display its SCREEN_<n>_DISPLAY names, else
 * on the next one there is. Those a same display holds go in a row, in its
 * middle */
void
Renderer::open_windows ()
{
	hl::Config *cfg = hl::Config::get ();
	unsigned n = (unsigned) naomi->screen_order.size (), i, k;
	int count = MAX2 (SDL_GetNumVideoDisplays (), 1);
	int w = (int) width * cfg->window_scale, h = (int) height * cfg->window_scale;
	std::vector<int> on (n);

	for (i = 0; i < n; i++) {
		const char *spec = i < hl::MAX_SCREENS ? cfg->screen_display[i] : "";
		int d = hl::Window::find_display (spec);

		if (d < 0 && spec[0] && strcasecmp (spec, "AUTO"))
			HL_ERROR ("SCREEN_%u_DISPLAY: no display '%s' (they are listed at the start of the log)", i + 1, spec);
		on[i] = d >= 0 ? d : ((int) i < count ? (int) i : 0);
	}
	outputs.resize (n);
	for (i = 0; i < n; i++) {
		unsigned before = 0, same = 0;
		SDL_Rect r = { 0, 0, 1920, 1080 };
		int x, y;

		for (k = 0; k < n; k++)
			if (on[k] == on[i]) {
				same++;
				if (k < i)
					before++;
			}
		SDL_GetDisplayBounds (on[i], &r);
		x = r.x + MAX2 ((r.w - (int) same * w) / 2, 0) + (int) before * w;
		y = r.y + MAX2 ((r.h - h) / 2, 0);
		if (!i)
			hl::Window::place (on[i], x, y);
		else {
			outputs[i].window.reset (new hl::ExtraWindow ("Hikaruloader", on[i], x, y, w, h));
			outputs[i].presentation.reset (new hl::Presentation ());
		}
	}
	SDL_RaiseWindow (hl::Window::sdl_window ());
}

void
Renderer::update_titles ()
{
	std::vector<unsigned> shown = naomi->shown_boards ();
	char text[320];
	size_t i;

	if (naomi->boards.size () < 2)
		return;
	if (naomi->multi_screen == hl::MULTI_SIDE_BY_SIDE && shown.size () > 1) {
		size_t at = 0;

		at += (size_t) snprintf (text, sizeof (text), " - boards");
		for (i = 0; i < shown.size (); i++)
			at += (size_t) snprintf (text + at, sizeof (text) - at, " %u", shown[i] + 1);
	} else
		snprintf (text, sizeof (text), " - board %u", shown[0] + 1);
	hl::Window::set_title_detail (text);
	for (i = 1; i < outputs.size () && i < shown.size (); i++) {
		snprintf (text, sizeof (text), "%s - board %u", naomi->game->name, shown[i] + 1);
		outputs[i].window->set_title (text);
	}
}

/* The boards' pictures in a row, left to right, in one texture */
Picture
Renderer::side_by_side (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures)
{
	unsigned scale = (unsigned) std::max (1, hl::Config::get ()->render_scale);
	unsigned one = width * scale, h = height * scale, w = one * (unsigned) shown.size (), i;
	Picture wide;

	if (!wide_fb) {
		glGenFramebuffers (1, &wide_fb);
		glGenTextures (1, &wide_color);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, wide_fb);
	if (wide_w != w || wide_h != h) {
		glBindTexture (GL_TEXTURE_2D, wide_color);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture (GL_TEXTURE_2D, 0);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wide_color, 0);
		wide_w = w;
		wide_h = h;
	}
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, w, h);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	for (i = 0; i < shown.size (); i++) {
		glViewport ((GLint) (i * one), 0, one, h);
		drawer (shown[i])->blit (pictures[i]);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	wide.texture = wide_color;
	return wide;
}

/* A picture of w x h in a window, as the [Display] settings want it */
void
Renderer::show (Output &out, const Picture &p, BoardRenderer *blitter, unsigned w, unsigned h)
{
	hl::ScreenLayout layout;
	GLuint crt_fb;
	int dw, dh;

	if (out.window) {
		out.window->begin (&dw, &dh);
		hl::Bezel::layout (&layout, dw, dh, (int) w, (int) h);
	} else {
		hl::Window::drawable_size (&dw, &dh);
		hl::Window::layout (&layout);
	}
	out.presentation->copy_view ();
	crt_fb = out.presentation->begin (w, h);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	hl::Window::set_output_viewport (layout, dw, dh);
	if (crt_fb) {
		glDisable (GL_SCISSOR_TEST);
		glBindFramebuffer (GL_FRAMEBUFFER, crt_fb);
		glViewport (0, 0, w, h);
	}
	blitter->blit (p);
	out.presentation->end (0, dw, dh, layout);
	if (out.window)
		out.window->end ();
}

void
Renderer::on_end_frame ()
{
	std::vector<unsigned> shown = naomi->shown_boards ();
	std::vector<Picture> pictures;
	size_t i;

	for (i = 0; i < shown.size (); i++)
		pictures.push_back (drawer (shown[i])->draw_picture ());
	if (naomi->multi_screen == hl::MULTI_SIDE_BY_SIDE && shown.size () > 1) {
		Picture wide = side_by_side (shown, pictures);

		show (outputs[0], wide, drawer (shown[0]), width * (unsigned) shown.size (), height);
		return;
	}
	/* The main one last: its swap, after this, waits for the screen */
	for (i = outputs.size (); i-- > 0;)
		if (i < shown.size ())
			show (outputs[i], pictures[i], drawer (shown[i]), width, height);
}

void
BoardRenderer::destroy_oit ()
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
BoardRenderer::oit_buffers (unsigned w, unsigned h)
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
	glBindFramebuffer (GL_FRAMEBUFFER, target->fb);

	glGenBuffers (1, &oit.nodes);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, oit.nodes);
	glBufferData (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr) oit.max_nodes * 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
	glGenBuffers (1, &oit.counter);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, oit.counter);
	glBufferData (GL_ATOMIC_COUNTER_BUFFER, sizeof (zero), &zero, GL_DYNAMIC_DRAW);
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
	if (!ok || glGetError () != GL_NO_ERROR) {
		HL_ERROR ("naomi: no room for the translucency's %u fragments; each triangle sorted whole",
		          oit.max_nodes);
		destroy_oit ();
		oit.failed = true;
		return false;
	}
	oit.w = w;
	oit.h = h;
	HL_PRINT ("naomi: translucency sorted at each pixel: %ux%u, %u fragments (%u MB)",
	          w, h, oit.max_nodes, (unsigned) ((uint64_t) oit.max_nodes * 16 >> 20));
	return true;
}

/* The translucent batches, sorted at each pixel (see "Translucency") */
void
BoardRenderer::draw_translucent_per_pixel ()
{
	uint32_t zero = 0;

	/* The opaque polygons' depth, for the collecting shader */
	glBindFramebuffer (GL_READ_FRAMEBUFFER, target->fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, oit.depth_fb);
	glBlitFramebuffer (0, 0, oit.w, oit.h, 0, 0, oit.w, oit.h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, target->fb);

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
	for (const Batch &b : draw->scene->batches[LIST_TRANSLUCENT])
		draw_batch (collect, *draw, b, false, pixel_scale_x (), pixel_scale_y ());
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* The picture drawn so far, which the lists are blended over */
	glBindFramebuffer (GL_READ_FRAMEBUFFER, target->fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, oit.color_fb);
	glBlitFramebuffer (0, 0, oit.w, oit.h, 0, 0, oit.w, oit.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, target->fb);
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
			HL_PRINT ("naomi: translucency: %u fragments for %u places, more made", used, oit.max_nodes);
			unsigned w = oit.w, h = oit.h;
			destroy_oit ();
			oit.max_nodes = std::min (std::max (oit.max_nodes * 2, used + used / 4), OIT_MAX_NODES);
			oit.w = w;
			oit.h = h;
		}
	}
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, 0);
}

} /* namespace naomi */
