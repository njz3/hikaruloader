Hikaruloader's changes
======================

What each version brings, for those who play. How the versions are numbered
and released: [DEVELOPMENT.md](DEVELOPMENT.md), "Versions and releases".


In preparation
--------------

- The games folder changed in the settings of the game list: its games now
  start, where they were looked for in the folder of the program's start;
  the list shows them at once when the folder is added, its field left
  (Enter), Save or Scan again pressed, else when the settings close.
- NASCAR Arcade's view change button in the Controls tab: *Change view*
  (`ViewChange` in `controls.ini`, linuxloader's and windowsloader's name),
  V or a controller's Y by default; before, only Down changed the view, and
  a `ViewChange` binding was ignored.


0.4.1 (2026-11-03)
------------------

- NASCAR Arcade's gears: the cabinet's four-speed lever, for the game's
  MANUAL. Shift up and Shift down a gear at each press (Page Up, Page Down,
  a controller's R1, L1), or an H shifter's positions; neutral between
  gears and an H shifter's lever in neutral as in sm2-emu, in the Controls
  tab, which also shows the gear the game reads.
- Star Wars: Racer Arcade's levers, each the thrust of one of the pod's
  engines: *Combined* (the default), the accelerator gives the thrust and
  the wheel the turn, for a wheel and pedals or a controller; *Original*,
  an axis per lever as on the cabinet. Before, the wheel and the
  accelerator went to the levers as they were: the pod pulled to one side.


0.4.0 (2026-11-03)
------------------

- Faster: about twice as fast in the six games, the same pictures; as fast
  as Odin or faster here. The SH-4s' recompiler goes from one block to the
  next itself, compares the games' code with the memory only when it may
  have changed, keeps the processors' registers in the PC's through a
  block, and writes out the jumps and the floating-point arithmetic; the
  waits are found sooner and cost less (the slave's in the BIOS skipped at
  once, the delay loops and the waits on a byte taken by the recompiled
  code itself); the recompiler allocates almost nothing and looks memory
  up with fewer instructions; the ROMs and the sound boards' mailboxes are read in place. The
  drawing thread sends the graphics card only what changed from one mesh
  to the next and shows its frames itself, and gets the 2D layers' written
  rows alone. Odin's author, Réda Chérif-Touil, showed the way.
- The translucent polygons (smoke, glass, flames, lights) are blended from
  the farthest at each pixel, as on the board, right where they cross:
  *Sort the translucent polygons at each pixel* in the Display tab, on by
  default. It needs a graphics card with OpenGL 4.3, and no MSAA; else each
  mesh is sorted as a whole, as before.
- Fixed: NASCAR Arcade's scenes were darker than on the board: the lights'
  own ambient share is now taken (the board's video and ours now match on
  the track). Brave Firefighters' and Planet Harriers' scenes are a little
  brighter too.
- Fixed: Planet Harriers' far scenery now fades into the sky, as on the
  board, instead of standing out dark.
- The board's clock follows the PC's: the bookkeeping's dates are right,
  and a clock set in the test menu (CLOCK SETTING) is kept with the game's
  nvram.
- Fixed: Air Trix's ranking screens had a grey background, black and olive
  on the board: the games' fog now grows with the distance as on the board.
- The spotlights rise from their edge as the games' own tables give it (Air
  Trix's halfpipe a little brighter); a lit translucent colour above white
  keeps its excess (Planet Harriers' logo galaxy); the motion blur's trails
  die out as on the board; the finer models come at twice the distance.
- No more stutter the first time an effect shows: each game's shader
  programs are prepared before its first frame, and kept compiled in
  `cache\shaders\` (with a write directory, there), read at once the next
  time. The six games' lists come with the program (`data\shaders\`), so
  their very first start prepares them too.
- These, as Odin's author, Réda Chérif-Touil, found them.
- Fixed: what the games draw through a pattern of dots was drawn whole:
  Planet Harriers' and Brave Firefighters' shadows (solid black, now
  dotted, the ground seen through), Air Trix's combo labels coming and
  going in dots, Star Wars Racer Arcade's far scenery coming in. The dots
  keep the board's size at every internal resolution. The patterns were
  found by Odin's author, Réda Chérif-Touil.
- Fixed: Planet Harriers' asteroids were twice as bright as on the board,
  flat, with grey shade: the materials' ambient colour now tints and dims
  their shade, olive as on the board. The other games are a few percent
  darker where their materials have such a colour.
- Fixed: with the log on a network drive, Planet Harriers fell to 6-15
  frames a second in its attract's demos and in a game. Some games repeat
  a message at each frame (Planet Harriers some 80 a frame, Star Wars Racer
  Arcade up to 1300, Virtual-On 4 and Brave Firefighters a few): each is
  now written 8 times at most, and the log goes to the disk once a second
  at most.
- The Display tab's "Screen shape" is gone: on a CRT driven at a super
  resolution (1920x240), its real shape as the aspect ratio (4:3 or 16:9,
  black bars) in a stretched display mode does the same.
- Three and four linked cabinets: `[Network] COMM_BOARD = PROGRAM` runs the
  communication board's own program on its 68000, as on the cabinet
  (ported from Odin, by Réda Chérif-Touil), and the instances of a ring in
  step at a fraction of a frame. Star Wars Racer Arcade races with three
  and four cabinets, Virtual-On 4 links its four sites. The default stays
  `SIMULATED`, two cabinets as before; see "Linked cabinets" in the
  README.
- The romsets can be 7z as well as zips (`airtrix.7z`, `hikaru.7z`...), read
  with the LZMA SDK (public domain, built in): the picker and `-L` find
  them, merged, split or non-merged alike, a zip and a 7z of a set together
  too. A solid 7z is unpacked whole at the game's start.
- The controllers' hotkeys, as RetroArch has them: hold Guide (Xbox, PS or
  Home) and press Start to quit, B to go back to the picker, Back for the
  settings, R1 and L1 to save and load the state, X to pause, Y for
  Virtual-On 4's card; the game sees none of the controller's buttons while
  Guide is held. They are `controls.ini` bindings like the others
  (`GC0_BUTTON_GUIDE + GC0_BUTTON_START`), bound in the Controls tab by
  holding Guide. A `controls.ini` of an earlier version keeps its bindings
  as they are: bind them in the Controls tab, or delete the file for the
  defaults. Escape, back to the picker, is a binding too now (Emulator,
  Back).
- The wheel's rotation for NASCAR Arcade, as Odin has it: the Controls tab's
  Wheel rotation makes the game's wheel go from one end to the other over
  270, 360, 540, 900 or 1080 degrees of a wheel's turn instead of its whole
  turn (the default, as before), given the turn set in the wheel's driver
  (900 degrees by default); the force feedback's centring spring follows.
  `[Input]` `WHEEL_ROTATION` and `WHEEL_RANGE` in `hikaruloader.ini`.
- CRT presets (Enhancements tab, *CRT preset*): a CRT's tube, curved glass,
  scanlines, phosphor mask and glow drawn over the picture by a libretro
  shader preset. Lola Retro's are shipped, by Zoki (MIT licence), as Odin
  ships them: the cabinets' 29-inch monitor and 50-inch projector, and PC
  monitors of the VGA days from good to extreme, curved or flat. The CRT
  filter is greyed while a preset is chosen. None by default.
- Bezels (Display tab, *Bezel*), where the window is wider than the
  picture's frame: the picture's own edges blurred and dimmed, or the
  game's bezel picture with the game in its screen, in Batocera's format
  (`artwork\bezels\<set>.png` and its `.info`; a clone takes its parent's).
  None is shipped. Off by default, black as before.
- New, off by default, in the Emulation tab: *I/O board running its own
  program*. The JVS I/O board's processor runs its program as on the
  cabinet, instead of its simulation: the 837-13551's TMP90PH44 (a Toshiba
  TLCS-90) running 315-6215 (MAME's `jvs13551.zip`), NASCAR Arcade's
  837-13741 running epr-21868 (`18wheelr.zip`). Its JVS TEST shows the
  board's own answers (NASCAR's: "837-13741 I/O CONTROL BD2"); Test and
  Service then go through that board, as on a cabinet. Ported from Odin, by
  Réda Chérif-Touil.
- New, off by default, in the Emulation tab: *Security chips*. The ROM
  board's X76F100 secure EEPROM and the main board's 93C46 answer from
  their images (MAME's newer `hikaru.zip`), and the BIOS checks them, as on
  the board, instead of skipping the check; NASCAR Arcade's protection
  answers as its Actel FPGA does. The games play the same.


0.3.0 (2026-11-01)
------------------

- The cabinet's outputs, for the programs that drive real lamps, motors and
  wheels (MAMEHooker, BackForceFeeder, DOFLinx...), in MAME's network
  protocol as Supermodel, Flycast and sm2-emu send them: each game's lamps,
  under the names of its test menu, and NASCAR Arcade's drive board
  commands, which a real drive board can answer. Off by default: the
  settings' new Outputs tab, which also lists what the game sends, with the
  values.
- The picture's settings as in the other emulators (linuxloader,
  Supermodel, Flycast, sm2-emu), in the Display tab: the internal
  resolution, native to 6 times (3840x2880, 4K's class); the aspect ratio,
  original, 4:3 or 16:9 with black bars, stretched or cropped (the crop's
  position chosen, the cut parts possibly bent onto the edges); the display
  modes each also stretched; whole multiples only as a box of its own. A
  settings file from before keeps its choices.
- The Emulation tab's "Fake the MIE" is gone: it left the games without
  their controls, and every game runs with the board's own MIE.


0.2.2 (2026-11-01)
------------------

- The log no longer gets 800 to 2300 "ROMBD out-of-bounds" errors as a
  game starts: the game measuring its ROM board's chips, banks the board
  has not included, nothing wrong.


0.2.1 (2026-11-01)
------------------

- Microsoft Defender took 0.2.0's program for a Trojan
  (Bearfoos.A!ml, a false detection) and removed it: this version's
  program is a new build.
- Fixed: the log got a line at each frame of a game, which slowed the
  games down with the log on a network drive.


0.2.0 (2026-11-01)
------------------

- Fixed: on NVIDIA graphics cards, 0.1.0 crashed when a game started.
- Fixed: NASCAR Arcade's HOW TO PLAY and DRIVING TIPS screens, and Star
  Wars Racer Arcade's SEGA logo, were all white: the 2D layers and the 3D
  now go in the order the game asks for.
- Fixed: NASCAR Arcade's night race and its drivers' cards, and Star Wars
  Racer Arcade's text crawl, smeared by a motion blur that grew scene after
  scene: the ROM boards' protection is emulated, and the games no longer
  raise their blur.
- Fixed: surfaces showing through one another in the 3D, in saw teeth or
  specks: Star Wars Racer Arcade's pod engines, NASCAR Arcade's cars on the
  drivers' cards, Virtual-On 4's sword and lights, Air Trix's shadows, Brave
  Firefighters' distant windows. The 3D's depth is now as precise far as
  near.
- Fixed: NASCAR Arcade's tracks had lost their white lines, Air Trix's
  skater cast only part of his shadow, and far away Brave Firefighters'
  lit windows showed through the buildings in front: the games' depth
  offsets, which lay what is painted on a surface over it, are now read
  as the board reads them.
- Fixed: Planet Harriers stayed 15 seconds on a black screen after its
  WARNING: the processors' divisions done in double precision came out
  wrong, and the game unpacked its sounds a little at a time.
- Faster: about a third less processor time a frame in every game, the
  same pictures.
- Air Trix's protection chip is emulated: the game poses its skater itself,
  as on the board (his shadow his whole silhouette). It needs the SH-4
  clock at 100 MHz or more, now the default (57 before); a settings file
  from an earlier version keeps its clock: set 100 in the settings'
  Emulation tab.
- Planet Harriers' protection chip is emulated too: the check the game
  makes at its start passes as on the board, with no patch of the game.
- Fixed: a game stopped now and then on "GL ERROR: out of memory" (Intel
  graphics, after thousands of frames): the graphics memory is reused from
  frame to frame, and a frame it fails is drawn without, the game going on.
- On a laptop with two graphics cards, the NVIDIA or AMD one is used, not
  the integrated Intel one.
- Virtual-On 4's card reader: the game no longer stops at its start on the
  reader's error outside the USA region. A VO4 card, a file, goes in and out
  of the reader with F7; the game in its card vending mode makes new ones.
- Several games folders, separated by `;` (`D:\roms;E:\mame\roms`), looked
  in in that order: the BIOS can be in one, the games in another. The
  settings' Picker tab adds one with Windows' folder dialog and marks those
  not found.
- The games' pictures in the picker: yours, in `artwork\` beside the
  program; and, when the Picker tab says so (off by default), the Arcade
  Database's flyers and marquees, downloaded once into `cache\artwork\`.
- Enhancements, all off by default, in a settings tab of their own: the 3D
  drawn at 2 to 4 times its size, MSAA, FXAA, anisotropic filtering,
  textures enlarged by Scale2x, bloom, a CRT filter.


0.1.0 (2026-10-31)
------------------

The first version of Hikaruloader, the njz3 fork of Stefano Teso's Valkyrie.

- Windows, one program with no DLL to install; a picker of the games and a
  settings window, in English and French.
- The games as MAME romsets (merged, split or not), found by their CRC32:
  Air Trix, Brave Firefighters, Planet Harriers, NASCAR Arcade, Star Wars:
  Racer Arcade and Virtual-On 4 play, with their sound.
- Controls: keys, controllers, a wheel and its pedals, bound in the
  settings; NASCAR Arcade's force feedback, from its drive board.
- Linked cabinets: two instances race or fight together over the network
  (Planet Harriers, NASCAR Arcade, Star Wars: Racer Arcade, Virtual-On 4).
- Saves: each game's settings, records and bookkeeping kept as on the
  board; save states (F4, F1); a write directory for read-only cabinets.
- Display: window or full screen, aspect ratios, the screen's shape for
  CRTs driven at super resolutions, a smooth scaling filter, widescreen 3D,
  a longer draw distance, the drawing on a thread of its own.
- Sound: from mono to 7.1 speakers, a latency to choose, the board's
  effects.
