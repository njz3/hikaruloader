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
- `HR_NO_AUDIO=1` opens no sound output; `HR_WAV=<path>` writes all the
  sound to a WAV file as well.
- `HR_CLOCK=<count>`: the board's clock starts at that count (seconds since
  1964-01-01 00:00, decimal or 0x hex) whatever the PC's time. In agent
  mode it starts at 0x5BFC8900 (26 Nov 2012 00:00) without it, so that runs
  repeat.
- `MIE_SPEED` and `MIE_IDLE_SKIP=0`, see below.
- `FFB_GAIN`, `FFB_INVERT`: the force feedback (see README.md);
- `FFB_SEND_MS`: the pause after each write to the wheel (16), on the force
  feedback's thread; 0 lets the wheel's USB slow the frames down again;
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
MMU and its FPU's moves and rounding, the Z80, the TLCS-90, the 68000, the ARM7, the AICA's arithmetic, the sound output, the EEPROM,
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
  (`srally2`: NASCAR Arcade's, a Model 3's; `f355`, `18wheeler`),
  `servo_board` `838-14174` the wheel's servo board on the MIDI port (Club
  Kart, The King of Route 66, Initial D, Driving Simulator), `card_reader`
  (`von4` on the Hikaru's MIE, `initiald` and `clubkart` on the NAOMI's
  SCIF), `hopper` `swp` (Club Kart Prize's medals), `cabinet` a Gaelco
  cabinet's outputs (`atv`, `smashdrv`, `football`). The log names each one
  at the start; a value not known is an error there. `g2_board` the board on the G2 bus's
  outside (`naomi-g2board.cpp`, the games' 837-14381:
  `machine.devices.g2_board:option ('log', 1)` logs its accesses).
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

- F4 saves `<game>-0.hlstate` (in the write directory's `<game>/` when there is
  one), the others moved up to `-4`, the oldest; F1 loads the one F2 and F3
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
to stderr. Once the game is loaded: `ready GAME 640 480`. The protocol is
shared: change it only together with Supermodel and sm2-emu.

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
| `state [save\|load\|newer\|older]` | F4, F1, F2 and F3: `state N`, N the one chosen |
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
