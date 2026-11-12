Hikaruloader's specifications
=============================

Hikaruloader, Copyright (C) 2026 Benjamin Maurin (njz3). Released under the
GNU GPL v3 or later: see LICENSE.

How some of the controls must behave, rule by rule, each with the tests that
hold it. A change to one of these behaviours changes this file and its tests
together. The user's manual is [README.md](README.md); the emulator's
insides are in [DEVELOPMENT.md](DEVELOPMENT.md).


The gear lever
--------------

Some cabinets have a gear lever that stays where it is put. Each of its
positions closes switches on the I/O board, which the game decodes as its
INPUT TEST shows them (`gearbox` in `data/games.json`, `hl::Gearbox`).

| Game | Gears | Lever |
|---|---|---|
| NASCAR Arcade (`sgnascar`) | N, 1 to 4 | H4 |
| F355 Challenge (`f355`, `f355twin`, `f355twn2`) | N, 1 to 6 | H6; its wing paddles are switches of the game's own |
| 18 Wheeler (`18wheelr` and its sets) | R, N, 1 (LOW), 2 (HIGH) | reverse, neutral, low and high |
| Crazy Taxi (`crzytaxi`) | R, 1 (DRIVE) | drive and reverse, no neutral |
| The King of Route 66 (`kingrt66`, `kingrt66p`) | R (SHIFT [R], p2:12), 1 (SHIFT [D], p2:13) | drive and reverse, no neutral |

A player moves it with the `[Driving]` rows of `controls.ini`:

- **the H shifter's buttons**: `GearReverse`, `GearNeutral`, `Gear1` to
  `Gear6`. A vJoy H panel holds the button of the position it is in;
  a real H shifter holds its gear's button;
- **Shift up and Shift down**: `ShiftUp`, `ShiftDown`, a paddle or a
  sequential lever.

Both kinds can be bound at once, as on a cabinet whose `controls.ini` serves
an H panel and an up and down panel alike. The rules below are the same for
an H4, an H6 and the levers with a reverse.

### The rules

1. **A button pressed engages its gear.** The press is the event; letting
   go is none: the gear stays engaged after the button is released.
2. **A button held gives no other event.** The last gear engaged stays. A
   shift up or down while a button is held engages the next gear, and the
   held button does not take it back.
3. **Of several buttons, the last pressed.** Its press is the last event.
   Buttons pressed in the same frame: the highest gear of them.
4. **Shift up and Shift down engage the next gear** up or down, in the
   game's order (R, then 1, 2...), neutral not one of them: from neutral,
   up is gear 1 and down is reverse; from gear 1, down is reverse, or
   nothing in a game without one. Past the last gear, nothing. Each press
   one gear; held, no other. F355's wing paddles are its own switches
   (`ShiftUp` and `ShiftDown` on its I/O board): there, the lever moves by
   its buttons alone.
5. **Neutral only by its button, or while shifting.** `GearNeutral`
   pressed engages neutral, kept until the next gear's event. A shift from
   a gear to another (by a button, Shift up or Shift down) goes through
   neutral for `[Input]` `SHIFT_NEUTRAL_MS` (0 by default: at once), as a
   real lever's travel. There is no such time from or to neutral, nor in a
   game without neutral (Crazy Taxi, The King of Route 66).
6. **At the game's start**, as the game first reads its switches: the
   gear of a button already held; of several held (all of them, say), the
   highest; none held, neutral; in a game without neutral, its first
   forward gear (DRIVE). The buttons held then give no event when they
   are let go or stay held. A shift pressed then is held, no event.
7. **A button for a gear the game does not have is left aside**:
   `GearNeutral` in Crazy Taxi and The King of Route 66, `Gear5`, `Gear6`
   and `GearReverse` in NASCAR Arcade.
8. **The agent's gear** (`set Gear N`, -1 to 6) comes over the controls
   until `release Gear` (or `release all`); the controls' gear then
   comes back.
9. The lever is looked at once a frame of the machine's, as the game polls
   its switches; a frame asked twice gets the same gear.

`SHIFTER_NEUTRAL` (until 0.8.0) put the lever in neutral whenever none of
the H buttons was held. Rules 1 and 5 replace it; the key is left aside in
an older `hikaruloader.ini`.

### Its tests

- `gearboxtest` (`tests/gearbox_test.cpp`, run by ctest): the rules as
  scenarios, each a gearbox and a script of frames, each frame the buttons
  pressed (`+3`) and let go (`-3`) and the gear the game gets (`=3`, `=N`,
  `=R`). The scenarios are named by their rule: *start*, *event*, *held*,
  *several*, *neutral*, *paddles*, *crossing*, *agent*. Then the games'
  levers from `data/games.json`: their gears and the switches each gear
  closes on their I/O board (NASCAR Arcade, F355, 18 Wheeler, The King of
  Route 66, Crazy Taxi).
- Checked by hand in the games' INPUT TESTs, with the cabinets' bindings
  (`GearNeutral` on vJoy button 17, `GearReverse` 18, `Gear1` to `Gear6` 19
  to 24, Shift up and down 9 and 10) and the agent: NASCAR Arcade's GEAR
  POSITION, F355's 6 SPEED SHIFT, 18 Wheeler's SHIFT [L], [H] and [R], The
  King of Route 66's SHIFT [D] and [R].


Star Wars: Racer Arcade's levers
--------------------------------

The cabinet has two thrust levers, one for each of the pod's engines
(`LeftLever` on `an:0`, `RightLever` on `an:1`): C000 at rest, 4000 pushed
all the way. Even, the pod goes straight; the side pushing harder turns it
the other way (the left engine alone turns it right); both back, it stops.
The brake is a switch and a channel (`p1:8`, `an:2`), the boost button 1
(`p1:9`), the left and right buttons `p1:10` and `p1:11` (the menus).

`[Input]` `TWIN_LEVERS` chooses how a player's controls make the two levers:

- **`COMBINED`** (the default), for a wheel and pedals or a controller's
  stick and trigger: the accelerator A (`Accelerator`, 0 to 1) is the
  thrust of both, the wheel S (`Steering`, -1 left to 1 right) the
  difference, the pod turning to the side steered:

      left lever  = clamp (A x (1 + S), 0, 1)
      right lever = clamp (A x (1 - S), 0, 1)

  Straight, both levers at A; the wheel at half right, the left lever at
  1.5 A (1 at most) and the right one at A / 2; no accelerator, both at
  rest whatever the wheel.
- **`ORIGINAL`**, as on the cabinet: `LeftLever` and `RightLever`, an axis
  each from 0 (at rest) to 1 (a controller's two sticks pushed up by
  default); `LeftLever_Digital` and `RightLever_Digital` (A and D on the
  keyboard) push them all the way in `Axis_Key_Ramp`. The wheel and the
  accelerator move neither.

A lever's value v (0 to 1) reaches the game as `C000 + (4000 - C000) x v`:
C000 at rest, 8000 half way, 4000 all the way.

### Its tests

- `controlstest` (`tests/controls_test.cpp`): *levers: a combined pair*
  (the pair read from `data/games.json`), *the accelerator's thrust on
  both* (A 1, S 0: 4000 and 4000), *to the right, the right one eased* (A
  1, S 0.5: 4000 and 8000), *no accelerator, no thrust* (C000), *an axis
  each* (`ORIGINAL`, `LeftLever` 0.5 with the accelerator pressed: 8000
  and C000).
