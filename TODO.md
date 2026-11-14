What is left to do
==================

Only what remains: what each version brought is in
[CHANGELOG.md](CHANGELOG.md), how the parts work in the code's comments and
in [DEVELOPMENT.md](DEVELOPMENT.md); the findings behind the work done are in
this file's history (git).


GENERAL
-------

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
- Widescreen codes (`widescreen` in games.json): those of the sets not at
  hand (initd, initdo, initdexpo, initdv2*, initdv3j*, toyfight, lupinsho,
  doa2m, beachspi, cspike, monkeyba, zombrvn, gunsur2) not checked, their
  `from` taken as the same kind's; Slash Out's and Spawn's left out (what
  their words hold not known); Jambo! Safari's narrows the view in height
  (the same width seen, closer) where 18 Wheeler's widens it. The games
  without codes (Crazy Taxi, F355, Club Kart, The King of Route 66...) keep
  their 4:3 view with what they send beyond its sides.
- Controls: the mahjong panel, the card readers' own switches and Club Kart's view button
  are not placed (data/games.json).
- NAOMI, seen in a pass over the 259 sets (3000 frames, attract only):
  - the SWP hopper board (hopper): its RESET switch (TURN ON AND OFF RESET SW.) is on
    the 837-14381, its input not known;
  - the Star Horse sets (shorse*, shors2k1, shors2k2*, shorsep*, shorsepr*): SELF TEST
    says I/O BAD; they want the 837-13785 ARCNET&IO board, of which nothing is known
    (no access on the G2 bus: it is not the same board as the medal games');
  - World Kicks' FRONT/BACK sensors (p1:7 to p1:0, active low, its SWITCH TEST's), left
    closed as Flycast has them: what they see on the cabinet is not known;
  - alienfnt looks for its link at each start (160 s, START skips it): its settings have
    a master and a slave, no setting without the link;
  - mushik4e and mushik2e (MKG TKOB): ERROR 100 RFID READER NOT READY, they want the RFID
    reader/writers on Maple ports B and C (838-14243);
  - f355twin, to check against a real board: its music (a stream on the master) plays in
    the attract, the menus and Race mode, not in Training (voice guidance there); its
    screen board plays the race's effects only; its first 50 s are silent (the boot: a
    TMU wait and three JVS chain enumerations the game asks for); no engine in the attract.
- NAOMI: the translucent modifier volumes (per-pixel translucency only)
  are not yet seen changing a picture in any game. In play too, those that
  send them cover no translucent polygon they could change: Club Kart (82
  volumes a race frame, 8 shadowed polygons) and Virtua Striker 3 (6, 192
  shadowed) with one parameter set each and the chip's shadow scaling off
  (FPU_SHAD_SCALE bit 8 clear), The King of Route 66 (23) with none
  shadowed; Airline Pilots sends none in flight. The Elan's vertex lists
  take no time and its interrupt output is not wired: neither is
  documented.
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
- An event-driven scheduler (not urgent: no game is known to need it; the
  troubles of the line's grain were mended where they showed, VON4's DMA
  copied at its start).
  - Now: each machine runs a video line at a time, some 30 us. The Hikaru,
    544 lines a frame: the master SH-4 a line's cycles (some 3000 at 100
    MHz), the slave, the MEMCTL's DMA and the GPU's command processor, the
    MIE's Z80, the two AICAs (ARM7 and timers), the communication board.
    The NAOMI the same on each board (the servo board each millisecond),
    several boards in slices of 8 lines, a thread each; Gaelco's 525
    lines.
  - The scheduler: each part says when it next acts (a DMA's end, a
    timer's overflow, a serial port's next byte, an interrupt, the Z80's
    answer); the CPUs run up to the next event, it is done, and again: the
    grain a cycle, not a line.
  - What it would bring: a DMA or an interrupt done when due, not at a
    line's end; the CPUs (master and slave, SH-4 and ARM7, SH-4 and Z80)
    in step closer than a line; the timers, serial ports and performance
    counters exact; the CPUs' waits skipped in general, not by each game's
    loop found.
  - What it costs: the machines' loops rewritten, each part scheduling its
    events; the JIT left at the cycle due (it updates its count at a
    block's end); the NAOMI's slices between threads; the save states'
    layout. Every game compared frame for frame before and after; maybe
    slower (more exits from the blocks).
  - In steps, when a game needs it: 1. a queue of events within the
    present loop for the DMAs and the timers, the CPUs' grain the same; 2.
    the lines cut at the next event; 3. the JIT.
- Speed: AIRTRIX and VON4 slower than Odin. Measured on an idle machine
  (2026-11-14, mains power; the best of 5 runs, frames 600 to 3600 of the
  attract, no speed limit, each in its window: Odin's frames by ODIN_SPEED,
  ours by the agent's step): AIRTRIX 8.31 ms against Odin's 6.55 (1.27),
  VON4 4.22 against 3.40 (1.24), SWRACER 3.59 against 3.35 (1.07);
  SGNASCAR 4.75 against 5.16 (0.92), BRAVEFF 4.52 against 5.73 (0.79),
  PHARRIER 4.49 against 5.76 (0.78). Ours varies more from run to run
  (VON4 4.2 to 8.5 ms). To find: where AIRTRIX's and VON4's frames go
  (HR_PROFILE), the drawing or the CPUs.

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
- Crazy Taxi: the money earned and the customers delivered (no ride driven
  to its end in agent mode: the taxi stopped short of the stop's zone, by a
  wall, and one put inside it at a standstill kept its customer; to check
  in a game played by hand), the driver, the surfaces (the wheels' ground
  ids not named), the traffic; the hits worked out from the velocity (the
  game flags none), the velocity sent as the mean of two frames (it swings
  across the taxi from a frame to the next, against a wall the most).
- Jambo! Safari: checked in the first area (Expert, Beginner), one capture
  (Beginner's); not seen: Expert's net, the next areas, a continue, the
  name entry; the rope's length (the HUD's distance) and the crates'
  animals not found; no jump in the game (no airborne).
- F355 Challenge: checked on f355twin and f355 (Motegi, AT, the paddles on
  the twin, the H stick on the deluxe, Training, Driving and Race); the
  surfaces, the Grand Prix and the other courses' laps, the cars' count
  not found; f355twn2 checked at Atlanta (its H stick not tried: its
  neutral taken as the others'); the opponents' scale (0.95 to 1.24 a
  car) not understood; the prototypes not checked.
- 18 Wheeler: stages 1 and 2 and the parking played to their ends (the
  timer held by a RAM write on the long drives), stages 3 and 4 reached by
  RAM writes, their rival not driven to; the place against the rival is
  worked out along the truck: in stage 2 the rival takes another road and
  reads behind while it leads (the route's block, position - 34, could
  tell: the rival's own not found); the rival's bonus has no fixed
  address; the other sets' stage 2 rival found by its code, not run; the
  world 1.7 times as fast as the speed the game shows.
- The King of Route 66: AREA 1's first two stages; a hit flag, the
  wheels, the rival's speed not found; the nitro only seen ordered by a
  RAM write (B+18: the voice's word is not recognised in the emulator),
  its pickups never met; kingrt66p's boot through the test menu not
  tried.
- Club Kart: Beginner races on two courses, clubk2k3 one on Type A; no
  speedometer, no engine (a value with the speed alone), the wheels, the
  hits; the Prize sets: a GOAL with no money, the Double Up, clubkpzb's
  second credit (its shields), the meters' addresses after another history
  (the same in every state seen); clubk2k3's prototypes not in
  games.json.
- Initial D Arcade Stage: initdexp in Legend of the Streets (AE86, Myogi,
  Usui, Happogahara), no card; the race's laps (the course's screen only),
  a hit flag, the wheels' grounds, Time Attack, a card in the reader not
  checked; initdv3e on Myogi and Irohazaka (its other courses' ids not
  seen); the finish told from a time up only when seen (a state loaded
  in the results says not finished); initd, initdv2j and initdv3j not
  tried.
- Tokyo Bus Guide: checked on the first course's practice part; a stop
  served (the doors, the score), the other courses, a hit flag, the
  traffic lights not found; its motor board's forces not played (the
  answers alone); the game takes no start in its first attract cycle
  (some 3 minutes after the power-on).
- Gaelco Football and the other games: no decoder (not racing games).

Star Wars: Racer Arcade's force feedback made from the telemetry
(`src/hl/ffb-effects.cpp`): tuned on recordings in agent mode only, not yet
felt on a wheel nor on a game controller; the landings not tried (no jump
driven); the bass shaker's level against a deluxe cabinet's. Crazy Taxi's
the same way: its scale (`FfbScale`: 36 m/s, 50 m/s² hard turns, bumps
past 60 m/s²) from rides in agent mode, not felt yet.

Jambo! Safari's deluxe drive board (`src/hl/driveboard-jambo.cpp`): its own
EPROM not known. Scud Race's (EPR-19338A) runs on the Z80, the game's
commands being of that family (checked: the OUTPUT TEST's rolls; in play 25
and 11, 20 and 22 roped, 5x and 6x the rope's pull, 5A 6A at a hit, 01 and
04 the ground changing, Cx the scenes). How Jambo's own program differs,
unknown; not felt on a wheel. The servo board 838-12912-01 (`DriveBoard`):
what FB does (taken as the torque's gain, as BackForceFeeder's
EPR-19338A docs guess it; the games lower it while the board pushes: Scud
Race's 20 at a hit, Jambo's 20 roped), FD (a damping?), FC 0-3 (its own
vibration, the 3x: played at 20 Hz, 6 to 94 % in 15 steps, as Supermodel
reads the two bytes; their real meaning, a strength or a frequency, not
known); to measure on a real board. Without the
scud set, the simulation's road effects are sines of EPR-19338A's measured
amplitudes and periods (its tables' shapes not known), and its torque by
the wheel's position is left out.

The SH-4's performance counters (`src/cpu/sh/sh4.cpp`): the elapsed time
only (event 23), the other events held; PMCLR taken as clearing when the
counter starts again, from the games' code (to check on a board, with a
program reading the counter after writing 2000).

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
