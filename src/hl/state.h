/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/state.h, Copyright (C) 2011-2013, Stefano Teso.
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

#ifndef __HL_STATE_H__
#define __HL_STATE_H__

#include <string>
#include <vector>

#include "hl/core.h"

namespace hl {

/* A save state file: a header, the size of the contents, then the contents,
 * compressed (zlib, with miniz): what the machine, its buffers and its
 * devices put, in that order (see Machine::load_save_state). The contents
 * stay in memory until finish (). */
class State {
public:
	enum Mode {
		SAVE,
		LOAD
	};

	/* SAVE: an empty state, for path; LOAD: reads path, checks its header
	 * and uncompresses it. ok () tells whether that worked. */
	State (const char *path, Mode mode);
	~State () {}

	State (const State &) = delete;
	State &operator= (const State &) = delete;

	bool	ok () const { return valid; }
	Mode	mode () const { return state_mode; }

	/* 0 on success, -1 on error or in the wrong mode */
	int	put (const void *src, uint32_t size);
	int	get (void *dst, uint32_t size);

	/* SAVE: compresses the contents and writes the file; LOAD: checks that
	 * they were all read, as a state of another layout would not be. 0 on
	 * success */
	int	finish ();

private:
	std::string		 path;
	Mode			 state_mode;
	bool			 valid;
	std::vector<uint8_t>	 data;
	size_t			 offset;
};

} /* namespace hl */

#endif /* __HL_STATE_H__ */
