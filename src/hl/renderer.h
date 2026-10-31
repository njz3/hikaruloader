/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/renderer.h, Copyright (C) 2011, Stefano Teso.
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

#ifndef __HL_REND_H__
#define __HL_REND_H__

#include "hl/core.h"

#ifdef NDEBUG
#define HL_ASSERT_NO_GL_ERROR()
#else
#define HL_ASSERT_NO_GL_ERROR() \
	do { \
		GLenum error = glGetError (); \
		const char *msg = NULL; \
		switch (error) { \
		case GL_INVALID_ENUM: \
			msg = "invalid enum"; \
			break; \
		case GL_INVALID_VALUE: \
			msg = "invalid value"; \
			break; \
		case GL_INVALID_OPERATION: \
			msg = "invalid operation"; \
			break; \
		case GL_INVALID_FRAMEBUFFER_OPERATION: \
			msg = "invalid fb operation"; \
			break; \
		case GL_OUT_OF_MEMORY: \
			msg = "out of memory"; \
			break; \
		default: \
			break; \
		} \
		if (msg) \
			HL_ERROR ("GL ERROR: %s", msg); \
		HL_ASSERT (error == GL_NO_ERROR); \
	} while (0)
#endif

namespace hl {

/* A machine's renderer: draws the frames into the window (hl::Window).
 * Today's machine renderers use OpenGL; the frame is presented by the
 * window, so that Vulkan or Direct3D 11 renderers can take their place. */
class Renderer {
public:
	/* Opens the window at that size, or joins the one already open */
	Renderer (unsigned width, unsigned height);
	virtual ~Renderer () {}

	Renderer (const Renderer &) = delete;
	Renderer &operator= (const Renderer &) = delete;

	/* Zero-filled, as the C code they come from relied on calloc () */
	static void *operator new (size_t size)
	{
		void *ptr = ::operator new (size);
		memset (ptr, 0, size);
		return ptr;
	}
	static void operator delete (void *ptr) { ::operator delete (ptr); }

	void		begin_frame ();
	/* Draws the frame, then lets the window present it */
	void		end_frame ();
	virtual void	reset () {}

	unsigned width;
	unsigned height;
	/* Debug flags of the frame, shown with the frame rate */
	char message[256];

protected:
	virtual void	on_begin_frame () {}
	virtual void	on_end_frame () {}
};

/* OpenGL helpers of the machine renderers */
namespace gl {
void	clear_errors ();
GLuint	compile_program (const char *vs_src, const char *fs_src);
void	destroy_program (GLuint program);
void	print_uniforms (GLuint program);
} /* namespace gl */

/* Matrices, whatever the graphics API */
void	ortho (mtx4x4f_t proj, float l, float r, float b, float t, float n, float f);
void	frustum (mtx4x4f_t proj, float l, float r, float b, float t, float n, float f);
bool	compute_normal_matrix (mtx3x3f_t dst, mtx4x4f_t src);
void	translate (mtx4x4f_t m, float x, float y, float z);

} /* namespace hl */

#endif /* __HL_REND_H__ */
