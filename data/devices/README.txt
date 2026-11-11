The devices' profiles
=====================

A profile is a device's bindings, made once by someone who has that model of
wheel, pedals, shifter, stick or pad, and shared: when such a device is first
connected on a player, its profile is put on that player's bindings in
controls.ini, and the Controls tab's Default and Reset put it back.

Make one in the Controls tab: bind the device on its page, then "Save as a
profile...". It is written to the devices folder beside controls.ini, where
the user's profiles are read (over the ones here). Give that file to whoever
has the same device; send it to be shipped here.

A profile is a .ini file in controls.ini's syntax:

    [Device]
    # The device's name, as the Controls tab and hikaruloader.log give it
    Name = My Wheel
    # Its USB vendor and product (hikaruloader.log), found first
    VidPid = 1234:ABCD

    [Driving]
    Steering = JOY_AXIS_0
    Accelerator = JOY_AXIS_1_INVERTED
    ShiftUp = JOY_BUTTON_4

    [Player1]
    P1_Start = JOY_BUTTON_9

Its inputs have no player's number: JOY_ a joystick's (as SDL numbers its
axes, buttons and hats, from 0), GC_ a game controller's (by SDL's names:
GC_BUTTON_A, GC_AXIS_LEFTX). They go to the player the device is (JOY1_,
GC1_ for player 2). The rows are controls.ini's: see its notes.
