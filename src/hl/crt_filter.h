/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/video/picture_filter_gl.hpp,
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

#ifndef __HL_CRT_FILTER_H__
#define __HL_CRT_FILTER_H__

#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hl/core.h"
#include "hl/crt_preset.h"

namespace hl {

/* CRT_PRESET's passes (hl/crt_preset.h): each a GLSL file with a VERTEX and
 * a FRAGMENT part, each drawing into a texture the next ones read, the last
 * into the target. They run by the rules of 86Box's OpenGL renderer, which
 * the Lola Retro presets are written for (data/crt/lola-retro):
 *  - every texture is rounded up to a power of two, its picture in its
 *    lower left corner, the right way up; TexCoord spans the picture;
 *  - a pass is as large as its input (scale_type source), the viewport
 *    (viewport) or a number of pixels (absolute), times its scale;
 *    float_framebuffer gives it colours of 32-bit floats;
 *  - the texture a pass draws is read with the next pass's filter_linear
 *    and its own wrap_mode;
 *  - the uniforms are Texture, InputSize, TextureSize, OutputSize (the
 *    viewport, for every pass), FrameCount, FrameDirection, MVPMatrix and
 *    each parameter by its name; from the second pass also OrigTexture
 *    with its sizes, and PassNTexture (what pass N - 1 drew) with the
 *    sizes of its input;
 *  - the last pass has no scale and draws straight into the viewport.
 * One per GL context, made and used on the thread whose context is current
 * (the drawing thread's with THREADED_RENDERING). */
class CrtFilter {
public:
	/* The filter of a preset file, its passes built; NULL, logged, when
	 * the preset cannot be read, a pass does not build, or the context
	 * lacks what the passes need (GLSL 1.50, sampler objects) */
	static std::unique_ptr<CrtFilter> load (const std::string &preset);
	~CrtFilter ();
	CrtFilter (const CrtFilter &) = delete;
	CrtFilter &operator= (const CrtFilter &) = delete;

	/* The framebuffer to draw the machine's picture into, w x h pixels
	 * from its lower left corner, the right way up */
	GLuint	 frame (unsigned w, unsigned h);
	/* That picture through the passes, the last one into target_fb's
	 * viewport (x, y, w, h, from the bottom left), cut to clip (the
	 * same way; NULL: none); count, the frames drawn so far (FrameCount) */
	void	 draw (GLuint target_fb, const int viewport[4], const int *clip, unsigned count);

private:
	using Scale = crt_preset::Plan::Scale;

	/* A texture a pass reads: what it holds, its size, the size of the
	 * picture in it, how it is read */
	struct Picture {
		GLuint texture = 0, fb = 0;
		std::array<float, 2> size {}, texture_size {};
		bool linear = true, is_float = false;
		GLint wrap = GL_CLAMP_TO_BORDER;
	};

	struct Pass {
		GLuint program = 0, vao = 0, vbo = 0;
		std::array<Scale, 2> scale_type { Scale::SOURCE, Scale::SOURCE };
		std::array<float, 2> scale { 1.0f, 1.0f };
		bool last = false;	/* into the target */
		Picture out;
		/* The uniforms and the parameters it uses */
		GLint texture, input_size, texture_size, output_size, frame_count, frame_direction, mvp;
		GLint orig_texture, orig_input_size, orig_texture_size;
		std::vector<std::array<GLint, 3>> passes;	/* PassNTexture, ...TextureSize, ...InputSize */
		std::vector<std::pair<GLint, float>> parameters;
	};

	CrtFilter () = default;
	bool	 build (const crt_preset::Plan &plan);
	void	 find_uniforms (Pass &pass, size_t i, const std::vector<std::pair<std::string, float>> &parameters);
	void	 make_corners (Pass &pass);
	void	 size (unsigned w, unsigned h);
	GLuint	 sampler (bool linear, GLint wrap);

	std::vector<Pass> passes_;
	Picture frame_;
	std::array<unsigned, 2> frame_size_ {}, viewport_ {};
	std::map<std::pair<bool, GLint>, GLuint> samplers_;
};

} /* namespace hl */

#endif /* __HL_CRT_FILTER_H__ */
