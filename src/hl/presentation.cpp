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

#include <cmath>

#include "hl/config.h"
#include "hl/presentation.h"
#include "hl/renderer.h"

namespace hl {

/* A triangle over the viewport, its point of the picture from the bottom
 * left */
static const char *viewport_vs_source =
"#version 140\n"
"out vec2 p_uv;\n"
"void main (void)\n"
"{\n"
"	vec2 p = vec2 ((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);\n"
"	p_uv = p * 0.5 + 0.5;\n"
"	gl_Position = vec4 (p, 0.0, 1.0);\n"
"}\n";

/* The bezel's picture, its top row first in its texture */
static const char *image_fs_source =
"#version 140\n"
"uniform sampler2D u_texture;\n"
"in vec2 p_uv;\n"
"out vec4 frag_color;\n"
"void main (void)\n"
"{\n"
"	frag_color = texture (u_texture, vec2 (p_uv.x, 1.0 - p_uv.y));\n"
"}\n";

/* The sides lit by the picture, as Odin draws them: each pixel takes the
 * picture's edge nearest to it, at two of its levels, the larger near the
 * picture, smoothly (a B-spline through the texels), giving way to the
 * smaller and going from the light u_light.x at the picture to u_light.y at
 * the target's edge. u_rect, the picture in the target, from the bottom
 * left; u_band, how wide the sides are: left, below, right, above */
static const char *sides_fs_source =
"#version 140\n"
"uniform sampler2D u_texture;\n"
"uniform vec4 u_rect;\n"
"uniform vec4 u_band;\n"
"uniform vec2 u_lod;\n"
"uniform vec2 u_light;\n"
"out vec4 frag_color;\n"
"\n"
"/* Four filtered taps, each the weights of two texels of the B-spline\n"
" * each way */\n"
"vec3 smooth_texture (vec2 uv, float lod)\n"
"{\n"
"	vec2 size = vec2 (textureSize (u_texture, int (lod)));\n"
"	vec2 p = uv * size - 0.5;\n"
"	vec2 i = floor (p), f = p - i;\n"
"	vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;\n"
"	vec2 w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;\n"
"	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f * f - 3.0 * f * f * f) / 6.0;\n"
"	vec2 w3 = f * f * f / 6.0;\n"
"	vec2 g0 = w0 + w1, g1 = w2 + w3;\n"
"	vec2 h0 = (i - 0.5 + w1 / g0) / size, h1 = (i + 1.5 + w3 / g1) / size;\n"
"	return g0.y * (g0.x * textureLod (u_texture, h0, lod).rgb +\n"
"	               g1.x * textureLod (u_texture, vec2 (h1.x, h0.y), lod).rgb) +\n"
"	       g1.y * (g0.x * textureLod (u_texture, vec2 (h0.x, h1.y), lod).rgb +\n"
"	               g1.x * textureLod (u_texture, h1, lod).rgb);\n"
"}\n"
"\n"
"/* The point of the picture nearest to uv, at the middle of an edge texel\n"
" * at most */\n"
"vec2 edge (vec2 uv, float lod)\n"
"{\n"
"	vec2 half_texel = 0.5 / vec2 (textureSize (u_texture, int (lod)));\n"
"	return clamp (uv, half_texel, 1.0 - half_texel);\n"
"}\n"
"\n"
"void main (void)\n"
"{\n"
"	vec2 uv = (gl_FragCoord.xy - u_rect.xy) / u_rect.zw;\n"
"	vec2 lo = max (-uv, 0.0) * u_rect.zw / max (u_band.xy, 1.0);\n"
"	vec2 hi = max (uv - 1.0, 0.0) * u_rect.zw / max (u_band.zw, 1.0);\n"
"	float t = clamp (max (max (lo.x, lo.y), max (hi.x, hi.y)), 0.0, 1.0);\n"
"	vec3 near = smooth_texture (edge (uv, u_lod.x), u_lod.x);\n"
"	vec3 far = smooth_texture (edge (uv, u_lod.y), u_lod.y);\n"
"	frag_color = vec4 (mix (near, far, t) * mix (u_light.x, u_light.y, t), 1.0);\n"
"}\n";

/* The sides' light, at the picture and at the target's edge (Odin's); the
 * texels of the picture across its width at the larger level */
static const float SIDES_NEAR = 0.6f, SIDES_FAR = 0.05f;
static const float SIDES_TEXELS = 40.0f;

Presentation::Presentation ()
{
}

Presentation::~Presentation ()
{
	filter_.reset ();
	if (bezel_texture_)
		glDeleteTextures (1, &bezel_texture_);
	if (sides_fb_)
		glDeleteFramebuffers (1, &sides_fb_);
	if (sides_texture_)
		glDeleteTextures (1, &sides_texture_);
	if (vao_)
		glDeleteVertexArrays (1, &vao_);
	gl::destroy_program (image_program_);
	gl::destroy_program (sides_program_);
}

void
Presentation::copy_view ()
{
	Config *cfg = Config::get ();

	preset_ = cfg->crt_preset;
	bezel_mode_ = cfg->bezel;
	bezel_ = bezel_mode_ == BEZEL_IMAGE ? Bezel::image () : NULL;
}

GLuint
Presentation::begin (unsigned w, unsigned h)
{
	filtered_ = false;
	/* Loaded at its first frame, in the context that draws; a preset that
	 * fails is not tried again until the setting changes */
	if (preset_ != filter_preset_) {
		filter_preset_ = preset_;
		filter_.reset ();
		filter_frames_ = 0;
		if (!preset_.empty ()) {
			std::string path = crt_preset::find (preset_);

			if (path.empty ())
				HL_ERROR ("CRT_PRESET: '%s' not found in data/crt: not used", preset_.c_str ());
			else
				filter_ = CrtFilter::load (path);
		}
	}
	if (!filter_)
		return 0;
	frame_w_ = w;
	frame_h_ = h;
	filtered_ = true;
	return filter_->frame (w, h);
}

void
Presentation::build_programs ()
{
	if (vao_)
		return;
	glGenVertexArrays (1, &vao_);
	image_program_ = gl::compile_program (viewport_vs_source, image_fs_source);
	sides_program_ = gl::compile_program (viewport_vs_source, sides_fs_source);
	glUseProgram (image_program_);
	glUniform1i (glGetUniformLocation (image_program_, "u_texture"), 0);
	glUseProgram (sides_program_);
	glUniform1i (glGetUniformLocation (sides_program_, "u_texture"), 0);
	u_rect_ = glGetUniformLocation (sides_program_, "u_rect");
	u_band_ = glGetUniformLocation (sides_program_, "u_band");
	u_lod_ = glGetUniformLocation (sides_program_, "u_lod");
	u_light_ = glGetUniformLocation (sides_program_, "u_light");
	glUseProgram (0);
}

void
Presentation::end (GLuint target_fb, int tw, int th, const ScreenLayout &l)
{
	if (filtered_)
		filter_->draw (target_fb, l.image, l.frame, filter_frames_++);
	if (bezel_mode_ == BEZEL_BLUR)
		draw_sides (target_fb, tw, th, l);
	else if (bezel_mode_ == BEZEL_IMAGE && bezel_ && l.bezel[2] > 0 && l.bezel[3] > 0)
		draw_bezel (target_fb, l);
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glBindFramebuffer (GL_FRAMEBUFFER, target_fb);
}

/* BEZEL = BLUR: around the picture as it shows (its image cut to its frame),
 * its edges spread over the bars, blurred and dimmed */
void
Presentation::draw_sides (GLuint target_fb, int tw, int th, const ScreenLayout &l)
{
	int x0 = MAX2 (0, MAX2 (l.frame[0], l.image[0]));
	int y0 = MAX2 (0, MAX2 (l.frame[1], l.image[1]));
	int x1 = MIN2 (tw, MIN2 (l.frame[0] + l.frame[2], l.image[0] + l.image[2]));
	int y1 = MIN2 (th, MIN2 (l.frame[1] + l.frame[3], l.image[1] + l.image[3]));
	int w = x1 - x0, h = y1 - y0;
	/* What the sides read: the picture before the preset's tube, or as
	 * it shows */
	GLuint from = filtered_ ? filter_->frame (frame_w_, frame_h_) : target_fb;
	int src[4] = { x0, y0, w, h };
	int near;

	if (w < 2 || h < 2 || (x0 <= 0 && y0 <= 0 && x1 >= tw && y1 >= th))
		return;
	if (filtered_) {
		src[0] = src[1] = 0;
		src[2] = (int) frame_w_;
		src[3] = (int) frame_h_;
	}
	build_programs ();

	/* The picture copied at its size, then its levels down to one texel:
	 * each the mean of the one above */
	if (!sides_texture_ || sides_w_ != src[2] || sides_h_ != src[3]) {
		if (!sides_texture_)
			glGenTextures (1, &sides_texture_);
		if (!sides_fb_)
			glGenFramebuffers (1, &sides_fb_);
		sides_w_ = src[2];
		sides_h_ = src[3];
		sides_levels_ = 1 + (int) floor (log2 ((double) MAX2 (sides_w_, sides_h_)));
		glBindTexture (GL_TEXTURE_2D, sides_texture_);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, sides_w_, sides_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, sides_levels_ - 1);
		glBindFramebuffer (GL_FRAMEBUFFER, sides_fb_);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sides_texture_, 0);
	}
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, from);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, sides_fb_);
	glBlitFramebuffer (src[0], src[1], src[0] + src[2], src[1] + src[3], 0, 0, sides_w_, sides_h_,
	                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, sides_texture_);
	glGenerateMipmap (GL_TEXTURE_2D);

	/* The levels some 40 and 20 texels across, whatever the size */
	near = (int) lround (log2 (MAX2 (1.0, sides_w_ / SIDES_TEXELS)));
	near = MAX2 (0, MIN2 (near, sides_levels_ - 2));

	glBindFramebuffer (GL_FRAMEBUFFER, target_fb);
	glViewport (0, 0, tw, th);
	glUseProgram (sides_program_);
	glUniform4f (u_rect_, (float) x0, (float) y0, (float) w, (float) h);
	glUniform4f (u_band_, (float) x0, (float) y0, (float) (tw - x1), (float) (th - y1));
	glUniform2f (u_lod_, (float) near, (float) MIN2 (near + 1, sides_levels_ - 1));
	glUniform2f (u_light_, SIDES_NEAR, SIDES_FAR);
	glBindVertexArray (vao_);

	/* Only around the picture: left, right, below, above */
	{
		const int sides[4][4] = { { 0, 0, x0, th }, { x1, 0, tw - x1, th },
		                          { x0, 0, w, y0 }, { x0, y1, w, th - y1 } };

		glEnable (GL_SCISSOR_TEST);
		for (const auto &side : sides) {
			if (side[2] <= 0 || side[3] <= 0)
				continue;
			glScissor (side[0], side[1], side[2], side[3]);
			glDrawArrays (GL_TRIANGLES, 0, 3);
		}
		glDisable (GL_SCISSOR_TEST);
	}
	glBindVertexArray (0);
	glBindTexture (GL_TEXTURE_2D, 0);
	glUseProgram (0);
}

/* BEZEL = IMAGE: its picture over all, the game seen through its screen */
void
Presentation::draw_bezel (GLuint target_fb, const ScreenLayout &l)
{
	build_programs ();
	if (bezel_made_ != bezel_) {
		if (!bezel_texture_)
			glGenTextures (1, &bezel_texture_);
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, bezel_texture_);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, bezel_->w, bezel_->h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		              bezel_->rgba.data ());
		/* Mostly shown smaller than it is: its levels, smooth */
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenerateMipmap (GL_TEXTURE_2D);
		bezel_made_ = bezel_;
	}

	glBindFramebuffer (GL_FRAMEBUFFER, target_fb);
	glDisable (GL_SCISSOR_TEST);
	glViewport (l.bezel[0], l.bezel[1], l.bezel[2], l.bezel[3]);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram (image_program_);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, bezel_texture_);
	glBindVertexArray (vao_);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
	glBindTexture (GL_TEXTURE_2D, 0);
	glUseProgram (0);
	glDisable (GL_BLEND);
}

} /* namespace hl */
