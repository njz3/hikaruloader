Installing Hikaruloader
=======================

Two ways in: a release archive, ready to run (chapter 1), or the sources,
built with Visual Studio (chapter 2). Using the program, its settings and its
command line are in [README.md](README.md).


1. From a release archive
-------------------------

### What you need

- Windows 10 or 11, 64-bit, and a graphics card with OpenGL 3.1, its
  drivers up to date. Nothing else: the release's program is built static,
  its libraries and the Visual C++ runtime inside it.
- The games, as MAME romsets: see "What it needs" in [README.md](README.md).

### Installing

1. Unzip the archive into a folder of its own where you can write, for
   example `C:\Games\Hikaruloader` (not under `Program Files`: the program
   writes its settings and saves beside itself). The folder holds:

   | File | What it is |
   |---|---|
   | `hikaruloader.exe` | The program, all its libraries inside |
   | `data\games.json` | The list of the games and of their files |
   | `data\lang\` | The interface's translations (French) |
   | `data\crt\` | The CRT presets: Lola Retro's, by Zoki, with their licence (MIT, `data\crt\lola-retro\LICENSE`) |
   | `README.md`, `INSTALL.md`, `CHANGELOG.md`, `LICENSE` | The manual, this file, what each version brings, the licence |

2. Put the games' zips in a folder: a `roms` folder beside the program is
   found by itself. Any other folder: give it in the settings (Tab, Picker
   tab, Games folder, or *Add a folder...*), or with `-R` on the command
   line. Several folders, separated by `;`, are looked in in that order.
3. Start `hikaruloader.exe`. The picker lists the games, Ready when all their
   files are there; Enter launches the one chosen. The keys are in
   [README.md](README.md), "Keys and controllers".

### Starting it

- Without arguments (a double click, a shortcut): the picker.
- A game at once, without the picker: `hikaruloader.exe -r sgnascar`; with
  another games folder: `hikaruloader.exe -R D:\roms -r sgnascar`. Escape
  then quits, back to the frontend that started it (a cabinet's). All the
  options are in [README.md](README.md), "Command line".

### Where the files are

Beside `hikaruloader.exe`:

| File | What it is |
|---|---|
| `hikaruloader.ini` | The settings (Tab, then Save), written at the first start with a comment for each one: its values, its default |
| `controls.ini` | The keys and controllers, written likewise |
| `hikaruloader.log`, `hikaruloader.old.log` | The messages of the last start and of the one before (the Picker tab turns them off) |
| `nvram\<game>.nvram` | Each game's EEPROM and battery-backed RAM: its settings, records and bookkeeping |
| `nvram\von4.card` | Virtual-On 4's card (F7), made by the game's card terminal (README.md, "Virtual-On 4's cards") |
| `<game>.hlstate` | Each game's save state (F4 saves, F1 loads) |
| `artwork\`, `cache\artwork\` | The picker's pictures: yours, and those downloaded when the Picker tab says so (README.md, "Settings") |
| `artwork\bezels\` | Your bezel pictures, `<set>.png` and `<set>.info` (Batocera's format), shown with the Display tab's *Bezel* (README.md, "Settings") |

With a write directory (`--write-dir=<folder>`, or `WRITE_DIR` in the
`[Picker]` section of the `hikaruloader.ini` beside the program), they all go
there instead: the settings, the controls and the log at its root, each
game's files in `<folder>\<game>\`. See "Write directory" in
[README.md](README.md).

To go back to the default settings, delete `hikaruloader.ini` (and
`controls.ini` for the controls): they are written again at the next start.
Deleting a game's `.nvram` gives it a new board's settings.

### Updating

Unzip the new archive over the old folder. A release archive holds no
settings and no saves: yours stay. `data\games.json` is replaced, with the
new games and files, and so is `data\crt\lola-retro\`; presets of your own
in another folder of `data\crt\` stay. A save state of an older version may
not load.


2. Building from the sources
----------------------------

Step by step, on Windows, with Visual Studio, CMake and vcpkg.

### 1. The tools

- Visual Studio 2022 or later (Community will do), with the "Desktop
  development with C++" workload: it brings the MSVC compiler and CMake
  (3.21 or later).
- Git for Windows.
- vcpkg, which fetches and builds the libraries:

  ```
  git clone https://github.com/microsoft/vcpkg C:\libs\vcpkg
  C:\libs\vcpkg\bootstrap-vcpkg.bat
  setx VCPKG_ROOT C:\libs\vcpkg
  ```

  then open a new terminal, for `VCPKG_ROOT` to be set there.
- Python 3, optionally, for the tools of `tools\` (the translations, see
  [DEVELOPMENT.md](DEVELOPMENT.md)).

### 2. The sources

```
git clone --recursive https://github.com/njz3/hikaruloader.git
cd hikaruloader
```

`--recursive` also fetches Lua 5.5.1, the git submodule `3rdparty/lua`. In a
clone made without it:

```
git submodule update --init
```

The LZMA SDK's 7z decoder (26.04, public domain, `3rdparty/lzma`, its
`lzma-sdk.txt`) comes with the sources, built with them: the romsets in 7z.

### 3. Configuring

In a "Developer Command Prompt for VS" (or any command prompt), in the
sources' folder:

```
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
```

(In PowerShell, `$env:VCPKG_ROOT` in place of `%VCPKG_ROOT%`.) The first
time, vcpkg reads `vcpkg.json` and builds SDL2, GLEW, jansson, miniz and
Dear ImGui with its OpenGL 3 backend, for x64: several minutes, then they
are kept.

### 4. Building

```
cmake --build build --config Release
```

(`RelWithDebInfo` in place of `Release` keeps the debugging symbols, at the
same speed.) This makes `build\Release\hikaruloader.exe`, with its DLLs and
`data\` copied beside it: it runs from there.

```
build\Release\hikaruloader.exe -R D:\roms
```

In Visual Studio: open `build\hikaruloader.sln` (`.slnx` with Visual Studio
2026), make `hikaruloader` the startup project, F5. Or File, Open, Folder on
the sources' folder: Visual Studio reads `CMakeLists.txt` itself.

The `Makefile` runs the same CMake (`make`, with `BUILD` and `CONFIG`), for
the systems where the libraries are visible to CMake otherwise.

### 5. Testing

```
cd build
ctest -C Release
```

runs the unit tests: the processors (SH-4 MMU, Z80, ARM7), the sound (the
AICA's arithmetic, the output's resampling and rate), the EEPROM, the nvram
file, the write directory, the drive board, the translations.

### 6. Making a release archive

The releases are built static: vcpkg's libraries in their static form
(the `x64-windows-static` triplet) and the Visual C++ runtime linked in
(`/MT`), so that the program needs no DLL but Windows' own and no
redistributable. In a build folder of its own, beside `build`:

```
cmake -S . -B build-static -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build-static --config Release --target hikaruloader
```

The first configuring builds the static libraries (a few minutes). This
makes `build-static\Release\hikaruloader.exe` (some 4 MB) and its `data\`.
To check that it needs no other DLL, in a "Developer Command Prompt for VS":

```
dumpbin /dependents build-static\Release\hikaruloader.exe
```

lists only Windows' own (`KERNEL32.dll`, `OPENGL32.dll`, `USER32.dll`...).
Then the archive:

```
python tools/make_release.py
```

checks that the build is a release (its version, see "Versions and releases"
in [DEVELOPMENT.md](DEVELOPMENT.md)), that the program carries that version
and needs no DLL but Windows' own, then makes
`dist\hikaruloader-<version>-win64.zip` and its SHA-256: the program, its
`data\`, `README.md`, `INSTALL.md`, `CHANGELOG.md` and `LICENSE` (not `TODO.md`),
in a folder `hikaruloader-<version>\`, and none of the `.ini`, `.log`,
`nvram` and `.hlstate` of your own runs, so that an update keeps its users'
own. `--out <folder>` puts the archive elsewhere. The static program gives
the same pictures and sound as the other one; `build` stays the developers'
(its DLLs beside the program, faster to link).

### Further

- The emulator's insides and its debugging tools: [DEVELOPMENT.md](DEVELOPMENT.md).
- What is left to emulate: [TODO.md](https://github.com/njz3/hikaruloader/blob/main/TODO.md).
- Valkyrie's original instructions for Linux, for reference: at the end of
  [README.md](README.md).
