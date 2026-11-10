/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/m3comm.cpp (license BSD-3-Clause,
 * copyright-holders MetalliC), and Flycast's core/hw/naomi/naomi_m3comm.cpp and
 * core/network/naomi_network.cpp (GPL-2.0-or-later, Copyright 2020-2022
 * flyinghead).
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
#include <cstring>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/outputs.h"
#include "hl/ringlink.h"
#include "hl/state.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Communication Board
 * =======================
 *
 *  5F7018	The control: bit 0 the 68000's RAM (else the communication RAM)
 *		through the data port, 5 the 68000 out of reset, 14 clear: the
 *		board has the G1's DMA
 *  5F701C	The offset of the data port
 *  5F7020	The data: 16 bits, big endian, the offset going on by 2
 *  5F7024,28	Status
 *
 * Its 68000 is not run: the board is emulated from what it gives the games, its
 * program's work done here. They load it in the 68000's RAM, with these among
 * its parameters (big endian):
 *
 *  200		the mode: 0 master, else slave
 *  204		the bytes each node sends a frame (a slot)
 *
 * then let it go. The header at the start of the communication RAM, which the
 * games read each frame (big endian):
 *
 *  0		the mode (0 master, 1 slave)
 *  2		the nodes (in both bytes)
 *  4		this node's number (in both bytes)
 *  6		the frames since the link, the number in the previous node's
 *  8		timers and DMA addresses, for the games' debugging
 *
 * status 1 and 2 tell the game the link is up: FF01 and (nodes << 8) | node;
 * both 0 for no link. At F000, the first words of the 68000's parameters.
 *
 * The ring (each board's optical TX to the next one's RX) is a shift register.
 * The game writes its slot at 100, reads the nodes' slots from 100 + slot:
 * each frame the node sends its whole area of (nodes * slot) bytes, and what
 * comes in is put from 100 + slot, the previous nodes' ahead. Its own comes back
 * after the turn of the ring.
 *
 * The rings are CommRing's: a NetworkRing between two emulators, over TCP as the
 * Hikaru's does (hl::RingLink, [Network] in the ini, PORT_IN, ADDRESS_OUT,
 * PORT_OUT), the Hikaru's numbering (the master's ENUM goes round, each node
 * taking the next number, then its COUNT tells them all) and exchange (a node
 * sends then waits, 5 seconds at most, for the previous one's frame: the
 * emulators go on together). A LocalRing between the boards of one machine (a
 * cabinet of several whole systems): they all stop every SLICE_LINES lines, their
 * frames left in letter boxes for the next meeting.
 *
 * The games' link settings are their own (a seat in a test menu, a record of the
 * EEPROM): LINK = NO_LINK, MASTER, SLAVE writes those; the games not listed have
 * theirs set in their menus. LINK = AUTO leaves them: F355 Twin's backup memory
 * (data/naomi) says it is not linked, and a board with nobody at the other end,
 * the game not asking, stays alone.
 */

enum {
	COMM_CTRL	= 0x5F7018,
	COMM_OFFSET	= 0x5F701C,
	COMM_DATA	= 0x5F7020,
	COMM_STATUS1	= 0x5F7024,
	COMM_STATUS2	= 0x5F7028,
};

static const uint32_t RAM_SIZE = 128 * 1024;
static const uint16_t CTRL_CPU_RAM = 1 << 0, CTRL_RESET = 1 << 5, CTRL_G1DMA = 1 << 14;

static inline uint16_t
swap16 (uint16_t w)
{
	return (uint16_t) ((w >> 8) | (w << 8));
}

static inline void
put_be16 (uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}

/*
 * The Rings
 */

#define MSG_ENUM	0x4D554E45	/* "ENUM": round, node, slot */
#define MSG_COUNT	0x544E4F43	/* "CONT": round, nodes */
#define MSG_DATA	0x41544144	/* "DATA": number, the frame */

/* How long a frame waits for the previous node's, in ms; the frames between
 * two ENUMs of the master */
#define DATA_TIMEOUT	5000
#define ROUND_FRAMES	120

static void
put32 (uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t) (v >> (8 * i));
}

static uint32_t
get32 (const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Between two emulators */
class NetworkRing : public CommRing {
public:
	explicit NetworkRing (hl::Machine *mach_) : mach (mach_) {}

	bool start (uint16_t mode_, uint16_t slot) override
	{
		const hl::Config *cfg = hl::Config::get ();

		/* LINK says it, else what the game gave the board */
		mode = cfg->link > 0 ? (cfg->link == 1 ? 0 : 1) : mode_;
		slot_size = slot;
		node = 0;
		nodes = 0;
		round = round_frames = 0;
		failed = false;
		if (!link.open (cfg->port_in, cfg->address_out, cfg->port_out)) {
			failed = true;
			return false;
		}
		return true;
	}

	void stop () override { link.close (); }

	bool numbered (unsigned *node_, unsigned *nodes_) override
	{
		std::vector<uint8_t> data;
		uint8_t msg[8];
		uint32_t type;

		if (nodes) {
			*node_ = node;
			*nodes_ = nodes;
			return true;
		}
		if (!link.poll ())
			return false;
		if (!mode && (!round || ++round_frames >= ROUND_FRAMES)) {
			/* The master is node 0 */
			round++;
			round_frames = 0;
			put32 (msg, round);
			msg[4] = 0;
			msg[5] = 0;
			msg[6] = (uint8_t) slot_size;
			msg[7] = (uint8_t) (slot_size >> 8);
			link.send (MSG_ENUM, msg, 8);
		}
		while (link.receive (&type, data, 0)) {
			if (data.size () < 6 || (!mode && get32 (&data[0]) != round))
				continue;
			if (type == MSG_ENUM && data.size () >= 8) {
				if ((unsigned) (data[6] | (data[7] << 8)) != slot_size) {
					HL_ERROR ("comm: the nodes' slots differ: another game?");
					failed = true;
					return false;
				}
				if (!mode) {
					/* Back from the last node */
					put32 (msg, round);
					msg[4] = data[4] + 1;
					link.send (MSG_COUNT, msg, 6);
				} else {
					round = get32 (&data[0]);
					node = data[4] + 1;
					data[4] = (uint8_t) node;
					link.send (MSG_ENUM, &data[0], (uint32_t) data.size ());
				}
			} else if (type == MSG_COUNT) {
				if (mode) {
					if (get32 (&data[0]) != round)
						continue;
					link.send (MSG_COUNT, &data[0], (uint32_t) data.size ());
				} else
					node = 0;
				nodes = data[4];
				HL_PRINT ("comm: frame %llu: the ring is up, node %u of %u",
				          (unsigned long long) mach->frame, node + 1, nodes);
				*node_ = node;
				*nodes_ = nodes;
				return true;
			}
		}
		if (link.broken ()) {
			HL_ERROR ("comm: frame %llu: a node left while numbering",
			          (unsigned long long) mach->frame);
			failed = true;
		}
		return false;
	}

	bool exchange (const uint8_t *out, uint8_t *in, uint32_t size,
	               uint16_t number, uint16_t *number_in) override
	{
		std::vector<uint8_t> data, msg (size + 2);
		uint32_t type;

		msg[0] = (uint8_t) number;
		msg[1] = (uint8_t) (number >> 8);
		memcpy (&msg[2], out, size);
		if (!link.send (MSG_DATA, msg.data (), (uint32_t) msg.size ())) {
			fail ("the next node left");
			return false;
		}
		/* The numbering's last messages, from rounds the master sent
		 * again, may still come first */
		do {
			if (!link.receive (&type, data, DATA_TIMEOUT)) {
				fail (link.broken () ? "the previous node left" :
				      "no frame from the previous node");
				return false;
			}
		} while (type != MSG_DATA);
		if (data.size () >= 2) {
			*number_in = (uint16_t) (data[0] | (data[1] << 8));
			memcpy (in, &data[2], std::min ((size_t) size, data.size () - 2));
		}
		return true;
	}

	bool broken () const override { return failed; }

private:
	void fail (const char *why)
	{
		HL_ERROR ("comm: frame %llu: %s; the link is down",
		          (unsigned long long) mach->frame, why);
		failed = true;
	}

	hl::Machine *mach;
	hl::RingLink link;
	uint16_t mode = 0, slot_size = 0;
	unsigned node = 0, nodes = 0;
	uint32_t round = 0, round_frames = 0;
	bool failed = false;
};

/* Between the boards of one machine: called with all of them stopped */
class LocalRing : public CommRing {
public:
	LocalRing (CommBoxes *boxes_, unsigned index_) :
		boxes (boxes_), index (index_) {}

	bool start (uint16_t mode, uint16_t slot) override
	{
		(void) mode; (void) slot;
		boxes->boxes[index].full = false;
		boxes->started |= 1u << index;
		return true;
	}

	void stop () override {}

	/* Not before every board has started: as they are all powered on
	 * together, the games' negotiation starting then */
	bool numbered (unsigned *node, unsigned *nodes) override
	{
		*node = index;
		*nodes = (unsigned) boxes->boxes.size ();
		return boxes->started == (1u << *nodes) - 1;
	}

	/* What came first, to send it on with the frame */
	bool exchange (const uint8_t *out, uint8_t *in, uint32_t size,
	               uint16_t number, uint16_t *number_in) override
	{
		CommBoxes::Box &mine = boxes->boxes[index];
		CommBoxes::Box &next = boxes->boxes[(index + 1) % boxes->boxes.size ()];
		bool got = mine.full;

		if (got) {
			memcpy (in, mine.data.data (), std::min ((size_t) size, mine.data.size ()));
			*number_in = mine.number;
			mine.full = false;
		}
		next.data.assign (out, out + size);
		next.number = number;
		next.full = true;
		return got;
	}

	bool broken () const override { return false; }

private:
	CommBoxes *boxes;
	unsigned index;
};

CommRing *
comm_network_ring (hl::Machine *mach)
{
	return new NetworkRing (mach);
}

CommRing *
comm_local_ring (CommBoxes *boxes, unsigned index)
{
	return new LocalRing (boxes, index);
}

/*
 * The Board
 */

CommBoard::CommBoard () :
	m68k_ram (RAM_SIZE),
	comm_ram (RAM_SIZE),
	ring_state (RING_OFF),
	node (0),
	nodes (0),
	packet (0)
{
	reset ();
}

void
CommBoard::attach (CommRing *ring_)
{
	ring.reset (ring_);
}

void
CommBoard::reset ()
{
	std::fill (m68k_ram.begin (), m68k_ram.end (), 0);
	std::fill (comm_ram.begin (), comm_ram.end (), 0);
	control = 0xC000;
	offset = status1 = status2 = 0;
	if (ring && ring_state != RING_OFF)
		ring->stop ();
	ring_state = RING_OFF;
	node = nodes = 0;
	packet = 0;
}

uint16_t
CommBoard::slot_size () const
{
	return (uint16_t) ((m68k_ram[0x204] << 8) | m68k_ram[0x205]);
}

uint32_t
CommBoard::get (uint32_t addr)
{
	switch (addr) {
	case COMM_CTRL:
		return control;
	case COMM_OFFSET:
		return offset;
	case COMM_DATA: {
		const std::vector<uint8_t> &ram = (control & CTRL_CPU_RAM) ? m68k_ram : comm_ram;
		uint16_t value = (uint16_t) (ram[offset] | (ram[(offset + 1) & 0xFFFF] << 8));

		offset += 2;
		return swap16 (value);
	}
	case COMM_STATUS1:
		return status1;
	case COMM_STATUS2:
		return status2;
	default:
		return 0xFFFFFFFF;
	}
}

void
CommBoard::put (uint32_t addr, uint32_t val)
{
	switch (addr) {
	case COMM_CTRL:
		if (!(control & CTRL_RESET) && (val & CTRL_RESET)) {
			memset (comm_ram.data (), 0, 32);
			status1 = status2 = 0;
			control = (uint16_t) val;
			start_ring ();
		} else if ((control & CTRL_RESET) && !(val & CTRL_RESET)) {
			if (ring && ring_state != RING_OFF)
				ring->stop ();
			ring_state = RING_OFF;
		}
		control = (uint16_t) val;
		break;
	case COMM_OFFSET:
		offset = (uint16_t) val;
		break;
	case COMM_DATA: {
		std::vector<uint8_t> &ram = (control & CTRL_CPU_RAM) ? m68k_ram : comm_ram;
		uint16_t value = swap16 ((uint16_t) val);

		ram[offset] = (uint8_t) value;
		ram[(offset + 1) & 0xFFFF] = (uint8_t) (value >> 8);
		offset += 2;
		break;
	}
	case COMM_STATUS1:
		status1 = (uint16_t) val;
		break;
	case COMM_STATUS2:
		status2 = (uint16_t) val;
		break;
	default:
		break;
	}
}

bool
CommBoard::dma_to_ram (uint8_t *out, uint32_t size)
{
	uint32_t i;

	if (control & CTRL_G1DMA)
		return false;
	for (i = 0; i < size; i++)
		out[i] = comm_ram[(uint16_t) offset++];
	return true;
}

bool
CommBoard::dma_from_ram (const uint8_t *in, uint32_t size)
{
	uint32_t i;

	if (control & CTRL_G1DMA)
		return false;
	for (i = 0; i < size; i++)
		comm_ram[(uint16_t) offset++] = in[i];
	return true;
}

/* The game let the 68000 go, its parameters in its RAM */
void
CommBoard::start_ring ()
{
	uint16_t mode = (uint16_t) ((m68k_ram[0x200] << 8) | m68k_ram[0x201]);

	node = nodes = 0;
	packet = 0;
	if (!ring)
		return;
	HL_PRINT ("%s: starts as %s, slots of %u bytes", name, mode ? "a slave" : "the master", slot_size ());
	if (!ring->start (mode, slot_size ()))
		return;
	ring_state = RING_NUMBERING;
}

void
CommBoard::connected ()
{
	uint16_t slot = slot_size ();

	memset (&comm_ram[0xF000], 0, 16);
	comm_ram[0xF000] = 1;
	comm_ram[0xF001] = 1;
	comm_ram[0xF002] = m68k_ram[0x204];
	comm_ram[0xF003] = m68k_ram[0x205];

	memset (&comm_ram[0], 0, 32);
	put_be16 (&comm_ram[0], node == 0 ? 0 : 1);
	put_be16 (&comm_ram[2], (uint16_t) (nodes | (nodes << 8)));
	put_be16 (&comm_ram[4], (uint16_t) (node | (node << 8)));
	put_be16 (&comm_ram[8], node == 0 ? 0x7830 : 0x73A2);
	put_be16 (&comm_ram[10], (uint16_t) (0x100 + slot));
	put_be16 (&comm_ram[12], (uint16_t) (slot * nodes));
	put_be16 (&comm_ram[14], 0x100);
	put_be16 (&comm_ram[16], (uint16_t) (slot * nodes));
	status1 = 0xFF01;
	status2 = (uint16_t) ((nodes << 8) | node);
	HL_PRINT ("%s: linked, node %u of %u", name, node + 1, nodes);
}

/* The other end is gone: the game sees no link, and starts the board again */
void
CommBoard::lost ()
{
	status1 = status2 = 0;
	ring->stop ();
	ring_state = RING_OFF;
}

void
CommBoard::vblank ()
{
	uint32_t slot, size;
	uint16_t number = 0;

	if (ring_state == RING_OFF || !(control & CTRL_RESET))
		return;
	if (ring_state == RING_NUMBERING) {
		if (ring->numbered (&node, &nodes)) {
			connected ();
			ring_state = RING_UP;
		} else if (ring->broken ())
			lost ();
		return;
	}
	if (!status2)
		return;
	slot = slot_size ();
	size = slot * nodes;
	if (!slot || 0x100 + slot + size > 0x10000)
		return;
	if (ring->exchange (&comm_ram[0x100], &comm_ram[0x100 + slot], size, packet, &number))
		put_be16 (&comm_ram[6], number);
	packet++;
	if (ring->broken ())
		lost ();
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
CommBoard::save_state (hl::State *state)
{
	int ret = 0;

	SAVE (control);
	SAVE (offset);
	SAVE (status1);
	SAVE (status2);
	ret |= state->put (m68k_ram.data (), RAM_SIZE);
	ret |= state->put (comm_ram.data (), RAM_SIZE);
	return ret;
}

int
CommBoard::load_state (hl::State *state)
{
	int ret = 0;

	LOAD (control);
	LOAD (offset);
	LOAD (status1);
	LOAD (status2);
	ret |= state->get (m68k_ram.data (), RAM_SIZE);
	ret |= state->get (comm_ram.data (), RAM_SIZE);
	/* The ring is not in the state: numbered again */
	if (ring) {
		if (ring_state != RING_OFF)
			ring->stop ();
		ring_state = RING_OFF;
		if (control & CTRL_RESET)
			start_ring ();
	}
	return ret;
}

#undef SAVE
#undef LOAD

/*
 * The Games' Own Settings
 * =======================
 *
 * Where LINK goes in each game's settings, as a cabinet's owner sets them in
 * its test menu: 1 master, 2 slave, 0 no link and the like, a byte of the record
 * of the MIE's EEPROM or of the backup memory. The games not listed have theirs
 * set by hand in their menus. A game that has no record yet writes it at its
 * first start: LINK is set then, for the next one.
 */

static bool
title_is (const char *title, const char *name)
{
	return !strncmp (title, name, 32);
}

static bool
title_starts (const char *title, const char *name)
{
	return !strncmp (title, name, strlen (name));
}

/* node: -1 no link, 0 the master, else a slave's number. False when a record
 * the game has not written yet was wanted */
static bool
apply_game_settings (Board *b, int node, bool satellite)
{
	char title[33];
	const char *game = b->game->name;
	bool ok = true;

	memcpy (title, b->rom_image->ptr (0x30), 32);
	title[32] = 0;
	/* The trailing spaces do not count */
	for (int i = 31; i >= 0 && title[i] == ' '; i--)
		title[i] = 0;

	if (title_is (title, "ALIEN FRONT")) {
		ok = b->eeprom_write (0x3F, node == 0 ? 0 : 1);
	} else if (title_is (title, "MOBILE SUIT GUNDAM JAPAN") ||
	           title_is (title, "MOBILE SUIT GUNDAM DELUXE JAPAN")) {
		ok = b->eeprom_write (0x38, node == -1 ? 2 : node == 0 ? 0 : 1);
	} else if (title_is (title, " BIOHAZARD  GUN SURVIVOR2")) {
		ok = b->flash_write (0x21C, node == 0 ? 0 : 1);
		ok &= b->flash_write (0x22A, node == -1 ? 0 : 1);
	} else if (title_is (title, "HEAVY METAL JAPAN")) {
		ok = b->eeprom_write (0x31, node == -1 ? 0 : node == 0 ? 1 : 2);
	} else if (title_is (title, "OUTTRIGGER     JAPAN")) {
		ok = b->flash_write (0x21A, node == -1 ? 0 : 1);
		ok &= b->flash_write (0x21B, (uint8_t) node);
	} else if (title_is (title, "SLASHOUT JAPAN VERSION")) {
		ok = b->eeprom_write (0x30, (uint8_t) (node + 1));
	} else if (title_is (title, "SPAWN JAPAN")) {
		ok = b->eeprom_write (0x44, node == -1 ? 0 : node == 0 ? 1 : 2);
	} else if (title_is (title, "SPIKERS BATTLE JAPAN VERSION")) {
		ok = b->eeprom_write (0x30, node == -1 ? 0 : node == 0 ? 1 : 2);
	} else if (title_is (title, "VIRTUAL-ON ORATORIO TANGRAM")) {
		ok = b->eeprom_write (0x45, node == -1 ? 3 : node == 0 ? 0 : satellite ? 2 : 1);
	} else if (title_is (title, "WAVE RUNNER GP")) {
		ok = b->eeprom_write (0x33, (uint8_t) node);
		ok &= b->eeprom_write (0x35, node == -1 ? 2 : node == 0 ? 0 : 1);
	} else if (title_is (title, "WORLD KICKS")) {
		ok = b->flash_write (0x224, node == -1 ? 0 : 1);
		ok &= b->flash_write (0x220, node == 0 ? 0 : 1);
	} else if (title_is (title, "CLUB KART IN JAPAN") && strncmp (game, "clubkp", 6)) {
		ok = b->eeprom_write (0x34, node == -1 ? 0 : node == 0 ? 1 : satellite ? 3 : 2);
		if (node != -1 && ok) {
			if (!strncmp (game, "clubkrt", 7))
				ok = b->eeprom_write (0x4A, (uint8_t) ((b->eeprom_read (0x4A) & 0xF8) | node));
			else
				ok = b->eeprom_write (0x3D, (uint8_t) ((b->eeprom_read (0x3D) & 0xC7) | (node << 3)));
		}
	} else if (title_is (title, "INITIAL D") || title_is (title, "INITIAL D Ver.2") ||
	           title_is (title, "INITIAL D Ver.3")) {
		ok = b->eeprom_write (0x34, (uint8_t) ((b->eeprom_read (0x34) & 0xCF) |
		                                       (node == -1 ? 0x00 : node == 0 ? 0x10 : 0x20)));
	} else if (title_is (title, "THE KING OF ROUTE66")) {
		ok = b->eeprom_write (0x3D, (uint8_t) ((b->eeprom_read (0x3D) & 0x0F) |
		                                       (node == -1 ? 0x40 : node == 0 ? 0x50 : 0x60)));
	} else if (title_is (title, "F355 CHALLENGE JAPAN") && strcmp (game, "f355")) {
		ok = b->flash_write (0x230, node == -1 ? 0 : node == 0 ? 1 : 2);
		if (node != -1)
			ok &= b->flash_write (0x231, (uint8_t) node);
		/* The cabinet: 0 a deluxe's three screens, 1 a twin's one */
		ok &= b->flash_write (0x233, b->machine->boards.size () >= 3 ? 0 : 1);
	} else if (title_is (title, "SEGA TETRIS")) {
		ok = b->eeprom_write (0x50, (uint8_t) (node + 1));
	} else if (title_starts (title, " DERBY")) {
		if (b->eeprom_read (3) == 'B' && b->eeprom_read (4) == 'D' && b->eeprom_read (5) == 'Y')
			ok = b->eeprom_write (0x30, (uint8_t) (node == -1 ? b->eeprom_read (0x30) | 1 : b->eeprom_read (0x30) & 0xFE));
		else {
			ok = b->eeprom_write (0x45, (uint8_t) (b->eeprom_read (0x45) & 0xFC));
			ok &= b->eeprom_write (0x4E, (uint8_t) ((b->eeprom_read (0x4E) & 0xF8) | 3));
		}
	} else if (title_starts (title, "WCCF") && node != -1) {
		ok = b->eeprom_write (0x34, (uint8_t) node);
	}
	return ok;
}

/* Now and, the game having no record yet, each second until there is one */
void
Board::link_settings ()
{
	const hl::Config *cfg = hl::Config::get ();
	int node;

	if (cfg->link < 0 || is_atomiswave || index)
		return;
	node = cfg->link == 0 ? -1 : cfg->link == 1 ? 0 : (int) cfg->link_node;
	if (apply_game_settings (this, node, cfg->link == 3)) {
		if (link_settings_due)
			HL_PRINT ("naomi: LINK is in the game's settings, for its next start");
		link_settings_due = false;
	} else if (!link_settings_due) {
		HL_PRINT ("naomi: LINK waits for the game's first start: its settings are not written yet");
		link_settings_due = true;
	}
}

/*
 * Driving Simulator's Dashboard
 * =============================
 *
 * Eight bytes on the SCIF: 02, a frame's seven bytes, their sum.
 * Byte 2: the rev counter's hundreds of rpm plus 1; 3: the speed plus 1, km/h.
 */

void
DriveSimMeters::write (uint8_t value)
{
	if (frame.empty () && value != 2)
		return;
	if (frame.size () == 7) {
		uint8_t sum = 0;

		for (uint8_t b : frame)
			sum += b;
		if (sum == value) {
			int t = (frame[2] - 1) * 100, s = frame[3] - 1;

			if (t != tacho)
				hl::Outputs::get ()->signal ("tachometer", tacho = t);
			if (s != speed)
				hl::Outputs::get ()->signal ("speedometer", speed = s);
		}
		frame.clear ();
	} else
		frame.push_back (value);
}

} /* namespace naomi */
