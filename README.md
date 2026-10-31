Hikaruloader (formerly Valkyrie)
================================

Hikaruloader emulates SEGA's Hikaru arcade board on Windows: Air Trix, Brave
Firefighters, Planet Harriers, NASCAR Arcade, Star Wars: Racer Arcade and
Cyber Troopers Virtual-On 4. It is the njz3 fork
(https://github.com/njz3/hikaruloader, formerly njz3/valkyrie) of Valkyrie,
Stefano Teso's Hikaru emulator, whose own notes close this file.

<table>
<tr>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/airtrix.gif" width="240" alt="Air Trix"><br>Air Trix</td>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/braveff.gif" width="240" alt="Brave Firefighters"><br>Brave Firefighters</td>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/pharrier.gif" width="240" alt="Planet Harriers"><br>Planet Harriers</td>
</tr>
<tr>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/sgnascar.gif" width="240" alt="NASCAR Arcade"><br>NASCAR Arcade</td>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/swracer.gif" width="240" alt="Star Wars: Racer Arcade"><br>Star Wars: Racer Arcade</td>
<td align="center"><img src="https://raw.githubusercontent.com/njz3/hikaruloader/main/docs/media/von4.gif" width="240" alt="Cyber Troopers Virtual-On 4"><br>Cyber Troopers Virtual-On 4</td>
</tr>
</table>

The games' attract modes, captured on Hikaruloader.

Hikaruloader, Copyright (C) 2026 Benjamin Maurin (njz3).
Valkyrie, Copyright (C) 2011-2013 Stefano Teso.
Released under the GNU GPL v3 or later: see LICENSE.

This file is the user's manual. To install or build the program, see
[INSTALL.md](INSTALL.md); what each version brings,
[CHANGELOG.md](CHANGELOG.md); what is left to emulate,
[TODO.md](https://github.com/njz3/hikaruloader/blob/main/TODO.md), in the sources.
The version shows in the window's title and in the settings' About tab.


What it needs
-------------

- Windows 10 or 11, 64-bit, and a graphics card with OpenGL 3.1.
- The program's folder: `hikaruloader.exe` and `data/`, from a
  release archive or built from the sources ([INSTALL.md](INSTALL.md)).
- The games, as MAME romsets: the game's zip (`airtrix.zip`), its parent's
  for a clone (`airtrixo` needs `airtrix.zip` as well), and `hikaru.zip`, the
  BIOS. Merged, split and non-merged sets all work: the files are found by
  their CRC32. Folders of unpacked files (`<game>/`) still load.
- For NASCAR Arcade's drive board, optionally, Sega Rally 2's set
  (`srally2.zip`): its EPR-20512 then runs on an emulated Z80. Without it the
  board is simulated, and the force feedback works the same.


The games
---------

| Set | Game | Cabinet |
|---|---|---|
| `airtrix`, `airtrixo` | Air Trix (Rev A, original), 2000 | Four speakers |
| `braveff` | Brave Firefighters, 1999 | Two speakers |
| `pharrier` | Planet Harriers (Rev A), 2000 | Linked, four speakers |
| `sgnascar`, `sgnascaro` | NASCAR Arcade (Rev A, original), 2000 | Wheel and pedals, force feedback, linked, four speakers |
| `swracer` | Star Wars: Racer Arcade, 2000 | Linked, four speakers |
| `von4` | Cyber Troopers Virtual-On 4: Force (Rev E), 2001 | Linked, four speakers; its card reader is not emulated |

The picker also lists Gaelco's ATV Track, Gaelco Football and Smashing Drive
(six sets), which run on another board: they do not work yet, their screen
stays black. How far each game goes, and what is still wrong, is in
[TODO.md](https://github.com/njz3/hikaruloader/blob/main/TODO.md), in the sources.


Starting a game
---------------

Started without arguments, Hikaruloader shows its picker: the games of the
games folder, each marked Ready, Incomplete (some files missing) or Not
found. The arrows (or a controller's D-pad) choose, Enter (a controller's A,
or a double click) launches, Tab opens the settings, Escape quits. The games
folder is the one of the settings (Picker tab), else `roms` beside the
program.

In a game, Escape goes back to the picker; when the game was started from
the command line, it quits. F9 quits at once.


Command line
------------

```
hikaruloader.exe [options]
```

| Option | What it does |
|---|---|
| `-R <folder>` | The games folder for this start: the MAME zips, or a folder per game |
| `-r <set>` | Starts that game at once, by its MAME set name (`-r sgnascar`), without the picker |
| `-l <file>` | Loads that save state at the start: the game goes on from its frame. `-l 0` (any number) loads the game's own state, the one F4 saves |
| `-n <frames>` | Quits after that many frames |
| `-L`, `--list` | Lists the games and quits: title, set, version, year and maker, the MAME sets each one needs, each ROM file with its CRC32, and how many of them the games folder holds |
| `--write-dir=<folder>`, `--write-dir <folder>` | Writes everything in that folder: settings, log, saves (see "Write directory") |
| `-v` | More messages in the log |
| `-h` | Shows the options |

For example:

```
hikaruloader.exe -R D:\roms -r airtrix
hikaruloader.exe -r sgnascar -l D:\states\race.hlstate
hikaruloader.exe -L -R D:\roms
hikaruloader.exe --write-dir=C:\Racecab\Saves\hikaruloader -r swracer
```


Keys and controllers
--------------------

| By default | Action |
|---|---|
| F5, F6 | Test and Service. Test opens the game's test menu: its settings, its bookkeeping, the BIOS's system settings |
| 5, 6 | Coins |
| Enter, a controller's Start | Start |
| The arrows, a controller's D-pad or left stick | Up, down, left, right |
| Left Ctrl, Left Alt, Left Shift, Z; a controller's A, B, X, Y | Buttons 1 to 4 |
| A controller's left stick and triggers, a wheel's axes | Steering, accelerator, brake (the arrows too, all the way) |
| Space | Pause |
| F4, F1 | Save, load the game's state |
| Tab, a controller's Back | Settings |
| F12 | The frame rate over the picture |
| F9 | Quit |
| Escape | Back to the picker, or quit |

The Controls tab of the settings binds each action to keys, to the
controllers' buttons and axes (a wheel and its pedals included, with their
dead zones), several to each. The bindings go to `controls.ini`, written at
the first start with a comment on each line, in the syntax of linuxloader and
windowsloader. Its buttons can be numbered from 1, as Windows and vJoy do.


Settings
--------

Tab opens the settings, in the picker and in a game. Save writes them to
`hikaruloader.ini` beside the program, each with a comment giving its values
and its default; the file can be edited by hand as well. A few take effect at
the game's next start, as the window says.

**Display.** Window, full screen or borderless full screen (both at the
desktop's resolution: no mode change); the window's size, 1x to 4x 640x480;
the aspect ratio: the picture in its shape with black bars, the same at
whole multiples only (sharp pixels), or stretched to fill the screen. Also
v-sync, the frame limiter and its rate, the frame rate shown (F12), the
frame number in the title (to report a glitch), the mouse pointer hidden.

- *Screen shape*: the aspect ratio places the picture by the screen's shape,
  by default that of its resolution (square pixels). On a CRT driven at a
  super resolution (1920x240, say), choose its real shape, 4:3 or 16:9: the
  black bars then fall right, and a 4:3 picture is no longer stretched on a
  16:9 screen.
- *Scaling filter*: Sharp, or Smooth. Down to 240 rows on a CRT, Smooth
  makes each row the mean of two, where Sharp drops one in two (thin lines,
  small texts).
- *Draw on a thread of its own* (on by default): each frame is drawn while
  the next one is emulated. Faster when the drawing slows the game, but the
  picture comes a frame later.
- *Widescreen 3D (16:9)*: the 3D is drawn at 854x480 and seen wider; its
  middle stays as the game draws it. The 2D (texts, logos) stays 4:3 in the
  middle, or is stretched to 16:9 for a game that needs it. The games were
  made for 4:3: objects may be missing at the sides, or appear late.
- *Draw distance* (x1 to x4): the scenery's detailed models and its fog go
  that many times farther. What a game leaves out itself is not drawn.

**Sound.** The speakers: the sound card's, or mono, stereo, 2.1, quad, 5.1,
7.1. The games with four speakers play their front on the main sound board
and their rear on the second one; on two speakers the rear comes at the
front, 3 dB lower. Also the front/rear balance and the volume.

- *Attract sound*: the BIOS's ADVERTISE SOUND. A new board has it off, so
  the attract is silent; ON or OFF are written to the game's EEPROM at its
  start, AUTO keeps the EEPROM's.
- *Latency* (20 to 200 ms, 50 by default): the sound kept ahead. Less brings
  the sound closer to the picture; more avoids gaps on a slow PC.
- *Sound emulation*: Optimized (the default) or Interpreted, slower. Both
  give the same sound.

**Emulation.**

- *Region*: the board's country (Japan, USA, Export, Australia, Korea),
  written to the game's EEPROM at its start; AUTO keeps the EEPROM's. The
  games take their language from it: Brave Firefighters speaks English
  outside Japan.
- *SH-4 clock* (20 to 200 MHz, 57 by default, at the game's start): the
  board's 200 MHz is spent mostly waiting, which costs real time on the PC;
  57 MHz keeps every game above 60 frames a second. The games' timers count
  in real time whatever the clock.
- *SH-4 processors*: the recompiler (the default) or the interpreter, the
  same machine either way. *Memory access*: the same, three ways.
- *Fake the MIE*: Valkyrie's old shortcut, without the I/O board. Leave it
  off.

**Picker.** The games folder, the language (English, French; Automatic
follows Windows), the log file.


Saves and files
---------------

- `nvram/<game>.nvram`: the board's EEPROM (the game's settings, the BIOS's
  system settings) and its battery-backed RAM (records, bookkeeping), saved
  by themselves: the EEPROM half a second after the game writes it, the RAM
  every ten seconds when it changed, both when the game ends. Deleting the
  file gives the game a new board's settings back.
- `<game>.hlstate`: the save state of F4, which F1 loads. It holds the whole
  machine; a state may not load in another version of the program.
- `hikaruloader.ini`, `controls.ini`: the settings and the controls.
- `hikaruloader.log`: the messages of the last start (the one before in
  `hikaruloader.old.log`). On by default; the Picker tab's "Write a log
  file" (`LOG_FILE`) turns it off from the next start. Join it to a bug
  report.
- `data/games.json`: the list of the games and their files.


Write directory
---------------

A write directory keeps everything the program writes in one folder, so that
the rest of the disk can stay read-only: a cabinet under Windows' Unified
Write Filter, or games on a network share. Give it with `--write-dir`, or as
`WRITE_DIR` in the `[Picker]` section of the `hikaruloader.ini` beside the
program; a relative path is relative to the program's folder.

The settings, the controls and the log then go to its root, each game's
nvram and states to `<folder>/<game>/`. When it lacks the settings, the
controls or a game's nvram (its first start, or after a wipe), the usual file
is copied there, and from then on only the copy is used: after an update,
delete the copy to take the new file. If the folder cannot be created or
written to, nothing is saved at all, the usual files are only read, and a
notice says so for 20 seconds.


Linked cabinets
---------------

Planet Harriers, NASCAR Arcade, Star Wars: Racer Arcade and Virtual-On 4 can
link two cabinets or more. Each cabinet is one Hikaruloader, the instances
joined in a ring over the network, as the boards' optical fibres were: the
`[Network]` section of `hikaruloader.ini`.

- `LINK`: the BIOS's COMMUNICATION MODE, `MASTER`, `SLAVE`, `RELAY` or
  `NO_LINK`, written to the game's EEPROM at its start (`AUTO` keeps the
  EEPROM's; a new board has `NO_LINK`). One master per ring.
- Each instance listens on `PORT_IN` for the previous one, and connects to
  the next one at `ADDRESS_OUT` and `PORT_OUT`; the last one connects back
  to the master.
- Two on one PC, each with its own write directory: the master with
  `PORT_IN = 1970` and `PORT_OUT = 1971`, the slave the other way round.
- Star Wars: Racer Arcade and Virtual-On 4 want a different seat on each
  cabinet, in the game's test menu (SEAT NUMBER, SEAT LETTER); NASCAR Arcade
  and Planet Harriers number their cabinets by the ring.

The linked instances run frame by frame together: when one pauses, the
others wait, and they drop the link after 5 seconds; when one enters its test
menu or quits, at once.


Force feedback
--------------

NASCAR Arcade's drive board turns the game's commands into force feedback on
the first wheel or joystick that has it (DirectInput): a constant force,
with the servo board's spring and friction. With `srally2.zip` among the
games, the board runs Sega Rally 2's own program (EPR-20512) on an emulated
Z80; without it, a simulation of that program (`DRIVEBOARD_SIMULATED=1` in
the environment forces the simulation). It starts in some 11 seconds (CF,
CE, CD, then CC), as on the cabinet. Two environment variables adjust
it: `FFB_GAIN`, a percentage (100 by default), and `FFB_INVERT=1` for a
wheel that pushes the wrong way.


When something goes wrong
-------------------------

- A game is missing or Incomplete in the picker: `hikaruloader.exe -L`
  lists the files each game needs, and which ones the games folder lacks.
- The game is slow: keep the SH-4 clock at 57 MHz and the drawing on its
  own thread; the frame rate (F12) shows the gain.
- The picture is a frame late: turn off the drawing on its own thread.
- The attract is silent: set the Attract sound to ON.
- The sound crackles: raise the latency.
- The game asks for its settings at each start, or shows an error about its
  EEPROM: check that the program can write its `nvram` folder (or the write
  directory).
- Anything else: `hikaruloader.log` tells what happened.


Valkyrie
========

The notes of Valkyrie, from which Hikaruloader comes, kept for reference:
its own README, then its instructions for Linux. They describe Valkyrie, not
Hikaruloader: to install Hikaruloader, see [INSTALL.md](INSTALL.md).

Valkyrie, Copyright(c) 2011-2013 Stefano Teso
Released under the GPL v3 License.


DISCLAIMER
----------

Valkyrie is an experimental SEGA Hikaru emulator for GNU/Linux. It was
written entirely by reverse engineering the little Hikaru software available.
As of now, this Hikaru emulator is unstable, incomplete, primitive, slow,
crude, and sometimes hacky.

Until the time comes, Valkyrie should be considered a ``technological
preview'', and is really targeted towards developers rather than users.
If you are not interested in developing valkyrie, please consider waiting
until it becomes stable and usable enough.

Valkyrie is licensed under the GPL v3 License, see the LICENSE file.


CURRENT STATUS AND FUTURE DEVELOPMENTS
--------------------------------------

At the present moment, valkyrie is able to run all known Hikaru BOOTROMs
(versionsi 0.84, 0.92, 0.96) and the existing complete ROM dumps typically up
to attract mode. 3D emulation is almost complete, modulo bugs.  Please see
[TODO.md](https://github.com/njz3/hikaruloader/blob/main/TODO.md) for hints as to where the biggest emulation issues are.

You are free to clone this repository and do your own work on your branch,
provided that you respect the license. If you do work on valkyrie, please
contact me so we can possibly coordinate our efforts.

You are free to make use the information contained in this source for your
own project. If you make use of this work in any way, you may want to give
credit where it is due. You are also encouraged to keep your emulator open
source, so that other developers (and their users) can benefit from it. You
are also encouraged to contribute back the technical findings you may
stumble into.


ACKNOWLEDGMENTS
---------------

Thanks to the MAME developers for the hardware description and the initial
memory map annotated in their skeleton Hikaru driver (as of 2011.) Also
thanks to the MAME, nullDC, and lxdream developers for keeping/making their
software open source; part of valkyrie is based on their efforts. And thanks
to the Demul developers for the information they contributed.


Valkyrie's original instructions (Linux)
----------------------------------------

Make sure you have development packages for SDL and libjansson installed.
Under Debian (and likely Ubuntu) it's as simple as:

```
sudo apt-get install libsdl1.2-dev libjansson-dev
```

Then just do:

```
cd path/to/valkyrie
make
```

This will generate the bin/valkyrie binary. To run the bootrom with valkyrie,
type:

```
bin/valkyrie -R $PATH_TO_ROM_DIRECTORY -r hikaru_0.92
```

If you want to run a game, you have to also enable a hack that fakes the
IO board a little bit better, as follows:

```
MIE_HACK=1 bin/valkyrie -R $PATH_TO_ROM_DIRECTORY -r airtrix
```

If you forget to enable the hack, the game will either get stuck, or endlessly
display an "I/O board initializing" screen.

If you are a developer, and want to display on screen the contents of the
frame buffer or texture RAM, to see what gets uploaded, then set the
`HR_DRAW_FB` or `HR_DRAW_TEXRAM` environment variables, as follows:

```
MIE_HACK=1 HR_DRAW_TEXRAM=1 bin/valkyrie -R $PATH_TO_ROM_DIRECTORY -r airtrix
```

You can also install valkyrie for your user with:

```
make install
```

This will compile and install a copy of the valkyrie binary in `~/.local/bin`
and a copy of `vk-games.json` (the game description list used by valkyrie) in
`~/.local/share/valkyrie`. After this step, you can just do:

```
valkyrie -R $PATH_TO_ROM_DIRECtory -r $ROM_NAME
```

from anywhere in your filesystem.

XXX: due to the (dumb) way the loading code is designed, the ROM_DIRECTORY
is expected to contain a sub-directory for each game, containing the
uncompressed ROM images _and_ a copy of (or a symlink to) the BOOTROM image.
The latter should be named `hikaru_0.X.bin`, where X is the bootrom version,
that is, one of 84, 92 or 96. I *know* it is very rough, patches are welcome.

XXX: while valkyrie can run all three BOOTROM versions, it only supports
loading games using version 0.92; so make sure you are linking version 0.92
in the ROM directories!
