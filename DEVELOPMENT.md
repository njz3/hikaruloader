Hikaruloader for developers
===========================

Hikaruloader, Copyright (C) 2026 Benjamin Maurin (njz3). Released under the
GNU GPL v3 or later: see LICENSE.

Notes on the emulator's insides and its debugging tools, for those who work
on it. The user's manual is [README.md](README.md); building the program is
in [INSTALL.md](INSTALL.md); what is left to do in [TODO.md](TODO.md); how
some controls must behave, rule by rule with their tests (the gear lever,
Star Wars: Racer Arcade's levers), in [SPECS.md](SPECS.md).


SH-4 dynamic recompiler
-----------------------

The two SH-4s run their code from blocks decoded once and translated to
x86-64 (on Windows; the blocks alone elsewhere), and run forward the loops
that only wait. The settings (Tab, Emulation, "SH-4 processors") switch
them to the interpreter, from the next frame on: both give the same machine,
frame for frame. In the Lua shell, `cpu:option ('jit', 0)`, `('blocks', 0)`
and `('idle', 0)` turn these off one at a time.

The CPUs find their RAM and ROM through a table of pages (a host pointer
for each 4 KB page), the devices through their regions. The settings'
"Memory access" goes back to the regions alone, searched one after the
other, or to fastmem, from the next frame on (`cpu:option ('pages', 0)`,
`('fastmem', 1)` in the shell). Fastmem reserves 512 MB of address space
for each SH-4 and maps their RAM and ROM there a second time, at the
addresses they see (Windows 10 1803 and later): some 1 GB of address space
and 45 MB of working set more for SGNASCAR, for no measurable gain. The
dynarec writes its 32-bit loads and stores out for whichever way is chosen
(a class each: the interpreter's path, the page table, fastmem), the
devices always through the interpreter's path; the same machine all three.


Languages
---------

The interface's texts are in English in the sources, given to `tr ()`; the
translations are gettext catalogs, `data/lang/<code>.po` beside the program
(French: `fr.po`). The settings (Tab, Picker, "Language") list English and
the catalogs found, each by its own name; "Automatic" takes the system's
language when there is a catalog for it, English otherwise. A text with no
translation (an empty or fuzzy `msgstr`) shows in English.

To add a language, from the sources' folder:

```
python tools/po_update.py --new de Deutsch
```

makes `data/lang/de.po` from the template `data/lang/hikaruloader.pot`, to
translate with any .po editor (Poedit, Lokalize...) or by hand. Once the
interface's texts change, `python tools/po_update.py` makes the template
again from the sources and brings every catalog up to date (the new texts
added untranslated, those gone kept as obsolete); `--check` lists what is
left to translate. The catalog's header names the language for the list:
`X-Language-Name: Deutsch`.


Lua shell
---------

```
hikaruloader.exe -r airtrix -s 7070
```

opens a Lua 5.5 shell on 127.0.0.1:7070 (this machine only: the shell runs
whatever it is given). Connect with telnet, or with `tools/hlshell.py`:

```
python tools/hlshell.py -p 7070 "emu.pause ()" "regs ()"
```

`-x FILE` runs a Lua script once the game is loaded (after `init.lua`, below).

One line is one command; an expression prints its values, and a command left
open (a `for ... do` line) goes on with the `>> ` prompt. `help ()` lists the
commands, `ls ()` the machine's objects:

| Object | What it is |
|---|---|
| `emu` | The emulator: `pause ()`, `resume ()`, `step ([frames])`, `frame ()`, `status ()`, `reset ()`, `save_state (path)`, `load_state (path)`, `screenshot (path)`, `input (name, held)`, `hook (event, fn)`, `quit ()`... |
| `machine.cpus.master` | The SH-4s, also `machine.cpus.slave`: their registers as fields (`cpu.pc`, `cpu.r0`, `cpu.sr`, `cpu.fr0`..., to read and write); `cpu:step ([count])` runs count instructions of that CPU alone; `cpu:bpset (addr)`, `cpu:bpclear ([addr])`, `cpu:bplist ()` |
| `machine.memory.ram_m` | The memories: `ram_m`, `ram_s`, `cmdram`, `fb`, `texram0`, `texram1`, `aica_ram_m`, `aica_ram_s`, `mie_ram`, `bram`, `bootrom`, `eprom`, `maskrom`, `iregs_m`, `iregs_s` (the Hikaru's; a NAOMI's `ram`, `vram`, `sram`, `aica_ram`, `iregs`, its other boards' with their number after, `ram1`, `vram2`...); `mem.size`, `mem:dump (path [, offset [, length]])`, `mem:load (path [, offset])` |
| `machine.devices.gpu` | The chips' registers: `gpu`, `memctl_m`, `memctl_s`, `mie`, `mscomm`, `aica_m`, `aica_s`...; `dev:info ()`, their state in a line; `dev:option (name, value)`, a debugging option, e.g. the GPU's `log_cp` (the command lists, disassembled to the log, with `emu.verbose (1)`) |

To find where a CPU spends its time: `cpu:profile (true)`, run some frames,
then `p = cpu:profile ()` gives the instructions run per address, and
`hot (p)` prints the busiest ones. To find who writes a variable:
`cpu:wpset (addr [, length])`, then a `"write"` hook (below) hears each write
there, with the writer's PC.

The CPUs (through their address space, on-chip registers included), the
memories (by offset) and the devices (by register address) all have
`read_u8 (addr)` ... `read_u64`, `read_i8` ... `read_i64`,
`write_u8 (addr, value)` ... `write_u64`, `read (addr, length)` giving a
string, and `write (addr, bytes)`. Helpers: `hex (value)`,
`hexdump (obj, addr [, length])`, `regs ([cpu])`, `f32 (bits)`,
`bits32 (number)`.

A breakpoint stops its CPU before the instruction; the rest of the machine
finishes the frame, then the emulator pauses and the clients read
`*** break: master at 0C001000, frame 123`. With the shell open, an abort
(`HL_ABORT`, an assertion) no longer ends the program: the machine stays
paused as it failed, for the shell to look at it; it runs again once reset
(`emu.reset ()`) or a state loaded.


Versions and releases
---------------------

The version comes from git's tags, through `cmake/version.cmake`, at each
build (`hl_version.h`: the window's title, the log, the settings' About
tab, the program's Properties in the Explorer):

| Where the sources are | Version |
|---|---|
| On a tag `vX.Y.Z`, no tracked file changed | `X.Y.Z`: a release |
| N commits after it | `X.Y.(Z+1)-dev.N+DATE.HASH` |
| Tracked files changed | the same, ending in `.dirty` |
| No tag yet | `NEXT-dev+DATE.HASH`, NEXT the `project ()` version of `CMakeLists.txt` (the one in preparation) |
| Configured with `-DHL_FORCE_VERSION=X.Y.Z` | `X.Y.Z` whatever git says: a trial release |

The numbers follow semantic versioning: 0.x while the games are not all
playable from start to end; the minor number for new features, the patch
number for fixes alone. A development snapshot says so in the log, and its
file is marked prerelease.

A release:

1. Move `CHANGELOG.md`'s entry in preparation to the version, in plain
   words for those who play; set `project (... VERSION ...)` in
   `CMakeLists.txt` to the next one.
2. Commit, then tag that commit `vX.Y.Z` (`git tag -a v0.1.0 -m "..."`).
3. Build it static (INSTALL.md, chapter 2, step 6), then
   `python tools/make_release.py`: `dist\hikaruloader-X.Y.Z-win64.zip` and
   its `.sha256`. It refuses a development snapshot.
4. Unzip it into an empty folder, start a game or two from there; then
   push the commit and the tag, and publish the zip and its `.sha256`
   (`gh release create vX.Y.Z dist\hikaruloader-X.Y.Z-win64.zip ...
   --notes-file <the changelog's entry>`).

A trial release (the build and the tools, before any commit): configure
`build-static` with `-DHL_FORCE_VERSION=X.Y.Z`, build, run
`tools/make_release.py --out <folder>`; then configure again with
`-DHL_FORCE_VERSION=` (empty), for the next builds to take git's version.

The files keep versions of their own, apart from the program's: the nvram
container (`HLNV`, 1), the save states (0 until the first release; then a
new number at each change of their layout, a state of another number
refused with a message), `games.json`. The ini files need none: their
unknown keys are left aside, the missing ones take their defaults.


Profiling and debugging variables
---------------------------------

In the environment:

- `HR_PROFILE=1` measures each part of the frames (the SH-4s, the NAOMI's master one and its Elan apart, the GPU, the
  MIE, the AICAs and their ARM7s, voices and effects, the renderer and its
  drawing thread, the sound output) and writes the tree to the log on the
  way out, in milliseconds a frame. `HR_ARM_PROFILE=1` and
  `HR_Z80_PROFILE=1` add the busiest addresses of the AICAs' ARM7s and of
  the MIE's Z80, for their idle loops (they run them one instruction at a
  time, slower). With `HR_PROFILE`, each SH-4 also logs its cycles, those
  its idle loops took off ("sh4 M: ... skipped by the idle loops"), its
  blocks decoded and translated, the time its translations took, the
  arena's clears (all translated again) and its code epochs, and the share
  of its translated blocks gone on to the next without the run loop; and
  the frames' own CPU time on their thread (Windows: without the time other
  programs took the core, which the other times count, for a comparison on
  a busy machine);
  the drawing thread its time a frame ("renderer: the drawing thread: ...
  ms each": the frame can go no faster).
- `HR_SH4_CHAIN=0`: the SH-4s' translations go back to the run loop after
  each block (sh4.cpp, "Chaining"), as before November 2026, for a
  comparison.
- `HR_SH4_EPOCH_CHECK=1`: each SH-4 block's code compared with the memory
  at each entry all the same, and a rewrite the code epochs missed logged
  ("SH-4 block @... rewritten, the code epoch the same";
  `src/cpu/sh/sh4-blocks.h`): for a machine or a writer added.
- `HR_SAMPLE=1` (Windows): a sampling profile of the machine's thread and
  of the drawing thread, a sample a millisecond (another thread reads where
  they are), their busiest functions by name written to the log on the way
  out (the PDB beside the program, through dbghelp); the code of a DLL with
  its caller in the program (the first return into it found on the stack);
  the SH-4s' busiest translated blocks, with their instructions' words. Code the PDB
  does not name shows as the symbol before it, with its distance when far
  (vcruntime's memcpy as `_NLG_Return2`). `HR_SAMPLE_AT=1` adds each
  address's offset, `HR_SAMPLE_TOP=n` shows n functions (48), and
  `HR_SAMPLE_DUMP=<dir>` writes the 40 busiest translated blocks' x86-64
  code there (`block00.bin`...) with their samples by offset
  (`block00.txt`, its first line the block's words), for a disassembler.
- `HR_EXIT_DUMPS=1` dumps all the machine's memories on the way out
  (`<game>-ram-m.bin` and the others, some 150 MB), in the working directory
  or the write directory's `<game>/`.
- `HR_NO_DRAW=1` sends no mesh to the GPU; `HR_STATS=1` prints each frame's
  draws and triangles; `HR_SYNC=1` waits for each mesh and prints the slow
  ones; `HR_LOG=1` logs the renderer's states (on stdout: not with
  `-agent`). `HR_DEBUG_KEYS=1` lets keys step the renderer's debug switches
  (`debug_controls` in `hikaru-renderer.cpp`: 1, 2, 3, V, T, U, P, I, J, L,
  A, D, S, G; off by default, a player's keys). `HR_DEBUG_FLAGS=i=v,i=v`
  sets them without a keyboard, for an agent's run: 1 and 2 hide the 2D
  layers, 3 the 3D, 7=N draws only polytype N (6 the translucent meshes). `HR_GL_DEBUG=1` asks for a
  debug OpenGL context, whose driver then logs its errors as they happen
  ("GL: ..."): without it, Intel's says nothing (about 10% slower).
  `HR_GL_CHECK=1` (and `HR_GL_DEBUG=1`) looks for GL's errors after each
  call, to find where one comes from; without it, once a frame.
- `HR_LOG_INPUTS=1` logs the switches the game reads at each change (the
  simulated I/O board's): start 8000, service 4000, up 2000, down 1000,
  left 0800, right 0400, buttons 1 to 4 from 0200 down.
- `HR_LOG_SERVO=1` logs the NAOMI servo board's commands (838-14174) as
  their parameters change: the command, its two bytes, the board's
  milliseconds, whether the motor is on and the start's check running.
- `HR_NO_AUDIO=1` opens no sound output; `HR_WAV=<path>` writes all the
  sound to a WAV file as well.
- `HR_CLOCK=<count>`: the board's clock starts at that count (seconds since
  1964-01-01 00:00, decimal or 0x hex) whatever the PC's time. In agent
  mode it starts at 0x5BFC8900 (26 Nov 2012 00:00) without it, so that runs
  repeat.
- The NAOMI 2's Elan lights its vertices late (`src/mach/naomi/naomi-elan.cpp`,
  "The Lighting"): each kept with the lights of its time, its triangles sent
  with their colours to come, then all lit on several threads before the
  Holly draws the scene or starts another (`Elan::flush_lighting`, from
  `Holly::start_render` and `PVR_LIST_INIT`). Each vertex alone, the same
  colours whatever the threads: Club Kart 2003, Club Kart Prize, Virtua
  Fighter 4 and Initial D3 pixel-identical to 0.8.12, their memories too
  (`tools/regress.py`). `HR_PROFILE`'s Elan counts the flush.
  With `ELAN_LIGHTING = GPU` the flush leaves them to the renderer instead,
  with the scene (`Scene::lighting`: the light sets, the vertices to light,
  the clipped points, the scene's vertices to colour; the vectors swapped,
  not copied): `BoardRenderer::light_scene` lights them with two compute
  shaders (`light_cs_src`, the same operations as `Elan::light`;
  `color_cs_src`, the clipped points and `pack`'s bytes written into the
  vertex buffer) before the scene is drawn. The GPU's square roots,
  divisions and powers are not the processor's to the last bit: a colour
  may come a step (1/255) off, a picture read back into the VRAM with it.
  Without a compute shader (OpenGL below 4.3) the processor lights them.
- A NAOMI frame without a scene for the screen (`BoardRenderer::take`,
  `draw_frame_now`) shows the last picture drawn for 3 frames, then the
  framebuffer the display reads in the VRAM (`read_vram`: what the
  processor drew there, the BIOS's screens); but on and on the picture
  drawn while the display reads a framebuffer one of the two last scenes
  for the screen was drawn to, its VRAM unchanged since
  (`Holly::shows_rendered`: a hash of a word in four of a line in two at
  the STARTRENDER and then): the board's drawings are not written in our
  VRAM. King of Route 66's loading screen, drawn every 14 frames, flickered
  black.
- `HR_DISPLAY=<n>`: the window opens on SDL's display n (the log lists them
  at the start, `INDEX:n`), for runs kept off the monitor one works on; the
  window centred again stays on its monitor.
- `HR_HASH=<file>`: each memory of the machine hashed at the end of each
  frame, a line a frame: `F <frame> <name>=<hash>...` (`Machine::write_hashes`:
  its buffers, named as the Lua shell names them, a NAOMI board's with its
  number after the master's, `bufN` the devices' registers, then
  `hashed_extra ()`'s: the NAOMI link's memory). `HR_HASH_BLOCKS=<frame>[,...]`
  adds those frames' 64 KB blocks, `B <frame> <name> <offset> <hash>`: where
  two runs differ. A frame some 25 ms slower on a three-board NAOMI; for
  `tools/regress.py` (below, "Regressions").
- `MIE_SPEED` and `MIE_IDLE_SKIP=0`, see below.
- `FFB_GAIN`, `FFB_INVERT`: the force feedback (see README.md);
- `FFB_SEND_MS`: the pause after each write to the wheel (16), on the force
  feedback's thread; 0 lets the wheel's USB slow the frames down again;
  `DRIVEBOARD_SIMULATED=1`: the drive board's program simulated even with
  the srally2 set (NASCAR Arcade) or the scud set (Jambo! Safari).
- `COMM_BOARD = PROGRAM`'s ring (hikaru-commbd.cpp): `HR_COMM_LATENCY`, the
  fibre's latency in the board's clocks (2450, 8 lines); `HR_COMM_SLICE`,
  the lines between two times sent (2); `HR_COMM_SPIN`, the microseconds a
  wait looks for the next time before sleeping (300); `HR_COMM_SEND_SPIN`,
  the same for the link's sender thread (300). Each cabinet logs
  what went round every 10 s ("frames out, in, held, lost, late", its
  waits).

The unit tests run with `ctest -C RelWithDebInfo` in `build` (the SH-4's
MMU, its FPU's moves and rounding and its performance counters, the Z80, the TLCS-90, the 68000, the ARM7, the AICA's arithmetic, the sound output, the EEPROM,
the X76F100, the nvram file, the write directory, the games folders and their 7z, the
controls' hotkeys and wheel rotation on SDL's virtual joysticks, the drive
board, the card reader, the aspect ratio's layouts, the CRT presets' reader
and the shipped presets, the bezels' layouts, the translations).
`driveboardz80test` is not among them: it needs EPR-20512 (`epr-20512.bin`),
a script and a journal of Supermodel's, and is run by hand. With another
program and an empty journal, `DRIVEBOARD_LIST=1` lists what it writes,
NMI by NMI (EPR-19338A's values in `driveboard-jambo.cpp` were measured so);
`--boot` counts the cycles from the reset to the CTC's start. EPR-19338A,
Jambo! Safari's, is checked the same way as EPR-20512 against a journal of
Supermodel's on a scenario of all its commands: the same outputs, at most
an NMI apart (the results in the test's header).


Regressions
-----------

A game run twice from its boot, in agent mode with a fresh write directory,
gives the same memories at each frame (the board's clock fixed, the boards'
link exchanged at fixed points, a ring of one without a socket, the memories
zeroed). `tools/regress.py` compares two builds on that, frame by frame:

    python tools/regress.py --a old/ --b new/ --roms C:/roms --all --frames 1800 --jobs 8

each set run once with each program (`HR_HASH`), the first frame where they
differ and which memories: for a change that must not move a game. Without
`--b`, the same program twice: whether each game's runs repeat.
`--all` takes the sets of `data/games.json` whose zip, or their parent's, is
in `--roms` (several folders separated by `;`, as `-R` takes them: the zips
in one, the CHDs in another; one not starting, a file missing, is left out); `--ini` gives
the runs' `hikaruloader.ini`; the runs stay in `--work`. Both programs
from 0.8.10 on (`HR_HASH`). On 2026-11-20 the 273 sets whose files are on
the development PC (all but `prem11` and `mirstad`) started and repeated over
1800 frames (about 2 hours, 8 runs at once).
A difference is then narrowed with `HR_HASH_BLOCKS` at its frame, the
blocks' bytes read with the agent's `ram` and the writer found with the Lua
shell's `cpu:wpset` and `"write"` hook (above, "Lua shell").


Controls: the MIE and the JVS bus
---------------------------------

The games read the cabinet through the MIE (315-6146), whose Z80 runs a
program the game puts in its RAM; that program drives the JVS bus, where the
I/O boards hang in a chain. Hikaruloader runs that Z80 (`src/cpu/z80`, its
tests: `ctest -C RelWithDebInfo` in `build`), and emulates the bus and a Sega
837-13551 I/O board on it (`src/hl/jvs.cpp`): Test (F2), Service (F1, 9), the
coins (5, 6), Start (Enter), the arrows and four buttons, to bind in the
settings (Tab). The analog channels (wheel, pedals) come from controls.ini's
Driving section; from the shell too:
`machine.devices.io_board:write_u16 (0, 0x9000)` (the wheel, channel 0;
channel n at 2n), checked in the game's JVS TEST screen.

- The MIE's GPIO: the main board's Test and Service switches, and its 93C46
  EEPROM (`src/hl/eeprom.cpp`), where the BIOS keeps the system settings and
  the games theirs. The SH-4 sees the MIE's ports at 00800000 (port n at
  00800000 + 2n), as in MAME.
- The EEPROM is kept with the main board's backup RAM (64 KB, a battery's:
  the games' records and bookkeeping) in `nvram/<game>.nvram`
  (`src/hl/nvram.cpp`): a header (`HLNV`, the version, 1), a directory of
  the blocks (type, offset, size), then the blocks, `EEPR` (128 bytes, in the
  order of the addresses) and `BRAM`. The EEPROM is saved half a second after
  the game's last write, the backup RAM every ten seconds when it changed,
  both on the way out. A `nvram/<game>.eeprom` of the earlier versions is
  read when the game has no `.nvram` yet. The NAOMI's the same, its EEPROMs
  and its boards' settings two seconds after they change (its backup RAM
  went with those saves alone: a record taken in the middle of a game, and
  Club Kart Prize's POWER DOWN IN GAME at the next boot). Its two X76F100s
  (the main board's serial number, `EEPR`; the cartridge's, `X76F`) keep
  the BIOS's read password, `iyhktmhm`, whatever a dump or a backup holds,
  an erased serial number the dump's (`Board::nvram_read`): a zip's
  `x76f100_eeprom.bin` of other passwords, taken by its name (the log then
  gives its CRC against the one the game lists), stopped the BIOS at ERROR
  03 BOARD MALFUNCTIONING on the NAOMI 2 screen (Initial D Ver. 3 Export,
  0.8.6).
- No memory left (another program took it): a NAOMI's scenes are built
  again in the vectors of those no drawing holds any more (`Holly::
  new_scene`: no allocation a frame; they grew from nothing at each list);
  what the TA, a drawing or the SH-4's translator cannot get is dropped and
  told now and then (a parameter, a frame, a block interpreted), not the
  emulator stopped. The crash log gives a C++ exception's type (MSVC's
  throw information) and the stack, each frame's module and offset (a
  `/MAP` of the link, `_LINK_=/MAP:path`, names them).
- A NAOMI game without its `.nvram` starts from `data/naomi/<set>.sram` (the
  backup RAM) and `data/naomi/<set>.eeprom` (the MIE's EEPROM, 128 bytes)
  when they are there: a cabinet set up as the game needs it. F355 Twin's
  backup is not linked to another cabinet (from Flycast's resources), Club
  Kart Prize's EEPROM is a set-up cabinet's (Flycast's); Club Kart's four
  sets' (`clubkrt`, `clubk2k3`, `clubkprz`, `clubkpzb`) have their pedals'
  range 00-FF in the game's record (bytes 55 to 58: the accelerator's least
  and most, the brake's; the factory's 30 C0 30 C0; again at 87): made by
  clubkrt's VOLUME SETTING in agent mode, the others' the same bytes and
  their CRCs worked out (`eeprom_crc` over the record, at 36 and 40: the
  same file as the game's for clubkrt), each kept by its game at a new
  start; The King of Route 66's
  (`kingrt66`, `kingrt66p`: their factory EEPROMs are the same) has its
  VOLUME SETTING made, the game's record at 44 (and its copy at 80) holding
  the centre, the wheel's left and right ends, the accelerator's and the
  brake's least and most, 8000 0000 FF00 0000 FF00 0000 FF00 (the factory's
  are all 8000: 0/0 for a pedal at 8000). Made in agent mode: the game's
  test menu, VOLUME SETTING, the wheel and the pedals pushed to their ends,
  EXIT; the game writes the record and its CRCs, and the nvram's `MIEE`
  block is the file. The BIOS's part (0 to 35) is a new cabinet's, rewritten
  from the settings at each start.
- Without those files the backup RAM starts zeroed, as every emulated memory
  (`hl::Buffer::create`): those under 64 KB came from the heap, the NAOMI's
  SRAM of 32 KB among them, and held what it left there until 0.8.8; the
  BIOS reads its blocks' sums at 00200008 and 00200100 before writing
  them, and two runs of a game from boot went two ways from its 51st frame
  (Crazy Taxi, Sega Strike Fighter), about one in two.
- The boards of a cabinet (`src/mach/naomi/naomi-multiboard.cpp`) each read
  and write their own view of the link's memory, the writes exchanged when
  all stop (every 8 lines, in the boards' order); shared live, the slaves
  went two ways from frame 678 of Sega Strike Fighter's attract in 7 runs
  out of 8. Eight runs at once from boot now give the same memories (RAM,
  VRAM, sound RAM of each board, the link's) for 1800 frames. Its side
  boards' frame behind the master (`Renderer::late_middle`) is the same with
  either: not the threads'.
- A node of the ring that is its own next (`hl::RingLink`, `PORT_OUT` =
  `PORT_IN` on 127.0.0.1, a cabinet alone by default) opens no socket: what
  it sends is put back for its own `receive ()` at once (Gaelco's link at
  the next line). Through TCP, ATV Track's link words came back when the
  PC's threads got to them, and one run in six went another way from its
  frame 401 (a word more in its link's queue, 0C0A1D80); several programs
  on the PC with the default ports joined one another's ring. The Hikaru's
  communication board did so already (`CommBoard::loop`).
- The controls (`hl::GameControls`, `src/hl/gamecontrols.cpp`): every board
  (the JVS I/O boards of the Hikaru and the NAOMI, the Atomiswave's pads,
  Gaelco's ports and ADC, the MIEs' Test and Service, the card readers) asks
  it for the switches and channels of its ports; each is a role, a row of
  `controls.ini` (`Controls::row_def`, the table in `src/hl/controls.cpp`),
  placed by `data/games.json`: `layouts` (a board's usual places: `jvs`,
  `jvs_p34`, `atomiswave`, `atomiswave_p34`, `atomiswave_wheel`,
  `gaelco_football`, `gaelco_driving`), a game's `layout` (by default `jvs`,
  `atomiswave` on the Atomiswave; a list of them, each over the one before:
  `["jvs", "jvs_p34"]`) and `controls` over it, `"<role>" : "<port>:<n>"`
  or `{"at" : ..., "label" : "<its INPUT TEST's name>", "invert" : true,
  "range" : ["C000", "4000"]}` (null: not there); a place the game lists is
  taken from the layout's role. `"combined" : {"thrust" : "Accelerator",
  "turn" : "Steering", "side" : "left"}` makes a place one of a pair of
  thrust levers that `TWIN_LEVERS = COMBINED` drives from those two roles
  (Star Wars: Racer Arcade's).
  `"uses" : [roles]`: the layouts' roles the game has (its INPUT TEST's, from
  the survey), the others' places dropped, so that the Controls tab and the
  agent's `inputs` give exactly the game's controls; none: all of them.
  Ports: `sys`, `p1`, `p2` (a player's 16 switches, bit 15 START ... 4
  BUTTON6), `coin` (1, 2), `an` (channels); a cabinet's second 837-13551
  (the far end of its chain) `p3`, `p4`, `coin` 3 and 4, `sys2`, `an2`
  (`SegaIoBoard::unit`); the Atomiswave's `p1` to `p4` and `a1` to `a4`, a
  pad answering on each port the game's places use (two at least), coins 3
  and 4 the EX. IO board's; on ports 3 and 4 the EX. IO board's light guns
  `gun3`, `gun4` (0 X, 1 Y, 2 trigger, 3 off the picture: their aim latched
  to SPG_TRIGGER_POS by the Holly's Light-Gun mode, `HR_LOG_GUN=1` logs it)
  and mice `mouse3`, `mouse4` (0 X, 1 Y, 2 to 4 the buttons); a Namco JVS
  board's screen positions `pos1`, `pos2` (0 X, 1 Y, 2 off the picture: the
  `jvs_guns` layout; a board's channel n is `pos<first player + n>`, Mazan's
  second sensor board `pos2`); Gaelco's port selects (`f000`, `e800`,
  `7000`...).
  `gearbox`: a lever held in its positions, `"R"`, `"N"`, `"1"`... each the
  switches it closes (`hl::Gearbox`); `fixed`: switches always closed. A
  clone reads its parent's. `HR_LOG_INPUTS=1` logs the players' switches and
  the channels a JVS game reads, at each change. The table was made from the
  games' INPUT TESTs and MAME's ports; Lua's `emu.input (role, value)` holds
  a role as the agent does.
- The devices' profiles (`hl::DeviceProfiles`, `src/hl/deviceprofiles.cpp`):
  `data/devices/*.ini` beside the program, then `devices/*.ini` beside
  `controls.ini` (the user's, over them), each `[Device]` `Name` and
  `VidPid` and controls.ini's rows by the device's inputs (`JOY_`, `GC_`).
  `Controls::open_devices` finds each player's (`match_profiles`): a device
  new on its player (its GUID not `P<n>_Profiled`) gets its profile on that
  player's bindings (`put_device_defaults`), one without where one with was
  gets the defaults back. `Controls::device_default` is the Controls tab's
  Default and Reset, `save_profile` its *Save as a profile*. Checked in
  `tests/controls_test.cpp` (an SDL virtual wheel with its own USB ids), and
  with vJoy fed by `vJoyInterface.dll` in a normal run (`HR_LOG_INPUTS`).
- `games/<set>.ini` beside `hikaruloader.ini`: a game's own settings, those
  differing from the common ones (`Config::begin_game`, `save_for_game`);
  `Config::common_only` lists those kept common.
- NAOMI: `io_boards` in `data/games.json` lists a cabinet's JVS chain, nearest
  the main board first (the far end takes the first address), with the 837-13938
  encoder board (`hl::SegaIoBoard13938`); another 837-13551 has players 3 and
  4, as the four-player games read them (their JVS TEST's NODE 1);
  `io_players` the players of the first board (4 on Power Stone 2's);
  Namco's boards (`hl::NamcoIoBoard`, as Flycast has them): `namco-jyu`
  (Ninja Assault: two players, two guns), `namco-fcb` (Mazan: two, one per
  sensor of the sword, the first node 1), `namco-fca` (Gun Survivor 2: the
  gun's yaw, pitch and roll on `an` 0 to 2), `namco-v226` (World Kicks: an
  837-13551's identity, one player whose switches are the four players'
  buttons, twelve channels: the sticks on `an` 0 to 7, the kick sensors on 8
  to 11, as the SWITCH TEST reads them; its FRONT/BACK sensors, p1:7 to
  p1:0 active low, left closed). Their screen position (JVS
  command 25) in the counts the games' factory settings take (the shots land
  where they aim before any GUN INITIALIZE or SWORD ADJUSTMENT: measured in
  the tests, `HR_LOG_GUN=1` logs the positions read); command 70, Namco's
  board start (Mazan's sensors: `JvsDevice::maker_command`), as Flycast
  answers it, the FCB's channels 0 until it ends;
- A cabinet's other hardware, in `data/games.json` too (a clone's is its
  parent's, `hl::Game`): `io_boards` `["837-13844"]` the racing cabinets' I/O
  board (Hikaru and NAOMI), `drive_board` the wheel's drive board on it
  (`srally2`: NASCAR Arcade's, a Model 3's; `f355`, `18wheeler`; `jambo`,
  Jambo! Safari's deluxe one, a Model 3's: Scud Race's EPR-19338A from the
  scud set on its Z80, `driveboard-jambo.cpp` simulated otherwise;
  `tokyobus`, Tokyo Bus Guide's motor board, its EPROM not known: it speaks
  F355's board's commands, F0-F7 the answers (F1 10 while stopped, F0 the
  echo of the test), FE the start, 4x the spring (47 in the games
  played), D7 CF its centre, 8x-Bx the short vibrations at a
  contact (bit 5 the other way), E0-EF the test's torques: EPR-21867's
  program, `driveboard-f355.cpp`, plays them),
  `servo_board` `838-14174` the wheel's servo board on the MIDI port (Club
  Kart, The King of Route 66, Initial D, Driving Simulator), `card_reader`
  (`von4` on the Hikaru's MIE, `initiald` and `clubkart` on the NAOMI's
  SCIF), `hopper` `swp` (Club Kart Prize's medals), `cabinet` a Gaelco
  cabinet's outputs (`atv`, `smashdrv`, `football`). The log names each one
  at the start; a value not known is an error there. `g2_board` the board on the G2 bus's
  outside (`naomi-g2board.cpp`, the games' 837-14381:
  `machine.devices.g2_board:option ('log', 1)` logs its accesses).
- A NAOMI or Atomiswave set's widescreen codes, `widescreen` in
  `data/games.json` (each set its own, not inherited): `[{"at" : "0C5C64A8",
  "value" : "43700000", "from" : "43A00000"}, ...]`, 32-bit words of the
  main RAM. With WIDESCREEN, each word is set to `value` at each frame's end
  while it holds `from` (what the game puts there: another program, a
  clone or a slave board's leaves it alone) and put back without; while
  they all hold theirs, the game draws its view 16:9 squeezed into its 4:3
  picture, and the renderer stretches the whole picture to the width
  (`Board::anamorphic`, `Draw::stretch_all`); the log says when a board
  draws 16:9 or 4:3. Most are Flycast's widescreen cheats (core/cheats.cpp,
  by flyinghead and the authors it credits), each `from` read in the game
  here (lupinsho's word found at its own address, by the code that reads
  lupinshoo's); Slash Out's (the GD-ROM's word holds something else),
  Spikers Battle's (a word the game changes) and Asian Dynamite's (its
  words hold their value in some scenes only) left out. Crazy Taxi's, Club
  Kart's and The King of Route 66's found here, by writing each word of
  the RAM holding a value of the view's (320, 640, 4/3...) in a race and
  measuring the picture's change: Crazy Taxi's view's half width (320 ->
  240), Route 66's perspective's aspect (a literal of its code, 4/3 ->
  16/9), Club Kart's projection's pointer to the view's width moved to its
  height (the scale x 0.75; the centre is the width's half too) with the
  view's x offset at 80 (the centre kept), in each of its sets at the
  same code. F355 Challenge works its view's scale out in its registers
  from the camera's angle at each frame (no word to change); Tokyo Bus
  Guide's not found (its 320, 640, 4/3, 0.75 and 1.0 words change
  nothing, written from the boot too). The game computes its view at a
  race's or a scene's start: switched during one, the right picture comes
  at the next.
- A deluxe cabinet's views, `screen_view` in `data/games.json` (a clone's its
  parent's): `{"focal" : 864.7, "yaw" : 45, "share" : ["0C154CEC"]}`, the
  focal length of its 3D in pixels of the 640x480 screen and the turn of the
  right screen's view from the middle one's in degrees (the left one's the
  other way), in the game's race, and the words of the main RAM the master
  gives the other boards while their screens are joined (below: Airline
  Pilots' wings); they mark the games `MULTI_SCREEN_VIEW` serves (`Renderer::views`,
  `naomi-renderer.cpp`). Each scene's own are solved at each frame
  (`Renderer::solve_views`) on the vertices the middle board and a side one
  both send: the same vertex of the same model, found by its texture
  coordinates and colour (a key each board's list has once; untextured, 2D
  and far-plane vertices left out), is the same point of the world in each
  view (x, y on the screen, 1/W the inverse of its depth along that view's
  axis). Its depths give the side view's turn t and the focal length f,
  `z_side / z_middle = cos t - sin t (x_side - 320) / f`, by RANSAC (two
  pairs a guess, the one most of 128 pairs agree with, to a pixel and 1% of
  their depth, fitted again on all those that agree); its rows the centre
  row cy, `(y_side - cy) z_middle = (y_middle - cy) z_side`, their median.
  The values: F355 Challenge's race 87.14 degrees and 376.7, its attract's
  driver close 35.56 and 1082.3, the factory 81.19 and 415.7, the rankings
  54 and 686.3, a zoom followed frame by frame (2.4 degrees and 16500 to 7
  and 5500), a scene at 28.57 and 1361.3, the starting grid with a long
  lens 1.43 and 27580 (the side screens the middle one's picture 690 pixels
  on: a monitor's 640 and its frame's 50; a focal length taken up to a
  million); Airline Pilots 45 and 864.7 (its
  side boards' matrix at 0C0F1304, sin -0.7071 and 0.7071), its cockpit
  view projecting with its centre at row 156 (the horizon raised over the
  dashboard, near and far points alike), its chase view at 240; Sega Strike
  Fighter 45 (its own at 0C26E608) and 864.7 in its cockpit (below). A side
  is solved with 8 pairs or more, five on each other at least, two in five
  of those that tell and half their rows too (what moves between the
  boards' frames, a key two models share; F355's race seen from the car,
  its left board down to 52%); both
  sides with the same turn either way and the same focal length to a
  hundredth of a degree and 0.5%, or one, nine in ten of its pairs on each
  other, while the other has fewer than 8 (a cut: F355's race, its left
  board 7). The views come on once solved two frames in a row, stay through
  10 frames of too few points, and as long as both side screens draw no 3D
  (fewer than 4 triangles of a pixel or more on the screen: Sega Strike
  Fighter's jet over the middle screen alone, its left board's 24 others
  folded into a corner; nothing there they would turn, nor against them),
  go off at once when points enough do not
  agree (a cut to a scene of other cameras; the boards' frames apart while
  the camera turns, their rows a few pixels apart; Airline Pilots' plane at
  the gate before its side boards were solved a frame late: 55% and turns
  10.9 and 9.6 degrees on the master's last frame, to 0.001 pixel on the
  one before). Steady, solved every other frame (Airline
  Pilots' 40000 vertices a board, some 1.5 ms: reading them); F355's 0.3 ms.
  Sega Strike Fighter's side boards draw a frame behind the master in most
  of its scenes (its attract's, its training's, its missions' at times),
  Airline Pilots' in its attract's (its plane at the gate, close by its
  engine, its fuselage at 10.1 degrees and 4067, its city at 13.5 and 3045,
  its take-off down to 2.7 and 15300, its logo at 45 and 879):
  their vertices on the master's previous frame's to 0.01 pixel, some 50
  pixels from its last's while the camera turns. The game's own exchange,
  not the emulation's timing (Sega Strike Fighter's attract, measured
  2026-11-18): at line 17 of a frame the master writes its 122 words for
  the slaves and switches the bank, they see it at line 23, read 63 words,
  start their lists there and draw at line 520 as the master does, and every
  board shows the picture drawn the frame before (two buffers each, the
  same latency): those words hold the master's previous frame's views. No
  phase of the boards changes what the words hold. Each frame is solved on the
  middle board's last scene and on the one before (`Renderer::held_middle`),
  the one that went with the side boards' first; with the one before, the
  middle board's picture is shown a frame late (`Renderer::late_middle`:
  its scene for the screen drawn at the next frame, its last kept), the
  three screens the same moment. Its scenes' views: 45 and 864.7 (its
  cockpit, centre row 156 there; most of its attract), 38.13 and 1054.9,
  32.14 and 1294.9, 30.61 and 1373.8, 26.88 and 1608.7, 18.44 and 2594.5,
  15.63 and 3249. Keys repeated at the same place count once (a vertex of
  several triangles: its title's sky, none of its keys once in a list).
  A side with fewer than 8 pairs is paired by a vote (`vote_pairs`: its
  grid of tiles, each tile's key the same; its logo, the texture
  coordinates each board's own, reflections): each of its vertices and each
  of the middle board's with the same texture coordinates and colour (or
  colour alone) make a candidate, `f^2 = (dxs^2 - r^2 dxm^2) / (r^2 - 1)`,
  `r = z_side / z_middle`, `t = atan (dxm / f) - atan (dxs / f)` (a point's
  depths in two views of one eye are as the cosines of its angles from
  their axes; dx from the centre column), voted in 300 x 360 bins; the
  peak's median f and t, the centre row its pairs' most common one (its
  rings: points of a colour at the same x and depth above and below each
  other), the pairs on each other in x, y and depth then (the grid 3700 of
  6700 candidates). Not with pairs enough: Airline Pilots' plane at the
  gate has a peak at 10 degrees from its far points. The pairs that tell
  nothing are not counted (SideView::elsewhere): those brought out of the
  middle board's screen 200 pixels or a tenth of the focal length from
  where the middle board has their key (another copy of the model: its
  bomber, a second one by the left board's top edge 1500 pixels to the left
  of the middle view; its fleet's ships; its ground's tiles; a model in an
  inset's own view; a camera a frame apart moves them some 50 pixels at a
  focal length of 4000), and those of the middle board's a screen or more
  off its screen (by the plane of the eye, 80000 pixels away). RANSAC's
  guesses count the pairs on each other in their rows too (the centre row
  from the guess's pair of depths farther apart): in x and depth alone some
  by chance. A side nine in ten on each other also does with the other's
  pairs on each other with its turn the other way (a tenth of them, four at
  least, half when under 8 tell), or as the views on have them; both sides
  near also (SideView::near: 16 pairs at least on each other in their rows
  too, the nearest four times nearer than the others' middle depth; an eye
  apart would put the far points on each other, not the near ones): its
  training's cockpit on each other while it shows how to fire, the world
  held a frame apart by each board (12% of the left board's points). The
  views on, the master's camera shaken or turned alone (its crash: its side
  boards' views 90 degrees from each other about their vertical to 0.02
  pixel, the middle one tilted from their middle, 5 to 10 degrees over 4
  seconds; its left board's points all on the middle one's with a whole
  rotation, none with a turn): the rig kept (turn, focal length, centre
  row) and the middle view's rotation S from it solved on both sides' pairs
  at once, `x_middle = S Ry (+-yaw) x_side` (`solve_shake`: Gauss-Newton
  from none, on the pairs within a distance halved at each step from 400
  pixels, from both sides, the left alone and the right alone, the one
  whose side less on each other has the most); taken from 0.2 to 25
  degrees, each side 16 pairs at least on each other, three in four on
  each other shaken or not (the crash's points high over the right board's
  screen, the master drawing them unshaken; Airline Pilots' plane at the
  gate: a fifth with no shake, not taken). The side boards' 3D then turned
  by `Ry (-+2 joined) S Ry (+-yaw)` (View::rot, a matrix in the vertex
  shaders, u_view_m; a turn about the vertical otherwise), solved at each
  frame. The views off when points enough do not agree after 4 frames more
  (Airline Pilots' plane at the gate, its side boards a frame late but for
  a frame or two now and then; Strike Fighter's cuts between scenes of the
  same views, a few frames of them apart). Left in the cabinet's views: the scenes in 2D, its 3D texts (each board's
  letters facing its own camera, none drawn by two boards), its mission's
  briefing room (the middle board's in an inset with a projection of its
  own, a panel over its screen). The fixed values with
  the centre alone measured before missed the attracts' scenes, and put
  Airline Pilots' cockpit centre at 153; the pictures' overlaps before that
  gave values sliding along a ridge. The vertex shaders' 2D, left where it
  is, is a triangle whose 1/W is the same at its three vertices and 1 or
  more (the HUD), or the far plane's. With WIDESCREEN the side views would
  turn outwards, past what the games draw (the cockpit's sill there
  missing): the joined screens' field is narrowed to (outer screen's turn +
  a board's half field) / (2 x outer + 1), the 3D drawn with that focal
  length (View::out_focal, u_view_f), some 12% larger.
  In 4:3 the cabinet's gap between its monitors, between the middle board's
  640 and a side one's (Airline Pilots' 20.3 to 24.7 degrees), has what each
  board sends beyond its edge, not what it leaves out as out of its view:
  each board leaves out what lies wholly beyond its own view (Airline
  Pilots' overhead compass, the middle board's alone; at its take-off 512
  vertices of far hangars the left board's alone, 23 of a nearer building
  the middle board's alone; a house by F355's factory, the left board's
  alone). The middle board's scene is drawn too in each side screen's view
  (View::strips, strip_turn: turned by 2 x the joined half field, as the
  side boards' are turned; Picture::strip, its depth, the same pixels as the
  side screen's: nothing resampled), its HUD left out (in_flat 5: 2D at a
  1/W of 1 or more, narrower than the screen), its 2D as wide as the screen
  kept (the side screen's own there, each board drawing it alike: F355's
  race brightened by a white layer at 30%, 4DFFFFFF, over the whole
  screen; left out of a middle picture drawn wider, its strips came out
  darker), its backgrounds too. Over the side screen, from its inner edge
  to the side board's own view's edge (`fill_strips`, strip_fs_src), the
  two pictures are joined as one depth buffer, no fade: the nearer of the
  two by 3% (both depths along the side screen's axis); where both have
  the same surface the middle's up to the cut, the direction halfway between
  the two boards' own views (22.5 degrees in Airline Pilots: each board's
  picture beyond its own view by 2.2 degrees at most), the side's beyond.
  Solved on the vertices both boards send, the views put them on each
  other: Airline Pilots' take-off and climb, 1944 to 3471 pairs a side, a
  median of 0.05 to 0.1 pixel apart, 90% under 0.5 (the solve's own focal
  length, turn and centre again: 864.7, 44.998, 240.0); the columns' steps
  at the cut those of the columns around (Airline Pilots 1, F355 9; the
  wider middle picture faded in had a step of 118 to 168 in F355's race,
  its white layer missing). The fog of a turned view by the game's depth
  (fs_src game_z (), v_w: W across the triangle, linear in space): by the
  turned view's depth, along the screen's axis, the cut's point 2.9%
  deeper than along the side board's own, the side board fogged Airline
  Pilots' sky 2 to 4 levels brighter than the middle one there. Where the
  side board alone draws something, its pixels too (`side_only`: its
  textured 3D triangles none of whose vertices the middle board has, a
  vertex of the same texture coordinates where the side board's falls in
  the middle view, within 20 pixels and 10% of its depth; F355's car's
  wheel by its factory, 30% of the left board's vertices there not in the
  middle board's list, its wheel arch the middle board's, 2% deeper: no
  depth tells them apart; texture coordinates alone, a vertex's colour lit
  for each board's view; the middle board's vertices chained by texture
  coordinates and by cell of 32 pixels of its screen, the cells around
  looked at, 64 vertices each at most: a city's tiled textures by the
  thousand); where
  the side board's 2D is, its pixels (a side screen's own text: F355's
  trademark notice over its race's start, 2D at 1/W 100, translucent, no
  depth), not a triangle over a quarter of the screen (a fade's quad over
  the whole screen). Both masks in whole cells of 4 pixels and those around
  (View::mask_2d, worked out with the views on the machine's thread), over
  the strip's columns alone, from the side screen's inner edge to its
  board's own view's, 16 pixels more (a side screen's 2D rasterized all
  over took 2.5 ms a frame in Sega Strike Fighter's missions; the views
  some 2 ms a frame there now). What both boards leave out (an object
  wholly in the gap) none draws. A batch whose user clip is a tile or more
  narrower than the screen is an inset with a view of its own (Sega Strike
  Fighter's side screens' small views of the plane in its missions; its
  whole view clipped to the screen's width, its demonstrations' to a band
  of it): its 3D left where it is, as the 2D, out of the strips as the
  HUD, in the side screen's mask by its clip's rectangle (`inset`).
  Airline Pilots' side boards draw other wings than the master's: the plane
  has 20 of them, from the most bent upwards to the most bent downwards
  (its draw table's entries 1942 to 1961), the one drawn the plane object's
  +E88, set at each frame from word 7 of the record at 0C154CD0
  (0C154CEC). The master's flight writes that word (10 in level flight and
  on the ground); the side boards take the record from the packet the
  master sends them at each frame (0C05ECA0 copies it from 0DD5B698) but
  not that word, which the packet does not have: left at 0, they draw the
  first wing, bent upwards (in the climbing chase view their vertices 0.8
  pixels apart from the middle board's at the root, 6 to 12 in the strip,
  30 at the tips; the fuselage, the tailplane, the ground and the far
  clouds within 0.05; each board's matrix inverted on the vertices it
  sends: the model's own vertices apart, y 0.03306 against 0.01945). The
  game's own, the cabinet's view keeps it. Joined, the master gives the
  other boards that word at each frame, after it, all boards stopped
  (`Naomi::share_view_words`, `screen_view`'s "share"): they draw its wing
  from the next frame on, the wings and their letters meet.
- SGNASCAR has the I/O board of the racing cabinets, an 837-13844, and on it
  the drive board of a Model 3 (Sega Rally 2's, EPR-20512): its program on an
  emulated Z80 when the srally2 set has it, simulated otherwise
  (`src/hl/driveboard.cpp`): its command on the outputs 15 to 22, its reply
  on the players' second switch bytes. The start-up (CF, CE, CD, CC: 11 s),
  then the game's torques, become force feedback on the first SDL joystick
  that has it (DirectInput on Windows): a constant force, the servo board's
  spring and friction. `machine.devices.drive_board:info ()`, and
  `:option ('log', 1)` for the commands taken.
- `machine.devices.jvs:option ('log', 1)` logs every packet and its answer;
  `machine.devices.mie:option ('log_ports', 1)` the port accesses, the Z80's
  and the SH-4's; `machine.devices.mie:info ()` its PC and interrupts.
- `IO_BOARD_PROGRAM` (off by default): the I/O board's own CPU runs its
  program at the far end of the MIE's UART (`src/hl/ioboard.*`, an
  hl::JvsLine the JvsBus keeps): the 837-13551's TMP90PH44
  (`src/hl/tmp90ph44.*` around the TLCS-90 of `src/cpu/tlcs90`, its tests
  `tlcs90test`) running 315-6215 (the jvs13551 set), NASCAR's 837-13741 (a
  315-6146: a Z80, its UART, GPIO, converter, and a D71054) running
  epr-21868 (the 18wheelr set). Each the section `io_rom` of the game in
  `data/games.json`, optional. The line counts ticks of 1/9.216 GHz, in
  which the MIE's Z80 (576) and the UARTs' crystal (625) are whole: each
  byte the MIE writes starts once the one before has gone and reaches the
  board a character later; the board runs 64 us ahead of the MIE, which
  stops at each byte or sense change the board has for it. The simulated
  board stays, for the inputs and outputs (`SegaIoBoard::player_bits`,
  `drive_outputs`). The line's queues and the board are saved after the
  bus's own state, with the option only; `jvs:info ()` tells the board's
  PC and outputs, `jvs:option ('log', 1)` each byte.
- `SECURITY_CHIPS` (off by default): the ROM board's X76F100 on bus page 14
  (`src/hl/x76f100.*`, `x76f100test`) and the main board's 93C46 on the
  master's port A, from the sections `rombd_eeprom` and `mainbd_eeprom`
  (MAME's newer hikaru set); with both images, the BOOTROM's check of them
  is no longer patched out. SGNASCAR's protection then answers as its Actel
  FPGA (`hikaru-memctl.cpp`, `chip_get`). Their commands go to the log (the
  first ones), their state into the save states with the option only.
- VON4's card reader (`src/hl/cardreader.cpp`), on the MIE's serial line
  (ports 09-0D, which the game's program at the Z80's 9000 drives):
  `machine.devices.card_reader:option ('log', 1)` logs its frames,
  `('insert', 1)` and `('take', 1)` move the card; `:info ()` where the
  card is. The agent's input `Card` is the F8 key.
- The outputs (`src/hl/outputs.cpp`, README's "Cabinet outputs"): the I/O
  boards declare their general outputs as the machine is built
  (`Outputs::board`), report the bytes of each outputs command (`jvs`) and
  the drive board's commands (`drive_command`); the 837-13844 takes a
  client's `m3ffb_rx` as the drive board's reply (`drive_reply`). The
  emulator starts and stops the game's names and polls the server between
  frames. The games' names (`game_outputs`) were read in their OUTPUT TEST
  through the agent, a TCP client printing each line: each item turned on,
  the output that changed.
- The Z80 runs at 16 MHz, as in MAME: 1600 / the SH-4 clock in MHz T-states
  per 100 SH-4 cycles (16 at 100 MHz; `mie:option ('speed', n)`, or
  `MIE_SPEED`). SGNASCAR's MIE program then waits some 4 seconds before the
  JVS, and the game waits for it, as on the real board.
- The Z80's idle loops run at once: back at a loop's head as it was there,
  with nothing written, no port touched and no write of the SH-4's in
  between, it would run the same turn again until one of them comes, so the
  whole turns are counted at once (its cycles and R too). The games' MIE
  program waits so for the SH-4's commands four fifths of its time; the MIE
  then takes a third of the time, the same frames. `MIE_IDLE_SKIP=0` in the
  environment runs each instruction.


Save states
-----------

A state holds the whole machine: its RAMs and registers, both SH-4s
(registers, on-chip modules, running or asleep), the GPU (registers, command
processor, frame buffer configuration), the MEMCTLs, the MIE and its EEPROM,
the JVS boards and the drive board, the AICAs, the MSCOMM, and the frame
number: once a state is loaded, the emulation goes on from that frame. States
are compressed. Until the first release their layout follows the machine's,
so a state from another build may not load; Valkyrie's no longer do.

- F6 saves `<game>-0.hlstate` (in the write directory's `<game>/` when there is
  one), the others moved up to `-4`, the oldest; F7 loads the one F4 and F3
  have chosen, by default the latest (a `<game>.hlstate` of an older version
  counts as `-0` until the next save); `-l N` loads number `N` at startup,
  `-l FILE` loads `FILE`.
- From the shell: `emu.save_state (path)`, `emu.load_state (path)`.
- `emu.snapshots (every [, keep [, dir]])` saves a state every `every` frames,
  keeping the last `keep` (20) in `dir` (`snapshots`); `emu.snapshots ()`
  lists their frames.
- `emu.seek (frame)` goes to any frame: from the last snapshot before it (or
  from a hard reset), running the frames up to it. Going back drops the
  snapshots taken after it.


init.lua and hooks
------------------

`init.lua`, beside the program, runs once the game is loaded (then the script
of `-x FILE`): [data/init.example.lua](data/init.example.lua) is an example.
`emu.hook (event, fn)` runs `fn` on an event, and returns a number for
`emu.unhook (n)`:

| Event | When |
|---|---|
| `"start"`, `"exit"` | Once, before the first frame and at the end |
| `"frame_begin"`, `"frame_end"` | `fn (frame)` around each frame |
| `"break"`, `"abort"` | `fn (message)` when the machine stops |
| `"irq"` | `fn (from, cpu, line, raised)`: a chip asks a CPU for an interrupt, e.g. `("gpu", "master", "IRL2", true)` |
| `"interrupt"` | `fn (cpu, line, handler)`: a CPU takes one |
| `"write"` | `fn (cpu, addr, size, value, pc)`: a CPU writes an address of `cpu:wpset` |

What the hooks print goes to `hikaruloader.log`, and to the shell's clients.
The hooks cannot step or reset the machine while it runs (`irq`,
`interrupt`).


Agent mode
----------

```
hikaruloader.exe -r airtrix -a
```

(or `-agent`, `--agent`, `-agent-show`, `--agent-show`) lets a program drive
the emulator through stdin and stdout, with the protocol Supermodel's and
sm2-emu's agent modes share, so that one client drives the three: one command
a line, one answer each, `err <reason>` on failure; frames run only on `step`,
as fast as they can (no v-sync); the window shows them; the other messages go
to stderr. Once the game is loaded: `ready GAME 640 480`. A state asked for
with `-l` that cannot be loaded (missing, cut short, another version's) stops
the program before `ready`, with exit code 1: the client never drives a game
started afresh in its place. The protocol is shared: change it only together
with Supermodel and sm2-emu.

| Command | Answer |
|---|---|
| `step [N]` | N frames (1): `ok FRAME`; `err break: ...` or `err abort: ...` when the machine stopped |
| `set INPUT VALUE`, `setraw INPUT RAW` | Holds an input, a role of the game's (a row of controls.ini) over its bindings (a switch: 0 or 1; the analog controls, as sm2-emu's and Supermodel's: `Steering` -1 left to 1, `Accelerator`, `Brake`, `LeftLever`, `RightLever` 0 to 1; `Gear` the gear lever's, -1 reverse, 0 neutral, 1 to 4, those the game has, as sm2-emu's): `ok` |
| `get INPUT` | `value RAW` (`Gear`: the gear the game reads) |
| `release INPUT\|all` | `ok` |
| `inputs` | `inputs N`, then N lines `NAME TYPE`: the game's controls (games.json), by the names sm2-emu and Supermodel give them where they have one (Start1, Start2, Up, Button1..., GunX, Trigger, Offscreen, GunX2..., StickX, StickY...), else by their role (Test, Coin1, ViewChange, ShiftUp, Elevator, ThrustL, P2_Button3...); `Gear other` with a gear lever |
| `obs [W H]` | The picture box-scaled to W x H (640 x 480): `obs W H`, then W*H*3 bytes, RGB, top row first |
| `shot PATH` | The picture as a 24-bit BMP: `ok` |
| `save PATH`, `load PATH` | Save states: `ok` |
| `pointer N X Y BUTTONS [OFFSCREEN]` | Sets mouse or light gun N: X, Y from -1 to 1, buttons bits 0 left, 1 right, 2 middle |
| `state [save\|load\|newer\|older]` | F6, F7, F4 and F3: `state N`, N the one chosen |
| `ram ADDR LEN`, `mem ADDR LEN` | LEN bytes (hex arguments, at most 100000) through the master SH-4's bus: `ram LEN` (decimal), then the bytes |
| `poke ADDR VALUE [SIZE]` | A write through the same bus (hex; size 1, 2, 4): `ok` |
| `frame` | `frame N` |
| `settle [MS]` | A wait before each frame: `settle MS` |
| `ffb` | The force feedback since the last call: `ffb N`, then N lines `FRAME Steering EFFECT FORCE`, EFFECT `stop`, `constant` (FORCE -1..1, > 0 pushes left), `selfcenter`, `friction` or `vibrate` (0..1) |
| `servo` | `servo 0`: no drive board journal |
| `tel`, `tel fields LIST\|all`, `tel others N\|all` | The racing game's telemetry of the last frame, one JSON line (README, "Telemetry"; src/hl/telemetry.h), `tel none` for a game without a decoder; the selections answer `ok` |
| `lua CHUNK` | A command of the Lua shell: `lua N`, then its N lines |
| `quit` | `bye`; the end of stdin quits too |

With the drawing on its own thread (THREADED_RENDERING), `obs`, `shot` and
the screenshots wait for the last frame emulated: the answers are the same.
