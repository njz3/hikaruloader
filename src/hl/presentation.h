/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * The blurred sides derived from Odin's src/odin/host/gfx/glsl/sides.frag,
 * Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __HL_PRESENTATION_H__
#define __HL_PRESENTATION_H__

#include <memory>
#include <string>

#include "hl/aspect.h"
#include "hl/bezel.h"
#include "hl/core.h"
#include "hl/crt_filter.h"

namespace hl {

/* What becomes of a machine renderer's composed picture (the 3D and the 2D
 * layers) on its way to the window, beyond the aspect ratio's layout:
 * CRT_PRESET's passes (hl/crt_filter.h), drawn in CRT_FILTER's place, and
 * BEZEL, what is around it: the picture's own edges blurred and dimmed, or
 * the game's bezel picture over all (hl/bezel.h). One per GL context that
 * draws the frames (the drawing thread's with THREADED_RENDERING), its GL
 * objects made and deleted there. */
class Presentation {
public:
	Presentation ();
	~Presentation ();
	Presentation (const Presentation &) = delete;
	Presentation &operator= (const Presentation &) = delete;

	/* The settings and the game's bezel picture at the frame's end, on the
	 * thread that runs the machine (with THREADED_RENDERING, while the
	 * drawing thread waits): the next frame's */
	void	 copy_view ();
	/* Before the composition: the framebuffer to compose the picture into,
	 * w x h pixels from its bottom left corner, when a CRT preset takes it
	 * (the preset loaded at its first frame), the composition then without
	 * CRT_FILTER nor CROP_BARREL; 0 without, the picture composed into the
	 * target as before */
	GLuint	 begin (unsigned w, unsigned h);
	/* After it: the preset's passes into the target's picture (l.image,
	 * cut to l.frame), then BEZEL's sides or picture; the target
	 * target_fb, tw x th. Leaves no program, blending nor scissor on */
	void	 end (GLuint target_fb, int tw, int th, const ScreenLayout &l);

private:
	void	 draw_sides (GLuint target_fb, int tw, int th, const ScreenLayout &l);
	void	 draw_bezel (GLuint target_fb, const ScreenLayout &l);
	void	 build_programs ();

	/* copy_view ()'s */
	std::string	 preset_;
	int		 bezel_mode_ = 0;
	std::shared_ptr<const BezelImage> bezel_;

	/* The preset's filter: loaded for filter_preset_ (failed: not tried
	 * again until the setting changes); the frames it drew */
	std::unique_ptr<CrtFilter> filter_;
	std::string	 filter_preset_;
	unsigned	 filter_frames_ = 0, frame_w_ = 0, frame_h_ = 0;
	bool		 filtered_ = false;

	/* The bezel's texture, made from bezel_made_'s picture */
	GLuint		 bezel_texture_ = 0;
	std::shared_ptr<const BezelImage> bezel_made_;
	/* The sides: the shown picture copied, its levels made smaller */
	GLuint		 sides_texture_ = 0, sides_fb_ = 0;
	int		 sides_w_ = 0, sides_h_ = 0, sides_levels_ = 0;
	/* The programs (a triangle over the viewport), their uniforms */
	GLuint		 vao_ = 0, image_program_ = 0, sides_program_ = 0;
	GLint		 u_rect_ = -1, u_band_ = -1, u_lod_ = -1, u_light_ = -1;
};

} /* namespace hl */

#endif /* __HL_PRESENTATION_H__ */
