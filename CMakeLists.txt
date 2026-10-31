# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.

cmake_minimum_required (VERSION 3.21)

# The version in preparation, until git has a tag vX.Y.Z (cmake/version.cmake;
# DEVELOPMENT.md, "Versions and releases")
project (hikaruloader VERSION 0.1.1 LANGUAGES CXX)

# A release's version given by hand, for a trial release without a commit
# nor a tag: cmake -DHL_FORCE_VERSION=0.1.0 (empty: git's tags)
set (HL_FORCE_VERSION "" CACHE STRING "A release version given by hand (a trial release), else git's tags")

# Like the Makefile, keep HL_ASSERT and HL_LOG (-v) in optimized builds.
foreach (config RELEASE RELWITHDEBINFO MINSIZEREL)
	string (REGEX REPLACE "[/-]DNDEBUG" ""
		CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
endforeach ()

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

find_package (OpenGL REQUIRED)
find_package (GLEW REQUIRED)
find_package (SDL2 CONFIG REQUIRED)
find_package (jansson CONFIG REQUIRED)
find_package (miniz CONFIG REQUIRED)
find_package (imgui CONFIG REQUIRED)

set (HL_SRC
	src/hl/core.cpp
	src/hl/state.cpp
	src/hl/buffer.cpp
	src/hl/mmap.cpp
	src/hl/machine.cpp
	src/hl/games.cpp
	src/hl/romset.cpp
	src/hl/config.cpp
	src/hl/input.cpp
	src/hl/controls.cpp
	src/hl/renderer.cpp
	src/hl/window.cpp
	src/hl/compat.cpp
	src/hl/jvs.cpp
	src/hl/eeprom.cpp
	src/hl/nvram.cpp
	src/hl/paths.cpp
	src/hl/profile.cpp
	src/hl/ffb.cpp
	src/hl/driveboard.cpp
	src/hl/audio.cpp)

set (HIKARU_SRC
	src/cpu/sh/sh4.cpp
	src/cpu/sh/sh4-blocks.cpp
	src/cpu/sh/sh4-x64.cpp
	src/cpu/x64/x64-emitter.cpp
	src/cpu/z80/z80.cpp
	src/cpu/arm/arm7.cpp
	src/mach/hikaru/hikaru.cpp
	src/mach/hikaru/hikaru-mscomm.cpp
	src/mach/hikaru/hikaru-comm.cpp
	src/mach/hikaru/hikaru-mie.cpp
	src/mach/hikaru/hikaru-memctl.cpp
	src/mach/hikaru/hikaru-renderer.cpp
	src/mach/hikaru/hikaru-gpu.cpp
	src/mach/hikaru/hikaru-gpu-cp.cpp
	src/mach/hikaru/hikaru-gpu-private.cpp
	src/mach/hikaru/hikaru-aica.cpp
	src/mach/gaelco/gaelco.cpp
	src/hl/driveboard-z80.cpp
	src/mach/gaelco/gaelco-gpu.cpp)

# The ARM7 interpreter runs some 20 million instructions a second (the two
# AICAs'): its small helpers inlined, in RelWithDebInfo too (/Ob1 there)
if (MSVC)
	set_source_files_properties (src/cpu/arm/arm7.cpp PROPERTIES COMPILE_OPTIONS /Ob2)
endif ()

# The ImGui interface: the picker and the Tab settings. imgui_impl_sdl2 comes
# from Dear ImGui 1.92.6, as vcpkg's imgui no longer builds it.
set (UI_SRC
	src/ui/ui_common.cpp
	src/ui/ui_controls.cpp
	src/ui/ui_picker.cpp
	src/ui/ui_overlay.cpp
	src/ui/ui_po.cpp
	3rdparty/imgui_backends/imgui_impl_sdl2.cpp)

# Lua 5.5.1, from the 3rdparty/lua submodule: the language of the shell. Built
# as C++, so that its errors unwind the C++ frames of the bindings.
set (LUA_NAMES lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo
	ldump lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes
	loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib
	lvm lzio)
list (TRANSFORM LUA_NAMES PREPEND 3rdparty/lua/ OUTPUT_VARIABLE LUA_SRC)
list (TRANSFORM LUA_SRC APPEND .c)
set_source_files_properties (${LUA_SRC} PROPERTIES LANGUAGE CXX)
add_library (lua STATIC ${LUA_SRC})
target_include_directories (lua PUBLIC 3rdparty/lua)
if (MSVC)
	target_compile_definitions (lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else ()
	target_compile_definitions (lua PRIVATE LUA_USE_POSIX)
	target_link_libraries (lua PUBLIC m)
endif ()

# hl_version.h: the version from git's tags, refreshed at each build
set (HL_GENERATED ${CMAKE_BINARY_DIR}/generated)
set (HL_VERSION_ARGS -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${HL_GENERATED}/hl_version.h
	-DNEXT=${PROJECT_VERSION} -DFORCE=${HL_FORCE_VERSION}
	-P ${CMAKE_SOURCE_DIR}/cmake/version.cmake)
execute_process (COMMAND ${CMAKE_COMMAND} ${HL_VERSION_ARGS})
add_custom_target (hl_version ALL
	COMMAND ${CMAKE_COMMAND} ${HL_VERSION_ARGS}
	BYPRODUCTS ${HL_GENERATED}/hl_version.h)

add_library (hl STATIC ${HL_SRC})
add_dependencies (hl hl_version)
target_include_directories (hl PUBLIC src ${HL_GENERATED})
target_compile_definitions (hl PUBLIC HL_HAVE_HIKARU)
target_link_libraries (hl PUBLIC
	OpenGL::GL GLEW::GLEW SDL2::SDL2 jansson::jansson miniz::miniz)

if (MSVC)
	# The sources are UTF-8 (the interface's French texts).
	target_compile_options (hl PUBLIC /W3 /utf-8 /Zc:preprocessor)
	target_compile_definitions (hl PUBLIC
		_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
else ()
	target_compile_options (hl PUBLIC
		-Wall -Wno-strict-aliasing -Wno-format -Wno-unused-local-typedefs)
	target_link_libraries (hl PUBLIC m)
endif ()

add_executable (hikaruloader ${HIKARU_SRC} ${UI_SRC}
	src/hl/emulator.cpp src/hl/server.cpp src/hl/ringlink.cpp src/hl/shell.cpp src/hl/agent.cpp
	src/hl/main.cpp)
target_include_directories (hikaruloader PRIVATE 3rdparty/imgui_backends)
target_link_libraries (hikaruloader PRIVATE hl imgui::imgui lua)
if (WIN32)
	target_link_libraries (hikaruloader PRIVATE ws2_32)
	# The icon, "HL" (data/hikaruloader.ico, tools/make_icon.py)
	target_sources (hikaruloader PRIVATE src/hl/hikaruloader.rc)
endif ()
if (TARGET SDL2::SDL2main)
	target_link_libraries (hikaruloader PRIVATE SDL2::SDL2main)
endif ()

add_executable (hlbswap src/utils/bswap.cpp)
target_link_libraries (hlbswap PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (hlbswap PRIVATE SDL2::SDL2main)
endif ()

# Keep the game list next to the binary, where hikaruloader looks first; the
# example of init.lua; the interface's languages (data/lang/*.po).
add_custom_command (TARGET hikaruloader POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:hikaruloader>/data
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/data/games.json $<TARGET_FILE_DIR:hikaruloader>/data/games.json
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/data/init.example.lua $<TARGET_FILE_DIR:hikaruloader>/data/init.example.lua
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/lang $<TARGET_FILE_DIR:hikaruloader>/data/lang)

# The CPU cores' tests: ctest -C RelWithDebInfo
enable_testing ()
add_executable (z80test tests/z80_test.cpp src/cpu/z80/z80.cpp)
target_link_libraries (z80test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (z80test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME z80 COMMAND z80test)
add_executable (arm7test tests/arm7_test.cpp src/cpu/arm/arm7.cpp)
target_link_libraries (arm7test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (arm7test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME arm7 COMMAND arm7test)
add_executable (aicatest tests/aica_test.cpp)
target_include_directories (aicatest PRIVATE ${CMAKE_SOURCE_DIR}/src)
if (TARGET SDL2::SDL2main)
	target_link_libraries (aicatest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME aica COMMAND aicatest)
add_executable (sh4mmutest tests/sh4_mmu_test.cpp src/cpu/sh/sh4.cpp
	src/cpu/sh/sh4-blocks.cpp src/cpu/sh/sh4-x64.cpp src/cpu/x64/x64-emitter.cpp)
target_link_libraries (sh4mmutest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (sh4mmutest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME sh4mmu COMMAND sh4mmutest)
# The drive board's Z80 against a journal of Supermodel's (needs EPR-20512,
# not ours to ship: run by hand, not by ctest)
add_executable (driveboardz80test tests/driveboard_z80_test.cpp src/hl/driveboard-z80.cpp src/cpu/z80/z80.cpp)
target_link_libraries (driveboardz80test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (driveboardz80test PRIVATE SDL2::SDL2main)
endif ()
add_executable (audiotest tests/audio_test.cpp)
target_link_libraries (audiotest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (audiotest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME audio COMMAND audiotest)
add_executable (eepromtest tests/eeprom_test.cpp)
target_link_libraries (eepromtest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (eepromtest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME eeprom COMMAND eepromtest)
add_executable (nvramtest tests/nvram_test.cpp)
target_link_libraries (nvramtest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (nvramtest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME nvram COMMAND nvramtest)
add_executable (pathstest tests/paths_test.cpp)
target_link_libraries (pathstest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (pathstest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME paths COMMAND pathstest)
add_executable (driveboardtest tests/driveboard_test.cpp)
target_link_libraries (driveboardtest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (driveboardtest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME driveboard COMMAND driveboardtest)
add_executable (potest tests/po_test.cpp src/ui/ui_po.cpp)
target_include_directories (potest PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test (NAME po COMMAND potest ${CMAKE_SOURCE_DIR}/data/lang/fr.po)
