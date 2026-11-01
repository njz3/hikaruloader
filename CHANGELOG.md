Hikaruloader's changes
======================

What each version brings, for those who play. How the versions are numbered
and released: [DEVELOPMENT.md](DEVELOPMENT.md), "Versions and releases".


0.3.0 (not released yet)
------------------------

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
