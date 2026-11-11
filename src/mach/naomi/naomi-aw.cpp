/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/awboard.cpp and dc_atomiswave.cpp (license
 * BSD-3-Clause, copyright-holders Olivier Galibert, Andreas Naive, Angelo
 * Salese) and Flycast's core/hw/naomi/atomiswave.cpp and awcartridge.cpp
 * (GPL-2.0-or-later, flyinghead).
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
 * The Atomiswave
 * ==============
 *
 * Sammy's board is a NAOMI's little sister: the same SH-4, Holly and AICA, on
 * 16 MB of RAM and 8 MB of VRAM. What differs is here: a BIOS in a flash, a
 * 128 KB SRAM, no MIE (the controls are plain controllers on the Maple bus),
 * coins and more in the "modem" area, and a ROM board with its own registers
 * at 005F7000 and its own 16-bit encryption.
 */

#include <algorithm>

#include "hl/controls.h"
#include "hl/ffb.h"
#include "hl/input.h"
#include "hl/outputs.h"
#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The ROM Board
 * =============
 *
 *  00	 EPR offset, low word (in 16-bit units, from the start of the ROMs)
 *  04	 EPR offset, high word
 *  0C	 MPR record index: 64-byte records of the mask ROMs' file system
 *  10	 MPR first file index: the record of the first file
 *  14	 MPR file offset, low word
 *  18	 MPR file offset, high word
 *  80	 PIO: a word straight, not decrypted; written at EPR 007FFFFF, the
 *	 mask ROMs' bank
 *
 * A key of 8 bits: 0-3 a 16-bit XOR, 4-5 the S-boxes, 6-7 the permutation.
 */

enum { AW_EPR, AW_MPR_RECORD, AW_MPR_FILE };

static const uint8_t permutation_table[4][16] = {
	{14,1,11,15,7,3,8,13,0,4,2,12,6,10,5,9},
	{8,10,1,3,7,4,11,2,5,15,6,0,12,13,9,14},
	{4,5,9,6,1,13,7,11,10,0,14,12,8,15,2,3},
	{12,7,11,2,0,5,15,6,1,8,14,4,9,13,3,10}
};

static const struct {
	uint8_t S0[32], S1[16], S2[16], S3[8];
} sboxes_table[4] = {
	{
		{11,8,6,25,2,7,23,28,5,10,21,20,1,26,17,19,14,27,22,30,15,4,9,24,31,3,16,12,0,18,29,13},
		{13,5,9,6,4,2,11,10,12,0,8,1,3,14,15,7},
		{1,13,11,3,8,7,9,10,12,15,4,14,0,5,6,2},
		{3,0,5,6,2,4,1,7}
	},
	{
		{9,15,28,7,13,24,2,23,21,1,22,16,18,8,17,31,27,6,30,12,4,20,5,19,0,25,3,29,10,14,11,26},
		{5,2,13,11,8,6,12,1,4,3,0,10,14,15,7,9},
		{11,6,10,0,12,1,8,14,2,9,13,3,7,4,15,5},
		{1,5,6,2,4,7,3,0}
	},
	{
		{17,25,29,27,5,11,10,21,2,8,13,0,30,3,14,16,22,1,7,15,31,18,4,20,9,19,26,24,23,28,12,6},
		{15,3,2,11,7,14,6,12,1,13,0,8,9,4,10,5},
		{6,12,5,7,4,8,2,3,1,14,13,11,9,10,0,15},
		{5,1,0,3,4,6,2,7}
	},
	{
		{7,21,9,20,10,28,31,11,16,15,14,30,27,23,5,25,0,22,24,2,6,17,3,18,4,12,13,19,26,1,29,8},
		{10,6,5,1,13,0,9,2,15,14,7,11,8,3,12,4},
		{8,6,15,13,2,7,1,3,11,0,14,10,4,5,12,9},
		{6,5,4,1,0,2,3,7}
	}
};

/* 0: unknown or unused */
static const uint16_t xor_table[16] = {
	0x0000, 0, 0x97CF, 0x4BE3, 0x2255, 0x8DD6, 0, 0xC6A2,
	0xA1E8, 0xB3BF, 0x3B1A, 0x547A, 0, 0x935F, 0, 0
};

static uint16_t
bit_swap (uint16_t in, const uint8_t *vec)
{
	uint16_t out = 0;

	for (int i = 0; i < 16; i++)
		out |= ((in >> vec[i]) & 1) << (15 - i);
	return out;
}

static uint16_t
aw_decrypt (uint16_t cipher, uint32_t address, uint8_t key)
{
	static const uint8_t addr_swap[16] = { 13,5,2, 14,10,9,4, 15,11,6,1, 12,8,7,3,0 };
	const uint8_t *p = permutation_table[key >> 6];
	const auto &s = sboxes_table[(key >> 4) & 3];
	const uint8_t text_swap[16] = { p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8],
	                                p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0] };
	uint16_t aux = bit_swap (cipher, text_swap) ^ bit_swap ((uint16_t) address, addr_swap);
	unsigned b0 = s.S0[aux & 0x1F], b1 = s.S1[(aux >> 5) & 0xF];
	unsigned b2 = s.S2[(aux >> 9) & 0xF], b3 = s.S3[aux >> 13];

	return (uint16_t) (((b3 << 13) | (b2 << 9) | (b1 << 5) | b0) ^ xor_table[key & 15]);
}

AwCart::AwCart (hl::Buffer *rom, uint8_t key_) :
	Cart (rom, Cart::TYPE_ROM, 0, 0, NULL),
	data ((const uint16_t *) rom->ptr (0)),
	bytes (rom->size ()),
	key (key_)
{
	mpr_offset = decrypted (0x58 / 2) | (decrypted (0x5A / 2) << 16);
	HL_PRINT ("atomiswave: ROM board key %02X, mask ROMs from %08X", key, mpr_offset);
	reset ();
}

uint16_t
AwCart::decrypted (uint32_t address) const
{
	return aw_decrypt (data[address % (bytes / 2)], address, key);
}

void
AwCart::reset ()
{
	memset (&r, 0, sizeof (r));
}

void
AwCart::recalc (int mode)
{
	switch (mode) {
	case AW_EPR:
		r.dma_offset = r.epr_offset * 2;
		r.dma_limit = mpr_offset;
		break;
	case AW_MPR_RECORD:
		r.dma_offset = mpr_offset + r.record_index * 0x40;
		r.dma_limit = std::min (0x8000000u, bytes);
		break;
	default: {
		uint32_t first = (r.bank * 0x8000000 + mpr_offset + r.first_file_index * 0x40 + 8) / 2;
		uint32_t file = decrypted (first) | (decrypted (first + 1) << 16);

		r.dma_offset = (mpr_offset + file + r.file_offset * 2) & 0x7FFFFFF;
		r.dma_limit = std::min (0x8000000u, bytes);
		break;
	}
	}
	if (r.dma_offset >= mpr_offset) {
		uint32_t base = r.bank * 0x8000000;

		r.dma_offset += base;
		r.dma_limit = std::min (r.dma_limit + base, bytes);
	}
}

uint32_t
AwCart::get (unsigned reg)
{
	if (reg == 0x80) {
		uint32_t offset = r.epr_offset & 0x3FFFFFF;

		if (offset >= mpr_offset / 2)
			offset += r.bank * 0x4000000;
		return bytes > offset * 2 ? data[offset] : 0;
	}
	return 0xFFFF;
}

void
AwCart::put (unsigned reg, uint32_t val)
{
	switch (reg) {
	case 0x00:
		r.epr_offset = (r.epr_offset & 0xFFFF0000) | (uint16_t) val;
		recalc (AW_EPR);
		break;
	case 0x04:
		r.epr_offset = (r.epr_offset & 0x0000FFFF) | (val << 16);
		recalc (AW_EPR);
		break;
	case 0x0C:
		r.record_index = val;
		recalc (AW_MPR_RECORD);
		break;
	case 0x10:
		r.first_file_index = val;
		recalc (AW_MPR_FILE);
		break;
	case 0x14:
		r.file_offset = (r.file_offset & 0xFFFF0000) | (uint16_t) val;
		recalc (AW_MPR_FILE);
		break;
	case 0x18:
		r.file_offset = (r.file_offset & 0x0000FFFF) | (val << 16);
		recalc (AW_MPR_FILE);
		break;
	case 0x80:
		if (r.epr_offset == 0x7FFFFF)
			r.bank = val & 3;
		break;
	default:
		break;
	}
}

void
AwCart::dma (uint8_t *out, uint32_t size)
{
	uint32_t i;

	for (i = 0; i + 2 <= size; i += 2) {
		uint16_t word = 0;

		if (r.dma_offset < r.dma_limit)
			word = decrypted (r.dma_offset / 2);
		memcpy (out + i, &word, 2);
		r.dma_offset += 2;
	}
}

int
AwCart::save_state (hl::State *state)
{
	return state->put (&r, sizeof (r));
}

int
AwCart::load_state (hl::State *state)
{
	return state->get (&r, sizeof (r));
}

/*
 * The BIOS's Flash
 * ================
 *
 * A 29L001: read as a ROM (the map's region, for the code); writes come here,
 * unlock cycles at 555 and 2AA, then program, or erase a sector or all.
 */

AwFlash::AwFlash (hl::Machine *mach, hl::Buffer *rom) :
	hl::Device (mach),
	rom (rom),
	step (0)
{
}

void
AwFlash::reset (hl::ResetType type)
{
	(void) type;
	step = 0;
}

int
AwFlash::get (unsigned size, uint32_t addr, void *val)
{
	(void) addr;
	set_ptr (val, size, 0);
	return 0;
}

int
AwFlash::put (unsigned size, uint32_t addr, uint64_t val)
{
	const uint32_t a = addr & 0x1FFFF, cell = a & 0xFFF;
	const uint8_t byte = (uint8_t) val;

	(void) size;
	switch (step) {
	case 0:
	case 3:
		if (byte == 0xAA && cell == 0x555)
			step++;
		else if (byte == 0xF0)
			step = 0;
		break;
	case 1:
	case 4:
		step = (byte == 0x55 && cell == 0x2AA) ? step + 1 : 0;
		break;
	case 2:
		step = byte == 0xA0 ? 10 : byte == 0x80 ? 3 : 0;
		break;
	case 5:
		if (byte == 0x10)
			memset (rom->ptr (0), 0xFF, 0x20000);
		else if (byte == 0x30) {
			/* Its sectors: 16, 8, 8, 32 then 64 KB */
			uint32_t lo = a < 0x4000 ? 0 : a < 0x6000 ? 0x4000 : a < 0x8000 ? 0x6000 : a < 0x10000 ? 0x8000 : 0x10000;
			uint32_t hi = a < 0x4000 ? 0x4000 : a < 0x6000 ? 0x6000 : a < 0x8000 ? 0x8000 : a < 0x10000 ? 0x10000 : 0x20000;

			memset (rom->ptr (lo), 0xFF, hi - lo);
		}
		step = 0;
		changed = true;
		break;
	case 10:
		((uint8_t *) rom->ptr (0))[a] &= byte;
		step = 0;
		changed = true;
		break;
	default:
		step = 0;
		break;
	}
	return 0;
}

int
AwFlash::save_state (hl::State *state)
{
	return state->put (&step, sizeof (step));
}

int
AwFlash::load_state (hl::State *state)
{
	return state->get (&step, sizeof (step));
}

/*
 * The Controllers
 * ===============
 *
 * Dreamcast-like controllers on the Maple bus, two (four on Guilty Gear
 * Isuka). Their two bytes of switches, active low: right, left, down, up,
 * start, 1, 2, 3; then test and service on the first controller's, 4, 5, 6.
 * Maximum Speed and Faster Than Speed have their wheel on the second
 * controller's analogs: steering, gas, brake, twice.
 */

static const char AW_MODEL[] = "Dreamcast Controller";
static const char AW_LICENSE[] = "Produced By or Under License From SEGA ENTERPRISES,LTD.";

static void
copy_spaces (uint8_t *to, const char *text, unsigned size)
{
	unsigned i, n = (unsigned) strlen (text);

	/* The bus's words are little-endian on the SH-4: the bytes as they are */
	for (i = 0; i < size; i++)
		to[i] = i < n ? (uint8_t) text[i] : ' ';
}

uint16_t
AwControllers::switches (unsigned port) const
{
	uint16_t held = 0;

	/* Player 2's pad from controls.ini's P2_ rows ([Naomi]'s first); the
	 * wheel's has no switch; players 3 and 4 have no rows */
	if (port == 1 && !wheel) {
		hl::Controls &c = hl::Controls::get ();
		static const struct { const char *section, *key; uint16_t bit; } player2[] = {
			{ "Digital", "P2_Right", 0x0080 }, { "Digital", "P2_Left", 0x0040 },
			{ "Digital", "P2_Down", 0x0020 }, { "Digital", "P2_Up", 0x0010 },
			{ "Common", "P2_Start", 0x0008 }, { "Digital", "P2_Button1", 0x0004 },
			{ "Digital", "P2_Button2", 0x0002 }, { "Digital", "P2_Button3", 0x0001 },
			{ "Digital", "P2_Button4", 0x0400 }, { "Digital", "P2_Button5", 0x0200 },
			{ "Digital", "P2_Button6", 0x0100 },
		};

		for (const auto &b : player2)
			if (c.pressed (b.section, b.key))
				held |= b.bit;
		return (uint16_t) ~held;
	}
	if (port != 0)
		return 0xFFFF;
	if (hl::Input::action (hl::ACTION_RIGHT)) held |= 0x0080;
	if (hl::Input::action (hl::ACTION_LEFT)) held |= 0x0040;
	if (hl::Input::action (hl::ACTION_DOWN)) held |= 0x0020;
	if (hl::Input::action (hl::ACTION_UP)) held |= 0x0010;
	if (hl::Input::action (hl::ACTION_START1)) held |= 0x0008;
	if (hl::Input::action (hl::ACTION_BUTTON1)) held |= 0x0004;
	if (hl::Input::action (hl::ACTION_BUTTON2)) held |= 0x0002;
	if (hl::Input::action (hl::ACTION_BUTTON3)) held |= 0x0001;
	if (hl::Input::action (hl::ACTION_TEST)) held |= 0x4000;
	if (hl::Input::action (hl::ACTION_SERVICE)) held |= 0x2000;
	if (hl::Input::action (hl::ACTION_BUTTON4)) held |= 0x0400;
	if (hl::Controls::get ().pressed ("Naomi", "P1_Button5")) held |= 0x0200;
	if (hl::Controls::get ().pressed ("Naomi", "P1_Button6")) held |= 0x0100;
	return (uint16_t) ~held;
}

unsigned
AwControllers::reply (unsigned port, const uint32_t *request, uint32_t *out)
{
	const uint8_t command = (uint8_t) request[0];
	const uint32_t sender = (request[0] >> 16) & 0xFF;
	/* From the controller itself: none has anything plugged in it */
	const uint32_t me = (request[0] >> 8) & 0xFF;
	uint32_t words;

	if ((me & 0x1F) || port >= count)
		return 0;
	memset (out + 1, 0, 32);
	switch (command) {
	case 0x01:	/* device request */
	case 0x02: {	/* all status request */
		out[1] = 0x01000000;
		out[2] = 0xFF663F00;
		out[3] = out[4] = 0;
		out[5] = 0x000000FF;
		memset (out + 6, 0, 80);
		copy_spaces ((uint8_t *) (out + 1) + 18, AW_MODEL, 30);
		copy_spaces ((uint8_t *) (out + 1) + 48, AW_LICENSE, 60);
		out[28] = 0x01F401AE;
		if (command == 0x02) {
			copy_spaces ((uint8_t *) (out + 29), "Version 1.000,1998/05/11,315-6215-AB   ,Analog Module: The 4th Edition. 05/08", 80);
			words = 49;
			out[0] = 6;
		} else {
			words = 28;
			out[0] = 5;
		}
		break;
	}
	case 0x03:	/* reset */
		words = 0;
		out[0] = 7;
		break;
	case 0x09: {	/* get condition */
		uint8_t analog[6] = { 0x80, 0x00, 0x00, 0x80, 0x00, 0x00 };

		if (port == 1) {
			hl::Controls &c = hl::Controls::get ();
			float steer = std::max (-1.0f, std::min (1.0f, (float) c.axis ("Driving", "P1_Steer")));
			float gas = std::max (0.0f, std::min (1.0f, (float) c.pedal ("Driving", "P1_Gas")));
			float brake = std::max (0.0f, std::min (1.0f, (float) c.pedal ("Driving", "P1_Brake")));

			analog[0] = analog[3] = (uint8_t) (127.5f + steer * 127.5f);
			analog[1] = analog[4] = (uint8_t) (gas * 255.0f);
			analog[2] = analog[5] = (uint8_t) (brake * 255.0f);
		}
		out[1] = 0x01000000;
		out[2] = switches (port) | ((uint32_t) analog[0] << 16) | ((uint32_t) analog[1] << 24);
		out[3] = analog[2] | ((uint32_t) analog[3] << 8) | ((uint32_t) analog[4] << 16) | ((uint32_t) analog[5] << 24);
		words = 3;
		out[0] = 8;
		break;
	}
	default:
		return 0;
	}
	out[0] |= (sender << 8) | ((me | 0x20) << 16) | (words << 24);
	return words + 1;
}

/*
 * The "Modem" Area
 * ================
 *
 *  00600280 r	 0000dcba: coins 1 to 4, active low
 *  00600284 rw	 ddcc0000: the kind of devices on Maple ports 2 and 3, as written
 *  00600288 w	 0000dcba: coin counters 1, 2, lockouts 1, 2
 *  0060028C rw	 the digital outputs: on the wheel's games, bit 0 the force's
 *		 direction, bits 1 to 4 its strength
 */

AwModem::AwModem (Board *board_) :
	hl::Device (board_->machine),
	board (board_)
{
	reset (hl::RESET_HARD);
}

void
AwModem::reset (hl::ResetType type)
{
	(void) type;
	memset (&s, 0, sizeof (s));
}

/* A coin chute's switch stays closed some reads of a coin only: Guilty Gear XX
 * Accent Core wants a few, more makes a coin error */
uint8_t
AwModem::coins ()
{
	static const hl::Action chutes[2] = { hl::ACTION_COIN1, hl::ACTION_COIN2 };
	uint8_t val = 0xF;

	for (unsigned i = 0; i < 2; i++) {
		if (!hl::Input::action (chutes[i]))
			s.chute[i] = 0;
		else if (s.chute[i] < 8) {
			s.chute[i]++;
			val &= ~(1u << i);
		}
	}
	return val;
}

int
AwModem::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t data = 0xFF;

	switch (addr & 0x7FF) {
	case 0x280:
		data = coins ();
		break;
	case 0x284:
		data = s.devices;
		break;
	case 0x288:
		data = 0;
		break;
	case 0x28C:
		data = s.output;
		break;
	default:
		break;
	}
	set_ptr (val, size, data);
	return 0;
}

int
AwModem::put (unsigned size, uint32_t addr, uint64_t val)
{
	(void) size;
	switch (addr & 0x7FF) {
	case 0x284:
		s.devices = val & 0xF0;
		break;
	case 0x28C:
		if ((uint8_t) val != s.output) {
			s.output = (uint8_t) val;
			if (board->aw_wheel) {
				float force = ((s.output >> 1) & 15) / 15.0f * 0.5f;

				hl::Outputs::get ()->signal ("awffb", s.output);
				hl::ForceFeedback::set (hl::ForceFeedback::CONSTANT, (s.output & 1) ? -force : force, board->holly->frames);
			} else
				for (int i = 0; i < 8; i++) {
					char name[8];

					snprintf (name, sizeof (name), "lamp%d", i);
					hl::Outputs::get ()->signal (name, (s.output >> i) & 1);
				}
		}
		break;
	default:
		break;
	}
	return 0;
}

int
AwModem::save_state (hl::State *state)
{
	return state->put (&s, sizeof (s));
}

int
AwModem::load_state (hl::State *state)
{
	return state->get (&s, sizeof (s));
}

} /* namespace naomi */
