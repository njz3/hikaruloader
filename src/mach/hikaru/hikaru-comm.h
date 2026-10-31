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

#ifndef __HL_HIKARU_COMM_H__
#define __HL_HIKARU_COMM_H__

#include "hl/device.h"
#include "hl/machine.h"
#include "hl/ringlink.h"

namespace hikaru {

/* The communication board, on the external bus's bank 0E; see
 * hikaru-comm.cpp. */
class Comm : public hl::Device {
public:
	explicit Comm (hl::Machine *mach);

	/* addr: the offset in bank 0E */
	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;

	/* A frame's end: the board's own program's work */
	void vblank ();

private:
	enum State {
		STATE_HALTED,	/* its 68000 held in reset */
		STATE_BOOT,	/* just let go */
		STATE_LINK,	/* looking for the ring's nodes */
		STATE_READY,	/* the slots go round */
		STATE_ERROR
	};

	uint16_t get_prog (uint32_t addr) const { return prog[(addr >> 1) & 0x7FFF]; }
	void put_header (uint32_t addr, uint16_t val);
	void link_up ();
	void exchange ();
	void fail (const char *why);

	/* The comm RAM: two banks of 64 KB, the host's and the board's,
	 * which swap at each frame */
	uint8_t ram[2][0x10000];
	unsigned host_bank;
	/* The 68000's RAM, by 16-bit words */
	uint16_t prog[0x8000];
	/* The registers, one per 32-bit word */
	uint16_t regs[0x80];

	State state;
	uint16_t mode;		/* 0 master, 1-7FFF slave, 8000+ relay */
	unsigned slot_size;	/* each node's bytes */
	unsigned nodes, node;	/* the ring's, and ours from 0 */
	unsigned players, player; /* the same, the relays left out */
	uint16_t counter;	/* the frames since the link */

	/* The ring, to the other cabinets; the master's numbering round,
	 * and when it was sent, in frames */
	hl::RingLink link;
	uint32_t round;
	unsigned round_frames;
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_COMM_H__ */
