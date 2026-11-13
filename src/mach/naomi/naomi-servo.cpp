/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/midiffb.cpp (GPL-2.0-or-later,
 * Copyright 2025 flyinghead).
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
#include <cctype>
#include <cstring>
#include <string>

#include "hl/core.h"
#include "hl/ffb.h"
#include "hl/jvs.h"
#include "hl/outputs.h"

#include "mach/hikaru/hikaru-aica.h"
#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Servo Board 838-14174
 * =========================
 *
 * The wheel's motor of Club Kart, The King of Route 66 and Driving Simulator,
 * on the AICA's MIDI port. The game sends frames of 4 bytes: a command from
 * 80, two parameters of 7 bits, their checksum. The board answers each one from
 * the address FA set.
 *
 * Two ways: emulated, the effects going to the player's wheel; or relayed, once
 * a client of the network outputs (BackForceFeeder, in front of a real board)
 * answers "midiffb_rx = HH..": every frame goes out as "midiffb = HH HH HH",
 * the wheel takes no effect, and what the game reads is that client's, until
 * it leaves.
 */

enum {
	STATE_NOT_INITIALIZED,
	STATE_SEARCHING_INDEX,
	STATE_READY
};

static const uint32_t SHORT_MODE_MS = 240;
static const uint32_t INDEX_SEARCH_MS = 500;

ServoBoard::ServoBoard (Board *board_) :
	board (board_)
{
	reset ();
}

hikaru::Aica *
ServoBoard::aica () const
{
	return (hikaru::Aica *) board->aica;
}

void
ServoBoard::effect (int which, float force)
{
	hl::ForceFeedback::set ((hl::ForceFeedback::Effect) which, force, board->holly->frames);
}

int
ServoBoard::wheel_position () const
{
	return (int) std::clamp (s.position - s.center, 0.0f, 16383.0f);
}

bool
ServoBoard::holding_back () const
{
	return (s.torque > 0 && s.speed < -1.0f) || (s.torque < 0 && s.speed > 1.0f);
}

/* 81: the most torque driving the wheel (b1) and holding it back (b2) */
void
ServoBoard::apply_torque ()
{
	float limit;

	if (!s.active || s.calibrating || s.relayed)
		return;
	s.holds_back = holding_back ();
	limit = s.params[1][s.holds_back ? 1 : 0] / 127.0f;
	effect (hl::ForceFeedback::CONSTANT, std::clamp (s.torque / 128.0f * s.power, -limit, limit));
}

/* 88 b1: 0 on, 1 off, b2 the inverse of its gain; 8B b1 its force; 87 the
 * offset of its centre, as a torque, about 90 degrees at full scale */
void
ServoBoard::apply_spring ()
{
	float force, gain;
	int offset;

	if (!s.active || s.calibrating || s.relayed)
		return;
	if (s.params[8][0] != 0 || s.params[0xB][0] == 0) {
		effect (hl::ForceFeedback::SPRING, 0.0f);
		return;
	}
	force = std::min (s.params[0xB][0] / 32.0f, 1.0f) * s.power;
	gain = std::min (3.0f / std::max<int> (s.params[8][1], 1), 1.0f);
	offset = ((s.params[7][0] << 7) | s.params[7][1]) - 0x80;
	hl::ForceFeedback::spring_offset (-offset / 128.0f * 0.67f);
	effect (hl::ForceFeedback::SPRING, force * gain);
}

/* 86: the visco speed (b1) and its parameter (b2), acting as their ratio */
void
ServoBoard::apply_damper ()
{
	float ratio;

	if (!s.active || s.calibrating || s.relayed)
		return;
	ratio = s.params[6][1] / (float) std::max<int> (s.params[6][0], 1);
	effect (hl::ForceFeedback::FRICTION, ratio == 0.0f ? 0.0f : s.power * std::min (ratio / 8.0f, 1.0f));
}

/* 85: the frequency (b1, unused on the player's wheel) and the amplitude (b2).
 * The games repeat it each frame */
void
ServoBoard::apply_sine ()
{
	float amplitude;

	if (!s.active || s.relayed)
		return;
	amplitude = (s.params[5][0] == 0 || s.params[5][1] == 0) ? 0.0f : s.params[5][1] / 127.0f * s.power;
	if (amplitude != s.sine) {
		s.sine = amplitude;
		effect (hl::ForceFeedback::VIBRATE, amplitude);
	}
}

void
ServoBoard::stop_effects ()
{
	effect (hl::ForceFeedback::STOP, 0.0f);
	s.sine = -1.0f;
}

void
ServoBoard::apply_effects ()
{
	apply_torque ();
	apply_spring ();
	apply_damper ();
	apply_sine ();
}

/* As at power-up and after a reset (FF); the table as read back from a board
 * (9D index page, then address 1E), a page per line, indices 4 to F at 80 */
void
ServoBoard::reset_board ()
{
	static const uint8_t power_up[16][2] = {
		{ 0x00, 0x00 }, { 0x40, 0x7F }, { 0x04, 0x54 }, { 0x40, 0x04 },
		{ 0x01, 0x00 }, { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x01, 0x00 },
		{ 0x00, 0x03 }, { 0x00, 0x00 }, { 0x20, 0x40 }, { 0x00, 0x00 },
		{ 0x00, 0x00 }, { 0x01, 0x01 }, { 0x00, 0x01 }, { 0x00, 0x01 },
	};
	static const uint16_t table[16][4] = {
		{ 0x20, 0xC0, 0x80, 0x80 }, { 0x20, 0xB0, 0x80, 0x80 }, { 0x20, 0xA0, 0x80, 0x80 }, { 0x20, 0x90, 0x80, 0x80 },
		{ 0x30, 0xAB, 0x80, 0x80 }, { 0x40, 0xA0, 0x80, 0x80 }, { 0x40, 0x90, 0x80, 0x80 }, { 0x60, 0x95, 0x80, 0x80 },
		{ 0x10, 0xC0, 0x40, 0x80 }, { 0x10, 0xA0, 0x60, 0x80 }, { 0x10, 0xC0, 0xC0, 0x40 }, { 0x10, 0xA0, 0xA0, 0x60 },
		{ 0x10, 0xC0, 0x40, 0x40 }, { 0x10, 0xA0, 0x60, 0x60 }, { 0x10, 0xC0, 0x40, 0xC0 }, { 0x10, 0xA0, 0x60, 0xA0 },
	};
	int page, index;

	memcpy (s.params, power_up, sizeof (s.params));
	for (page = 0; page < 16; page++)
		for (index = 0; index < 16; index++)
			s.table[page * 16 + index] = index < 4 ? table[page][index] : 0x80;
	s.power = s.params[3][0] / 127.0f;
	s.torque = 0;
	s.short_mode = false;
	s.dropped = false;
	s.reply_address = 0x1F;
	s.state = STATE_NOT_INITIALIZED;
}

void
ServoBoard::reset ()
{
	bool relayed = s.relayed;

	memset (&s, 0, sizeof (s));
	s.relayed = relayed;
	s.position = 8192.0f;
	s.sine = -1.0f;
	reset_board ();
}

void
ServoBoard::send (uint8_t b1, uint8_t b2, uint8_t b3)
{
	hikaru::Aica *a = aica ();

	a->midi_send (b1);
	a->midi_send (b2);
	a->midi_send (b3);
	a->midi_send ((b1 ^ b2 ^ b3) & 0x7F);
}

/* The answer to a frame: from 80 once the index is found, A0 before */
void
ServoBoard::reply ()
{
	uint8_t base = s.state == STATE_READY ? 0x80 : 0xA0;

	switch (s.reply_address) {
	case 0x1F:
		/* 00 ready, 11 not initialized, 44 searching the index */
		aica ()->midi_send (s.state == STATE_READY ? 0x00 : s.state == STATE_SEARCHING_INDEX ? 0x44 : 0x11);
		break;
	case 0x10:
		send (base | 0x10, 0, s.state == STATE_READY ? 0 : s.state == STATE_SEARCHING_INDEX ? 4 : 1);
		break;
	case 0x11:
		send (base | 0x11, 0, 1);
		break;
	case 0x13:
		/* The wheel's speed: 01 00 is none */
		send (base | 0x13, 1, 0);
		break;
	case 0x14: {
		int pos = wheel_position ();

		send (base | 0x14, (pos >> 7) & 0x7F, pos & 0x7F);
		break;
	}
	case 0x1D:
		send (base | 0x1D, s.select[0], s.select[1]);
		break;
	case 0x1E: {
		uint16_t v = s.table[(s.select[1] * 16 + s.select[0]) & 0xFF];

		send (base | 0x1E, (v >> 7) & 0x7F, v & 0x7F);
		break;
	}
	case 0x00:
		send (base, 1, 0);
		break;
	default:
		if (s.reply_address < 0x10)
			send (base | s.reply_address, s.params[s.reply_address][0], s.params[s.reply_address][1]);
		else
			send (base | (s.reply_address & 0x1F), 1, 0);
		break;
	}
}

void
ServoBoard::process (uint8_t cmd, uint8_t b1, uint8_t b2)
{
	if (cmd < 0x90) {
		s.params[cmd & 15][0] = b1;
		s.params[cmd & 15][1] = b2;
	}
	switch (cmd) {
	case 0x80:
		/* b1 0 permanent, 1 short; b2 on or off */
		s.short_mode = b1 & 1;
		if (b2 == 0) {
			s.active = false;
			if (!s.relayed)
				stop_effects ();
			if (s.calibrating) {
				s.calibrating = false;
				((hl::SegaIoBoard *) board->io_board)->analog_set[0] = false;
				HL_PRINT ("servo board: calibration done");
			}
		} else {
			s.active = true;
			apply_effects ();
		}
		break;
	case 0x81:
		apply_torque ();
		break;
	case 0x83:
		s.power = b1 / 127.0f;
		apply_effects ();
		break;
	case 0x84:
		/* 01 00 none, 01 7F full left, 00 01 full right */
		s.torque = ((b1 << 7) | b2) - 0x80;
		s.dropped = false;
		apply_torque ();
		break;
	case 0x85:
		apply_sine ();
		break;
	case 0x86:
		/* A visco speed of 0 is kept as 1 */
		if (b1 == 0)
			s.params[6][0] = 1;
		apply_damper ();
		break;
	case 0x87:
	case 0x88:
	case 0x8B:
		apply_spring ();
		break;
	case 0x9D:
		s.select[0] = b1;
		s.select[1] = b2;
		break;
	case 0x9E:
		s.table[(s.select[1] * 16 + s.select[0]) & 0xFF] = (uint16_t) ((b1 << 7) | b2);
		break;
	case 0xF0:
		/* Here is the centre, 2000 */
		s.center = s.position - 0x2000;
		break;
	case 0xFA:
		s.reply_address = b2;
		break;
	case 0xFC:
		/* The index's search: busy for a while */
		s.state = STATE_SEARCHING_INDEX;
		s.search_end = s.now_ms + INDEX_SEARCH_MS;
		break;
	case 0xFF:
		reset_board ();
		s.active = false;
		if (!s.relayed) {
			stop_effects ();
			effect (hl::ForceFeedback::SPRING, 0.8f);
		}
		HL_PRINT ("servo board: calibrating, keep the wheel centred");
		s.calibrating = true;
		s.position = 8192.0f;
		s.center = 0.0f;
		break;
	default:
		break;
	}
}

/* A byte the game wrote to the MIDI port */
void
ServoBoard::midi_received (uint8_t data)
{
	if (data & 0x80)
		s.received = 0;
	s.frame[s.received] = data;
	if (s.received == 3 && ((s.frame[0] ^ s.frame[1] ^ s.frame[2]) & 0x7F) == s.frame[3]) {
		uint8_t cmd = s.frame[0];

		s.last_frame_ms = s.now_ms;
		/* HR_LOG_SERVO=1: each command whose parameters change */
		static const bool log_servo = getenv ("HR_LOG_SERVO") != NULL;
		static uint16_t last[128];
		if (log_servo && last[cmd & 0x7F] != (0x8000 | s.frame[1] << 7 | s.frame[2])) {
			last[cmd & 0x7F] = (uint16_t) (0x8000 | s.frame[1] << 7 | s.frame[2]);
			HL_PRINT ("servo board: %02X %02X %02X (ms %u, %s%s)", cmd, s.frame[1], s.frame[2], (unsigned) s.now_ms,
			          s.active ? "active" : "off", s.calibrating ? ", calibrating" : "");
		}
		process (cmd, s.frame[1], s.frame[2]);
		if (!s.calibrating) {
			/* The wheel, its encoder the other way */
			hl::SegaIoBoard *io = (hl::SegaIoBoard *) board->io_board;
			float next = std::clamp (8192.0f - ((int) io->analog[0] - 0x8000) / 4.0f, 0.0f, 16383.0f);

			s.speed = next - s.position;
			s.position = next;
			if (s.active && !s.dropped && s.torque != 0 && holding_back () != s.holds_back)
				apply_torque ();
		}
		if (!s.relayed)
			reply ();
		/* The status queries, only when a real board answers them */
		if (cmd != 0xFD || s.relayed)
			hl::Outputs::get ()->midi_frame (s.frame[0], s.frame[1], s.frame[2]);
	}
	s.received = (s.received + 1) % 4;
}

static void
midi_out (void *user, uint8_t data)
{
	((ServoBoard *) user)->midi_received (data);
}

void
ServoBoard::attach ()
{
	aica ()->midi_out = midi_out;
	aica ()->midi_user = this;
	hl::Outputs::get ()->midi_board ();
}

/* Each millisecond: the index found, the short mode's end, a client's answers */
void
ServoBoard::tick ()
{
	hl::Outputs *outputs = hl::Outputs::get ();
	std::string answer;

	s.now_ms++;
	/* Calibrating: the games check that the wheel turns with the motor, its
	 * potentiometer following, whatever the player's wheel does. The motor
	 * off, the wheel is free: the player's, the model following it (from a
	 * reset until the motor is on; all through the games' test modes, which
	 * reset the board and leave it off) */
	if (s.calibrating) {
		hl::SegaIoBoard *io = (hl::SegaIoBoard *) board->io_board;

		if (s.active) {
			/* The wheel's friction near its centre: the games' last
			 * small pulses, to settle it there, hardly move it; away
			 * from it they must still bring it back */
			const float friction = std::abs (s.position - 8192.0f) < 256.0f ? 12.0f : 0.0f;
			const float moving = std::abs ((float) s.torque) <= friction ? 0.0f :
			                     s.torque - std::copysign (friction, (float) s.torque);

			s.position = std::clamp (s.position + moving * 0.25f, 0.0f, 16383.0f);
			io->analog[0] = (uint16_t) std::clamp (0x8000 - (int) ((s.position - 8192.0f) * 4.0f), 0, 0xFFFF);
			io->analog_set[0] = true;
		} else {
			io->analog_set[0] = false;
			s.position = std::clamp (8192.0f - ((int) io->analog[0] - 0x8000) / 4.0f, 0.0f, 16383.0f);
		}
	}
	if (s.state == STATE_SEARCHING_INDEX && s.now_ms >= s.search_end)
		s.state = STATE_READY;
	if (s.active && s.short_mode && !s.dropped && s.torque != 0 && !s.relayed &&
	    s.now_ms - s.last_frame_ms > SHORT_MODE_MS) {
		s.dropped = true;
		effect (hl::ForceFeedback::CONSTANT, 0.0f);
	}
	if (!outputs->midi_relayed ()) {
		if (s.relayed) {
			s.relayed = false;
			HL_PRINT ("servo board: emulated from now on");
		}
	} else if (!s.relayed) {
		/* What was sent until now has been answered here */
		s.relayed = true;
		stop_effects ();
		while (outputs->midi_answer (&answer))
			;
		HL_PRINT ("servo board: the answers now come from a client's real board (midiffb_rx)");
	} else {
		while (outputs->midi_answer (&answer)) {
			size_t i;

			/* "9440 0054" or "94 40 00 54" too */
			answer.erase (std::remove_if (answer.begin (), answer.end (),
			                              [] (char c) { return !isxdigit ((unsigned char) c); }),
			              answer.end ());
			for (i = 0; i + 1 < answer.size (); i += 2)
				aica ()->midi_send ((uint8_t) strtoul (answer.substr (i, 2).c_str (), NULL, 16));
		}
	}
}

int
ServoBoard::save_state (hl::State *state)
{
	return state->put (&s, sizeof (s));
}

int
ServoBoard::load_state (hl::State *state)
{
	return state->get (&s, sizeof (s));
}

} /* namespace naomi */
