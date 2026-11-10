/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/naomigd.cpp (license BSD-3-Clause,
 * copyright-holders Olivier Galibert) and Flycast's
 * core/hw/naomi/gdcartridge.cpp and core/imgread/chd.cpp (GPL-2.0-or-later,
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

/*
 * The GD-ROM and DIMM Board
 * =========================
 *
 * The games on a GD-ROM run from a DIMM board: its firmware reads the disc's
 * file, DES-decrypted with the key of the board's PIC, into its memory, then
 * shows it to the NAOMI as a cartridge's ROM. Here the file is read and
 * decrypted once, before the game starts.
 *
 * The registers 005F703C-005F704C are the board's mailbox: a command (group in
 * bits 13-14, code in bits 9-12), its parameters; the NAOMI raises the board's
 * interrupt by writing bit 0 of the status low, and the board the NAOMI's EXT
 * interrupt (PCI). The firmware's start-up leaves what it found at 0C01FC04.
 * Every second it looks at a word the BIOS's board test sets (0C01FC08).
 *
 * The Net DIMM boards (games.json "net_dimm") hold their firmware 3.17 and the
 * memory of a network game: the top 16 MB of it are the buffers of the commands,
 * written by the NAOMI through the cartridge's PIO data port; the commands of
 * group 1 are the network's (DimmNet, naomi-netdimm.cpp).
 *
 * A GD-ROM keeps its game in the high-density area, from sector 45000; a CD
 * (the World Club Champion Football and Mahjong MJ boards) in its only track,
 * its file in the ROM directory. Which of the two is written on the disc
 * itself: byte 6EE of the PIC (the network firmware's) does not say; Dragon
 * Treasure's GD-ROMs have it set.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <libchdr/chd.h>

#include "hl/config.h"
#include "mach/naomi/naomi.h"

namespace naomi {

namespace {

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

static const u32 DES_LEFTSWAP[] = {
	0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
	0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101
};

static const u32 DES_RIGHTSWAP[] = {
	0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
	0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

static const u32 DES_SBOX1[] = {
	0x00808200, 0x00000000, 0x00008000, 0x00808202, 0x00808002, 0x00008202, 0x00000002, 0x00008000,
	0x00000200, 0x00808200, 0x00808202, 0x00000200, 0x00800202, 0x00808002, 0x00800000, 0x00000002,
	0x00000202, 0x00800200, 0x00800200, 0x00008200, 0x00008200, 0x00808000, 0x00808000, 0x00800202,
	0x00008002, 0x00800002, 0x00800002, 0x00008002, 0x00000000, 0x00000202, 0x00008202, 0x00800000,
	0x00008000, 0x00808202, 0x00000002, 0x00808000, 0x00808200, 0x00800000, 0x00800000, 0x00000200,
	0x00808002, 0x00008000, 0x00008200, 0x00800002, 0x00000200, 0x00000002, 0x00800202, 0x00008202,
	0x00808202, 0x00008002, 0x00808000, 0x00800202, 0x00800002, 0x00000202, 0x00008202, 0x00808200,
	0x00000202, 0x00800200, 0x00800200, 0x00000000, 0x00008002, 0x00008200, 0x00000000, 0x00808002
};

static const u32 DES_SBOX2[] = {
	0x40084010, 0x40004000, 0x00004000, 0x00084010, 0x00080000, 0x00000010, 0x40080010, 0x40004010,
	0x40000010, 0x40084010, 0x40084000, 0x40000000, 0x40004000, 0x00080000, 0x00000010, 0x40080010,
	0x00084000, 0x00080010, 0x40004010, 0x00000000, 0x40000000, 0x00004000, 0x00084010, 0x40080000,
	0x00080010, 0x40000010, 0x00000000, 0x00084000, 0x00004010, 0x40084000, 0x40080000, 0x00004010,
	0x00000000, 0x00084010, 0x40080010, 0x00080000, 0x40004010, 0x40080000, 0x40084000, 0x00004000,
	0x40080000, 0x40004000, 0x00000010, 0x40084010, 0x00084010, 0x00000010, 0x00004000, 0x40000000,
	0x00004010, 0x40084000, 0x00080000, 0x40000010, 0x00080010, 0x40004010, 0x40000010, 0x00080010,
	0x00084000, 0x00000000, 0x40004000, 0x00004010, 0x40000000, 0x40080010, 0x40084010, 0x00084000
};

static const u32 DES_SBOX3[] = {
	0x00000104, 0x04010100, 0x00000000, 0x04010004, 0x04000100, 0x00000000, 0x00010104, 0x04000100,
	0x00010004, 0x04000004, 0x04000004, 0x00010000, 0x04010104, 0x00010004, 0x04010000, 0x00000104,
	0x04000000, 0x00000004, 0x04010100, 0x00000100, 0x00010100, 0x04010000, 0x04010004, 0x00010104,
	0x04000104, 0x00010100, 0x00010000, 0x04000104, 0x00000004, 0x04010104, 0x00000100, 0x04000000,
	0x04010100, 0x04000000, 0x00010004, 0x00000104, 0x00010000, 0x04010100, 0x04000100, 0x00000000,
	0x00000100, 0x00010004, 0x04010104, 0x04000100, 0x04000004, 0x00000100, 0x00000000, 0x04010004,
	0x04000104, 0x00010000, 0x04000000, 0x04010104, 0x00000004, 0x00010104, 0x00010100, 0x04000004,
	0x04010000, 0x04000104, 0x00000104, 0x04010000, 0x00010104, 0x00000004, 0x04010004, 0x00010100
};

static const u32 DES_SBOX4[] = {
	0x80401000, 0x80001040, 0x80001040, 0x00000040, 0x00401040, 0x80400040, 0x80400000, 0x80001000,
	0x00000000, 0x00401000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00400040, 0x80400000,
	0x80000000, 0x00001000, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x80001000, 0x00001040,
	0x80400040, 0x80000000, 0x00001040, 0x00400040, 0x00001000, 0x00401040, 0x80401040, 0x80000040,
	0x00400040, 0x80400000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00000000, 0x00401000,
	0x00001040, 0x00400040, 0x80400040, 0x80000000, 0x80401000, 0x80001040, 0x80001040, 0x00000040,
	0x80401040, 0x80000040, 0x80000000, 0x00001000, 0x80400000, 0x80001000, 0x00401040, 0x80400040,
	0x80001000, 0x00001040, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x00001000, 0x00401040
};

static const u32 DES_SBOX5[] = {
	0x00000080, 0x01040080, 0x01040000, 0x21000080, 0x00040000, 0x00000080, 0x20000000, 0x01040000,
	0x20040080, 0x00040000, 0x01000080, 0x20040080, 0x21000080, 0x21040000, 0x00040080, 0x20000000,
	0x01000000, 0x20040000, 0x20040000, 0x00000000, 0x20000080, 0x21040080, 0x21040080, 0x01000080,
	0x21040000, 0x20000080, 0x00000000, 0x21000000, 0x01040080, 0x01000000, 0x21000000, 0x00040080,
	0x00040000, 0x21000080, 0x00000080, 0x01000000, 0x20000000, 0x01040000, 0x21000080, 0x20040080,
	0x01000080, 0x20000000, 0x21040000, 0x01040080, 0x20040080, 0x00000080, 0x01000000, 0x21040000,
	0x21040080, 0x00040080, 0x21000000, 0x21040080, 0x01040000, 0x00000000, 0x20040000, 0x21000000,
	0x00040080, 0x01000080, 0x20000080, 0x00040000, 0x00000000, 0x20040000, 0x01040080, 0x20000080
};

static const u32 DES_SBOX6[] = {
	0x10000008, 0x10200000, 0x00002000, 0x10202008, 0x10200000, 0x00000008, 0x10202008, 0x00200000,
	0x10002000, 0x00202008, 0x00200000, 0x10000008, 0x00200008, 0x10002000, 0x10000000, 0x00002008,
	0x00000000, 0x00200008, 0x10002008, 0x00002000, 0x00202000, 0x10002008, 0x00000008, 0x10200008,
	0x10200008, 0x00000000, 0x00202008, 0x10202000, 0x00002008, 0x00202000, 0x10202000, 0x10000000,
	0x10002000, 0x00000008, 0x10200008, 0x00202000, 0x10202008, 0x00200000, 0x00002008, 0x10000008,
	0x00200000, 0x10002000, 0x10000000, 0x00002008, 0x10000008, 0x10202008, 0x00202000, 0x10200000,
	0x00202008, 0x10202000, 0x00000000, 0x10200008, 0x00000008, 0x00002000, 0x10200000, 0x00202008,
	0x00002000, 0x00200008, 0x10002008, 0x00000000, 0x10202000, 0x10000000, 0x00200008, 0x10002008
};

static const u32 DES_SBOX7[] = {
	0x00100000, 0x02100001, 0x02000401, 0x00000000, 0x00000400, 0x02000401, 0x00100401, 0x02100400,
	0x02100401, 0x00100000, 0x00000000, 0x02000001, 0x00000001, 0x02000000, 0x02100001, 0x00000401,
	0x02000400, 0x00100401, 0x00100001, 0x02000400, 0x02000001, 0x02100000, 0x02100400, 0x00100001,
	0x02100000, 0x00000400, 0x00000401, 0x02100401, 0x00100400, 0x00000001, 0x02000000, 0x00100400,
	0x02000000, 0x00100400, 0x00100000, 0x02000401, 0x02000401, 0x02100001, 0x02100001, 0x00000001,
	0x00100001, 0x02000000, 0x02000400, 0x00100000, 0x02100400, 0x00000401, 0x00100401, 0x02100400,
	0x00000401, 0x02000001, 0x02100401, 0x02100000, 0x00100400, 0x00000000, 0x00000001, 0x02100401,
	0x00000000, 0x00100401, 0x02100000, 0x00000400, 0x02000001, 0x02000400, 0x00000400, 0x00100001
};

static const u32 DES_SBOX8[] = {
	0x08000820, 0x00000800, 0x00020000, 0x08020820, 0x08000000, 0x08000820, 0x00000020, 0x08000000,
	0x00020020, 0x08020000, 0x08020820, 0x00020800, 0x08020800, 0x00020820, 0x00000800, 0x00000020,
	0x08020000, 0x08000020, 0x08000800, 0x00000820, 0x00020800, 0x00020020, 0x08020020, 0x08020800,
	0x00000820, 0x00000000, 0x00000000, 0x08020020, 0x08000020, 0x08000800, 0x00020820, 0x00020000,
	0x00020820, 0x00020000, 0x08020800, 0x00000800, 0x00000020, 0x08020020, 0x00000800, 0x00020820,
	0x08000800, 0x00000020, 0x08000020, 0x08020000, 0x08020020, 0x08000000, 0x00020000, 0x08000820,
	0x00000000, 0x08020820, 0x00020020, 0x08000020, 0x08020000, 0x08000800, 0x08000820, 0x00000000,
	0x08020820, 0x00020800, 0x00020800, 0x00000820, 0x00000820, 0x00020020, 0x08000000, 0x08020800
};

static const u32 DES_MASK_TABLE[] = {
	0x24000000, 0x10000000, 0x08000000, 0x02080000, 0x01000000,
	0x00200000, 0x00100000, 0x00040000, 0x00020000, 0x00010000,
	0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200,
	0x00000100, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
	0x00000002, 0x00000001, 0x20000000, 0x10000000, 0x08000000,
	0x04000000, 0x02000000, 0x01000000, 0x00200000, 0x00100000,
	0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00002000,
	0x00001000, 0x00000808, 0x00000400, 0x00000200, 0x00000100,
	0x00000020, 0x00000011, 0x00000004, 0x00000002
};

static const u8 DES_ROTATE_TABLE[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

constexpr int FILENAME_LENGTH = 24;

inline static void permutate(u32 &a, u32 &b, u32 m, int shift)
{
	u32 temp = ((a>>shift) ^ b) & m;
	a ^= temp<<shift;
	b ^= temp;
}

static void des_generate_subkeys(const u64 key, u32 *subkeys)
{
	u32 l = key >> 32;
	u32 r = key;

	permutate(r, l, 0x0f0f0f0f, 4);
	permutate(r, l, 0x10101010, 0);

	l = (DES_LEFTSWAP[(l >> 0)  & 0xf] << 3) |
		(DES_LEFTSWAP[(l >> 8)  & 0xf] << 2) |
		(DES_LEFTSWAP[(l >> 16) & 0xf] << 1) |
		(DES_LEFTSWAP[(l >> 24) & 0xf] << 0) |
		(DES_LEFTSWAP[(l >> 5)  & 0xf] << 7) |
		(DES_LEFTSWAP[(l >> 13) & 0xf] << 6) |
		(DES_LEFTSWAP[(l >> 21) & 0xf] << 5) |
		(DES_LEFTSWAP[(l >> 29) & 0xf] << 4);

	r = (DES_RIGHTSWAP[(r >> 1)  & 0xf] << 3) |
		(DES_RIGHTSWAP[(r >> 9)  & 0xf] << 2) |
		(DES_RIGHTSWAP[(r >> 17) & 0xf] << 1) |
		(DES_RIGHTSWAP[(r >> 25) & 0xf] << 0) |
		(DES_RIGHTSWAP[(r >> 4)  & 0xf] << 7) |
		(DES_RIGHTSWAP[(r >> 12) & 0xf] << 6) |
		(DES_RIGHTSWAP[(r >> 20) & 0xf] << 5) |
		(DES_RIGHTSWAP[(r >> 28) & 0xf] << 4);

	l &= 0x0fffffff;
	r &= 0x0fffffff;


	for(int round = 0; round < 16; round++) {
		l = ((l << DES_ROTATE_TABLE[round]) | (l >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;
		r = ((r << DES_ROTATE_TABLE[round]) | (r >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;

		subkeys[round*2] =
			((l << 4)  & DES_MASK_TABLE[0]) |
			((l << 28) & DES_MASK_TABLE[1]) |
			((l << 14) & DES_MASK_TABLE[2]) |
			((l << 18) & DES_MASK_TABLE[3]) |
			((l << 6)  & DES_MASK_TABLE[4]) |
			((l << 9)  & DES_MASK_TABLE[5]) |
			((l >> 1)  & DES_MASK_TABLE[6]) |
			((l << 10) & DES_MASK_TABLE[7]) |
			((l << 2)  & DES_MASK_TABLE[8]) |
			((l >> 10) & DES_MASK_TABLE[9]) |
			((r >> 13) & DES_MASK_TABLE[10])|
			((r >> 4)  & DES_MASK_TABLE[11])|
			((r << 6)  & DES_MASK_TABLE[12])|
			((r >> 1)  & DES_MASK_TABLE[13])|
			((r >> 14) & DES_MASK_TABLE[14])|
			((r >> 0)  & DES_MASK_TABLE[15])|
			((r >> 5)  & DES_MASK_TABLE[16])|
			((r >> 10) & DES_MASK_TABLE[17])|
			((r >> 3)  & DES_MASK_TABLE[18])|
			((r >> 18) & DES_MASK_TABLE[19])|
			((r >> 26) & DES_MASK_TABLE[20])|
			((r >> 24) & DES_MASK_TABLE[21]);

		subkeys[round*2+1] =
			((l << 15) & DES_MASK_TABLE[22])|
			((l << 17) & DES_MASK_TABLE[23])|
			((l << 10) & DES_MASK_TABLE[24])|
			((l << 22) & DES_MASK_TABLE[25])|
			((l >> 2)  & DES_MASK_TABLE[26])|
			((l << 1)  & DES_MASK_TABLE[27])|
			((l << 16) & DES_MASK_TABLE[28])|
			((l << 11) & DES_MASK_TABLE[29])|
			((l << 3)  & DES_MASK_TABLE[30])|
			((l >> 6)  & DES_MASK_TABLE[31])|
			((l << 15) & DES_MASK_TABLE[32])|
			((l >> 4)  & DES_MASK_TABLE[33])|
			((r >> 2)  & DES_MASK_TABLE[34])|
			((r << 8)  & DES_MASK_TABLE[35])|
			((r >> 14) & DES_MASK_TABLE[36])|
			((r >> 9)  & DES_MASK_TABLE[37])|
			((r >> 0)  & DES_MASK_TABLE[38])|
			((r << 7)  & DES_MASK_TABLE[39])|
			((r >> 7)  & DES_MASK_TABLE[40])|
			((r >> 3)  & DES_MASK_TABLE[41])|
			((r << 2)  & DES_MASK_TABLE[42])|
			((r >> 21) & DES_MASK_TABLE[43]);
	}
}

template<bool decrypt>
u64 des_encrypt_decrypt(u64 src, const u32 *des_subkeys)
{
	u32 r = (src & 0x00000000ffffffffULL) >> 0;
	u32 l = (src & 0xffffffff00000000ULL) >> 32;

	permutate(l, r, 0x0f0f0f0f, 4);
	permutate(l, r, 0x0000ffff, 16);
	permutate(r, l, 0x33333333, 2);
	permutate(r, l, 0x00ff00ff, 8);
	permutate(l, r, 0x55555555, 1);

	int subkey;
	if constexpr (decrypt)
		subkey = 30;
	else
		subkey = 0;

	for(int i = 0; i < 32 ; i+=4) {
		u32 temp;

		temp = ((r<<1) | (r>>31)) ^ des_subkeys[subkey];
		l ^= DES_SBOX8[ (temp>>0)  & 0x3f ];
		l ^= DES_SBOX6[ (temp>>8)  & 0x3f ];
		l ^= DES_SBOX4[ (temp>>16) & 0x3f ];
		l ^= DES_SBOX2[ (temp>>24) & 0x3f ];
		subkey++;

		temp = ((r>>3) | (r<<29)) ^ des_subkeys[subkey];
		l ^= DES_SBOX7[ (temp>>0)  & 0x3f ];
		l ^= DES_SBOX5[ (temp>>8)  & 0x3f ];
		l ^= DES_SBOX3[ (temp>>16) & 0x3f ];
		l ^= DES_SBOX1[ (temp>>24) & 0x3f ];
		subkey++;
		if constexpr (decrypt)
			subkey -= 4;

		temp = ((l<<1) | (l>>31)) ^ des_subkeys[subkey];
		r ^= DES_SBOX8[ (temp>>0)  & 0x3f ];
		r ^= DES_SBOX6[ (temp>>8)  & 0x3f ];
		r ^= DES_SBOX4[ (temp>>16) & 0x3f ];
		r ^= DES_SBOX2[ (temp>>24) & 0x3f ];
		subkey++;

		temp = ((l>>3) | (l<<29)) ^ des_subkeys[subkey];
		r ^= DES_SBOX7[ (temp>>0)  & 0x3f ];
		r ^= DES_SBOX5[ (temp>>8)  & 0x3f ];
		r ^= DES_SBOX3[ (temp>>16) & 0x3f ];
		r ^= DES_SBOX1[ (temp>>24) & 0x3f ];
		subkey++;
		if constexpr (decrypt)
			subkey -= 4;
	}

	permutate(r, l, 0x55555555, 1);
	permutate(l, r, 0x00ff00ff, 8);
	permutate(l, r, 0x33333333, 2);
	permutate(r, l, 0x0000ffff, 16);
	permutate(r, l, 0x0f0f0f0f, 4);

	return (u64(r) << 32) | u64(l);
}

inline static u64 rev64(u64 src)
{
	u64 ret;

	ret = ((src & 0x00000000000000ffULL) << 56)
		| ((src & 0x000000000000ff00ULL) << 40)
		| ((src & 0x0000000000ff0000ULL) << 24)
		| ((src & 0x00000000ff000000ULL) << 8 )
		| ((src & 0x000000ff00000000ULL) >> 8 )
		| ((src & 0x0000ff0000000000ULL) >> 24)
		| ((src & 0x00ff000000000000ULL) >> 40)
		| ((src & 0xff00000000000000ULL) >> 56);

	return ret;
}

static void find_file(const char *name, const u8 *dir_sector, u32 &file_start, u32 &file_size)
{
	file_start = 0;
	file_size = 0;
	for (u32 pos = 0; pos < 2048 && dir_sector[pos] != 0; pos += dir_sector[pos])
	{
		if (dir_sector[pos + 25] & 2)
			continue;

		char fname[FILENAME_LENGTH + 1] {};
		const int len = std::min<int>(dir_sector[pos + 32], FILENAME_LENGTH);
		for (int i = 0; i < len; i++)
		{
			u8 c = dir_sector[pos + 33 + i];
			if (c == ';')
				break;
			fname[i] = c;
		}
		printf("file: [%s]\n", fname);fflush(stdout);
		bool found = false;
		if (name[0] == '*')
		{
			char *p = strchr(fname, name[1]);
			if (p != nullptr && !strcasecmp(p, &name[1]))
				found = true;
		}
		else
		{
			found = strcmp(fname, name) == 0;
		}
		if (found)
		{
			// start sector and size of file
			file_start = *(u32 *)&dir_sector[pos + 2];
			file_size = *(u32 *)&dir_sector[pos + 10];

			return;
		}
	}
}


} /* namespace */

/* A disc's sectors, from a CHD */
class GdDisc {
public:
	~GdDisc ()
	{
		if (chd)
			chd_close (chd);
	}

	bool open (const std::string &path)
	{
		if (chd_open (path.c_str (), CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE)
			return false;

		const chd_header *head = chd_get_header (chd);
		uint32_t total = 150, offset = 0;

		hunk.resize (head->hunkbytes);
		per_hunk = head->hunkbytes / (2352 + 96);
		for (int n = 0; ; n++) {
			char text[512], type[16], subtype[16], pgtype[16], pgsub[16];
			int id = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
			uint32_t len, tag;
			uint8_t flags;

			if (chd_get_metadata (chd, CDROM_TRACK_METADATA2_TAG, n, text, sizeof (text), &len, &tag, &flags) == CHDERR_NONE) {
				/* A CD: the World Club Champion Football and Mahjong MJ boards */
				sscanf (text, CDROM_TRACK_METADATA2_FORMAT, &id, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
				if (n == 0)
					pregap = 0;
			} else if (chd_get_metadata (chd, CDROM_TRACK_METADATA_TAG, n, text, sizeof (text), &len, &tag, &flags) == CHDERR_NONE) {
				sscanf (text, CDROM_TRACK_METADATA_FORMAT, &id, type, subtype, &frames);
			} else if (chd_get_metadata (chd, GDROM_OLD_METADATA_TAG, n, text, sizeof (text), &len, &tag, &flags) != CHDERR_NONE &&
				   chd_get_metadata (chd, GDROM_TRACK_METADATA_TAG, n, text, sizeof (text), &len, &tag, &flags) != CHDERR_NONE) {
				break;
			} else {
				sscanf (text, GDROM_TRACK_METADATA_FORMAT, &id, type, subtype, &frames, &pad, &pregap, pgtype, pgsub, &postgap);
			}

			Track t;
			printf("track %d: [%s]\n", n, text); fflush(stdout);
			uint32_t in_file =(pregap > 0 && pgtype[0] == 'V') ? (uint32_t) pregap : 0;

			t.start = total + pregap;
			total = t.start + frames - in_file;
			t.end = total - 1;
			t.offset = (int32_t) (offset + in_file) - (int32_t) t.start;
			t.raw = strcmp (type, "MODE1") && strcmp (type, "MODE1/2048") != 0;
			tracks.push_back (t);
			offset += (frames + 3) / 4 * 4;
		}
		return !tracks.empty () && per_hunk;
	}

	/* The 2048 bytes of the sector at this FAD */
	bool read (uint32_t fad, uint8_t *out)
	{
		for (const Track &t : tracks) {
			if (fad < t.start || fad > t.end)
				continue;

			uint32_t at = (uint32_t) ((int32_t) fad + t.offset);
			uint32_t which = at / per_hunk;

			if (which != current) {
				if (chd_read (chd, which, hunk.data ()) != CHDERR_NONE)
					return false;
				current = which;
			}
			memcpy (out, hunk.data () + (at % per_hunk) * (2352 + 96) + (t.raw ? 16 : 0), 2048);
			return true;
		}
		return false;
	}

private:
	struct Track {
		uint32_t start, end;
		int32_t offset;
		bool raw;
	};

	chd_file *chd = NULL;
	std::vector<uint8_t> hunk;
	std::vector<Track> tracks;
	uint32_t per_hunk = 0, current = ~0u;
};

/* Where the game's CHD is: <folder>/<game>/<name>.chd, or its parent's folder */
static std::string
find_chd (hl::Game *game, const char *name)
{
	char list[4096];
	std::string folders;

	hl::Config::get ()->games_dir_in_use (list, sizeof (list));
	folders = list;
	size_t at = 0;
	while (at <= folders.size ()) {
		size_t end = folders.find (';', at);
		std::string dir = folders.substr (at, end == std::string::npos ? end : end - at);

		for (const char *sub : { game->name, game->parent }) {
			if (!sub[0])
				continue;
			std::string path = dir + "/" + sub + "/" + name + ".chd";
			FILE *fp = fopen (path.c_str (), "rb");

			if (fp) {
				fclose (fp);
				return path;
			}
		}
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return "";
}

hl::Buffer *
GdCart::load (hl::Machine *machine, hl::Game *game, const uint8_t *pic, uint32_t pic_size)
{
	char name[128];
	uint64_t key = 0;
	uint8_t buffer[2048], dir[2048];
	uint32_t file_start = 0, file_size = 0, i, dir_sector = 0;
	bool on_cd = false;
	GdDisc disc;
	std::string path = find_chd (game, game->gdrom);

	if (path.empty ()) {
		HL_ERROR ("naomi: %s: %s.chd not found (in a folder named as the game)", game->name, game->gdrom);
		return NULL;
	}
	if (!disc.open (path)) {
		HL_ERROR ("naomi: '%s' is no GD-ROM image", path.c_str ());
		return NULL;
	}
	if (pic_size < 0x4000) {
		HL_ERROR ("naomi: %s: the DIMM board's PIC is missing", game->name);
		return NULL;
	}
	memset (name, 0, sizeof (name));
	for (i = 0; i < 7; i++) {
		name[i] = (char) pic[0x7C0 + i * 2];
		name[i + 7] = (char) pic[0x7E0 + i * 2];
		key |= (uint64_t) pic[0x780 + i * 2] << (56 - i * 8);
	}
	key |= pic[0x7A0];

	auto sector = [&] (uint32_t s, uint8_t *out) { return disc.read (s + 150, out); };

	/* The primary volume descriptor, the path table: at the high-density
	 * area's start when the disc has one */
	printf("name %s\n",name);fflush(stdout);
	on_cd = !sector (45000 + 16, buffer) || memcmp (buffer + 1, "CD001", 5) != 0;
	if (on_cd && !sector (16, buffer))
		return NULL;
	uint32_t table = buffer[0x8C] | (buffer[0x8D] << 8) | (buffer[0x8E] << 16) | ((uint32_t) buffer[0x8F] << 24);
	if (!sector (table, buffer))
		return NULL;
	printf("oncd %d table %u\n", on_cd, table); for (int z = 0; z < 96; z++) printf("%02x ", buffer[z]); printf("\n"); fflush(stdout);
	if (!on_cd) {
		uint32_t d = buffer[2] | (buffer[3] << 8) | (buffer[4] << 16) | ((uint32_t) buffer[5] << 24);

		dir_sector = d;
		if (!sector (d, dir))
			return NULL;
		find_file (name, dir, file_start, file_size);
		if (file_start && file_size == 0x100) {
			if (!sector (file_start, buffer))
				return NULL;
			memset (name, 0, sizeof (name));
			memcpy (name, buffer + 0xC0, FILENAME_LENGTH - 1);
		}
	} else {
		bool found = false;

		for (i = 0; i < 2048 && buffer[i]; i += buffer[i] + 8 + (buffer[i] & 1)) {
			if (buffer[i] == 3 && buffer[i + 8] == 'R' && buffer[i + 9] == 'O' && buffer[i + 10] == 'M') {
				uint32_t d = buffer[i + 2] | (buffer[i + 3] << 8) | (buffer[i + 4] << 16) | ((uint32_t) buffer[i + 5] << 24);

				memcpy (name, "ROM.BIN", 8);
				dir_sector = d;
				if (!sector (d, dir))
					return NULL;
				found = true;
				break;
			}
		}
		if (!found) {
			uint32_t d = buffer[2] | (buffer[3] << 8) | (buffer[4] << 16) | ((uint32_t) buffer[5] << 24);

			dir_sector = d;
			if (!sector (d, dir))
				return NULL;
		}
	}
	/* The directory: as many sectors as its own first record says */
	{
		uint32_t where = dir_sector, sectors = (dir[10] | (dir[11] << 8) | (dir[12] << 16) | ((uint32_t) dir[13] << 24)) / 2048;

		sectors = MAX2 (1u, MIN2 (sectors, 256u));
		file_start = file_size = 0;
		/* The name the PIC gives; else, its name not on the disc, any
		 * .BIN (the update discs) */
		for (const char *wanted : { (const char *) name, "*.BIN" }) {
			for (i = 0; i < sectors && !file_start; i++) {
				if (!sector (where + i, dir))
					return NULL;
				find_file (wanted, dir, file_start, file_size);
			}
			if (file_start)
				break;
		}
	}
	if (!file_start) {
		HL_ERROR ("naomi: %s: the game's file '%s' is not on the disc", game->name, name);
		return NULL;
	}
	uint32_t rounded = (file_size + 2047) & ~2047u, size = 4096;

	/* A Net DIMM has 128, 256 or 512 MB, its buffers in the top 16 */
	if (game->net_dimm) {
		size = 128 * MB;
		while (size < rounded + 16 * MB)
			size <<= 1;
	} else {
		while (size < rounded)
			size <<= 1;
	}
	hl::Buffer *dimm = hl::Buffer::create (size, 0, hl::Buffer::LE32);
	if (!dimm)
		return NULL;
	/* Not one of the machine's RAMs: a reset would clear it */
	(void) machine;
	HL_PRINT ("naomi: GD-ROM '%s': file %s, %u MB, read and decrypted...", path.c_str (), name, file_size >> 20);

	u32 subkeys[32];
	uint8_t *to = (uint8_t *) dimm->ptr (0);

	des_generate_subkeys (rev64 (key), subkeys);
	for (i = 0; i < rounded / 2048; i++) {
		if (!sector (file_start + i, to + i * 2048)) {
			HL_ERROR ("naomi: %s: cannot read sector %u", game->name, file_start + i);
			return NULL;
		}
		uint64_t *block = (uint64_t *) (to + i * 2048);

		for (unsigned k = 0; k < 256; k++)
			block[k] = des_encrypt_decrypt<true> (block[k], subkeys);
	}
	return dimm;
}

GdCart::GdCart (Board *board_, hl::Buffer *dimm) :
	Cart (dimm, Cart::TYPE_ROM, 0, 0, NULL),
	board (board_),
	bytes (dimm->size ()),
	memory ((uint8_t *) dimm->ptr (0)),
	lines_ns (0),
	mailbox_wait (0)
{
	memset (&d, 0, sizeof (d));
	if (board->game->net_dimm)
		net.reset (new DimmNet (memory, bytes, bytes - 16 * MB));
}

void
GdCart::poke (uint32_t addr, uint32_t val)
{
	board->mmap->put (4, addr, val);
}

uint32_t
GdCart::peek (uint32_t addr)
{
	uint32_t val = 0;

	board->mmap->get (4, addr, &val);
	return val;
}

void
GdCart::interrupt (bool on)
{
	d.irq = on;
	board->holly->set_external (8, on);
}

void
GdCart::reply (uint16_t command, bool failed, uint16_t offset, uint32_t parameter)
{
	d.command = (uint16_t) ((command + 0x400) | (failed ? 0xFF : 4));
	d.offset = offset;
	d.parameterh = (uint16_t) (parameter >> 16);
	d.parameterl = (uint16_t) parameter;
	interrupt (true);
}

void
GdCart::command ()
{
	const int group = (d.command >> 13) & 3, cmd = (d.command >> 9) & 15;

	if (group != 0) {
		if (net && group == 1) {
			DimmNet::Reply r;

			if (net->command ((uint16_t) (0x2000 | (cmd << 9)), (uint8_t) d.command, r))
				reply (r.command, r.failed, r.offset, r.parameter);
			return;
		}
		/* No board on a network here */
		reply (d.command & 0x7E00, true, 0, 0xFFFFFFFF);
		return;
	}
	if (cmd == 15) {
		/* The firmware's start-up: one DIMM of the file's size, a
		 * GD-ROM connected, firmware 1.02, then the base address */
		uint32_t config = (bytes >= 512 * MB ? 3u : bytes >= 256 * MB ? 2u : 1u) << 16;

		if (net)
			net_startup ();
		else {
			poke (0x0C01FC04, config | 0x60000000 | (bytes >> 20));
			poke (0x0C01FC0C, 0x1020000 | 0x264);
		}
		/* The DIMM board's serial number: the X76F100's */
		for (unsigned i = 0; i < 4; i++) {
			uint32_t w;

			memcpy (&w, eeprom.image + hl::X76F100::IMAGE_DATA + 0x20 + i * 4, 4);
			poke (0x0C01FC40 + i * 4, w);
		}
		d.command = 0x8600;
		d.offset = 0;
		d.parameterl = 0;
		d.parameterh = 0x0C00;
		interrupt (true);
		d.started = 1;
	}
}

/* Firmware 3.17's: network enabled and its settings, the buffers' offset */
void
GdCart::net_startup ()
{
	uint32_t size = bytes >= 512 * MB ? 3u : bytes >= 256 * MB ? 2u : 1u;
	uint32_t ip = net->address ();
	uint32_t a = (ip >> 24) | ((ip >> 8) & 0xFF00) | ((ip << 8) & 0xFF0000) | (ip << 24);

	poke (0x0C01FC04, (size << 16) | 0x78000000 | ((bytes - 16 * MB) >> 20));
	poke (0x0C01FC0C, 0x3170000 | 0x264);
	poke (0x0C01FC10, 0);
	poke (0x0C01FC14, 1);
	poke (0x0C01FC20, 0x78000);
	poke (0x0C01FC24, 0x3E000A);
	poke (0x0C01FC28, 0x18077F);
	poke (0x0C01FC2C, 0x10014);
	poke (0x0C01FC18, 0x10002);
	/* Network order; the mask, the gateway and the DNS of a /24 */
	poke (0x0C01FC60, a);
	poke (0x0C01FC64, 0x00FFFFFF);
	poke (0x0C01FC68, (a & 0x00FFFFFF) | 0x01000000);
	poke (0x0C01FC6C, (a & 0x00FFFFFF) | 0x01000000);
	poke (0x0C01FC70, 0);
	for (uint32_t at = 0x0C01FC74; at <= 0x0C01FC94; at += 4)
		poke (at, 0);
}

void
GdCart::text (uint32_t addr, const char *line)
{
	char t[16] = { 0 };

	strncpy (t, line, sizeof (t));
	for (unsigned k = 0; k < 16; k += 4) {
		uint32_t w;

		memcpy (&w, t + k, 4);
		poke (addr + k, w);
	}
}

/* What a Net DIMM's firmware answers to the test request */
void
GdCart::net_test (uint32_t request)
{
	if (request & 1) {
		poke (0x0C01FC08, request & ~1u);
		text (0x0C01FD00, "CHECKING DIMM BD");
		text (0x0C01FD10, "DIMM0 - GOOD");
		text (0x0C01FD20, "DIMM1 - GOOD");
		text (0x0C01FD30, "--- COMPLETED---");
		poke (0x0C01FC0C, 0x0317A264);
	} else if (request & 0x40) {
		uint32_t ip = net->address ();

		poke (0x0C01FC08, request & ~0x40u);
		poke (0x0C01FC60, (ip >> 24) | ((ip >> 8) & 0xFF00) | ((ip << 8) & 0xFF0000) | (ip << 24));
		poke (0x0C01FC0C, 0x03170264);
	} else if (request & 0x400) {
		poke (0x0C01FC08, request & ~0x400u);
		poke (0x0C01FC70, 0);
		poke (0x0C01FC0C, 0x03170264);
	} else if (request & 0x10000) {
		poke (0x0C01FC08, request & ~0x10000u);
		poke (0x0C01FC0C, 0x03170264);
	} else if (request & 0x20000) {
		poke (0x0C01FC08, request & ~0x20000u);
		text (0x0C01FD00, "CHECKING NETWORK");
		text (0x0C01FD10, "NO CABLE TEST");
		text (0x0C01FD20, "--- COMPLETED---");
		poke (0x0C01FC0C, 0x03170264);
	} else if (request) {
		poke (0x0C01FC08, 0);
		poke (0x0C01FC0C, 0x03170100);
	}
}

void
GdCart::poll (unsigned lines)
{
	if (mailbox_wait)
		mailbox_wait--;
	if (!net)
		return;

	/* 60 frames of 544 lines to a second: close enough for time-outs */
	lines_ns += (uint64_t) lines * 30637;

	uint64_t us = lines_ns / 1000;

	lines_ns %= 1000;
	if (d.irq || mailbox_wait)
		return;

	DimmNet::Reply r;

	if (net->poll (us, r))
		reply (r.command, r.failed, r.offset, r.parameter);
}

/* Each frame: once a second the board looks at the test request */
void
GdCart::frame ()
{
	if (!d.started || ++d.frames < 60 || d.irq || mailbox_wait)
		return;
	d.frames = 0;

	uint32_t request = peek (0x0C01FC08);

	d.command = (uint16_t) ((6 << 9) | 0x8000 | ((0x0C01FC08 >> 16) & 0x1FF));
	d.offset = 0xFC08;
	d.parameterl = d.parameterh = 0;
	interrupt (true);
	if (net) {
		net_test (request);
		return;
	}
	if (request & 1) {
		static const char *const lines[4] = { "CHECKING DIMM BD", "DIMM0 - GOOD", "DIMM1 - GOOD", "--- COMPLETED---" };

		poke (0x0C01FC08, request & ~1u);
		for (unsigned i = 0; i < 4; i++) {
			char text[16] = { 0 };

			strncpy (text, lines[i], sizeof (text));
			for (unsigned k = 0; k < 16; k += 4) {
				uint32_t w;

				memcpy (&w, text + k, 4);
				poke (0x0C01FD00 + i * 16 + k, w);
			}
		}
		poke (0x0C01FC0C, 0x0102A264);
	} else if (request) {
		poke (0x0C01FC08, 0);
		poke (0x0C01FC0C, 0x03170100);
	}
}

uint32_t
GdCart::get (unsigned reg)
{
	switch (reg) {
	case 0x3C:
		return d.command;
	case 0x40:
		return d.offset;
	case 0x44:
		return d.parameterl;
	case 0x48:
		return d.parameterh;
	case 0x4C:
		return 0x111 & ~((d.irq ? 1u : 0u) << 8);
	default:
		return Cart::get (reg);
	}
}

void
GdCart::put (unsigned reg, uint32_t val)
{
	switch (reg) {
	case 0x3C:
		d.command = (uint16_t) val;
		mailbox_wait = 8;
		break;
	case 0x40:
		d.offset = (uint16_t) val;
		mailbox_wait = 8;
		break;
	case 0x44:
		d.parameterl = (uint16_t) val;
		mailbox_wait = 8;
		break;
	case 0x48:
		d.parameterh = (uint16_t) val;
		mailbox_wait = 8;
		break;
	case 0x4C:
		if (val & 0x100)
			interrupt (false);
		if (!(val & 1)) {
			mailbox_wait = 0;
			command ();
		}
		break;
	case 0x08:
		/* The PIO port writes the DIMM's memory: a Net DIMM's buffers */
		if (net) {
			uint32_t at = pio_offset () & (bytes - 1);

			memory[at] = (uint8_t) val;
			if (at + 1 < bytes)
				memory[at + 1] = (uint8_t) (val >> 8);
		}
		Cart::put (reg, val);
		break;
	default:
		Cart::put (reg, val);
		break;
	}
}

void
GdCart::reset ()
{
	Cart::reset ();
	memset (&d, 0, sizeof (d));
	if (net)
		net->close_all ();
	mailbox_wait = 0;
}

int
GdCart::save_state (hl::State *state)
{
	int ret = Cart::save_state (state);

	return ret | state->put (&d, sizeof (d));
}

int
GdCart::load_state (hl::State *state)
{
	int ret = Cart::load_state (state);

	/* What the sockets knew cannot be saved */
	if (net)
		net->close_all ();
	mailbox_wait = 0;
	return ret | state->get (&d, sizeof (d));
}

} /* namespace naomi */
