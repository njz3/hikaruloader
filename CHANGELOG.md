Hikaruloader's changes
======================

What each version brings, for those who play. How the versions are numbered
and released: [DEVELOPMENT.md](DEVELOPMENT.md), "Versions and releases".


Next version
------------

- Fixed: the NAOMI games got player 1's controls alone, buttons 1 to 4:
  player 2 (start, stick, buttons 1 to 6) and buttons 5 and 6 reach them now
  (Marvel vs. Capcom 2, the fighting games, the two-player cabinets).
- Fixed: the Atomiswave's player 2 pad sent nothing: its stick and buttons
  1 to 6 reach the games now (the wheel cabinets excepted).
- Fixed: Smashing Drive's wheel was the wrong way round (the taxi went left
  when turned right) and so was its motor: both checked in its INPUT TEST
  and OUTPUT TEST (WHEEL MOTOR holds the wheel in the middle), ATV Track's
  as before.
- Beach Spikers starts as a four-player cabinet on a blank EEPROM (its
  header names no players): a game with players 3 and 4 gets them.
- Sega Yonin Uchi Mahjong MJ's Update Disc Ver.1.007 (CDP-10002A) starts:
  its CD is in mode 2, whose sectors' data were read 8 bytes too soon (the
  game's file not found); it gets to its network wait, as the other discs.
- Mushiking The King Of Beetles 2006 Second no longer black after the NAOMI
  logo: the G1 bus's DMA gives its end address and length done (SB_GDSTARD,
  SB_GDLEND), by which the game counts the sectors it has read. Mushiking IV
  gets to its RFID reader's error, as Mushiking II.
- The SH-4's floating point rounds as the games set it (FPSCR: toward zero
  from the reset, the denormals as zeros), no longer always to the nearest:
  Virtua Tennis's numbers no longer a last bit apart; a few attracts run a
  little differently (Crazy Taxi, Sports Jam).
- Spawn and Heavy Metal: Geomatrix start alone on a new save: their own first
  settings made the cabinet a slave, waiting for its master (`LINK = AUTO`
  now gives such a game `NO_LINK`, as a new cabinet's owner would; the others
  keep theirs).
- World Kicks on its Namco V226 I/O board: the four players' sticks, buttons
  and kicks at the ball (`P1_Kick` to `P4_Kick`: a trigger, how hard), checked
  in its SWITCH TEST and in a match; new controls for players 3 and 4's
  analog sticks (`P3_StickX`...).
- Rhythm Tengoku no longer black after the NAOMI logo: an M4 cartridge
  reads FF past its chips, as erased flash (the game looks for a table of
  files at the top of the cartridge and stopped on none).
- Virtua Tennis (the cartridge and the GD-ROM) no longer black after the
  Sega logo: the G1 bus's DMA starts on a 32-byte boundary, as the board's
  (its scenes load in chunks of 66666 bytes, each one over the end of the
  one before).
- Virtua Athletics, Keyboard Maniax and Psyvariar 2 no longer stop at their
  start (invalid instruction 0000): the SH-4 runs 0000 as a no-op, as the
  board does (Virtua Athletics has one in the delay slot of a jump).
- Namco's I/O boards for their NAOMI cabinets: Ninja Assault (no longer
  I/O PCB ERROR: its two guns aim where they point, the trigger off the
  picture reloads), Mazan: Flash of the Blade (no longer black: the sword
  on the mouse, a quick swing slashes, its two sensors' boards), Gun
  Survivor 2 (the gun's yaw, pitch and roll, the trigger, the gun button),
  checked in their I/O tests.
- The Atomiswave's light guns (Ranger Mission, Sports Shooting USA, Extreme
  Hunting, Sega Clay Challenge) and trackballs (WaiWai Drive, Sega Bass
  Fishing Challenge's reel), on the EX. IO board's controller ports: a gun's
  aim latched where it points as the picture is drawn, its trigger, off the
  picture; their GUN ADJUST and GUN TEST work. The Maple bus's other
  instructions (reset, NOP, light gun) are one word, as the hardware has
  them.
- Device profiles: a model of device's bindings (a wheel, pedals, a shifter,
  a stick, a pad) put on a player's bindings the first time that device is
  connected, and what its *Default* and *Reset...* put back. *Save as a
  profile...* on a device's page writes one to `devices/` beside
  `controls.ini`, to share; *Clear...* removes all of a device's bindings.
  `data/devices/README.txt` gives the file's syntax.
- Players 3 and 4 (`[Player3]`, `[Player4]` in `controls.ini`: the third and
  fourth controllers; coins 3 and 4 on 7 and 8) for the four-player games:
  Power Stone 2 (its I/O board of four players), WWF Royal Rumble, Virtua
  Athlete, Beach Spikers and Mahjong MJ (their second I/O board, which
  answered nothing pressed), Guilty Gear Isuka and Dirty Pigskin Football
  (the Atomiswave's third and fourth pads).
- The Controls tab's *Levers* part shows with the games that have a pair of
  thrust levers (Star Wars: Racer Arcade), or no game running.
- The cabinets' other hardware is described in `data/games.json` instead of
  being chosen by the games' names or titles: the racing I/O board and its
  drive board, the servo board, the card readers, the medal hopper, the
  Gaelco cabinets' outputs; the log names them at the start.
- Fixed: Maximum Speed's and Faster Than Speed's wheel and pedals stayed at 0
  (their CONTROLER SETTING SCREEN, 0 mph in a race): they are on the
  Atomiswave's third controller port, not on the second. Their wheel's force
  goes to the network outputs too (`awffb`), and at its whole strength: the
  game's 15 is the motor's top (it was half), its WHEEL STRENGTH setting
  (OFF, LIGHT, NORMAL, HEAVY) as on the cabinet; the force's way checked in
  Faster Than Speed's WHEEL FEEDBACK TEST (the wheel pushed back to the
  middle).
- The joysticks, wheels and controllers are read in the background by
  default (`BACKGROUND_INPUT = true`): vJoy fed by another program, a front
  end in front. The keyboard still needs the focus.
- The Controls tab shows each game's own controls only (`uses` in
  `data/games.json`, from its INPUT TEST): NASCAR Arcade no longer lists six
  buttons and a second player. *What the game receives* shows each channel's
  value as the INPUT TESTs do (80H the middle) and the switches ON or OFF.
- Widescreen 3D (16:9) switched during a game: applied when the settings close,
  the window's picture 854 or 640 wide at once; the game's own view widened
  from the next time it computes it (Hikaru, NAOMI, Gaelco).
- A key or a button on the wheel or a pedal no longer sends it all the way at
  once, as in MAME: it goes there in 250 ms and back to the middle in 100 ms
  when let go, both held or turned the other way (`Axis_Key_Ramp`,
  `Axis_Key_Return` in `controls.ini`, 0 for at once).
- `--find <text>`: the games whose set, title or maker hold each word of the
  text, whatever the case (`--find "f355 twin"`); with `-L`, those in full.
- Airline Pilots' lamps on the network outputs, under the names of its
  OUTPUT TEST: start, flap up and down, view change, landing gear up and
  down. On the NAOMI cabinets of several boards, the outputs are the first
  board's only: the others' no longer put theirs over them.
- A game's own bindings: *Save these bindings for <set> alone* in the Controls
  tab writes `controls/<set>.ini`, read instead of `controls.ini` for that game.
- Airline Pilots' five analog inputs can be bound (control wheel's elevator and
  aileron, rudder pedal, thrust levers L and R, as its INPUT TEST names them);
  they were missing or fixed at an end.
- The controls unified: every game reads the same rows of `controls.ini`,
  whatever its machine (Hikaru, NAOMI, Atomiswave, Gaelco); `[Naomi]` and
  `[Gaelco]` are gone, `[Digital]` is `[Player1]` and `[Player2]`, the rows
  have their common names (`Steering`, `Accelerator`, `Coin1`, `Card`,
  `ShiftUp`, `P1_GunX`, `P1_Trigger`...). Which control is where on each
  game's board is in `data/games.json` (`layouts`, `controls`, `gearbox`).
  An earlier `controls.ini` is not read as it was: delete it for the new
  defaults, and bind again.
- The gear levers held in their positions emulated from Shift up and down
  or an H shifter's positions: NASCAR Arcade's H, F355's six-speed H (5 and
  6 new), 18 Wheeler's low, high and reverse, Crazy Taxi's drive and reverse
  (reverse new: `GearReverse`). Their switches no longer take a player's
  stick.
- F355 Challenge's controls as its INPUT TEST reads them: the wing paddles
  (Shift up and down), the assist switches SC, TC, ABS and IBS (buttons 1 to
  4: they were on the arrows, which also steer), the clutch (`Clutch`).
- Star Wars: Racer Arcade's levers on the keyboard too, in the original mode:
  A and D (`LeftLever_Digital`, `RightLever_Digital`).
- Air Trix's yaw and roll on an analog stick (its roll stayed at 00).
- `INVERT_STICK_Y` (was `PHARRIER_INVERT_Y`): every analog stick game's, a
  game's own setting to save for it alone.
- The games' own controls shown first in the Controls tab, under the game's
  names: the view change (it reached no NAOMI game: Initial D, King of Route 66, 18 Wheeler,
  Club Kart Cycraft, Wave Runner GP...), the gear lever (Initial D, 18 Wheeler,
  Crazy Taxi, Jambo! Safari), the flying games' controls (`[Flight]`: Airline
  Pilots, Strike Fighter), the light guns' sights, trigger and reload
  (Confidential Mission, The Maze of the Kings, Lupin the Third, Death Crimson
  OX), the analog sticks (Monkey Ball, Dynamite Baseball, World Series
  Baseball, Zombie Revenge, Soul Surfer, Shakatto Tambourine).
- Fixed: those analog sticks rested at one end, and Alien Front's pedals were
  swapped (its brake on the second channel, its accelerator on the third).
- The agent mode presses the gear lever (ShiftUp, ShiftDown), player 2's start
  (Start2) and the game's own controls: a flying game's (Elevator, Aileron,
  Rudder, ThrustL, ThrustR), the light guns' (GunX, GunY, Trigger, Offscreen, and
  GunX2... for player 2), the analog sticks' (StickX, StickY); `inputs` lists
  those of the game running.
- A game's own settings: *Save for <set> alone* in the settings writes those
  that differ from the common ones to `games/<set>.ini`, read over
  `hikaruloader.ini` for that game (aspect ratio, widescreen, force feedback's
  gains...); *Common settings* removes it.
- Fixed: Airline Pilots (deluxe, three boards) mostly stayed on "Please Wait
  until Slaves synchronize...", without a sound after the BIOS's chime; it
  starts every time now, its music and sounds with it. The boards of a cabinet
  see each other's link moves at the same moments each run.


0.7.2 (2026-11-11)
------------------

- Planet Harriers with a controller: the stick goes where the controller's
  stick is again (moved, the hero kept jumping from one edge to the other);
  `PHARRIER_INVERT_Y` stays.
- F8 on the Hikaru, one screen at a time: the picture of its second monitor,
  once a game drives it (hikcheck's two-monitor test), and back.
- A click in the game's window can capture the mouse (`CAPTURE_MOUSE`, the
  Display tab); Tab, Alt, the Windows key or another window let it go.
- NAOMI cabinets of several boards: every board's sound is heard, not only
  the master's.
- The Game tab's screens (one board, side by side, a window each, their
  monitors) apply when the settings close, without a restart.
- Fixed: the texts of hikcheck's two-monitor test ("MONITOR #1", "MONITOR #2",
  its count and "PRESS TEST BUTTON TO EXIT") were missing: the viewport a
  game goes back to after one it pushed is the one it had recalled.
- Fixed: Air Trix's copyright and trademark on its title were unreadable (the
  GPU's copies within the framebuffer kept the pixels' order in their words).
- NAOMI: the pictures drawn by a thread of their own with `THREADED_RENDERING`
  (on by default, as for the Hikaru), the boards' emulation going on meanwhile.
- NAOMI: the framebuffer a game shows without a 3D scene (its 2D screens) read
  some 6 times faster, 0.3 ms a frame instead of 2.
- Ferrari F355 Challenge: the deluxe cabinet's sound (its volume control,
  on the drive board's I/O board, turned up), and in a race the dashboard,
  the lap times and the track map on every version.


0.7.1 (2026-11-10)
------------------

- The settings' Game tab: the options of the game running, during a game
  (the screens of a cabinet of several, Planet Harriers' stick).
- Planet Harriers with a controller: the stick moves as Brave Firefighters'
  sights do, and its up and down can be inverted (`PHARRIER_INVERT_Y`).
- Alt+Enter: full screen or a window. The main keys in the About tab.
- NAOMI's M4 cartridges: their header read plain or through the decryption,
  for the BIOS of their region: Illvelo, Mamoru-kun, Shooting Love 2007 and
  Mushiking start.
- Initial D links from the first start of a new save (the machine restarts
  once by itself).
- F355 Challenge Deluxe, one screen at a time: the picture stays on the boards
  with a monitor (no more black screen).
- A game's stray pointer no longer takes the emulator down.
- Club Kart for Cycraft and Initial D Ver. 3 Cycraft Edition start: their boards
  are Net DIMMs and the cabinet's motion PC is answered by Hikaruloader.
- The Net DIMM's TCP service (port 10703), on `DIMM_LISTEN`: reading and
  writing the DIMM, peeking at the NAOMI, the host's control word. It listens
  on `127.0.0.1` unless `DIMM_SERVICE_IP` says otherwise.
- Capcom vs. SNK, Project Justice, Power Stone 2, Gundam: Federation vs. Zeon
  and Sega Tetris no longer stop during their attract: their cartridges' 64 KB
  decryption buffer had only half its size.
- Azumanga Daioh Puzzle Bobble and Quiz Keitai Q mode (and the games that go to
  sleep between two frames) no longer stay black: the SH-4 now wakes up.
- NAOMI: the cabinets with two I/O boards start (Virtua Golf and Shootout Pool
  with their trackball, on the first mouse; WWF Royal Rumble and Mahjong MJ with
  a second board for players 3 and 4). Virtua Athlete passes the check too but
  stops further on.
- The medal games' G2 expansion board and its backup RAM: Dragon Treasure gets
  past its G2 I/O error, as far as its network check, once the first boot has
  written its backup (start it a second time).


0.7.0 (2026-11-10)
------------------

- NAOMI and NAOMI 2 games, the cabinets of several boards included (F355
  Challenge Deluxe's three screens, Airline Pilots, Strike Fighter).
- Atomiswave games: 36 in the list, the wheel games with their force feedback.
- GD-ROM games on the DIMM board: Initial D Arcade Stage Ver. 3 Export plays,
  with its magnetic card (F7) and its wheel. The CHD goes in a folder named as
  the game, next to the zip.
- `[Naomi]` in controls.ini: NAOMI and Atomiswave's own controls.
- 18 Wheeler and F355 Challenge have their drive boards.
- Club Kart Prize plays with its coin hopper board: Coin 1 goes in its chute,
  and its credits are kept in the game's nvram file.
- The SH-4's instruction cache: the Hikaru's check ROM tests its RAM and shows
  its car on the road.
- `SECURITY_CHIPS` is on by default: the BIOS is no longer patched when the
  chips' images are in the set.
- The picker has the Hikaru and Gaelco games only for now. The others start
  from the command line, `-r <set>`, with `-z <zip or 7z>` for an archive of
  any name.
- `WIDESCREEN`: the games' own view limits are widened by recognising their
  code on the SH-4, without a patch at a fixed address for each game: clones
  are covered too.
- The Hikaru's 24 kHz modes fill the picture. Its second video output has its
  own window with `MULTI_SCREEN = WINDOWS`: the check ROM's two-monitor test.
- Initial D links two cabinets over `[Network]`, from the second start of a new
  save.
- The GD-ROM games on a Net DIMM board start: its firmware 3.17, its network
  settings and its sockets, reached through `[Network]` `DIMM_IP`, `DIMM_MAP`,
  `DIMM_PORT_OFFSET` and `DIMM_LISTEN`. Nothing leaves the PC by default.
- Dragon Treasure and Dragon Treasure 2 load: a GD-ROM's game is read from its
  high-density area whatever the PIC says.
- NAOMI: the BIOS of a region the game's header accepts, its latest revision:
  the Japanese games no longer stop on an Export BIOS.
- `MULTI_SCREEN = WINDOWS`: each window's monitor chosen from a list, by its
  name and its mode, or AUTO.
- The games of several NAOMI boards run faster: F355 Challenge Deluxe's four
  boards from about 40 to 56 frames a second.
- NAOMI games link over `[Network]`: F355 Challenge Twin and the others with a
  communication board, two Hikaruloaders on a TCP ring. `LINK_NODE`: a slave's
  car or seat.
- Driving Simulator: its three whole systems, on their ring, in one
  Hikaruloader (F8 or `MULTI_SCREEN` for its three screens).


0.6.2 (2026-11-09)
------------------

- Smashing Drive in widescreen: the game now sees the 16:9 width, no more
  black areas at the sides of the picture.


0.6.1 (2026-11-08)
------------------

- Star Wars: Racer Arcade in widescreen: the game now sees the 16:9 width, so
  the pods at the sides of the picture come whole, no longer in pieces or not
  at all.
- ATV Track in widescreen: no more black wedges of ground at the sides.
- NASCAR Arcade, Planet Harriers, Air Trix and Virtual On 4 see the 16:9 width
  too in widescreen.


0.6.0 (2026-11-08)
------------------

- The Gaelco games run at the SH-4 clock of the settings, no longer at twice it.
- The SH-4 clock is 150 MHz by default, 100 to 200 on its slider: some games
  show graphical glitches at 100.
- "(Paused)" in the window's title while the game is paused or the settings
  are open.
- The Controls tab's Default and Reset for a wheel that is not the first
  joystick: the defaults at its own number.
- New outputs for the tools that read them: `nozzle1` and `nozzle2` in Brave
  Firefighters; `shot1`, `bullet1`, `missile1` and `bomb1` in Planet Harriers.
- Star Wars: Racer Arcade: a controller's motors only for a boost at high
  speed and the hits, no longer all the time.
- *Force feedback device* in the Controls tab: a wheel or a controller's motors.
- Vibrations in two more games: a jolt of its own for each of Planet Harriers'
  weapons, and a gentle one while Brave Firefighters' nozzle sprays
  (`[ForceFeedback]` `SHOTS`, `MISSILES`, `BOMBS`, `NOZZLE`).
- Brave Firefighters and Planet Harriers play with one or two mice or light
  guns, each read on its own (`[Pointer]` in `controls.ini`). In Brave
  Firefighters, a controller's stick moves the sight, at a speed to set.
- A new configuration has the attract's sound on, the Export region, the mouse
  pointer hidden over the game and P for the pause.
- The meshes' depth bias as Odin reads it, two families that move only against
  themselves: SEGA NASCAR's shadows and track lines are whole, Star Wars: Racer
  Arcade's columns stand in front of its walls (not with MSAA).
- Star Wars: Racer Arcade: the pilots' arms joined at the elbow.
- Five save states in rotation: F4 always makes a new number 0
  (`<game>-0.hlstate`), the older ones move up to number 4 (`-4`). F1 loads the
  latest, or the one chosen with F2 (newer) and F3 (older), shown at the top of
  the picture. An older version's `<game>.hlstate` counts as number 0.
- The compiled shaders kept on disk are made again when their sources change.
- Star Wars: Racer Arcade's force feedback: each effect on the wheel has its
  own strength in `hikaruloader.ini` (`[ForceFeedback]`, `WHEEL_SPRING`,
  `WHEEL_ENGINES`, `WHEEL_IMPACT`...). The engines' vibration is 30% lower;
  the boost's, the hits' and the bumps' stay as they were.
- Star Wars: Racer Arcade's force feedback: the centring of the wheel now
  follows the engines' tilt from the cockpit, to the side they bank. The
  telemetry gives that tilt too (`engines_roll`).
- To debug the force feedback: *Show the effects over the game* in the
  Controls tab (`[ForceFeedback]` `HUD`, off by default) writes the spring,
  the friction, the force, the sine and the rumble at the top left.


0.5.4 (2026-11-08)
------------------

- Star Wars: Racer Arcade's left and right buttons where the Controls tab
  says: the two were swapped, as the game's input test showed.


0.5.3 (2026-11-08)
------------------

- Star Wars: Racer Arcade at full speed with a force feedback wheel: its
  forces go to the wheel on a thread of their own, the game no longer
  waiting for the USB (some 20 frames a second in a race on a SideWinder).
- A gain for all the force feedback, of every game, on a wheel and on a
  controller's motors: *Force feedback gain* in the Controls tab, 0 to 300%
  (`[ForceFeedback]` `GAIN`), felt at once.
- The controllers read in the background, the keyboard never: *Read the
  controllers in the background* in the Controls tab (`[Input]`
  `BACKGROUND_INPUT`, off by default), for a game playing behind another
  window.


0.5.2 (2026-11-08)
------------------

- ATV Track and Smashing Drive a third faster to emulate, for older PCs:
  their main CPU's wait for each frame, which polls the board, is no longer
  run turn after turn.
- ATV Track and Smashing Drive's fog: the far scenery fades into the fog's
  colour, as the PowerVR chip does it (it was never drawn: the fog table was
  read from the far end). The chip's other fog modes and its colour clamp
  are drawn too, for games that would use them.
- The Gaelco games in widescreen too (*Widescreen 3D (16:9)* in the Display
  tab): the 3D seen wider, the HUD at 4:3 in the middle or stretched to
  16:9 with the sky and the pictures (*2D in widescreen*).
- ATV Track's telemetry: the riders the game drives, with everyone's place;
  the distance along the stage counted from each stage's start.


0.5.1 (2026-11-07)
------------------

- The Gaelco cabinets' own buttons in a section of their own, *Gaelco
  cabinets* in the Controls tab (`[Gaelco]` in `controls.ini`): Volume up and
  Volume down (Page Up, Page Down), ATV Track's two brake levers, Smashing
  Drive's horn. ATV Track and Smashing Drive no longer read the Hikaru
  games' Up, Down and Buttons, where a cabinet's volume buttons also changed
  NASCAR Arcade's view; Smashing Drive's gas and brake are the pedals alone.
- Telemetry for motion simulators, dashboards and force feedback tools, as
  Supermodel and sm2-emu send it (`[Telemetry]`, off by default): a JSON
  line a frame on a TCP port, Forza Motorsport 7's "Data Out" datagrams for
  SimHub, FlyPT Mover, SimTools. NASCAR Arcade: the player's car and the 29
  others, gear, rpm, draft. Star Wars: Racer Arcade: the player's pod and
  the 11 others, each engine's thrust, the boost, the hits, the hover
  height, and the deluxe cabinet's bass shaker level. ATV Track and Smashing
  Drive: the vehicle, its jumps, the time left.
- Star Wars: Racer Arcade's force feedback, made from what the game does: on
  a wheel, the centring and the damping with the speed, the turns' weight,
  the kicks of the walls and pods hit, the scraping, the engines' rumble and
  the deluxe cabinet's bass shaker, the boost, the bumps; on a game
  controller, its two motors' rumble. `[ForceFeedback]` sets each family's
  strength.
- The agent mode drives the wheel, the pedals and Star Wars: Racer Arcade's
  levers (`set Steering`, `Accelerator`, `Brake`, `LeftLever`, `RightLever`),
  as sm2-emu's and Supermodel's.


0.5.0 (2026-11-07)
------------------

- Gaelco's games play: ATV Track, Gaelco Football and Smashing Drive, in all
  their sets, at their 60 frames a second, with their pictures, sound,
  controls and test menus. Their settings and scores are kept.
- Gaelco Football's player 2 has their rows in the Controls tab.
- Smashing Drive's two cabinets link, over `[Network]`.
- ATV Track's and Smashing Drive's steering motor drives the force feedback;
  the Gaelco games' lamps, coin counters and Smashing Drive's seat motor go to
  the network outputs.
- The Gaelco games' translucent polygons blended from the farthest at each
  pixel, as their chip does (`SORT_TRANSLUCENT_PIXELS`): where smoke, dust
  or glass cross.
- Smashing Drive's volume, at 0 at each start as on its board, is raised in
  a second and a half (Volume Up held), no longer twenty: its "Vol" shows
  for a shorter while.
- NASCAR Arcade: the V key, its view change since 0.4.2, no longer makes the
  3D vanish until the next start. The renderer's debugging keys (V and other
  letters, 1, 2, 3) no longer change the picture in normal use.
- Star Wars Racer Arcade's ROMBD TEST finds all its ICs GOOD, under their
  own numbers: all but the first pair were BAD, the board's ROMs read in
  the wrong layout by the test (the game itself read them right).


0.4.2 (2026-11-04)
------------------

- The games folder changed in the settings of the game list: its games now
  start, where they were looked for in the folder of the program's start;
  the list shows them at once when the folder is added, its field left
  (Enter), Save or Scan again pressed, else when the settings close.
- NASCAR Arcade's view change button in the Controls tab: *Change view*
  (`ViewChange` in `controls.ini`, linuxloader's and windowsloader's name),
  V or a controller's Y by default; before, only Down changed the view, and
  a `ViewChange` binding was ignored.
- The window's close button quits again, in a game and in the game list
  after one: the drawing thread's hidden window kept the program open.
- NASCAR Arcade: the smoke of a collision is a light haze, no longer a
  white square; the rear-view mirror shows the track the right way up (it
  was upside down), and the race's view has the height the game gives it;
  leaving the SYSTEM MENU with EXIT goes back to the game, where it stayed
  on a black screen. The cursor of the menus shines again as it moves.
  Planet Harriers' nebula drifts more slowly, as the game asks.


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
