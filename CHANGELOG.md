Hikaruloader's changes
======================

What each version brings, for those who play. How the versions are numbered
and released: [DEVELOPMENT.md](DEVELOPMENT.md), "Versions and releases".


Next version (not released yet)
-------------------------------

- Fixed: on NVIDIA graphics cards, 0.1.0 crashed when a game started.
- Fixed: NASCAR Arcade's HOW TO PLAY and DRIVING TIPS screens, and Star
  Wars Racer Arcade's SEGA logo, were all white: the 2D layers and the 3D
  now go in the order the game asks for.
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
