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

#include "hl/types.h"
#include "hl/core.h"

#include <atomic>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

unsigned hl_verbosity = 0;

static FILE *log_file = NULL;

void
hl::Log::write (FILE *stream, const char *fmt, ...)
{
	va_list va;

	if (stream == stdout && stdout_reserved)
		stream = stderr;

	va_start (va, fmt);
	vfprintf (stream, fmt, va);
	va_end (va);

	if (log_file) {
		va_start (va, fmt);
		vfprintf (log_file, fmt, va);
		va_end (va);
		/* The errors on the disk for a crash's log, but at most once a
		 * second: a game writing errors at each frame, the log on a
		 * network drive, went down to 10 frames a second. A crash's
		 * handlers (below) and the end write the rest */
		if (stream == stderr) {
			static std::atomic<int64_t> last_flush (0);
			int64_t now = std::chrono::duration_cast<std::chrono::milliseconds> (
				std::chrono::steady_clock::now ().time_since_epoch ()).count ();
			if (now - last_flush.load () >= 1000) {
				last_flush.store (now);
				fflush (log_file);
			}
		}
	}
}

static void
log_abort (int sig)
{
	(void) sig;
	if (log_file) {
		fprintf (log_file, "FATAL: abort () called\n");
		fflush (log_file);
	}
}

#ifdef _WIN32
/* A C++ exception's type, from its throw information (MSVC's, x64: the
 * offsets from the image's base): its decorated name, ".?AVbad_alloc@std@@"
 * for std::bad_alloc; NULL when it cannot be read */
static const char *
cpp_exception_type (const EXCEPTION_RECORD *record)
{
#if defined(_MSC_VER) && defined(_M_X64)
	if (record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4)
		return NULL;
	const char *name = NULL;
	__try {
		uintptr_t base = (uintptr_t) record->ExceptionInformation[3];
		const uint32_t *throw_info = (const uint32_t *) record->ExceptionInformation[2];
		/* ThrowInfo: attributes, unwind, forward compat, the catchable
		 * types' array (a count, then each one's offset); a catchable
		 * type: properties, its type descriptor (a vtable, a spare, the
		 * name) */
		const int32_t *types = (const int32_t *) (base + throw_info[3]);
		const uint32_t *type = (const uint32_t *) (base + (uint32_t) types[1]);
		name = (const char *) (base + type[1] + 16);
		if (!name[0])
			name = NULL;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		name = NULL;
	}
	return name;
#else
	(void) record;
	return NULL;
#endif
}

/* Where the thread was: each frame's module and offset, from the handler's
 * own frames down through the dispatcher to the code that failed */
static void
log_backtrace ()
{
	void *frames[40];
	USHORT n = RtlCaptureStackBackTrace (0, 40, frames, NULL);

	fprintf (log_file, "       stack:");
	for (USHORT i = 0; i < n; i++) {
		HMODULE module = NULL;
		char path[MAX_PATH] = "?";
		const char *base = path;

		GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                    (LPCSTR) frames[i], &module);
		if (module) {
			GetModuleFileNameA (module, path, sizeof (path));
			const char *slash = strrchr (path, '\\');
			base = slash ? slash + 1 : path;
		}
		fprintf (log_file, "%s %s+0x%llX", i % 4 ? "" : "\n        ", base,
		         (unsigned long long) ((char *) frames[i] - (char *) module));
	}
	fprintf (log_file, "\n");
}

static LONG WINAPI
log_exception (EXCEPTION_POINTERS *info)
{
	if (log_file) {
		EXCEPTION_RECORD *record = info->ExceptionRecord;
		HMODULE module = NULL;
		char name[MAX_PATH] = "?";

		GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                    (LPCSTR) record->ExceptionAddress, &module);
		if (module)
			GetModuleFileNameA (module, name, sizeof (name));
		fprintf (log_file, "FATAL: exception %08lX at %p (%s+0x%llX), thread %lu\n",
		         (unsigned long) record->ExceptionCode, record->ExceptionAddress,
		         name, (unsigned long long) ((char *) record->ExceptionAddress - (char *) module),
		         (unsigned long) GetCurrentThreadId ());
		if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
		    record->NumberParameters >= 2)
			fprintf (log_file, "       %s of address %p\n",
			         record->ExceptionInformation[0] ? "write" : "read",
			         (void *) record->ExceptionInformation[1]);
		const char *type = cpp_exception_type (record);
		if (type)
			fprintf (log_file, "       a C++ exception, %s%s\n", type,
			         strstr (type, "bad_alloc") ? ": no memory left" : "");
		log_backtrace ();
		fflush (log_file);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void
hl::Log::open (const char *path, const char *old_path)
{
	if (old_path) {
		remove (old_path);
		rename (path, old_path);
	}
	log_file = fopen (path, "w");
	if (!log_file)
		return;

	signal (SIGABRT, log_abort);
#ifdef _WIN32
	SetUnhandledExceptionFilter (log_exception);
#endif
}

unsigned long
hl::Log::thread_id ()
{
#ifdef _WIN32
	return (unsigned long) GetCurrentThreadId ();
#else
	return (unsigned long) pthread_self ();
#endif
}

bool hl::Log::recover_aborts = false;
bool hl::Log::stdout_reserved = false;

void
hl::Log::fatal (const char *file, int line, const char *function, const char *fmt, ...)
{
	char message[1024], where[1536];
	const char *name;
	va_list va;

	va_start (va, fmt);
	vsnprintf (message, sizeof (message), fmt, va);
	va_end (va);
	/* The file's name, without the build's folders */
	for (name = file; *file; file++)
		if (*file == '/' || *file == '\\')
			name = file + 1;
	snprintf (where, sizeof (where), "%s:%d %s(): %s", name, line, function, message);

	write (stderr, "FATAL: %s\n", where);
	if (recover_aborts)
		throw Abort (where);
	abort ();
}

void
hl::Log::close ()
{
	if (log_file) {
		fclose (log_file);
		log_file = NULL;
	}
}

bool
hl_util_get_bool_option (const char *name, bool fallback)
{
	char *env = getenv (name);
	if (env) {
		if (!strcasecmp (env, "TRUE") || !strcmp (env, "1"))
			return true;
		if (!strcasecmp (env, "FALSE") || !strcmp (env, "0"))
			return false;
	}
	return fallback;
}

int
hl_util_get_int_option (const char *name, int fallback)
{
	char *env = getenv (name);
	if (env)
		return atoi (env);
	return fallback;
}
