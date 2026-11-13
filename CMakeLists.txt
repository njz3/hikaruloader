# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.

cmake_minimum_required (VERSION 3.21)

# The version in preparation, until git has a tag vX.Y.Z (cmake/version.cmake;
# DEVELOPMENT.md, "Versions and releases")
project (hikaruloader VERSION 0.8.2 LANGUAGES C CXX)

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
# stb_image, the picker's pictures (src/ui/ui_artwork.cpp)
find_path (STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

set (HL_SRC
	src/hl/core.cpp
	src/hl/state.cpp
	src/hl/buffer.cpp
	src/hl/mmap.cpp
	src/hl/machine.cpp
	src/hl/games.cpp
	src/hl/archive.cpp
	src/hl/romset.cpp
	src/hl/config.cpp
	src/hl/input.cpp
	src/hl/pointers.cpp
	src/hl/controls.cpp
	src/hl/renderer.cpp
	src/hl/window.cpp
	src/hl/displays.cpp
	src/hl/aspect.cpp
	src/hl/bezel.cpp
	src/hl/crt_preset.cpp
	src/hl/crt_filter.cpp
	src/hl/presentation.cpp
	src/hl/compat.cpp
	src/hl/jvs.cpp
	src/hl/eeprom.cpp
	src/hl/x76f100.cpp
	src/hl/nvram.cpp
	src/hl/paths.cpp
	src/hl/profile.cpp
	src/hl/ffb.cpp
	src/hl/gamecontrols.cpp
	src/hl/deviceprofiles.cpp
	src/hl/gearbox.cpp
	src/hl/driveboard.cpp
	src/hl/cardreader.cpp
	src/hl/outputs.cpp
	src/hl/audio.cpp)

set (HIKARU_SRC
	src/cpu/sh/sh4.cpp
	src/cpu/sh/sh4-blocks.cpp
	src/cpu/sh/sh4-wide-view.cpp
	src/cpu/sh/sh4-x64.cpp
	src/cpu/x64/x64-emitter.cpp
	src/cpu/z80/z80.cpp
	src/cpu/tlcs90/tlcs90.cpp
	src/cpu/arm/arm7.cpp
	src/cpu/m68k/m68000.cpp
	src/cpu/m68k/m68000-ops.cpp
	src/mach/hikaru/hikaru.cpp
	src/mach/hikaru/hikaru-mscomm.cpp
	src/mach/hikaru/hikaru-comm.cpp
	src/mach/hikaru/hikaru-commbd.cpp
	src/mach/hikaru/hikaru-mie.cpp
	src/mach/hikaru/hikaru-memctl.cpp
	src/mach/hikaru/hikaru-5881.cpp
	src/mach/hikaru/hikaru-renderer.cpp
	src/mach/hikaru/hikaru-gpu.cpp
	src/mach/hikaru/hikaru-gpu-cp.cpp
	src/mach/hikaru/hikaru-gpu-private.cpp
	src/mach/hikaru/hikaru-aica.cpp
	src/mach/gaelco/gaelco.cpp
	src/mach/naomi/naomi.cpp
	src/mach/naomi/naomi-holly.cpp
	src/mach/naomi/naomi-elan.cpp
	src/mach/naomi/naomi-cart.cpp
	src/mach/naomi/naomi-mie.cpp
	src/mach/naomi/naomi-comm.cpp
	src/mach/naomi/naomi-multiboard.cpp
	src/mach/naomi/naomi-renderer.cpp
	src/mach/naomi/naomi-servo.cpp
	src/mach/naomi/naomi-card.cpp
	src/mach/naomi/naomi-hopper.cpp
	src/mach/naomi/naomi-g2board.cpp
	src/mach/naomi/naomi-aw.cpp
	src/mach/naomi/naomi-gd.cpp
	src/mach/naomi/naomi-netdimm.cpp
	src/mach/naomi/naomi-smcp.cpp
	src/hl/driveboard-f355.cpp
	src/hl/driveboard-jambo.cpp
	src/hl/driveboard-z80.cpp
	src/hl/tmp90ph44.cpp
	src/hl/ioboard.cpp
	src/mach/gaelco/gaelco-gpu.cpp)

# RelWithDebInfo as fast as Release, its symbols kept (the debugger's, the
# profiler's): MSVC's inlines there only what is marked inline (/Ob1) and
# links incrementally, through a jump at each call (the ARM7 interpreter's
# helpers, the GPU's and the AICAs' code a few percent slower)
if (MSVC)
	foreach (lang C CXX)
		string (REPLACE "/Ob1" "/Ob2" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
	endforeach ()
	string (REPLACE "/INCREMENTAL" "/INCREMENTAL:NO" CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
	        "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")
endif ()

# The ImGui interface: the picker and the Tab settings. imgui_impl_sdl2 comes
# from Dear ImGui 1.92.6, as vcpkg's imgui no longer builds it.
set (UI_SRC
	src/ui/ui_artwork.cpp
	src/ui/ui_common.cpp
	src/ui/ui_controls.cpp
	src/ui/ui_folder.cpp
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

# The LZMA SDK 26.04's 7z decoder, from 3rdparty/lzma (public domain, Igor
# Pavlov; lzma-sdk.txt): the romsets in .7z (src/hl/archive.cpp). PPMd too,
# which 7-Zip can be asked for.
set (LZMA_NAMES 7zAlloc 7zArcIn 7zBuf 7zBuf2 7zCrc 7zCrcOpt 7zDec 7zFile
	7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec Ppmd7
	Ppmd7Dec)
list (TRANSFORM LZMA_NAMES PREPEND 3rdparty/lzma/ OUTPUT_VARIABLE LZMA_SRC)
list (TRANSFORM LZMA_SRC APPEND .c)
add_library (lzma STATIC ${LZMA_SRC})
target_include_directories (lzma PUBLIC 3rdparty/lzma)
target_compile_definitions (lzma PRIVATE Z7_PPMD_SUPPORT)
if (MSVC)
	target_compile_definitions (lzma PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

# libchdr, the GD-ROM images (BSD-3-Clause, Romain Tisserand; with its LZMA,
# zlib and zstd, 3rdparty/libchdr/deps)
set (BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory (3rdparty/libchdr EXCLUDE_FROM_ALL)

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
# stb_image, the bezels' pictures (src/hl/bezel.cpp, a static copy)
target_include_directories (hl PRIVATE ${STB_INCLUDE_DIRS})
target_compile_definitions (hl PUBLIC HL_HAVE_HIKARU)
target_link_libraries (hl PUBLIC
	OpenGL::GL GLEW::GLEW SDL2::SDL2 jansson::jansson miniz::miniz lzma chdr-static)
if (WIN32)
	# The outputs' server (src/hl/outputs.cpp); HR_SAMPLE's names
	# (src/hl/profile.cpp)
	target_link_libraries (hl PUBLIC ws2_32 dbghelp)
endif ()

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
	src/hl/telemetry.cpp src/hl/telemetry-games.cpp src/hl/ffb-effects.cpp
	src/hl/main.cpp)
target_include_directories (hikaruloader PRIVATE 3rdparty/imgui_backends ${STB_INCLUDE_DIRS})
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
# example of init.lua; the interface's languages (data/lang/*.po); the CRT
# presets (data/crt, Lola Retro's with its licence); the games' lists of
# shader programs (data/shaders); the devices' profiles (data/devices).
add_custom_command (TARGET hikaruloader POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:hikaruloader>/data
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/data/games.json $<TARGET_FILE_DIR:hikaruloader>/data/games.json
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_SOURCE_DIR}/data/init.example.lua $<TARGET_FILE_DIR:hikaruloader>/data/init.example.lua
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/lang $<TARGET_FILE_DIR:hikaruloader>/data/lang
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/crt $<TARGET_FILE_DIR:hikaruloader>/data/crt
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/naomi $<TARGET_FILE_DIR:hikaruloader>/data/naomi
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/shaders $<TARGET_FILE_DIR:hikaruloader>/data/shaders
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_SOURCE_DIR}/data/devices $<TARGET_FILE_DIR:hikaruloader>/data/devices)

# The CPU cores' tests: ctest -C RelWithDebInfo
enable_testing ()
add_executable (z80test tests/z80_test.cpp src/cpu/z80/z80.cpp)
target_link_libraries (z80test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (z80test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME z80 COMMAND z80test)
add_executable (tlcs90test tests/tlcs90_test.cpp src/cpu/tlcs90/tlcs90.cpp)
target_link_libraries (tlcs90test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (tlcs90test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME tlcs90 COMMAND tlcs90test)
add_executable (x76f100test tests/x76f100_test.cpp)
target_link_libraries (x76f100test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (x76f100test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME x76f100 COMMAND x76f100test)
add_executable (arm7test tests/arm7_test.cpp src/cpu/arm/arm7.cpp)
target_link_libraries (arm7test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (arm7test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME arm7 COMMAND arm7test)
add_executable (m68000test tests/m68000_test.cpp src/cpu/m68k/m68000.cpp src/cpu/m68k/m68000-ops.cpp)
target_link_libraries (m68000test PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (m68000test PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME m68000 COMMAND m68000test)
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
add_executable (romsettest tests/romset_test.cpp)
target_link_libraries (romsettest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (romsettest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME romset COMMAND romsettest)
# The wheel's rotation and the controllers' hotkeys, on SDL's virtual
# joysticks
add_executable (controlstest tests/controls_test.cpp)
target_link_libraries (controlstest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (controlstest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME controls COMMAND controlstest ${CMAKE_SOURCE_DIR}/data/games.json)
# The gear lever's rules (SPECS.md), scenario by scenario, and the games'
# levers from data/games.json
add_executable (gearboxtest tests/gearbox_test.cpp)
target_link_libraries (gearboxtest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (gearboxtest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME gearbox COMMAND gearboxtest ${CMAKE_SOURCE_DIR}/data/games.json)
add_executable (driveboardtest tests/driveboard_test.cpp src/hl/driveboard-jambo.cpp)
target_link_libraries (driveboardtest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (driveboardtest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME driveboard COMMAND driveboardtest)
add_executable (cardreadertest tests/cardreader_test.cpp)
target_link_libraries (cardreadertest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (cardreadertest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME cardreader COMMAND cardreadertest)
add_executable (aspecttest tests/aspect_test.cpp)
target_link_libraries (aspecttest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (aspecttest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME aspect COMMAND aspecttest)
add_executable (crtpresettest tests/crt_preset_test.cpp)
target_link_libraries (crtpresettest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (crtpresettest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME crt_preset COMMAND crtpresettest ${CMAKE_SOURCE_DIR}/data/crt)
add_executable (bezeltest tests/bezel_test.cpp)
target_link_libraries (bezeltest PRIVATE hl)
if (TARGET SDL2::SDL2main)
	target_link_libraries (bezeltest PRIVATE SDL2::SDL2main)
endif ()
add_test (NAME bezel COMMAND bezeltest)
add_executable (potest tests/po_test.cpp src/ui/ui_po.cpp)
target_include_directories (potest PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test (NAME po COMMAND potest ${CMAKE_SOURCE_DIR}/data/lang/fr.po)
