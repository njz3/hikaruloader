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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hl/presentation.h"
#include "hl/renderer.h"
#include "hl/window.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/* What a board shows: a texture, the rows bottom up or not; 0 for nothing.
 * x0, x1: the part of its width shown */
struct Picture {
	GLuint texture = 0;
	bool flip = false;
	float x0 = 0.0f, x1 = 1.0f;
	/* Its depth, when drawn from a scene (MULTI_SCREEN_VIEW's strips): a
	 * depth texture of the same size, a fragment's 1/W its value times
	 * depth_unit (0: none) */
	GLuint depth = 0;
	float depth_unit = 0.0f;
	/* MULTI_SCREEN_VIEW's middle board: its scene drawn too in the left
	 * and the right screens' views (BoardRenderer::View::strips), each with
	 * its depth, for their strips (Renderer::fill_strips ()); 0: none */
	GLuint strip[2] = { 0, 0 }, strip_depth[2] = { 0, 0 };
};

/* MULTI_SCREEN_VIEW's flat, curved, dome and Panini pictures: the screens'
 * pictures, joined (side by side in a texture, panels turned by twice half
 * from each other), seen through another projection; in pixels of a game's
 * screen */
struct Projection {
	int	 mode = 0;	/* hl::VIEW_* */
	unsigned panels = 1;
	float	 panel_w = 640.0f, panel_h = 480.0f;
	float	 focal = 1.0f, centre_y = 240.0f, half = 0.5f;
	/* The focal length of the picture made, out_focal () */
	float	 out_focal = 1.0f;
	/* The part of the whole picture's width drawn */
	float	 x0 = 0.0f, x1 = 1.0f;
};

/* The smallest focal length showing the panels' whole width without a
 * black area beyond their pictures (Projection::out_focal) */
float	 out_focal (const Projection &p);

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

	/* The view its scenes for the screen are drawn in (MULTI_SCREEN_VIEW):
	 * their 3D turned by turn radians about the vertical through the eye
	 * (to the right), as the game would draw it with that camera; focal,
	 * centre_y: the game's projection, in pixels of its screen; focal 0:
	 * as the game draws it; out_focal: the one drawn with (larger: the
	 * picture enlarged) */
	struct View {
		float turn = 0.0f, focal = 0.0f, centre_y = 240.0f, out_focal = 0.0f;
		/* rotated: the turn about another axis, rot (rows first; turn its
		 * part about the vertical): a side board's when the master's
		 * camera alone is shaken (Renderer::views ()) */
		bool rotated = false;
		float rot[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		/* The middle board's, in 4:3: its scene drawn again in the left
		 * and the right screens' views, turned by strip_turn[0] and [1]
		 * (Picture::strip), its 2D but the layers as wide as the screen
		 * left out */
		bool strips = false;
		float strip_turn[2] = { 0.0f, 0.0f };
		/* A side screen's strip (Renderer::fill_strips ()): the cells of
		 * MASK_CELL pixels of its 640x480 its 2D covers, MASK_W x MASK_H, the
		 * top row first; empty: none */
		enum { MASK_CELL = 4, MASK_W = 640 / MASK_CELL, MASK_H = 480 / MASK_CELL };
		std::vector<uint8_t> mask_2d;
	};

	/* What a frame's drawing takes of the board, on the machine's thread:
	 * its scenes (copies of what they read), and the framebuffer the game
	 * shows when it has had no scene for the screen for a while; the view
	 * they are drawn in */
	struct Frame {
		std::vector<Draw> draws;
		bool to_texture = false, has_screen = false;
		bool vram = false;
		unsigned vram_w = 0, vram_h = 0;
		/* No scene for a while, but the display reads a framebuffer one
		 * was drawn to, untouched since (Holly::shows_rendered ()): the
		 * last picture drawn shown on, not the VRAM (a game showing its
		 * loading screen, drawn once in a while) */
		bool rendered = false;
		/* The framebuffer's lines as read (vram_line bytes each, their
		 * words in order), turned into colours where they are drawn
		 * (show_vram ()): the machine's thread spent 0.36 ms a frame on
		 * Extreme Hunting's screens without a scene */
		std::vector<uint8_t> vram_raw;
		unsigned vram_line = 0, vram_bytes = 2, vram_depth = 0;
		View view;
	};
	/* quiet: the frames without a scene for the screen, this one counted */
	static Frame take (Holly *holly, unsigned &quiet);
	/* This frame's, as draw_picture () takes it */
	Frame	 take_frame ();
	/* The frame drawn: its picture (a scene to a texture back into the
	 * VRAM, the machine waiting) */
	Picture	 draw_frame (Frame &f);
	/* draw_frame's work, which may run out of memory */
	Picture	 draw_frame_now (Frame &f);
	/* Called once a frame's scenes drawn to textures are back in the
	 * VRAM, before its screen's (the drawing thread's: the machine goes
	 * on from there) */
	std::function<void ()> textures_done;
	/* This frame's scenes, in that view */
	Picture	 draw_picture (const View &view = View ());
	/* A picture over the bound framebuffer's viewport */
	void	 blit (const Picture &p);
	/* The joined screens' picture src (Projection) over the bound
	 * framebuffer's viewport */
	void	 project (const Picture &src, const Projection &pr);
	/* Over a side screen's joined picture (the bound viewport, side's
	 * picture), the middle board's picture in that screen's view (middle's
	 * strip[k]) where the two boards' views do not cover each other
	 * (MULTI_SCREEN_VIEW: the cabinet's gap between monitors, up to edge,
	 * the side board's own view's edge): the nearer of the two, the
	 * middle's up to cut where they draw the same; the side's where its
	 * 2D is (mask_2d: View's, NULL none). cut, edge: pixels from the middle
	 * view's centre; turn: the side screen's from the middle one; focal:
	 * both pictures' */
	void	 fill_strip (const Picture &middle, int k, const Picture &side, const std::vector<uint8_t> *mask_2d,
		             float panel_w, float panel_h, float focal, float turn, float cut, float edge);
	/* WIDESCREEN switched: the screen's scenes that wide from the next */
	void	 set_width (unsigned w) { width = w; }

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
		GLint u_width, u_stretch_2d, u_stretch_all, u_clamp_max, u_clamp_min;
		GLint u_scale, u_target, u_pt_ref, u_lod_bias, u_max_lod, u_shade_scale, u_clip_out;
		GLint u_view, u_view_c, u_view_f, u_view_m, u_strip_pass;
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
	/* ELAN_LIGHTING GPU: the scene's vertices to light (Scene::lighting)
	 * lit, their colours put in vbo, as draw_scene () filled it */
	void light_scene (const Scene &scene);
	void elan_scene (const Scene &scene);
	void set_scene_uniforms (const Program &p);
	/* The view turns d's 3D (View): a scene for the screen */
	bool view_on (const Draw &d) const;
	/* The view's uniforms (View) of a program drawing d: u_view, u_view_c */
	void set_view_uniforms (const Draw &d, GLint u_view, GLint u_view_c, GLint u_view_f, GLint u_view_m) const;
	bool oit_buffers (unsigned w, unsigned h);
	void destroy_oit ();
	void draw_translucent_per_pixel ();
	void draw_translucent_volumes ();
	void write_back (const Draw &d, Target &t);
	static bool read_vram (Holly *holly, Frame &f);
	bool show_vram (const Frame &f);
	std::vector<uint32_t> vram_rgba;
	GLuint filter_lines (const Draw &d);
	GLuint texture (uint32_t tcw, uint32_t tsp);

	Holly *holly;
	unsigned width, height;
	/* The drawing going on, its view */
	const Draw *draw = nullptr;
	View view;
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
	GLint u_blit_tex, u_blit_span, u_blit_flip, u_mod_zscale, u_mod_width, u_mod_scale, u_mod_target;
	GLint u_mod_view, u_mod_view_c, u_mod_view_f, u_mod_view_m;
	/* Projection's */
	GLuint proj_program = 0;
	GLint u_proj_tex, u_proj_mode, u_proj_panels, u_proj_panel, u_proj_focal, u_proj_cy, u_proj_half;
	GLint u_proj_out_focal, u_proj_slot;
	/* fill_strip ()'s */
	GLuint strip_program = 0;
	GLint u_strip_tex, u_strip_panel, u_strip_focal, u_strip_turn, u_strip_edge, u_strip_cut;
	GLint u_strip_depth, u_strip_side_depth, u_strip_units, u_strip_side_span, u_strip_mask, u_strip_has_mask;
	GLuint strip_mask_tex = 0;
	GLuint vao, vbo, mod_vao, mod_vbo, blit_vao, fog_tex, vram_tex;
	/* light_scene ()'s compute shaders, the lighting's and the colouring's;
	 * the buffers of the light sets, the vertices to light, their colours,
	 * the clipped points and the vertices to colour */
	GLuint light_program = 0, color_program = 0;
	GLint u_light_count = -1, u_color_count = -1;
	GLuint light_buffers[5] = {};
	/* elan_scene ()'s: the points' and the triangles' passes; the buffers
	 * of the light sets, the lists' words, each vertex's list, the lists,
	 * the points, the triangles */
	GLuint elan_points_program = 0, elan_triangles_program = 0;
	GLint u_points_count = -1, u_triangles_count = -1, u_triangles_base = -1;
	GLuint elan_buffers[6] = {};
	/* The vertex buffer's first of them (draw_scene ()) */
	uint32_t elan_at = 0;
	uint32_t first_of (const Batch &b) const { return b.elan ? elan_at + b.first : b.first; }
	/* The vertices' 2D marks while the view turns (draw_scene ()) */
	GLuint flag_vbo;
	std::vector<uint8_t> flags;
	/* The middle board's scene in the side screens' views (View::strips),
	 * being drawn: its HUD left out (vs_src) */
	bool strip_pass = false;
	Target strip_targets[2];

	Target screen, offscreen, filtered;
	GLuint filter_program = 0;
	GLint u_filter_tex, u_filter_line, u_filter_coef;
	GLuint shown_color = 0;	/* the last picture for the screen */
	float shown_depth_unit = 0.0f;	/* its depth's (Picture::depth_unit) */
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
		/* The resolving program's */
		GLint u_below = -1;
		GLint u_view = -1, u_view_c = -1, u_view_f = -1, u_view_m = -1;
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

	/* The windows' titles: which board each shows, the view they are drawn
	 * in (MULTI_SCREEN_VIEW) */
	void	 update_titles ();
	void	 apply_multi_screen () override;
	void	 apply_widescreen () override;

	/* THREADED_RENDERING: the pictures drawn by a thread of their own */
	struct Thread;

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
	Picture	 side_by_side (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures,
		               BoardRenderer *with);
	/* MULTI_SCREEN_VIEW: each shown board's view for these frames; all as
	 * the game's when the game has none measured, the boards are not its
	 * screens, or the scene is not one of the views measured */
	std::vector<BoardRenderer::View> views (const std::vector<unsigned> &shown,
	                                        const std::vector<BoardRenderer::Frame> &frames) const;
	/* The side views' turn from the middle one, the focal length and the
	 * centre row, solved on the points the boards both draw; false: a scene
	 * of other cameras (enough: there were points enough to tell) */
	bool	 solve_views (const std::vector<BoardRenderer::Frame> &frames, const Draw *middle, float *yaw, float *focal,
		              float *centre, bool *enough) const;
	/* The same, steadied from frame to frame */
	bool	 steady_views (const std::vector<BoardRenderer::Frame> &frames, float *yaw, float *focal,
		               float *centre) const;
	mutable float steady_yaw = 0.0f, steady_f = 0.0f, steady_cy = 240.0f;
	mutable unsigned views_left = 0, views_frames = 0;
	mutable bool views_steady = false, views_solved = false;
	/* Both side boards' scenes without 3D (solve_views ()): nothing there
	 * the views would turn, nor against them */
	mutable bool views_bare = false;
	/* The game has views and they are wanted for the boards shown */
	bool	 views_wanted (const std::vector<unsigned> &shown) const;
	/* The middle board's last scene for the screen (held_valid), shown
	 * (held_shown); the side boards a frame behind it (middle_late): its
	 * picture shown a frame late (late_middle ()) */
	Draw	 held_middle;
	bool	 held_valid = false, held_shown = true;
	mutable bool middle_late = false;
	/* The master's camera shaken alone (solve_shake ()): the middle view's
	 * rotation from the rig's, rows first */
	mutable bool views_shaken = false;
	mutable float views_shake[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	void	 late_middle (const std::vector<unsigned> &shown, std::vector<BoardRenderer::Frame> &frames,
		              const std::vector<BoardRenderer::View> &v);
	/* The titles told the views of this frame's drawing */
	void	 note_views (const std::vector<unsigned> &shown, const std::vector<BoardRenderer::View> &v);
	/* What the titles say of them: " (16:9 Panini)", " (16:9 Panini: off in
	 * this scene)", or nothing */
	std::string view_label;
	/* The projection the boards' joined pictures, drawn in those views, go
	 * through; mode 0: none (CABINET, JOINED, a scene of other cameras) */
	Projection projection (const std::vector<BoardRenderer::View> &drawn_in) const;
	/* The side screens' strips from the middle board drawn wider */
	void	 fill_strips (const std::vector<Picture> &pictures, const std::vector<BoardRenderer::View> &v,
		              BoardRenderer *with);
	/* The joined pictures through it, in the projection's texture */
	Picture	 projected (const Picture &wide, const Projection &pr, BoardRenderer *with);
	/* The pictures where MULTI_SCREEN wants them; with: the blitter
	 * (NULL: each board's) */
	void	 present_pictures (const std::vector<unsigned> &shown, const std::vector<Picture> &pictures,
		                   BoardRenderer *with, const std::vector<BoardRenderer::View> &drawn_in);

	bool	 start_thread ();
	void	 stop_thread ();
	void	 wait_thread (bool rethrow);
	/* A frame drawn to textures: the machine goes on at once, the VRAM
	 * those are written back to guarded meanwhile (hl::Mmap::guard (),
	 * Holly::vram_sync): the machine waits for them there, as late as it
	 * can, instead of at the frame's end. The VRAM's offsets written, a
	 * pair a board */
	std::vector<std::vector<std::pair<uint32_t, uint32_t>>> rtt_ranges;
	std::vector<hl::Mmap *> rtt_guarded;
	bool	 rtt_pending = false;
	bool	 rtt_defer (const std::vector<unsigned> &shown);
	void	 rtt_wait ();
	/* rtt_wait () when bytes from offs of board's VRAM are written back */
	void	 rtt_wait (unsigned board, uint32_t offs, uint32_t bytes);
	/* Holly::vram_later's: the textures to take from the VRAM once those
	 * pixels are back (under the thread's lock); fill_later () takes them */
	struct Later {
		unsigned board;
		uint32_t addr, bytes;
		std::shared_ptr<TextureData> data;
	};
	std::vector<Later> rtt_later;
	bool	 rtt_texture_later (unsigned board, const std::shared_ptr<TextureData> &data, uint32_t addr,
		                    uint32_t bytes);
	void	 fill_later ();
	void	 sync () override { rtt_wait (); }
	static void thread_main (Renderer *owner);
	void	 draw_job ();
	void	 threaded_end_frame (const std::vector<unsigned> &shown);
	void	 present (unsigned set);
	/* On the thread, bound to the window: the set it drew, shown and
	 * swapped there (Thread::present) */
	void	 present_on_thread (unsigned set);
	static bool flush (void *data);

	Naomi	*naomi;
	std::vector<std::unique_ptr<BoardRenderer>> drawers;
	std::vector<Output> outputs;
	GLuint	 wide_fb = 0, wide_color = 0;
	unsigned wide_w = 0, wide_h = 0;
	GLuint	 proj_fb = 0, proj_color = 0;
	unsigned proj_w = 0, proj_h = 0;
	/* The monitors the windows were opened on (SCREEN_<n>_DISPLAY) */
	std::string displays;

	Thread	*thread = nullptr;
	/* The window's context's, for the thread's pictures */
	std::unique_ptr<BoardRenderer> blitter;
	/* Each board's frames without a scene for the screen (Frame) */
	std::vector<unsigned> quiet;
};

} /* namespace naomi */

#endif /* __HL_NAOMI_RENDERER_H__ */
