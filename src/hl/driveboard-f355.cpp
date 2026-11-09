/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/maple/maple_jvs.cpp (GPL-2.0-or-later,
 * Copyright flyinghead and contributors).
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

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hl/driveboard.h"
#include "hl/ffb.h"
#include "hl/machine.h"
#include "hl/state.h"

namespace hl {

/*
 * F355 Challenge's Drive Board (EPR-21867)
 * ========================================
 *
 * Its program as high level code, from the disassembly of the EPROM. It runs on
 * an NMI at 250 Hz:
 *
 *  - a command must last 3 NMIs to be seen, and is acted upon when it changes
 *  - below F0, only obeyed once FE has started the board
 *  - F0-F7 select what the board answers
 *  - FA-FD set the gain, only while the board is stopped
 *  - 00-3F the ramp to the next spring strength, 4 NMIs a step
 *  - 40-7F the spring strength (bits 0-3) and the damping curve (bits 4-5)
 *  - 80-BF one of 32 short vibrations, bit 5 the other way
 *  - Dx then Cx the wheel position the spring pulls to
 *  - E0-EF a constant torque
 *
 * The board sums a constant torque, a spring, a damping and a vibration into
 * one torque; here the spring and the damping go to the wheel's own effects.
 */

/* Each one ended by 0, 0x80 neutral; written every 4 NMIs */
static const uint8_t waveforms[] = {
	192, 0,
	192, 192, 0,
	192, 192, 192, 0,
	192, 192, 192, 192, 0,
	64, 192, 0,
	64, 192, 192, 0,
	64, 192, 192, 192, 0,
	64, 192, 192, 192, 192, 0,
	64, 64, 192, 0,
	64, 64, 192, 192, 0,
	64, 64, 192, 192, 192, 0,
	64, 64, 192, 192, 192, 192, 0,
	64, 64, 64, 192, 0,
	64, 64, 64, 192, 192, 0,
	64, 64, 64, 192, 192, 192, 0,
	64, 64, 64, 192, 192, 192, 192, 0,
	255, 0,
	255, 255, 0,
	255, 255, 255, 0,
	255, 255, 255, 255, 0,
	1, 255, 0,
	1, 255, 255, 0,
	1, 255, 255, 255, 0,
	1, 255, 255, 255, 255, 0,
	148, 106, 148, 106, 148, 106, 148, 106, 148, 106, 148, 106, 0,
	160, 96, 160, 96, 160, 96, 160, 96, 160, 96, 160, 96, 0,
	176, 80, 176, 80, 176, 80, 176, 80, 176, 80, 176, 80, 0,
	148, 148, 106, 106, 148, 148, 106, 106, 148, 148, 106, 106, 0,
	160, 160, 96, 96, 160, 160, 96, 96, 160, 160, 96, 96, 0,
	176, 176, 80, 80, 176, 176, 80, 80, 176, 176, 80, 80, 0,
	160, 160, 96, 96, 96, 160, 160, 96, 96, 96, 160, 160, 96, 96, 96, 0,
	176, 176, 80, 80, 80, 176, 176, 80, 80, 80, 176, 176, 80, 80, 80, 0,
};

static const uint32_t NO_VIBRATION = 0xFFFFFFFF;

DriveBoardF355::DriveBoardF355 (Machine *mach_) :
	mach (mach_)
{
	reset ();
}

void
DriveBoardF355::reset ()
{
	memset (&s, 0, sizeof (s));
	s.reply_source = 0xFF;
	s.power = 1.0f;
	s.reference = s.reference_high = 0x80;
	s.constant = 0x80;
	s.vibration = NO_VIBRATION;
	s.last_torque = NAN;
}

uint8_t
DriveBoardF355::reply () const
{
	switch (s.reply_source) {
	case 0:
		return s.last_command;
	case 1:
		/* Bit 4 when stopped */
		return s.running ? 0 : 0x10;
	case 5:
		/* The wheel's potentiometer */
		return s.wheel;
	case 2:
	case 3:
	case 4:
	case 6:
	case 7:
		return 0;
	default:
		return 0xFF;
	}
}

void
DriveBoardF355::update_spring ()
{
	/* strength x curve (|wheel - reference|) / 256, up to 210, the curve
	 * rising by about 1.34 a step of the wheel */
	float force = std::min (s.strength * 1.34f / 256.0f, 1.0f) * s.power;

	ForceFeedback::spring_offset ((s.reference - 0x80) / 128.0f);
	ForceFeedback::set (ForceFeedback::SPRING, force, mach->frame);
}

void
DriveBoardF355::update_damper ()
{
	/* A curve indexed by the wheel's travel over 8 NMIs; full scale at: */
	static const int full_scale[4] = { 0, 96, 48, 32 };
	int full = full_scale[s.damping & 3];
	float slope = full ? std::min (32.0f / full, 1.0f) : 0.0f;

	ForceFeedback::set (ForceFeedback::FRICTION, slope * s.power, mach->frame);
}

void
DriveBoardF355::update_torque ()
{
	int vibration = 0x80, sum;
	float torque;

	if (s.vibration != NO_VIBRATION) {
		vibration = waveforms[s.vibration];
		if (s.vibration_reversed)
			vibration = (uint8_t) ~vibration;
	}
	sum = (int) s.constant + vibration - 0x100;
	torque = std::clamp (sum / 128.0f, -1.0f, 1.0f) * s.power;
	if (torque != s.last_torque) {
		s.last_torque = torque;
		ForceFeedback::set (ForceFeedback::CONSTANT, torque, mach->frame);
	}
}

void
DriveBoardF355::set_strength (uint8_t level)
{
	uint8_t target = (uint8_t) (level * 14);

	if (target == s.strength) {
		s.ramp_steps = s.ramp_step = 0;
	} else if (s.ramp_length == 0) {
		s.strength = target;
		s.ramp_steps = s.ramp_step = 0;
		update_spring ();
	} else {
		s.ramp_from = s.strength;
		s.ramp_to = target;
		s.ramp_steps = s.ramp_length;
		s.ramp_step = 0;
	}
}

void
DriveBoardF355::execute (uint8_t cmd)
{
	if (cmd >= 0xF0) {
		static const float gains[4] = { 1.0f, 0.9f, 0.8f, 0.6f };

		if (cmd <= 0xF7) {
			s.reply_source = cmd & 7;
		} else if (cmd >= 0xFA && cmd <= 0xFD) {
			if (!s.running)
				s.power = gains[cmd - 0xFA];
		} else if (cmd == 0xFE) {
			s.running = true;
			s.strength = 0;
			s.ramp_length = 0;
			s.ramp_steps = s.ramp_step = 0;
			s.damping = 2;
			s.reference = s.reference_high = 0x80;
			s.constant = 0x80;
			s.vibration = NO_VIBRATION;
			s.last_torque = NAN;
			update_spring ();
			update_damper ();
			update_torque ();
		} else if (cmd == 0xFF) {
			s.running = false;
			s.vibration = NO_VIBRATION;
			ForceFeedback::set (ForceFeedback::STOP, 0.0f, mach->frame);
		}
		return;
	}
	if (!s.running)
		return;
	if (cmd < 0x40) {
		s.ramp_length = (uint8_t) (cmd * 4);
	} else if (cmd < 0x80) {
		uint8_t curve = (cmd >> 4) & 3;

		set_strength (cmd & 15);
		if (curve != s.damping) {
			s.damping = curve;
			update_damper ();
		}
	} else if (cmd < 0xC0) {
		int i;

		s.vibration = 0;
		for (i = cmd & 0x1F; i > 0; i--)
			while (waveforms[s.vibration++] != 0)
				;
		s.vibration_reversed = (cmd & 0x20) != 0;
		s.vibration_nmis = 0;
	} else if (cmd < 0xD0) {
		s.reference = s.reference_high | (cmd & 15);
		update_spring ();
	} else if (cmd < 0xE0) {
		s.reference_high = (uint8_t) ((cmd & 15) << 4);
	} else {
		static const uint8_t levels[16] = {
			0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10,
			0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0
		};

		s.constant = levels[cmd & 15];
		update_torque ();
	}
}

void
DriveBoardF355::nmi (uint8_t command, uint8_t wheel_, Servo &)
{
	s.wheel = wheel_;
	/* DriveBoard's NMIs come at 500 Hz, this program's at 250 */
	s.half = !s.half;
	if (s.half)
		return;
	if (command != s.pending) {
		s.pending = command;
		s.pending_nmis = 1;
	} else if (s.pending_nmis < 3 && ++s.pending_nmis == 3 && s.pending != s.last_command) {
		s.last_command = s.pending;
		execute (s.pending);
	}
	if (!s.running)
		return;
	if (s.ramp_step < s.ramp_steps) {
		int delta;

		s.ramp_step++;
		delta = abs ((int) s.ramp_to - s.ramp_from) * s.ramp_step / s.ramp_steps;
		s.strength = (uint8_t) (s.ramp_to > s.ramp_from ? s.ramp_from + delta : s.ramp_from - delta);
		if (s.ramp_step == s.ramp_steps || s.ramp_step % 4 == 0)
			update_spring ();
	}
	if (s.vibration != NO_VIBRATION && s.vibration_nmis++ % 4 == 0) {
		if (waveforms[s.vibration] == 0)
			s.vibration = NO_VIBRATION;
		update_torque ();
		if (s.vibration != NO_VIBRATION)
			s.vibration++;
	}
}

int
DriveBoardF355::save_state (State *state)
{
	return state->put (&s, sizeof (s));
}

int
DriveBoardF355::load_state (State *state)
{
	return state->get (&s, sizeof (s));
}

} /* namespace hl */
