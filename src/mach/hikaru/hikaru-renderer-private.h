/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-renderer-private.h, Copyright (C) 2011-2014, Stefano Teso.
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

#ifndef __HIKARU_RENDERER_PRIVATE_H__
#define __HIKARU_RENDERER_PRIVATE_H__

#include <vector>

#include "hl/buffer.h"
#include "hl/renderer.h"
#include "hl/window.h"
#include "mach/hikaru/hikaru-gpu-private.h"

namespace hl {
class Presentation;
}

namespace hikaru {


enum {
	HR_DEBUG_LOG,
	HR_DEBUG_NO_LAYER1,
	HR_DEBUG_NO_LAYER2,
	HR_DEBUG_NO_3D,
	HR_DEBUG_SELECT_VIEWPORT,
	HR_DEBUG_NO_TEXTURES,
	HR_DEBUG_NO_MIPMAPS,
	HR_DEBUG_SELECT_POLYTYPE,
	HR_DEBUG_NO_INSTANCING,
	HR_DEBUG_SELECT_INSTANCE,
	HR_DEBUG_NO_LIGHTING,
	HR_DEBUG_NO_AMBIENT,
	HR_DEBUG_NO_DIFFUSE,
	HR_DEBUG_NO_SPECULAR,
	HR_DEBUG_FOG,

	HR_NUM_DEBUG_VARS
};

typedef union {
	struct {
		uint64_t has_texture		: 1;
		uint64_t has_lighting		: 1;
		uint64_t has_two_sided_lighting	: 1;
		/* Shading mode 2: the material's ambient (691) alone, not
		 * lit by the scene's ambient light */
		uint64_t has_fixed_ambient	: 1;
		uint64_t tex_mode		: 3;	/* 2C1's */
		uint64_t has_alpha_mode		: 1;	/* 881's */
		uint64_t has_alpha_test		: 1;	/* punch-through */

		uint64_t has_light0		: 1;
		uint64_t light0_type		: 2;
		uint64_t light0_att_type	: 3;
		uint64_t has_light0_specular	: 1;

		uint64_t has_light1		: 1;
		uint64_t light1_type		: 2;
		uint64_t light1_att_type	: 3;
		uint64_t has_light1_specular	: 1;

		uint64_t has_light2		: 1;
		uint64_t light2_type		: 2;
		uint64_t light2_att_type	: 3;
		uint64_t has_light2_specular	: 1;

		uint64_t has_light3		: 1;
		uint64_t light3_type		: 2;
		uint64_t light3_att_type	: 3;
		uint64_t has_light3_specular	: 1;

		uint64_t has_fog		: 1;
		uint64_t has_pattern		: 1;	/* 501's, not every pixel */
		uint64_t has_cue		: 1;	/* 621 D: faded by depth */
		/* A translucent mesh's fragments kept in their pixels' lists
		 * (OpenGL 4.3), not drawn: see "Translucency" */
		uint64_t collect		: 1;
		/* Blended by its alpha, weighted in the shader before the
		 * clamp (weighs_by_alpha) */
		uint64_t has_weight		: 1;
	};
	uint64_t full;
} hikaru_glsl_variant_t;

typedef struct {
	GLuint id;
	hikaru_texhead_t th;
	uint32_t gen;		/* its slots' writes when uploaded */
	/* TEXTURE_SCALE's factor it was uploaded at; ANISOTROPY's, set on
	 * it */
	uint8_t scale, aniso;
} hikaru_texture_t;

/* A post effect's picture and its framebuffer object */
typedef struct {
	GLuint fb, tex;
	unsigned w, h;
} hikaru_target_t;

typedef struct {
	uint32_t		first;	/* its first vertex in meshes.verts */
	uint32_t		num_tris;
	uint32_t		addr[2];
	uint32_t		vp_index;
	uint32_t		mv_index;
	uint32_t		num_instances;
	/* mv_index + 1 holds the matrix pushed before (161 P), which the
	 * vertices with link take */
	bool			has_link;
	uint32_t		mat_index;
	uint32_t		tex_index;
	uint32_t		ls_index;
	/* Punch-through (poly type 4): its pixels with an alpha out of
	 * alpha_thresh (C81, 154) are dropped, the others opaque */
	bool			has_alpha_test;
	float			alpha_thresh[2];
	/* 101: the depth bias, its mode and amount (upload_depth) */
	uint16_t		depth_bias;
	/* 501: the dot pattern it is drawn with, 0 to 31 (upload_pattern) */
	uint8_t			pattern;
	uint8_t			polytype;	/* HIKARU_POLYTYPE_* */
	bool			has_two_sided_lighting;
	uint32_t		num;
	/* The middle of its vertices, in the model: for the back to front
	 * order of the translucent meshes */
	vec3f_t			center;
} hikaru_mesh_t;

/* The Hikaru renderer (OpenGL): turns the meshes, textures, lights and
 * layers the GPU's command processor sends into frames. With
 * THREADED_RENDERING ([Display], taken at the game's start) a thread of its
 * own draws them, while the machine runs the next frame; see
 * hikaru-renderer.cpp, "The drawing thread". */
class Renderer : public hl::Renderer {
public:
	Renderer (hl::Buffer *fb, hl::Buffer *texram[2]);
	~Renderer ();
	/* The game's set name, for its list of shader programs (see "The
	 * programs' cache" in the .cpp) */
	static void set_game (const char *name);

	void invalidate_texcache (hikaru_texhead_t *th);
	/* The MEMCTL's write to a bank of the TEXRAM, at offs; the whole
	 * TEXRAM changed (a reset, a state loaded): the drawing thread's copy
	 * takes them, and the textures there are uploaded again */
	void texram_written (unsigned bank, uint32_t offs)
	{
		if (thread)
			texram_dirty[bank & 1][(offs >> 16) & 0x3F][(offs >> 5) & 0x7F] = true;
	}
	void invalidate_texram ();
	void push_vertices (hikaru_vertex_t *v, uint32_t flags, unsigned num);
	void begin_mesh (uint32_t addr, bool is_static);
	void end_mesh (uint32_t addr);
	void split_mesh (uint32_t addr);
	void on_begin_frame () override;
	void on_end_frame () override;
	void reset () override;
	void set_gpu (void *gpu_as_void);


	Gpu *gpu;

	/* What draw () reads of the GPU's: copied at the frame's end */
	struct {
		decltype (Gpu::fb_config) fb_config;
		decltype (Gpu::state.layers) layers;
		/* 1A0001B8 & 3, 1A0001BC & 7: the stacking order of the 3D
		 * and the layers, how each is tested (see hikaru-gpu.cpp) */
		unsigned layer_order, layer_test;
		uint32_t lists_begun;
	} frame_gpu;
	/* The TEXRAM and the framebuffer draw () reads: the GPU's, or the
	 * drawing thread's copies */
	hl::Buffer *texram_src[2], *fb_src;
	/* Where draw () puts the picture: the window (0), or the drawing
	 * thread's texture, with the window's size and the picture's place
	 * (the aspect ratio's, hl/aspect.h) */
	GLuint target_fb;
	int target_width, target_height;
	hl::ScreenLayout target_layout;
	/* The scene's width: 640, 854 in widescreen (hl::Renderer::width,
	 * taken at the game's start); the 2D layers' 640 in its middle */
	unsigned scene_w;
	/* The settings draw () follows, taken at the frame's end: the 2D
	 * layers stretched to the scene's width, the scaling filter, the draw
	 * distance's factor (on the fog); [Enhancements]', the bloom and the
	 * CRT filter from 0 to 1 */
	struct {
		bool stretch_2d, linear;
		float distance;
		unsigned scale, msaa, anisotropy, texture_scale;
		bool fxaa;
		float bloom, crt;
		bool sort_pixels;	/* SORT_TRANSLUCENT_PIXELS */
	} view;
	/* CRT_PRESET and BEZEL around the composition (hl/presentation.h):
	 * the GL objects of the context that draws (build_fb_state ()) */
	hl::Presentation *presentation;

	hikaru_viewport_t	*vp_list;
	uint32_t		 num_vps;

	hikaru_modelview_t	*mv_list;
	uint32_t		 num_mvs;
	uint32_t		 num_instances;

	hikaru_material_t	*mat_list;
	uint32_t		 num_mats;

	hikaru_texhead_t	*tex_list;
	uint32_t		 num_texs;

	hikaru_lightset_t	*ls_list;
	/* Each lightset's spot tables (194): a light's 16 points, 0x400 is
	 * 1.0, from the edge of its cone to its full light (upload_lightset) */
	struct hikaru_light_tables_t {
		uint16_t points[4][16];
	}			*ls_tables;
	uint32_t		 num_lss;

	hikaru_mesh_t		*mesh_list[8][8];
	uint32_t		 num_meshes[8][8];
	uint32_t		 total_meshes;

	struct {
		unsigned		num_verts, num_tris;
		hikaru_vertex_t		tmp[4];
		hikaru_vertex_body_t	all[MAX_VERTICES_PER_MESH];
	} push;

	struct {
		hikaru_mesh_t		*current;

		hikaru_glsl_variant_t	variant;
		GLuint			program;
		GLuint			vao;
		/* The frame's vertices, all meshes', in one buffer: one
		 * upload a frame, not one buffer a mesh */
		std::vector<hikaru_vertex_body_t> verts;
		GLuint			vbo;
		/* Its storage's size: the largest frame's yet */
		size_t			vbo_size;

		struct {
			GLuint		u_projection;
			GLuint		u_modelview;
			GLuint		u_normal;
			GLuint		u_link_modelview;
			GLuint		u_link_normal;
			struct {
				GLuint	position;
				GLuint	direction;
				GLuint	diffuse;
				GLuint	cone;
				GLuint	extents;
				GLuint	ambient;
				GLuint	table;
			} u_lights[4];
			GLuint		u_ambient;
			GLuint		u_texture;
			GLuint		u_fog;
			GLuint		u_fog_color;
			GLuint		u_alpha_range;
			GLuint		u_depth;
			GLuint		u_pattern;
			GLuint		u_max_nodes;
			GLuint		u_tag;
			GLuint		u_opaque_depth;
		} locs;
	} meshes;

	/* Translucency: the translucent meshes' fragments kept in a list a
	 * pixel, then blended from the farthest (OpenGL 4.3, without MSAA;
	 * else each mesh sorted whole). See "Translucency" in the .cpp */
	struct {
		bool checked;		/* the card asked, at the first scene */
		bool available;		/* the card's OpenGL has it */
		bool on;		/* for this frame's scene */
		bool collecting;	/* the meshes drawn now go to the lists */
		uint32_t tag;		/* the mesh's: blending mode, order << 18 */
		unsigned w, h;
		/* Each pixel's last fragment + 1 (0: none), the fragments
		 * (uvec4: colour, depth, the next + 1, tag), their count */
		GLuint heads, nodes, counter;
		uint32_t max_nodes;
		/* The opaque meshes' depth, copied for the shader's own test;
		 * the picture before the lists, which they are blended over */
		GLuint depth_tex, depth_fb, color_tex, color_fb;
		GLuint resolve, vao;
		unsigned frames;
	} oit;

	struct {
		hikaru_texture_t cache[2][0x40][0x80];
		bool is_clear[2];
		/* The IDMA's writes to each 16x16 slot: a cached texture is
		 * uploaded again only when its own slots were written. Each
		 * write used to clear the bank's whole cache: BRAVEFF's demo,
		 * uploading textures every frame, recreated hundreds of them a
		 * frame, until the GPU ran out of memory */
		uint32_t slot_writes[2][0x40][0x80];
	} textures;

	struct {
		GLuint program;
		GLuint fb[2], cb[2], db[2];
		/* The scene's buffers: scene_w x 480 times RENDER_SCALE, built
		 * again when it or MSAA changes; with MSAA, the scene drawn in
		 * ms_fb, then resolved into fb[] */
		unsigned w, h, scale, samples;
		GLuint ms_fb, ms_cb, ms_db;
		/* What the graphics card can: MSAA's samples, the anisotropy */
		int max_samples;
		float max_aniso;
		/* The post effects: FXAA's picture, the buffers' size; the
		 * bloom's two, a quarter of the scene's */
		hikaru_target_t post, bloom[2];
		/* The frames' blending (781/181) in integers, as the board's
		 * blender: a copy of the picture drawn, and its program */
		hikaru_target_t drawn;
		struct {
			GLuint program;
			GLint u_drawn, u_kept, u_rate;
		} blend;
		struct {
			GLuint program;
			GLuint u_texture, u_texel, u_down;
		} fxaa, bright, blur;
		GLuint vao, vbo;
		/* The layers' textures, kept from frame to frame, and the
		 * format of their storage (0 none yet) */
		GLuint layers[2];
		unsigned layer_formats[2];
		/* The GPU's command lists begun when the scene was last drawn:
		 * a frame without one keeps that picture */
		uint32_t lists_drawn;
		struct {
			GLuint u_front;
			GLuint u_back;
			GLuint u_layer1;
			GLuint u_layer2;
			GLuint u_factor;
			GLuint u_mult_f;
			GLuint u_mult_b;
			GLuint u_mult_1;
			GLuint u_mult_2;
			GLuint u_order;
			GLuint u_test;
			GLuint u_invert_f;
			GLuint u_invert_b;
			GLuint u_layer_x;
			GLuint u_down;
			GLuint u_texel;
			GLuint u_bloom;
			GLuint u_bloom_k;
			GLuint u_crt;
			GLuint u_out;
			GLuint u_barrel;
			GLuint u_frame;
			GLuint u_barrel1;
			GLuint u_barrel2;
		} locs;
	} framebuffer;

	struct {
		int32_t flags[HR_NUM_DEBUG_VARS];
		/* HR_NO_DRAW: no mesh sent to the GPU (a frame that hangs it);
		 * HR_STATS: each frame's draws and triangles on stderr;
		 * HR_SYNC: each mesh waited for and timed, the slow ones
		 * printed */
		bool no_draw, stats, sync;
		uint64_t draws, tris;
		uint32_t max_tris, max_instances, max_tris_mesh, slow_mesh;
		uint32_t uploads;	/* textures uploaded this frame */
		double gpu_ms, slow_ms;
	} debug;

private:
	struct Thread;
	/* THREADED_RENDERING's thread, NULL without */
	Thread *thread;
	/* Its copies of the TEXRAM and the framebuffer; the TEXRAM's 16x16
	 * slots written since the last frame was handed over */
	hl::Buffer *texram_shadow[2], *fb_shadow;
	bool texram_dirty[2][0x40][0x80];

	/* The drawing thread's renderer, its lists and GL objects its own */
	explicit Renderer (Renderer *owner);
	bool is_drawer;
	bool start_thread (hl::Buffer *fb, hl::Buffer *texram[2]);
	void stop_thread ();
	static void thread_main (Renderer *owner);
	void wait_thread (bool rethrow);
	void hand_over ();
	void draw_job ();
	void show (unsigned i);
	static bool flush (void *data);
};

#define LOG(fmt_, ...) \
	do { \
		if (hr->debug.flags[HR_DEBUG_LOG]) \
			fprintf (stdout, "\tHR: " fmt_ "\n", ##__VA_ARGS__); \
	} while (0)

uint32_t	rgba1111_to_rgba4444 (uint8_t pixel);
uint16_t	abgr1555_to_rgba5551 (uint16_t pixel);
uint16_t	abgr4444_to_rgba4444 (uint16_t pixel);
uint32_t	a8_to_rgba8888 (uint32_t a);

} /* namespace hikaru */

#endif /* __HIKARU_RENDERER_PRIVATE_H__ */

