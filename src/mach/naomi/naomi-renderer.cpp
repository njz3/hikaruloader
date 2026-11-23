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
#include <array>
#include <new>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

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
 * the far plane's 1e-6 (Smashing Drive's sky, the pictures). A game with its
 * widescreen codes draws its picture 16:9 squeezed into 640: all of it
 * stretched to the whole width (u_stretch_all), its texts too (some of its
 * panels, Initial D's, go through its view and are squeezed with the 3D) */
static const char *vs_src = R"(#version 430
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_base;
layout (location = 3) in vec4 in_offset;
layout (location = 4) in uint in_flat;
out vec2 v_uv;
out vec4 v_base;
out vec4 v_offset;
noperspective out float v_z;
/* The vertex's W, the game's: across the turned view's triangles
 * interpolated for its depth (linear in space) */
out float v_w;
uniform float u_width;
uniform int u_stretch_2d;
uniform int u_stretch_all;
uniform vec2 u_scale;
uniform vec2 u_target;
uniform vec4 u_view;
uniform vec2 u_view_c;
uniform float u_view_f;
uniform mat3 u_view_m;
uniform int u_strip_pass;
void main ()
{
	float w = 1.0 / in_pos.z;
	bool flat_2d = in_pos.z >= 1.0 || in_pos.z <= 1.0001e-6;
	/* At 640, and the 2D stretched: as before (the uniform's division
	 * rounds otherwise, a few pixels a frame) */
	float px = in_pos.x * u_scale.x, py = in_pos.y * u_scale.y, pw = 1.0;
	/* MULTI_SCREEN_VIEW: the 3D turned about the eye, as the game's camera
	 * turned so would draw it (u_view_m: the turn, about the vertical but
	 * when the master's camera alone is shaken; u_view: z the game's focal
	 * length, w 1; u_view_c: the view's centre; u_view_f: the focal length
	 * drawn with; in_flat: bit 0 a 2D
	 * triangle's vertex, draw_scene ()); in homogeneous coordinates, pw the
	 * depth along the new axis over the old one, for the GPU to clip what
	 * comes behind */
	if (u_view.w != 0.0 && (in_flat & 1u) == 0u) {
		vec2 d = (vec2 (px, py) - u_view_c) / u_view.z;
		vec3 q = u_view_m * vec3 (d, 1.0);

		pw = q.z;
		px = u_view_c.x * pw + u_view_f * q.x;
		py = u_view_c.y * pw + u_view_f * q.y;
	}
	float x = px / (u_target.x * 0.5) - pw;
	if (u_width > 640.0 && u_stretch_all == 0 && !(u_stretch_2d != 0 && flat_2d))
		x = (px + (u_width - 640.0) * 0.5 * pw) / (u_width * 0.5) - pw;
	gl_Position = vec4 (x, pw - py / (u_target.y * 0.5), 0.0, pw) * w;
	/* MULTI_SCREEN_VIEW's middle board in a side screen's view, for its
	 * strip (View::strips): its HUD away (in_flat 5: 2D at a 1/W of 1 or
	 * more, narrower than the screen), the side screen's own there; its
	 * layers as wide as the screen kept, the side board's alike (F355's
	 * race brightened by a white one at 30%), and its backgrounds */
	if (u_strip_pass != 0 && (in_flat & 7u) == 5u)
		gl_Position = vec4 (2.0, 2.0, 2.0, 1.0);
	v_uv = in_uv;
	v_base = in_base;
	v_offset = in_offset;
	v_z = in_pos.z;
	v_w = w;
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
in float v_w;
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
uniform vec4 u_view;
/* The fragment's 1/W: the turned view's (MULTI_SCREEN_VIEW) from the GPU's
 * clipped and interpolated W */
float frag_z ()
{
	return u_view.w != 0.0 ? gl_FragCoord.w : v_z;
}
/* The fragment's 1/W as the game's view has it, for its fog: the turned
 * view's depth along the screen's axis, 2.9% deeper at the cut between
 * Airline Pilots' boards than along the side board's, fogged it more than
 * the middle board fogs the same point (MULTI_SCREEN_VIEW) */
float game_z ()
{
	return u_view.w != 0.0 ? 1.0 / v_w : v_z;
}
#ifdef COLLECT
layout (binding = 0, offset = 0) uniform atomic_uint u_count;
layout (binding = 0, r32ui) uniform coherent uimage2D u_heads;
layout (std430, binding = 0) buffer Nodes { uvec4 nodes[]; };
uniform uint u_max_nodes;
uniform uint u_tag;
uniform uint u_node_flags;
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
		color.rgb = mix (color.rgb, u_fog_color, fog_table (game_z ()));
	else if (fog == 1 && (u_flags & 32) != 0)
		color.rgb = mix (color.rgb, u_fog_color_vertex, v_offset.a);
	else if (fog == 3)
		color = vec4 (u_fog_color, fog_table (game_z ()));
	/* After the fog: the two passes add up to it once */
	color *= trilinear;
	float depth = frag_z () * u_zscale;
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
		                     next | u_node_flags, u_tag + (uint (gl_PrimitiveID) << 8));
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
 *  - the translucent modifier volumes applied, each to each fragment: the
 *    chip processes them layer by layer, so the area of one pixel differs
 *    from one layer to the next. A polygon the volumes can change is collected
 *    twice, once for each of its areas (NODE_* in the third word): only the
 *    fragments of the area the volumes put them in are blended;
 *  - resolved: a pass over the picture blends each pixel's list over a copy
 *    of what is drawn, the farthest first, with the TSP word's two blending
 *    instructions as the chip has them (the other's colour, the source's or
 *    the destination's alpha), each result rounded to 8 bits. Eight
 *    fragments are put in order at a time, again until the list is done.
 *    The pass empties the heads for the next frame.
 *
 * The buffer, 2 fragments a pixel at first, grows when a frame fills it.
 * Without it, each triangle is sorted whole (sort_translucent ()), and the
 * translucent volumes are not applied: all those polygons are in area 0.
 */

/* The node's third word: next + 1 below, these above */
#define NODE_VOLUMES	0x80000000u	/* the volumes change this polygon */
#define NODE_AREA1	0x40000000u	/* it is its area 1's fragment */
#define NODE_STENCIL	0x20000000u	/* inside the volume being done */
#define NODE_INSIDE	0x10000000u	/* inside the volumes so far */

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
		for (i = head; i != 0u; i = nodes[i - 1u].z & 0x0FFFFFFFu) {
			float d = uintBitsToFloat (nodes[i - 1u].y);
			uint o = nodes[i - 1u].w >> 8, flags = nodes[i - 1u].z;

			/* Of a polygon the volumes change, its fragment of the
			 * area this pixel of that layer is in */
			if ((flags & 0x80000000u) != 0u && ((flags & 0x40000000u) != 0u) != ((flags & 0x10000000u) != 0u))
				continue;
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


/* u_span: the part of the texture's width shown, its start and its width
 * less 1 (0, 0: all of it) */
static const char *blit_vs_src = R"(#version 430
out vec2 v_uv;
uniform int u_flip;
uniform vec2 u_span;
void main ()
{
	vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = vec2 (u_span.x + p.x * (1.0 + u_span.y), u_flip != 0 ? 1.0 - p.y : p.y);
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

/* MULTI_SCREEN_VIEW's flat, curved, dome and Panini pictures (Projection):
 * each pixel's direction through the projection, then the joined panel
 * facing it, whose picture has it where the game's camera turned so puts it.
 * Directions x to the right, y down, z ahead; the panels' textures bottom
 * row first. u_mode: 2 a plane, 3 a cylinder (x its angle, y its height), 4
 * a sphere (x and y its angles), 5 Panini's (d = 1: x is twice the tangent
 * of half the angle, y the height over half of 1 plus its cosine) */
static const char *proj_fs_src = R"(#version 430
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_tex;
uniform int u_mode;
uniform float u_panels;
uniform vec2 u_panel;
uniform float u_focal;
uniform float u_cy;
uniform float u_half;
uniform float u_out_focal;
uniform vec2 u_slot;
void main ()
{
	float wide = u_panels * u_panel.x;
	float x = (mix (u_slot.x, u_slot.y, v_uv.x) - 0.5) * wide / u_out_focal;
	float y = ((1.0 - v_uv.y) * u_panel.y - u_cy) / u_out_focal;
	vec3 d;
	if (u_mode == 2)
		d = vec3 (x, y, 1.0);
	else if (u_mode == 3)
		d = vec3 (sin (x), y, cos (x));
	else if (u_mode == 4)
		d = vec3 (sin (x) * cos (y), sin (y), cos (x) * cos (y));
	else {
		float a = 2.0 * atan (x * 0.5);
		d = vec3 (sin (a), y * (1.0 + cos (a)) * 0.5, cos (a));
	}
	float side = (u_panels - 1.0) * 0.5;
	float k = clamp (floor (atan (d.x, d.z) / (2.0 * u_half) + 0.5), -side, side);
	float t = k * 2.0 * u_half, c = cos (t), s = sin (t);
	float px = d.x * c - d.z * s, pz = d.x * s + d.z * c;
	vec2 p = vec2 (u_panel.x * 0.5 + u_focal * px / pz, u_cy + u_focal * d.y / pz);
	if (pz <= 1e-4 || p.x < 0.0 || p.x > u_panel.x || p.y < 0.0 || p.y > u_panel.y) {
		out_color = vec4 (0.0, 0.0, 0.0, 1.0);
		return;
	}
	out_color = vec4 (texture (u_tex, vec2 (((k + side) * u_panel.x + p.x) / wide, 1.0 - p.y / u_panel.y)).rgb, 1.0);
}
)";

/* MULTI_SCREEN_VIEW's strips (BoardRenderer::fill_strip ()): over a side
 * screen's joined picture, the middle board's scene drawn in that screen's
 * view too (u_tex, u_depth; Picture::strip), in the same pixels: no fade,
 * nothing resampled. Each board leaves out what lies wholly beyond its own
 * view: in the directions the side board's view does not cover (the
 * cabinet's gap between monitors, up to u_edge from the middle view's
 * centre) each board has what the other left out (the side's: Airline
 * Pilots' far hangars, a house of F355's town; the middle's: Airline
 * Pilots' overhead compass). There the two pictures are joined as one depth
 * buffer: the nearer of the two (u_depth, u_side_depth: 1/W by u_units, both
 * along the side screen's axis); where both have the same surface, the
 * middle's up to the cut (u_cut: the direction halfway between the two
 * boards' own views, each board's picture beyond its own screen by half
 * the gap at most, 2.2 degrees in Airline Pilots), both views solved on the
 * vertices the boards both send (falling on each other to a tenth of a
 * pixel in Airline Pilots); where the side board's 2D is (u_mask,
 * View::mask_2d: its text, translucent, has no depth), the side board's.
 * u_turn: the side screen's turn from the middle one; u_focal: both
 * pictures'; u_side_span: the side picture's part shown */
static const char *strip_fs_src = R"(#version 430
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_tex;
uniform sampler2D u_depth;
uniform sampler2D u_side_depth;
uniform vec2 u_units;
uniform vec2 u_side_span;
uniform sampler2D u_mask;
uniform int u_has_mask;
uniform vec2 u_panel;
uniform float u_focal;
uniform float u_turn;
uniform float u_edge;
uniform float u_cut;
void main ()
{
	float x = (v_uv.x - 0.5) * u_panel.x / u_focal;
	float c = cos (u_turn), s = sin (u_turn);
	float X = x * c + s, Z = -x * s + c;
	if (Z <= 1e-4)
		discard;
	float from_middle = abs (u_focal * X / Z);
	if (from_middle >= u_edge)
		discard;
	bool middle = from_middle < u_cut;
	if (u_units.x > 0.0 && u_units.y > 0.0) {
		float side = texture (u_side_depth, vec2 (mix (u_side_span.x, u_side_span.y, v_uv.x), v_uv.y)).r * u_units.y;
		float mid = texture (u_depth, v_uv).r * u_units.x;
		if (side > mid * 1.03 && side > 1e-6)
			middle = false;
		else if (mid > side * 1.03 && mid > 1e-6)
			middle = true;
	}
	/* Where the side board's 2D or what it alone draws is, its pixels:
	 * whole cells */
	if (u_has_mask != 0 && texture (u_mask, vec2 (v_uv.x, 1.0 - v_uv.y)).r > 0.5)
		middle = false;
	if (!middle)
		discard;
	out_color = vec4 (texture (u_tex, v_uv).rgb, 1.0);
}
)";

static const char *mod_vs_src = R"(#version 430
layout (location = 0) in vec3 in_pos;
noperspective out float v_z;
uniform float u_width;
uniform vec2 u_scale;
uniform vec2 u_target;
uniform vec4 u_view;
uniform vec2 u_view_c;
uniform float u_view_f;
uniform mat3 u_view_m;
void main ()
{
	float w = 1.0 / in_pos.z;
	float px = in_pos.x * u_scale.x, py = in_pos.y * u_scale.y, pw = 1.0;
	/* MULTI_SCREEN_VIEW: turned as the polygons' vertex shader turns them */
	if (u_view.w != 0.0) {
		vec2 d = (vec2 (px, py) - u_view_c) / u_view.z;
		vec3 q = u_view_m * vec3 (d, 1.0);

		pw = q.z;
		px = u_view_c.x * pw + u_view_f * q.x;
		py = u_view_c.y * pw + u_view_f * q.y;
	}
	float x = px / (u_target.x * 0.5) - pw;
	/* WIDESCREEN: where the polygons' vertex shader puts them */
	if (u_width > 640.0)
		x = (px + (u_width - 640.0) * 0.5 * pw) / (u_width * 0.5) - pw;
	gl_Position = vec4 (x, pw - py / (u_target.y * 0.5), 0.0, pw) * w;
	v_z = in_pos.z;
}
)";

static const char *mod_fs_src = R"(#version 430
noperspective in float v_z;
out vec4 out_color;
uniform float u_zscale;
uniform vec4 u_view;
void main ()
{
	out_color = vec4 (0.0);
	gl_FragDepth = (u_view.w != 0.0 ? gl_FragCoord.w : v_z) * u_zscale;
}
)";

/* A translucent volume's triangles over each fragment of their pixels, as the
 * opaque volumes' stencil over the depth (draw_modifiers ()), u_mode: 0 a closed
 * volume's face inverts the fragments behind it, 1 an open volume's sets them,
 * 2 and 3 the sum of an inclusion and of an exclusion volume */
static const char *mod_tr_fs_src = R"(#version 430
struct Node { uint color; float depth; uint next; uint tag; };
layout (binding = 0, r32ui) readonly uniform uimage2D u_heads;
layout (std430, binding = 0) coherent buffer Nodes { Node nodes[]; };
noperspective in float v_z;
uniform float u_zscale;
uniform int u_mode;
uniform vec4 u_view;
#define VOLUMES 0x80000000u
#define STENCIL 0x20000000u
#define INSIDE 0x10000000u
void main ()
{
	float depth = (u_view.w != 0.0 ? gl_FragCoord.w : v_z) * u_zscale;
	uint i = imageLoad (u_heads, ivec2 (gl_FragCoord.xy)).r;

	while (i != 0u) {
		uint flags = nodes[i - 1u].next;

		if ((flags & VOLUMES) != 0u) {
			switch (u_mode) {
			case 0:
				if (depth > nodes[i - 1u].depth)
					atomicXor (nodes[i - 1u].next, STENCIL);
				break;
			case 1:
				if (depth > nodes[i - 1u].depth)
					atomicOr (nodes[i - 1u].next, STENCIL);
				break;
			case 2:
				/* inside = inside | volume */
				if ((atomicAnd (nodes[i - 1u].next, ~STENCIL) & STENCIL) != 0u)
					atomicOr (nodes[i - 1u].next, INSIDE);
				break;
			default:
				/* inside = inside & !volume (its result counts inverted:
				 * outside the volume is what stays) */
				if ((atomicAnd (nodes[i - 1u].next, ~STENCIL) & STENCIL) != 0u)
					atomicAnd (nodes[i - 1u].next, ~INSIDE);
				break;
			}
		}
		i = flags & 0x0FFFFFFFu;
	}
}
)";

/*
 * The NAOMI 2's Lighting by the GPU (ELAN_LIGHTING GPU)
 *
 * The Elan's vertices to light come with their scene (Scene::lighting, see
 * naomi-elan.cpp): light_cs_src lights them as Elan::light () does, one
 * vertex an invocation, into their colours; color_cs_src then puts each scene
 * vertex's colours (a lit vertex's, or a clipped point's between two) in the
 * vertex buffer, as pack () makes them. The same operations in the same
 * order, but the GPU's square roots, divisions and powers are not the
 * processor's to the last bit: a colour may come a step (1/255) off.
 */
static const char *light_cs_src = R"(#version 430
layout (local_size_x = 64) in;
struct Light {
	vec4 direction;
	vec4 position;
	vec4 color;
	vec4 atten;
	int flags;
	int routing;
	int dmode;
	int smode;
};
struct LightSet {
	vec4 ambient_base;
	vec4 ambient_offset;
	float gloss;
	uint count;
	uint flags;
	uint pad;
	Light lights[16];
};
struct Lit {
	float pos[3];
	float normal[3];
	float base[4];
	float offset[4];
	uint set;
	uint add;
};
layout (std430, binding = 1) readonly buffer Sets { LightSet sets[]; };
layout (std430, binding = 2) readonly buffer Lits { Lit lit[]; };
layout (std430, binding = 3) writeonly buffer Colors { vec4 colors[]; };
uniform uint u_count;

float clamp01 (float f)
{
	return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
}

float power01 (float x, float gloss)
{
	return x <= 0.0 ? 0.0 : x >= 1.0 ? 1.0 : pow (x, gloss);
}

void main ()
{
	uint i = gl_GlobalInvocationID.x;

	if (i >= u_count)
		return;

	uint s = lit[i].set;
	vec3 pos = vec3 (lit[i].pos[0], lit[i].pos[1], lit[i].pos[2]);
	vec3 normal = vec3 (lit[i].normal[0], lit[i].normal[1], lit[i].normal[2]);
	vec4 base = vec4 (lit[i].base[0], lit[i].base[1], lit[i].base[2], lit[i].base[3]);
	vec4 offset = vec4 (lit[i].offset[0], lit[i].offset[1], lit[i].offset[2], lit[i].offset[3]);
	vec3 diffuse = vec3 (0.0), specular = vec3 (0.0);
	float diffuse_alpha = 0.0, specular_alpha = 0.0;
	vec3 eye = pos;
	float len = sqrt (dot (eye, eye));

	if (len > 0.0)
		eye /= len;

	float d = 2.0 * dot (normal, eye);
	vec3 reflected = eye - d * normal;
	/* No specular colour: its terms left out, as Elan::light () */
	bool no_offset = offset.r == 0.0 && offset.g == 0.0 && offset.b == 0.0;
	uint count = sets[s].count;
	float gloss = sets[s].gloss;

	for (uint k = 0u; k < count; k++) {
		Light l = sets[s].lights[k];
		bool parallel = l.direction.w != 0.0;
		vec3 t = parallel ? l.direction.xyz : l.position.xyz - pos;
		float dist = sqrt (dot (t, t));
		float kd = 1.0, ka = 1.0;

		if (!parallel && dist > 0.0)
			t /= dist;
		/* By the distance, then the angle: a point's light */
		if (!parallel && (l.flags & 4) != 0)
			kd = clamp01 (l.atten.y * (l.position.w != 0.0 ? dist : 1.0 / dist) + l.atten.x);
		if (!parallel && (l.flags & 8) != 0) {
			float to_axis = dot (t, l.direction.xyz);

			ka = clamp01 ((1.0 - (0.0 < to_axis ? to_axis : 0.0)) * l.atten.w + l.atten.z);
		}

		vec3 color = l.color.rgb * kd * ka;
		float to_normal = dot (normal, t);
		float to_reflected = dot (t, reflected);
		/* ROUTE_SUB, BASE_FACTOR */
		float polarity = (l.routing & 8) != 0 ? -2.0 : 2.0;

		if ((l.flags & 1) != 0) {
			float factor = polarity;

			if (l.dmode == 0)
				factor *= max (to_normal, 0.0);
			else if (l.dmode == 1)
				factor *= abs (to_normal);
			if ((l.routing & 4) != 0)
				diffuse_alpha += color.r * factor;
			else if ((l.routing & 2) != 0)
				specular += color * factor * base.rgb;
			else
				diffuse += color * factor * base.rgb;
		}
		if ((l.flags & 2) != 0 && !(no_offset && (l.routing & 4) == 0)) {
			float factor = polarity;

			if (l.smode == 0)
				factor *= clamp01 (power01 (max (to_reflected, 0.0), gloss));
			else if (l.smode == 1)
				factor *= clamp01 (power01 (abs (to_reflected), gloss));
			if ((l.routing & 4) != 0)
				specular_alpha += color.r * factor;
			else if ((l.routing & 1) != 0)
				specular += color * factor * offset.rgb;
			else
				diffuse += color * factor * offset.rgb;
		}
	}

	uint f = sets[s].flags;

	diffuse += sets[s].ambient_base.rgb * ((f & 2u) != 0u ? base.rgb : vec3 (1.0));
	specular += sets[s].ambient_offset.rgb * ((f & 4u) != 0u ? offset.rgb : vec3 (1.0));
	base = vec4 (diffuse, base.a + diffuse_alpha);
	offset = vec4 (specular, offset.a + specular_alpha);
	/* What is over 1 in the base goes to the offset */
	if ((f & 1u) != 0u)
		offset += max (base - 1.0, 0.0);
	base = clamp (base, 0.0, 1.0);
	offset = clamp (offset, 0.0, 1.0);
	if (lit[i].add != 0u)
		base += offset;
	colors[2u * i] = base;
	colors[2u * i + 1u] = offset;
}
)";

static const char *color_cs_src = R"(#version 430
layout (local_size_x = 64) in;
struct Clip {
	int a;
	int b;
	float t;
	float base[8];
	float offset[8];
	float out_base[4];
	float out_offset[4];
};
struct Ref {
	uint at;
	int ref;
};
layout (std430, binding = 3) readonly buffer Colors { vec4 colors[]; };
layout (std430, binding = 4) readonly buffer Clips { Clip clips[]; };
layout (std430, binding = 5) readonly buffer Refs { Ref refs[]; };
/* The vertex buffer, 7 words a vertex (Vertex): base the sixth, offset the
 * seventh */
layout (std430, binding = 6) buffer Vertices { uint words[]; };
uniform uint u_count;

uint to_byte (float f)
{
	return f <= 0.0 ? 0u : f >= 1.0 ? 255u : uint (f * 255.0 + 0.5);
}

uint pack_color (vec4 c)
{
	return (to_byte (c.a) << 24) | (to_byte (c.r) << 16) | (to_byte (c.g) << 8) | to_byte (c.b);
}

void main ()
{
	uint i = gl_GlobalInvocationID.x;

	if (i >= u_count)
		return;

	Ref r = refs[i];
	vec4 base, offset;

	if (r.ref >= 0) {
		base = colors[2 * r.ref];
		offset = colors[2 * r.ref + 1];
	} else {
		int k = -2 - r.ref;
		int ends[2] = int[2] (clips[k].a, clips[k].b);
		vec4 b[2], o[2];

		for (int e = 0; e < 2; e++) {
			if (ends[e] >= 0) {
				b[e] = colors[2 * ends[e]];
				o[e] = colors[2 * ends[e] + 1];
			} else {
				int at = 4 * e;

				b[e] = vec4 (clips[k].base[at], clips[k].base[at + 1], clips[k].base[at + 2],
				             clips[k].base[at + 3]);
				o[e] = vec4 (clips[k].offset[at], clips[k].offset[at + 1], clips[k].offset[at + 2],
				             clips[k].offset[at + 3]);
			}
		}

		float t = clips[k].t;

		base = b[0] + (b[1] - b[0]) * t;
		offset = o[0] + (o[1] - o[0]) * t;
	}
	words[r.at * 7u + 5u] = pack_color (base);
	words[r.at * 7u + 6u] = pack_color (offset);
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
	glDeleteBuffers (5, light_buffers);
	hl::gl::destroy_program (light_program);
	hl::gl::destroy_program (color_program);
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
	p.u_stretch_all = glGetUniformLocation (p.id, "u_stretch_all");
	p.u_clamp_max = glGetUniformLocation (p.id, "u_clamp_max");
	p.u_clamp_min = glGetUniformLocation (p.id, "u_clamp_min");
	p.u_scale = glGetUniformLocation (p.id, "u_scale");
	p.u_target = glGetUniformLocation (p.id, "u_target");
	p.u_pt_ref = glGetUniformLocation (p.id, "u_pt_ref");
	p.u_shade_scale = glGetUniformLocation (p.id, "u_shade_scale");
	p.u_clip_out = glGetUniformLocation (p.id, "u_clip_out");
	p.u_lod_bias = glGetUniformLocation (p.id, "u_lod_bias");
	p.u_max_lod = glGetUniformLocation (p.id, "u_max_lod");
	p.u_view = glGetUniformLocation (p.id, "u_view");
	p.u_view_c = glGetUniformLocation (p.id, "u_view_c");
	p.u_view_f = glGetUniformLocation (p.id, "u_view_f");
	p.u_view_m = glGetUniformLocation (p.id, "u_view_m");
	p.u_strip_pass = glGetUniformLocation (p.id, "u_strip_pass");
}

void
BoardRenderer::setup ()
{
	program.id = hl::gl::compile_program (vs_src, fs_src);
	locate (program);
	program.u_tag = program.u_max_nodes = program.u_opaque_depth = program.u_node_flags = -1;

	{
		std::string src = fs_src;

		src.insert (src.find ('\n') + 1, "#define COLLECT 1\n");
		collect.id = hl::gl::compile_program (vs_src, src.c_str ());
		locate (collect);
		collect.u_tag = glGetUniformLocation (collect.id, "u_tag");
		collect.u_max_nodes = glGetUniformLocation (collect.id, "u_max_nodes");
		collect.u_opaque_depth = glGetUniformLocation (collect.id, "u_opaque_depth");
		collect.u_node_flags = glGetUniformLocation (collect.id, "u_node_flags");
		oit.resolve = hl::gl::compile_program (resolve_vs_src, resolve_fs_src);
		oit.u_below = glGetUniformLocation (oit.resolve, "u_below");
		oit.volumes = hl::gl::compile_program (mod_vs_src, mod_tr_fs_src);
		oit.u_mode = glGetUniformLocation (oit.volumes, "u_mode");
		oit.u_zscale = glGetUniformLocation (oit.volumes, "u_zscale");
		oit.u_width = glGetUniformLocation (oit.volumes, "u_width");
		oit.u_scale = glGetUniformLocation (oit.volumes, "u_scale");
		oit.u_view = glGetUniformLocation (oit.volumes, "u_view");
		oit.u_view_c = glGetUniformLocation (oit.volumes, "u_view_c");
		oit.u_view_f = glGetUniformLocation (oit.volumes, "u_view_f");
		oit.u_view_m = glGetUniformLocation (oit.volumes, "u_view_m");
		oit.u_target = glGetUniformLocation (oit.volumes, "u_target");
		glGenVertexArrays (1, &oit.vao);
		if (!oit.volumes)
			HL_ERROR ("naomi: no translucent modifier volumes");
		if (!collect.id || !oit.resolve) {
			HL_ERROR ("naomi: no translucency at each pixel");
			oit.failed = true;
		}
	}
	blit_program = hl::gl::compile_program (blit_vs_src, blit_fs_src);
	u_blit_tex = glGetUniformLocation (blit_program, "u_tex");
	u_blit_flip = glGetUniformLocation (blit_program, "u_flip");
	u_blit_span = glGetUniformLocation (blit_program, "u_span");
	mod_program = hl::gl::compile_program (mod_vs_src, mod_fs_src);
	u_mod_zscale = glGetUniformLocation (mod_program, "u_zscale");
	u_mod_width = glGetUniformLocation (mod_program, "u_width");
	u_mod_scale = glGetUniformLocation (mod_program, "u_scale");
	u_mod_target = glGetUniformLocation (mod_program, "u_target");
	u_mod_view = glGetUniformLocation (mod_program, "u_view");
	u_mod_view_c = glGetUniformLocation (mod_program, "u_view_c");
	u_mod_view_f = glGetUniformLocation (mod_program, "u_view_f");
	u_mod_view_m = glGetUniformLocation (mod_program, "u_view_m");
	proj_program = hl::gl::compile_program (blit_vs_src, proj_fs_src);
	u_proj_tex = glGetUniformLocation (proj_program, "u_tex");
	u_proj_mode = glGetUniformLocation (proj_program, "u_mode");
	u_proj_panels = glGetUniformLocation (proj_program, "u_panels");
	u_proj_panel = glGetUniformLocation (proj_program, "u_panel");
	u_proj_focal = glGetUniformLocation (proj_program, "u_focal");
	u_proj_cy = glGetUniformLocation (proj_program, "u_cy");
	u_proj_half = glGetUniformLocation (proj_program, "u_half");
	u_proj_out_focal = glGetUniformLocation (proj_program, "u_out_focal");
	u_proj_slot = glGetUniformLocation (proj_program, "u_slot");
	if (!proj_program)
		HL_ERROR ("naomi: no MULTI_SCREEN_VIEW projection");
	strip_program = hl::gl::compile_program (blit_vs_src, strip_fs_src);
	u_strip_tex = glGetUniformLocation (strip_program, "u_tex");
	u_strip_panel = glGetUniformLocation (strip_program, "u_panel");
	u_strip_focal = glGetUniformLocation (strip_program, "u_focal");
	u_strip_turn = glGetUniformLocation (strip_program, "u_turn");
	u_strip_edge = glGetUniformLocation (strip_program, "u_edge");
	u_strip_cut = glGetUniformLocation (strip_program, "u_cut");
	u_strip_depth = glGetUniformLocation (strip_program, "u_depth");
	u_strip_side_depth = glGetUniformLocation (strip_program, "u_side_depth");
	u_strip_units = glGetUniformLocation (strip_program, "u_units");
	u_strip_side_span = glGetUniformLocation (strip_program, "u_side_span");
	u_strip_mask = glGetUniformLocation (strip_program, "u_mask");
	u_strip_has_mask = glGetUniformLocation (strip_program, "u_has_mask");

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
	/* MULTI_SCREEN_VIEW's 2D marks, on while the view turns (draw_scene ()) */
	glGenBuffers (1, &flag_vbo);
	glBindBuffer (GL_ARRAY_BUFFER, flag_vbo);
	glVertexAttribIPointer (4, 1, GL_UNSIGNED_BYTE, 1, (void *) 0);
	glVertexAttribI4ui (4, 0, 0, 0, 0);

	/* ELAN_LIGHTING GPU's (light_scene ()) */
	light_program = hl::gl::compile_compute (light_cs_src);
	color_program = hl::gl::compile_compute (color_cs_src);
	u_light_count = glGetUniformLocation (light_program, "u_count");
	u_color_count = glGetUniformLocation (color_program, "u_count");
	glGenBuffers (5, light_buffers);
	if (light_program && color_program)
		gpu_lighting = true;
	else {
		if (hl::Config::get ()->elan_lighting == hl::ELAN_LIGHTING_GPU)
			HL_ERROR ("naomi: the NAOMI 2's vertices lit by the processor (ELAN_LIGHTING GPU needs OpenGL 4.3)");
		gpu_lighting = false;
	}

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
	glUniform1i (p.u_stretch_2d, hl::Config::get ()->widescreen_2d == hl::WIDESCREEN_2D_STRETCHED && width > 640);
	glUniform1i (p.u_strip_pass, strip_pass ? 1 : 0);
	glUniform1i (p.u_stretch_all, d.stretch_all);
	glUniform2f (p.u_scale, d.scale_x, d.scale_y);
	glUniform2f (p.u_target, pixel_x, pixel_y);
	glUniform1f (p.u_pt_ref, d.pt_alpha_ref / 255.0f);
	glUniform1f (p.u_shade_scale, (d.shad_scale & 255) / 256.0f);
	set_view_uniforms (d, p.u_view, p.u_view_c, p.u_view_f, p.u_view_m);
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

/* The view (MULTI_SCREEN_VIEW): turned for a scene to the screen, not one to
 * a texture, nor one the widescreen codes squeeze */
bool
BoardRenderer::view_on (const Draw &d) const
{
	return view.focal > 0.0f && (view.turn != 0.0f || (view.out_focal > 0.0f && view.out_focal != view.focal)) &&
	       !d.to_texture && !d.stretch_all;
}

void
BoardRenderer::set_view_uniforms (const Draw &d, GLint u_view, GLint u_view_c, GLint u_view_f, GLint u_view_m) const
{
	bool on = view_on (d);
	float c = cosf (view.turn), s = sinf (view.turn);
	const float yaw[9] = { c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c };

	glUniform4f (u_view, c, s, view.focal, on ? 1.0f : 0.0f);
	glUniform2f (u_view_c, 320.0f, view.centre_y);
	glUniform1f (u_view_f, view.out_focal > 0.0f ? view.out_focal : view.focal);
	glUniformMatrix3fv (u_view_m, 1, GL_TRUE, view.rotated ? view.rot : yaw);
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
		/* The place in the lists: the same for a polygon's two areas */
		glUniform1ui (p.u_tag, ((tsp >> 29) & 7) | (((tsp >> 26) & 7) << 3) | (((tsp >> 25) & 1) << 6) |
		              (((tsp >> 24) & 1) << 7) | ((b.first / 3) << 8));
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

		/* WIDESCREEN: a clip as wide as the screen leaves its sides to the
		 * 3D drawn beyond (Strike Fighter clips its whole view so) */
		if (ox > 0.0f && b.x0 == 0 && b.x1 * d.scale_x >= 640.0f) {
			x0 = 0.0f;
			x1 = (float) target->w;
		}

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
	} else if (!opaque && p.u_tag >= 0 && collect_areas && b.shadow &&
	           (b.two_volume || (draw->shad_scale & 0x100))) {
		/* Both areas into the lists: the volumes will choose, layer by layer */
		glUniform1ui (p.u_node_flags, NODE_VOLUMES);
		draw_piece (p, b, b.tsp, b.tcw, b.first, false, opaque);
		glUniform1ui (p.u_node_flags, NODE_VOLUMES | NODE_AREA1);
		if (b.two_volume)
			draw_piece (p, b, b.tsp1, b.tcw1, vertices1_at + b.first1, false, opaque);
		else
			draw_piece (p, b, b.tsp, b.tcw, b.first, true, opaque);
		glUniform1ui (p.u_node_flags, 0);
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
	/* Stretched with the picture (640: the whole width) */
	glUniform1f (u_mod_width, d.to_texture || d.stretch_all ? 640.0f : (float) width);
	glUniform2f (u_mod_scale, d.scale_x, d.scale_y);
	glUniform2f (u_mod_target, pixel_x, pixel_y);
	set_view_uniforms (d, u_mod_view, u_mod_view_c, u_mod_view_f, u_mod_view_m);
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

/* The translucent modifier volumes, over the fragments collected: the same
 * passes as draw_modifiers (), each fragment for a stencil (see "Translucency") */
void
BoardRenderer::draw_translucent_volumes ()
{
	const Draw &d = *draw;
	const Scene &scene = *d.scene;
	int base = -1;

	glUseProgram (oit.volumes);
	glUniform1f (oit.u_zscale, 1.0f / std::max (scene.max_z, d.background_z));
	glUniform1f (oit.u_width, d.to_texture || d.stretch_all ? 640.0f : (float) width);
	glUniform2f (oit.u_scale, d.scale_x, d.scale_y);
	glUniform2f (oit.u_target, pixel_x, pixel_y);
	set_view_uniforms (d, oit.u_view, oit.u_view_c, oit.u_view_f, oit.u_view_m);
	glBindVertexArray (mod_vao);
	glBindBuffer (GL_ARRAY_BUFFER, mod_vbo);
	glBufferData (GL_ARRAY_BUFFER, scene.mod_vertices.size () * sizeof (float), scene.mod_vertices.data (), GL_STREAM_DRAW);
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_STENCIL_TEST);
	for (const ModVol &m : scene.mod_volumes_tr) {
		unsigned mode = m.isp >> 29;
		bool either = !((m.isp >> 26) & 1) && mode > 0;

		if (!m.count)
			continue;
		if (base < 0)
			base = (int) m.first;
		glUniform1i (oit.u_mode, either ? 1 : 0);
		glDrawArrays (GL_TRIANGLES, m.first, m.count);
		glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT);
		if (mode == 1 || mode == 2) {
			glUniform1i (oit.u_mode, mode == 1 ? 2 : 3);
			glDrawArrays (GL_TRIANGLES, base, m.first + m.count - base);
			glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT);
			base = -1;
		}
	}
}

void
BoardRenderer::light_scene (const Scene &scene)
{
	static_assert (sizeof (Vertex) == 28 && offsetof (Vertex, base) == 20 && offsetof (Vertex, offset) == 24,
	               "color_cs_src writes a Vertex's 7 words");
	static_assert (sizeof (GpuLight) == 80 && sizeof (GpuLightSet) == 1328 && sizeof (LitVertex) == 64 &&
	               sizeof (ClipColor) == 108 && sizeof (ColorRef) == 8, "the shaders' std430");
	const SceneLighting &l = scene.lighting;

	if (l.empty () || !light_program || !color_program)
		return;

	auto upload = [] (GLuint buffer, GLuint binding, size_t bytes, const void *data) {
		glBindBuffer (GL_SHADER_STORAGE_BUFFER, buffer);
		glBufferData (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr) std::max<size_t> (bytes, 16), bytes ? data : NULL,
		              GL_STREAM_DRAW);
		glBindBufferBase (GL_SHADER_STORAGE_BUFFER, binding, buffer);
	};
	upload (light_buffers[0], 1, l.sets.size () * sizeof (GpuLightSet), l.sets.data ());
	upload (light_buffers[1], 2, l.lit.size () * sizeof (LitVertex), l.lit.data ());
	upload (light_buffers[2], 3, l.lit.size () * 32, NULL);
	upload (light_buffers[3], 4, l.clips.size () * sizeof (ClipColor), l.clips.data ());
	upload (light_buffers[4], 5, l.refs.size () * sizeof (ColorRef), l.refs.data ());
	glBindBufferBase (GL_SHADER_STORAGE_BUFFER, 6, vbo);
	if (!l.lit.empty ()) {
		glUseProgram (light_program);
		glUniform1ui (u_light_count, (GLuint) l.lit.size ());
		glDispatchCompute ((GLuint) ((l.lit.size () + 63) / 64), 1, 1);
		glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT);
	}
	glUseProgram (color_program);
	glUniform1ui (u_color_count, (GLuint) l.refs.size ());
	glDispatchCompute ((GLuint) ((l.refs.size () + 63) / 64), 1, 1);
	glMemoryBarrier (GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
	for (GLuint binding = 1; binding <= 6; binding++)
		glBindBufferBase (GL_SHADER_STORAGE_BUFFER, binding, 0);
	glBindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
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

/* A batch drawn in a part of the screen, an inset with a view of its own: its
 * user clip a tile or more narrower than the screen (Sega Strike Fighter's
 * side screens' small views of the plane; its whole view clipped to the
 * screen's width, its demonstrations' to a band of it). MULTI_SCREEN_VIEW
 * leaves it where it is, as the 2D */
static bool
inset (const Batch &b, const Draw &d)
{
	return b.clip_on && (float) (b.x1 - b.x0) * d.scale_x <= 608.0f;
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
	/* The NAOMI 2's vertices lit there (ELAN_LIGHTING GPU) */
	if (!scene.lighting.empty ()) {
		light_scene (scene);
		glUseProgram (program.id);
	}
	/* The view turned (MULTI_SCREEN_VIEW): each triangle's vertices marked
	 * 2D when its 1/W is the same at its three, 1 or more (the HUD's) or
	 * the far plane's (the backgrounds, the background plane): those stay
	 * where they are, the others turn, Airline Pilots' cockpit among them
	 * (3D near the eye, a 1/W up to 90; its boards' cockpits are the same
	 * model, their vertices falling on each other's once turned) */
	glBindBuffer (GL_ARRAY_BUFFER, flag_vbo);
	/* In the middle board's strip pass (View::strips) bit 1 too, a 2D
	 * triangle as wide as the screen, and bit 2, a 2D one at a 1/W of 1 or
	 * more (the HUD's; vs_src) */
	if (view_on (d)) {
		auto mark = [&] (const std::vector<Vertex> &v, size_t at) {
			for (size_t i = 0; i + 2 < v.size (); i += 3) {
				float z = v[i].z;
				uint8_t flat = z == v[i + 1].z && z == v[i + 2].z && (z >= 1.0f || z <= 1.0001e-6f);

				if (flat && strip_pass) {
					float lo = std::min ({ v[i].x, v[i + 1].x, v[i + 2].x }) * d.scale_x;
					float hi = std::max ({ v[i].x, v[i + 1].x, v[i + 2].x }) * d.scale_x;

					flat |= (lo <= 0.5f && hi >= 639.5f ? 2 : 0) | (z >= 1.0f ? 4 : 0);
				}
				flags[at + i] = flags[at + i + 1] = flags[at + i + 2] = flat;
			}
		};

		flags.assign (background.first + 6, strip_pass ? 3 : 1);
		mark (scene.vertices, 0);
		mark (scene.vertices1, vertices1_at);
		/* The insets' 3D as the 2D, left out of the strips as the HUD */
		for (const std::vector<Batch> &list : scene.batches)
			for (const Batch &b : list)
				if (inset (b, d)) {
					std::fill (flags.begin () + b.first, flags.begin () + b.first + b.count, strip_pass ? 5 : 1);
					if (b.two_volume)
						std::fill (flags.begin () + vertices1_at + b.first1,
						           flags.begin () + vertices1_at + b.first1 + b.count, strip_pass ? 5 : 1);
				}
		glBufferData (GL_ARRAY_BUFFER, flags.size (), flags.data (), GL_STREAM_DRAW);
		glEnableVertexAttribArray (4);
	} else
		glDisableVertexAttribArray (4);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
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

/* No scene for the screen: the framebuffer the game shows, as it is, read
 * on the machine's thread */
bool
BoardRenderer::read_vram (Holly *holly, Frame &f)
{
	uint32_t ctrl = holly->fb_r_ctrl (), size = holly->fb_r_size ();
	unsigned depth = (ctrl >> 2) & 3;
	unsigned bytes = depth == 0 || depth == 1 ? 2 : depth == 2 ? 3 : 4;
	unsigned line = (size & 0x3FF) + 1, lines = ((size >> 10) & 0x3FF) + 1, modulus = (size >> 20) & 0x3FF;
	unsigned w = line * 4 / bytes, x, y;
	uint32_t at = holly->fb_r_sof1 () & 0x00FFFFFC;
	std::vector<uint32_t> &rgba = f.vram_rgba;
	const uint8_t *base = (const uint8_t *) holly->vram->ptr (0);
	uint8_t row[4096 + 4];

	f.vram = false;
	if (!(ctrl & 1) || w > 1024 || lines > 1024 || !base || holly->vram->size () < 0x01000000)
		return false;
	rgba.resize (w * lines);
	for (y = 0; y < lines; y++) {
		/* The line's 32-bit words, each whole at its place in the
		 * interleaved banks (Holly::map32) */
		for (x = 0; x < line; x++)
			memcpy (&row[x * 4], base + Holly::map32 ((at + x * 4) & 0x00FFFFFC), 4);
		for (x = 0; x < w; x++) {
			const uint8_t *q = &row[x * bytes];
			uint32_t c, p;

			c = bytes == 2 ? (uint32_t) (q[0] | q[1] << 8) :
			    bytes == 3 ? (uint32_t) (q[0] | q[1] << 8 | q[2] << 16) :
			                 (uint32_t) (q[0] | q[1] << 8 | q[2] << 16 | (uint32_t) q[3] << 24);
			switch (depth) {
			case 0: p = rgba_16 (0, (uint16_t) c) | 0xFF000000u; break;
			case 1: p = rgba_16 (1, (uint16_t) c); break;
			default: p = ((c >> 16) & 255) | (c & 0xFF00) | ((c & 255) << 16) | 0xFF000000u; break;
			}
			rgba[y * w + x] = p;
		}
		at += line * 4 + (modulus ? (modulus - 1) * 4 : 0);
	}
	f.vram = true;
	f.vram_w = w;
	f.vram_h = lines;
	return true;
}

bool
BoardRenderer::show_vram (const Frame &f)
{
	if (!f.vram)
		return false;
	glBindTexture (GL_TEXTURE_2D, vram_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, f.vram_w, f.vram_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.vram_rgba.data ());
	glBindTexture (GL_TEXTURE_2D, 0);
	return true;
}

BoardRenderer::Frame
BoardRenderer::take (Holly *holly, unsigned &quiet)
{
	Frame f;

	f.draws = holly->take_draws ();
	for (const Draw &d : f.draws) {
		if (d.to_texture)
			f.to_texture = true;
		else
			f.has_screen = true;
	}
	/* As draw_frame () counts: the framebuffer shown from the fourth */
	if (f.has_screen)
		quiet = 0;
	else if (quiet < 1000)
		quiet++;
	if (!f.has_screen && quiet >= 4) {
		f.rendered = holly->shows_rendered ();
		if (!f.rendered)
			read_vram (holly, f);
	}
	return f;
}

BoardRenderer::Frame
BoardRenderer::take_frame ()
{
	unsigned q = frames_without;

	return take (holly, q);
}

Picture
BoardRenderer::draw_picture (const View &v)
{
	Frame f = take_frame ();

	f.view = v;
	return draw_frame (f);
}

/* A frame drawn; for want of memory (another program took it), none: the
 * picture kept, the emulation going on */
Picture
BoardRenderer::draw_frame (Frame &f)
{
	try {
		return draw_frame_now (f);
	} catch (const std::bad_alloc &) {
		HL_ERROR_FEW ("naomi: no memory left (another program took it?): a frame not drawn");
		return Picture ();
	}
}

Picture
BoardRenderer::draw_frame_now (Frame &f)
{
	std::vector<Draw> &draws = f.draws;
	unsigned scale = (unsigned) std::max (1, hl::Config::get ()->render_scale);
	const Draw *for_screen = NULL;
	Picture picture;

	view = f.view;
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
	bool strips = false;

	if (for_screen) {
		draw_scene (*for_screen, screen, false);
		shown_color = filter_lines (*for_screen);
		shown_depth_unit = std::max (for_screen->scene->max_z, for_screen->background_z);
		frames_without = 0;
		/* MULTI_SCREEN_VIEW's middle board: its scene in the side screens'
		 * views too, for their strips (Picture::strip) */
		if (view.strips && !for_screen->stretch_all) {
			View own = view;

			for (int k = 0; k < 2; k++) {
				view.turn = own.strip_turn[k];
				view.rotated = false;
				strip_pass = true;
				make_target (strip_targets[k], width * scale, height * scale);
				draw_scene (*for_screen, strip_targets[k], false);
				strip_pass = false;
			}
			view = own;
			strips = true;
		}
	} else if (frames_without < 1000)
		frames_without++;

	if (frames_without < 4 || f.rendered) {
		picture.texture = shown_color;
		picture.depth = screen.depth;
		picture.depth_unit = shown_depth_unit;
		for (int k = 0; strips && k < 2; k++) {
			picture.strip[k] = strip_targets[k].color;
			picture.strip_depth[k] = strip_targets[k].depth;
		}
	} else if (show_vram (f)) {
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
	glUniform1i (u_blit_flip, p.flip ? 1 : 0);
	glUniform2f (u_blit_span, p.x0, p.x1 - p.x0 - 1.0f);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	glUseProgram (0);
	glBindTexture (GL_TEXTURE_2D, 0);
}

void
BoardRenderer::fill_strip (const Picture &middle, int k, const Picture &side, const std::vector<uint8_t> *mask_2d,
                           float panel_w, float panel_h, float focal, float turn, float cut, float edge)
{
	bool mask = mask_2d && mask_2d->size () == (size_t) View::MASK_W * View::MASK_H;
	bool depths = middle.strip_depth[k] && side.depth && !side.flip && middle.depth_unit > 0.0f && side.depth_unit > 0.0f;

	if (!middle.strip[k] || !strip_program)
		return;
	glDisable (GL_DEPTH_TEST);
	/* Each pixel the middle picture's or left as the side's (strip_fs_src),
	 * nothing blended */
	glDisable (GL_BLEND);
	glUseProgram (strip_program);
	glBindVertexArray (blit_vao);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, middle.strip[k]);
	glUniform1i (u_strip_tex, 0);
	glUniform2f (u_strip_panel, panel_w, panel_h);
	glUniform1f (u_strip_focal, focal);
	glUniform1f (u_strip_turn, turn);
	glUniform1f (u_strip_edge, edge);
	glUniform1f (u_strip_cut, cut);
	glUniform2f (u_strip_units, depths ? middle.depth_unit : 0.0f, depths ? side.depth_unit : 0.0f);
	glUniform2f (u_strip_side_span, side.x0, side.x1);
	glUniform1i (u_strip_depth, 1);
	glUniform1i (u_strip_side_depth, 2);
	glUniform1i (u_strip_mask, 3);
	glUniform1i (u_strip_has_mask, mask);
	if (mask) {
		glActiveTexture (GL_TEXTURE3);
		if (!strip_mask_tex) {
			glGenTextures (1, &strip_mask_tex);
			glBindTexture (GL_TEXTURE_2D, strip_mask_tex);
			glTexImage2D (GL_TEXTURE_2D, 0, GL_R8, View::MASK_W, View::MASK_H, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		} else
			glBindTexture (GL_TEXTURE_2D, strip_mask_tex);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, View::MASK_W, View::MASK_H, GL_RED, GL_UNSIGNED_BYTE, mask_2d->data ());
		glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
		glActiveTexture (GL_TEXTURE0);
	}
	if (depths) {
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, middle.strip_depth[k]);
		glActiveTexture (GL_TEXTURE2);
		glBindTexture (GL_TEXTURE_2D, side.depth);
		glActiveTexture (GL_TEXTURE0);
	}
	glDrawArrays (GL_TRIANGLES, 0, 3);
	if (mask) {
		glActiveTexture (GL_TEXTURE3);
		glBindTexture (GL_TEXTURE_2D, 0);
		glActiveTexture (GL_TEXTURE0);
	}
	if (depths) {
		glActiveTexture (GL_TEXTURE2);
		glBindTexture (GL_TEXTURE_2D, 0);
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, 0);
		glActiveTexture (GL_TEXTURE0);
	}
	glDisable (GL_BLEND);
	glBindVertexArray (0);
	glUseProgram (0);
	glBindTexture (GL_TEXTURE_2D, 0);
}

void
BoardRenderer::project (const Picture &src, const Projection &pr)
{
	if (!src.texture || !proj_program)
		return;
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glUseProgram (proj_program);
	glBindVertexArray (blit_vao);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, src.texture);
	glUniform1i (u_proj_tex, 0);
	glUniform1i (u_proj_mode, pr.mode);
	glUniform1f (u_proj_panels, (float) pr.panels);
	glUniform2f (u_proj_panel, pr.panel_w, pr.panel_h);
	glUniform1f (u_proj_focal, pr.focal);
	glUniform1f (u_proj_cy, pr.centre_y);
	glUniform1f (u_proj_half, pr.half);
	glUniform1f (u_proj_out_focal, pr.out_focal);
	glUniform2f (u_proj_slot, pr.x0, pr.x1);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	glUseProgram (0);
	glBindTexture (GL_TEXTURE_2D, 0);
}

/* MULTI_SCREEN_VIEW's projections (proj_fs_src): x and y of an edge of the
 * picture over its focal length, the angles they reach, and the height of
 * a direction there over its horizontal distance */
static float
edge_angle (int mode, float x)
{
	switch (mode) {
	case hl::VIEW_FLAT:	return atanf (x);
	case hl::VIEW_PANINI:	return 2.0f * atanf (x * 0.5f);
	default:		return x;
	}
}

static float
height_at (int mode, float y, float angle)
{
	switch (mode) {
	case hl::VIEW_FLAT:	return y * cosf (angle);
	case hl::VIEW_DOME:	return tanf (std::min (y, 1.5f));
	case hl::VIEW_PANINI:	return y * (1.0f + cosf (angle)) * 0.5f;
	default:		return y;
	}
}

/* The picture's rows up to the edge nearest its centre, across its whole
 * width, in the panels' pictures: at each angle the panel facing it shows
 * heights up to its own over its focal length, less at its sides by the
 * cosine of the angle from its middle; the panels together reach panels
 * times half on either side. The smallest focal length that keeps both in,
 * by halves (the edge's angle and height both fall as it grows); a plane
 * stops at 80 degrees */
float
out_focal (const Projection &p)
{
	const float pi = 3.14159265f;
	float rows = std::min (p.centre_y, p.panel_h - p.centre_y);
	float reach = std::min (p.panels * p.half, p.mode == hl::VIEW_FLAT ? 80.0f * pi / 180.0f : pi);
	float half_w = p.panels * p.panel_w * 0.5f;
	auto fits = [&] (float F) {
		float edge = edge_angle (p.mode, half_w / F), y = rows / F;

		if (edge > reach)
			return false;
		/* Each step, and the panels' edges, where they show the least
		 * (a pixel to spare: the texture's filtering) */
		for (int i = 0; i <= 64 + (int) p.panels; i++) {
			float a = i <= 64 ? edge * i / 64.0f : std::min (edge, (2 * (i - 64) - 1) * p.half);
			float k = std::min (floorf (a / (2.0f * p.half) + 0.5f), (p.panels - 1) * 0.5f);
			float shown = (rows - 1.0f) / p.focal * cosf (std::min (fabsf (a - k * 2.0f * p.half), p.half));

			if (height_at (p.mode, y, a) > shown)
				return false;
		}
		return true;
	};
	float lo = p.focal * 0.05f, hi = p.focal * 100.0f;

	for (int i = 0; i < 40; i++) {
		float mid = (lo + hi) * 0.5f;

		if (fits (mid))
			hi = mid;
		else
			lo = mid;
	}
	return hi;
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
	quiet.assign (naomi->boards.size (), 1000);
	if (hl::Config::get ()->threaded_rendering)
		start_thread ();
}

static std::string
display_names ()
{
	std::string s;

	for (unsigned i = 0; i < hl::MAX_SCREENS; i++)
		s += std::string (hl::Config::get ()->screen_display[i]) + ";";
	return s;
}

/* The settings closed: the scenes 854 or 640 wide (WIDESCREEN), the drawing
 * thread idle meanwhile */
void
Renderer::apply_widescreen ()
{
	unsigned w = hl::Config::get ()->widescreen ? 854 : 640, across;

	if (w == width)
		return;
	if (thread)
		wait_thread (false);
	width = w;
	for (auto &d : drawers)
		if (d)
			d->set_width (w);
	if (blitter)
		blitter->set_width (w);
	across = naomi->multi_screen == hl::MULTI_SIDE_BY_SIDE ? (unsigned) naomi->screen_order.size () : 1;
	hl::Window::set_native_size ((int) (width * across), (int) height);
	HL_PRINT ("naomi: the scenes %u wide from now on (WIDESCREEN)", w);
}

/* The settings closed: the windows as MULTI_SCREEN and SCREEN_<n>_DISPLAY now
 * say, without a restart */
void
Renderer::apply_multi_screen ()
{
	int want = naomi->boards.size () > 1 && naomi->screen_order.size () > 1 ?
	           hl::Config::get ()->multi_screen : (int) hl::MULTI_ONE;
	unsigned across;

	if (want == naomi->multi_screen && (want != hl::MULTI_WINDOWS || displays == display_names ()))
		return;
	naomi->multi_screen = want;
	outputs.resize (1);
	hl::Window::place (-1, 0, 0);
	across = want == hl::MULTI_SIDE_BY_SIDE ? (unsigned) naomi->screen_order.size () : 1;
	hl::set_layout_screens ((int) across);
	hl::Window::set_native_size ((int) (width * across), (int) height);
	if (want == hl::MULTI_WINDOWS)
		open_windows ();
	HL_PRINT ("naomi: MULTI_SCREEN %d from now on", want);
	update_titles ();
}

Renderer::~Renderer ()
{
	stop_thread ();
	blitter.reset ();
	outputs.clear ();
	drawers.clear ();
	if (wide_fb)
		glDeleteFramebuffers (1, &wide_fb);
	if (wide_color)
		glDeleteTextures (1, &wide_color);
	if (proj_fb)
		glDeleteFramebuffers (1, &proj_fb);
	if (proj_color)
		glDeleteTextures (1, &proj_color);
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

	displays = display_names ();

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
		snprintf (text + at, sizeof (text) - at, "%s", view_label.c_str ());
	} else
		snprintf (text, sizeof (text), " - board %u%s", shown[0] + 1, view_label.c_str ());
	hl::Window::set_title_detail (text);
	for (i = 1; i < outputs.size () && i < shown.size (); i++) {
		snprintf (text, sizeof (text), "%s - board %u%s", naomi->game->name, shown[i] + 1, view_label.c_str ());
		outputs[i].window->set_title (text);
	}
}

/* The titles say the view the boards are drawn in (MULTI_SCREEN_VIEW), when
 * one is: " (16:9 Panini)"; the one chosen held back in a scene of other
 * cameras (measured_scene ()): " (16:9 Panini: off in this scene)"; again
 * at each change */
void
Renderer::note_views (const std::vector<unsigned> &shown, const std::vector<BoardRenderer::View> &v)
{
	static const char *const names[hl::NUM_SCREEN_VIEWS] = {
		"", "Joined", "Flat", "Curved", "Dome", "Panini"
	};
	int mode = hl::Config::get ()->multi_screen_view;
	const hl::Game *game = naomi->game;
	std::string label;

	if (mode > hl::VIEW_CABINET && mode < hl::NUM_SCREEN_VIEWS && game && game->view_focal > 0.0f &&
	    naomi->multi_screen != hl::MULTI_ONE && shown.size () > 1 && shown == naomi->screen_order) {
		label = std::string (" (") + (width > 640 ? "16:9 " : "4:3 ") + names[mode];
		label += (!v.empty () && v[0].focal > 0.0f) ? ")" : ": off in this scene)";
	}
	if (label != view_label) {
		view_label = label;
		update_titles ();
	}
}

/* The boards' pictures in a row, left to right, in one texture */
Picture
Renderer::side_by_side (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures,
                        BoardRenderer *with)
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
		(with ? with : drawer (shown[i]))->blit (pictures[i]);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	wide.texture = wide_color;
	return wide;
}

/*
 * MULTI_SCREEN_VIEW
 *
 * A deluxe cabinet's boards draw a view each with the same eye, the side
 * ones turned (games.json "screen_view", a game's race: F355 Challenge's
 * side views 87.14 degrees from the middle one, each 80.7 wide in 4:3;
 * Airline Pilots' 45, each 40.6 wide, its turn the side boards' matrix at
 * 0C0F1304; each scene's own solved at each frame, solve_views ()): its
 * monitors' angle and their edges, a gap between flat screens side by side,
 * an overlap once WIDESCREEN widens each. JOINED turns the side
 * views' 3D to meet the middle one's edges, the k-th screen from the middle
 * by k times the field of a screen (twice half), in the vertex shaders (the
 * 3D the games send beyond their 640, as WIDESCREEN shows, fills what the
 * turn brings in); their 2D, the HUD and Airline Pilots' cockpit, stays as
 * it is. The other views take the joined pictures through a projection
 * (proj_fs_src): a plane, a cylinder, a sphere, Panini's; their 2D bent with
 * the 3D.
 */
/* A frame's scene for the screen: its last one (draw_frame_now ()'s) */
static const Draw *
screen_draw (const BoardRenderer::Frame &f)
{
	const Draw *found = NULL;

	for (const Draw &d : f.draws)
		if (!d.to_texture && d.scene)
			found = &d;
	return found;
}

/*
 * The views, each scene's own (F355's race 87.14 degrees between screens with
 * a focal length of 376.7, its driver seen close in its attract 35.57 and
 * 1082.5, the factory 81.19 and 415.7, the rankings 54 and 686.3, the
 * starting grid 1.43 and 27580; Airline
 * Pilots' 45 and 864.7, its centre row 156 in the cockpit, 240 behind the
 * plane), solved at each frame on the points the middle board and a side one
 * both draw. The same vertex of the same model, found by its texture
 * coordinates and colour (once in each board's list), is the same point of
 * the world in each board's view (x, y on its screen, 1/W the inverse of its
 * depth along that view's axis): the side view's turned into the middle one's
 * about the vertical through the eye, it falls on the middle one's. Its
 * depths give the turn t and the focal length f, z_side / z_middle =
 * cos t - sin t (x_side - 320) / f; its rows the centre cy, (y_side - cy)
 * z_middle = (y_middle - cy) z_side. A scene whose side views are not the
 * middle one's turned (Airline Pilots' plane on the ground, each screen its
 * own camera; a turn or a focal length not the same on both sides) leaves
 * most points apart: the views as the cabinet's there
 */
/* A vertex of a triangle the views turn: not the 2D, whose three vertices
 * have the same 1/W, 1 or more or the far plane's (draw_scene ()) */
static bool
is_3d (const std::vector<Vertex> &v, size_t i)
{
	size_t t = i - i % 3;
	float z = v[t].z;

	if (t + 2 >= v.size () || v[i].z <= 1e-6f)
		return false;
	return !(z == v[t + 1].z && z == v[t + 2].z && (z >= 1.0f || z <= 1.0001e-6f));
}

/* The same vertex: at the same place (a vertex of several triangles, in the
 * list once for each) */
static inline bool
same_place (const Vertex &a, const Vertex &b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* A board's textured 3D vertices by their texture coordinates and colour: the
 * index of the one vertex with that key (once, or at the same place each time:
 * Sega Strike Fighter's sky over its title, none of its keys once in a list),
 * -2 when several places have it */
struct KeyTable {
	struct Slot {
		uint64_t key;
		int32_t at;
	};
	std::vector<Slot> slots;
	/* A bit by key: most keys looked for (the side boards' points out of
	 * the middle one's view) not there, told at once */
	std::vector<uint64_t> bits;
	size_t mask = 0;

	/* A vertex's key, 0 for none (not textured, at the far plane); its
	 * triangle's 3D (is_3d ()) */
	static uint64_t key (const Vertex &x)
	{
		uint32_t u, w;

		if ((x.u == 0.0f && x.v == 0.0f) || x.z <= 1e-6f)
			return 0;
		memcpy (&u, &x.u, 4);
		memcpy (&w, &x.v, 4);
		uint64_t k = ((uint64_t) u << 32 | w) ^ ((uint64_t) x.base * 0x9E3779B97F4A7C15ull);
		return k ? k : 1;
	}
	static size_t slot (uint64_t k) { return (size_t) ((k * 0xD6E8FEB86659FD93ull) >> 32); }
	static uint32_t bit (uint64_t k) { return (uint32_t) ((k * 0x9FB21C651E98DF25ull) >> 44); }
	void build (const std::vector<Vertex> &v)
	{
		size_t size = 1024;

		while (size * 2 < v.size () * 3)
			size <<= 1;
		slots.assign (size, Slot { 0, -1 });
		bits.assign ((1u << 20) / 64, 0);
		mask = size - 1;
		for (size_t i = 0; i < v.size (); i++) {
			if (i % 3 == 0 && !is_3d (v, i)) {
				i += 2;
				continue;
			}
			uint64_t k = key (v[i]);
			uint32_t b;
			size_t h;

			if (!k)
				continue;
			b = bit (k);
			bits[b >> 6] |= 1ull << (b & 63);
			for (h = slot (k) & mask; slots[h].at != -1 && slots[h].key != k; h = (h + 1) & mask)
				;
			if (slots[h].at == -1)
				slots[h] = Slot { k, (int32_t) i };
			else if (slots[h].at >= 0 && !same_place (v[slots[h].at], v[i]))
				slots[h].at = -2;
		}
	}
	int32_t find (uint64_t k) const
	{
		uint32_t b = bit (k);
		size_t h;

		if (!((bits[b >> 6] >> (b & 63)) & 1))
			return -1;
		for (h = slot (k) & mask; slots[h].at != -1; h = (h + 1) & mask)
			if (slots[h].key == k)
				return slots[h].at;
		return -1;
	}
};

/* A point both boards draw: x, y, 1/W on the side board's screen, on the
 * middle one's */
struct ViewPair {
	float xs, ys, zs, xm, ym, zm;
};

/* The side view's turn from the middle one, the focal length, the centre row;
 * pairs: the points both have, agree: those it brings on the middle one's,
 * agree_y: their rows too, elsewhere: those of another copy of a model
 * (agreeing ()), out of the count */
struct SideView {
	float turn = 0.0f, focal = 0.0f, cy = 240.0f;
	unsigned pairs = 0, agree = 0, agree_y = 0, elsewhere = 0;
	/* The points on each other in their rows too nearer by four than the
	 * others (solve_side ()) */
	bool near = false;
	unsigned counted () const { return pairs - elsewhere; }
};

/* The pairs a turn and a focal length (from A = cos t, B = -sin t / f) bring
 * on the middle board's points, to a pixel and 1% of their depth (rows: in
 * their rows too, cy the centre row).
 * elsewhere: of the others, those that tell nothing: brought out of the
 * middle board's screen 200 pixels or more from where the middle board has
 * their key, a tenth of the focal length (another copy of the model, 700 to
 * 2400 pixels away; a camera a frame apart some 50: Airline Pilots' plane
 * at the gate, at 4000), the one the side board has out of
 * the middle board's view: Sega Strike Fighter's bomber, a second one by the
 * left board's top edge, 1500 pixels to the left of the middle view where
 * the middle board has the first; its fleet's ships; its ground's tiles, the
 * middle board's 2000 pixels off its other edge; a model in an inset's view
 * of its own); the middle board's a screen or more off its screen (a point
 * by the plane of the eye, 80000 pixels away, each board's to a percent) */
static unsigned
agreeing (const std::vector<ViewPair> &p, double A, double B, std::vector<uint8_t> *mark, float cy = 240.0f,
          unsigned *elsewhere = NULL, bool rows = false)
{
	double s = sqrt (std::max (0.0, 1.0 - A * A)) * (B < 0.0 ? 1.0 : -1.0), f = -s / B;
	unsigned n = 0;

	if (elsewhere)
		*elsewhere = 0;
	for (size_t i = 0; i < p.size (); i++) {
		double Z = 1.0 / p[i].zs, X = (p[i].xs - 320.0) / f * Z;
		double Xm = X * A + Z * s, Zm = -X * s + Z * A;
		bool ok = Zm > 0.0 && fabs (320.0 + f * Xm / Zm - p[i].xm) < 1.0 && fabs (1.0 / Zm - p[i].zm) < 0.01 * p[i].zm &&
		          (!rows || fabs (cy + (p[i].ys - cy) * Z / Zm - p[i].ym) < 1.0);

		n += ok;
		if (mark)
			(*mark)[i] = ok;
		if (ok || !elsewhere)
			continue;
		double x = Zm > 0.0 ? 320.0 + f * Xm / Zm : 0.0, y = Zm > 0.0 ? cy + (p[i].ys - cy) * Z / Zm : 0.0;

		*elsewhere += Zm <= 0.0 ||
		              ((x < 0.0 || x > 640.0 || y < 0.0 || y > 480.0) && hypot (x - p[i].xm, y - p[i].ym) >= std::max (200.0, 0.1 * f)) ||
		              p[i].xm < -640.0f || p[i].xm > 1280.0f || p[i].ym < -480.0f || p[i].ym > 960.0f;
	}
	return n;
}

static SideView
solve_side (const std::vector<ViewPair> &p, float cy_before)
{
	SideView v;
	size_t n = p.size (), i;
	uint32_t seed = 12345;
	auto pick = [&] () {
		seed = seed * 1664525u + 1013904223u;
		return (size_t) ((seed >> 8) % n);
	};
	/* A turn under 90 degrees, a focal length up to a million (F355's
	 * starting grid seen with a long lens: 1.43 degrees and 27580, the side
	 * screens the middle one's picture 690 pixels on, a monitor's 640 and
	 * its frame's 50) */
	auto valid = [] (double A, double B) {
		double f = B != 0.0 ? sqrt (std::max (0.0, 1.0 - A * A)) / fabs (B) : 0.0;
		return A > 0.0 && A < 0.99999 && f > 50.0 && f < 1.0e6;
	};
	double best_A = 0.0, best_B = 0.0;
	unsigned best = 0;

	v.pairs = (unsigned) n;
	if (n < 8)
		return v;
	/* From two pairs at a time, the one most of 128 of them agree with, in
	 * their rows too (the centre row from the pair of depths farther apart),
	 * until nine in ten do: the points on each other in x and in depth alone
	 * some by chance (Sega Strike Fighter's training, its world held a frame
	 * apart by each board while its cockpit is on each other: some of the
	 * world's) */
	static thread_local std::vector<ViewPair> some;
	auto row_of = [&] (const ViewPair &a, const ViewPair &b) {
		const ViewPair &w = fabsf (a.zm - a.zs) / a.zs > fabsf (b.zm - b.zs) / b.zs ? a : b;

		return fabsf (w.zm - w.zs) > 0.02f * w.zs ? (w.ys * w.zm - w.ym * w.zs) / (w.zm - w.zs) : cy_before;
	};
	float best_cy = cy_before;

	some.clear ();
	for (i = 0; i < n; i += std::max<size_t> (1, n / 128))
		some.push_back (p[i]);
	for (int k = 0; k < 200 && best * 10 < some.size () * 9; k++) {
		const ViewPair &a = p[pick ()], &b = p[pick ()];
		double da = a.xs - 320.0, db = b.xs - 320.0;

		if (fabs (da - db) < 10.0)
			continue;
		double ra = a.zs / a.zm, rb = b.zs / b.zm, B = (ra - rb) / (da - db), A = ra - B * da;

		if (!valid (A, B))
			continue;
		float cy = row_of (a, b);
		unsigned m = agreeing (some, A, B, NULL, cy, NULL, true);

		if (m > best) {
			best = m;
			best_A = A;
			best_B = B;
			best_cy = cy;
		}
	}
	if (best < 4)
		return v;
	/* Fitted again on all those that agree */
	std::vector<uint8_t> mark (n);
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, c = 0.0;

	agreeing (p, best_A, best_B, &mark, best_cy, NULL, true);
	for (i = 0; i < n; i++)
		if (mark[i]) {
			double x = p[i].xs - 320.0, y = p[i].zs / p[i].zm;

			sx += x;
			sy += y;
			sxx += x * x;
			sxy += x * y;
			c++;
		}
	double den = c * sxx - sx * sx;

	if (den != 0.0) {
		double B = (c * sxy - sx * sy) / den, A = (sy - B * sx) / c;

		if (valid (A, B) &&
		    agreeing (p, A, B, NULL, best_cy, NULL, true) >= agreeing (p, best_A, best_B, NULL, best_cy, NULL, true)) {
			best_A = A;
			best_B = B;
		}
	}
	/* The centre row from those on each other in their rows then */
	agreeing (p, best_A, best_B, &mark, best_cy, NULL, true);
	double s = sqrt (std::max (0.0, 1.0 - best_A * best_A)) * (best_B < 0.0 ? 1.0 : -1.0);

	v.turn = (float) atan2 (s, best_A);
	v.focal = (float) (-s / best_B);
	/* The centre row: each pair's, of depths apart enough, the median */
	std::vector<float> rows;

	for (i = 0; i < n; i++)
		if (mark[i] && fabsf (p[i].zm - p[i].zs) > 0.02f * p[i].zs)
			rows.push_back ((p[i].ys * p[i].zm - p[i].ym * p[i].zs) / (p[i].zm - p[i].zs));
	v.cy = best_cy;
	if (rows.size () >= 8) {
		std::nth_element (rows.begin (), rows.begin () + rows.size () / 2, rows.end ());
		v.cy = rows[rows.size () / 2];
	}
	v.agree = agreeing (p, best_A, best_B, &mark);
	for (i = 0; i < n; i++)
		v.agree_y += mark[i] && fabsf (v.cy + (p[i].ys - v.cy) * p[i].zm / p[i].zs - p[i].ym) < 1.0f;
	agreeing (p, best_A, best_B, NULL, v.cy, &v.elsewhere);
	/* Near: those on each other in their rows too, the nearest of them four
	 * times nearer at least than the others' middle depth, 16 of them at
	 * least (an eye apart would bring the far points on each other, not
	 * the near ones) */
	std::vector<float> others;
	float nearest = 1e30f;
	unsigned full = agreeing (p, best_A, best_B, &mark, v.cy, NULL, true);

	for (i = 0; i < n; i++)
		if (mark[i])
			nearest = std::min (nearest, 1.0f / p[i].zm);
		else
			others.push_back (1.0f / p[i].zm);
	if (full >= 16 && others.size () >= 8) {
		std::nth_element (others.begin (), others.begin () + others.size () / 2, others.end ());
		v.near = nearest * 4.0f <= others[others.size () / 2];
	}
	return v;
}

/* The middle board's key table of a scene, built once: this frame's and the
 * one before (Renderer::held_middle) kept, each solved against */
static const KeyTable &
middle_keys (const Draw &m)
{
	struct Slot {
		std::shared_ptr<Scene> scene;	/* kept alive: no other at its address */
		KeyTable table;
	};
	static thread_local Slot slots[2];
	static thread_local int last = 0;

	for (int k = 0; k < 2; k++)
		if (slots[k].scene == m.scene) {
			last = k;
			return slots[k].table;
		}
	last ^= 1;
	slots[last].scene = m.scene;
	slots[last].table.build (m.scene->vertices);
	return slots[last].table;
}

/* A board's 3D vertices by a key, a chain each (vote_pairs ()): KIND_UV_COLOUR
 * their texture coordinates and colour (a key many have: a grid's tiles),
 * KIND_COLOUR their colour alone (the texture coordinates each board's own:
 * Sega Strike Fighter's logo, its reflections) */
struct KeyChains {
	enum { BITS = 16, KIND_UV_COLOUR = 0, KIND_COLOUR = 1 };
	std::vector<int32_t> head, next;

	static uint64_t key (const Vertex &x, int kind)
	{
		uint32_t u, w;
		uint64_t k;

		if (x.z <= 1e-6f || x.z >= 1.0f)
			return 0;
		memcpy (&u, &x.u, 4);
		memcpy (&w, &x.v, 4);
		k = kind == KIND_COLOUR ? (uint64_t) x.base + 1 : (((uint64_t) u << 32 | w) ^ ((uint64_t) x.base * 0x9E3779B97F4A7C15ull));
		k *= 0xD6E8FEB86659FD93ull;
		return k ? k : 1;
	}
	void build (const std::vector<Vertex> &v, int kind)
	{
		head.assign ((size_t) 1 << BITS, -1);
		next.assign (v.size (), -1);
		for (size_t i = 0; i + 2 < v.size (); i += 3) {
			if (!is_3d (v, i))
				continue;
			for (size_t j = i; j < i + 3; j++) {
				uint64_t k = key (v[j], kind);

				if (!k)
					continue;
				size_t h = (size_t) (k >> (64 - BITS));

				next[j] = head[h];
				head[h] = (int32_t) j;
			}
		}
	}
};

/* The middle board's chains of a scene, of a kind, built once */
static const KeyChains &
middle_key_chains (const Draw &m, int kind)
{
	static thread_local KeyChains tables[2];
	static thread_local std::shared_ptr<Scene> built[2];	/* kept alive: no other at its address */

	if (built[kind] != m.scene) {
		tables[kind].build (m.scene->vertices, kind);
		built[kind] = m.scene;
	}
	return tables[kind];
}

/* A side board's pairs with the middle one by a vote, when too few keys are
 * once in each board's list (Sega Strike Fighter's grid of tiles, its logo's
 * texture coordinates each board's own): each vertex of the side board's with
 * each of the middle board's of the same key (KeyChains) makes a candidate,
 * a focal length f and a turn t; two views of one eye turned about the
 * vertical through it see a point at depths whose ratio is the cosines' of
 * its angles from their axes: r = z_side / z_middle, f^2 = (dxs^2 - r^2 dxm^2)
 * / (r^2 - 1), t = atan (dxm / f) - atan (dxs / f) (dx: from the centre
 * column). The (f, t) most candidates have (the grid's 3700 of 6700, the
 * others spread), each side vertex's first candidate there; sign: the turn's
 * (the left board's to the left). Its rows tell nothing to the vote:
 * solve_side ()'s centre row checks them */
/* log2 (x) to some 0.005, atan (x) to 1e-5 radians, fast (vote_pairs ()) */
static inline float
fast_log2 (float x)
{
	uint32_t b;
	float m;

	memcpy (&b, &x, 4);
	float e = (float) ((int) (b >> 23) - 127);

	b = (b & 0x7FFFFFu) | 0x3F800000u;
	memcpy (&m, &b, 4);
	return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static inline float
fast_atan (float x)
{
	bool big = fabsf (x) > 1.0f;
	float y = big ? 1.0f / x : x, y2 = y * y;
	float a = y * (0.9998660f + y2 * (-0.3302995f + y2 * (0.1801410f + y2 * (-0.0851330f + y2 * 0.0208351f))));

	return big ? (x > 0.0f ? 1.5707963f : -1.5707963f) - a : a;
}

static bool
vote_pairs (const Draw &d, const Draw &m, float sign, std::vector<ViewPair> &out)
{
	/* Some 128 of the side board's triangles, spread over its list, each
	 * vertex's key's first 1024 of the middle board's */
	enum { NF = 300, NT = 360, MAX_CANDIDATES = 400000, MAX_CHAIN = 1024, TRIANGLES = 128 };
	struct Candidate {
		uint16_t bf, bt;
		uint32_t s, q;
		float f, turn;
	};
	static thread_local std::vector<uint32_t> hist;
	static thread_local std::vector<float> fs, ts;
	static thread_local std::vector<uint32_t> rows;
	static thread_local std::vector<Candidate> cands;
	static thread_local std::unordered_set<uint64_t> used;
	const std::vector<Vertex> &sv = d.scene->vertices, &mv = m.scene->vertices;
	const float lf0 = fast_log2 (100.0f), lf1 = fast_log2 (1.0e5f);
	size_t stride = 3 * std::max<size_t> (1, sv.size () / 3 / TRIANGLES);

	for (int kind = KeyChains::KIND_UV_COLOUR; kind <= KeyChains::KIND_COLOUR; kind++) {
		const KeyChains &mc = middle_key_chains (m, kind);

		hist.assign ((size_t) NF * NT, 0);
		cands.clear ();
		for (size_t i = 0; i + 2 < sv.size () && cands.size () < MAX_CANDIDATES; i += stride) {
			if (!is_3d (sv, i))
				continue;
			for (size_t j = i; j < i + 3; j++) {
				uint64_t k = KeyChains::key (sv[j], kind);
				float xs = sv[j].x * d.scale_x - 320.0f;
				int walked = 0;

				if (!k)
					continue;
				for (int32_t q = mc.head[(size_t) (k >> (64 - KeyChains::BITS))]; q >= 0 && walked < MAX_CHAIN;
				     q = mc.next[q], walked++) {
					const Vertex &t = mv[q];

					if (KeyChains::key (t, kind) != k)
						continue;
					float r = sv[j].z / t.z, r2 = r * r, xm = t.x * m.scale_x - 320.0f;

					if (fabsf (r2 - 1.0f) < 1e-3f)
						continue;
					float f2 = (xs * xs - r2 * xm * xm) / (r2 - 1.0f);

					if (!(f2 > 1.0e4f && f2 < 1.0e10f))
						continue;
					/* atan (xm / f) - atan (xs / f) */
					float f = sqrtf (f2), turn = fast_atan ((xm - xs) * f / (f2 + xm * xs));

					if (f2 + xm * xs < 0.0f)
						turn += turn < 0.0f ? 3.1415927f : -3.1415927f;
					int bf = (int) ((0.5f * fast_log2 (f2) - lf0) / (lf1 - lf0) * NF);
					int bt = (int) ((turn * 57.29578f + 90.0f) * 2.0f);

					if (turn * sign <= 0.0f || bf < 0 || bf >= NF || bt < 0 || bt >= NT)
						continue;
					hist[(size_t) bf * NT + bt]++;
					cands.push_back ({ (uint16_t) bf, (uint16_t) bt, (uint32_t) j, (uint32_t) q, f, turn });
				}
			}
		}
		/* The most, three bins by three */
		unsigned best = 0;
		int pf = 0, pt = 0;

		for (int a = 1; a + 1 < NF; a++)
			for (int b = 1; b + 1 < NT; b++) {
				unsigned n = 0;

				if (!hist[(size_t) a * NT + b])
					continue;
				for (int da = -1; da <= 1; da++)
					for (int db = -1; db <= 1; db++)
						n += hist[(size_t) (a + da) * NT + b + db];
				if (n > best) {
					best = n;
					pf = a;
					pt = b;
				}
			}
		if (best < 8)
			continue;
		/* The peak's focal length and turn, their medians; its centre row,
		 * the one most of its pairs make ((ys - cy) zm = (ym - cy) zs, of
		 * depths apart enough): the vote's pairs on each other in x and in
		 * depth, not all in their rows (Sega Strike Fighter's rings, points
		 * of a colour at the same x and depth up and down each, 60% of
		 * them) */
		fs.clear ();
		ts.clear ();
		rows.assign (960, 0);
		for (const Candidate &c : cands)
			if (abs ((int) c.bf - pf) <= 1 && abs ((int) c.bt - pt) <= 1) {
				const Vertex &s = sv[c.s], &t = mv[c.q];

				fs.push_back (c.f);
				ts.push_back (c.turn);
				if (fabsf (t.z - s.z) > 0.02f * s.z) {
					float cy = (s.y * d.scale_y * t.z - t.y * m.scale_y * s.z) / (t.z - s.z);

					if (cy >= -240.0f && cy < 720.0f)
						rows[(size_t) (cy + 240.0f)]++;
				}
			}
		std::nth_element (fs.begin (), fs.begin () + fs.size () / 2, fs.end ());
		std::nth_element (ts.begin (), ts.begin () + ts.size () / 2, ts.end ());
		float f = fs[fs.size () / 2], turn = ts[ts.size () / 2], cy = 240.0f, cs = cosf (turn), sn = sinf (turn);
		unsigned most = 0;

		for (size_t r = 1; r + 1 < rows.size (); r++)
			if (rows[r - 1] + rows[r] + rows[r + 1] > most) {
				most = rows[r - 1] + rows[r] + rows[r + 1];
				cy = (float) r - 240.0f + 0.5f;
			}
		/* Those on each other then, to two pixels and 2% of their depth;
		 * each side vertex (each place) once */
		out.clear ();
		used.clear ();
		for (const Candidate &c : cands) {
			if (abs ((int) c.bf - pf) > 1 || abs ((int) c.bt - pt) > 1)
				continue;
			const Vertex &s = sv[c.s], &t = mv[c.q];
			float Z = 1.0f / s.z, X = (s.x * d.scale_x - 320.0f) / f * Z, Y = (s.y * d.scale_y - cy) / f * Z;
			float Xm = X * cs + Z * sn, Zm = -X * sn + Z * cs;

			if (Zm <= 1e-6f || fabsf (320.0f + f * Xm / Zm - t.x * m.scale_x) > 2.0f ||
			    fabsf (cy + f * Y / Zm - t.y * m.scale_y) > 2.0f || fabsf (1.0f / Zm - t.z) > 0.02f * t.z)
				continue;
			uint32_t xb, yb, zb;

			memcpy (&xb, &s.x, 4);
			memcpy (&yb, &s.y, 4);
			memcpy (&zb, &s.z, 4);
			if (!used.insert (((uint64_t) xb << 32 | yb) ^ (uint64_t) zb * 0x9E3779B97F4A7C15ull).second)
				continue;
			out.push_back ({ s.x * d.scale_x, s.y * d.scale_y, s.z, t.x * m.scale_x, t.y * m.scale_y, t.z });
			if (out.size () >= 2048)
				break;
		}
		if (out.size () >= 8)
			return true;
	}
	out.clear ();
	return false;
}

/* 3x3 matrices, rows first */
static void
mat3_mul (const double *a, const double *b, double *out)
{
	double r[9];

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
	memcpy (out, r, sizeof (r));
}

/* A turn about the vertical (to the right: x' = x cos t + z sin t) */
static void
mat3_yaw (double t, double *out)
{
	double c = cos (t), s = sin (t);
	const double m[9] = { c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c };

	memcpy (out, m, sizeof (m));
}

/* The master's camera shaken alone (Sega Strike Fighter's crash: its side
 * boards' views turned 90 degrees from each other about their vertical, to
 * 0.02 pixel, the middle one 3.5 degrees in pitch and 3.7 in roll from
 * their middle): the rotation S of the middle view from the rig's middle,
 * the side views the rig's still (turned yaw either way about it, the focal
 * length f, the centre row cy): x_middle = S Ry (+-yaw) x_side, on both
 * sides' pairs at once. Gauss-Newton from no rotation, on the pairs within
 * a distance halved at each step, 150 pixels to one (another copy of a
 * model far off); agree: each side's pairs then on each other to a pixel in
 * x and y and 1% of their depth */
static void
solve_shake_from (const std::vector<ViewPair> *pairs, int sides, float yaw, float f, float cy, double *S, unsigned *agree,
                  unsigned *either)
{
	double R[2][9];
	float thr = 400.0f;

	mat3_yaw (-yaw, R[0]);
	mat3_yaw (yaw, R[1]);
	for (int i = 0; i < 9; i++)
		S[i] = i % 4 == 0 ? 1.0 : 0.0;
	for (int it = 0; it < 12; it++) {
		double JJ[9] = { 0 }, Jr[3] = { 0 };
		unsigned n = 0;

		for (int k = 0; k < 2; k++) {
			double M[9];

			if (!((sides >> k) & 1))
				continue;
			mat3_mul (S, R[k], M);
			for (const ViewPair &p : pairs[k]) {
				double Z = 1.0 / p.zs, P[3] = { (p.xs - 320.0) / f * Z, (p.ys - cy) / f * Z, Z }, u[3];

				for (int i = 0; i < 3; i++)
					u[i] = M[i * 3] * P[0] + M[i * 3 + 1] * P[1] + M[i * 3 + 2] * P[2];
				if (u[2] <= 1e-6)
					continue;
				double px = 320.0 + f * u[0] / u[2], py = cy + f * u[1] / u[2];
				double rx = p.xm - px, ry = p.ym - py;

				if (fabs (rx) > thr || fabs (ry) > thr)
					continue;
				/* The point turned by w x u: d px / d w, d py / d w */
				double a = f / u[2], bx = -f * u[0] / (u[2] * u[2]), by = -f * u[1] / (u[2] * u[2]);
				double jx[3] = { bx * u[1], a * u[2] - bx * u[0], -a * u[1] };
				double jy[3] = { -a * u[2] + by * u[1], -by * u[0], a * u[0] };

				for (int i = 0; i < 3; i++) {
					for (int j = 0; j < 3; j++)
						JJ[i * 3 + j] += jx[i] * jx[j] + jy[i] * jy[j];
					Jr[i] += jx[i] * rx + jy[i] * ry;
				}
				n++;
			}
		}
		if (n < 8)
			break;
		/* JJ w = Jr, by Cramer */
		double det = JJ[0] * (JJ[4] * JJ[8] - JJ[5] * JJ[7]) - JJ[1] * (JJ[3] * JJ[8] - JJ[5] * JJ[6]) +
		             JJ[2] * (JJ[3] * JJ[7] - JJ[4] * JJ[6]);

		if (fabs (det) < 1e-30)
			break;
		double w[3];

		for (int c = 0; c < 3; c++) {
			double m[9];

			memcpy (m, JJ, sizeof (m));
			for (int r = 0; r < 3; r++)
				m[r * 3 + c] = Jr[r];
			w[c] = (m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
			        m[2] * (m[3] * m[7] - m[4] * m[6])) / det;
		}
		/* S = exp ([w]x) S (Rodrigues) */
		double t = sqrt (w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), E[9];

		if (t > 1e-12) {
			double x = w[0] / t, y = w[1] / t, z = w[2] / t, c = cos (t), s = sin (t), v = 1.0 - c;
			const double e[9] = { c + x * x * v, x * y * v - z * s, x * z * v + y * s,
			                      y * x * v + z * s, c + y * y * v, y * z * v - x * s,
			                      z * x * v - y * s, z * y * v + x * s, c + z * z * v };

			memcpy (E, e, sizeof (E));
			mat3_mul (E, S, S);
		}
		thr = std::max (1.0f, thr * 0.5f);
	}
	/* The pairs on each other shaken, and those either shaken or not */
	for (int k = 0; k < 2; k++) {
		double M[9];

		agree[k] = either[k] = 0;
		mat3_mul (S, R[k], M);
		for (const ViewPair &p : pairs[k]) {
			double Z = 1.0 / p.zs, P[3] = { (p.xs - 320.0) / f * Z, (p.ys - cy) / f * Z, Z };
			bool on[2];

			for (int w = 0; w < 2; w++) {
				const double *T = w ? R[k] : M;
				double u[3];

				for (int i = 0; i < 3; i++)
					u[i] = T[i * 3] * P[0] + T[i * 3 + 1] * P[1] + T[i * 3 + 2] * P[2];
				on[w] = u[2] > 1e-6 && fabs (320.0 + f * u[0] / u[2] - p.xm) < 1.0 &&
				        fabs (cy + f * u[1] / u[2] - p.ym) < 1.0 && fabs (1.0 / u[2] - p.zm) < 0.01 * p.zm;
			}
			agree[k] += on[0];
			either[k] += on[0] || on[1];
		}
	}
}

/* solve_shake_from () on both sides, on the left alone, on the right alone:
 * the one whose side the less on each other has the most (Sega Strike
 * Fighter's crash: the right board's points high over its screen on each
 * other unshaken, the master drawing them so, the world below shaken, its
 * left board's all) */
static void
solve_shake (const std::vector<ViewPair> *pairs, float yaw, float f, float cy, double *S, unsigned *agree,
             unsigned *either)
{
	double best = -1.0;

	for (int sides = 3; sides >= 1; sides--) {
		double T[9];
		unsigned a[2], e[2];

		solve_shake_from (pairs, sides, yaw, f, cy, T, a, e);
		double worst = std::min ((double) a[0] / std::max<size_t> (1, pairs[0].size ()),
		                         (double) a[1] / std::max<size_t> (1, pairs[1].size ()));

		if (worst > best) {
			best = worst;
			memcpy (S, T, sizeof (T));
			memcpy (agree, a, sizeof (a));
			memcpy (either, e, sizeof (e));
		}
	}
}

/* Both side views solved, the same turn either way and the same focal
 * length: true; enough: each had points enough (else none to tell) */
bool
Renderer::solve_views (const std::vector<BoardRenderer::Frame> &frames, const Draw *m, float *yaw, float *focal,
                       float *centre, bool *enough) const
{
	/* Fewer 3D triangles on each side screen: bare (views_bare) */
	static const unsigned BARE_MOST = 4;
	static thread_local std::vector<std::pair<uint64_t, uint32_t>> found;
	static thread_local std::vector<ViewPair> pairs[2];
	SideView v[2];

	*enough = false;
	views_shaken = false;
	views_bare = false;
	if (!m || frames.size () != 3)
		return false;
	/* The side screens black but for their 2D (Sega Strike Fighter's jet
	 * flying over the middle screen alone, its attract's): their 3D
	 * triangles on the screen, a pixel at least (the left board's 24 there,
	 * a model folded into the screen's corner, are not) */
	views_bare = true;
	for (int k = 0; k < 2 && views_bare; k++)
		if (const Draw *d = screen_draw (frames[k ? 2 : 0])) {
			const std::vector<Vertex> &sv = d->scene->vertices;
			unsigned n = 0;

			for (size_t i = 0; i + 2 < sv.size () && n < BARE_MOST; i += 3) {
				if (!is_3d (sv, i))
					continue;
				float x[3], y[3];

				for (int j = 0; j < 3; j++) {
					x[j] = sv[i + j].x * d->scale_x;
					y[j] = sv[i + j].y * d->scale_y;
				}
				float area = fabsf ((x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])) * 0.5f;

				if (area >= 1.0f && std::max ({ x[0], x[1], x[2] }) > 0.0f && std::min ({ x[0], x[1], x[2] }) < width &&
				    std::max ({ y[0], y[1], y[2] }) > 0.0f && std::min ({ y[0], y[1], y[2] }) < 480.0f)
					n++;
			}
			views_bare = n < BARE_MOST;
		}
	const KeyTable &middle = middle_keys (*m);

	for (int k = 0; k < 2; k++) {
		const Draw *d = screen_draw (frames[k ? 2 : 0]);

		if (!d)
			return false;
		const std::vector<Vertex> &sv = d->scene->vertices, &mv = m->scene->vertices;

		/* The side's vertices whose key the middle has once, those whose key
		 * the side has once too */
		found.clear ();
		for (size_t i = 0; i < sv.size (); i++) {
			if (i % 3 == 0 && !is_3d (sv, i)) {
				i += 2;
				continue;
			}
			uint64_t k = KeyTable::key (sv[i]);

			if (k && middle.find (k) >= 0)
				found.push_back ({ k, (uint32_t) i });
		}
		std::sort (found.begin (), found.end ());
		pairs[k].clear ();
		for (size_t a = 0, b; a < found.size (); a = b) {
			bool one = true;

			for (b = a + 1; b < found.size () && found[b].first == found[a].first; b++)
				one = one && same_place (sv[found[b].second], sv[found[a].second]);
			if (!one)
				continue;
			const Vertex &s = sv[found[a].second], &t = mv[middle.find (found[a].first)];

			pairs[k].push_back ({ s.x * d->scale_x, s.y * d->scale_y, s.z, t.x * m->scale_x, t.y * m->scale_y, t.z });
		}
		v[k] = solve_side (pairs[k], steady_cy);
		if (v[k].pairs < 8 && vote_pairs (*d, *m, k ? 1.0f : -1.0f, pairs[k]))
			v[k] = solve_side (pairs[k], steady_cy);
	}
	*enough = v[0].pairs >= 8 && v[1].pairs >= 8;
	/* A side solved: five of its points on each other at least, two in five
	 * of those not of another copy of a model (SideView::elsewhere), half
	 * their rows too (the others what moves between the boards' frames, a
	 * key two models share, Airline Pilots' wings flexed by each board;
	 * F355's race seen from the car, its left board 59%) */
	bool solved[2];

	for (int k = 0; k < 2; k++)
		solved[k] = v[k].pairs >= 8 && v[k].agree >= 5 && v[k].agree * 5 >= v[k].counted () * 2 &&
		            v[k].agree_y * 2 >= v[k].counted ();
	/* Both, solved each on its own, the same turn either way and the same
	 * focal length to a hundredth of a degree and 0.5% (one rig: the views
	 * of one eye; two fits apart so close no chance makes) */
	if (solved[0] && solved[1] && fabsf (v[0].turn + v[1].turn) <= 0.0035f &&
	    fabsf (v[0].focal - v[1].focal) <= 0.005f * v[1].focal && v[1].turn > 0.0f) {
		*yaw = (v[1].turn - v[0].turn) * 0.5f;
		*focal = (v[0].focal + v[1].focal) * 0.5f;
		*centre = (v[0].cy + v[1].cy) * 0.5f;
		return true;
	}
	/* Or both near (SideView::near), the same turn either way and the same
	 * focal length (Sega Strike Fighter's training: its cockpit on each
	 * other, its world each board's a frame apart, held while it shows how
	 * to fire; 12% of the left board's points) */
	if (v[0].near && v[1].near && fabsf (v[0].turn + v[1].turn) <= 0.0035f &&
	    fabsf (v[0].focal - v[1].focal) <= 0.005f * v[1].focal && v[1].turn > 0.0f) {
		*yaw = (v[1].turn - v[0].turn) * 0.5f;
		*focal = (v[0].focal + v[1].focal) * 0.5f;
		*centre = (v[0].cy + v[1].cy) * 0.5f;
		return true;
	}
	/* Or one, nine in ten of its points on each other, while the other has
	 * too few to tell (a cut: F355's race, its left board 7) or has its own
	 * turned the other way among its points: a tenth of them and four at
	 * least, or half when under 8 tell (the others a model drawn several
	 * times, each board seeing other ones: Sega Strike Fighter's
	 * helicopters, 24% of the left board's points; its fleet, one) */
	for (int k = 0; k < 2; k++) {
		if (!solved[k] || v[k].agree * 10 < v[k].counted () * 9 || (k ? v[k].turn <= 0.0f : v[k].turn >= 0.0f))
			continue;
		double A = cos (v[k].turn), B = -sin (v[k].turn) / v[k].focal;
		unsigned away = 0, other = v[!k].pairs < 8 ? 0 : agreeing (pairs[!k], A, -B, NULL, v[k].cy, &away);
		unsigned tell = v[!k].pairs - away;

		/* Or the views on (steady_views ()) as it has them: the other's
		 * points another copy's all but a few (its fleet: the left board's
		 * ships, three of their points by the middle board's top edge) */
		bool same = views_left && fabsf (fabsf (v[k].turn) - steady_yaw) <= 0.0035f &&
		            fabsf (v[k].focal - steady_f) <= 0.005f * steady_f;

		if (same || v[!k].pairs < 8 || (tell < 8 ? other * 2 >= tell : other >= 4 && other * 10 >= tell)) {
			*yaw = fabsf (v[k].turn);
			*focal = v[k].focal;
			*centre = v[k].cy;
			return true;
		}
	}
	/* Or, the views on, the rig as it was with the master's camera shaken
	 * alone (solve_shake ()): from 0.2 to 25 degrees, each side's pairs on
	 * each other shaken, 16 at least, or not shaken (the crash's points
	 * high over the right board's screen), three in four (Airline Pilots'
	 * plane at the gate, its side boards' cameras between two of the
	 * master's frames: a fifth on each other with no shake) */
	if (views_left && steady_f > 0.0f && v[0].pairs >= 8 && v[1].pairs >= 8) {
		double S[9];
		unsigned agree[2], either[2];

		solve_shake (pairs, steady_yaw, steady_f, steady_cy, S, agree, either);
		double angle = acos (std::max (-1.0, std::min (1.0, (S[0] + S[4] + S[8] - 1.0) * 0.5)));

		if (angle >= 0.0035 && angle <= 0.44 && agree[0] >= 16 && agree[1] >= 16 && either[0] * 4 >= v[0].pairs * 3 &&
		    either[1] * 4 >= v[1].pairs * 3) {
			for (int i = 0; i < 9; i++)
				views_shake[i] = (float) S[i];
			views_shaken = true;
			*yaw = steady_yaw;
			*focal = steady_f;
			*centre = steady_cy;
			return true;
		}
	}
	return false;
}

/* solve_views () from frame to frame: on once solved two frames in a row (not
 * for a frame alone: Airline Pilots' camera, turning, solved at a still
 * moment), the views kept while they move less than a pixel (a frame's
 * solution varies by a little), kept through 10 frames of too few points (a
 * turn in the scenery), off after 4 frames more when points enough do not
 * agree (a cut to another scene's cameras; the boards' frames apart while
 * the camera moves, for a frame or two: Airline Pilots' plane at the gate,
 * its side boards' cameras a frame late but now and then; Sega Strike
 * Fighter's cuts between scenes of the same views, 4 frames apart).
 * Steady, solved every other frame (Airline Pilots' 40000 vertices a board,
 * some 1.5 ms); each frame while they move (F355's zooms) */
bool
Renderer::steady_views (const std::vector<BoardRenderer::Frame> &frames, float *yaw, float *focal,
                        float *centre) const
{
	float y, f, cy;
	bool enough = false, solved = false;

	if (views_left && views_steady && (views_frames++ & 1)) {
		*yaw = steady_yaw;
		*focal = steady_f;
		*centre = steady_cy;
		return true;
	}
	/* Against the middle board's scene that goes with the side ones' (its
	 * last, or the one before: middle_late) first, then the other one */
	const Draw *now = frames.size () == 3 ? screen_draw (frames[1]) : NULL;
	const Draw *before = held_valid ? &held_middle : NULL;

	for (int k = 0; k < 2 && !solved; k++) {
		bool late = middle_late != (k == 1), more = false;

		solved = solve_views (frames, late ? before : now, &y, &f, &cy, &more);
		enough |= more;
		if (solved)
			middle_late = late;
	}
	if (solved) {
		/* Shaken: solved at each frame, the shake moving */
		views_steady = views_left && !views_shaken && fabsf (y - steady_yaw) <= 0.002f &&
		               fabsf (f - steady_f) <= 0.003f * f && fabsf (cy - steady_cy) <= 1.0f;
		if (!views_steady) {
			steady_yaw = y;
			steady_f = f;
			steady_cy = cy;
		}
		if (views_left || views_solved)
			views_left = 10;
		views_solved = true;
	} else if (views_left && views_bare) {
		/* No 3D on the side screens: the views kept as they are, their
		 * strips going on with the middle board's picture (Sega Strike
		 * Fighter's jet, its nose over the edge: 2 or 3 frames of the
		 * cabinet's views at each attract, 10 frames after it left the
		 * side screens) */
		views_solved = false;
	} else {
		views_steady = views_solved = false;
		views_left = !views_left ? 0 : enough ? std::min (views_left - 1, 4u) : views_left - 1;
	}
	*yaw = steady_yaw;
	*focal = steady_f;
	*centre = steady_cy;
	return views_left != 0;
}

/* View::mask_2d's cells (MASK_CELL pixels of a side screen's 640x480) */
struct CellGrid {
	std::vector<uint8_t> in;
	bool any = false;
	/* The columns of cells looked at: the strip's (views ()) */
	int lo = 0, hi = BoardRenderer::View::MASK_W - 1;

	CellGrid () : in ((size_t) BoardRenderer::View::MASK_W * BoardRenderer::View::MASK_H, 0) {}
	/* Each cell whose middle is in the triangle */
	void tri (const float *x, const float *y)
	{
		const int W = BoardRenderer::View::MASK_W, H = BoardRenderer::View::MASK_H;
		const float cell = (float) BoardRenderer::View::MASK_CELL;
		float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);

		if (area == 0.0f)
			return;
		int c0 = std::max (lo, (int) floorf (std::max (-1.0e6f, std::min ({ x[0], x[1], x[2] })) / cell));
		int c1 = std::min (hi, (int) floorf (std::min (1.0e6f, std::max ({ x[0], x[1], x[2] })) / cell));
		int r0 = std::max (0, (int) floorf (std::max (-1.0e6f, std::min ({ y[0], y[1], y[2] })) / cell));
		int r1 = std::min (H - 1, (int) floorf (std::min (1.0e6f, std::max ({ y[0], y[1], y[2] })) / cell));

		for (int r = r0; r <= r1; r++)
			for (int c = c0; c <= c1; c++) {
				float px = (c + 0.5f) * cell, py = (r + 0.5f) * cell, e[3];

				for (int k = 0; k < 3; k++) {
					int l = (k + 1) % 3;

					e[k] = ((x[l] - x[k]) * (py - y[k]) - (px - x[k]) * (y[l] - y[k])) * (area > 0.0f ? 1.0f : -1.0f);
				}
				if (e[0] >= 0.0f && e[1] >= 0.0f && e[2] >= 0.0f) {
					in[(size_t) r * W + c] = 1;
					any = true;
				}
			}
	}
	/* The cells and those around them (a letter, a wheel's rim by a
	 * triangle's edge); none: empty */
	void to_mask (std::vector<uint8_t> &m) const
	{
		const int W = BoardRenderer::View::MASK_W, H = BoardRenderer::View::MASK_H;

		m.clear ();
		if (!any)
			return;
		m.assign ((size_t) W * H, 0);
		for (int r = 0; r < H; r++)
			for (int c = 0; c < W; c++)
				if (in[(size_t) r * W + c])
					for (int dr = -1; dr <= 1; dr++)
						for (int dc = -1; dc <= 1; dc++)
							if (r + dr >= 0 && r + dr < H && c + dc >= 0 && c + dc < W)
								m[(size_t) (r + dr) * W + c + dc] = 255;
	}
};

/* A side screen's 2D (View::mask_2d): its triangles of the same 1/W at their
 * three vertices, 1 or more (as draw_scene () tells them; F355's trademark
 * notice over its race's start, at 100, each a 2.7% of the screen), and its
 * insets. Not the far plane's (the backgrounds), nor a triangle over a
 * quarter of the screen: a fade's (F355's scenes fade in by a quad over the
 * whole screen) */
static void
mask_2d (const Draw &d, CellGrid &g)
{
	const std::vector<Vertex> &v = d.scene->vertices;

	/* Its insets too (inset ()), their clips' rectangles */
	for (const std::vector<Batch> &list : d.scene->batches)
		for (const Batch &b : list)
			if (inset (b, d) && b.count) {
				float x0 = b.x0 * d.scale_x, x1 = b.x1 * d.scale_x, y0 = b.y0 * d.scale_y, y1 = b.y1 * d.scale_y;
				float xa[3] = { x0, x1, x1 }, ya[3] = { y0, y0, y1 }, xb[3] = { x0, x1, x0 }, yb[3] = { y0, y1, y1 };

				g.tri (xa, ya);
				g.tri (xb, yb);
			}

	for (size_t t = 0; t + 2 < v.size (); t += 3) {
		float z = v[t].z, x[3], y[3];

		if (z < 1.0f || z != v[t + 1].z || z != v[t + 2].z)
			continue;
		for (int k = 0; k < 3; k++) {
			x[k] = v[t + k].x * d.scale_x;
			y[k] = v[t + k].y * d.scale_y;
		}
		if (fabsf ((x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])) * 0.5f > 640.0f * 480.0f * 0.25f)
			continue;
		g.tri (x, y);
	}
}

/* The middle board's textured 3D vertices by their texture coordinates
 * alone (several share them: a car's four wheels) and by the cell of CELL
 * pixels of its screen they are in, a chain each (a tiled texture's corners
 * by the thousand all over a city, Sega Strike Fighter's, but a few in a
 * cell) */
struct UvChains {
	enum { BITS = 18, CELL = 32 };
	std::vector<int32_t> head, next;

	static uint64_t key (const Vertex &x)
	{
		uint32_t u, w;

		if ((x.u == 0.0f && x.v == 0.0f) || x.z <= 1e-6f)
			return 0;
		memcpy (&u, &x.u, 4);
		memcpy (&w, &x.v, 4);
		return ((uint64_t) u << 32 | w) * 0x9E3779B97F4A7C15ull;
	}
	static int cell (float p)
	{
		return (int) floorf (std::max (-1.0e6f, std::min (1.0e6f, p)) / CELL);
	}
	static size_t bucket (uint64_t k, int cx, int cy)
	{
		uint64_t h = (k ^ ((uint64_t) (uint32_t) cx * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t) (uint32_t) cy * 0x165667B19E3779F9ull)) *
		             0x9E3779B97F4A7C15ull;

		return (size_t) (h >> (64 - BITS));
	}
	void build (const Draw &m)
	{
		const std::vector<Vertex> &v = m.scene->vertices;

		head.assign ((size_t) 1 << BITS, -1);
		next.assign (v.size (), -1);
		for (size_t i = 0; i + 2 < v.size (); i += 3) {
			if (!is_3d (v, i))
				continue;
			for (size_t j = i; j < i + 3; j++) {
				uint64_t k = key (v[j]);

				if (!k)
					continue;
				size_t h = bucket (k, cell (v[j].x * m.scale_x), cell (v[j].y * m.scale_y));

				next[j] = head[h];
				head[h] = (int32_t) j;
			}
		}
	}
};

/* The middle board's chains of a scene, built once */
static const UvChains &
middle_chains (const Draw &m)
{
	static thread_local UvChains table;
	static thread_local std::shared_ptr<Scene> built;	/* kept alive: no other at its address */

	if (built != m.scene) {
		table.build (m);
		built = m.scene;
	}
	return table;
}

/* What the side board alone draws by the seam (View::mask_2d too): its
 * textured 3D triangles none of whose vertices the middle board has, a
 * vertex of the same texture coordinates where the side board's falls in
 * the middle view (its turn yaw, its focal length and centre: views ()'s),
 * within 20 pixels and 10% of its depth (each board flexes Airline Pilots'
 * wings its own way, up to 12 pixels apart there; a car's other wheels are
 * far); put where the joined view (view) shows them on the side screen,
 * within 200 pixels of its inner edge (inner: the edge's x). The objects
 * the middle board leaves out as out of its view: a car's wheel by F355's
 * factory (30% of the left board's vertices there not in the middle one's
 * list), its wheel arch the middle board's, 2% deeper: no depth tells them
 * apart. Texture coordinates alone: a vertex's colour is lit for each
 * board's view (Airline Pilots' wing, its letters) */
static void
side_only (const Draw &d, const Draw &m, const UvChains &middle, const double *to_middle,
           const BoardRenderer::View &view, float inner, CellGrid &g)
{
	const std::vector<Vertex> &v = d.scene->vertices, &mv = m.scene->vertices;
	float f = view.focal, fo = view.out_focal > 0.0f ? view.out_focal : view.focal, cy = view.centre_y;
	/* The side board's view to its screen's (View::rot, or its turn), to
	 * the middle board's (to_middle) */
	double P[9], R[9];

	if (view.rotated)
		for (int k = 0; k < 9; k++)
			P[k] = view.rot[k];
	else
		mat3_yaw (view.turn, P);
	memcpy (R, to_middle, sizeof (R));
	if (f <= 0.0f)
		return;
	for (size_t i = 0; i + 2 < v.size (); i += 3) {
		float x[3], y[3], dx[3], dy[3];
		uint64_t key[3];
		bool keep = is_3d (v, i);

		/* On the side screen, by its inner edge */
		for (int k = 0; k < 3 && keep; k++) {
			const Vertex &p = v[i + k];
			float pw;

			key[k] = UvChains::key (p);
			dx[k] = (p.x * d.scale_x - 320.0f) / f;
			dy[k] = (p.y * d.scale_y - cy) / f;
			pw = (float) (P[6] * dx[k] + P[7] * dy[k] + P[8]);
			keep = key[k] && pw > 1e-4f;
			x[k] = 320.0f + fo * (float) (P[0] * dx[k] + P[1] * dy[k] + P[2]) / pw;
			y[k] = cy + fo * (float) (P[3] * dx[k] + P[4] * dy[k] + P[5]) / pw;
		}
		if (!keep || std::min ({ fabsf (x[0] - inner), fabsf (x[1] - inner), fabsf (x[2] - inner) }) > 200.0f)
			continue;
		/* None of its vertices where the middle board would have it (the
		 * cells around; a chain looked at for 64 vertices at most, past
		 * that taken as there) */
		for (int k = 0; k < 3 && keep; k++) {
			float Z = 1.0f / v[i + k].z, X = dx[k] * Z, Y = dy[k] * Z;
			float Xm = (float) (R[0] * X + R[1] * Y + R[2] * Z), Ym = (float) (R[3] * X + R[4] * Y + R[5] * Z);
			float Zm = (float) (R[6] * X + R[7] * Y + R[8] * Z);

			if (Zm <= 1e-6f)
				continue;
			float xm = 320.0f + f * Xm / Zm, ym = cy + f * Ym / Zm, zm = 1.0f / Zm;
			int cx = UvChains::cell (xm), cyc = UvChains::cell (ym);

			for (int n = 0; n < 9 && keep; n++) {
				int looked = 0;

				for (int32_t j = middle.head[UvChains::bucket (key[k], cx + n % 3 - 1, cyc + n / 3 - 1)]; j >= 0 && keep;
				     j = middle.next[j]) {
					const Vertex &q = mv[j];

					keep = ++looked < 64 &&
					       !(UvChains::key (q) == key[k] && fabsf (q.x * m.scale_x - xm) < 20.0f &&
					         fabsf (q.y * m.scale_y - ym) < 20.0f && fabsf (q.z - zm) < 0.1f * zm);
				}
			}
		}
		if (keep)
			g.tri (x, y);
	}
}

/* The game has views to solve (games.json "screen_view"), the boards shown
 * are its screens, a view other than the cabinet's chosen */
bool
Renderer::views_wanted (const std::vector<unsigned> &shown) const
{
	const hl::Game *game = naomi->game;

	return game && game->view_focal > 0.0f && shown.size () >= 2 && naomi->multi_screen != hl::MULTI_ONE &&
	       hl::Config::get ()->multi_screen_view != hl::VIEW_CABINET && shown == naomi->screen_order;
}

/* The side boards a frame behind the middle one (middle_late: their views
 * solved on its scene before its last; Sega Strike Fighter's attract, its
 * camera passed on to them a frame late, the 3D one frame apart, some 50
 * pixels while it turns): the middle board's scene for the screen drawn a
 * frame late, its last kept (held_middle) for the next frame, the three
 * screens the same moment. Else its last kept all the same, the next frame
 * solved on both */
void
Renderer::late_middle (const std::vector<unsigned> &shown, std::vector<BoardRenderer::Frame> &frames,
                       const std::vector<BoardRenderer::View> &v)
{
	Draw *now = NULL;

	if (frames.size () != 3 || !views_wanted (shown)) {
		held_middle = Draw ();
		held_valid = middle_late = false;
		return;
	}
	for (Draw &d : frames[1].draws)
		if (!d.to_texture && d.scene)
			now = &d;
	if (v.size () == 3 && v[0].focal > 0.0f && middle_late && held_valid) {
		if (now) {
			std::swap (*now, held_middle);
			held_shown = false;
		} else if (!held_shown) {
			/* None since: the one kept, at last */
			frames[1].draws.push_back (held_middle);
			held_shown = true;
		}
		return;
	}
	if (now) {
		held_middle = *now;
		held_valid = held_shown = true;
	}
}

std::vector<BoardRenderer::View>
Renderer::views (const std::vector<unsigned> &shown, const std::vector<BoardRenderer::Frame> &frames) const
{
	size_t n = shown.size (), i;
	std::vector<BoardRenderer::View> v (n);
	float centre = 240.0f, f = 0.0f, yaw = 0.0f;

	if (!views_wanted (shown) || !steady_views (frames, &yaw, &f, &centre))
		return v;
	float half = atanf (width * 0.5f / f);
	/* The outer screens' far edges no farther than the boards draw (their
	 * views' edges): with WIDESCREEN the screens are wider than the turn
	 * between them, the side views would turn outwards past what the games
	 * draw (Airline Pilots' cockpit, missing there); the screens' field
	 * narrowed so, the picture a little larger (some 12%) */
	float outer = (float) (n - 1) * 0.5f, joined = std::min (half, (outer * yaw + half) / (2.0f * outer + 1.0f));

	/* Each board's view to the middle one's: turned by yaw, the master's
	 * shake on it (solve_shake ()) */
	std::vector<std::array<double, 9>> to_middle (n);

	for (i = 0; i < n; i++) {
		float k = (float) i - outer;

		v[i].turn = k * (yaw - 2.0f * joined);
		v[i].focal = f;
		v[i].out_focal = joined < half ? width * 0.5f / tanf (joined) : f;
		v[i].centre_y = centre;
		mat3_yaw (k * yaw, to_middle[i].data ());
		if (views_shaken && n == 3 && i != n / 2) {
			double S[9], screen[9];

			for (int j = 0; j < 9; j++)
				S[j] = views_shake[j];
			mat3_mul (S, to_middle[i].data (), to_middle[i].data ());
			mat3_yaw (-k * 2.0f * joined, screen);
			mat3_mul (screen, to_middle[i].data (), screen);
			for (int j = 0; j < 9; j++)
				v[i].rot[j] = (float) screen[j];
			v[i].rotated = true;
		}
	}
	/* In 4:3 the middle board's scene drawn in the side screens' views too,
	 * their axes 2 x joined either side of its own (strip_turn: to the
	 * left the view turns the scene to the right): their strips by their
	 * inner edges (fill_strips ()), but where their 2D is and what the
	 * middle board left out */
	const Draw *m = n == 3 && frames.size () == 3 ? screen_draw (frames[1]) : NULL;

	if (middle_late && held_valid)
		m = &held_middle;

	if (width < 854 && n == 3) {
		v[1].strips = true;
		v[1].strip_turn[0] = 2.0f * joined;
		v[1].strip_turn[1] = -2.0f * joined;
		for (i = 0; i < n && i < frames.size (); i++)
			if (i != n / 2)
				if (const Draw *d = screen_draw (frames[i])) {
					CellGrid g;
					/* Only the strip's columns: from the side screen's inner
					 * edge to its board's own view's (fill_strips ()), 16
					 * pixels more (Airline Pilots' 74 pixels, Sega Strike
					 * Fighter's 250 with a long lens; a side screen's 2D
					 * rasterized all over took 2.5 ms in its missions) */
					float gap = 320.0f + f * tanf (std::max (-1.5f, std::min (1.5f, 3.0f * half - yaw))) - 16.0f;
					const int cell = BoardRenderer::View::MASK_CELL;

					if (i < n / 2)
						g.lo = std::max (0, (int) floorf (gap / cell));
					else
						g.hi = std::min (BoardRenderer::View::MASK_W - 1, (int) floorf ((640.0f - gap) / cell));

					mask_2d (*d, g);
					if (m)
						side_only (*d, *m, middle_chains (*m), to_middle[i].data (), v[i], i < n / 2 ? 640.0f : 0.0f,
						           g);
					g.to_mask (v[i].mask_2d);
				}
	}
	return v;
}

/* Over the side screens of the joined pictures (side_by_side ()'s texture,
 * bound), by their inner edges, the middle board's scene drawn in their
 * views (Picture::strip): in the cabinet's gap between its monitors each
 * board has what the other one leaves out (Airline Pilots' overhead
 * compass, 22 to 25 degrees from the middle, the middle board's) */
void
Renderer::fill_strips (const std::vector<Picture> &pictures, const std::vector<BoardRenderer::View> &v,
                       BoardRenderer *with)
{
	size_t n = pictures.size (), middle = n / 2, i;
	unsigned scale = (unsigned) std::max (1, hl::Config::get ()->render_scale);

	if (n != 3 || v.size () != n || !pictures[middle].strip[0] || !pictures[middle].strip[1])
		return;
	float f = v[middle].out_focal > 0.0f ? v[middle].out_focal : v[middle].focal;
	float step = 2.0f * atanf (width * 0.5f / f);

	glBindFramebuffer (GL_FRAMEBUFFER, wide_fb);
	glDisable (GL_SCISSOR_TEST);
	for (i = middle - 1; i <= middle + 1; i += 2) {
		/* The side screen's turn from its board's view (View::turn) and
		 * the middle screen's field make the boards' turn; the cut halfway
		 * between their views, the side board's own view from its edge */
		float k = (float) i - (float) middle, yaw = v[i].turn / k + step;
		float cut = f * tanf (std::min (yaw * 0.5f, 1.55f));
		float edge = f * tanf (std::min (yaw - atanf (width * 0.5f / v[i].focal), 1.55f));

		glViewport ((GLint) (i * width * scale), 0, width * scale, height * scale);
		with->fill_strip (pictures[middle], i < middle ? 0 : 1, pictures[i], &v[i].mask_2d, (float) width,
		                  (float) height, f, k * step, cut, edge);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

Projection
Renderer::projection (const std::vector<BoardRenderer::View> &v) const
{
	int mode = hl::Config::get ()->multi_screen_view;
	Projection p;

	if (v.empty () || v[0].focal <= 0.0f || mode < hl::VIEW_FLAT || mode >= hl::NUM_SCREEN_VIEWS)
		return p;
	p.mode = mode;
	p.panels = (unsigned) v.size ();
	p.panel_w = (float) width;
	p.panel_h = (float) height;
	/* The joined panels' focal length (View::out_focal) */
	p.focal = v[0].out_focal > 0.0f ? v[0].out_focal : v[0].focal;
	p.centre_y = v[0].centre_y;
	p.half = atanf (width * 0.5f / p.focal);
	p.out_focal = out_focal (p);
	return p;
}

Picture
Renderer::projected (const Picture &wide, const Projection &pr, BoardRenderer *with)
{
	Picture out;

	if (!proj_fb) {
		glGenFramebuffers (1, &proj_fb);
		glGenTextures (1, &proj_color);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, proj_fb);
	if (proj_w != wide_w || proj_h != wide_h) {
		glBindTexture (GL_TEXTURE_2D, proj_color);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, wide_w, wide_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture (GL_TEXTURE_2D, 0);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, proj_color, 0);
		proj_w = wide_w;
		proj_h = wide_h;
	}
	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, proj_w, proj_h);
	with->project (wide, pr);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	out.texture = proj_color;
	return out;
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
Renderer::present_pictures (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures,
                            BoardRenderer *with, const std::vector<BoardRenderer::View> &drawn_in)
{
	size_t i;

	if (shown.empty ())
		return;
	Projection pr = projection (drawn_in);
	BoardRenderer *blit_with = with ? with : drawer (shown[0]);
	bool joined = shown.size () > 1 && !drawn_in.empty () && drawn_in[0].focal > 0.0f;

	if (naomi->multi_screen == hl::MULTI_SIDE_BY_SIDE && shown.size () > 1) {
		Picture wide = side_by_side (shown, pictures, with);

		if (joined)
			fill_strips (pictures, drawn_in, blit_with);
		if (pr.mode)
			wide = projected (wide, pr, blit_with);
		show (outputs[0], wide, blit_with, width * (unsigned) shown.size (), height);
		return;
	}
	/* The joined pictures in windows: each its part of them */
	if (joined) {
		Picture all = side_by_side (shown, pictures, with);

		fill_strips (pictures, drawn_in, blit_with);
		if (pr.mode)
			all = projected (all, pr, blit_with);

		for (i = outputs.size (); i-- > 0;)
			if (i < shown.size ()) {
				Picture part = all;

				part.x0 = (float) i / (float) shown.size ();
				part.x1 = (float) (i + 1) / (float) shown.size ();
				show (outputs[i], part, blit_with, width, height);
			}
		return;
	}
	/* The main one last: its swap, after this, waits for the screen */
	for (i = outputs.size (); i-- > 0;)
		if (i < shown.size () && i < pictures.size ())
			show (outputs[i], pictures[i], with ? with : drawer (shown[i]), width, height);
}

void
Renderer::on_end_frame ()
{
	std::vector<unsigned> shown = naomi->shown_boards ();
	std::vector<Picture> pictures;
	size_t i;

	if (thread) {
		threaded_end_frame (shown);
		return;
	}
	std::vector<BoardRenderer::View> v;

	try {
		std::vector<BoardRenderer::Frame> frames;

		for (i = 0; i < shown.size (); i++)
			frames.push_back (drawer (shown[i])->take_frame ());
		v = views (shown, frames);
		note_views (shown, v);
		late_middle (shown, frames, v);
		for (i = 0; i < shown.size (); i++) {
			frames[i].view = v[i];
			pictures.push_back (drawer (shown[i])->draw_frame (frames[i]));
		}
	} catch (const std::bad_alloc &) {
		HL_ERROR_FEW ("naomi: no memory left (another program took it?): a frame not drawn");
		return;
	}
	present_pictures (shown, pictures, NULL, v);
}

/*
 * The drawing thread
 *
 * THREADED_RENDERING: the boards' pictures drawn by a thread of their own, in
 * a GL context sharing the window's, while the machine runs the next frame.
 * At a frame's end the machine's thread takes each shown board's scenes (the
 * copies a STARTRENDER made of what they read) and, after a while without
 * one, the framebuffer the game shows; it waits for the thread to be done
 * with the frame before, hands this one over and shows that one, a frame
 * later than without the thread. The pictures go to two sets of textures in
 * turn. A frame with a scene drawn to a texture is waited for and shown at
 * once: its pixels go back into the VRAM, which the game reads next. The
 * screenshots, the agent's "shot" and the pause take the last frame (flush).
 */
struct Renderer::Thread {
	std::thread		 thread;
	std::mutex		 lock;
	std::condition_variable	 wake, idle;
	/* busy: not waiting for a job (starting, drawing) */
	bool			 busy = true, job = false, quit = false, ready = false;
	/* An HL_ABORT there, thrown again by the machine's frame */
	std::string		 error;
	SDL_GLContext		 context = NULL;

	/* A board's picture, copied: the thread's texture and framebuffer */
	struct Out {
		GLuint		 texture = 0, fb = 0;
		int		 w = 0, h = 0;
		bool		 flip = false, valid = false;
		float		 x0 = 0.0f, x1 = 1.0f;
		/* Its depth, copied while the screens' views are on */
		GLuint		 depth = 0;
		int		 depth_w = 0, depth_h = 0;
		float		 depth_unit = 0.0f;
		/* The middle board's scene in the side screens' views and their
		 * depths (Picture::strip), copied */
		struct Strip {
			GLuint	 texture = 0, depth = 0;
			int	 w = 0, h = 0;
			bool	 valid = false;
		} strips[2];
	};
	struct Set {
		std::vector<unsigned> shown;
		std::vector<Out> outs;
		/* The views they were drawn in */
		std::vector<BoardRenderer::View> views;
		/* The thread done with it; the window done showing it */
		GLsync		 drawn = 0, shown_sync = 0;
	} sets[2];
	/* The job: the boards shown, their frames, the set drawn into */
	std::vector<unsigned>	 job_shown;
	std::vector<BoardRenderer::Frame> job_frames;
	unsigned		 job_set = 0;
	GLuint			 read_fb = 0;
	/* The set of the next frame, of the last one; the frames handed over,
	 * the last one shown */
	unsigned		 next = 0, last = 0;
	uint64_t		 jobs = 0, shown_job = 0;
};

/* A sync of the other context's: its work done on the GPU, then deleted */
static void
client_wait (GLsync sync)
{
	while (glClientWaitSync (sync, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000) == GL_TIMEOUT_EXPIRED)
		;
	glDeleteSync (sync);
}

bool
Renderer::start_thread ()
{
	Thread *t;

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
	thread = t;
	t->thread = std::thread (thread_main, this);
	wait_thread (false);
	if (!t->ready) {
		HL_ERROR ("THREADED_RENDERING: the drawing thread did not start (%s), the frames drawn without it",
		          t->error.c_str ());
		stop_thread ();
		return false;
	}
	hl::Window::set_flush (flush, this);
	HL_PRINT ("naomi: the frames drawn by a thread of their own (THREADED_RENDERING)");
	return true;
}

void
Renderer::stop_thread ()
{
	Thread *t = thread;

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
	hl::Window::delete_context (t->context);
	delete t;
	thread = nullptr;
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
Renderer::thread_main (Renderer *owner)
{
	Thread *t = owner->thread;

	HL_PRINT ("naomi: the drawing thread is thread %lu", hl::Log::thread_id ());
	if (hl::Window::make_current (t->context))
		t->ready = true;
	else
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
			if (t->ready)
				owner->draw_job ();
		} catch (const hl::Abort &e) {
			t->error = e.what ();
		}
	}

	/* Its GL objects, in its context */
	for (auto &d : owner->drawers)
		d.reset ();
	for (auto &s : t->sets) {
		for (auto &o : s.outs) {
			if (o.fb)
				glDeleteFramebuffers (1, &o.fb);
			if (o.texture)
				glDeleteTextures (1, &o.texture);
			if (o.depth)
				glDeleteTextures (1, &o.depth);
			for (auto &c : o.strips) {
				if (c.texture)
					glDeleteTextures (1, &c.texture);
				if (c.depth)
					glDeleteTextures (1, &c.depth);
			}
		}
		if (s.drawn)
			glDeleteSync (s.drawn);
		if (s.shown_sync)
			glDeleteSync (s.shown_sync);
	}
	if (t->read_fb)
		glDeleteFramebuffers (1, &t->read_fb);
	glFinish ();
	hl::Window::make_current (NULL);
}

/* On the thread: the frame handed over, each board's picture copied into the
 * set's textures */
void
Renderer::draw_job ()
{
	Thread *t = thread;
	auto &s = t->sets[t->job_set];
	size_t i;

	/* The window done showing what the set held */
	if (s.shown_sync) {
		client_wait (s.shown_sync);
		s.shown_sync = 0;
	}
	s.shown = t->job_shown;
	s.views.clear ();
	for (const BoardRenderer::Frame &f : t->job_frames)
		s.views.push_back (f.view);
	s.outs.resize (s.shown.size ());
	for (i = 0; i < s.shown.size () && i < t->job_frames.size (); i++) {
		Picture p = drawer (s.shown[i])->draw_frame (t->job_frames[i]);
		auto &o = s.outs[i];
		GLint w = 0, h = 0;

		o.valid = p.texture != 0;
		o.flip = p.flip;
		o.x0 = p.x0;
		o.x1 = p.x1;
		for (auto &c : o.strips)
			c.valid = false;
		if (!o.valid)
			continue;
		glBindTexture (GL_TEXTURE_2D, p.texture);
		glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		if (!o.texture || o.w != w || o.h != h) {
			if (!o.texture)
				glGenTextures (1, &o.texture);
			glBindTexture (GL_TEXTURE_2D, o.texture);
			glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			if (!o.fb)
				glGenFramebuffers (1, &o.fb);
			glBindFramebuffer (GL_FRAMEBUFFER, o.fb);
			glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o.texture, 0);
			o.w = w;
			o.h = h;
		}
		glBindTexture (GL_TEXTURE_2D, 0);
		if (!t->read_fb)
			glGenFramebuffers (1, &t->read_fb);
		glBindFramebuffer (GL_READ_FRAMEBUFFER, t->read_fb);
		glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.texture, 0);
		glBindFramebuffer (GL_DRAW_FRAMEBUFFER, o.fb);
		glDisable (GL_SCISSOR_TEST);
		glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		/* MULTI_SCREEN_VIEW's strips (fill_strips ()): the depth too */
		o.depth_unit = 0.0f;
		if (p.depth && p.depth_unit > 0.0f && t->job_frames[i].view.focal > 0.0f) {
			if (!o.depth || o.depth_w != w || o.depth_h != h) {
				if (!o.depth)
					glGenTextures (1, &o.depth);
				glBindTexture (GL_TEXTURE_2D, o.depth);
				glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
				              GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glBindTexture (GL_TEXTURE_2D, 0);
				o.depth_w = w;
				o.depth_h = h;
			}
			glCopyImageSubData (p.depth, GL_TEXTURE_2D, 0, 0, 0, 0, o.depth, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
			o.depth_unit = p.depth_unit;
		}
		for (int k = 0; k < 2; k++) {
			auto &c = o.strips[k];

			if (!p.strip[k] || !p.strip_depth[k] || o.depth_unit <= 0.0f)
				continue;
			if (!c.texture || c.w != w || c.h != h) {
				if (!c.texture) {
					glGenTextures (1, &c.texture);
					glGenTextures (1, &c.depth);
				}
				glBindTexture (GL_TEXTURE_2D, c.texture);
				glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glBindTexture (GL_TEXTURE_2D, c.depth);
				glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
				              GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
				glBindTexture (GL_TEXTURE_2D, 0);
				c.w = w;
				c.h = h;
			}
			glCopyImageSubData (p.strip[k], GL_TEXTURE_2D, 0, 0, 0, 0, c.texture, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
			glCopyImageSubData (p.strip_depth[k], GL_TEXTURE_2D, 0, 0, 0, 0, c.depth, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
			c.valid = true;
		}
	}
	t->job_frames.clear ();
	if (s.drawn)
		glDeleteSync (s.drawn);
	s.drawn = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	/* The fence on its way, for the window's context to wait on */
	glFlush ();
}

/* In the window's context: a set's pictures where MULTI_SCREEN wants them */
void
Renderer::present (unsigned set)
{
	auto &s = thread->sets[set];
	std::vector<Picture> pictures;

	if (s.drawn) {
		client_wait (s.drawn);
		s.drawn = 0;
	}
	for (const auto &o : s.outs) {
		Picture p;

		p.texture = o.valid ? o.texture : 0;
		p.flip = o.flip;
		p.x0 = o.x0;
		p.x1 = o.x1;
		p.depth = o.depth_unit > 0.0f ? o.depth : 0;
		p.depth_unit = o.depth_unit;
		for (int k = 0; k < 2; k++)
			if (o.strips[k].valid) {
				p.strip[k] = o.strips[k].texture;
				p.strip_depth[k] = o.strips[k].depth;
			}
		pictures.push_back (p);
	}
	if (!blitter)
		blitter.reset (new BoardRenderer (naomi->boards[0]->holly, width, height));
	present_pictures (s.shown, pictures, blitter.get (), s.views);
	/* Shown twice (flush ()): the first one's sync, done with */
	if (s.shown_sync)
		client_wait (s.shown_sync);
	s.shown_sync = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush ();
}

void
Renderer::threaded_end_frame (const std::vector<unsigned> &shown)
{
	Thread *t = thread;
	std::vector<BoardRenderer::Frame> frames;
	bool at_once = false;
	uint64_t before;
	unsigned set, prev;

	/* For want of memory, this frame left out: the last one stays */
	try {
		for (unsigned b : shown) {
			frames.push_back (BoardRenderer::take (naomi->boards[b]->holly, quiet[b]));
			at_once |= frames.back ().to_texture;
		}
		std::vector<BoardRenderer::View> v = views (shown, frames);

		note_views (shown, v);
		late_middle (shown, frames, v);
		for (size_t k = 0; k < frames.size (); k++)
			frames[k].view = v[k];
	} catch (const std::bad_alloc &) {
		HL_ERROR_FEW ("naomi: no memory left (another program took it?): a frame not drawn");
		return;
	}
	/* The frame before drawn: this one to the thread, that one to the
	 * window */
	wait_thread (true);
	set = t->next;
	prev = t->last;
	before = t->jobs;
	t->job_frames.swap (frames);
	t->job_shown = shown;
	t->job_set = set;
	t->last = set;
	t->next ^= 1;
	t->jobs++;
	{
		std::lock_guard<std::mutex> lock (t->lock);
		t->busy = true;
		t->job = true;
	}
	t->wake.notify_one ();
	if (at_once) {
		wait_thread (true);
		present (set);
		t->shown_job = t->jobs;
	} else if (before) {
		present (prev);
		t->shown_job = before;
	} else {
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		glDisable (GL_SCISSOR_TEST);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
	}
}

/* Window::set_flush ()'s: the last frame drawn, to the window if it is not
 * there yet */
bool
Renderer::flush (void *data)
{
	Renderer *r = (Renderer *) data;
	Thread *t = r->thread;

	r->wait_thread (false);
	if (!t->jobs || t->shown_job == t->jobs)
		return false;
	r->present (t->last);
	t->shown_job = t->jobs;
	return true;
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

	collect_areas = false;
	if (oit.volumes)
		for (const ModVol &m : draw->scene->mod_volumes_tr)
			collect_areas = collect_areas || m.count;

	/* The opaque polygons' depth, for the collecting shader */
	glBindFramebuffer (GL_READ_FRAMEBUFFER, target->fb);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, oit.depth_fb);
	glBlitFramebuffer (0, 0, oit.w, oit.h, 0, 0, oit.w, oit.h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer (GL_FRAMEBUFFER, target->fb);

	/* Emptied by the GPU: written from here, the driver would wait for
	 * the last frame's collecting to be done with it */
	glBindBuffer (GL_ATOMIC_COUNTER_BUFFER, oit.counter);
	glClearBufferSubData (GL_ATOMIC_COUNTER_BUFFER, GL_R32UI, 0, sizeof (zero), GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
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
	glUniform1ui (collect.u_node_flags, 0);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask (GL_FALSE);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	for (const Batch &b : draw->scene->batches[LIST_TRANSLUCENT])
		draw_batch (collect, *draw, b, false, pixel_scale_x (), pixel_scale_y ());
	/* The last batch's user clip off: the volumes, the copy and the resolve
	 * cover the whole picture, the resolve emptying every pixel's list for
	 * the next frame */
	glDisable (GL_SCISSOR_TEST);
	glMemoryBarrier (GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	if (collect_areas) {
		draw_translucent_volumes ();
		glBindVertexArray (vao);
		collect_areas = false;
	}
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

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
	glUniform1i (oit.u_below, OIT_BELOW_UNIT);
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
