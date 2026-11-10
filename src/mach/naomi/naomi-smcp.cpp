/*
 * Hikaruloader
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

/*
 * The motion PC of a Cycraft cabinet (SMCP)
 * =========================================
 *
 * A Cycraft cabinet moves on three actuators driven by a small PC of its own
 * (Simuline's, Linux with RTLinux, CAN bus to the drives). The game's Net DIMM
 * reaches it over TCP, as a client, on port 2000 of 192.168.1.1. The PC keeps a
 * status word and takes packets of 48 bytes, a command in the first byte; only
 * the status question has an answer, the 2 bytes of the word. The fields of the
 * packets are little endian:
 *
 *   90            status?      the 16 bits of the status word
 *   91            the version  4 + 4 characters at offset 8: the PC accepts
 *                              its own game's and raises NORMAL
 *   88            the course   2 floats at offset 8 (kept for the motion)
 *   73, 72        go to the neutral, initial position
 *   83            start driving (from the neutral position)
 *   80-82, 86     the driving data (ignored: nothing moves here)
 *   84            stop driving
 *
 * The status word:
 *
 *   bits 0-1      position: 0 unknown, 1 moving, 2 initial, 3 neutral
 *   bit 2         NORMAL
 *   bit 3         a controller failed
 *   bit 4         driving
 *   bit 7         seat belt fastened (or no sensor)  bit 8   cabin stopped
 *   bit 9         safety sensor         bit 10  emergency stop
 *   bit 15        illegal version
 *
 * This peer is a healthy base that has finished its start-up: the commands
 * change the word as the PC's program does, the moves take a time, the safety
 * inputs never trip and the seat belt is taken as fastened, as on a cabinet
 * configured without its sensor. The driving data move nothing: a real base with its PC
 * goes in DIMM_MAP instead.
 */

#include <cstring>

#include "hl/core.h"
#include "mach/naomi/naomi-smcp.h"

namespace naomi {

enum {
	POSITION	= 3,
	MOVING		= 1,
	INITIAL		= 2,
	NEUTRAL		= 3,
	NORMAL		= 1u << 2,
	FAILED		= 1u << 3,
	DRIVING		= 1u << 4,
	SEAT_BELT	= 1u << 7,
	PACKET_SIZE	= 48,
};

/* A move of the motion base between its rest positions */
static const uint64_t MOVE_US = 2000000;

void
SmcpPeer::reset ()
{
	in.clear ();
	out.clear ();
	sent = 0;
	status = INITIAL | SEAT_BELT;
	target = 0;
	move_us = 0;
}

void
SmcpPeer::command (const uint8_t *p)
{
	const uint8_t c = p[0];

	if (c == 0x90) {
		out.push_back ((uint8_t) status);
		out.push_back ((uint8_t) (status >> 8));
		return;
	}
	if (c == 0x91) {
		char version[9] = { 0 };

		memcpy (version, p + 8, 8);
		HL_PRINT ("naomi: motion PC: the game's version is '%s'", version);
		status |= NORMAL;
		return;
	}
	/* The others want a base in working order */
	if ((status & FAILED) || !(status & NORMAL))
		return;
	if (status & DRIVING) {
		if (c == 0x84)
			status &= ~(DRIVING | POSITION);
		return;
	}
	if ((status & POSITION) == NEUTRAL) {
		if (c == 0x83) {
			status |= DRIVING;
			return;
		}
	}
	if ((status & POSITION) != MOVING && (c == 0x73 || c == 0x72)) {
		target = c == 0x73 ? NEUTRAL : INITIAL;
		status = (status & ~(uint32_t) POSITION) | MOVING;
		move_us = MOVE_US;
	}
}

void
SmcpPeer::input (const uint8_t *data, uint32_t size)
{
	in.insert (in.end (), data, data + size);

	size_t at = 0;

	for (; in.size () - at >= PACKET_SIZE; at += PACKET_SIZE)
		command (in.data () + at);
	in.erase (in.begin (), in.begin () + (std::ptrdiff_t) at);
}

uint32_t
SmcpPeer::output (uint8_t *to, uint32_t size)
{
	const size_t left = out.size () - sent;
	const uint32_t n = left < size ? (uint32_t) left : size;

	if (n) {
		memcpy (to, out.data () + sent, n);
		sent += n;
	}
	if (sent == out.size ()) {
		out.clear ();
		sent = 0;
	}
	return n;
}

void
SmcpPeer::tick (uint64_t elapsed_us)
{
	if (!move_us)
		return;
	if (move_us > elapsed_us) {
		move_us -= elapsed_us;
		return;
	}
	move_us = 0;
	status = (status & ~(uint32_t) POSITION) | target;
}

} /* namespace naomi */
