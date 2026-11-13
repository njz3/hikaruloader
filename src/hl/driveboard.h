/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Hikaruloader is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Hikaruloader is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __HL_DRIVEBOARD_H__
#define __HL_DRIVEBOARD_H__

#include "hl/core.h"
#include "hl/device.h"

namespace hl {

class State;

/* What runs on a Model 3 drive board: its program, one NMI at a time. The
 * interface of every kind of board: the program simulated (DriveBoardSr2),
 * or, later, the board's Z80 running its EPROM. At each NMI the program
 * reads the main board's command and the wheel's position, may write a
 * register of the servo board, and puts its reply on its output port. */
class DriveBoardProgram {
public:
	/* The servo board 838-12912-01, as the program writes it: a register
	 * (F8-FF), then its value */
	class Servo {
	public:
		virtual ~Servo () {}
		virtual void servo_write (uint8_t reg, uint8_t value) = 0;
	};

	virtual ~DriveBoardProgram () {}

	/* The power-on */
	virtual void	 reset () = 0;
	/* One NMI: command, the byte on the command port; wheel, the ADC's
	 * channel (00 left, 80 the centre, FF right) */
	virtual void	 nmi (uint8_t command, uint8_t wheel, Servo &servo) = 0;
	/* The byte on the reply port */
	virtual uint8_t	 reply () const = 0;

	virtual int	 save_state (State *state) = 0;
	virtual int	 load_state (State *state) = 0;
	virtual const char *debug_string () { return NULL; }
	/* It plays the effects itself: the servo board's registers are unused */
	virtual bool	 drives_wheel () const { return false; }

	/* The commands taken, to the log */
	bool		 log = false;
};

/* A Model 3 drive board (838-13366 "Drive BD SRT" and its kin): a Z80 board
 * between the main board and the servo board that drives the wheel's
 * motor. The main board writes a command on eight lines and reads a reply
 * on eight others: on the Hikaru, through the JVS I/O board (see
 * SegaIoBoard13844). The program runs on an NMI at 500 Hz; here the NMIs are
 * run on the machine's frames, all those due, when the main board writes or
 * reads (a command counts once it has lasted 4 NMIs). The commands written
 * are queued, each shown for HOLD NMIs at least (a frame), so that none is
 * lost when several come within a frame (16 at most, the repeats dropped,
 * as Supermodel does). The servo board's
 * registers become force feedback (hl::ForceFeedback):
 *
 *   FE  The torque: 80 none, above pushes left, below right, full at 80+-63
 *   FD  A friction, its high nibble
 *   FC  Sub-register 4 (bits 0-2): the servo board's own centring spring,
 *       its strength in bits 3-6 (bit 7: off)
 *   FB  A gain, when bit 7 is set
 *   FF  The motor idle: no force at all, until a torque or a friction
 *
 * The program writes the torque at each NMI, a frame's NMIs here at once,
 * and the wheel takes a force a frame: a torque swinging quickly (the
 * board's road effects, its shakes: 8 to 33 times a second about the
 * spring's torque) would come out as a few of its points, at random. Kept
 * NMI by NMI, a quick swing of the program's own, changing within the
 * frames (find_swing: some 12 Hz and up; a torque the game sets at each
 * frame, as NASCAR Arcade's, is played as it is) goes to the wheel's
 * vibration, at its amplitude and its period, and the torque's mean over its
 * whole periods to the constant force (a slower swing, a force a frame
 * follows it); a torque of one NMI far from both its neighbours (a bump of
 * EPR-19338A's road effect 02) to a jolt of the vibration. The gain (FB)
 * scales them all.
 *
 * Its registers in the Lua shell: get (1, 0) the reply, get (1, 1) the
 * command, get (1, 2) the torque. Option "log": the commands taken. */
/* F355 Challenge's (EPR-21867), its program as high level code: the effects
 * go to the wheel directly. See driveboard-f355.cpp */
class DriveBoardF355 : public DriveBoardProgram {
public:
	/* wheeler: 18 Wheeler's EPR-23000, the same program but for its gains, its
	 * damping curves and its 5x and 7x, which move the spring's centre */
	explicit DriveBoardF355 (Machine *mach, bool wheeler = false);

	void	 reset () override;
	void	 nmi (uint8_t command, uint8_t wheel, Servo &servo) override;
	uint8_t	 reply () const override;
	bool	 drives_wheel () const override { return true; }
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;

private:
	void	 execute (uint8_t cmd);
	void	 set_strength (uint8_t level);
	void	 update_spring ();
	void	 update_damper ();
	void	 update_torque ();

	Machine	*mach;
	bool	 wheeler;
	struct {
		bool	 running, half, vibration_reversed;
		int8_t	 reference_shift;
		uint8_t	 reply_source, last_command, pending, pending_nmis, wheel;
		uint8_t	 ramp_length, strength, ramp_from, ramp_to, ramp_steps, ramp_step, damping;
		uint8_t	 reference, reference_high, constant, vibration_nmis;
		uint32_t vibration;
		float	 power, last_torque;
	} s;
};

/* Jambo! Safari's deluxe cabinet's, its EPROM not known: the servo board's
 * registers written as EPR-19338A, Scud Race's, writes them, when that
 * EPROM is not there to run on the Z80. See driveboard-jambo.cpp */
class DriveBoardJambo : public DriveBoardProgram {
public:
	explicit DriveBoardJambo (Machine *mach);

	void	 reset () override;
	void	 nmi (uint8_t command, uint8_t wheel, Servo &servo) override;
	uint8_t	 reply () const override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;

private:
	void	 execute (uint8_t cmd, Servo &servo);
	void	 spring (Servo &servo);
	unsigned next_bump ();

	Machine	*mach;
	struct {
		uint8_t	 last_command, mode, road, spring, push;
		uint16_t nmis, idle_nmis, bump_wait;
		uint32_t random;
	} s;
};

class DriveBoard : public Device, private DriveBoardProgram::Servo {
public:
	static const unsigned NMI_HZ = 500;
	static const unsigned FRAME_HZ = 60;
	/* A command's NMIs at least, and the commands queued at most */
	static const unsigned HOLD = 9;
	static const unsigned QUEUE = 16;

	/* It keeps program, and deletes it */
	DriveBoard (Machine *mach, DriveBoardProgram *program);
	~DriveBoard () override;

	/* The main board's side */
	void	 write (uint8_t command);
	uint8_t	 read ();
	void	 set_wheel (uint8_t position) { wheel = position; }

	int	 get (unsigned size, uint32_t addr, void *val) override;
	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;
	bool	 set_option (const char *name, int value) override;

	DriveBoardProgram *program;
	/* The servo board's registers, as written last */
	uint8_t	 torque, friction, spring, gain, idle;

	/* A swing of the torque: over samples, an NMI each, the oldest first
	 * (HISTORY at most), the torque less its mean swinging quickly enough
	 * (some 12 to 45 Hz) and widely enough: true, its half amplitude
	 * (steps), its period (NMIs) and the torque's mean over its last whole
	 * periods (see driveboard.cpp) */
	struct Swing {
		float	 amplitude, period, mean;
	};
	static bool	 find_swing (const uint8_t *samples, unsigned count, Swing &swing);
	/* The samples kept */
	static const unsigned HISTORY = 128;

private:
	void	 servo_write (uint8_t reg, uint8_t value) override;
	void	 catch_up ();
	void	 update_ffb ();

	uint8_t	 command, wheel;
	/* Run since the power-on */
	uint64_t nmis;
	/* The torque at each NMI, the last HISTORY (a ring; not saved: empty
	 * again after a state's load); a jolt's force, and its last NMI */
	uint8_t	 history[HISTORY];
	unsigned history_count, history_next;
	float	 jolt;
	uint64_t jolt_until;
	/* The swing played, and the frames it is held for yet */
	Swing	 swing;
	unsigned swing_frames;
	uint64_t swing_frame;
	/* The commands written, not shown yet; the last one queued; the
	 * NMIs the current one has been shown */
	uint8_t	 queue[QUEUE];
	uint32_t queued, held;
	uint8_t	 last_queued;
};

/* EPR-20512, the program of Sega Rally 2's drive board, which NASCAR Arcade's
 * uses too, simulated from what is known of it (BackForceFeeder's
 * DRIVEBOARD.md); no ROM needed:
 *
 * - The start-up: replies CF (the servo board set up, 8 s), CE (1 s: 60-9F,
 *   the cockpit's centre), CD (1 s: 70-8F, the wheel's), the calibration
 *   (taken as done at once: the wheel is not driven here), then CC, ready
 *   (1 s: B0/B1, the cabinet), the motor free.
 * - Then the commands: 00-3F a torque to the left (FE 80+n), 40-7F to the
 *   right (FE 80-n), in a game (C7) only, held until the next; C1 attract,
 *   the motor free; C2 (in C1) and C6 a shake of 0.6 s and 0.8 s; C7 the
 *   game, at the operator's strength; C8, C9 the motor free; CA the test
 *   (80-87); CB the start-up again from CE; D0-D7 what to reply (the status,
 *   the DIP switches, the centres, the wheel...); F0-FF an echo.
 * - A command counts at 4 NMIs in a row, and only if it differs from the
 *   last one taken.
 *
 * The shakes' waveforms are in the EPROM's tables: approximated here, a sine
 * of 25 Hz at their amplitudes. */
class DriveBoardSr2 : public DriveBoardProgram {
public:
	/* strength: the operator's, the DIP switches 1-3 to 1-5, 0 to 7 */
	explicit DriveBoardSr2 (unsigned strength = 4);

	void	 reset () override;
	void	 nmi (uint8_t command, uint8_t wheel, Servo &servo) override;
	uint8_t	 reply () const override { return out; }

	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;

	/* As the program reads them: ON reads 0 */
	uint8_t	 dip1, dip2;

private:
	enum Phase {
		SETUP,		/* CF */
		COCKPIT,	/* CE */
		CENTRE,		/* CD */
		READY,		/* CC */
		RUN
	};

	void	 take (uint8_t cmd, Servo &servo);
	void	 test (unsigned what, Servo &servo);
	void	 settings (Servo &servo, bool game, bool gain);
	void	 shake (unsigned nmis, uint8_t amplitude);
	uint8_t	 status () const;

	/* Plain integers, for the states */
	uint32_t phase, ticks, shake_left, shake_length;
	uint8_t	 seen, stable, taken, took;
	/* The mode, the last of C1-CB taken (0 before); the reply chosen,
	 * D0-D7 as 0-7, 8 the echo */
	uint8_t	 mode, select, echo;
	uint8_t	 wheel_centre, cockpit_centre, cabinet, lamp, wheel, shake_amplitude;
	uint8_t	 out;
};

} /* namespace hl */

#endif /* __HL_DRIVEBOARD_H__ */
