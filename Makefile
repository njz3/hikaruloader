# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.
#
# Hikaruloader builds with CMake (see CMakeLists.txt and INSTALL.md); this
# Makefile, which used to build Valkyrie's C sources with gcc, only runs it.
# The dependencies (SDL2, GLEW, jansson, miniz, Dear ImGui with its OpenGL 3
# backend) must be visible to CMake, e.g. through vcpkg.

BUILD ?= build
CONFIG ?= Release

.PHONY: all clean

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(CONFIG)
	cmake --build $(BUILD) --config $(CONFIG)

clean:
	rm -rf $(BUILD)
