/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/buffer.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_BUFFER_H__
#define __HL_BUFFER_H__

#include "hl/core.h"
#include "hl/state.h"

static inline bool
is_size_valid (unsigned size)
{
	return size == 1 || size == 2 || size == 4 || size == 8;
}

static inline int
set_ptr (void *ptr, unsigned size, uint64_t val)
{
	if (!is_size_valid (size))
		return -1;
	switch (size) {
	case 1:
		*(uint8_t *) ptr = (uint8_t) val;
		break;
	case 2:
		*(uint16_t *) ptr = (uint16_t) val;
		break;
	case 4:
		*(uint32_t *) ptr = (uint32_t) val;
		break;
	default:
		*(uint64_t *) ptr = (uint64_t) val;
		break;
	}
	return 0;
}

namespace hl {

/* Deletes an object and clears the pointer to it */
template <typename T>
static inline void
destroy (T *&ptr)
{
	delete ptr;
	ptr = NULL;
}

/* A block of emulated memory (RAM, ROM, registers): byte-addressed, read and
 * written 1, 2, 4 or 8 bytes at a time in its byte order. */
class Buffer {
public:
	enum Order {
		NATIVE,
		LE32,
		BE32
	};

	/* NULL when the memory cannot be allocated */
	static Buffer	*create (unsigned size, unsigned alignment = 0, Order order = NATIVE);
	/* The contents of a file; NULL when it is missing, or not reqsize
	 * bytes long (unless reqsize is ~0) */
	static Buffer	*from_file (const char *path, unsigned reqsize);

	~Buffer ();

	Buffer (const Buffer &) = delete;
	Buffer &operator= (const Buffer &) = delete;

	unsigned	 size () const { return buf_size; }
	Order		 byte_order () const { return order; }
	/* The paging file's section that holds it (Windows' HANDLE), for a
	 * second view; NULL for a buffer of the heap */
	void		*shared_section () const { return section; }
	static const unsigned SECTION_MIN = 64 * 1024;
	/* NULL past the end */
	void		*ptr (unsigned offs);

	uint64_t	 get (unsigned size, uint32_t offs) const;
	void		 put (unsigned size, uint32_t offs, uint64_t val);
	void		 clear ();

	void		 print () const;
	void		 print_some (unsigned lo, unsigned hi) const;
	void		 dump (const char *path) const;
	void		 dumpf (const char *fmt, ...) const;

	int		 load_state (State *state);
	int		 save_state (State *state);

private:
	Buffer (uint8_t *ptr, unsigned size, Order order) :
		buf_ptr (ptr), buf_size (size), order (order) {}

	uint8_t *buf_ptr;
	unsigned buf_size;
	Order order;
	void *section = NULL;
};

/* NATIVE and LE32 are the same on the little endian hosts Hikaruloader runs on */
inline uint64_t
Buffer::get (unsigned size, uint32_t offs) const
{
	const uint8_t *p = &buf_ptr[offs];

	HL_ASSERT (is_size_valid (size));
	HL_ASSERT ((offs + size - 1) < buf_size);

	if (order == BE32) {
		switch (size) {
		case 1:
			return *p;
		case 2:
			return cpu_to_be16 (*(uint16_t *) p);
		case 4:
			return cpu_to_be32 (*(uint32_t *) p);
		default:
			return cpu_to_be64 (*(uint64_t *) p);
		}
	}
	switch (size) {
	case 1:
		return *p;
	case 2:
		return cpu_to_le16 (*(uint16_t *) p);
	case 4:
		return cpu_to_le32 (*(uint32_t *) p);
	default:
		return cpu_to_le64 (*(uint64_t *) p);
	}
}

inline void
Buffer::put (unsigned size, uint32_t offs, uint64_t val)
{
	uint8_t *p = &buf_ptr[offs];

	HL_ASSERT (is_size_valid (size));
	HL_ASSERT ((offs + size - 1) < buf_size);

	if (order == BE32) {
		switch (size) {
		case 1:
			*p = (uint8_t) val;
			break;
		case 2:
			*(uint16_t *) p = cpu_to_be16 ((uint16_t) val);
			break;
		case 4:
			*(uint32_t *) p = cpu_to_be32 ((uint32_t) val);
			break;
		default:
			/* XXX as in the original: the upper half is lost */
			*(uint64_t *) p = cpu_to_be64 ((uint32_t) val);
			break;
		}
		return;
	}
	switch (size) {
	case 1:
		*p = (uint8_t) val;
		break;
	case 2:
		*(uint16_t *) p = cpu_to_le16 ((uint16_t) val);
		break;
	case 4:
		*(uint32_t *) p = cpu_to_le32 ((uint32_t) val);
		break;
	default:
		*(uint64_t *) p = cpu_to_le64 ((uint64_t) val);
		break;
	}
}

} /* namespace hl */

#endif /* __HL_BUFFER_H__ */
