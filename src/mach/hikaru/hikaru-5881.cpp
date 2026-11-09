/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/315-5881_crypt.cpp, Copyright (C)
 * Andreas Naive, Olivier Galibert, David Haywood, under the BSD-3-Clause
 * license, which its parts here keep:
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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

#include <string.h>

#include "mach/hikaru/hikaru-5881.h"

namespace hikaru {

/*
 * The 315-5881
 * ============
 *
 * As MAME has it (its notes, in short): a stream cipher in counter mode,
 * whose 16-bit block cipher is two 4-round Feistel networks of 6-in,
 * 2-out s-boxes. The first takes the counter (the word's address), the
 * game's key and the sequence key (the subkey the game writes), and gives
 * a middle result; the second takes the encrypted word, the keys and that
 * result. A stream is made of substreams, each with an 18-bit header: a
 * flag (1: compressed) and the substream's size, (A + 1) * (B + 1), 9 + 8
 * bits; the plaintext's words are 2 bits off the cipher's. A compressed
 * substream is decompressed by lines, each from the previous one, by the
 * prefix trees below.
 *
 * On the Hikaru (SWRACER, PHARRIER, AIRTRIX), the "M3" way: the game writes
 * a block to the chip's 64 KB of RAM, then its address and subkey, and
 * reads the result word by word (see hikaru-memctl.cpp).
 */

#define BUFFER_SIZE		2
#define FLAG_COMPRESSED		0x20000

struct Sbox {
	uint8_t table[64];
	/* The input bits' positions, -1 none but the key's */
	int inputs[6];
	/* The output bits' positions */
	int outputs[2];
};

static const Sbox fn1_sboxes[4][4] = {
	{   // 1st round
		{
			{
				0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
				2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
			},
			{3,4,5,7,-1,-1},
			{0,4}
		},

		{
			{
				2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
				0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
			},
			{0,1,2,5,6,7},
			{1,6}
		},

		{
			{
				0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
				1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
			},
			{0,2,5,6,7,-1},
			{2,3}
		},

		{
			{
				3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
				1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
			},
			{1,2,3,4,6,7},
			{5,7}
		},
	},
	{   // 2nd round
		{
			{
				3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
				2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
			},
			{0,1,3,4,5,7},
			{0,4}
		},

		{
			{
				2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
				3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
			},
			{0,1,3,4,6,-1},
			{1,5}
		},

		{
			{
				2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
				3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
			},
			{1,2,5,6,7,6},
			{2,7}
		},

		{
			{
				0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
				0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
			},
			{2,3,4,5,6,7},
			{3,6}
		},
	},
	{   // 3rd round
		{
			{
				0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
				0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
			},
			{1,2,3,4,5,7},
			{0,5}
		},

		{
			{
				1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
				1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
			},
			{0,2,4,5,6,7},
			{1,6}
		},

		{
			{
				0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{0,2,4,6,7,-1},
			{2,3}
		},

		{
			{
				0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
				2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
			},
			{0,1,2,3,6,-1},
			{4,7}
		},
	},
	{   // 4th round
		{
			{
				0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
				3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
			},
			{0,1,3,4,5,6},
			{0,5}
		},

		{
			{
				0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
				3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
			},
			{0,1,2,3,5,7},
			{1,7}
		},

		{
			{
				0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
				3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
			},
			{2,3,4,5,6,7},
			{2,3}
		},

		{
			{
				0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
				1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
			},
			{0,1,2,4,6,7},
			{4,6}
		},
	},
};

static const Sbox fn2_sboxes[4][4] = {
	{   // 1st round
		{
			{
				3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
				2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
			},
			{1,3,4,5,6,7},
			{0,7}
		},

		{
			{
				0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
				3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
			},
			{0,3,5,6,5,0},
			{1,2}
		},

		{
			{
				0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
				0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
			},
			{0,2,3,4,7,-1},
			{3,4}
		},

		{
			{
				2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
				2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
			},
			{1,2,5,6,-1,-1},
			{5,6}
		},
	},
	{   // 2nd round
		{
			{
				2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
				2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
			},
			{0,1,4,5,6,7},
			{0,7}
		},

		{
			{
				0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
				1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
			},
			{0,2,3,6,7,-1},
			{1,5}
		},

		{
			{
				1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
				1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
			},
			{2,3,4,6,7,2},
			{2,3}
		},

		{
			{
				2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
				3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
			},
			{1,2,3,4,5,6},
			{4,6}
		},
	},
	{   // 3rd round
		{
			{
				0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
				3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
			},
			{1,4,5,6,7,5},
			{0,5}
		},

		{
			{
				0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
				2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
			},
			{0,2,3,4,6,7},
			{1,7}
		},

		{
			{
				2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{1,2,4,7,2,-1},
			{2,4}
		},

		{
			{
				0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
				3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
			},
			{0,1,2,3,5,7},
			{3,6}
		},
	},
	{   // 4th round
		{
			{
				0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
				0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
			},
			{0,1,2,4,7,-1},
			{0,5}
		},

		{
			{
				2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
				0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
			},
			{0,1,3,5,6,-1},
			{1,3}
		},

		{
			{
				0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
				3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
			},
			{0,2,3,4,5,7},
			{2,7}
		},

		{
			{
				0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
				2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
			},
			{2,3,4,5,6,7},
			{4,6}
		},
	},
};

static const int fn1_game_key_scheduling[38][2] = {
	{1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
	{6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
	{12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
	{19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
	{25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

static const int fn2_game_key_scheduling[32][2] = {
	{0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
	{8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
	{14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
	{22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

static const int fn1_sequence_key_scheduling[20][2] = {
	{0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
	{6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
	{13,74}, {14,19}, {14,80}, {15,26}
};

static const int fn2_sequence_key_scheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

static const int fn2_middle_result_scheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

static const uint8_t trees[9][2][32] = {
	{
		{0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
			0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
			0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
			0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
		{0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
			0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
	},
	{
		{0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
			0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
			0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
			0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
			0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
			0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
			0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
};

static inline int
bit (uint32_t x, int n)
{
	return (x >> n) & 1;
}

/* MAME's bitswap<16> (value, b15 ... b0): bit 15 - i of the result is
 * bit b[i] of value */
static uint16_t
bitswap16 (uint16_t value, const int (&b)[16])
{
	uint16_t r = 0;
	for (int i = 0; i < 16; i++)
		r |= (uint16_t) (bit (value, b[i]) << (15 - i));
	return r;
}

static int
feistel_function (int input, const Sbox *sboxes, uint32_t subkeys)
{
	int result = 0;

	for (int m = 0; m < 4; m++) {
		int aux = 0;
		for (int k = 0; k < 6; k++)
			if (sboxes[m].inputs[k] != -1)
				aux |= bit (input, sboxes[m].inputs[k]) << k;
		aux = sboxes[m].table[(aux ^ subkeys) & 0x3F];
		for (int k = 0; k < 2; k++)
			result |= bit (aux, k) << sboxes[m].outputs[k];
		subkeys >>= 6;
	}
	return result;
}

static void
schedule (uint32_t subkeys[4], int position)
{
	subkeys[position / 24] ^= 1u << (position % 24);
}

static uint16_t
block_decrypt (uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data)
{
	static const int counter_swap[16] = { 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2 };
	static const int data_swap[16] = { 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10 };
	static const int result_swap[16] = { 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8 };
	uint32_t fn1_subkeys[4] = { 0, 0, 0, 0 }, fn2_subkeys[4] = { 0, 0, 0, 0 };
	int j, A, B, aux, middle_result;

	/* The game key's, then the sequence key's scheduling */
	for (j = 0; j < 38; j++)
		if (bit (game_key, fn1_game_key_scheduling[j][0]))
			schedule (fn1_subkeys, fn1_game_key_scheduling[j][1]);
	for (j = 0; j < 32; j++)
		if (bit (game_key, fn2_game_key_scheduling[j][0]))
			schedule (fn2_subkeys, fn2_game_key_scheduling[j][1]);
	for (j = 0; j < 20; j++)
		if (bit (sequence_key, fn1_sequence_key_scheduling[j][0]))
			schedule (fn1_subkeys, fn1_sequence_key_scheduling[j][1]);
	for (j = 0; j < 16; j++)
		if (bit (sequence_key, j))
			schedule (fn2_subkeys, fn2_sequence_key_scheduling[j]);

	/* The first network, on the counter */
	aux = bitswap16 (counter, counter_swap);
	B = aux >> 8;
	A = (aux & 0xFF) ^ feistel_function (B, fn1_sboxes[0], fn1_subkeys[0]);
	B ^= feistel_function (A, fn1_sboxes[1], fn1_subkeys[1]);
	A ^= feistel_function (B, fn1_sboxes[2], fn1_subkeys[2]);
	B ^= feistel_function (A, fn1_sboxes[3], fn1_subkeys[3]);
	middle_result = (B << 8) | A;

	for (j = 0; j < 16; j++)
		if (bit (middle_result, j))
			schedule (fn2_subkeys, fn2_middle_result_scheduling[j]);

	/* The second, on the data */
	aux = bitswap16 (data, data_swap);
	B = aux >> 8;
	A = (aux & 0xFF) ^ feistel_function (B, fn2_sboxes[0], fn2_subkeys[0]);
	B ^= feistel_function (A, fn2_sboxes[1], fn2_subkeys[1]);
	A ^= feistel_function (B, fn2_sboxes[2], fn2_subkeys[2]);
	B ^= feistel_function (A, fn2_sboxes[3], fn2_subkeys[3]);
	aux = (B << 8) | A;
	return bitswap16 ((uint16_t) aux, result_swap);
}

void
Crypt5881::reset ()
{
	memset (&s, 0, sizeof (s));
}

void
Crypt5881::set_addr_low (uint16_t data)
{
	s.cur_address = (s.cur_address & 0xFFFF0000) | data;
	s.enc_ready = 0;
	s.first_read = 1;
}

/* As MAME's: the high address taken as 0 */
void
Crypt5881::set_addr_high (uint16_t data)
{
	s.cur_address &= 0x0000FFFF;
	if (high_address)
		s.cur_address |= (uint32_t) data << 16;
	s.enc_ready = 0;
	s.buffer_bit = 7;
	s.buffer_bit2 = 15;
	s.first_read = 1;
}

void
Crypt5881::set_subkey (uint16_t data)
{
	s.subkey = data;
	s.enc_ready = 0;
}

uint16_t
Crypt5881::read_le ()
{
	uint16_t v;

	/* A dummy word before the stream */
	if (s.first_read && dummy_first) {
		s.first_read = 0;
		return 0;
	}
	v = do_decrypt ();
	return (uint16_t) ((v >> 8) | (v << 8));
}

uint16_t
Crypt5881::do_decrypt ()
{
	const uint8_t *base;

	if (!s.enc_ready)
		enc_start ();
	if (s.dec_header & FLAG_COMPRESSED) {
		if (s.line_buffer_pos >= s.line_buffer_size) {
			if (s.done_compression == 1)
				enc_start ();
			line_fill ();
		}
		base = s.lines[s.line_cur] + s.line_buffer_pos;
		s.line_buffer_pos += 2;
	} else {
		if (s.buffer_pos == BUFFER_SIZE)
			enc_fill ();
		base = s.buffer + s.buffer_pos;
		s.buffer_pos += 2;
	}
	return (uint16_t) ((base[0] << 8) | base[1]);
}

uint16_t
Crypt5881::get_decrypted_16 ()
{
	uint16_t enc = read_cb (opaque, s.cur_address);
	uint16_t dec = block_decrypt (key, s.subkey, (uint16_t) s.cur_address, enc);
	uint16_t res = (uint16_t) ((dec & 3) | (s.dec_hist & 0xFFFC));

	s.dec_hist = dec;
	s.cur_address++;
	return res;
}

void
Crypt5881::enc_start ()
{
	int blocky;

	s.block_pos = 0;
	s.done_compression = 0;
	s.buffer_pos = BUFFER_SIZE;
	/* Bits left from the decompression: the header's first two */
	if (s.buffer_bit2 < 14)
		s.dec_header = (s.buffer2a & 3u) << 16;
	else {
		s.dec_hist = 0;
		s.dec_header = (uint32_t) get_decrypted_16 () << 16;
	}
	s.dec_header |= get_decrypted_16 ();

	/* The substream's size: 0001FF00 + 1 by 000000FF + 1; a compressed
	 * one's lines are of the first */
	s.block_numlines = (int32_t) (s.dec_header & 0xFF) + 1;
	blocky = (int32_t) ((s.dec_header & 0x1FF00) >> 8) + 1;
	s.block_size = s.block_numlines * blocky;
	if (s.dec_header & FLAG_COMPRESSED) {
		s.line_buffer_size = blocky;
		s.line_buffer_pos = s.line_buffer_size;
		s.buffer_bit = 7;
		s.buffer_bit2 = 15;
	}
	s.enc_ready = 1;
}

void
Crypt5881::enc_fill ()
{
	for (int i = 0; i != BUFFER_SIZE; i += 2) {
		uint16_t val = get_decrypted_16 ();
		s.buffer[i] = (uint8_t) val;
		s.buffer[i + 1] = (uint8_t) (val >> 8);
		s.block_pos += 2;
		/* A new header at the substream's end */
		if (!(s.dec_header & FLAG_COMPRESSED) && s.block_pos == s.block_size)
			enc_start ();
	}
	s.buffer_pos = 0;
}

int
Crypt5881::get_compressed_bit ()
{
	int res;

	if (s.buffer_bit2 == 15) {
		s.buffer_bit2 = 0;
		s.buffer2a = get_decrypted_16 ();
		s.buffer2[0] = (uint8_t) s.buffer2a;
		s.buffer2[1] = (uint8_t) (s.buffer2a >> 8);
		s.buffer_pos = 0;
	} else
		s.buffer_bit2++;

	res = (s.buffer2[(s.buffer_pos & 1) ^ 1] >> s.buffer_bit) & 1;
	if (--s.buffer_bit == -1) {
		s.buffer_bit = 7;
		s.buffer_pos++;
	}
	return res;
}

/* A line of a compressed substream, from the previous one. In the trees,
 * 0xxxxxxx is the next node; 1a0bbccc an end, ccc + 1 times: a = 1, a copy
 * from the previous line, at bb's offset (0, +1, 0, -1), a = 0, a byte of
 * the stream; FF none */
void
Crypt5881::line_fill ()
{
	static const int offsets[4] = { 0, 1, 0, -1 };
	const uint8_t *lp = s.lines[s.line_cur];
	uint8_t *lc;
	int size = s.line_buffer_size, i;

	s.line_cur ^= 1;
	lc = s.lines[s.line_cur];
	s.line_buffer_pos = 0;

	for (i = 0; i < size;) {
		/* The tree: the line's start, its inside, its last 7 bytes */
		int slot = i ? (i < size - 7 ? 1 : (i & 7) + 1) : 0;
		uint32_t tmp = 0;
		while (!(tmp & 0x80))
			tmp = trees[slot][get_compressed_bit ()][tmp];
		/* Not in the trees: the rest of the line left as it is */
		if (tmp == 0xFF)
			break;
		int count = (int) (tmp & 7) + 1;
		if (tmp & 0x40) {
			int offset = offsets[(tmp & 0x18) >> 3];
			for (int j = 0; j < count && i < size; j++, i++)
				lc[i ^ 1] = lp[((i + offset + size) % size) ^ 1];
		} else {
			uint8_t byte = 0;
			for (int b = 0; b < 8; b++)
				byte = (uint8_t) ((byte << 1) | get_compressed_bit ());
			for (int j = 0; j < count && i < size; j++, i++)
				lc[i ^ 1] = byte;
		}
	}

	if (++s.block_pos == s.block_numlines)
		s.done_compression = 1;
}

} /* namespace hikaru */
