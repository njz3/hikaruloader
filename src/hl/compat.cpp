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

#include "hl/core.h"

#ifdef _WIN32

char	*optarg = NULL;
int	 optind = 1, opterr = 1, optopt = 0;

/* Minimal POSIX getopt (): no argument permutation, no long options. */
int
getopt (int argc, char * const argv[], const char *optstring)
{
	static int pos = 1;
	const char *spec;
	char *arg;

	optarg = NULL;
	if (optind >= argc)
		return -1;

	arg = argv[optind];
	if (pos == 1) {
		if (arg[0] != '-' || arg[1] == '\0')
			return -1;
		if (!strcmp (arg, "--")) {
			optind++;
			return -1;
		}
	}

	optopt = arg[pos];
	spec = strchr (optstring, optopt);
	if (!spec || optopt == ':') {
		if (opterr && optstring[0] != ':')
			fprintf (stderr, "%s: invalid option -- '%c'\n",
			         argv[0], optopt);
		if (!arg[++pos]) {
			optind++;
			pos = 1;
		}
		return '?';
	}

	if (spec[1] == ':') {
		if (arg[pos + 1])
			optarg = &arg[pos + 1];
		else if (optind + 1 < argc)
			optarg = argv[++optind];
		else {
			if (opterr && optstring[0] != ':')
				fprintf (stderr, "%s: option requires an argument -- '%c'\n",
				         argv[0], optopt);
			optind++;
			pos = 1;
			return (optstring[0] == ':') ? ':' : '?';
		}
		optind++;
		pos = 1;
	} else if (!arg[++pos]) {
		optind++;
		pos = 1;
	}
	return optopt;
}

int
asprintf (char **strp, const char *fmt, ...)
{
	va_list va;
	int len;

	va_start (va, fmt);
	len = vsnprintf (NULL, 0, fmt, va);
	va_end (va);

	*strp = NULL;
	if (len < 0)
		return -1;

	*strp = (char *) malloc (len + 1);
	if (!*strp)
		return -1;

	va_start (va, fmt);
	vsnprintf (*strp, len + 1, fmt, va);
	va_end (va);
	return len;
}

#endif /* _WIN32 */
