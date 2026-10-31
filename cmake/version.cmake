# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.
#
# Writes OUT (hl_version.h) with the program's version, from git's tags in
# SRC (DEVELOPMENT.md, "Versions and releases"):
# - HEAD on a tag vX.Y.Z, no tracked file changed: X.Y.Z, a release;
# - N commits after it, or files changed: X.Y.(Z+1)-dev.N+DATE.HASH, with
#   ".dirty" at the end when files changed;
# - no tag yet: NEXT-dev+DATE.HASH, NEXT the version in preparation
#   (project () in CMakeLists.txt); without git, NEXT-dev+unknown;
# - FORCE (HL_FORCE_VERSION), X.Y.Z given by hand: that release, whatever git
#   says, for a trial release without a commit nor a tag.
# Run at each build; the file is rewritten only when it changes, so that a
# build without new commits recompiles nothing.

execute_process (COMMAND git log -1 --format=%cd --date=format:%Y%m%d
	WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE date
	OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process (COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE hash
	OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process (COMMAND git status --porcelain --untracked-files=no
	WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE changes
	OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process (COMMAND git describe --tags --long --match "v[0-9]*"
	WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE described
	OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE no_tag)

set (build 0)
set (dev 1)
if (FORCE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
	set (major ${CMAKE_MATCH_1})
	set (minor ${CMAKE_MATCH_2})
	set (patch ${CMAKE_MATCH_3})
	set (dev 0)
elseif (NOT no_tag AND described MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g")
	set (major ${CMAKE_MATCH_1})
	set (minor ${CMAKE_MATCH_2})
	set (patch ${CMAKE_MATCH_3})
	set (build ${CMAKE_MATCH_4})
	if (build EQUAL 0 AND NOT changes)
		set (dev 0)
	else ()
		math (EXPR patch "${patch} + 1")
	endif ()
else ()
	string (REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" ok "${NEXT}")
	set (major ${CMAKE_MATCH_1})
	set (minor ${CMAKE_MATCH_2})
	set (patch ${CMAKE_MATCH_3})
endif ()

set (version "${major}.${minor}.${patch}")
if (dev)
	if (build GREATER 0)
		string (APPEND version "-dev.${build}")
	else ()
		string (APPEND version "-dev")
	endif ()
	if (hash)
		string (APPEND version "+${date}.${hash}")
		if (changes)
			string (APPEND version ".dirty")
		endif ()
	else ()
		string (APPEND version "+unknown")
	endif ()
endif ()

set (content "/* Written by cmake/version.cmake: plain #defines, for hikaruloader.rc too */
#define HL_VERSION \"${version}\"
#define HL_VERSION_MAJOR ${major}
#define HL_VERSION_MINOR ${minor}
#define HL_VERSION_PATCH ${patch}
#define HL_VERSION_BUILD ${build}
#define HL_VERSION_DEV ${dev}
#define HL_VERSION_RC ${major},${minor},${patch},${build}
")
if (EXISTS ${OUT})
	file (READ ${OUT} old)
endif ()
if (NOT "${old}" STREQUAL "${content}")
	file (WRITE ${OUT} "${content}")
endif ()
