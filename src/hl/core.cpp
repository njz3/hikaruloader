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
		if (stream == stderr)
			fflush (log_file);
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
