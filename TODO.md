What is left to do
==================

Only what remains: what each version brought is in
[CHANGELOG.md](CHANGELOG.md), how the parts work in the code's comments and
in [DEVELOPMENT.md](DEVELOPMENT.md); the findings behind the work done are in
this file's history (git).


GENERAL
-------

- Hikaru, hikcheck's C.R.T. TEST PAGE#2 (page 1, its colour bars and "31KHz", is right): a quad over the
  whole picture shows the 64x64 font's top left corner (texhead 0, 12.4 texcoords 0-1E1, 0-161)
  magnified some 20 times; the texhead committed after it (16x16, repeated, whole texels) would make
  a 16-texel grid of those coordinates, but nothing in the list recalls it (its 0E8 headers have
  bit 16 set, the glyphs' not; taking texhead 1 for that bit did not give a clean grid).
- Atomiswave: the wheel games' force (maxspeed, ftspeed): read as their manuals wire it and
  checked in Faster Than Speed's WHEEL FEEDBACK TEST, not yet felt on a real wheel; the
  motor's own lag (its belt, the wheel's weight) is not modelled. The guns' factory aim (a
  blank NVRAM) is off by up to 100 pixels, each game its own: their GUN ADJUST sets it;
  Extreme Hunting 2 shows its monitor test's patterns for ever: it wants the ALL.Net
  board and its PIC (01000000-0100011F, "G2IFSOJ AM", MAME's notes), not known enough to
  emulate (MAME patches the game's RAM). The trackballs read a stick's speed, not a mouse's moves.
  Ranger Mission's GUN TEST's "trigger" goes 0 and 1 whether the trigger is pulled or not,
  as its guns' Light-Gun modes come (one every 15 frames or so): the shots register in its
  GUN ADJUST and Sports Shooting's calibration; what that line shows, to see.
- Controls: the mahjong panel, the card readers' own switches and Club Kart's view button
  are not placed (data/games.json).
- NAOMI, seen in a pass over the 259 sets (3000 frames, attract only):
  - the SWP hopper board (hopper): its RESET switch (TURN ON AND OFF RESET SW.) is on
    the 837-14381, its input not known;
  - the Star Horse sets (shorse*, shors2k1, shors2k2*, shorsep*, shorsepr*): SELF TEST
    says I/O BAD; they want the 837-13785 ARCNET&IO board, of which nothing is known
    (no access on the G2 bus: it is not the same board as the medal games');
  - World Kicks runs on an 837-13551 (its menus, its four sticks on channels 0 to 7),
    not its own Namco V226 board: its kick sensors (KICK, FRONT/BACK in its SWITCH
    TEST) would be channels 8 to 11 of the V226's twelve, the four players' buttons on
    player 1's switches (Flycast); roles for players 3 and 4's sticks and the kicks
    are missing;
  - vtennis and vtennisg stay black from frame 1324: their frame loop waits on a lock
    (bit 0 of 0C287E58) its end-of-render interrupt releases, and that frame's lists stop
    after the opaque one (no modifier volume or translucent list end), as the scene
    loaded then has bad data: its models' 80-byte elements give material indices far out
    of their table (0C0A30CE: reads at 389CFCD8, 17AF3670..., writes near 00000000).
    Not the cartridge (the GD-ROM version does the same, the M2 board's ROM as MAME's),
    nor the recompiler (the interpreter does the same), nor the instruction cache; to
    find by comparing the RAM with a reference at the scene change;
  - spawn, hmgeo, alienfnt want a linked cabinet in their factory settings (blank EEPROM):
    their own test menus change it; mushi2k62 and rhytngk black at frame 2400.
  - f355twin, to check against a real board: its music (a stream on the master) plays in
    the attract, the menus and Race mode, not in Training (voice guidance there); its
    screen board plays the race's effects only; its first 50 s are silent (the boot: a
    TMU wait and three JVS chain enumerations the game asks for); no engine in the attract.
- NAOMI: the translucent modifier volumes change no picture in the attracts
  of Virtua Striker 3, Club Kart, The King of Route 66 and Airline Pilots:
  to see in play; per-pixel translucency only. The Elan's vertex lists take
  no time and its interrupt output is not wired: neither is documented.
- Net DIMM games: their network commands answer, Virtua Fighter 4 reaches
  its network check. Not yet playing: Dragon Treasure waits for its
  satellites on the network, its medal mechanisms' ports on the 837-14381 are
  not known (bit 6 of 01004024 clears its error screens: its RESET switch,
  probably), Mahjong MJ starts its network, World Club Champion Football
  waits for its server (192.168.102.1 port 59200, its "control process",
  which speaks first and is not in any set: what it sends is not known); the
  DIMM firmware update discs read no firmware version.
- Cycraft cabinets: the motion PC is answered, its motion data ignored (nothing
  moves); no game played to the end with it yet.
- An event-driven scheduler. The machine runs a line at a time (544 a
  frame): each SH-4 a line's cycles, then the MEMCTL's DMA, the GPU, the
  MIE and the AICAs.
- Speed: measure the six games against Odin again on an idle machine. The
  last numbers (November 2026) were taken with each game run beside Odin
  on a machine busy with a build: ours 0.74 to 0.99 of Odin's frame time.

The outputs (`src/hl/outputs.cpp`):

- Brave Firefighters' menu is the developers' one, with no OUTPUT TEST:
  outputs 3, 5 and 6 (likely player 2's nozzle and the revolving lights)
  not seen; the game's own B.F.F. TEST MENU not reached.
- NASCAR Arcade's OUTPUT TEST lights START BUTTON and VIEW CHANGE BUTTON in
  turn (outputs 1 and 4) whatever the cursor: which is which is a guess.
  Output 14 stays on, unnamed.
- Virtual-On 4's program has LAMP and COIN SHUTTER items, not in its OUTPUT
  TEST here (the VO4 terminal's, likely).
- Air Trix's LOCK MECHA. reads OFF in the test with output 4 at 1; it stays
  at 0 in the attract and after Start.
- The outputs in the agent protocol (shared with Supermodel and sm2-emu).

The telemetry (`src/hl/telemetry-games.cpp`), what its decoders lack:

- NASCAR Arcade: checked on Talladega alone (152 segments a lap; the other
  tracks' and the track's number not found, nor the lap's length, hence no
  gaps); the end of a race won (only time up seen); the contacts between
  cars; AUTOMATIC or MANUAL.
- Star Wars: Racer Arcade: Tatooine alone (400 segments a lap); the boost's
  bit not seen set in the checks; no jump driven (airborne, landings); the
  hover height's rest; the shaker's level against a deluxe cabinet's; the
  laps (4 taken, never driven to the end); the opponents' places are
  worked out from their progress.
- ATV Track: the riders' places worked out from their distance along the
  stage (the game shows none); the second rider seen on screen once only.
- Smashing Drive: the damage (8C22BF44 probably, a count that followed the
  HUD's bar at two crashes, its range unknown; 8C20144C is the last hit's
  strength), the fares; a linked game's other taxi.
- Gaelco Football and the other games: no decoder (not racing games).

Star Wars: Racer Arcade's force feedback made from the telemetry
(`src/hl/ffb-effects.cpp`): tuned on recordings in agent mode only, not yet
felt on a wheel nor on a game controller; the landings not tried (no jump
driven); the bass shaker's level against a deluxe cabinet's.

The controls (`src/hl/controls.cpp`):

- The devices' profiles: none shipped yet (`data/devices`): to make from real
  wheels, pedals, shifters and sticks, with *Save as a profile...*.
- `BACKGROUND_INPUT` (true by default): vJoy is read with another window in
  front, but its axes and buttons reached no game while a Windows Security
  window (PickerHost.exe, a credential prompt) was in front; why not known.

- The controllers' hotkeys (Guide + a button) are checked on SDL's virtual
  controller only: to try on real pads (an Xbox pad's Guide through XInput,
  which the Game Bar takes too; a DualShock's PS button). A joystick SDL
  has no mapping for has no Guide, hence no hotkeys.
- The wheel's rotation (WHEEL_ROTATION, WHEEL_RANGE) is checked on a
  virtual wheel, not felt on a real one; SDL 2 cannot read the turn a
  wheel's driver is set to. The servo board's friction is left as it was.


HIKARU
------

- The slave SH-4's accesses to the GPU, to move into hikaru-gpu.cpp
  (Valkyrie's note); the slave's MEMCTL is not run (never seen used).
- The MIE: its other ports (40, 80, 81, 84); the interrupts it routes to
  the SH-4's IRL2 (GPIO B, C, F); GPIO E's bits 0-3, which the BIOS waits
  for at boot (Valkyrie's toggle kept). Its clock: 16 MHz (MAME's figure;
  some 13.5 measured on a NAOMI).
- The I/O boards running their own programs (`IO_BOARD_PROGRAM`), what was
  guessed (as in Odin, whose notes these are):
  - Test and Service go to the I/O board only in that mode (on both boards
    the BIOS took one press for two);
  - the inputs read every millisecond of the board's time; a coin closes its
    switch 50 ms, then leaves it open 50 ms;
  - 837-13551: its 14.7456 MHz crystal taken as the MIE's UART's; the
    TMP90C844's registers where MAME has them, T01MOD and T23MOD as MAME
    reads them; timers 0 and 2 ask for nothing; INTEH bit 2 enables 0028 and
    bit 0 0038; the serial interrupts' enables and IRFH's bits but bit 2;
    P5 reads its latch, P3's latch at 1 after the reset, ADMOD F7 converts
    AN0 to AN3 over and over; INTWD waits for the interrupts to be open; P3
    bits 0-1 read 3;
  - 837-13741: its 315-6146 at the MIE's clock; vector F8 its UART, FE its
    timer every 1 ms (the D71054's wiring and its first pulse); port 91 reads
    FF; ports 40, 60, 80, 81 and 0E written at the start, and 90 read, only
    logged;
  - a simulated board answers at once (no reply delay).
- The drive board: the servo board's spring and friction are guesses; the
  analog channels per game (0, 1 and 2 take the wheel, accelerator and
  brake in every game); the force feedback's settings in the settings
  window; a network output of the command for a real drive board.
- The AICAs:
  - the RTC: at frame 1584 from power-on the BIOS showed 15 s more than the
    frames' time (the samples before the first frame?);
  - the levels against a real board's (no recording yet);
  - PHARRIER's SH-4 streams data across the start of the DSP's ring
    (7DFFC0-7E073F) at times, a 5 ms noise once: kept as the registers say,
    to check on a board;
  - they never interrupt the SH-4s in the attracts (MCIPD bit 5 enabled,
    never set); SWRACER and SGNASCAR restart their drivers 4-5 times in
    4000 frames, AIRTRIX once: why.
- The EEPROMs (`SECURITY_CHIPS`): their writes are not kept from a session
  to the next (no write seen); whether the X76F100's SDA floats once
  deselected (it reads high, as in MAME), and whether a read goes past the
  data's end; the Actel's other status bits, what bits 30, 1 and 0 of its
  control do, its register 0C at power-on (19620217 kept).
- The communication board, simulated (`COMM_BOARD = SIMULATED`): its
  program's work is guessed (the status words' bits after the link, the
  68000's RAM at 1F000 the games read as zeros, the registers 00 and 40).
- The communication board's program run (`COMM_BOARD = PROGRAM`):
  - SGNASCAR with more than 2 cabinets (up to 8), its relay (the monitor);
    PHARRIER and VON4's battle with the program: not tried;
  - two PCs: the hops over a network take longer than the fibre's 0.25 ms
    (HR_COMM_LATENCY to try; 16 lines already halve VON4's frames a turn
    with 4 cabinets);
  - the cost: a ring runs as fast as its slowest instance, four instances
    saturate an 8-core laptop; shared memory between the instances of one
    PC would spare the TCP sends; the spins (HR_COMM_SPIN,
    HR_COMM_SEND_SPIN) to tune on a quiet PC;
  - on Windows, SO_REUSEADDR lets two instances listen on the same PORT_IN
    (a ring of 4 took a stale instance's time): SO_EXCLUSIVEADDRUSE, if a
    quit instance's port can then be taken again at once;
  - Odin's guesses stand: the 68000 at 10 MHz, IRQ5's counter, a frame held
    1 ms at most, the status registers' 05;
  - save states keep the board but not the frames on their way: a cabinet
    loaded goes out of step;
  - the seats are set in each game's test menu: no Network tab in the
    settings window.


SH-4
----

- The translations: T in the host's flags; registers kept across chained
  blocks; DIV1, ADDC and the FPU's doubles written out. Odin's master SH-4
  still takes less time than ours (SWRACER about 0.57 ms a frame, ours about
  0.8).
- Fastmem without its bit map test: the devices through the faults, the
  faulting code patched to call the slow path.
- Unwind information for other hosts than Windows: the recompiler is off
  there until then.
- The busy waits that are neither idle loops nor timer polls (AIRTRIX's
  loop on the MIE's mailbox, AT:@0C69B358), run forward, to make 200 MHz as
  fast as the default 100.


GPU / RENDERING
---------------

- The far clipping planes the games give: their meaning (the scenery is
  drawn to 1e5 instead).
- 161 (P): AIRTRIX's runs of 20-27 pushes before its skater, of unknown use.
- Shading: texture mode 3 unknown; 091 (diffuse) falls through to 291, as
  in Valkyrie; the poly types "shadow A/B" (103 types 2, 3) never seen.
- The lights' ambient level (051): BRAVEFF's burning wall stays brighter
  than on the board (42 against 32); the D bit (BRAVEFF's reticles' light,
  not off).
- A parallel light with a null direction (lights every side): not seen in
  the attracts' frames compared.
- The depth queue (621 D): its types 2, 4 and 6 (a density) not done.
- Spotlights: whether the rise from the cone's edge is linear; its width
  (451's eeeemmmm) differs from Odin's by 13% at most.
- The translucent meshes sorted at each pixel, and the depth bias's two
  families: not with MSAA (the lists and the eye depths are a pixel's).
- The dot patterns (501): not compared with a board video; in widescreen the
  4x4 squares start at the 3D's left edge, not the 4:3 picture's.
- Blending: AIRTRIX's OHOSOHO magenta (A81 mode 2) looks too bright, no
  board reference yet.
- Mipmaps: 0C1's A and B look like a LOD bias (B maybe signed), not used (a
  fixed bias of -1 is).
- Fog (621): its alpha byte and T unknown.
- Model caching in VBOs: the frame's vertices go to one buffer, uploaded
  once a frame.
- Widescreen: backgrounds or fades made for 4:3 leave bands at the sides, to
  see game by game. The games' own culling on the SH-4 is widened where its
  code is recognised (src/cpu/sh/sh4-wide-view.cpp); BRAVEFF and Gaelco
  Football have no such routine; NAOMI is not covered. DRAW_DISTANCE: what
  each game gains, to see.
- CRT presets and bezels: the presets' tube stays 4:3 (their LR_FIT_43), a
  widescreen picture squeezed in it; CROP_BARREL's bent edges are not drawn
  with a preset; the window's context is GL 3.1 and the passes GLSL 1.50,
  which Intel's and AMD's drivers may refuse (a 3.3 context would settle
  it); the larger presets' cost on an Intel UHD, unmeasured; the bezel
  shows only beside a frame narrower than the window, not above or below
  one; a game's bezel picture is read once, at its start.
- BRAVEFF on this PC's Intel UHD (driver 32.0.101.6881): about once in 5000
  to 25000 frames the driver can allocate nothing more (GL_OUT_OF_MEMORY in
  BufferData, then FenceSync) and crashes in igxelpicd64.dll. Not seen on
  NVIDIA: to try another Intel driver.


AIRTRIX
-------

- The skater's skinning: which bone each vertex takes (nothing in the
  vertices or in CMDRAM tells; each takes the nearest bone segment of the
  rest pose, a guess that poses him as the game does). The master reads
  back 48100E20 after each frame (unknown).
- Its outputs and its MIE mailbox wait: see GENERAL and SH-4.

Board video: https://www.youtube.com/watch?v=vAet7ncKqqs


BRAVEFF
-------

- Its outputs (see GENERAL), and the Intel driver's crash (GPU).

Board video: http://www.youtube.com/watch?v=rwzzj7dzxE8


VON4 (Cyber Troopers Virtual-On 4)
----------------------------------

- The card reader: not checked, the transfer's new card watched to its end,
  the game's own card test (SETUP RS422, CARD RW TEST), the card reader
  cleaning menu.
- Its battle of four with the communication board's program, and its LAMP
  and COIN SHUTTER outputs (see HIKARU and GENERAL).


PHARRIER
--------

- The colours against the board's: the board videos' capture desaturates
  them (the brightness agrees within 3-5% on matched frames).
- The level of detail's scale is a guess (see 055).
- Its twin cabinet game with the communication board's program: not tried.

Board videos:
 http://www.youtube.com/watch?v=_kED3wt6p90
 http://www.youtube.com/watch?v=VCVnNBkeRX8


SGNASCAR
--------

- More than 2 linked cabinets with the communication board's program, and
  its relay (see HIKARU).
- Its START and VIEW CHANGE lamps told apart (see GENERAL).

Board videos:
 http://www.youtube.com/watch?v=N2rPqafQr84
 https://www.youtube.com/watch?v=U1GAuk3Uuh8


GAELCO (src/mach/gaelco)
------------------------

ATV Track, Gaelco Football and Smashing Drive play, two Smashing Drive linked.
Still to do:

- The renderer: mipmaps generated, not read; the background plane's colour
  alone; no modifier volumes, the texture formats 3, 4 and 6 and the palettes
  (none of the three games uses them).
- Interrupt 7 (a frame drawn) is raised at once, not when the frame would be.
- Widescreen: the 2D told from the 3D by its depth (the HUD at a 1/W of 1
  or more, the backgrounds at 1e-6, the 3D below 0.2 in both games); ATV
  Track's and Smashing Drive's culling is widened on their slaves (0C006CB6,
  0C00348C); Gaelco Football's objects' test (8C0048B4, k 0.8) did not run in
  the attract mode, and is left as it is; dimmed 4:3 boxes stay 4:3 with the 2D centred; Gaelco Football lacks a
  billboard's piece on the right.
- Fog: the table's, per pixel, not yet compared with a real board's
  pictures. The three games use the table and no fog only, and never the
  colour clamp: the vertices' fog, the table's "mode 2" and the clamp are
  drawn as the PowerVR 2 manual says, checked by forcing them on the games'
  polygons only.
- The force feedback's strength on a real wheel (its way checked in ATV Track's
  INPUT/OUTPUT test and Smashing Drive's OUTPUT TEST: the motor holds the wheel
  in the middle).
- ATV Track's and Gaelco Football's link, if any: only Smashing Drive's menu has
  one; ATV Track's motion outputs.
- Gaelco Football's ball sensors: its ball test (sensors, power, height,
  direction) shows them at 0. The game reads port A's 3800 once a frame,
  which nothing answers; no bit of it held changes the test.
