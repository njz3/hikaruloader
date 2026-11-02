/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/host/video/picture_filter_gl.cpp,
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

#include <algorithm>
#include <cstring>

#include "hl/crt_filter.h"

namespace hl {

static GLuint
build_shader (GLenum type, const std::string &text, const std::string &name)
{
	GLuint shader = glCreateShader (type);
	const char *source = text.c_str ();
	GLint status;

	glShaderSource (shader, 1, &source, NULL);
	glCompileShader (shader);
	glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1024];

		glGetShaderInfoLog (shader, sizeof (log), NULL, log);
		HL_ERROR ("CRT_PRESET: %s (%s) does not build: %s", name.c_str (),
		          type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		glDeleteShader (shader);
		return 0;
	}
	return shader;
}

/* The program of a pass; 0, logged, when it does not build */
static GLuint
build_program (const std::string &source, const std::string &name)
{
	GLuint vertex = build_shader (GL_VERTEX_SHADER, crt_preset::part (source, "VERTEX"), name);
	GLuint fragment = vertex ? build_shader (GL_FRAGMENT_SHADER, crt_preset::part (source, "FRAGMENT"), name) : 0;
	GLuint program = 0;
	GLint status = GL_FALSE;

	if (vertex && fragment) {
		program = glCreateProgram ();
		glAttachShader (program, vertex);
		glAttachShader (program, fragment);
		glLinkProgram (program);
		glGetProgramiv (program, GL_LINK_STATUS, &status);
	}
	if (program && !status) {
		char log[1024];

		glGetProgramInfoLog (program, sizeof (log), NULL, log);
		HL_ERROR ("CRT_PRESET: %s does not link: %s", name.c_str (), log);
		glDeleteProgram (program);
		program = 0;
	}
	if (vertex)
		glDeleteShader (vertex);
	if (fragment)
		glDeleteShader (fragment);
	return program;
}

/* Black, its alpha at 1 */
static void
clear_texture (GLuint fb)
{
	glBindFramebuffer (GL_FRAMEBUFFER, fb);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

/* Each parameter a constant of the source: its value does not change while
 * the filter is used, and the driver then leaves out the work it turns off
 * (Lola Retro's crt pass on an HD 520, as Odin measured it: 115 ms a picture
 * with uniforms, 6 ms so). One declared otherwise stays a uniform. */
static void
with_constants (std::string &source, const std::vector<std::pair<std::string, float>> &parameters)
{
	for (const auto &[name, value] : parameters) {
		std::string uniform = "uniform float " + name + ";";
		char literal[32];

		snprintf (literal, sizeof (literal), "%.9g", (double) value);

		std::string constant = "const float " + name + " = " + literal +
		                       (strpbrk (literal, ".e") ? "" : ".0") + ";";

		for (size_t at = source.find (uniform); at != std::string::npos; at = source.find (uniform, at))
			source.replace (at, uniform.size (), constant);
	}
}

/* An 86Box texture is RGB: here RGBA, which every driver draws into, its
 * alpha kept at 1 (cleared so, never written: draw ()) */
static void
make_texture (GLuint &texture, GLuint &fb, unsigned w, unsigned h, bool is_float)
{
	if (!texture)
		glGenTextures (1, &texture);
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexImage2D (GL_TEXTURE_2D, 0, is_float ? GL_RGBA32F : GL_RGBA8, (GLsizei) w, (GLsizei) h, 0,
	              GL_RGBA, is_float ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
	/* Sampled by the sampler objects; one level, complete without them */
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glBindTexture (GL_TEXTURE_2D, 0);
	if (!fb)
		glGenFramebuffers (1, &fb);
	glBindFramebuffer (GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	clear_texture (fb);
}

std::unique_ptr<CrtFilter>
CrtFilter::load (const std::string &preset)
{
	crt_preset::Plan plan;
	std::string error;

	/* The samplers, GL 3.3's, an extension of the window's 3.1 context.
	 * The passes' GLSL version (Lola Retro's 1.50), the driver's to take */
	if (!GLEW_VERSION_3_3 && !GLEW_ARB_sampler_objects) {
		HL_ERROR ("CRT_PRESET: '%s' needs sampler objects (OpenGL 3.3): not used", preset.c_str ());
		return NULL;
	}
	if (!crt_preset::read_plan (preset, &plan, &error)) {
		HL_ERROR ("CRT_PRESET: '%s': %s: not used", preset.c_str (), error.c_str ());
		return NULL;
	}

	std::unique_ptr<CrtFilter> filter (new CrtFilter ());
	if (!filter->build (plan))
		return NULL;
	HL_PRINT ("CRT_PRESET: '%s', %u passes", preset.c_str (), (unsigned) filter->passes_.size ());
	return filter;
}

bool
CrtFilter::build (const crt_preset::Plan &plan)
{
	passes_.resize (plan.passes.size ());
	for (size_t i = 0; i < plan.passes.size (); i++) {
		const crt_preset::Plan::Pass &from = plan.passes[i];
		Pass &pass = passes_[i];

		pass.out.linear = from.linear;
		pass.out.is_float = from.is_float;
		pass.out.wrap = from.wrap == crt_preset::Plan::Wrap::REPEAT ? GL_REPEAT :
		                from.wrap == crt_preset::Plan::Wrap::MIRRORED ? GL_MIRRORED_REPEAT :
		                from.wrap == crt_preset::Plan::Wrap::EDGE ? GL_CLAMP_TO_EDGE : GL_CLAMP_TO_BORDER;
		pass.scale_type = from.scale_type;
		pass.scale = from.scale;
	}
	passes_.back ().last = true;

	for (size_t i = 0; i < passes_.size (); i++) {
		Pass &pass = passes_[i];
		std::string source = plan.passes[i].source;

		with_constants (source, plan.parameters);
		pass.program = build_program (source, plan.passes[i].file);
		if (!pass.program)
			return false;
		find_uniforms (pass, i, plan.parameters);
		make_corners (pass);
	}
	return true;
}

/* The uniforms of a pass the frames set: its texture and sizes, those of the
 * source and of each pass before it, the preset's parameters */
void
CrtFilter::find_uniforms (Pass &pass, size_t i, const std::vector<std::pair<std::string, float>> &parameters)
{
	auto uniform = [&pass] (const std::string &uniform_name) {
		return glGetUniformLocation (pass.program, uniform_name.c_str ());
	};

	pass.texture = uniform ("Texture");
	pass.input_size = uniform ("InputSize");
	pass.texture_size = uniform ("TextureSize");
	pass.output_size = uniform ("OutputSize");
	pass.frame_count = uniform ("FrameCount");
	pass.frame_direction = uniform ("FrameDirection");
	pass.mvp = uniform ("MVPMatrix");
	pass.orig_texture = uniform ("OrigTexture");
	pass.orig_input_size = uniform ("OrigInputSize");
	pass.orig_texture_size = uniform ("OrigTextureSize");
	for (size_t k = 1; k <= i; k++) {
		std::string before = "Pass" + std::to_string (k);

		pass.passes.push_back ({ uniform (before + "Texture"), uniform (before + "TextureSize"),
		                         uniform (before + "InputSize") });
	}
	for (const auto &[parameter, value] : parameters)
		if (GLint at = uniform (parameter); at >= 0)
			pass.parameters.emplace_back (at, value);
}

/* Four corners: VertexCoord, then TexCoord, set by size () */
void
CrtFilter::make_corners (Pass &pass)
{
	static const GLfloat QUAD[16] = {};
	GLint vertex = glGetAttribLocation (pass.program, "VertexCoord");
	GLint tex = glGetAttribLocation (pass.program, "TexCoord");

	glGenVertexArrays (1, &pass.vao);
	glGenBuffers (1, &pass.vbo);
	glBindVertexArray (pass.vao);
	glBindBuffer (GL_ARRAY_BUFFER, pass.vbo);
	glBufferData (GL_ARRAY_BUFFER, sizeof (QUAD), QUAD, GL_DYNAMIC_DRAW);
	if (vertex >= 0) {
		glEnableVertexAttribArray ((GLuint) vertex);
		glVertexAttribPointer ((GLuint) vertex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat), NULL);
	}
	if (tex >= 0) {
		glEnableVertexAttribArray ((GLuint) tex);
		glVertexAttribPointer ((GLuint) tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat),
		                       (const void *) (2 * sizeof (GLfloat)));
	}
	glBindVertexArray (0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
}

CrtFilter::~CrtFilter ()
{
	for (Pass &pass : passes_) {
		if (pass.program)
			glDeleteProgram (pass.program);
		glDeleteVertexArrays (1, &pass.vao);
		glDeleteBuffers (1, &pass.vbo);
		glDeleteFramebuffers (1, &pass.out.fb);
		glDeleteTextures (1, &pass.out.texture);
	}
	glDeleteFramebuffers (1, &frame_.fb);
	glDeleteTextures (1, &frame_.texture);
	for (const auto &[kind, sampler] : samplers_)
		glDeleteSamplers (1, &sampler);
}

GLuint
CrtFilter::frame (unsigned w, unsigned h)
{
	if (frame_size_[0] != w || frame_size_[1] != h) {
		frame_size_ = { w, h };
		frame_.size = { (float) w, (float) h };
		frame_.texture_size = crt_preset::texture_of (frame_.size);
		make_texture (frame_.texture, frame_.fb, (unsigned) frame_.texture_size[0],
		              (unsigned) frame_.texture_size[1], false);
		viewport_ = {};
	}
	return frame_.fb;
}

/* The size of each pass for a viewport of w x h, its texture and its
 * corners; the textures black again (a smaller picture leaves nothing of
 * the larger around it) */
void
CrtFilter::size (unsigned w, unsigned h)
{
	std::array<float, 2> viewport = { (float) w, (float) h };
	const Picture *in = &frame_;

	viewport_ = { w, h };
	for (Pass &pass : passes_) {
		pass.out.size = crt_preset::pass_size (pass.scale_type, pass.scale, viewport, in->size);
		if (!pass.last) {
			std::array<float, 2> texture_size = crt_preset::texture_of (pass.out.size);

			if (texture_size != pass.out.texture_size || !pass.out.texture) {
				pass.out.texture_size = texture_size;
				make_texture (pass.out.texture, pass.out.fb, (unsigned) texture_size[0],
				              (unsigned) texture_size[1], pass.out.is_float);
			} else
				clear_texture (pass.out.fb);
		}

		std::array<GLfloat, 16> quad = crt_preset::corners (in->size, in->texture_size);

		glBindBuffer (GL_ARRAY_BUFFER, pass.vbo);
		glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (quad), quad.data ());
		in = &pass.out;
	}
	glBindBuffer (GL_ARRAY_BUFFER, 0);
}

GLuint
CrtFilter::sampler (bool linear, GLint wrap)
{
	GLuint &held = samplers_[{ linear, wrap }];

	if (!held) {
		static const GLfloat BLACK[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		glGenSamplers (1, &held);
		glSamplerParameteri (held, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri (held, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri (held, GL_TEXTURE_WRAP_S, wrap);
		glSamplerParameteri (held, GL_TEXTURE_WRAP_T, wrap);
		glSamplerParameterfv (held, GL_TEXTURE_BORDER_COLOR, BLACK);
	}
	return held;
}

void
CrtFilter::draw (GLuint target_fb, const int viewport[4], const int *clip, unsigned count)
{
	static const GLfloat IDENTITY[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	unsigned w = (unsigned) MAX2 (1, viewport[2]), h = (unsigned) MAX2 (1, viewport[3]);
	const Picture *in = &frame_;
	GLuint units = 0;
	/* What the drawing of the 3D left on: back faces culled */
	GLboolean culling = glIsEnabled (GL_CULL_FACE);

	/* The passes draw whole textures; the last one alone is cut */
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_CULL_FACE);
	glDisable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	if (viewport_[0] != w || viewport_[1] != h)
		size (w, h);
	for (size_t i = 0; i < passes_.size (); i++) {
		const Pass &pass = passes_[i];
		/* The last reads its input linear, as 86Box does with its own
		 * filter setting */
		bool linear = pass.last || in->linear;
		GLuint unit = 0;
		auto bind = [&] (GLint uniform, const Picture &picture, bool picture_linear) {
			glActiveTexture (GL_TEXTURE0 + unit);
			glBindTexture (GL_TEXTURE_2D, picture.texture);
			glBindSampler (unit, sampler (picture_linear, picture.wrap));
			if (uniform >= 0)
				glUniform1i (uniform, (GLint) unit);
			unit++;
		};

		glUseProgram (pass.program);
		bind (pass.texture, *in, linear);
		glUniform2f (pass.input_size, in->size[0], in->size[1]);
		glUniform2f (pass.texture_size, in->texture_size[0], in->texture_size[1]);
		glUniform2f (pass.output_size, (float) w, (float) h);
		glUniform1i (pass.frame_count, (GLint) count);
		glUniform1i (pass.frame_direction, 1);
		glUniformMatrix4fv (pass.mvp, 1, GL_FALSE, IDENTITY);
		for (const auto &[at, value] : pass.parameters)
			glUniform1f (at, value);
		if (i > 0) {
			bind (pass.orig_texture, frame_, frame_.linear);
			glUniform2f (pass.orig_input_size, frame_.size[0], frame_.size[1]);
			glUniform2f (pass.orig_texture_size, frame_.texture_size[0], frame_.texture_size[1]);
			for (size_t k = 0; k < i; k++) {
				const Picture &drawn = passes_[k].out, &read = k ? passes_[k - 1].out : frame_;

				bind (pass.passes[k][0], drawn, &drawn == in ? linear : drawn.linear);
				glUniform2f (pass.passes[k][1], read.texture_size[0], read.texture_size[1]);
				glUniform2f (pass.passes[k][2], read.size[0], read.size[1]);
			}
		}

		if (pass.last) {
			glBindFramebuffer (GL_FRAMEBUFFER, target_fb);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glViewport (viewport[0], viewport[1], (GLsizei) w, (GLsizei) h);
			if (clip) {
				glScissor (clip[0], clip[1], clip[2], clip[3]);
				glEnable (GL_SCISSOR_TEST);
			}
		} else {
			/* No pass leaves a pixel of its picture undrawn: no clear
			 * (8192 x 1024 floats for Lola Retro's beam) */
			glBindFramebuffer (GL_FRAMEBUFFER, pass.out.fb);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			glViewport (0, 0, (GLsizei) pass.out.size[0], (GLsizei) pass.out.size[1]);
		}
		glBindVertexArray (pass.vao);
		glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
		units = MAX2 (units, unit);
		in = &pass.out;
	}
	glDisable (GL_SCISSOR_TEST);
	for (GLuint unit = 0; unit < units; unit++) {
		glActiveTexture (GL_TEXTURE0 + unit);
		glBindTexture (GL_TEXTURE_2D, 0);
		glBindSampler (unit, 0);
	}
	glActiveTexture (GL_TEXTURE0);
	glBindVertexArray (0);
	glUseProgram (0);
	if (culling)
		glEnable (GL_CULL_FACE);
}

} /* namespace hl */
