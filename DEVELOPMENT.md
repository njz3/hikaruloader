Hikaruloader for developers
===========================

Hikaruloader, Copyright (C) 2026 Benjamin Maurin (njz3). Released under the
GNU GPL v3 or later: see LICENSE.

Notes on the emulator's insides and its debugging tools, for those who work
on it. The user's manual is [README.md](README.md); building the program is
in [INSTALL.md](INSTALL.md); what is left to do in [TODO.md](TODO.md).


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
| `machine.memory.ram_m` | The memories: `ram_m`, `ram_s`, `cmdram`, `fb`, `texram0`, `texram1`, `aica_ram_m`, `aica_ram_s`, `mie_ram`, `bram`, `bootrom`, `eprom`, `maskrom`, `iregs_m`, `iregs_s`; `mem.size`, `mem:dump (path [, offset [, length]])`, `mem:load (path [, offset])` |
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

- `HR_PROFILE=1` measures each part of the frames (the SH-4s, the GPU, the
  MIE, the AICAs and their ARM7s, voices and effects, the renderer and its
  drawing thread, the sound output) and writes the tree to the log on the
  way out, in milliseconds a frame. `HR_ARM_PROFILE=1` and
  `HR_Z80_PROFILE=1` add the busiest addresses of the AICAs' ARM7s and of
  the MIE's Z80, for their idle loops (they run them one instruction at a
  time, slower).
- `HR_EXIT_DUMPS=1` dumps all the machine's memories on the way out
  (`<game>-ram-m.bin` and the others, some 150 MB), in the working directory
  or the write directory's `<game>/`.
- `HR_NO_DRAW=1` sends no mesh to the GPU; `HR_STATS=1` prints each frame's
  draws and triangles; `HR_SYNC=1` waits for each mesh and prints the slow
  ones; `HR_LOG=1` logs the renderer's states. `HR_GL_DEBUG=1` asks for a
  debug OpenGL context, whose driver then logs its errors as they happen
  ("GL: ..."): without it, Intel's says nothing (about 10% slower).
- `HR_NO_AUDIO=1` opens no sound output; `HR_WAV=<path>` writes all the
  sound to a WAV file as well.
- `HR_CLOCK=<count>`: the board's clock starts at that count (seconds since
  1964-01-01 00:00, decimal or 0x hex) whatever the PC's time. In agent
  mode it starts at 0x5BFC8900 (26 Nov 2012 00:00) without it, so that runs
  repeat.
- `MIE_HACK=1`: Valkyrie's hack instead of the MIE (below); `MIE_SPEED` and
  `MIE_IDLE_SKIP=0`, see below.
- `FFB_GAIN`, `FFB_INVERT`: the force feedback (see README.md);
  `DRIVEBOARD_SIMULATED=1`: the drive board's program simulated even with
  the srally2 set.
- `COMM_BOARD = PROGRAM`'s ring (hikaru-commbd.cpp): `HR_COMM_LATENCY`, the
  fibre's latency in the board's clocks (2450, 8 lines); `HR_COMM_SLICE`,
  the lines between two times sent (2); `HR_COMM_SPIN`, the microseconds a
  wait looks for the next time before sleeping (300); `HR_COMM_SEND_SPIN`,
  the same for the link's sender thread (300). Each cabinet logs
  what went round every 10 s ("frames out, in, held, lost, late", its
  waits).

The unit tests run with `ctest -C RelWithDebInfo` in `build` (the SH-4's
MMU and its FPU's moves, the Z80, the TLCS-90, the 68000, the ARM7, the AICA's arithmetic, the sound output, the EEPROM,
the X76F100, the nvram file, the write directory, the games folders and their 7z, the
controls' hotkeys and wheel rotation on SDL's virtual joysticks, the drive
board, the card reader, the aspect ratio's layouts, the CRT presets' reader
and the shipped presets, the bezels' layouts, the translations).
`driveboardz80test` is not among them: it needs EPR-20512 (`epr-20512.bin`),
a script and a journal of Supermodel's, and is run by hand.


Controls: the MIE and the JVS bus
---------------------------------

The games read the cabinet through the MIE (315-6146), whose Z80 runs a
program the game puts in its RAM; that program drives the JVS bus, where the
I/O boards hang in a chain. Hikaruloader runs that Z80 (`src/cpu/z80`, its
tests: `ctest -C RelWithDebInfo` in `build`), and emulates the bus and a Sega
837-13551 I/O board on it (`src/hl/jvs.cpp`): Test (F5), Service (F6), the
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
  read when the game has no `.nvram` yet.
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
  card is. The agent's input `Card` is the F7 key.
- The outputs (`src/hl/outputs.cpp`, README's "Cabinet outputs"): the I/O
  boards declare their general outputs as the machine is built
  (`Outputs::board`), report the bytes of each outputs command (`jvs`) and
  the drive board's commands (`drive_command`); the 837-13844 takes a
  client's `m3ffb_rx` as the drive board's reply (`drive_reply`). The
  emulator starts and stops the game's names and polls the server between
  frames. The games' names (`game_outputs`) were read in their OUTPUT TEST
  through the agent, a TCP client printing each line: each item turned on,
  the output that changed.
- `MIE_HACK=1` in the environment brings back Valkyrie's hack, for
  debugging: no Z80, no JVS, Test and Service alone (no controls, no
  outputs). It was a setting, "Fake the MIE", until November 2026; an
  `hikaruloader.ini`'s `MIE_HACK` is no longer read.
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

- F4 saves `<game>.hlstate` (in the write directory's `<game>/` when there is
  one), F1 loads it; `-l FILE` loads `FILE` at startup.
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
to stderr. Once the game is loaded: `ready GAME 640 480`. The protocol is
shared: change it only together with Supermodel and sm2-emu.

| Command | Answer |
|---|---|
| `step [N]` | N frames (1): `ok FRAME`; `err break: ...` or `err abort: ...` when the machine stopped |
| `set INPUT VALUE`, `setraw INPUT RAW` | Holds an input (a switch: 0 or 1): `ok` |
| `get INPUT` | `value RAW` |
| `release INPUT\|all` | `ok` |
| `inputs` | `inputs N`, then N lines `NAME TYPE`: Test, Service |
| `obs [W H]` | The picture box-scaled to W x H (640 x 480): `obs W H`, then W*H*3 bytes, RGB, top row first |
| `shot PATH` | The picture as a 24-bit BMP: `ok` |
| `save PATH`, `load PATH` | Save states: `ok` |
| `ram ADDR LEN`, `mem ADDR LEN` | LEN bytes (hex arguments, at most 100000) through the master SH-4's bus: `ram LEN` (decimal), then the bytes |
| `poke ADDR VALUE [SIZE]` | A write through the same bus (hex; size 1, 2, 4): `ok` |
| `frame` | `frame N` |
| `settle [MS]` | A wait before each frame: `settle MS` |
| `ffb` | The force feedback since the last call: `ffb N`, then N lines `FRAME Steering EFFECT FORCE`, EFFECT `stop`, `constant` (FORCE -1..1, > 0 pushes left), `selfcenter`, `friction` or `vibrate` (0..1) |
| `servo`, `tel` | `servo 0`, `tel none`: no drive board journal or telemetry yet |
| `lua CHUNK` | A command of the Lua shell: `lua N`, then its N lines |
| `quit` | `bye`; the end of stdin quits too |

With the drawing on its own thread (THREADED_RENDERING), `obs`, `shot` and
the screenshots wait for the last frame emulated: the answers are the same.
