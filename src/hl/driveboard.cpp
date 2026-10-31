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

#include <math.h>

#include "hl/driveboard.h"
#include "hl/ffb.h"
#include "hl/machine.h"
#include "hl/state.h"

namespace hl {

/* The servo board's registers */
#define SERVO_ORIGIN	0xF8
#define SERVO_F9	0xF9
#define SERVO_FA	0xFA
#define SERVO_GAIN	0xFB
#define SERVO_SPRING	0xFC
#define SERVO_FRICTION	0xFD
#define SERVO_TORQUE	0xFE
#define SERVO_IDLE	0xFF

/* At most 10 s of NMIs caught up at once; a longer gap (a state of a version
 * without the board) is skipped */
#define MAX_CATCH_UP	(10 * DriveBoard::NMI_HZ)

/* DriveBoard */

DriveBoard::DriveBoard (Machine *mach, DriveBoardProgram *program) :
	Device (mach),
	program (program)
{
	reset (RESET_HARD);
}

DriveBoard::~DriveBoard ()
{
	ForceFeedback::set (ForceFeedback::STOP, 0.0f, mach->frame);
	delete program;
}

void
DriveBoard::catch_up ()
{
	uint64_t due = mach->frame * NMI_HZ / FRAME_HZ;

	if (due < nmis || due - nmis > MAX_CATCH_UP)
		nmis = due;
	while (nmis < due) {
		if (queued && held >= HOLD) {
			command = queue[0];
			memmove (queue, queue + 1, --queued);
			held = 0;
		}
		program->nmi (command, wheel, *this);
		nmis++;
		held++;
	}
	update_ffb ();
}

void
DriveBoard::write (uint8_t value)
{
	catch_up ();
	if (value == last_queued)
		return;
	if (queued == QUEUE)
		memmove (queue, queue + 1, --queued);
	queue[queued++] = value;
	last_queued = value;
}

uint8_t
DriveBoard::read ()
{
	catch_up ();
	return program->reply ();
}

void
DriveBoard::servo_write (uint8_t reg, uint8_t value)
{
	switch (reg) {
	case SERVO_TORQUE:
		torque = value;
		idle = 0;
		break;
	case SERVO_FRICTION:
		friction = value;
		idle = 0;
		break;
	case SERVO_SPRING:
		if ((value & 7) == 4)
			spring = value;
		break;
	case SERVO_GAIN:
		if (value & 0x80)
			gain = value & 0x7F;
		break;
	case SERVO_IDLE:
		idle = 1;
		break;
	default:
		/* The origin and the set-up */
		break;
	}
}

/* The servo board's registers as forces, the way Supermodel's wheel board
 * reads them: the torque, twice its distance from 80 (beyond one) on 127 */
void
DriveBoard::update_ffb ()
{
	int d = (int) torque - 0x80;
	float constant = 0.0f, spring_force, friction_force;

	if (idle) {
		ForceFeedback::set (ForceFeedback::STOP, 0.0f, mach->frame);
		return;
	}
	if (d > 1)
		constant = 2.0f * (d - 1) / 127.0f;
	else if (d < -1)
		constant = 2.0f * (d + 1) / 127.0f;
	constant *= gain / 127.0f;
	spring_force = (spring & 0x80) ? 0.0f : ((spring & 0x78) >> 3) * 0x18 / 255.0f;
	friction_force = (friction >> 4) * 0x20 / 255.0f;
	ForceFeedback::set (ForceFeedback::CONSTANT, constant, mach->frame);
	ForceFeedback::set (ForceFeedback::SPRING, spring_force, mach->frame);
	ForceFeedback::set (ForceFeedback::FRICTION, friction_force, mach->frame);
}

int
DriveBoard::get (unsigned size, uint32_t addr, void *val)
{
	if (size != 1 || addr > 2)
		return -1;
	set_ptr (val, 1, (addr == 0) ? program->reply () : (addr == 1) ? command : torque);
	return 0;
}

void
DriveBoard::reset (ResetType type)
{
	if (type != RESET_HARD)
		return;
	program->reset ();
	command = 0xFF;
	wheel = 0x80;
	nmis = 0;
	queued = 0;
	held = HOLD;
	last_queued = 0xFF;
	torque = 0x80;
	friction = spring = 0;
	gain = 0x7F;
	idle = 1;
	ForceFeedback::set (ForceFeedback::STOP, 0.0f, mach->frame);
}

int
DriveBoard::save_state (State *state)
{
	int ret = state->put (&command, 1);

	ret |= state->put (&wheel, 1);
	ret |= state->put (&nmis, sizeof (nmis));
	ret |= state->put (&torque, 1);
	ret |= state->put (&friction, 1);
	ret |= state->put (&spring, 1);
	ret |= state->put (&gain, 1);
	ret |= state->put (&idle, 1);
	ret |= state->put (queue, sizeof (queue));
	ret |= state->put (&queued, sizeof (queued));
	ret |= state->put (&held, sizeof (held));
	ret |= state->put (&last_queued, 1);
	ret |= program->save_state (state);
	return ret;
}

int
DriveBoard::load_state (State *state)
{
	int ret = state->get (&command, 1);
	ret |= state->get (&wheel, 1);
	ret |= state->get (&nmis, sizeof (nmis));
	ret |= state->get (&torque, 1);
	ret |= state->get (&friction, 1);
	ret |= state->get (&spring, 1);
	ret |= state->get (&gain, 1);
	ret |= state->get (&idle, 1);
	ret |= state->get (queue, sizeof (queue));
	ret |= state->get (&queued, sizeof (queued));
	ret |= state->get (&held, sizeof (held));
	ret |= state->get (&last_queued, 1);
	if (queued > QUEUE)
		queued = 0;
	ret |= program->load_state (state);
	update_ffb ();
	return ret;
}

const char *
DriveBoard::debug_string ()
{
	static char text[200];
	const char *own = program->debug_string ();

	snprintf (text, sizeof (text), "%s; command %02X, reply %02X, servo FE %02X FD %02X FC %02X FB %02X%s",
	          own ? own : "", command, program->reply (), torque, friction, spring, gain,
	          idle ? ", idle" : "");
	return text;
}

bool
DriveBoard::set_option (const char *name, int value)
{
	if (strcmp (name, "log"))
		return false;
	program->log = value != 0;
	return true;
}

/* DriveBoardSr2 */

/* The phases' lengths, in NMIs */
#define SETUP_NMIS	(8 * DriveBoard::NMI_HZ)
#define PHASE_NMIS	(1 * DriveBoard::NMI_HZ)

/* The replies at start-up */
static const uint8_t phase_reply[] = { 0xCF, 0xCE, 0xCD, 0xCC };

/* The operator's strength: FD and FC by row */
static const uint8_t strength_rows[8][2] = {
	{ 0x11, 0x0C }, { 0x21, 0x14 }, { 0x21, 0x1C }, { 0x31, 0x24 },
	{ 0x31, 0x2C }, { 0x41, 0x34 }, { 0x51, 0x3C }, { 0x51, 0x44 },
};

/* The reply chosen by F0-FF */
#define SELECT_ECHO	8

DriveBoardSr2::DriveBoardSr2 (unsigned strength)
{
	/* 1-3 to 1-5: the strength's row; the others OFF: the game */
	dip1 = (uint8_t) (0xFF & ~((strength & 7) << 2));
	dip2 = 0xFF;
	reset ();
}

void
DriveBoardSr2::reset ()
{
	phase = SETUP;
	ticks = SETUP_NMIS;
	shake_left = shake_length = 0;
	seen = 0xFF;
	stable = taken = took = 0;
	mode = select = echo = 0;
	wheel_centre = cockpit_centre = 0x80;
	cabinet = lamp = 0;
	wheel = 0x80;
	shake_amplitude = 0;
	out = phase_reply[SETUP];
}

/* The settings the modes write first; game: the operator's strength */
void
DriveBoardSr2::settings (Servo &servo, bool game, bool gain)
{
	const uint8_t *row = strength_rows[((~dip1) >> 2) & 7];

	servo.servo_write (SERVO_FRICTION, game ? row[0] : 0x00);
	servo.servo_write (SERVO_SPRING, 0x0A);
	servo.servo_write (SERVO_SPRING, 0x0B);
	if (game)
		servo.servo_write (SERVO_SPRING, row[1]);
	if (gain)
		servo.servo_write (SERVO_GAIN, 0xFF);
}

void
DriveBoardSr2::shake (unsigned nmis, uint8_t amplitude)
{
	shake_left = shake_length = nmis;
	shake_amplitude = amplitude;
}

/* Bits 0-2: the servo board's alarms cleared; bit 4: the output (port 2D);
 * bits 6-7, the wheel's motion: still */
uint8_t
DriveBoardSr2::status () const
{
	return (uint8_t) (0x07 | (lamp ? 0x10 : 0));
}

/* The test's commands, 80-87, in CA */
void
DriveBoardSr2::test (unsigned what, Servo &servo)
{
	switch (what) {
	case 0:
		servo.servo_write (SERVO_TORQUE, 0x80);
		break;
	case 1:
		servo.servo_write (SERVO_TORQUE, 0x69);
		break;
	case 2:
		servo.servo_write (SERVO_TORQUE, 0x97);
		break;
	case 3:
	case 4:
		lamp = (what == 3);
		break;
	case 5:
		/* The wheel's centre where the wheel is, within 70-8F */
		if (wheel >= 0x70 && wheel <= 0x8F) {
			wheel_centre = wheel;
			servo.servo_write (SERVO_ORIGIN, 0xF9);
			servo.servo_write (SERVO_TORQUE, 0x80);
			servo.servo_write (SERVO_ORIGIN, 0xFA);
		}
		break;
	case 6:
		/* No cockpit: it stays at 80 */
		cockpit_centre = 0x80;
		break;
	default:
		/* The displays' blink */
		break;
	}
}

void
DriveBoardSr2::take (uint8_t cmd, Servo &servo)
{
	if (log)
		HL_PRINT ("drive board: command %02X (reply %02X)", cmd, out);

	/* The start-up takes few commands */
	if (phase != RUN) {
		if (cmd == 0xCA && phase != SETUP) {
			settings (servo, false, true);
			servo.servo_write (SERVO_IDLE, 0xFF);
			phase = RUN;
			mode = 0xCA;
		} else if (phase == COCKPIT && cmd >= 0x60 && cmd <= 0x9F)
			cockpit_centre = cmd;
		else if (phase == CENTRE && cmd >= 0x70 && cmd <= 0x8F)
			wheel_centre = cmd;
		else if (phase == READY && (cmd == 0xB0 || cmd == 0xB1))
			cabinet = cmd & 1;
		return;
	}

	if (cmd < 0x80) {
		/* A torque, held: in a game only */
		if (mode == 0xC7) {
			servo.servo_write (SERVO_FRICTION, 0x11);
			servo.servo_write (SERVO_TORQUE, (uint8_t) ((cmd < 0x40) ? 0x80 + (cmd & 0x3F) : 0x80 - (cmd & 0x3F)));
		}
	} else if (cmd < 0x90) {
		if (mode == 0xCA)
			test (cmd & 7, servo);
	} else if (cmd >= 0xC0 && cmd < 0xD0) {
		switch (cmd) {
		case 0xC1:
		case 0xC6:
		case 0xC8:
		case 0xC9:
		case 0xCA:
		case 0xCB:
			/* The settings (C8 and C9 leave the gain), the motor
			 * free */
			settings (servo, false, cmd != 0xC8 && cmd != 0xC9);
			servo.servo_write (SERVO_IDLE, 0xFF);
			mode = cmd;
			shake_left = 0;
			if (cmd == 0xC6)
				shake (400, 19);
			else if (cmd == 0xC8 || cmd == 0xC9)
				lamp = 1;
			else if (cmd == 0xCB) {
				/* The start-up again from CE */
				mode = 0;
				phase = COCKPIT;
				ticks = PHASE_NMIS;
			}
			break;
		case 0xC2:
			if (mode == 0xC1)
				shake (300, 15);
			break;
		case 0xC7:
			settings (servo, true, true);
			mode = cmd;
			shake_left = 0;
			break;
		default:
			/* C0, C3-C5, CC-CF: nothing */
			break;
		}
	} else if (cmd >= 0xD0 && cmd < 0xE0)
		select = cmd & 7;
	else if (cmd >= 0xF0) {
		select = SELECT_ECHO;
		echo = cmd;
	}
}

void
DriveBoardSr2::nmi (uint8_t command, uint8_t position, Servo &servo)
{
	wheel = position;

	/* The command: taken once read the same at 4 NMIs in a row, and only
	 * if it differs from the last one taken */
	if (command != seen) {
		seen = command;
		stable = 0;
	}
	if (stable < 4 && ++stable == 4 && (!took || seen != taken)) {
		taken = seen;
		took = 1;
		take (seen, servo);
	}

	switch (phase) {
	case SETUP:
		if (ticks == SETUP_NMIS) {
			/* The servo board set up */
			servo.servo_write (SERVO_IDLE, 0xFF);
			servo.servo_write (SERVO_TORQUE, 0x80);
			servo.servo_write (SERVO_FRICTION, 0x00);
			servo.servo_write (SERVO_SPRING, 0x0A);
			servo.servo_write (SERVO_SPRING, 0x0B);
			servo.servo_write (SERVO_F9, 0x9F);
			servo.servo_write (SERVO_IDLE, 0xFF);
		}
		if (--ticks == 0) {
			phase = COCKPIT;
			ticks = PHASE_NMIS;
		}
		break;
	case COCKPIT:
		if (--ticks == 0) {
			phase = CENTRE;
			ticks = PHASE_NMIS;
		}
		break;
	case CENTRE:
		if (--ticks == 0) {
			/* The calibration, done at once; the servo board's
			 * settings */
			servo.servo_write (SERVO_ORIGIN, 0xF9);
			servo.servo_write (SERVO_TORQUE, 0x80);
			servo.servo_write (SERVO_FRICTION, 0x66);
			servo.servo_write (SERVO_ORIGIN, 0xFA);
			servo.servo_write (SERVO_FA, 0xF8);
			servo.servo_write (SERVO_FA, 0x0E);
			servo.servo_write (SERVO_FA, 0x4F);
			servo.servo_write (SERVO_GAIN, 0x05);
			phase = READY;
			ticks = PHASE_NMIS;
		}
		break;
	case READY:
		if (--ticks == 0) {
			/* The motor free, then the main loop */
			servo.servo_write (SERVO_TORQUE, 0x80);
			servo.servo_write (SERVO_FRICTION, 0x00);
			servo.servo_write (SERVO_IDLE, 0xFF);
			phase = RUN;
		}
		break;
	default:
		break;
	}

	/* A shake: a torque at every NMI, then FE 80 held */
	if (shake_left) {
		unsigned i = shake_length - shake_left;
		double v = shake_amplitude * sin (2.0 * 3.14159265358979 * i / 20.0);
		shake_left--;
		servo.servo_write (SERVO_TORQUE, shake_left ? (uint8_t) lrint (0x80 + v) : 0x80);
	}

	if (phase != RUN)
		out = phase_reply[phase];
	else switch (select) {
	case 0:
		out = status ();
		break;
	case 1:
		out = dip1;
		break;
	case 2:
		out = dip2;
		break;
	case 3:
		out = wheel_centre;
		break;
	case 4:
		out = cockpit_centre;
		break;
	case 5:
		out = wheel;
		break;
	case 6:
		/* No cockpit */
		out = 0x80;
		break;
	case 7:
		out = phase_reply[READY];
		break;
	default:
		out = echo;
		break;
	}
}

int
DriveBoardSr2::save_state (State *state)
{
	int ret = state->put (&phase, sizeof (phase));

	ret |= state->put (&ticks, sizeof (ticks));
	ret |= state->put (&shake_left, sizeof (shake_left));
	ret |= state->put (&shake_length, sizeof (shake_length));
	ret |= state->put (&seen, 1);
	ret |= state->put (&stable, 1);
	ret |= state->put (&taken, 1);
	ret |= state->put (&took, 1);
	ret |= state->put (&mode, 1);
	ret |= state->put (&select, 1);
	ret |= state->put (&echo, 1);
	ret |= state->put (&wheel_centre, 1);
	ret |= state->put (&cockpit_centre, 1);
	ret |= state->put (&cabinet, 1);
	ret |= state->put (&lamp, 1);
	ret |= state->put (&wheel, 1);
	ret |= state->put (&shake_amplitude, 1);
	ret |= state->put (&out, 1);
	return ret;
}

int
DriveBoardSr2::load_state (State *state)
{
	int ret = state->get (&phase, sizeof (phase));

	ret |= state->get (&ticks, sizeof (ticks));
	ret |= state->get (&shake_left, sizeof (shake_left));
	ret |= state->get (&shake_length, sizeof (shake_length));
	ret |= state->get (&seen, 1);
	ret |= state->get (&stable, 1);
	ret |= state->get (&taken, 1);
	ret |= state->get (&took, 1);
	ret |= state->get (&mode, 1);
	ret |= state->get (&select, 1);
	ret |= state->get (&echo, 1);
	ret |= state->get (&wheel_centre, 1);
	ret |= state->get (&cockpit_centre, 1);
	ret |= state->get (&cabinet, 1);
	ret |= state->get (&lamp, 1);
	ret |= state->get (&wheel, 1);
	ret |= state->get (&shake_amplitude, 1);
	ret |= state->get (&out, 1);
	return ret;
}

const char *
DriveBoardSr2::debug_string ()
{
	static const char *const phases[] = { "set-up", "cockpit's centre", "wheel's centre", "ready", "running" };
	static char text[96];

	snprintf (text, sizeof (text), "EPR-20512 simulated, %s, mode %02X, last command %02X",
	          phases[phase], mode, taken);
	return text;
}

} /* namespace hl */
