/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2011, 2012, Stefano Teso
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

#ifndef __HL_CORE_H__
#define __HL_CORE_H__

/* hl:: and HL_ stand for Hikaruloader; in Valkyrie, its former name, they
 * were vk:: and VK_. */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <type_traits>
#include <stdexcept>
#include <signal.h>

#ifdef _WIN32
#include "hl/compat.h"
#else
#include <endian.h> /* See mesa p_config.h */
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#endif

#include <GL/glew.h>
#include <SDL.h>

#include <jansson.h>

#include "hl/types.h"

/* Determine target endianess */
#if defined (_WIN32)
/* All Windows targets are little endian */
# define HL_LITTLE_ENDIAN
#elif !defined (__BYTE_ORDER)
# error "__BYTE_ORDER not defined"
#elif __BYTE_ORDER == __LITTLE_ENDIAN
# define HL_LITTLE_ENDIAN
#elif __BYTE_ORDER == __BIG_ENDIAN
# define HL_BIG_ENDIAN
#else
# error "unhandled __BYTE_ORDER value"
#endif

/* The version, from git's tags (cmake/version.cmake): HL_VERSION, "0.2.0" for
 * a release, "0.2.1-dev.3+20261101.ab12cd3" between two; its numbers;
 * HL_VERSION_DEV, not a release */
#if __has_include("hl_version.h")
#include "hl_version.h"
#else
#define HL_VERSION		"unknown"
#define HL_VERSION_MAJOR	0
#define HL_VERSION_MINOR	0
#define HL_VERSION_PATCH	0
#define HL_VERSION_BUILD	0
#define HL_VERSION_DEV		1
#endif
/* The full version, as the window's title shows it (formerly the hand-set
 * number, the commit's date and hash, and "-njz3") */
#define HL_VERSION_FULL		HL_VERSION

extern unsigned hl_verbosity;

typedef enum {
	HL_SWAP_NONE	= 0,
	HL_SWAP_BSWAP16	= (1 << 0),
	HL_SWAP_BSWAP32	= (1 << 1),

	HL_NUM_SWAP_TYPES
} hl_swap;

#define ALLOC(type_) \
	((type_ *) calloc (1, sizeof (type_)))

#define NUMELEM(arr_) \
	(sizeof (arr_) / sizeof ((arr_)[0]))

/* Stands in for GCC's case ranges, which MSVC lacks */
#define IN_RANGE(x_,lo_,hi_) \
	((x_) >= (lo_) && (x_) <= (hi_))

#define MIN2(a_,b_) \
	(((a_) < (b_)) ? (a_) : (b_))

#define MAX2(a_,b_) \
	(((a_) > (b_)) ? (a_) : (b_))

#define MIN3(a_,b_,c_) \
	(((a_) < (b_) && (a_) < (c_)) ? (a_) : \
	 ((b_) < (a_) && (b_) < (c_)) ? (b_) : (c_))

#define MAX3(a_,b_) \
	(((a_) > (b_) && (a_) > (c_)) ? (a_) : \
	 ((b_) > (a_) && (b_) > (c_)) ? (b_) : (c_))

#define MIN4(a,b,c,d) \
	(((a_) < (b_) && (a_) < (c_) && (a_) < (d_)) ? (a_) : \
	 ((b_) < (a_) && (b_) < (c_) && (b_) < (d_)) ? (b_) : \
	 ((c_) < (a_) && (c_) < (b_) && (c_) < (d_)) ? (b_) : (d_))

#define MAX4(a,b,c,d) \
	(((a_) > (b_) && (a_) > (c_) && (a_) > (d_)) ? (a_) : \
	 ((b_) > (a_) && (b_) > (c_) && (b_) > (d_)) ? (b_) : \
	 ((c_) > (a_) && (c_) > (b_) && (c_) > (d_)) ? (b_) : (d_))

static inline bool
is_pow2 (unsigned n)
{
	return !(n & (n - 1));
}

/* The following should automatically be detected as byte-swap operations by
 * the compiler and transformed into the corresponding CPU operation. No need
 * for hand-made assembly. */

static inline uint16_t
bswap16 (const uint16_t val)
{
	return ((val << 8) & 0xFF00) |
	       ((val >> 8) & 0x00FF);
}

static inline uint32_t
bswap32 (const uint32_t val)
{
	return ((val << 24) & 0xFF000000) |
	       ((val <<  8) & 0x00FF0000) |
	       ((val >>  8) & 0x0000FF00) |
	       ((val >> 24) & 0x000000FF);
}

static inline uint64_t
bswap64 (const uint64_t val)
{
	return ((val << 56) & 0xFF00000000000000ULL) |
	       ((val << 40) & 0x00FF000000000000ULL) |
	       ((val << 24) & 0x0000FF0000000000ULL) |
	       ((val <<  8) & 0x000000FF00000000ULL) |
	       ((val >>  8) & 0x00000000FF000000ULL) |
	       ((val >> 24) & 0x0000000000FF0000ULL) |
	       ((val >> 40) & 0x000000000000FF00ULL) |
	       ((val >> 56) & 0x00000000000000FFULL);
}

static inline uint16_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_le16 (uint16_t a)
#else
cpu_to_be16 (uint16_t a)
#endif
{
	return a;
}

static inline uint32_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_le32 (uint32_t a)
#else
cpu_to_be32 (uint32_t a)
#endif
{
	return a;
}

static inline uint64_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_le64 (uint64_t a)
#else
cpu_to_be64 (uint64_t a)
#endif
{
	return a;
}

static inline uint16_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_be16 (uint16_t a)
#else
cpu_to_le16 (uint16_t a)
#endif
{
	return bswap16 (a);
}

static inline uint32_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_be32 (uint32_t a)
#else
cpu_to_le32 (uint32_t a)
#endif
{
	return bswap32 (a);
}

static inline uint64_t
#ifdef HL_LITTLE_ENDIAN
cpu_to_be64 (uint64_t a)
#else
cpu_to_le64 (uint64_t a)
#endif
{
	return bswap64 (a);
}

static inline uint64_t
cpu_to_be (unsigned size, uint64_t val) {
	switch (size) {
	case 2:
		val = cpu_to_be16 (val);
		break;
	case 4:
		val = cpu_to_be32 (val);
		break;
	case 8:
		val = cpu_to_be64 (val);
		break;
	}
	return val;
}

static inline uint64_t
cpu_to_le (unsigned size, uint64_t val) {
	switch (size) {
	case 2:
		val = cpu_to_le16 (val);
		break;
	case 4:
		val = cpu_to_le32 (val);
		break;
	case 8:
		val = cpu_to_le64 (val);
		break;
	}
	return val;
}

static inline int32_t
signext_n_32 (const uint32_t in, const unsigned sign_bit)
{
	const uint32_t bit  = 1 << sign_bit;
	const uint32_t mask = bit - 1;
	uint32_t out = in & mask;
	if (in & bit) {
		out |= ~1 & ~mask;
	}
	return (int32_t) out;
}

static inline int64_t
signext_n_64 (const uint64_t in, const unsigned sign_bit)
{
	const uint64_t bit  = 1ull << sign_bit;
	const uint64_t mask = bit - 1;
	uint64_t out = in & mask;
	if (in & bit) {
		out |= ~1 & ~mask;
	}
	return (int64_t) out;
}

/* If cond_ is zero, the array size will be negative and the compilation will
 * fail. Taken from Mesa */
#define HL_STATIC_ASSERT(cond_) \
	do { \
		typedef int static_assertion_failed[(!!(cond_))*2-1]; \
	} while (0);

namespace hl {

/** What HL_ABORT throws instead of aborting while Log::recover_aborts is set
 * (the Lua shell is open): the machine stops where it failed, and the program
 * goes on, for the shell to look at it */
class Abort : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

/** hikaruloader.log: every message, and the crashes */
class Log {
public:
	/** Writes a message to stream and to the log file, when one is open;
	 * messages to stderr are flushed to the log at once, to survive a
	 * crash */
	static void	write (FILE *stream, const char *fmt, ...);
	/** Opens the log file, keeping the previous one as old_path; and logs
	 * crashes (Windows exceptions, abort ()) to it */
	static void	open (const char *path, const char *old_path);
	static void	close ();
	/** HL_ABORT: logs "FATAL: <file>:<line> <function>(): <message>", then
	 * aborts, or throws an Abort while recover_aborts is set */
	[[noreturn]] static void fatal (const char *file, int line, const char *function,
	                                const char *fmt, ...);
	static bool	recover_aborts;
	/** The calling thread's number, as a crash in the log names it */
	static unsigned long thread_id ();
	/** The agent's protocol has stdout: the messages go to stderr */
	static bool	stdout_reserved;
};

} /* namespace hl */

/** Prints a formatted message to stdout */
#define HL_PRINT(fmt_, ...) \
	do { \
		hl::Log::write (stdout, fmt_ "\n", ##__VA_ARGS__); \
	} while (0)

/** Prints a formatted message to stdout in DEBUG builds, and only if verbosity
 * is non-zero */
#ifdef NDEBUG
#define HL_LOG(fmt_, ...)
#else
#define HL_LOG(fmt_, ...) \
	do { \
		if (hl_verbosity > 0) \
			hl::Log::write (stdout, fmt_ "\n", ##__VA_ARGS__); \
	} while (0)
#endif

/** Prints a formatted message, prefixed by 'ERROR', to stderr */
#define HL_ERROR(fmt_, ...) \
	do { \
		hl::Log::write (stderr, "ERROR: " fmt_ "\n", ##__VA_ARGS__); \
	} while (0)

/** Prints a formatted message, prefixed by 'FATAL', to stderr, and aborts; see
 * hl::Log::fatal */
#define HL_ABORT(fmt_, ...) \
	hl::Log::fatal (__FILE__, __LINE__, __FUNCTION__, fmt_, ##__VA_ARGS__)

/** If the condition fails, prints a formatted message, prefixed by 'FATAL',
 * to stderr, and exits */
#ifdef NDEBUG
#define HL_ASSERT(_cond)
#else
#define HL_ASSERT(_cond) \
	do { \
		if (!(_cond)) { \
			HL_ABORT ("assertion failed, aborting"); \
		} \
	} while (0)
#endif

bool	hl_util_get_bool_option (const char *name, bool fallback);
int	hl_util_get_int_option (const char *name, int fallback);

#endif /* __HL_CORE_H__ */

