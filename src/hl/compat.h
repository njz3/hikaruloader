/*
 * Hikaruloader (formerly Valkyrie)
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

#ifndef __HL_COMPAT_H__
#define __HL_COMPAT_H__

/* Replacements for the POSIX/GNU functions missing from the MSVC runtime. */

#ifdef _WIN32

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#ifdef __cplusplus
extern "C" {
#endif

extern char	*optarg;
extern int	 optind, opterr, optopt;

int	getopt (int argc, char * const argv[], const char *optstring);
int	asprintf (char **strp, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* __HL_COMPAT_H__ */
