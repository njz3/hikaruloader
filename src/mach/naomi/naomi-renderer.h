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

#ifndef __HL_NAOMI_RENDERER_H__
#define __HL_NAOMI_RENDERER_H__

#include <memory>
#include <unordered_map>
#include <vector>

#include "hl/presentation.h"
#include "hl/renderer.h"
#include "hl/window.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/* What a board shows: a texture, the rows bottom up or not; 0 for nothing */
struct Picture {
	GLuint texture = 0;
	bool flip = false;
};

/* Draws one board's Holly: its scenes at RENDER_SCALE x 640x480, those to a
 * texture at their own size back into its VRAM; see naomi-renderer.cpp */
class BoardRenderer {
public:
	BoardRenderer (Holly *holly, unsigned width, unsigned height);
	~BoardRenderer ();

	BoardRenderer (const BoardRenderer &) = delete;
	BoardRenderer &operator= (const BoardRenderer &) = delete;

	/* Zero-filled, as hl::Renderer's */
	static void *operator new (size_t size)
	{
		void *ptr = ::operator new (size);
		memset (ptr, 0, size);
		return ptr;
	}
	static void operator delete (void *ptr) { ::operator delete (ptr); }

	/* This frame's scenes */
	Picture	 draw_picture ();
	/* A picture over the bound framebuffer's viewport */
	void	 blit (const Picture &p);

private:
	struct Texture {
		GLuint id;
		uint64_t hash;
		uint64_t seen;	/* the last frame that checked its data */
	};

	/* A polygon program's uniforms */
	struct Program {
		GLuint id;
		GLint u_flags, u_tex, u_fog, u_zscale, u_fog_color, u_fog_color_vertex, u_fog_density;
		GLint u_width, u_stretch_2d, u_clamp_max, u_clamp_min;
		GLint u_scale, u_target, u_pt_ref, u_lod_bias, u_max_lod, u_shade_scale, u_clip_out;
		/* The collecting one's (else -1) */
		GLint u_tag, u_max_nodes, u_opaque_depth, u_node_flags;
	};

	/* A framebuffer with a depth and a stencil */
	struct Target {
		GLuint fb = 0, color = 0, depth = 0;
		unsigned w = 0, h = 0;
	};

	void setup ();
	void make_target (Target &t, unsigned w, unsigned h);
	void draw_scene (const Draw &d, Target &t, bool to_texture);
	void draw_batch (const Program &p, const Draw &d, const Batch &b, bool opaque, float sx, float sy);
	void draw_piece (const Program &p, const Batch &b, uint32_t tsp, uint32_t tcw, uint32_t first, bool shadowed,
	                 bool opaque);
	void draw_modifiers (const Draw &d, float sx, float sy);
	void background_vertices (const Draw &d, Vertex *quad) const;
	void set_scene_uniforms (const Program &p);
	bool oit_buffers (unsigned w, unsigned h);
	void destroy_oit ();
	void draw_translucent_per_pixel ();
	void draw_translucent_volumes ();
	void write_back (const Draw &d, Target &t);
	bool show_vram ();
	GLuint filter_lines (const Draw &d);
	GLuint texture (uint32_t tcw, uint32_t tsp);

	Holly *holly;
	unsigned width, height;
	/* The drawing going on */
	const Draw *draw = nullptr;
	Target *target = nullptr;
	float pixel_x = 1.0f, pixel_y = 1.0f;
	int punch_flag = 0;
	bool depth_pass = false;
	/* The stencil has the modifier volumes' area 1 in bit 0 */
	bool volumes_ready = false;
	/* The translucent volumes will choose between the two areas' fragments */
	bool collect_areas = false;
	/* Where Scene::vertices1 starts in the vertex buffer */
	uint32_t vertices1_at = 0;
	float pixel_scale_x () const { return (float) target->w / pixel_x; }
	float pixel_scale_y () const { return (float) target->h / pixel_y; }
	std::unordered_map<uint64_t, Texture> textures;
	uint64_t frame_count = 0;

	Program program = {}, collect = {};
	GLuint blit_program, mod_program;
	GLint u_blit_tex, u_mod_zscale, u_mod_width, u_mod_scale, u_mod_target;
	GLuint vao, vbo, mod_vao, mod_vbo, blit_vao, fog_tex, vram_tex;

	Target screen, offscreen, filtered;
	GLuint filter_program = 0;
	GLint u_filter_tex, u_filter_line, u_filter_coef;
	GLuint shown_color = 0;	/* the last picture for the screen */
	/* The frames since a scene was drawn for the screen */
	unsigned frames_without = 1000;

	/* The translucent polygons sorted at each pixel, as Gaelco's */
	struct {
		GLuint heads = 0, nodes = 0, counter = 0;
		GLuint depth_tex = 0, depth_fb = 0, color_tex = 0, color_fb = 0;
		GLuint resolve = 0, vao = 0;
		/* The translucent modifier volumes' program */
		GLuint volumes = 0;
		GLint u_mode = -1, u_zscale = -1, u_width = -1, u_scale = -1, u_target = -1;
		unsigned w = 0, h = 0;
		uint32_t max_nodes = 0;
		unsigned frames = 0;
		bool failed = false;
	} oit;
};

/* The machine's renderer: a BoardRenderer for each board shown, their pictures
 * where [Display] MULTI_SCREEN wants them: one board's in the window, all of
 * them side by side in it, or each in a window of its own */
class Renderer : public hl::Renderer {
public:
	explicit Renderer (Naomi *naomi);
	~Renderer () override;

	/* The windows' titles: which board each shows */
	void	 update_titles ();

protected:
	void	 on_end_frame () override;

private:
	struct Output {
		std::unique_ptr<hl::Presentation> presentation;
		std::unique_ptr<hl::ExtraWindow> window;	/* none: the main one */
	};

	BoardRenderer *drawer (unsigned board);
	void	 open_windows ();
	void	 show (Output &out, const Picture &p, BoardRenderer *blitter, unsigned w, unsigned h);
	Picture	 side_by_side (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures);

	Naomi	*naomi;
	std::vector<std::unique_ptr<BoardRenderer>> drawers;
	std::vector<Output> outputs;
	GLuint	 wide_fb = 0, wide_color = 0;
	unsigned wide_w = 0, wide_h = 0;
};

} /* namespace naomi */

#endif /* __HL_NAOMI_RENDERER_H__ */
