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

#include "hl/core.h"
#include "hl/config.h"
#include "hl/state.h"

#include "mach/hikaru/hikaru-comm.h"

namespace hikaru {

/*
 * Communication Board
 * ===================
 *
 * The SAMURAI COMMUNICATION BD (837-13404), on top of the ROM board, links
 * the cabinets of PHARRIER (2, a twin cabinet), SGNASCAR (up to 8, and a
 * relay for a monitor), SWRACER (up to 4) and VON4: each board's optical
 * TX to the next one's RX, in a ring. AIRTRIX and BRAVEFF have no link.
 * It has its own 68000, whose program the game uploads.
 *
 * The BIOS finds it with its comm RAM (BIOS 0.92 @000079CC: bank 0E's 64
 * first bytes written and read back) and then shows the COMMUNICATION MODE
 * of its SYSTEM ASSIGNMENTS: NO LINK, MASTER, SLAVE, RELAY (byte 9 of the
 * EEPROM's settings, 0 to 3). The games use the board unless NO LINK.
 *
 * Bank 0E
 * =======
 *
 *  00000-0FFFF	Comm RAM, the host's bank (32-bit wide: the BIOS tests it
 *		with 32-bit patterns)
 *  10000-101FF	Registers, 16-bit, one per 32-bit word
 *  20000-3FFFF	The 68000's RAM (64 KB), a 16-bit word per 32-bit word:
 *		its vectors first (SSP FF00, PC 280)
 *
 * The DMA reaches it as 8E (the MSB dropped) too.
 *
 * Registers
 * =========
 *
 *  Offset	68000's	Meaning
 *  +000	00	Written 1 at the start (SWRA:@0C150A00)
 *  +080	40	Written 1 at the start
 *  +110	88	Status 0, the host's and the 68000's mailbox
 *  +114	8A	Status 1: the link's state
 *  +180	C0	The 68000's reset: 0 holds it, else it runs
 *
 * Start (SWRA:@0C150A00, @0C150E44)
 * =================================
 *
 * The game holds the 68000, tests the comm RAM, uploads the program, and
 * writes its parameters into the 68000's RAM:
 *
 *  200	mode: 0 master, 1-7FFF slave, 8000 and up relay
 *  204	the bytes each node sends a frame (a slot; SWRACER: CA0)
 *  208	the game's ID (4 bytes: "BBP-")
 *  20C	two more words (SWRACER: 0105, 0806)
 *
 * It then writes 7FFF to status 1, lets the 68000 go, and polls status 1
 * (SWRA:@0C150E9E) a frame at a time: still 7FFF, the board is starting;
 * 0, it looks for the ring's nodes; bit 15, an error; else the ring is up,
 * the nodes in bits 8-12 and this node's number (from 0) in bits 0-4.
 * Then it reads 16 bytes of the 68000's RAM at 1F000 (bytes 1 and 4 kept),
 * and checks the nodes' slots for 120 frames: their game and version IDs
 * (NODE#  GAME-ID  VER-ID), errors A000/9000 with the node's bit.
 *
 * Each frame (SWRA:@0C15109C) the game DMAs its own slot to comm RAM 100
 * and the others' from 100 + slot, the nodes' in a row, and copies the
 * header's 32 bytes at 0: its word at 4 changes each frame.
 *
 * The board's program is not run here ([Network] COMM_BOARD = SIMULATED,
 * the default): the board is emulated from what it gives the game. With
 * COMM_BOARD = PROGRAM, hikaru-commbd.cpp runs it on the board's 68000,
 * for more than two cabinets. The header, in the 68000's words (a 16-bit
 * word at h of the host is the 68000's at h ^ 2, the 32-bit words alike):
 *
 *  0	the mode
 *  2	the nodes (bits 8-15, and 0-7)
 *  4	this node's number (bits 8-15, and 0-7)
 *  6	the frames since the link
 *
 * The ring (hl::RingLink, [Network] in the ini) is a shift register:
 * each frame, each node sends its slots one by one to the next, its own
 * first, and gets the previous node's after its own; its own comes back
 * last, so the host reads as many slots as nodes from 100 + slot. A
 * master alone is its own next node: SWRACER then runs as NODE ID 1/1.
 *
 * Before that, the master numbers the nodes: its ENUM goes round, each
 * node taking the number after the previous one's (a relay, not playing,
 * the same player number), back to the master with the last; its COUNT
 * then tells them all. A node that misses a frame's slot for 5 seconds
 * breaks the link (status 1 bit 15); the game starts the board again.
 *
 * Games
 * =====
 *
 * Two instances on one PC (October 2026), each a seat of its own:
 *
 *  SWRACER	slots of CA0; LINK GAME OK in the attract, a race of two
 *		(POSITION 1st/2, 2nd/2). Each cabinet its SEAT NUMBER (GAME
 *		ASSIGNMENTS, shown when linked; the backup RAM's 407), else
 *		CONFLICTING SEAT NUMBERS
 *  SGNASCAR	slots of 740; PLEASE WAIT FOR OTHER DRIVERS, the master's
 *		track for both, a race of two, each seeing the other's car
 *  PHARRIER	slots of 5C0; a twin cabinet's game for two (Would you
 *		like to play with Player 2?), both players in each picture
 *  VON4	slots of 1200, then 2400 (it starts the board twice); each
 *		cabinet its SEAT LETTER (GAME ASSIGNMENTS; the EEPROM's byte
 *		30), else SEAT SETTING IS WRONG; a versus of two, each the
 *		other's HUMAN enemy, each one's life the other's enemy's
 *
 * A game entering its test menu holds its board: the other nodes lose the
 * link at once, and their games start their boards again (SGNASCAR: some
 * 40 frames later), which number the ring again once all are back.
 */

#define MSG_ENUM	0x4D554E45	/* "ENUM": round, node, player, slot */
#define MSG_COUNT	0x544E4F43	/* "CONT": round, nodes, players */
#define MSG_DATA	0x41544144	/* "DATA": a slot */

/* How long a frame waits for the previous node's slot, in ms; the frames
 * between two ENUMs of the master */
#define DATA_TIMEOUT	5000
#define ROUND_FRAMES	120

/* With the frame, for the nodes' logs side by side */
#define COMM_PRINT(fmt_, ...) \
	HL_PRINT ("comm: frame %llu: " fmt_, (unsigned long long) mach->frame, ##__VA_ARGS__)

#define REG_STATUS0	0x110
#define REG_STATUS1	0x114
#define REG_RUN		0x180

Comm::Comm (hl::Machine *mach) :
	hl::Device (mach)
{
}

void
Comm::put_header (uint32_t addr, uint16_t val)
{
	uint8_t *p = ram[host_bank ^ 1] + (addr ^ 2);
	p[0] = val & 0xFF;
	p[1] = val >> 8;
}

int
Comm::get (unsigned size, uint32_t addr, void *val)
{
	uint64_t v = 0;

	addr &= 0xFFFFFF;
	if (addr < 0x10000) {
		memcpy (&v, ram[host_bank] + addr, MIN2 (size, 0x10000 - addr));
	} else if (addr < 0x10200) {
		v = regs[(addr >> 2) & 0x7F];
		HL_LOG ("COMM R%u %05X = %llX", size * 8, addr, (unsigned long long) v);
	} else if (addr >= 0x20000 && addr < 0x40000) {
		v = prog[(addr - 0x20000) >> 2];
	} else {
		HL_LOG ("COMM R%u %05X: unknown", size * 8, addr);
		return -1;
	}
	set_ptr (val, size, v);
	return 0;
}

int
Comm::put (unsigned size, uint32_t addr, uint64_t val)
{
	addr &= 0xFFFFFF;
	if (addr < 0x10000) {
		memcpy (ram[host_bank] + addr, &val, MIN2 (size, 0x10000 - addr));
	} else if (addr < 0x10200) {
		unsigned reg = (addr >> 2) & 0x7F;
		HL_LOG ("COMM W%u %05X = %llX", size * 8, addr, (unsigned long long) val);
		if ((addr & 0x1FC) == REG_RUN) {
			if (!val) {
				if (state != STATE_HALTED)
					COMM_PRINT ("the game holds the board");
				state = STATE_HALTED;
				link.close ();
			}
			else if (!regs[reg])
				state = STATE_BOOT;
		}
		regs[reg] = (uint16_t) val;
	} else if (addr >= 0x20000 && addr < 0x40000) {
		prog[(addr - 0x20000) >> 2] = (uint16_t) val;
	} else {
		HL_LOG ("COMM W%u %05X = %llX: unknown", size * 8, addr, (unsigned long long) val);
		return -1;
	}
	return 0;
}

static void
put32 (uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = v >> 24;
}

static uint32_t
get32 (const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

void
Comm::fail (const char *why)
{
	HL_ERROR ("comm: frame %llu: %s; the link is down",
	          (unsigned long long) mach->frame, why);
	regs[REG_STATUS1 >> 2] = 0x8000;
	link.close ();
	state = STATE_ERROR;
}

/* The numbering, a step a frame without waiting */
void
Comm::link_up ()
{
	std::vector<uint8_t> data;
	uint8_t msg[12];
	uint32_t type;

	if (!link.poll ())
		return;
	if (!mode && (!round || ++round_frames >= ROUND_FRAMES)) {
		/* The master is node 0, player 0 */
		round++;
		round_frames = 0;
		put32 (msg, round);
		msg[4] = 0;
		msg[5] = 0;
		msg[6] = slot_size & 0xFF;
		msg[7] = slot_size >> 8;
		link.send (MSG_ENUM, msg, 8);
	}
	while (link.receive (&type, data, 0)) {
		if (data.size () < 6 || (!mode && get32 (&data[0]) != round))
			continue;
		if (type == MSG_ENUM && data.size () >= 8) {
			if ((unsigned) (data[6] | (data[7] << 8)) != slot_size) {
				fail ("the nodes' slots differ: another game?");
				return;
			}
			if (!mode) {
				/* Back from the last node */
				nodes = data[4] + 1;
				players = data[5] + 1;
				put32 (msg, round);
				msg[4] = (uint8_t) nodes;
				msg[5] = (uint8_t) players;
				link.send (MSG_COUNT, msg, 6);
			} else {
				round = get32 (&data[0]);
				node = data[4] + 1;
				player = data[5] + (mode < 0x8000 ? 1 : 0);
				data[4] = (uint8_t) node;
				data[5] = (uint8_t) player;
				link.send (MSG_ENUM, &data[0], (uint32_t) data.size ());
			}
		} else if (type == MSG_COUNT) {
			if (mode && get32 (&data[0]) != round)
				continue;
			nodes = data[4];
			players = data[5];
			if (mode)
				link.send (MSG_COUNT, &data[0], (uint32_t) data.size ());
			else
				node = player = 0;
			counter = 0;
			regs[REG_STATUS1 >> 2] = (uint16_t) ((players << 8) | player |
			                                     (mode >= 0x8000 ? 0x80 : 0));
			COMM_PRINT ("the ring is up, node %u of %u, player %u of %u%s",
			          node + 1, nodes, player + 1, players,
			          mode >= 0x8000 ? " (a relay)" : "");
			state = STATE_READY;
			return;
		}
	}
	if (link.broken ())
		fail ("a node left while numbering");
}

/* A frame of the ring: the slots go round, the banks swap */
void
Comm::exchange ()
{
	/* The bank the host filled a frame ago */
	uint8_t *bank = ram[host_bank ^ 1];
	std::vector<uint8_t> data;
	uint32_t type;
	unsigned i;

	counter++;
	put_header (0x0, mode);
	put_header (0x2, (uint16_t) ((players << 8) | nodes));
	put_header (0x4, (uint16_t) ((player << 8) | node));
	put_header (0x6, counter);
	for (i = 0; i < nodes; i++) {
		unsigned at = 0x100 + (i + 1) * slot_size;
		if (!link.send (MSG_DATA, bank + 0x100 + i * slot_size, slot_size)) {
			fail ("the next node left");
			return;
		}
		/* The numbering's last messages, from rounds the master sent
		 * again, may still come first */
		do {
			if (!link.receive (&type, data, DATA_TIMEOUT)) {
				fail (link.broken () ? "the previous node left" :
				      "no slot from the previous node");
				return;
			}
		} while (type != MSG_DATA);
		if (!data.empty () && at + data.size () <= 0x10000)
			memcpy (bank + at, &data[0], data.size ());
	}
	host_bank ^= 1;
}

void
Comm::vblank ()
{
	const hl::Config *cfg = hl::Config::get ();

	switch (state) {
	case STATE_HALTED:
	case STATE_ERROR:
		break;
	case STATE_BOOT:
		/* The program's parameters */
		mode = get_prog (0x200);
		slot_size = get_prog (0x204);
		COMM_PRINT ("the board starts as %s, slots of %u bytes",
		          !mode ? "master" : mode < 0x8000 ? "slave" : "relay",
		          slot_size);
		memset (ram, 0, sizeof (ram));
		regs[REG_STATUS0 >> 2] = 0;
		regs[REG_STATUS1 >> 2] = 0;
		nodes = node = players = player = 0;
		round = 0;
		round_frames = 0;
		if (!slot_size || 0x100 + 2 * slot_size > 0x10000)
			fail ("its slots don't fit");
		else if (!cfg->link)
			/* NO_LINK: no socket, nor the firewall's question */
			fail ("LINK = NO_LINK");
		else if (!link.is_open () &&
		         !link.open (cfg->port_in, cfg->address_out, cfg->port_out))
			fail ("no ring");
		else
			state = STATE_LINK;
		break;
	case STATE_LINK:
		link_up ();
		break;
	case STATE_READY:
		exchange ();
		break;
	}
}

void
Comm::reset (hl::ResetType type)
{
	(void) type;
	link.close ();
	memset (ram, 0, sizeof (ram));
	memset (prog, 0, sizeof (prog));
	memset (regs, 0, sizeof (regs));
	host_bank = 0;
	state = STATE_HALTED;
	mode = 0;
	slot_size = 0;
	nodes = node = players = player = 0;
	counter = 0;
	round = 0;
	round_frames = 0;
}

int
Comm::save_state (hl::State *state_)
{
	int ret = 0;
	ret |= state_->put (ram, sizeof (ram));
	ret |= state_->put (&host_bank, sizeof (host_bank));
	ret |= state_->put (prog, sizeof (prog));
	ret |= state_->put (regs, sizeof (regs));
	ret |= state_->put (&state, sizeof (state));
	ret |= state_->put (&mode, sizeof (mode));
	ret |= state_->put (&slot_size, sizeof (slot_size));
	ret |= state_->put (&nodes, sizeof (nodes));
	ret |= state_->put (&node, sizeof (node));
	ret |= state_->put (&counter, sizeof (counter));
	return ret;
}

int
Comm::load_state (hl::State *state_)
{
	int ret = 0;
	ret |= state_->get (ram, sizeof (ram));
	ret |= state_->get (&host_bank, sizeof (host_bank));
	ret |= state_->get (prog, sizeof (prog));
	ret |= state_->get (regs, sizeof (regs));
	ret |= state_->get (&state, sizeof (state));
	ret |= state_->get (&mode, sizeof (mode));
	ret |= state_->get (&slot_size, sizeof (slot_size));
	ret |= state_->get (&nodes, sizeof (nodes));
	ret |= state_->get (&node, sizeof (node));
	ret |= state_->get (&counter, sizeof (counter));
	/* The ring is not in the state: numbered again */
	if (state == STATE_LINK || state == STATE_READY)
		state = STATE_BOOT;
	return ret;
}

const char *
Comm::debug_string ()
{
	static char out[96];
	static const char *names[] = { "halted", "boot", "link", "ready", "error" };
	snprintf (out, sizeof (out), "%s, status %04X %04X, %u/%u nodes",
	          names[state], regs[REG_STATUS0 >> 2], regs[REG_STATUS1 >> 2],
	          node, nodes);
	return out;
}

} /* namespace hikaru */
