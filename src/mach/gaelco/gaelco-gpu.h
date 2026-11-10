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

#ifndef __HL_GAELCO_GPU_H__
#define __HL_GAELCO_GPU_H__

#include <memory>
#include <unordered_map>
#include <vector>

#include "hl/device.h"
#include "hl/presentation.h"
#include "hl/renderer.h"

namespace gaelco {

/* A frame's lists, read from the VRAM when the game starts its drawing: the
 * vertices in the board's screen coordinates, and the runs that share a
 * polygon header */
struct Vertex {
	float x, y, z;		/* z: 1/w */
	float u, v;
	uint32_t base;		/* A8R8G8B8 */
	uint32_t offset;
};

struct Batch {
	uint32_t isp, tsp, tcw;
	uint32_t first, count;
	bool translucent;
};

struct Frame {
	std::vector<Vertex> vertices;
	std::vector<Batch> batches;
	float max_z;
	uint32_t background;
	unsigned stride;	/* in order textures' lines, in texels */
	/* The chip state's fog: its colours (the table's, the vertices'),
	 * density, and the table; the colour clamp's bounds (ARGB) */
	uint32_t fog_color;
	uint32_t fog_color_vertex;
	uint32_t fog_density;
	uint16_t fog_table[128];
	uint32_t clamp_max, clamp_min;
};

/* The NEC PowerVR Neon 250 (a PowerVR series 2, PMX1) on the slave SH-4: its
 * registers at 14000000-14003FFF, its PCI configuration at 14004xxx, its
 * 32 MB at 18000000; see gaelco-gpu.cpp */
class Neon250 : public hl::Device {
public:
	Neon250 (hl::Machine *mach, hl::Buffer *vram);

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;

	/* The frame to show, once a new one is there (else NULL): the
	 * renderer's from then on */
	std::unique_ptr<Frame> take_frame () { return std::move (ready); }
	/* The screen's vertical blank */
	void vblank ();

	/* The frames drawn, from the reset */
	uint32_t frames;

	hl::Buffer *vram;

private:
	void raise (unsigned bit);
	void update_irq ();
	void kick ();
	void run_ccb (uint32_t ccb);
	void build_frame (uint32_t region, uint32_t state);

	uint16_t pending;
	uint16_t mask;
	/* The next commands to read in the two rings (their write pointer, in
	 * the VRAM, for the first; a slot's word 6 for the second) */
	uint32_t ring_read;
	uint32_t list_read;
	std::unique_ptr<Frame> ready;
	/* What was written, for the log and the Lua shell */
	uint32_t regs[0x5000 / 4];
	/* The registers read and written so far, logged the first time */
	uint8_t seen_r[0x5000 / 4 / 8];
	uint8_t seen_w[0x5000 / 4 / 8];
};

/* Draws the Neon 250's frames, at RENDER_SCALE x 640x480 */
class Renderer : public hl::Renderer {
public:
	explicit Renderer (Neon250 *gpu);
	~Renderer () override;
	void	 apply_widescreen () override;

protected:
	void on_end_frame () override;

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
		/* The collecting one's (else -1) */
		GLint u_tag, u_max_nodes, u_opaque_depth;
	};

	void setup ();
	void draw_scene ();
	void draw_batch (const Program &p, const Batch &b);
	void set_scene_uniforms (const Program &p);
	bool oit_buffers (unsigned w, unsigned h);
	void destroy_oit ();
	void draw_translucent_per_pixel ();
	GLuint texture (uint32_t tcw, uint32_t tsp);

	Neon250 *gpu;
	std::unique_ptr<Frame> frame;
	/* Its translucent triangles sorted whole (without the lists) */
	bool frame_sorted = false;
	std::unique_ptr<hl::Presentation> presentation;
	std::unordered_map<uint64_t, Texture> textures;
	uint64_t frame_count;

	Program program = {}, collect = {};
	GLuint blit_program;
	GLint u_blit_tex;
	GLuint vao, vbo, blit_vao, fog_tex;
	GLuint fb, fb_color, fb_depth;
	unsigned fb_w, fb_h;

	/* The translucent polygons sorted at each pixel (see "Translucency"):
	 * each pixel's last fragment + 1, the fragments (uvec4: colour, depth,
	 * the next + 1, tag), their count; copies of the opaque depth and of
	 * the picture before them */
	struct {
		GLuint heads = 0, nodes = 0, counter = 0;
		GLuint depth_tex = 0, depth_fb = 0, color_tex = 0, color_fb = 0;
		GLuint resolve = 0, vao = 0;
		unsigned w = 0, h = 0;
		uint32_t max_nodes = 0;
		unsigned frames = 0;
		bool failed = false;
	} oit;
};

} /* namespace gaelco */

#endif /* __HL_GAELCO_GPU_H__ */
