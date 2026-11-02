PROCESSORS AND CHIPS (Hikaruloader, October 2026)
------------------------------------------------

| Part | Where | State |
|---|---|---|
| 2 x SH-4 (Hitachi SH7750, 200 MHz) | Main board | Dynamic recompiler (the default; the settings' "SH-4 processors" switches to the interpreter at the next frame), at the clock of the `SH4_CLOCK` setting (100 MHz by default since November 2026, AIRTRIX's slave needing it; formerly 57, and 50 before; 200 MHz as on the board, but the games' busy waits then cost about real time and the boots fall below 60 fps), the timers (TMU) at the board's Pφ (50 MHz) whatever the clock, and their prescalers: the code decoded once in blocks (`src/cpu/sh/sh4-blocks.*`), translated to x86-64 by our own emitter (`src/cpu/x64/`, `src/cpu/sh/sh4-x64.*`): the frequent ALU instructions and branches written out, the loads and stores of every size and the FPU's moves too (FPSCR.SZ tested as they run), the others calls to the interpreter's handlers; those reach RAM and ROM straight as the translations do (Mmap::direct). The logs' CPU and machine state (debug_string, two strdup and a sprintf) is no longer made when the log is off: it was, at each on-chip register's and each logged device's access. With these, about a third less time a frame in every game, the same pictures (the six games' frames 900-3600, the interpreter's too). Same machine as the interpreter, frame for frame (SGNASCAR, SWRACER checked). The idle and delay loops run forward (`cpu:option ('idle', 0)`). The interpreter stays for the debugger, and runs alone while the MMU is on (its TLBs, their exceptions run again after their handler, LDTLB, the TLB arrays; TRAPA; `tests/sh4_mmu_test.cpp`): Smashing Drive's. Also the operand cache as RAM (CCR.ORA, 7C000000) and the SCIF's and SCI's output in the log. The FPU's fmov moves 32 bits unless SZ, whatever PR, and DRn is FRn (high half) and FRn+1, a 64-bit move taking FRn from the lower address (pairs of singles): the compilers' unsigned division (2^31 loaded in DR4 by two fmov.s) gave 0 or 1, and PHARRIER unpacked its sound data a kilobyte a frame, 15 s of black screen (`tests/sh4_mmu_test.cpp`) |
| Z80 of the MIE (315-6146) | Main board | Emulated (`src/cpu/z80`, 16 MHz); the MIE's 2 KB internal ROM is not dumped, a stub plays its part; the SH-4s request its bus (local bus 02000000) to reach its RAM, which stops it. Its idle loops run at once, exactly (`MIE_IDLE_SKIP=0`: one instruction at a time) |
| 93C46 serial EEPROM | Main board, on the MIE's GPIO 5/6 | Emulated (`src/hl/eeprom.cpp`), kept with the backup RAM in `nvram/<game>.nvram` |
| 93C46 serial EEPROM (IC115) | Main board, on the master SH-4's port A (bits 2-5) | Emulated with `SECURITY_CHIPS` (November 2026, off by default; `src/hl/eeprom.cpp`, on `hikaru.cpp`'s port A): its image `93c46.ic115` from MAME's newer hikaru set, the main board's serial number (AABE-...), which the BOOTROM reads (16 READs) and checks with a CRC-16 (@0C00087C). Without the option, that check is patched out of the BOOTROM (Valkyrie's patch at 0x8AE, 0x8E6 in 0.96) |
| 2 x ARM7DI, in the two AICAs (Yamaha, 315-6232) | AICA board, main board | Interpreted (`src/cpu/arm/arm7.*`, `tests/arm7_test.cpp`), each with its AICA's 8 MB of RAM, timers and interrupts; the drivers' scan of their 64 slots run at once, exactly (`SOUND_CORE = OPTIMIZED`) |
| AICA voices, DSP, filter, LFO | In each AICA | Emulated (`src/mach/hikaru/hikaru-aica.*`): the 64 channels (PCM 16/8, ADPCM), envelopes, filter, LFOs, the DSP's effects and its ring buffer; out to the host's speakers (`src/hl/audio.*`: front and rear, resampled, its clock followed). The RTC reads a fixed time |
| 68000 | Communication board 837-13404 (linked cabinets) | Two ways (`[Network] COMM_BOARD`): simulated by default, the board emulated from what it gives the games (`src/mach/hikaru/hikaru-comm.*`), two instances playing together in SWRACER, SGNASCAR, PHARRIER and VON4; or run, `PROGRAM`, the 68000 (`src/cpu/m68k/`, from Odin's) running the program the game uploads, with the board's 315-5917s as Odin has them (`src/mach/hikaru/hikaru-commbd.*`), the instances in step at 2 lines: 3 and 4 instances link (SWRACER races of 4, VON4's 4 sites). The ring over TCP either way (`src/hl/ringlink.*`, `[Network]`) |
| TMP90PH44 (Toshiba TLCS-90) | JVS I/O board 837-13551 | Simulated at the JVS level by default (`src/hl/jvs.cpp`); with `IO_BOARD_PROGRAM` (November 2026, off by default) its CPU runs its program, 315-6215 from MAME's jvs13551 set (`src/cpu/tlcs90`, `src/hl/tmp90ph44.*`, `src/hl/ioboard.*`, from Odin's, by Réda Chérif-Touil): the timers, the watchdog, the serial channel, the A/D converter and the ports as the program uses them, its bytes on the MIE's line one at a time at 115200 bauds. The TLCS-90 core decodes the PROM's instructions once (`tests/tlcs90_test.cpp`). The JVS TEST then shows the board's own answers |
| 315-6146 (Z80) and D71054 | NASCAR's JVS I/O board 837-13741 | Simulated as an 837-13844 by default; with `IO_BOARD_PROGRAM`, the 315-6146 runs epr-21868 from MAME's 18wheelr set (`src/hl/ioboard.*`): its UART 1 on the MIE's line, its interrupts (the UART at level 3, the D71054's 1 ms pulse at vector FE), its GPIO (the switches, the coins, the 22 outputs: the drive board's command on 15-22, its reply on the switches, as on the cabinet), its converter (the wheel, the pedals). Its waiting loop (03A2-041A, polling the UART's LSR) runs at once, exactly, as the MIE's do |
| 315-5881, X76F100 secure EEPROM, Lattice CPLDs | ROM board | The protection on the bus at 0A000000 (November 2026, `hikaru-memctl.cpp`): SGNASCAR's streams a MASKROM block, decrypted and decompressed (the NAOMI "M1" scheme, key 56DEDF33); SWRACER's is a 315-5881 (`hikaru-5881.cpp`, from MAME's), its 64 KB of RAM and registers at 0A010000 as the NAOMI "M3" carts. When their check failed, SGNASCAR and SWRACER raised their motion blur (181) to FC: the night race and the text crawl smeared. AIRTRIX's and PHARRIER's are on too (November 2026). PHARRIER's (2912C68A) is checked at the game's start (@0C103460): a ROM block (2FEE6000) written to it, subkey 5784, and the 0x273C bytes read back compared with the clear one at 103FD000; the patch that skipped the check is gone. AIRTRIX's decrypts right (12 blocks, @0C0440F0, the same words across their ends): the slave's real program, copied to its 0C010000 through aperture 16, which poses the skater itself from the bones' table (48100000) into the routine the CP calls. Some 875,000 instructions a frame: at 57 MHz it ends after the CP called the routine (the body not drawn). The recompiler now takes about 4 ms a frame for it (25 ms before November 2026): over 80 s of attract, 147 frames a second with the chip, 176 without, here. With it the CP's posing is off (gpu->skin.slave_poses), and the SH-4s need 100 MHz (SH4_CLOCK) for the slave to end before the CP's call: the skater drawn whole, posed as the board does, his shadow his whole silhouette; at 57 MHz his body is missing. At 100 MHz the six games stay above 60 frames a second here (80 s of attract: AIRTRIX 100, SWRACER 83, VON4 113; SWRACER's boot the slowest, 69), their attract a little ahead, closer to the board's (PHARRIER unpacks its sounds sooner). Without the chip the slave runs a plain program that leaves the rest pose, which the CP poses ("Skinning" in hikaru-gpu-cp.cpp). The default clock is 100 MHz since; an ini written before keeps its own (57). The X76F100 secure EEPROM is emulated with `SECURITY_CHIPS` (November 2026, off by default; `src/hl/x76f100.*`, from Odin's and MAME's; `tests/x76f100_test.cpp`) on bus page 14: its image `x76f100.ic85` from MAME's newer hikaru set (no passwords, the ROM board's serial number BBAE-...), which the BOOTROM reads (command 81, page 0) and checks (@0C0008A2): blank, it stops on ERROR 2. With the option, SGNASCAR's protection also answers as its Actel FPGA (317-0283-COM): the status (80008000 while a stream runs), the stream started and stopped by the control's bit 31 and ended by symbol 111, read a byte at a time as wide as the access, the registers at 00-13 only; the games play the same (frames identical). Without the option, unchanged |
| Drive board (force feedback) | SGNASCAR | A Model 3 board on the 837-13844's outputs and switches: its Z80 running its EPROM, EPR-20512 (`src/hl/driveboard-z80.cpp`, from Supermodel's), taken from the user's `srally2.zip`; without it, the program simulated (`src/hl/driveboard.cpp`). SWRACER has none: its output test lists lamps and a boost lock only. `-L` and the picker tell which games have one, and which link (`data/games.json`, from the games' test menus) |


DONE IN HIKARULOADER (October 2026)
-----------------------------------

What the fork added to Valkyrie, in short; the sections below give the
details and what is left.

- The program: C++20 and CMake, built on Windows with MSVC and vcpkg (the
  Makefile runs CMake); a picker of the games and a settings window (Dear
  ImGui), in English and French; MAME romsets, zips or 7z (the LZMA SDK),
  found by CRC32; `-L` lists the games and their files ([README.md](README.md),
  [INSTALL.md](INSTALL.md)).
- The controls: keys, controllers, wheel and pedals, bound in the settings,
  saved to `controls.ini` (linuxloader's syntax); Test and Service, coins,
  Start, four buttons, the analog channels; the controllers' hotkeys (Guide
  + a button) and a wheel's rotation for the game's wheel, as Odin has them.
- The saves: each game's EEPROM and backup RAM in `nvram/<game>.nvram` (a
  container: header, directory, blocks); the save states of the whole
  machine (F4, F1, `-l`); a write directory (`--write-dir`) for read-only
  cabinets.
- The I/O: the MIE's Z80, the JVS bus and an 837-13551 I/O board (an
  837-13844 for SGNASCAR, with its Model 3 drive board and the force
  feedback); VON4's card reader; the EEPROM's system settings (region,
  attract sound, link mode) from the settings. Optionally (November 2026,
  off by default): the I/O boards' CPUs running their own programs
  (315-6215 on the 837-13551's TLCS-90, epr-21868 on NASCAR's 837-13741),
  and the security chips (the ROM board's X76F100, the main board's 93C46,
  SGNASCAR's Actel FPGA), the BOOTROM's check of them run.
- The outputs (November 2026, `src/hl/outputs.cpp`): the I/O board's lamps
  and SGNASCAR's drive board commands in MAME's network protocol, as
  Supermodel, Flycast and sm2-emu send them (TCP 8000, the UDP announcement
  on 8001, `m3ffb` and its `m3ffb_rx` answer); each game's names from its
  OUTPUT TEST, the item turned on and the output that changed; the
  settings' Outputs tab lists them with their values.
- The picture's settings as linuxloader, Supermodel, Flycast and sm2-emu
  have them (November 2026, `src/hl/aspect.cpp`, from Supermodel's
  AspectRatio): RENDER_SCALE up to 6 in the Display tab, ASPECT_RATIO's
  seven frames with CROP_POSITION and the bent edges (CROP_BARREL, in the
  final pass's shader), DISPLAY_MODE's _STRETCH, INTEGER_SCALE,
  SCALING_FILTER; the old values read.
- The sound: both AICAs, their ARM7s, voices, DSP effects, to the PC's
  speakers (from mono to 7.1), resampled, the sound card's clock followed.
- The linked cabinets: the communication board emulated, the instances in a
  ring over TCP; the four linked games play two together.
- The SH-4s: a dynamic recompiler to x86-64, their idle and timer loops run
  forward, the clock a setting (100 MHz by default); their MMU (interpreter).
- The rendering: shading, lights, blending, punch-through and translucent
  meshes sorted, the dot patterns (501), fog, mipmaps, levels of detail, the
  2D layers, AIRTRIX's skinning; the drawing on a thread of its own;
  widescreen 3D, a smooth scaling filter, a longer draw distance; the
  enhancements: the 3D at 2x to 6x, MSAA, FXAA, anisotropic filtering,
  textures enlarged by Scale2x, bloom, a CRT filter; libretro CRT presets
  (Lola Retro's shipped) and bezels around the picture, from Odin's.
- Speed: the MIE's and the ARM7s' idle loops run at once, exactly; the
  sound chips' registers and DSP program kept decoded (`SOUND_CORE`).
- The tools: a Lua shell with breakpoints, watchpoints and hooks, an agent
  mode shared with Supermodel and sm2-emu, `HR_PROFILE`, unit tests
  (`ctest`); see [DEVELOPMENT.md](DEVELOPMENT.md).
- A second machine under way: Gaelco's PowerVR board (below).


GENERAL
-------

Add support for "required" field in the JSON file. Done in Hikaruloader the
other way round: a section is required unless marked `"optional": true`
(the srally2 set's EPR-20512, for SGNASCAR's drive board); `"set"` names
the MAME set a section comes from.

For later, write an event-driven scheduler. The machine runs a line at a
time (544 a frame): each SH-4 a line's cycles, then the MEMCTL's DMA, the
GPU, the MIE and the AICAs; the waits the games poll are run forward (the
SH-4s' idle loops, and their loops reading the timer, sh4.cpp).

I'm seriously thinking about slowling "porting" valkyrie to C++... Mmm.
Done in Hikaruloader: all of it is C++20 now.

The outputs (`src/hl/outputs.cpp`), what is left:

- Brave Firefighters' menu (Test, in the attract or at the start) is the
  developers' one, with no OUTPUT TEST: outputs 1 and 4 (the start lamps)
  and 2 (player 1's nozzle, as it sprays) seen in a game; 3, 5 and 6,
  likely player 2's nozzle and the revolving lights, not seen (no player 2
  in the agent). The game's own B.F.F. TEST MENU not reached.
- NASCAR Arcade's OUTPUT TEST lights START BUTTON and VIEW CHANGE BUTTON in
  turn (outputs 1 and 4), whatever the cursor: which is which taken from the
  other games (start on 1). Output 14 stays on, unnamed.
- Virtual-On 4's program has LAMP and COIN SHUTTER items too, not in its
  OUTPUT TEST here (the VO4 terminal's, likely).
- Air Trix's LOCK MECHA. reads OFF in the test with output 4 at 1; it stays
  at 0 in the attract and after Start.
- MAME's Windows messages (MAMEOutput, as sm2-emu and Supermodel have them),
  and the outputs in the agent protocol (shared with Supermodel and
  sm2-emu), not done.

The controls (`src/hl/controls.cpp`), what is left:

- The controllers' hotkeys (Guide + a button, November 2026) are checked on
  SDL's virtual game controller (tests/controls_test.cpp), not yet on real
  pads: an Xbox pad's Guide through XInput (the Game Bar takes it too), a
  DualShock's PS button. A joystick that SDL knows no mapping for has no
  Guide, hence no hotkeys.
- The wheel's rotation ([Input] WHEEL_ROTATION and WHEEL_RANGE, November
  2026) is checked on a virtual wheel, not felt on a real one; SDL 2 cannot
  read the turn a wheel's driver is set to, hence WHEEL_RANGE. The servo
  board's spring gets as much stiffer, up to the device's strongest; its
  friction, which follows the speed, is left as it was.


HIKARU
------

Move the slave GPU access to hikaru-gpu.c. Proper support may require the
introduction of a new vk_device_accessors_t class.

Proper MIE emulation (wrap a Z80 emulator in a vk_cpu_t sub-class and tape
it to the code.) Done in Hikaruloader: the Z80, the JVS bus, an 837-13551
I/O board, the GPIO with the 93C46 EEPROM and the MAINBD switches, the ports
as the SH-4 sees them (00800000). Left: the MIE's other ports (40, 80, 81,
84); the interrupts it routes to the SH-4's IRL2 (GPIO B, C, F); GPIO E's
bits 0-3, which the BIOS waits for at boot (Valkyrie's toggle kept). It
runs at 16 MHz (MAME's figure; some 13.5 measured on a NAOMI).

The I/O boards running their own programs (`IO_BOARD_PROGRAM`, November
2026, off by default; `src/hl/ioboard.*`, ported from Odin): the 837-13551's
TMP90PH44 running 315-6215, NASCAR's 837-13741 running epr-21868, at the far
end of the MIE's UART (hl::JvsLine): each byte the MIE sends reaches the
board a character later (86.8 us at 115200 bauds), the board's come back the
same way, the board run a piece (64 us) ahead of the MIE, the MIE stopped at
each of its bytes; the MIE's LSR tells when its bytes are gone. Checked: the
six games boot, count their coins, start and play; the BIOS's JVS TEST shows
the board's own name and inputs (BRAVEFF's test menu has none: its service
credits counted); NASCAR's INPUT and OUTPUT TESTs show its wheel, pedals and
lamps, its drive board taking the same commands; the save states replay the
same. Left, or guessed (as in Odin, whose notes these are):
- The cabinet's Test and Service go to the I/O board only in that mode: on
  both boards at once, the BIOS took one press for two, the board's
  reaching the game a few frames after the main board's switches.
- The inputs are read every millisecond of the board's time; a coin closes
  its switch 50 ms, then leaves it open 50 ms.
- 837-13551: its 14.7456 MHz crystal taken as the MIE's UART's; the
  TMP90C844's registers where MAME has them, T01MOD and T23MOD as MAME reads
  them; timers 0 and 2 ask for nothing; INTEH bit 2 enables 0028 and bit 0
  0038; the serial interrupts' enables, the bits of IRFH but bit 2, guessed;
  P5 reads its latch, P3's latch at 1 after the reset, ADMOD F7 converts AN0
  to AN3 over and over; INTWD waits for the interrupts to be open. P3 bits
  0-1 read 3 (no board after it).
- 837-13741: its 315-6146 at the MIE's clock; vector F8 its UART, FE its
  timer every 1 ms (the D71054's wiring, and its first pulse a period after
  the load, guessed); port 91 reads FF (no board after it, the DIP switches
  off); ports 40, 60, 80, 81 and 0E, written at the start, and 90 read, are
  only logged.
- The reply's delay of a simulated board is still none: the simulation
  answers when the MIE releases the line.

The drive board: done in Hikaruloader, its Z80 running EPR-20512 (a
DriveBoardProgram, DriveBoardZ80, as the simulation is), the EPROM from the
user's srally2 set (an optional section of another set in games.json), the
simulation without it or with DRIVEBOARD_SIMULATED=1. Checked against a
journal of Supermodel's (tests/driveboard_z80_test.cpp, by hand: the EPROM is
not ours to ship): the same replies and servo writes, their 552 changes in
the same order and, against Supermodel's Z80 now counting Zilog's T-states
too (DJNZ, the conditional JR, RET and CALL, LDIR and LDDR), at the same
NMIs but one of 21776. Before, its board started 2 frames sooner (its RAM
test and clearing 16% shorter), and then the NMIs fell a little apart in the
frames, a command at a frame's start seen an NMI sooner or later. The NMIs
come every
16000 cycles from the CTC's write, as the CTC counts (Supermodel's a few
cycles late each); the board's wait states are unknown. The commands are
queued, each shown a frame at least. Left: the servo board's spring and friction are
guesses; the analog
channels per game (0, 1 and 2 take controls.ini's wheel, accelerator and
brake in every game, unless the shell sets them); the force feedback's
settings in the settings window; a network output of the command for a real
drive board.

Proper AICA emulation (especially the RTC and IRQs.) Done in Hikaruloader:
the ARM7s, the 64 voices, the DSP, the timers and interrupts, the RTC (the
PC's local time counted from 1964-01-01 as the BOOTROM's date routine does,
by the samples; CLOCK SETTING's difference kept in the nvram's CLCK block;
HR_CLOCK and the agent fix its start). Left:
- The RTC: at frame 1584 from power-on the BIOS showed 15 s more than the
  frames' time (the samples before the first frame?).
- The levels against a real board's (no recording yet). On 66 s of attract
  the outputs stay under full scale (AIRTRIX's rear: 3 samples clipped);
  in play each board reaches it. Folded to two speakers at full gain, the
  quad games clipped (PHARRIER 2833 samples in 93 s, SWRACER 2308, SGNASCAR
  1960): the rear now comes at -3 dB, the peaks left rounded off
  (hl::Audio::soft_limit). Excerpts: build/RelWithDebInfo/sound_tests.
- PHARRIER's SH-4 streams data from the ROM board to 7DFFC0-7E073F of the
  first AICA's RAM, across the start of the DSP's ring (RBP FC0: 7E0000), at
  times; read by the reverb's taps before the DSP's writes cover it, it gave
  a 5 ms noise once. As the registers read, the board would do the same:
  kept. With the writes there dropped, the WAV of frames 0-5800 is identical.
- The AICAs never interrupt the SH-4s in the attracts (MCIPD bit 5 enabled,
  never set by the drivers); SWRACER and SGNASCAR restart their drivers
  4-5 times in 4000 frames, AIRTRIX once.

Implement the three EEPROMs (MAINBD, ROMBD, MIE.) The MIE's is done. The
MAINBD's (a 93C46 on port A) and the ROMBD's (an X76F100 on bus page 14)
with `SECURITY_CHIPS` (November 2026, off by default), from MAME's images,
with SGNASCAR's Actel FPGA as the chip; the BOOTROM's check of them then
runs and passes. Left: their writes are not kept from a session to the next
(the BIOS and the games only read them: no write seen); whether the
X76F100's SDA floats once it is deselected (it reads high, as in MAME), and
whether a read goes past the data's end; the Actel's other status bits,
what bits 30, 1 and 0 of its control do, and the value of its register 0C
at the power-on (19620217 kept, which the BOOTROM's probe finds there).

The communication board, simulated (hikaru-comm.cpp, `COMM_BOARD =
SIMULATED`, the default): the four linked games play together, two
instances on one PC: a race of two in SWRACER and SGNASCAR, PHARRIER's twin
cabinet game, a battle of two in VON4 (each one's life the other's
enemy's). Its program's work is guessed: the status words' bits after the
link (status 1's high byte taken as the nodes, the games showing 1/1
alone), the 68000's RAM at 1F000 the games read (zeros), the registers 00
and 40 the games set at the start.

The communication board's program run (hikaru-commbd.cpp, `COMM_BOARD =
PROGRAM`, November 2026): the 68000 (src/cpu/m68k/, tests/m68000_test.cpp)
and the board's 315-5917s as Odin emulates them (by Réda Chérif-Touil), the
program the games upload running on them; the instances of a ring in step
at 2 lines of the Hikaru's, a fibre of 8 lines (0.25 ms), their frames
aligned. Done on one PC: SWRACER with 2, 3 and 4 instances (CONNECTION WAS
ESTABLISHED, NODE ID n/4, a race of four, POSITION 1st/4 to 4th/4); VON4
with 4 (THIS IS MASTER SITE, SLAVE SITE, seats A to D, packet size
11E4/1200, then its title). Left:
- SGNASCAR with more than 2 (up to 8: a hop of 0.25 ms leaves its small
  frame 2 ms round 8 boards, within its turn's 4.5 ms; not tried), the
  relay (its monitor); PHARRIER and VON4's battle with the program (not
  tried yet).
- Two PCs: the hops over a network take longer than the 0.25 ms of the
  fibre (HR_COMM_LATENCY to try; 16 lines already halve VON4's frames a
  turn with 4 cabinets, the program skipping every other turn).
- The cost: in step, a ring runs as fast as its slowest instance, and four
  instances saturate an 8-core laptop (four SWRACERs racing each other at
  about 25 fps unthrottled, four racing alone at 37; see
  hikaru-commbd.cpp). Shared memory between the instances of one PC would
  spare the TCP sends (15 us each on Windows, a thread of the link's own
  sends them); the spins (HR_COMM_SPIN, HR_COMM_SEND_SPIN) are to tune on
  a quiet PC.
- On Windows, SO_REUSEADDR lets two instances listen on the same PORT_IN,
  the connections then going to either (a ring of 4 took a stale
  instance's time, 29 s ahead): SO_EXCLUSIVEADDRUSE, if it lets a quit
  instance's port be taken again at once.
- Odin's guesses stand (its TODO, "Network board"): the 68000 at 10 MHz,
  IRQ5's counter, a frame held 1 ms at most, the status registers' 05.
- Save states keep the board but not the frames on their way: a cabinet
  loaded goes out of step and takes the ring's time again.
- The seats are set in each game's test menu; the settings window has no
  Network tab.


SH-4
----

Import the LLVM jit core into valkyrie. Still a bit rough on the edges, but
already faster than the interpreter. Hikaruloader's way instead: its own
dynamic recompiler on the blocks of sh4-blocks.h (decoded once, checked
against the memory on entry, run with the interpreter's order of
interrupts and cycles), translated to x86-64 (sh4-x64.cpp). The memory
maps have a table of pages (host pointers for RAM and ROM, the devices
through their regions) and fastmem (RAM and ROM, in sections, mapped in a
reserved 512 MB at the CPUs' addresses; the settings' "Memory access").
The translations write the 32-bit loads and stores out, a backend class
for each way (Sh4X64Calls, Sh4X64Pages, Sh4X64Fastmem), the devices through
the interpreter's path. Next: the byte and word accesses, the FPU's
(fmov, a tenth of SGNASCAR's instructions); fastmem without its bit map
test (the devices through the faults, the faulting code patched to call
the slow path); blocks chained to each other; unwind
information for other hosts than Windows (the JIT is off there until
then); the busy waits that are neither idle loops nor timer polls
(AIRTRIX's loop on the MIE's mailbox, AT:@0C69B358) run forward, to make
200 MHz as fast as the default.


GPU / RENDERING
---------------

The projection clipping planes are probably scaled by some value, because
using the ones provided by the games as they are leads to a far too close
far clipping plane. Use 'p' to toggle a standard 90 degree FOV projection and
see things (affects all games). In Hikaruloader: the projection comes from
the viewport's clip rectangle and offset, the far plane is not the games'
(the scenery is drawn to 1e5), and 'p' now cycles through the poly types
(a debugging key). Left: the meaning of the far planes the games give.

Fix alpha ordering, urgently. Done in Hikaruloader: the punch-through
meshes (poly type 4) are alpha-tested and drawn before the translucent ones,
which are drawn back to front (see the framebuffer blending below).

The matrices pushed by 161 (P) are no instances: the vertices with S take
the one pushed, the other ones the current matrix (hikaru-gpu-cp.cpp, 161).
Done in Hikaruloader: BRAVEFF's characters and flames are whole, AIRTRIX's
trick trails drawn. Left: AIRTRIX's runs of 20-27 pushes before its skater
(the board's last positions), of unknown use.

Shading, done in Hikaruloader (hikaru-renderer.cpp, mesh_fs_source). The
base colour is the material's diffuse (091), or, textured, by the mode in
2C1's top bits: the texel (0), the texel over the diffuse (1), the texel
times the "ambient" 291 (2: most of AIRTRIX's scenery, its blue
"restaurant" sign), the diffuse with the texel's alpha (4), from the
diffuse to 291 by the texel (5); 881's alpha mode mixes by the alpha. Unlit
meshes (881 shading mode 0) show it as is: AIRTRIX's shadows dark, its
OHOSOHO sign magenta. Lit, it is multiplied by the lights plus the scene's
ambient light (011) times 291 and 691 (the ambient reflectance, 7F common;
mode 2: 691 alone), and the specular is added: the light's colour times
491's 6-bit colour. The 291 in mode 1 since November 2026: PHARRIER's
asteroids (291 olive, 691 FF) were twice as bright as on the board's video,
their shade grey where the board's is olive (35.5 s, the rock's luminance
percentiles 22/48/89 on the board, 54/99/168 before, 27/59/120 after); in
mode 2 it turned PHARRIER's orange flames magenta. SWRACER and SGNASCAR
(291 FF) are unchanged, AIRTRIX, VON4 and BRAVEFF a few percent darker.
Two-sided lighting lights the side seen (its normal turned for a triangle
seen from behind), no longer abs (N.L). A material lit with all its lights off still takes the
ambient. The ambient light (011) stays through viewport recalls (AIRTRIX's
skater, in viewport 1 committed without one, was black under its spots).
Left: mode 3 unknown; 091 (diffuse) falls through to 291 too, as in
Valkyrie; no game seen uses the poly types "shadow A/B" (103 types 2, 3).

From Odin's decoding (Réda Chérif-Touil), November 2026, checked against
the board videos:
- A light's ambient level (051 bits 16-19, PHARRIER's light editor's
  "amb"): level / 16 of its colour on every polygon it reaches, facing it
  or not, cut by its distance and cone. SGNASCAR's sun (directional, 4 and
  6) brought its track up to the board's: median 154-190 before, 195-231
  after, 207-224 on the board video's ENTRY DRIVERS. BRAVEFF's point lights
  (its fire's orange at 15, flickers at 4): its platform scene closer (mean
  48 before, 65 after, the board 61-78), its burning wall farther (31
  before, 42 after, the board 32). Left: why the wall; the D bit (BRAVEFF's
  reticles' light, not off).
- A parallel light with a null direction lights every side (BRAVEFF's
  stages, as Odin saw on a cabinet); the shader's direction was no number.
  Not seen in the attracts' frames compared.
- The depth queue without a colour (621 D, type 0): the opaque and
  punch-through meshes fade into what is behind them, linearly in the
  eye's depth (start near / Q, range 1 / P). PHARRIER's far mountains fade
  into the sky. Its type is 3 bits (2, 4, 6 a density: not done).
- The coloured fog is linear in the eye's depth too: P = 1 / (end - start),
  Q = near / end, as the VON4 library builds them (it was linear in 1/z
  from 1/P to 1/Q). AIRTRIX's ranking screen (P 0.0025, Q 0.0000833, near
  0.1: white from 800 to 1200) went from grey (109/130/169 in its top,
  middle and bottom bands) to black and olive (0 / 37,36,30 /
  104,101,83), the board video's 0 / 19,19,10 / 82,81,61. SGNASCAR's
  attract frames unchanged.
- The spots' tables (194): the light rises from its cone's edge along its
  table's 16 points (the first of a light's four, 0x400 = 1.0), as US
  patent 6,333,742 draws it. AIRTRIX and SGNASCAR load 0.05, 0.10, 0.20 ...
  0.50, then 0.05 a point to 1.0, in all their lights: AIRTRIX's halfpipe a
  few percent brighter. A table never loaded (all 0) keeps the straight
  rise. The rise's width (451's eeeemmmm, ours 32 / (32 - m) x 2^e, Odin's
  1 + m / 16 x 2^e) differs by 13 % at most: kept.
- The blended meshes of modes 0 and 2 weighted by their alpha before the
  clamp: a lit colour above 1 keeps its excess (PHARRIER's logo galaxy).
- The frames' blending (781/181) in integers, as the patent's blender: a
  rate from 0x80 counting one more, each product's fraction dropped. Blended
  in floats, a pixel rounded to the nearest could stay one step above the
  new picture at FC, a trail that never died out.
- The level of detail's sizes in 1/16 of a pixel (32 x radius x f /
  distance; SGNASCAR's sizes are those of 300, 400 and 500 units at 60
  degrees): 1/8 before, so the finer models now come at twice the distance.

Translucency (November 2026): with OpenGL 4.3 and no MSAA, the translucent
meshes' fragments are kept in a list a pixel, then blended over a copy of
the picture, the farthest first, ties in the order sent (the mesh's, then
its triangle's), in an 8-bit buffer's arithmetic, each blend rounded to the
nearest; eight at a time, again until the list is done, none lost
(SORT_TRANSLUCENT_PIXELS, "Translucency" in hikaru-renderer.cpp), as Odin
does; else each mesh sorted whole by its middle. The context asks for 4.3
first, 3.1 then (NVIDIA gave 3.1 as asked). The buffer, 4 fragments a pixel
at first, grows when a frame fills it. Left: MSAA (the lists are a
pixel's).

Shader programs (November 2026): each game's variants listed in
cache/shaders/<game>.txt as they are met and compiled before its first
frame; each program's binary (OpenGL 4.1 program binaries) kept in
cache/shaders/<variant>.bin with the driver's name. AIRTRIX's 81 programs
ready in 0.5 s at its second start.

Dot patterns (501, November 2026): the meshes after a 501 are drawn on the
pixels of each 4 x 4 square of the screen that its pattern keeps (the 32
patterns of the Xbox 360 port, as Odin's author Réda Chérif-Touil found
them), at the board's pixel size whatever RENDER_SCALE; a call keeps the
caller's pattern, its return takes it back. PHARRIER's and BRAVEFF's
shadows, solid black before, are dotted, the ground seen through; AIRTRIX's
combo labels come and go in dots; SWRACER's far scenery comes in dotted.
Left: not compared with a board video yet; in widescreen the squares start
at the 3D's left edge, not the 4:3 picture's.

Spotlights: a spot's direction goes from the light to the scene, a
directional light's to the light. Its cone is its 451: the low 16 bits the
cosine of its edge (/32768), the high byte eeeemmmm the width over which
its light rises, (32 - m) / 32 * 2^-e, in cosine (AIRTRIX's skater).
Left: whether the rise is linear.

Implement framebuffer blending (instructions 781, 181; PHARRIER). Done in
Hikaruloader for the two framebuffers that swap every frame: the blend stays
in the front buffer, the next frame's back, so the old pictures fade out
over several frames (front = back x factor + front x (1 - factor), the
factor the old picture's share): the motion blur of PHARRIER's intro and of
BRAVEFF's party scene, as on the real board; FF holds the picture. The 2D
layers' ABGR1555 pixels go two to a 32-bit word, the left one in the high
half (BRAVEFF's and SWRACER's WARNING screens, SGNASCAR's attract texts). A
layer shows when enabled (1A0001B4). The 3D and the layers are stacked in
the order of 1A0001B8 & 3, bottom to top: 0 layer 2, layer 1, 3D; 1 layer
2, 3D, layer 1; 2 layer 1, 3D, layer 2; 3 3D, layer 2, layer 1. From the
top down the first there gives the pixel, the bottom one otherwise;
1A0001BC bits 0-2 (3D, layer 1, layer 2) say whether a pixel is there by
its alpha (1) or by a colour not black (0), the scene's alpha being 1
wherever a mesh drew. This fixed SGNASCAR's HOW TO PLAY and DRIVING TIPS
(order 1: the 3D over a white layer 2, under the texts) and SWRACER's SEGA
logo, both white before; AIRTRIX's INSERT COIN goes under its skate scenes
(order 0), BRAVEFF's "I/O Board is Initializing" now shows. The
translucent meshes are drawn back to front (AIRTRIX's OHOSOHO sign: its
magenta behind a panel with the letters cut out). Done in Hikaruloader: the
meshes of poly types 5 and 6 blend by their material's mode (A81): by
alpha, times the scene, added by alpha (AIRTRIX's trick trails glow where
they meet the skater), or added over the scene dimmed by alpha. Poly type 4
(punch-through) is alpha-tested against its material's range (C81, 154:
7F..FF in AIRTRIX), opaque and drawn before the translucent meshes. Left:
AIRTRIX's OHOSOHO magenta (A81 mode 2) is at about 1.5 times another
emulator's; no board reference yet. The materials have no alpha. The viewports' clip
rectangles count their rows from the top, as their offsets do (SGNASCAR's
driver cards).

Figure out the effect of mesh LOD on texturing (normally, and SGNASCAR),
disable mipmaps when appropriate (instruction 0C1 maybe). Done in
Hikaruloader for SGNASCAR: 2C1 bit 14 makes the texcoords whole texels
instead of 12.4 (its hi-res LODs, driver numbers, checkers, "ENTRY DRIVERS").
Mipmaps (October 2026): in a frame of PHARRIER, SGNASCAR and AIRTRIX, every
texture's level 1 in TEXRAM is its level 0 halved (PHARRIER and SGNASCAR
within 2 of a 2x2 average; AIRTRIX's within 27, filtered otherwise, not
other data): none to disable there. All their texheads have 0C1 mode 1;
its A (0-8) and B (0, B, E, F) look like a LOD bias, B maybe signed (SGNASCAR
B, its crisp textures), not used yet (a fixed bias of -1 is).

Fogging: on, from the viewports' 621 (P and Q taken as 1/z where the fog
starts and where it is whole, linear in 1/z between), on the materials
without 881's z; the attracts show little of it, as the references do. Its
alpha byte and T are unknown.

VON4's player virtuaroid had black and cyan patches: the HUD's flat
materials (#000000, #00FFC6) committed over its own. Fixed: the material and
texhead indices of 083 and 0C3 are 15 bits, not 14 (VON4 sets its robot's
base at 5005 and its HUD's at 1004; its bases go up to 7832); the tables
hold 32768 entries.

Alpha testing (154), done: see the punch-through meshes above.

Implement proper model caching using VBOs. For now the frame's vertices go
to one buffer, in one upload (a buffer a mesh before: some 1400 a frame in
SGNASCAR).

The drawing on a thread of its own, done in Hikaruloader (THREADED_RENDERING,
on by default): at a frame's end the lists and what the drawing reads of the
machine (the GPU's framebuffer settings and layers, the layers' rows, the
TEXRAM's written slots) go to a thread with its own GL context, which draws
while the next frame is emulated: 6 to 40% more frames a second, a frame
later; the screenshots wait for the last frame.

Fix texture wrap/repeat (BRAVEFF, AIRTRIX do not work properly yet.)
Done in Hikaruloader: each texture's own wrap and mirror (2C1 bits 22-25,
U first); BRAVEFF's "INSERT COIN(S)" is no longer backwards. Also: ABGR1111
textures 2w x 2h (SGNASCAR's test menu font), and the projection centred on
the viewport's offset (that menu at its place).

Widescreen, done in Hikaruloader ([Display] WIDESCREEN, WIDESCREEN_2D): the
3D at 854x480; the viewports as wide as the screen (clip 0 to 640) get a
wider projection, their middle 640 pixels unchanged, the others keep theirs,
moved to the middle; the 2D layers 640x480 in the middle or stretched. Left:
the games' own culling (on the SH-4) leaves objects out at the sides, and the
3D backgrounds or fades made for 4:3 leave bands there; to see game by game.

A CRT's super resolution (1920x240) takes its real shape as ASPECT_RATIO and
a _STRETCH DISPLAY_MODE (SCREEN_SHAPE until November 2026, now ignored);
SCALING_FILTER = LINEAR averages the rows down to 240. DRAW_DISTANCE divides
561's LOD distance (the CP's choice of levels of detail, 055 and 005) and
the fog's 1/z (621) by its factor; the renderer already draws to 1e5, past
the viewports' far plane. Left: what each game gains, to see.

CRT presets and bezels, done in Hikaruloader (November 2026, from Odin's
picture_preset, picture_filter_gl and display_bezel): CRT_PRESET runs a
libretro .glslp's passes by 86Box's OpenGL rules (`src/hl/crt_preset.*`, the
reader, `tests/crt_preset_test.cpp`; `src/hl/crt_filter.*`, the passes), the
picture composed for them at its own size (scene_w x 480) instead of into
the window; Lola Retro's twelve presets in `data/crt`. CRT_FILTER is not
drawn with one (greyed in the settings): the preset's mask and ours would
beat. BEZEL (`src/hl/bezel.*`, `src/hl/presentation.*`, `tests/bezel_test.cpp`):
BLUR, the picture's edges spread over the sides at two of its mipmap levels,
dimmed (Odin's sides shader); IMAGE, Batocera's `artwork/bezels/<set>.png`
and `.info` (else the see-through middle), the picture laid out by
screen_layout in the bezel's screen as in a window of that size. Left: the
presets' tube stays 4:3 (their LR_FIT_43), a widescreen picture squeezed in
it; CROP_BARREL's bent edges are not drawn with a preset (the composition
goes to the preset's frame, unbent); the window's context is GL 3.1 and the
passes are GLSL 1.50, which NVIDIA's driver takes (checked on the RTX 3050):
Intel's and AMD's may refuse them, the preset then logged and not used
(asking the window for a 3.3 context would settle it); the larger presets'
cost on the Intel UHD, unmeasured; the bezel shows only beside a frame
narrower than the window, not above or below one; a game's bezel picture
is read when the game starts, not again if the file changes.

GL errors that abort a game, now and then (October 2026, Intel UHD): AIRTRIX
at frame 2633 on the HL_ASSERT_NO_GL_ERROR at the end of upload_layer (agent
mode, THREADED_RENDERING off; other runs go past 9000), BRAVEFF at 8177 on
the one after glClear in on_end_frame's threaded branch; with two instances
at once, crashes instead (AIRTRIX about 2400-2700 and 900, BRAVEFF about
6000-6300). Likely an error made earlier and read there, glGetError keeping
it until read: the NVIDIA cabinet logged GL_INVALID_ENUM after the interface
(ImGui). The 10-minute passes (November 2026) showed it is GL_OUT_OF_MEMORY
(AIRTRIX at 3478, BRAVEFF at 33138 on glBufferData in upload_frame_vertices;
the same in VON4 at 14122 with two instances at once), the KHR_debug callback
silent (no debug context). Since: the GPU out of memory no longer stops the
machine (hl::gl::check_error logs it with its place, the frame goes on, the
3D of a frame whose vertices found no memory not drawn); the vertex buffer
keeps the largest frame's size, given back to the driver and written into
each frame; the layers' textures keep their storage (glTexSubImage2D). It
still happens in BRAVEFF, on this PC's Intel UHD (driver 32.0.101.6881),
about once in 5000 to 25000 frames, at 60 fps too: the driver can allocate
nothing more (GL_OUT_OF_MEMORY in BufferData, then in FenceSync, as a debug
context, HR_GL_DEBUG=1, tells), then crashes in igxelpicd64.dll on its
drawing thread. Not ours to grow: the process's GPU memory stays at 150 MB,
the vertex buffer at 7.5 MB at most; no driver reset in Windows' log. A
debug context made it rarer (1 crash in 8 passes of 20000 frames, against 2
in 4), at 10% of the speed; deleting the replaced textures 3 frames later
changed nothing. AIRTRIX, SGNASCAR and SWRACER ran 10 minutes without it.
On this PC's NVIDIA (RTX 3050, taken since November 2026), BRAVEFF ran 4
passes of 20040 frames without it, nor any GL error. Left: another Intel
driver.


AIRTRIX
-------

Attract mode works, with its WARNING screen (Valkyrie's patch shortened it
by 656 frames), the SEGA and Hitmaker logos. In Hikaruloader the game plays,
its skater animated (below), with its sound (its attract plays on the second
sound board only).

Emulation wise, the biggest issue is with the player character: it is not
animated. Talking with the Demul guys, we figured out the following: the game
uploads matrix data to CMDRAM  48100000-48100E2x, along with commands, but we
don't know what device that data is for. It is written by the master SH-4 but
never reads them back, and there is strong evidence that the slave SH-4 does
not touch them ever. The matrices come along with a pointer to a GPU routine in
CMDRAM (in master-SH-4-address-space-format), which, guess what?, draws the
player character. I *think* they specify additional transformations to be
applied to the skater body parts, but I don't know yet how.

The table is in the area at 1500001C (48100000), written each frame by the
master SH-4 (AT:@0C0431B0): a word (2 or 3), the address of the routine
(B4102120 for 48102120), 28 bones (4x4, column-major, 48100008-48100707) and
their rest poses, inverted (48100708-48100D47). The routine has four copies
(frame & 3), written once in the rest pose (a T-pose); the hands and the
board are other meshes, drawn with the skater's matrix times their bone.
Done in Hikaruloader: the CP poses the routine's vertices as it calls it
(bone x rest pose x vertex, hikaru-gpu-cp.cpp, "Skinning"). Left: which
bone each vertex takes. Nothing in the vertices or in CMDRAM tells; each
takes the nearest bone segment of the rest pose, a guess that poses the
skater as the game does. The master reads back 48100E20 after each frame
(unknown).


BRAVEFF
-------

Attract mode works, and the game plays, with its sound. Its how to play's
texts and counters were wrong texels
while 0D1's panning moved all the texheads; it moves those with 2C1 bit 15
only (PHARRIER's: its sky's clouds), and BRAVEFF has none.
Its language follows the board's country: the BIOS's COUNTRY SETTING, byte 5
of the system settings it keeps in the MIE's EEPROM (two copies of 64
bytes, a CRC-16 each), which the Region setting writes. Its attract is silent
while byte 6, the BIOS's ADVERTISE SOUND, is 0 (a new board's), which the
Attract sound setting writes.

References:
 http://www.youtube.com/watch?v=rwzzj7dzxE8


Cyber Troops Virtual On 4
-------------------------

Not dumped yet. Dumped since: MAME's von4 set (Rev E). In Hikaruloader it
boots and plays, and two linked instances fight a battle of two (see the
communication board). Its robot's black and cyan patches are fixed (see the
GPU's 083 and 0C3).

Its card reader (November 2026), a Sanwa CRP-1231 with a stocker and a
printer (hl::CardReader), on the MIE's serial line: the game puts a program
at the Z80's 9000 that drives it through ports 09-0D (see hikaru-mie.cpp).
CARD R/W (GAME ASSIGNMENTS, the EEPROM's byte 32) is ON by default outside
the USA, whose menu hides it; without the reader, the game stopped at its
start on CARD R/W INITIALIZE, then the reader's error. Checked: the reader's
start (fonts, RETRY SET, INIT), a new card made by the terminal (GAME START
MODE VENDOR, free play: name, Virtual-roid, written and printed), that card
read at the Virtual-roid's choice, written at the sortie and at the game's
end, then given back; the terminal's transfer: the old card read, written
(no sortie left), printed and given back. A cancel with the card inside
answers S3 '4': with '0', the game calls the card stuck in the reader. Not
checked: the transfer's new card watched to its end, the game's own card
test (SETUP RS422, CARD RW TEST), the card reader cleaning menu.

Its "ERROR 7" after "OBSONG Load Err" is gone: its start lists "Sound OK"
(already in the 10-minute passes of November 2026; which fix did it, not
looked for).


PHARRIER
--------

Attract mode works, its intro's motion blur too; the blue frames of its demo
are gone. The game plays, with its sound, and two linked instances play its
twin cabinet game. Its meshes change their material and texhead in their middle (the
hexagon's silver arms, the rails, the characters' shoes: their soles): a
recalled material or texhead now splits the mesh. The LOD jumps compare the
models' size on the screen with their sizes (I): the characters take their
finest meshes, rounded shoes; the scale is a guess (see 055).

The attract's asteroids (frames 1100-1450) show notches in their outline
and one is bean-shaped: their models' own outlines (November 2026). Each
is a closed mesh in the ROM (1412 triangles for the large one, 130 to 226
for the others), every edge shared by two triangles when the strips are
built as the pivot bit says (it keeps the fan's first vertex: no other
rule closes them), all with the same matrix; projected by hand, its
triangles draw the same outline. Not missing triangles, culling, LOD or
depth.

Brightness (October 2026): the board's video (below) looked brighter on the
stage's metal, near-white silver against our mid grey. Frame for frame it is
not: matched to ours (94.0 s to frame 5882, the hexagonal pod; 107.5 s to
frame 6975, the rings), the luminance agrees within 3% on the pod (median 93
against 100) and 5% over the rings frame. The capture (VGA to S-video) lifts
the darks and flattens the contrast (a fit of ours to it: gain 0.7-0.96,
offset +3 to +38), which reads as silver. Tried on the pod and rejected,
each far too bright (+22 to +55 levels): 291 doubled in texture mode 2 (its
7F as 1), 691 doubled, the specular's exponent divided by 8; two-sided
lighting changes little (+11). 691 at 80 = 1 also turned AIRTRIX's orange
foot-pad icon yellow: FF = 1 stays. The pod's lit materials: 091 #FFFFFF,
291 and 691 7F to E5, specular 40 with shininess 04 to 11, the stage's light
directional (0D4 AC). Left: the colours, which the capture desaturates.

References:
 http://www.youtube.com/watch?v=_kED3wt6p90
 http://www.youtube.com/watch?v=VCVnNBkeRX8


SWRACER (PODRACE)
-----------------

Fully dumped as MAME's swracer set. The game plays in Hikaruloader, with its
sound, two linked instances racing each other. Attract mode works: the
static mesh precision of its 901 instructions (above 8D, larger units) made
a negative shift, and the machine aborted at frame 1812. Its WARNING screen
needs the layers' pixel order (left pixel in the high half of each word).
The SEGA logo is meant to come up
mirrored: a character turns it round.

The viewports' near plane is at 0.01: a projection's depth, 24 bits, then
steps by z^2 / near / 2^24, 0.015 at 50 units, and the pod engines' panels
and decals z-fought (saw teeth on the gold panels, white specks at the
cones; attract frames 2380-2420). Done in Hikaruloader: the fragments' depth
is log2 (1 + z) / log2 (1 + far), a step of about z / 2^20. The meshes'
depth bias (101, every polytype) moves them nearer, as a 32-bit float
depth buffer's bias would: its low 10 bits the amount, bit 10 the mode.
Clear, a constant: 1/z up by about amount * 2000 / 2^24 / ln 2 of itself
(2000 units of the depth's last bit). Set, a slope: 1/z up by its slope
across a pixel times amount * 6 / 256. It was a polygon offset of the
whole 16 bits before, punch-through and translucent only: PHARRIER's
translucent meshes take 0x400, no bias at all, which moved them nearer.
With it SGNASCAR's tracks show their white lines (they were hidden), AIRTRIX's
skater casts his whole shadow, and BRAVEFF's far windows (204, 409) no longer
show through the buildings in front: the old offset, constant in 1/z over a
0.01 near plane, grew far too large far away.

The "/" of "AND/OR" on the WARNING screen is not in the game's font: the
BIOS's loader copies the font from the ROM board (master 0C3EBxxx), 8x8
cells of 16-bit pixels, and the "/" cell is all 7FFF (white, alpha clear),
the mark of the font's empty cells (its first two are the same). The text
is put together by the master (0C013336) in the framebuffer's rows 480-487
and copied to the layer: nothing there to draw a slash with. Not an
emulation bug.


SGNASCAR
--------

Attract mode works, and the game plays: its wheel and pedals, its drive
board's force feedback, its sound, two linked instances racing each other.
The original version (sgnascaro) runs as well.

The BOOTROM (all version) reads from invalid memory regions when loading the
game data from ROM to RAM; that is likely a game bug.

References:
 http://www.youtube.com/watch?v=N2rPqafQr84


GAELCO (src/mach/gaelco)
------------------------

ATV Track, Gaelco Football (REF. 020419/020201) and Smashing Drive (REF.
010131): two SH-4s, an NEC PowerVR Neon 250 on the slave, an Altera FPGA.
From MAME's driver (atvtrack.cpp), which runs none of them. The machine is
there: its memory maps, the shared RAM and ATV Track's boot loader (taken
out of the FPGA's configuration), the four NAND chips, the control
registers (the slave held until port A and register 0 let it go), the 32
kHz DAC and its interrupt, the inputs behind port A (an ADC0834-like serial
ADC for the wheel and pedals), Smashing Drive's data flash (its settings,
programmed AMD's way), the states. The window stays black.

Both boot to their main loops (3000 frames): the master loads the slave's
files from the NAND through the shared RAM's mailbox (1C0-1FC: the slave's
commands at 1E0, e.g. 23 a read of 200 bytes, the master's answer at 1EC),
the slave sets the Neon 250 up (its PCI BARs: 18000000, 14000000) and
starts a frame every other frame, the DAC starts (ATV Track at frame 710,
Smashing Drive at 320). Smashing Drive's MMU and operand cache RAM work.
Gaelco Football stopped at its first frame on ICR = FF80 (reserved bits,
now dropped), then waited for the SCI's TDRE: its slave now writes there,
the log shows it, "SH4 I/O at 60661 baud", sets a "PMX" up ("PMX appears
to have woken up", "the test CCB failed") and loads its microcode; black.

To do, in order:
 - check what was guessed: the DAC's IRL pins ("IRL5"), the ADC's protocol
   and channels, JP8's game inputs, the Neon 250's vblank (its interrupts
   7, 8 and 15 are unmasked: 7 the frame drawn, the others unknown);
 - the Neon 250: how the slave hands it its lists (registers at 14000000,
   VRAM at 18000000), then a PowerVR 2 renderer (the Dreamcast's family:
   the tile accelerator's lists, ISP/TSP, twiddled and VQ textures, the
   translucent polygons sorted, the modifier volumes);
 - the NAND kept in the game's nvram (settings, bookkeeping);
 - Smashing Drive: its MMU is on from the start (identity pages of 1 MB
   over the RAM and the data ROM, the store queues onto the control
   registers), so its CPUs run in the interpreter: the translations to
   x86-64 under the MMU, for speed;
 - the RS-422 link (DB9), the lamps and the motion outputs.
