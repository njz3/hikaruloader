/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/renderer.c, Copyright (C) 2011, Stefano Teso.
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

#include <atomic>

#include "hl/profile.h"
#include "hl/renderer.h"
#include "hl/window.h"

namespace hl {

Renderer::Renderer (unsigned width, unsigned height) :
	width (width),
	height (height)
{
	HL_ASSERT (width);
	HL_ASSERT (height);

	message[0] = '\0';
	Window::open (width, height);
	gl::clear_errors ();
}

void
Renderer::begin_frame ()
{
	message[0] = '\0';
	on_begin_frame ();
}

void
Renderer::end_frame ()
{
	on_end_frame ();
	HL_PROFILE (PRESENT);
	Window::frame_done (message);
}

void
gl::clear_errors ()
{
	while (glGetError () != GL_NO_ERROR);
}

GLenum
gl::check_error (const char *file, int line)
{
	/* Both drawing contexts' */
	static std::atomic<unsigned> out_of_memory;
	GLenum error = glGetError ();
	const char *msg = NULL;
	unsigned n;

	switch (error) {
	case GL_NO_ERROR:
		return error;
	case GL_INVALID_ENUM:
		msg = "invalid enum";
		break;
	case GL_INVALID_VALUE:
		msg = "invalid value";
		break;
	case GL_INVALID_OPERATION:
		msg = "invalid operation";
		break;
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		msg = "invalid fb operation";
		break;
	case GL_OUT_OF_MEMORY:
		n = ++out_of_memory;
		if (n <= 10 || !(n % 1000))
			HL_ERROR ("GL ERROR: out of memory (%s:%d; %u times): the frame goes on without",
			          file, line, n);
		return error;
	default:
		msg = "unknown";
		break;
	}
	HL_ERROR ("GL ERROR: %s (%s:%d)", msg, file, line);
	return error;
}

static GLuint
compile_shader (GLenum type, const char *src)
{
	char info[256];
	GLuint id;
	GLint status;

	id = glCreateShader (type);
	glShaderSource (id, 1, (const GLchar **) &src, NULL);
	glCompileShader (id);
	glGetShaderiv (id, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog (id, sizeof (info), NULL, info);
		HL_ERROR ("could not compile GLSL shader: '%s'\n", info);
		HL_ERROR ("source:\n%s\n", src);
		glDeleteShader (id);
		HL_ASSERT (0);
	}
	return id;
}

GLuint
gl::compile_program (const char *vs_src, const char *fs_src)
{
	char info[256];
	GLuint id, vs, fs;
	GLint status;

	vs = compile_shader (GL_VERTEX_SHADER, vs_src);
	HL_ASSERT_NO_GL_ERROR ();

	fs = compile_shader (GL_FRAGMENT_SHADER, fs_src);
	HL_ASSERT_NO_GL_ERROR ();

	id = glCreateProgram ();
	glAttachShader (id, vs);
	glAttachShader (id, fs);
	/* gl_FragColor is unavailable in forward-compatible contexts */
	glBindFragDataLocation (id, 0, "frag_color");
	glLinkProgram (id);
	glGetProgramiv (id, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		glGetProgramInfoLog (id, sizeof (info), NULL, info);
		HL_ERROR ("could not link GLSL program: '%s'\n", info);
		HL_ERROR ("vs source:\n%s\n", vs_src);
		HL_ERROR ("fs source:\n%s\n", fs_src);
		glDeleteProgram (id);
		HL_ASSERT (0);
	}

	/* "If a shader object to be deleted is attached to a program object,
	 * it will be flagged for deletion, but it will not be deleted until it
	 * is no longer attached to any program object, for any rendering
	 * context." */
	glDeleteShader (vs);
	glDeleteShader (fs);
	HL_ASSERT_NO_GL_ERROR ();

	return id;
}

void
gl::destroy_program (GLuint program)
{
	if (program) {
		glUseProgram (0);
		glDeleteProgram (program);
	}
}

void
gl::print_uniforms (GLuint program)
{
	GLchar id[256];
	GLint count, loc, size, i;
	GLenum type;

	glGetProgramiv (program, GL_ACTIVE_UNIFORMS, &count);
	for (i = 0; i < count; i++) {
		glGetActiveUniform (program, i, sizeof (id), NULL, &size, &type, id);
		loc = glGetUniformLocation (program, id);
		HL_LOG ("uniform %u : %s <size %d>", loc, id, size);
	}
}

/* Taken from Mesa */
void
ortho (mtx4x4f_t proj, float l, float r, float b, float t, float n, float f)
{
	proj[0][0] = 2.0f / (r - l);
	proj[1][0] = 0.0f;
	proj[2][0] = 0.0f;
	proj[3][0] = - (r + l) / (r - l);

	proj[0][1] = 0.0f;
	proj[1][1] = 2.0f / (t - b);
	proj[2][1] = 0.0f;
	proj[3][1] = - (t + b) / (t - b);

	proj[0][2] = 0.0f;
	proj[1][2] = 0.0f;
	proj[2][2] = -2.0f / (f - n);
	proj[3][2] = - (f + n) / (f - n);

	proj[0][3] = 0.0f;
	proj[1][3] = 0.0f;
	proj[2][3] = 0.0f;
	proj[3][3] = 1.0f;
}

/* Taken from Mesa */
void
frustum (mtx4x4f_t proj, float l, float r, float b, float t, float n, float f)
{
	proj[0][0] = (2.0f * n) / (r - l);
	proj[1][0] = 0.0f;
	proj[2][0] = (r + l) / (r - l);
	proj[3][0] = 0.0f;

	proj[0][1] = 0.0f;
	proj[1][1] = (2.0f * n) / (t - b);
	proj[2][1] = (t + b) / (t - b);
	proj[3][1] = 0.0f;

	proj[0][2] = 0.0f;
	proj[1][2] = 0.0f;
	proj[2][2] = - (f + n) / (f - n);
	proj[3][2] = - (2.0f * f * n) / (f - n);

	proj[0][3] = 0.0f;
	proj[1][3] = 0.0f;
	proj[2][3] = -1.0f;
	proj[3][3] = 0.0f;
}

/* Taken from Mesa */
bool
compute_normal_matrix (mtx3x3f_t dst, mtx4x4f_t src)
{
	union {
		mtx4x4f_t m;
		float f[16];
	} inv;
	float det, *s = (float *) src;
	int i, j;

	/* Compute the inverse of the source (projection) matrix. */

	inv.f[0] = s[5]  * s[10] * s[15] - 
	           s[5]  * s[11] * s[14] - 
	           s[9]  * s[6]  * s[15] + 
	           s[9]  * s[7]  * s[14] +
	           s[13] * s[6]  * s[11] - 
	           s[13] * s[7]  * s[10];

	inv.f[4] = -s[4]  * s[10] * s[15] + 
	            s[4]  * s[11] * s[14] + 
	            s[8]  * s[6]  * s[15] - 
	            s[8]  * s[7]  * s[14] - 
	            s[12] * s[6]  * s[11] + 
	            s[12] * s[7]  * s[10];

	inv.f[8] = s[4]  * s[9] * s[15] - 
	           s[4]  * s[11] * s[13] - 
	           s[8]  * s[5] * s[15] + 
	           s[8]  * s[7] * s[13] + 
	           s[12] * s[5] * s[11] - 
	           s[12] * s[7] * s[9];

	inv.f[12] = -s[4]  * s[9] * s[14] + 
	             s[4]  * s[10] * s[13] +
	             s[8]  * s[5] * s[14] - 
	             s[8]  * s[6] * s[13] - 
	             s[12] * s[5] * s[10] + 
	             s[12] * s[6] * s[9];

	inv.f[1] = -s[1]  * s[10] * s[15] + 
	            s[1]  * s[11] * s[14] + 
	            s[9]  * s[2] * s[15] - 
	            s[9]  * s[3] * s[14] - 
	            s[13] * s[2] * s[11] + 
	            s[13] * s[3] * s[10];

	inv.f[5] = s[0]  * s[10] * s[15] - 
	           s[0]  * s[11] * s[14] - 
	           s[8]  * s[2] * s[15] + 
	           s[8]  * s[3] * s[14] + 
	           s[12] * s[2] * s[11] - 
	           s[12] * s[3] * s[10];

	inv.f[9] = -s[0]  * s[9] * s[15] + 
	            s[0]  * s[11] * s[13] + 
	            s[8]  * s[1] * s[15] - 
	            s[8]  * s[3] * s[13] - 
	            s[12] * s[1] * s[11] + 
	            s[12] * s[3] * s[9];

	inv.f[13] = s[0]  * s[9] * s[14] - 
	            s[0]  * s[10] * s[13] - 
	            s[8]  * s[1] * s[14] + 
	            s[8]  * s[2] * s[13] + 
	            s[12] * s[1] * s[10] - 
	            s[12] * s[2] * s[9];

	inv.f[2] = s[1]  * s[6] * s[15] - 
	           s[1]  * s[7] * s[14] - 
	           s[5]  * s[2] * s[15] + 
	           s[5]  * s[3] * s[14] + 
	           s[13] * s[2] * s[7] - 
	           s[13] * s[3] * s[6];

	inv.f[6] = -s[0]  * s[6] * s[15] + 
	            s[0]  * s[7] * s[14] + 
	            s[4]  * s[2] * s[15] - 
	            s[4]  * s[3] * s[14] - 
	            s[12] * s[2] * s[7] + 
	            s[12] * s[3] * s[6];

	inv.f[10] = s[0]  * s[5] * s[15] - 
	            s[0]  * s[7] * s[13] - 
	            s[4]  * s[1] * s[15] + 
	            s[4]  * s[3] * s[13] + 
	            s[12] * s[1] * s[7] - 
	            s[12] * s[3] * s[5];

	inv.f[14] = -s[0]  * s[5] * s[14] + 
	             s[0]  * s[6] * s[13] + 
	             s[4]  * s[1] * s[14] - 
	             s[4]  * s[2] * s[13] - 
	             s[12] * s[1] * s[6] + 
	             s[12] * s[2] * s[5];

	inv.f[3] = -s[1] * s[6] * s[11] + 
	            s[1] * s[7] * s[10] + 
	            s[5] * s[2] * s[11] - 
	            s[5] * s[3] * s[10] - 
	            s[9] * s[2] * s[7] + 
	            s[9] * s[3] * s[6];

	inv.f[7] = s[0] * s[6] * s[11] - 
	           s[0] * s[7] * s[10] - 
	           s[4] * s[2] * s[11] + 
	           s[4] * s[3] * s[10] + 
	           s[8] * s[2] * s[7] - 
	           s[8] * s[3] * s[6];

	inv.f[11] = -s[0] * s[5] * s[11] + 
	             s[0] * s[7] * s[9] + 
	             s[4] * s[1] * s[11] - 
	             s[4] * s[3] * s[9] - 
	             s[8] * s[1] * s[7] + 
	             s[8] * s[3] * s[5];

	inv.f[15] = s[0] * s[5] * s[10] - 
	            s[0] * s[6] * s[9] - 
	            s[4] * s[1] * s[10] + 
	            s[4] * s[2] * s[9] + 
	            s[8] * s[1] * s[6] - 
	            s[8] * s[2] * s[5];

	det = s[0] * inv.f[0] +
	      s[1] * inv.f[4] +
	      s[2] * inv.f[8] +
	      s[3] * inv.f[12];
	if (det == 0.0)
		return false;

	/* Extract the top-left 3x3 matrix from the inverse. */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			dst[i][j] = inv.m[j][i] * (1.0f / det);

	return true;
}

void
translate (mtx4x4f_t m, float x, float y, float z)
{
	m[3][0] += m[0][0] * x + m[1][0] * y + m[2][0] * z;
	m[3][1] += m[0][1] * x + m[1][1] * y + m[2][1] * z;
	m[3][2] += m[0][2] * x + m[1][2] * y + m[2][2] * z;
	m[3][3] += m[0][3] * x + m[1][3] * y + m[2][3] * z;
}

} /* namespace hl */
