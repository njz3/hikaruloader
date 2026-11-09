/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-memctl.c, Copyright (C) 2011, 2012, Stefano Teso.
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

#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-memctl.h"
#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-renderer.h"
#include "cpu/sh/sh4.h"

namespace hikaru {

/*
 * Memory Controller
 * =================
 *
 * The Hikaru mainboard is connected to a number of external boards: the
 * ROM board (ROMBD), the primary sound board, an optional sound board, an
 * optional communication board.
 *
 * Access to these devices is performed through an 'external BUS'. The
 * memory controller decides which external device is mapped to what
 * part of the SH-4 address space.
 *
 * There are (very likely) two of these memory controllers, one for each
 * SH-4 CPU. They are likely the two xxx-xxxxx SEGA ICs conveniently placed
 * in the proximity of the two CPUs.
 *
 * Aside from memory device mapping, they also provide a DMA facility to
 * transfer data directly from different devices on the external BUS.
 *
 *
 * Mapping
 * =======
 *
 * Direct access to these extenral devices is possible by mapping them in
 * the SH-4 with the memory controller: in particular, they get mapped within
 * a couple of apertures: one is located at 02000000-03FFFFFF, the other
 * at 16000000-17FFFFFF. Other portions of the SH-4 address space (possibly
 * all of it) may be managed in a similar way, but apparently their emulation
 * is not critical (their mappings don't seem to change.)
 *
 * What is mapped to the apertures is determined by the  MMIOs. See
 * get_bank_for_addr () for the details. Each byte in the MMIOs (+10, +14,
 * and +18 are confirmed) controls a 16 MB aperture. See @0C006688.
 *
 *
 * MMIO Ports
 * ==========
 *
 *  Offset	+3       +2       +1       +0
 *  +0x00	IIIIIIII IIIIIIII IIIIIIII IIIIIIII	Controller ID
 *  +0x04	---u---- -------- ---sEEEE EEFFFFFF	DMA Status
 *  +0x08	-------- -------- -------- --------
 *  +0x0C	-------- -------- -------- --------
 *  +0x10	dddddddd cccccccc bbbbbbbb aaaaaaaa	Aperture Addresses 0
 *  +0x14	hhhhhhhh gggggggg ffffffff eeeeeeee	Aperture Addresses 1
 *  +0x18	llllllll kkkkkkkk jjjjjjjj iiiiiiii	Aperture Addresses 2
 *  +0x1C	pppppppp oooooooo nnnnnnnn mmmmmmmm	Aperture Control 0?
 *  +0x20	tttttttt ssssssss rrrrrrrr qqqqqqqq	Aperture Control 1?
 *  +0x24	xxxxxxxx wwwwwwww vvvvvvvv uuuuuuuu	Aperture Control 2?
 *  +0x28	-------- -------- -------- --------
 *  +0x2C	-------- -------- -------- --------
 *  +0x30	SSSSSSSS SSSSSSSS SSSSSSSS SSS-----	DMA Source Address
 *  +0x34	DDDDDDDD DDDDDDDD DDDDDDDD DDD-----	DMA Destination Address
 *  +0x38	-------C LLLLLLLL LLLLLLLL LLLLLLLL	DMA Control
 *  +0x3C	-------- -------- -------- XXXXXXXX	Unknown
 *
 * Fields	Meaning			Values			References
 * -----------------------------------------------------------------------
 * +0x00	I = ID (0 for master, ~0 for slave)
 * +0x04	u = Unknown
 *		s = DMA status
 *		E = BUS error bits, Master
 *		F = BUS error bits, Slave
 * +0x10	a = Controls 14000080+
 *		b = Controls 15000000?
 *					40 [m]			@0C00BDFC
 *		c = Controls 16xxxxxx	40 [m]			@0C0016A4
 *					02 [m]			@0C00BDFC
 *		d = Controls 17xxxxxx	41 [m]			@0C0016A4
 *					04 [m]			@0C001C70
 *					06 [m]			@0C001C70
 *					03 [m]			@0C00BDFC
 * +0x14	e = Unknown		C0 [m]			@0C0016A4
 *					E6 [s]			@0C00BE70
 *					70 [s] !!!!!!!!!!!
 *		f = Unknown		C1 [m]			@0C0016A4
 *					EE [s]			@0C00BE70
 *		g = Unknown		F2 [m]			@0C0016A4
 *					C2 [m]			@0C00BDFC
 *					F4 [s]			@0C00BE70
 *		h = Unknown		F3 [m]			@0C0016A4
 *					C3 [m]			@0C00BDFC
 *					CC [s]			@0C00BE70
 * +0x18	i = Controls 00xxxxxx ?
 *		j = Controls 01xxxxxx ?
 *		k = Controls 02xxxxxx				@0C0016A4
 *		l = Controls 03xxxxxx				@0C007964,@0C001F3C
 * +0x1C	m = Controls 18xxxxxx[m]			@0C001CC4
 *		    Controls 14xxxxxx[s]	00,01
 *		n = Unknown
 *		o = Unknown			01 [m]		@0C00BDFC
 *						01 [s]		@0C00BE70
 *		p = Unknown
 * +0x20	q = Unknown			FE [m]		@0C00xxxx, @0C00BDFC
 *		r = Unknown			00 [m]
 *		s = Unknown			FE [m]
 *		t = Unknown			00 [m]
 * +0x24	u = Unknown			E6 [m]
 *		v = Unknown			5E [m]
 *						EE [m] MIE	@0C00BDFC
 *		w = Unknown			F4 [m] (E4, B4, F4 while accessing banks D, E in AIRTRIX)
 *						FD [m] SNDBD	@0C001F3C, @oCooBDFC
 *		x = Unknown			CC [m]		@0C00BDFC, @0C007820 => NIBBLES
 * +0x30	S = DMA source address
 * +0x34	D = DMA destination address
 * +0x38	C = DMA control (begin/busy)
 *		L = DMA transfer length in 32-bit words
 *		    See @0C008640
 * +0x3C	X = 0C to access the SNDBD1:027028BC
 *		    A2 to access the SNDBD2:027028BC
 *		    See @0C001748
 *
 * NOTE: other interesting evidence is at PH:@0C0124B8.
 *
 * NOTE: accessing 3C may alter other registers; for instance, the code at
 * @0C001748  saves/restores 04000018 before accessing 0400003C.
 *
 * NOTE: accessing the bus (apertures) may give rise to errors, both during
 * DMA operation and during normal access; these errors get reported in
 * fields E and F.
 *
 * NOTE: 0400001E (field 'o') may be related to cache and/or master-slave
 * syncrhonization. See PH:@0C0D6740 and its only caller.
 *
 * NOTE: direct access to the external BUS is likely prohibited when the DMA
 * is running. See the IRL2 handler, AICA case.
 *
 *
 * DMA Operation
 * =============
 *
 * The DMA is likely used to transfer data from the main RAM to devices on
 * different boards (ROMBD, SNDBD, SNDBD2, NETBD). DMA operation is initiated
 * by setting bit 24 of +38. Upon termination, the MEMCTL raises IRL 1 on
 * the master SH-4, and sets bit 12 of +04 and its corresponding error field.
 *
 * See @0C008640 for more details XXX
 *
 * NOTE: it may be the case that bit 12 and the error field are mutually
 * exclusive.
 *
 *
 * External BUS Address Space
 * ==========================
 *
 * The address space of the external BUS is as follows:
 *
 * 04000000-043FFFFF	Texture RAM Bank 0
 * 06000000-063FFFFF	Texture RAM Bank 1
 * 0A000000-0A00FFFF	Unknown; ROMBD-related
 * 0C000000-0CFFFFFF	Sound Board 1 (its status at 0C000000, the AICA from 0C700000)
 * 0D000000-0DFFFFFF	Sound Board 2 [Optional]
 * 0E000000-0E03FFFF	Communication Board [Optional]; see hikaru-comm.cpp
 * 10000000-3FFFFFFF	ROMBD (EPROM, MASKROM, EEPROM get mapped here)
 * 40000000-41FFFFFF	Slave RAM
 * 48000000-483FFFFF	GPU CMDRAM
 * 4C000000-4C3FFFFF	GPU Unknown
 * 70000000-71FFFFFF	Master RAM
 *
 * NOTE: apparently the external bus is 31 bits wide or less. The MSB is
 * used in mysterious ways. For instance, the code uses the MEMCTL DMA to
 * read ROM data, but accesses the following ranges:
 *
 * 90000000-9FFFFFFF - 80000000 = 10000000-1FFFFFFF	EPROM
 * A0000000-AFFFFFFF - 80000000 = 20000000-2FFFFFFF	MASKROM
 *
 *
 * Rom Board (ROMBD)
 * =================
 *
 * There are two known types of ROM board (which are neatly documented in
 * the MAME hikaru driver.) The EPROM/MASKROM external BUS address can be
 * recovered from information at offset +13C of the EPROM data.
 *
 * This is what this data looks like:
 *
 *	AIRTRIX (Type 1)		ICs	Size
 *	================		===	====
 *	
 *	0003 fee8 c889 97c2 620c	29,30	2 x 4MB
 *	ffff 0000 0000 0000 0000	 		
 *	ffff 0000 0000 0000 0000	 		
 *	ffff 0000 0000 0000 0000	 		
 *	0005 b9a5 9e67 a52a bce0	37,38	2 x 16MB
 *	ffff 0000 0000 0000 0000	 
 *	0005 dabb b621 4bd4 5e6b	41,42	2 x 16MB
 *	ffff 0000 0000 0000 0000	 
 *	0005 0d06 ad63 790f a27e	45,46	2 x 16MB
 *	ffff 0000 0000 0000 0000	 
 *	0005 bdbb 4f01 14a7 6a4e	49,50	2 x 16MB
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	
 *	BRAVEFF (Type ?)		ICs	Size
 *	================		===	====
 *	
 *	0002 0000 0000 0000 0000 	29,30	2 x 2MB
 *	0002 be43 7023 7077 f161	31,32	2 x 2MB
 *	0002 c60d d4f0 b533 8f66	33,34	2 x 2MB
 *	ffff 0000 0000 0000 0000 
 *	0004 8613 2876 3700 2f6d 		2 x 4MB
 *	0004 f545 a454 b97e bb4c 
 *	0004 d6ff 6fe3 df40 f343 
 *	0004 e3b6 2f23 b2b6 61c7 
 *	0004 4792 7015 853b faf1 
 *	0004 c44b 7a18 24dc e336 
 *	0004 e3b0 e492 17bb e589 
 *	0004 cd9f 08f3 3183 cd5c 
 *	0004 8fb6 3fa8 ebbb 9ed9 
 *	0004 2316 c644 66cc b590 
 *	0004 47d0 320d e677 85ad 
 *	0004 d76d cf62 4d9e 8564 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	
 *	PHARRIER			ICs	Size
 *	========			===	====
 *	
 *	0003 0000 0000 0000 0000	29,30	2 x 4MB 
 *	0003 388c 13b5 d289 5910	31,32	2 x 4MB
 *	0003 09a8 578f 73e5 94f7	33,34	2 x 4MB
 *	0003 b1de 6dad 9019 6dd5	35,36	2 x 4MB
 *	0005 7f16 2c37 1f9f aae5	37,38	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 986c 8d7a bd1d 5304	41,42	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 9784 b33d cb75 b08b	45,46	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 5056 b3a9 cbde be85	49,50	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 3d36 05bf d629 8ed6	53,54	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 b9f5 0082 5875 8163	57,58	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 b19d e7cc 158c d180	61,62	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	0005 34bc 677b 5524 349e	65,66	2 x 16MB
 *	ffff 0000 0000 0000 0000	
 *	
 *	PODRACE (Type 1)		ICs	Size
 *	================		===	====	
 *	
 *	0003 0000 0000 0000 0000	29,30	2 x 4MB
 *	0003 5f01 4174 3594 38b3	31,32	2 x 4MB
 *	ffff 0000 0000 0000 0000	
 *	ffff 0000 0000 0000 0000	
 *	0004 7993 8e18 4d44 d239	37,38	2 x 8MB
 *	0004 4135 beab f0c8 04e2	39,40	2 x 8MB
 *	0004 9532 4c1c 925d 02fb	41,42	2 x 8MB
 *	0004 0809 7050 72bc 9311	43,44	2 x 8MB
 *	0004 de84 9d8a 7a5c e7fc	45,46	2 x 8MB
 *	0004 6806 1392 edf1 7bd1	47,48	2 x 8MB
 *	0004 b82d e114 5792 e5e5	49,50	2 x 8MB
 *	0004 3af3 a97c a8cc 721d	51,52	2 x 8MB
 *	0004 ced7 d3cf 6b67 fc76	53,54	2 x 8MB
 *	0004 586c 6954 13a0 db38	55,56	2 x 8MB
 *	0004 4f03 42bf 8ea6 adb6	57,58	2 x 8MB
 *	0004 8645 fc30 3847 ca6b	59,60	2 x 8MB
 *	0004 4140 01c4 ebe6 8085	61,62	2 x 8MB
 *	0004 b68b 7467 4715 4787	63,64	2 x 8MB
 *	0004 3cd6 144a e5d3 ba35	65,66	2 x 8MB
 *	0004 e668 08ed 1fe8 c4a1	67,68	2 x 8MB
 *	
 *	SGNASCAR (Type 2)		ICs	Size		Bus Bank
 *	=================		===	====		========
 *	
 *	0003 0000 0000 0000 0000	35,36	2 x 4MB		10,11
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	0005 0352 d263 49fd 4ad3	19,20	2 x 16MB 	20,21
 *	0005 e717 d635 3637 0e8e	21,22			22,23
 *	0005 4001 8dab c65d bde3	23,24			24,25
 *	0005 615c 293d 7507 1d85	25,26			26,27
 *	0005 90a2 eccc 2b1e 2f9b	27,28			28,29
 *	0005 c98b 3ffb 51e3 701b	29,30			2A,2B
 *	0005 523f 2979 953c 2e5c	31,32			2C,2D
 *	0005 28cf 283f f17b 74fb	33,34			2E,2F
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *	ffff 0000 0000 0000 0000 
 *
 * The mapping from IC numbers to entries has been determined by matching
 * the CRCs in the entries and the CRC values reported in the MAME driver.
 *
 * Each entry (10 bytes) can be defined like this:
 *
 *	struct {
 *		uint16_t log_size;
 *		uint16_t bytesum_lo;
 *		uint16_t wordsum_lo;
 *		uint16_t bytesum_hi;
 *		uint16_t wordsum_hi;
 *	} rom_layout[4+16];
 *
 * and describes the size and CRC for an IC pair: the first four entries
 * describe the EPROMs, the remaining 16 the MASKROMs. If the first word
 * is FFFF, those ICs are not populated.
 *
 * Given the i'th entry, the size and location of an IC (pair) can be
 * computed as follows (see @0C004ED2):
 *
 *	base = samurai_tag_at (0x18000000) ? 8 :
 *	       samurai_tag_at (0x10000000) ? 0 : 0;
 *	
 *	size = 1*MB << rom_layout[i].log_size
 *	
 *	if (i < 4)
 *		///////////////////////////////////////////
 *		// EPROM
 *		// SAMURAI at 10000000 -> base = 0 -> EPROM at 10,11,12,13
 *		// SAMURAI at 18000000 -> base = 8 -> EPROM at 18,19,1A,1B
 *		// each section is 4 or 8 MB (2 ICs)
 *		///////////////////////////////////////////
 *		bank = base + 0x10 + i
 *
 *	else if ((_0C00F01C & C) != 8)
 *		///////////////////////////////////////////
 *		// MASKROM, type 1
 *		// SAMURAI at 10000000 -> base = 0 -> MASKROM at 20,21,22,23,...
 *		// SAMURAI at 18000000 -> base = 8 -> MASKROM at 30,31,32,33,...
 *		// each section is 16 or 32 MB (2 ICs)
 *		///////////////////////////////////////////
 *		bank = base * 2 + 0x1C + i 
 *
 *	else
 *		///////////////////////////////////////////
 *		// MASKROM, type 2
 *		// SAMURAI at 10000000 -> base = 0 -> MASKROM at 20,22,24,26,...
 *		// SAMURAI at 18000000 -> base = 8 -> MASKROM at 30,32,34,36,...
 *		// each section is 16 or 32 MB (2 ICs)
 *		///////////////////////////////////////////
 *		bank = base * 2 + 0x20 + (i - 4) * 2
 *	
 *	bus_addr = bank >> 24
 *	if (size < 8*MB)
 *		bus_addr += 4*MB
 *
 * Here base is determined by the bootrom (see @0C00B834) by looking for
 * the 'SAMURAI' string at both BUS addresses 18000000 and 10000000. If
 * the string is found at bank 18, then the base is 8; it's 0 otherwise.
 *
 * What IC is mapped to what entry is determined by bits 2 and 3 of 0C00F01C.
 * See memctl_get () and @0C004B32. 
 */

/* TODO modify the hikaru->mmap_[ms] instead */
/* TODO raise m/s bus error on bad access */

static int
rombd_get (Hikaru *hikaru, unsigned size, uint32_t bus_addr, void *val)
{
	hikaru_rombd_config_t *config = &hikaru->rombd_config;
	uint32_t bank = bus_addr >> 24;
	uint32_t offs = bus_addr & 0xFFFFFF;

	/* Access here is valid even if performed on the wrong banks: we set
	 * the ptr to garbage here because the hikaru bootrom reads
	 * indiscriminately from banks 10-1B (including the EEPROM bank!) to
	 * figure out the EPROM format. We don't want spurious matches
	 * (that is, 0 vs. 0) to affect the computation. */
	set_ptr (val, size, 0xDEADBEEFDEADBEEFull);

	/* Nothing else to do if there's no actual ROM data */
	if (!config->has_rom)
		return 0;

	if (bank >= config->eprom_bank[0] &&
	    bank <= config->eprom_bank[1]) {
		/* ROMBD EPROM */
		uint32_t num = bank - config->eprom_bank[0]; /* 0 ... 3 */
		uint32_t bank_size = config->eprom_bank_size == 2 ? 4*MB : 8*MB;
		uint32_t bank_mask = bank_size - 1;
		uint32_t real_offs = (offs & bank_mask) + num * bank_size;

		/* A bank the board has not is read too: the games measure their
		 * ICs (SWRACER @0C011D24, the same routine in the others), each
		 * bank 10-1B's first 16 KB against the same bytes 4 MB up, a
		 * match a 4 MB IC. 381 reads a missing bank, an error each before
		 * (2286 in AIRTRIX's log). Its bytes (the garbage above) compare
		 * equal, and the games go on: not logged */
		if (real_offs < hikaru->eprom->size ())
			set_ptr (val, size, hikaru->eprom->get (size, real_offs));
		else if (num * bank_size < hikaru->eprom->size ())
			HL_CPU_ERROR (hikaru->sh_current, "ROMBD out-of-bounds R%u %08X [%08X]",
			              size * 8, bus_addr, real_offs);

	} else if (hikaru->maskrom && bank >= config->maskrom_bank[0] &&
	           bank <= config->maskrom_bank[1]) {
		/* ROMBD MASKROM */
		/* XXX take in account MASKROM stretching here */
		uint32_t num = bank - config->maskrom_bank[0]; /* 0 ... 15 */
		uint32_t bank_size = 16*MB;
		uint32_t bank_mask = bank_size - 1;
		uint32_t real_offs = (offs & bank_mask) + num * bank_size;

		if (real_offs < hikaru->maskrom->size ())
			set_ptr (val, size, hikaru->maskrom->get (size, real_offs));
		else
			HL_CPU_ERROR (hikaru->sh_current, "ROMBD out-of-bounds R%u %08X [num=%X offs=%X bsize=%X bmask=%X roffs=%08X]",
			              size * 8, bus_addr, num, offs, bank_size, bank_mask, real_offs);
	}
	return 0;
}

/*
 * The ROM board's protection
 * ==========================
 *
 * SGNASCAR checks its ROM board as each scene starts (@0C012E98): unless a
 * block it reads from the device at bus 0A000000 is the one it expects
 * (@0C1ADEF0, a text), it raises its motion blur's factor (181) by 20, up to
 * FC, and the picture smears. The device, its registers on bus addresses
 * 0A000000 + (offs & 1FF):
 *
 *   00  Control: bit 31 written starts the stream at the MASKROM's byte
 *       (val & 0FFFFFFF), in the bus's order
 *   04  Status: 00 read back, bit 31 the stream ready
 *   08  4252, the chip's
 *   0C  What was written (the game checks 12345678; the BIOS finds 19620217
 *       there on a stretched MASKROM's board, see below)
 *   10  The stream: 4 bytes a read, the first in bits 0-7
 *
 * The stream, the scheme of Sega's NAOMI "M1" cartridges (MAME's
 * naomi_m1_board): the MASKROM's bytes a b c d, by 4, give d^b, a^c, b, a,
 * each XORed with the key's byte (the key little endian). Its first 111
 * bytes are a dictionary; then a bit stream, its high bits first, of
 * symbols in a prefix code (00 + 2 bits: 0-3; 010 + 2 bits: 4-7; 011 + 3
 * bits: 8-15; 10 + 5 bits: 16-47; 11 + 6 bits: 48-111), each a byte: 0 the
 * next 8 bits, 111 FF, the others the dictionary's. When the dictionary's
 * first byte has bit 6, each word of 4 bytes is a difference: with the
 * last word's two high bytes h0 h1, b0 = h0 - b0, b1 = h1 - b1; then
 * b2 = b0 - b2, b3 = b1 - b3, and b2 b3 for the next word.
 */

static uint8_t
prot_byte (Hikaru *hikaru)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	uint32_t i = prot->state.pos++;
	uint32_t at = prot->state.start + (i & ~3u);
	uint8_t a, b, c, d, x;

	if (!hikaru->maskrom || at + 3 >= hikaru->maskrom->size ())
		return 0;
	a = (uint8_t) hikaru->maskrom->get (1, at);
	b = (uint8_t) hikaru->maskrom->get (1, at + 1);
	c = (uint8_t) hikaru->maskrom->get (1, at + 2);
	d = (uint8_t) hikaru->maskrom->get (1, at + 3);
	switch (i & 3) {
	case 0:  x = d ^ b; break;
	case 1:  x = a ^ c; break;
	case 2:  x = b; break;
	default: x = a; break;
	}
	return x ^ (uint8_t) (prot->key >> (8 * (i & 3)));
}

static unsigned
prot_bits (Hikaru *hikaru, unsigned n)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;

	while (prot->state.nbits < (int32_t) n) {
		prot->state.bits = (prot->state.bits << 8) | prot_byte (hikaru);
		prot->state.nbits += 8;
	}
	prot->state.nbits -= n;
	return (prot->state.bits >> prot->state.nbits) & ((1u << n) - 1);
}

static uint8_t
prot_out_byte (Hikaru *hikaru)
{
	unsigned sym;

	if (!prot_bits (hikaru, 1)) {
		if (!prot_bits (hikaru, 1))
			sym = prot_bits (hikaru, 2);
		else if (prot_bits (hikaru, 1))
			sym = 8 + prot_bits (hikaru, 3);
		else
			sym = 4 + prot_bits (hikaru, 2);
	} else if (prot_bits (hikaru, 1))
		sym = 0x30 + prot_bits (hikaru, 6);
	else
		sym = 0x10 + prot_bits (hikaru, 5);

	if (sym == 0x6F)
		return 0xFF;
	if (sym == 0)
		return (uint8_t) prot_bits (hikaru, 8);
	return hikaru->rombd_prot.state.dict[sym];
}

static uint32_t
prot_word (Hikaru *hikaru)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	uint8_t b[4];
	unsigned i;

	for (i = 0; i < 4; i++)
		b[i] = prot_out_byte (hikaru);
	if (prot->state.dict[0] & 0x40) {
		if (prot->state.has_hist) {
			b[0] = (uint8_t) (prot->state.hist[0] - b[0]);
			b[1] = (uint8_t) (prot->state.hist[1] - b[1]);
		}
		b[2] = (uint8_t) (b[0] - b[2]);
		b[3] = (uint8_t) (b[1] - b[3]);
		prot->state.hist[0] = b[2];
		prot->state.hist[1] = b[3];
		prot->state.has_hist = 1;
	}
	return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
}

static void
prot_start (Hikaru *hikaru, uint32_t start)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	unsigned i;

	prot->state.start = start;
	prot->state.pos = 0;
	prot->state.bits = 0;
	prot->state.nbits = 0;
	prot->state.has_hist = 0;
	for (i = 0; i < sizeof (prot->state.dict); i++)
		prot->state.dict[i] = prot_byte (hikaru);
}

/*
 * With SECURITY_CHIPS, that device answers as the chip, an Actel A54SX32
 * stamped 317-0283-COM on the 834-14125 board (after MAME's naomi_m1_board
 * and Odin's actel_m1.cpp): its registers at 00-13 only (0 elsewhere); 04,
 * the status, 80008000 while a stream runs (bit 15 too, which SGNASCAR's
 * debug function at 0C0342A4 waits for), else 0; 00 starts the stream when
 * its bit 31 rises (the game never gives a start: its bytes from the
 * MASKROM's first, the text of the game), stops it when that bit falls; 10
 * gives as many bytes as its access is wide, the first lowest, the
 * differences taken a byte at a time from the byte two back; symbol 111 ends
 * the stream, zeros after. Not known, and read by no code of the game: the
 * status's other bits, what bits 30, 1 and 0 of the control do (the game
 * keeps them, 0C012F5A). Each start and stop to the log
 */

static uint8_t
chip_byte (Hikaru *hikaru)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	unsigned sym;
	uint8_t byte;

	if (!prot->chip_state.running || prot->chip_state.ended)
		return 0;
	if (!prot_bits (hikaru, 1)) {
		if (!prot_bits (hikaru, 1))
			sym = prot_bits (hikaru, 2);
		else if (prot_bits (hikaru, 1))
			sym = 8 + prot_bits (hikaru, 3);
		else
			sym = 4 + prot_bits (hikaru, 2);
	} else if (prot_bits (hikaru, 1))
		sym = 0x30 + prot_bits (hikaru, 6);
	else
		sym = 0x10 + prot_bits (hikaru, 5);
	if (sym == 0x6F) {
		prot->chip_state.ended = 1;
		return 0;
	}
	byte = sym ? prot->state.dict[sym] : (uint8_t) prot_bits (hikaru, 8);
	if ((prot->state.dict[0] & 0x40) && prot->chip_state.out >= 2)
		byte = (uint8_t) (prot->chip_state.last[prot->chip_state.out & 1] - byte);
	prot->chip_state.last[prot->chip_state.out & 1] = byte;
	prot->chip_state.out++;
	return byte;
}

static uint32_t
chip_get (Hikaru *hikaru, unsigned size, uint32_t offs)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	uint32_t reg = offs & ~3u, val = 0;
	unsigned i;

	if (offs > 0x13)
		return 0;
	switch (reg) {
	case 0x00:
		val = prot->state.ctrl;
		break;
	case 0x04:
		val = prot->chip_state.running ? 0x80008000 : 0;
		break;
	case 0x08:
		val = 0x4252;
		break;
	case 0x0C:
		val = prot->state.regc;
		break;
	case 0x10:
		for (i = 0; i < size && i < 4; i++)
			val |= (uint32_t) chip_byte (hikaru) << (8 * i);
		return val;
	}
	val >>= 8 * (offs & 3);
	return (size >= 4) ? val : (val & ((1u << (8 * size)) - 1));
}

static void
chip_put (Hikaru *hikaru, unsigned size, uint32_t offs, uint32_t val)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;
	uint32_t reg = offs & ~3u, shift = 8 * (offs & 3);
	uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : (((1u << (8 * size)) - 1) << shift);
	uint32_t old;
	static unsigned told = 0;

	if (reg == 0x00) {
		val = (prot->state.ctrl & ~mask) | ((val << shift) & mask);
		old = prot->state.ctrl;
		prot->state.ctrl = val;
		if ((val & 0x80000000) && !(old & 0x80000000)) {
			prot_start (hikaru, 0);
			memset (&prot->chip_state, 0, sizeof (prot->chip_state));
			prot->chip_state.running = 1;
			if (told < 16)
				HL_PRINT ("actel: the stream started (control %08X)%s", val,
				          ++told == 16 ? " (the next ones not told)" : "");
		} else if (!(val & 0x80000000) && prot->chip_state.running) {
			prot->chip_state.running = 0;
			if (told < 16)
				HL_PRINT ("actel: the stream stopped after %u bytes%s", prot->chip_state.out,
				          prot->chip_state.ended ? ", its end reached" : "");
		}
	} else if (reg == 0x0C)
		prot->state.regc = (prot->state.regc & ~mask) | ((val << shift) & mask);
}

static uint32_t
prot_get (Hikaru *hikaru, uint32_t reg)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;

	switch (reg) {
	case 0x00:
	case 0x04:
		return prot->state.ctrl;
	case 0x08:
		return 0x4252;
	case 0x0C:
		return prot->state.regc;
	case 0x10:
		return prot_word (hikaru);
	}
	return 0;
}

static void
prot_put (Hikaru *hikaru, uint32_t reg, uint32_t val)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;

	switch (reg) {
	case 0x00:
		prot->state.ctrl = val;
		if (val & 0x80000000)
			prot_start (hikaru, val & 0x0FFFFFFF);
		break;
	case 0x0C:
		prot->state.regc = val;
		break;
	}
}

/*
 * SWRACER, PHARRIER and AIRTRIX have a 315-5881 there instead (see
 * hikaru-5881.cpp), with 64 KB of RAM at 0A000000 and its registers at
 * 0A010000 (as the NAOMI's "M3" cartridges): the game writes a block to the
 * RAM, then 1000A (the address's high half), 10008 (its low half), 1000C
 * (the subkey), and reads the result at 1000E, a dummy word first; 10000
 * bit 0 is busy. SWRACER checks the chip by its RAM (AAAA, 5555, @0C13FA30)
 * and decrypts with it (@0C13FA68). Only SWRACER's is turned on (see
 * hikaru_set_rombd_config: AIRTRIX's skater loses his body with it).
 */

static uint32_t
crypt_get (Hikaru *hikaru, unsigned size, uint32_t offs)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;

	if (offs < sizeof (prot->ram)) {
		uint32_t val = 0;
		for (unsigned i = 0; i < size && offs + i < sizeof (prot->ram); i++)
			val |= (uint32_t) prot->ram[offs + i] << (8 * i);
		return val;
	}
	switch (offs) {
	case 0x1000E:
		return prot->crypt.read_le ();
	}
	return 0;
}

static void
crypt_put (Hikaru *hikaru, unsigned size, uint32_t offs, uint32_t val)
{
	hikaru_rombd_prot_t *prot = &hikaru->rombd_prot;

	if (offs < sizeof (prot->ram)) {
		for (unsigned i = 0; i < size && offs + i < sizeof (prot->ram); i++)
			prot->ram[offs + i] = (uint8_t) (val >> (8 * i));
		return;
	}
	switch (offs) {
	case 0x10008:
		prot->crypt.set_addr_low ((uint16_t) val);
		if (size == 4)
			prot->crypt.set_addr_high ((uint16_t) (val >> 16));
		break;
	case 0x1000A:
		prot->crypt.set_addr_high ((uint16_t) val);
		break;
	case 0x1000C:
		prot->crypt.set_subkey ((uint16_t) val);
		break;
	}
}

static int
memctl_bus_get (Memctl *memctl, unsigned size, uint32_t bus_addr, void *val)
{
	Hikaru *hikaru = (Hikaru *) memctl->mach;
	uint32_t bank = bus_addr >> 24;
	uint32_t offs = bus_addr & 0xFFFFFF;
	bool log = false;

	set_ptr (val, size, 0);
	if (hikaru->rombd_eeprom && bank == hikaru->rombd_config.eeprom_bank) {
		/* SECURITY_CHIPS: the X76F100, its SDA on bit 0 (MAME's
		 * hikaru.cpp), the whole page alike */
		set_ptr (val, size, hikaru->rombd_eeprom->sda_read () ? 1 : 0);
		return 0;
	}
	if (bus_addr >= 0x04000000 && bus_addr <= 0x043FFFFF) {
		/* TEXRAM Bank 0 */
		set_ptr (val, size, hikaru->texram[0]->get (size, offs));
	} else if (bus_addr >= 0x06000000 && bus_addr <= 0x063FFFFF) {
		/* TEXRAM Bank 1 */
		set_ptr (val, size, hikaru->texram[1]->get (size, offs));
	} else if (bus_addr >= 0x0A000000 && bus_addr <= 0x0AFFFFFF) {
		/* Unknown */
		/* Here's the thing: the value of bits 2 and 3 of 0C00F01C
		 * (which is GBR 28) depends on whether these two ports
		 * retain the value '0x19620217'.
		 * 
		 * If the value of the upper two bits is 4, then the EPROM
		 * start at IC 29; they start at 35 otherwise. See
		 * @0C004BF8.
		 *
		 * If the value is 8, then the MASKROM placement in the bus
		 * address space is non-linear. See @0C004F82 for details.
		 *
		 * These bits are 4 for everything except SGNASCAR, whose
		 * pairs of 16 MB ICs take two banks each, and 8 for it: the
		 * games' ROMBD TEST reads each pair where they say and finds
		 * all ICs GOOD then (SWRACER's 8 MB pairs, linear, had all
		 * but the first BAD when it was 8).
		 *
		 * A board with the protection (SGNASCAR's): its device, whose
		 * 0C keeps what was written (19620217 at first). With a
		 * 315-5881, its RAM and registers, but 08 and 0C as below */
		if (hikaru->rombd_prot.mode == 1 && hikaru->rombd_prot.chip) {
			set_ptr (val, size, chip_get (hikaru, size, offs));
			return 0;
		}
		if (hikaru->rombd_prot.mode == 1) {
			set_ptr (val, size, prot_get (hikaru, offs & 0x1FF));
			return 0;
		}
		if (hikaru->rombd_prot.mode == 2 && offs != 0x8 && offs != 0xC) {
			set_ptr (val, size, crypt_get (hikaru, size, offs));
			return 0;
		}
		switch (offs) {
		case 0x8:
			if (!hikaru->rombd_config.maskrom_is_stretched)
				set_ptr (val, size, 0x19620217);
			break;
		case 0xC:
			if (hikaru->rombd_config.maskrom_is_stretched)
				set_ptr (val, size, 0x19620217);
			break;
		}
		log = true;
	} else if (bus_addr >= 0x0C000000 && bus_addr <= 0x0C6FFFFF) {
		/* Sound Board 1, below its AICA: bit 2 of 0C000000 tells the
		 * extension board (the second AICA) is there, always here;
		 * PHARRIER stops on "ERROR !! NO AICA EXTENSION BOARD."
		 * without it (PH:@0C0B2884) */
		set_ptr (val, size, offs ? 0 : 4);
		log = offs != 0;
	} else if (bus_addr >= 0x0C000000 && bus_addr <= 0x0CFFFFFF) {
		/* AICA 1 */
		return hikaru->aica_m->get (size, bus_addr, val);
	} else if (bus_addr >= 0x0D000000 && bus_addr <= 0x0DFFFFFF) {
		/* AICA 2 */
		return hikaru->aica_s->get (size, bus_addr, val);
	} else if (bus_addr >= 0x0E000000 && bus_addr <= 0x0E03FFFF) {
		/* Communication Board */
		return hikaru->comm->get (size, offs, val);
	} else if (bus_addr >= 0x10000000 && bus_addr <= 0x3FFFFFFF) {
		/* ROMBD */
		return rombd_get (hikaru, size, bus_addr, val);
	} else if (bus_addr >= 0x40000000 && bus_addr <= 0x41FFFFFF) {
		/* Slave RAM */
		set_ptr (val, size, hikaru->ram_s->get (size, bus_addr & 0x01FFFFFF));
	} else if (bus_addr >= 0x70000000 && bus_addr <= 0x71FFFFFF) {
		/* Master RAM */
		set_ptr (val, size, hikaru->ram_m->get (size, bus_addr & 0x01FFFFFF));
	} else if (bank == hikaru->rombd_config.eeprom_bank && offs == 0) {
		/* ROMBD EEPROM */
		set_ptr (val, size, 0xFFFFFFFF);
		log = true;
	} else {
		HL_CPU_ERROR (hikaru->sh_current, "MEMCTL R%u %08X", size * 8, bus_addr);
		return -1;
	}
	if (log)
		HL_CPU_LOG (hikaru->sh_current, "MEMCTL R%u %08X", size * 8, bus_addr);
	return 0;
}

static const struct {
	uint32_t bit_in, bit_out;
} d_to_t[] = {
	{ 0x000001, 0x000001 },
	{ 0x000800, 0x000002 },
	{ 0x000002, 0x000004 },
	{ 0x001000, 0x000008 },
	{ 0x000004, 0x000010 },
	{ 0x002000, 0x000020 },
	{ 0x000008, 0x000040 },
	{ 0x004000, 0x000080 },
	{ 0x000010, 0x000100 },
	{ 0x008000, 0x000200 },
	{ 0x000020, 0x000400 },
	{ 0x010000, 0x000800 },
	{ 0x000040, 0x001000 },
	{ 0x020000, 0x002000 },
	{ 0x000080, 0x004000 },
	{ 0x040000, 0x008000 },
	{ 0x000100, 0x010000 },
	{ 0x080000, 0x020000 },
	{ 0x000200, 0x040000 },
	{ 0x100000, 0x080000 },
	{ 0x000400, 0x100000 },
};

static uint32_t
twiddle_offs (uint32_t offs)
{
	uint32_t toffs = 0, i;
	for (i = 0; i < NUMELEM (d_to_t); i++) {
		if (offs & d_to_t[i].bit_out)
			toffs |= d_to_t[i].bit_in;
	}
	return toffs;
}

static void
texram_put (Hikaru *hikaru, uint32_t bank, uint32_t size, uint32_t offs, uint64_t val)
{
	/* For the drawing thread's copy (THREADED_RENDERING) */
	Renderer *renderer = (Renderer *) hikaru->renderer;

	if (((Gpu *) hikaru->gpu)->is_texram_twiddled ()) {
		hikaru->texram[bank]->put (size, offs, val);
		renderer->texram_written (bank, offs);
	} else {
		uint32_t toffs_lo = twiddle_offs ((offs + 0) >> 1) << 1;
		uint32_t toffs_hi = twiddle_offs ((offs + 2) >> 1) << 1;

		HL_ASSERT (size == 4);

		hikaru->texram[bank]->put (2, toffs_lo, bswap16 (val));
		hikaru->texram[bank]->put (2, toffs_hi, bswap16 (val >> 16));
		renderer->texram_written (bank, toffs_lo);
		renderer->texram_written (bank, toffs_hi);
	}
}

static int
memctl_bus_put (Memctl *memctl, unsigned size, uint32_t bus_addr, uint64_t val)
{
	Hikaru *hikaru = (Hikaru *) memctl->mach;
	uint32_t bank = bus_addr >> 24;
	uint32_t offs = bus_addr & 0xFFFFFF;
	bool log = false;

	if (hikaru->rombd_eeprom && bank == hikaru->rombd_config.eeprom_bank) {
		/* SECURITY_CHIPS: the X76F100's pins, bit 0 SDA, 1 SCL, 2 /CS,
		 * 3 /RST */
		hikaru->rombd_eeprom->set_pins (!(val & 4), !(val & 8), (val >> 1) & 1, val & 1);
		return 0;
	}
	if (bus_addr >= 0x04000000 && bus_addr <= 0x043FFFFF) {
		/* TEXRAM Bank 0 */
		texram_put (hikaru, 0, size, offs, val);
	} else if (bus_addr >= 0x06000000 && bus_addr <= 0x063FFFFF) {
		/* TEXRAM Bank 1 */
		texram_put (hikaru, 1, size, offs, val);
	} else if (bus_addr >= 0x0A000000 && bus_addr <= 0x0AFFFFFF) {
		/* The ROM board's protection, else unknown */
		if (hikaru->rombd_prot.mode == 1 && hikaru->rombd_prot.chip)
			chip_put (hikaru, size, offs, (uint32_t) val);
		else if (hikaru->rombd_prot.mode == 1)
			prot_put (hikaru, offs & 0x1FF, (uint32_t) val);
		else if (hikaru->rombd_prot.mode == 2)
			crypt_put (hikaru, size, offs, (uint32_t) val);
		log = true;
	} else if (bus_addr >= 0x0C000000 && bus_addr <= 0x0CFFFFFF) {
		/* AICA 1 */
		return hikaru->aica_m->put (size, bus_addr, val);
	} else if (bus_addr >= 0x0D000000 && bus_addr <= 0x0DFFFFFF) {
		/* AICA 2 */
		return hikaru->aica_s->put (size, bus_addr, val);
	} else if (bus_addr >= 0x0E000000 && bus_addr <= 0x0E03FFFF) {
		/* Communication Board */
		return hikaru->comm->put (size, offs, val);
	} else if (bus_addr >= 0x40000000 && bus_addr <= 0x41FFFFFF) {
		/* Slave RAM: its code compared again, when there */
		hikaru->ram_s->put (size, bus_addr & 0x01FFFFFF, val);
		((Sh4 *) hikaru->sh_s)->blocks.written (0x0C000000 | (bus_addr & 0x01FFFFFF), size);
	} else if (bus_addr >= 0x48000000 && bus_addr <= 0x483FFFFF) {
		/* GPU CMD RAM */
		hikaru->cmdram->put (size, bus_addr & 0x3FFFFF, val);
	} else if (bus_addr >= 0x70000000 && bus_addr <= 0x71FFFFFF) {
		/* Master RAM: its code compared again, when there */
		hikaru->ram_m->put (size, bus_addr & 0x01FFFFFF, val);
		((Sh4 *) hikaru->sh_m)->blocks.written (0x0C000000 | (bus_addr & 0x01FFFFFF), size);
	} else if (bank == hikaru->rombd_config.eeprom_bank && offs == 0) {
		/* ROMBD EEPROM */
		log = true;
	} else {
		HL_CPU_ERROR (hikaru->sh_current, "MEMCTL W%u %08X = %lX", size * 8, bus_addr, val);
		return -1;
	}
	if (log)
		HL_CPU_LOG (hikaru->sh_current, "MEMCTL W%u %08X = %lX", size * 8, bus_addr, val);
	return 0;
}

static uint32_t
get_bank_for_addr (Memctl *memctl, uint32_t addr)
{
	uint32_t reg;
	switch (addr >> 24) {
	case 0x02:
		reg = 0x1A;
		break;
	case 0x03:
		reg = 0x1B;
		break;
	case 0x16:
		reg = 0x12;
		break;
	case 0x17:
		reg = 0x13;
		break;
	case 0x18:
		reg = 0x1C;
		break;
	default:
		return 0;
	}
	return memctl->regs->get (1, reg);
}

/* Copies up to words of the DMA in progress, its registers moved on (the
 * games read the destination's end back); true when none are left */
static bool
memctl_dma_copy (Memctl *memctl, uint32_t words)
{
	uint32_t src = (uint32_t) memctl->regs->get (4, 0x30);
	uint32_t dst = (uint32_t) memctl->regs->get (4, 0x34);
	uint32_t len = (uint32_t) memctl->regs->get (4, 0x38);
	uint32_t ctl = len >> 24;

	len &= 0xFFFFFF;
	HL_LOG ("MEMCTL DMA: %08X -> %08X x %08X", src, dst, len);
	words = MIN2 (words, len);
	len -= words;
	while (words--) {
		uint32_t tmp;
		memctl_bus_get (memctl, 4, src & 0x7FFFFFFF, &tmp);
		memctl_bus_put (memctl, 4, dst & 0x7FFFFFFF, tmp);
		src += 4;
		dst += 4;
	}
	memctl->regs->put (4, 0x30, src);
	memctl->regs->put (4, 0x34, dst);
	memctl->regs->put (4, 0x38, (ctl << 24) | len);
	return len == 0;
}

/* The apertures read in place
 *
 * The games read their ROMs through the aperture at 02000000, and poll
 * their sound programs' mailboxes in the AICAs' RAM there (SWRACER
 * @0C15158E, 2700 reads a frame), a call of the memory map's each. The
 * master's page table shows a page of an aperture as it is first read when
 * its bank holds plain memory there, as memctl_bus_get () reads it: a ROM's
 * (rombd_get ()), a TEXRAM's, an AICA's RAM, a RAM's; its pages hidden
 * again when the bank changes. The writes go through here all the same
 * (the TEXRAMs' and the RAMs' are watched). */

/* The host's 4 KB that memctl_bus_get () reads at bus_addr (4 KB aligned),
 * NULL for anything else */
static const uint8_t *
bus_page (Hikaru *hikaru, uint32_t bus_addr)
{
	hikaru_rombd_config_t *config = &hikaru->rombd_config;
	uint32_t bank = bus_addr >> 24;
	uint32_t offs = bus_addr & 0xFFFFFF;

	if (hikaru->rombd_eeprom && bank == config->eeprom_bank)
		return NULL;
	if (bus_addr >= 0x04000000 && bus_addr <= 0x043FFFFF)
		return (const uint8_t *) hikaru->texram[0]->ptr (offs);
	if (bus_addr >= 0x06000000 && bus_addr <= 0x063FFFFF)
		return (const uint8_t *) hikaru->texram[1]->ptr (offs);
	/* The AICAs' RAMs (Aica::get ()) */
	if (bus_addr >= 0x0C800000 && bus_addr <= 0x0CFFFFFF)
		return (const uint8_t *) hikaru->aica_ram_m->ptr (bus_addr & 0x7FFFFF);
	if (bus_addr >= 0x0D800000 && bus_addr <= 0x0DFFFFFF)
		return (const uint8_t *) hikaru->aica_ram_s->ptr (bus_addr & 0x7FFFFF);
	if (bus_addr >= 0x10000000 && bus_addr <= 0x3FFFFFFF) {
		hl::Buffer *rom = NULL;
		uint32_t bank_size = 0, num = 0, real_offs;

		if (!config->has_rom)
			return NULL;
		if (bank >= config->eprom_bank[0] && bank <= config->eprom_bank[1]) {
			rom = hikaru->eprom;
			bank_size = config->eprom_bank_size == 2 ? 4*MB : 8*MB;
			num = bank - config->eprom_bank[0];
		} else if (hikaru->maskrom && bank >= config->maskrom_bank[0] && bank <= config->maskrom_bank[1]) {
			rom = hikaru->maskrom;
			bank_size = 16*MB;
			num = bank - config->maskrom_bank[0];
		}
		if (!rom)
			return NULL;
		real_offs = (offs & (bank_size - 1)) + num * bank_size;
		return real_offs + 4096 <= rom->size () ? (const uint8_t *) rom->ptr (real_offs) : NULL;
	}
	if (bus_addr >= 0x40000000 && bus_addr <= 0x41FFFFFF)
		return (const uint8_t *) hikaru->ram_s->ptr (bus_addr & 0x01FFFFFF);
	if (bus_addr >= 0x70000000 && bus_addr <= 0x71FFFFFF)
		return (const uint8_t *) hikaru->ram_m->ptr (bus_addr & 0x01FFFFFF);
	return NULL;
}

/* The aperture of addr: 0-3 (02, 03, 16, 17), -1 none */
static int
aperture_of (uint32_t addr)
{
	switch (addr >> 24) {
	case 0x02: return 0;
	case 0x03: return 1;
	case 0x16: return 2;
	case 0x17: return 3;
	}
	return -1;
}

static void
hide_aperture (Memctl *memctl, int k)
{
	hl::Mmap *mmap = ((Hikaru *) memctl->mach)->mmap_m;

	for (uint32_t page : memctl->shown[k])
		if (mmap)
			mmap->show (page, NULL);
	memctl->shown[k].clear ();
}

void
Memctl::hide_pages ()
{
	for (int k = 0; k < 4; k++)
		hide_aperture (this, k);
}

int
Memctl::get (unsigned size, uint32_t addr, void *val)
{
	hl::Device *dev = this;
	Memctl *memctl = (Memctl *) dev;
	Hikaru *hikaru = (Hikaru *) memctl->mach;
	uint32_t bank, bus_addr;
	int k;

	if (addr >= 0x04000000 && addr <= 0x0400003F) {
		/* MMIOs */
		set_ptr (val, size, memctl->regs->get (size, addr & 0x3F));
		return 0;
	}

	bank = get_bank_for_addr (memctl, addr);
	/* The banks from 80: the bridge's local bus, the others PCI; both
	 * are one here, the MSB dropped (the games' DMAs reach the
	 * communication board as 8E, PHARRIER the sound board as 8C) */
	bank &= 0x7F;
	bus_addr = (bank << 24) | (addr & 0xFFFFFF);

	/* Its page shown from now on, when plain memory (above) */
	k = aperture_of (addr);
	if (memctl->master && k >= 0 && hikaru->mmap_m && hikaru->mmap_m->page_table ()) {
		const uint8_t *host;

		if (bank != memctl->shown_bank[k]) {
			hide_aperture (memctl, k);
			memctl->shown_bank[k] = bank;
		}
		host = bus_page (hikaru, bus_addr & ~0xFFFu);
		if (host) {
			hikaru->mmap_m->show (addr & ~0xFFFu, host);
			memctl->shown[k].push_back (addr & ~0xFFFu);
		}
	}
	return memctl_bus_get (memctl, size, bus_addr, val);
}

int
Memctl::put (unsigned size, uint32_t addr, uint64_t val)
{
	hl::Device *dev = this;
	Memctl *memctl = (Memctl *) dev;
	uint32_t bank, bus_addr;

	if (addr >= 0x04000000 && addr <= 0x0400003F) {
		/* MEMCTL MMIOs */
		switch (addr & 0xFF) {
		case 0x04:
			HL_ASSERT (size == 2);
			{
				uint16_t old;
				old = memctl->regs->get (size, 0x04);
				val = (old & 0xF000) |
				      (old & ~val & 0xFC0) | /* Master BUS/DMA error */
				      (old & ~val & 0x03F); /* Slave BUS/DMA error */
			}
			break;
		case 0x06:
			HL_ASSERT (size == 2);
			break;
		case 0x30:
		case 0x34:
		case 0x38:
			HL_ASSERT (size == 4);
			break;
		}
		memctl->regs->put (size, addr & 0x3F, val);
		/* An aperture's bank changed: its pages hidden */
		for (int k = 0; k < 4; k++) {
			static const uint32_t windows[4] = { 0x02000000, 0x03000000, 0x16000000, 0x17000000 };

			if (memctl->master && !memctl->shown[k].empty () &&
			    (get_bank_for_addr (memctl, windows[k]) & 0x7F) != memctl->shown_bank[k])
				hide_aperture (memctl, k);
		}
		/* A DMA started: its words copied at once, as the real one
		 * gets ahead of the CPU at once. VON4 starts one, then writes
		 * its next list over the source, the same buffer, without
		 * waiting: copied at the end of the CPU's slice, its battle's
		 * lists came out half overwritten, the CP ran into their
		 * floats, and the 3D stayed black. Its end, and its IRQ, still
		 * come with exec () */
		if ((addr & 0x3F) == 0x38 && (val & 0x01000000))
			memctl_dma_copy (memctl, 0xFFFFFF);
		return 0;
	}

	bank = get_bank_for_addr (memctl, addr) & 0x7F;
	bus_addr = (bank << 24) | (addr & 0xFFFFFF);
	return memctl_bus_put (memctl, size, bus_addr, val);
}

int
Memctl::exec (int cycles)
{
	hl::Device *dev = this;
	Memctl *memctl = (Memctl *) dev;

	if (!(memctl->regs->get (4, 0x38) & 0x01000000))
		return 0;

	/* DMA is running; one word per cycle for those not copied at their
	 * start (none, now) */
	if (!memctl_dma_copy (memctl, (uint32_t) MAX2 (cycles, 0)))
		return 0;

	/* Transfer completed */
	memctl->regs->put (4, 0x38, 0);
	/* Set DMA done, clear error flags */
	memctl->regs->put (2, 0x04, 0x1000);
	/* Raise an IRQ */
	((Hikaru *) memctl->mach)->raise_memctl_irq ();
	return 0;
}

void
Memctl::reset (hl::ResetType type)
{
	hl::Device *dev = this;
	Memctl *memctl = (Memctl *) dev;

	memctl->regs->clear ();
	memctl->regs->put (4, 0x00, memctl->master ? 0 : 0xFFFFFFFF);
	memctl->hide_pages ();
}

Memctl::Memctl (hl::Machine *mach, bool master) :
	hl::Device (mach),
	master (master)
{
	regs = hl::Buffer::create (0x40, 0, hl::Buffer::LE32);
	HL_ASSERT (regs);
	mach->register_buffer (regs);
}

} /* namespace hikaru */
