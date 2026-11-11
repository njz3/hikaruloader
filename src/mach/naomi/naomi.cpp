/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/naomi.cpp and dc.cpp (license
 * BSD-3-Clause, copyright-holders Samuele Zannoli, R. Belmont, ElSemi, David
 * Haywood, Angelo Salese, Olivier Galibert, MetalliC), and Flycast's
 * core/hw/naomi/naomi_flashrom.cpp (GPL-2.0-or-later, Copyright 2020
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
#include "hl/machine.h"
#include "hl/paths.h"
#include "hl/state.h"

#include "cpu/sh/sh4.h"
#include "mach/hikaru/hikaru-aica.h"
#include "mach/naomi/naomi.h"

#include <immintrin.h>
#include "hl/outputs.h"
#include "mach/naomi/naomi-renderer.h"

namespace naomi {

/*
 * The Board's Memory
 * ==================
 *
 *  00000000-001FFFFF	The BIOS
 *  00200000-00207FFF	The backup RAM
 *  005F6800-005F9FFF	The Holly's registers
 *  00600000-006007FF	The modem's place: nothing
 *  00700000-0071000F	The AICA's registers, its clock
 *  00800000-00FFFFFF	The AICA's RAM
 *  01000000-01FFFFFF	The G2 bus's outside: floating
 *  04000000-04FFFFFF	The VRAM, 64 bits wide
 *  05000000-05FFFFFF	The same, 32 bits wide: banks interleaved
 *  085F6800-085F9FFF	The registers of both Hollys at once (NAOMI 2)
 *  08800000-088000FF	The Elan's registers (NAOMI 2)
 *  09000000-09FFFFFF	The Elan's command port (NAOMI 2)
 *  0A000000-0BFFFFFF	The Elan's RAM (NAOMI 2)
 *  0C000000-0DFFFFFF	The RAM, mirrored up to 0FFFFFFF
 *  10000000-13FFFFFF	The TA's FIFO, the YUV converter, the direct paths
 *
 * Bit 25 of the first three mirrors them.
 */

#define DEV_FLAGS (HL_REGION_RW | HL_REGION_SIZE_ALL)

/* Forwards to its owner: the regions that are not the Holly's registers */
class Area4 : public hl::Device {
public:
	Area4 (hl::Machine *mach, Holly *holly) : hl::Device (mach), holly (holly) {}
	int get (unsigned size, uint32_t addr, void *val) override
	{
		(void) addr;
		set_ptr (val, size, 0);
		return 0;
	}
	int put (unsigned size, uint32_t addr, uint64_t val) override
	{
		holly->area4_put (size, addr, val);
		return 0;
	}
private:
	Holly *holly;
};

class Vram32 : public hl::Device {
public:
	Vram32 (hl::Machine *mach, hl::Buffer *vram) : hl::Device (mach), vram (vram) {}
	int get (unsigned size, uint32_t addr, void *val) override
	{
		if (size == 8) {
			uint64_t v = vram->get (4, Holly::map32 (addr & 0x00FFFFFC)) |
			             (uint64_t) vram->get (4, Holly::map32 ((addr + 4) & 0x00FFFFFC)) << 32;
			set_ptr (val, 8, v);
		} else
			set_ptr (val, size, vram->get (size, Holly::map32 (addr & 0x00FFFFFF & ~(size - 1))));
		return 0;
	}
	int put (unsigned size, uint32_t addr, uint64_t val) override
	{
		if (size == 8) {
			vram->put (4, Holly::map32 (addr & 0x00FFFFFC), (uint32_t) val);
			vram->put (4, Holly::map32 ((addr + 4) & 0x00FFFFFC), (uint32_t) (val >> 32));
		} else
			vram->put (size, Holly::map32 (addr & 0x00FFFFFF & ~(size - 1)), val);
		return 0;
	}
private:
	hl::Buffer *vram;
};

/* What nothing answers there: reads 0 or the floating bus, writes lost */
class Nothing : public hl::Device {
public:
	Nothing (hl::Machine *mach, uint32_t value) : hl::Device (mach), value (value) {}
	int get (unsigned size, uint32_t addr, void *val) override
	{
		(void) addr;
		set_ptr (val, size, (uint64_t) value | ((uint64_t) value << 32));
		return 0;
	}
	int put (unsigned size, uint32_t addr, uint64_t val) override
	{
		(void) size; (void) addr; (void) val;
		return 0;
	}
private:
	uint32_t value;
};

/* The AICA as the SH-4 sees it, nothing being wired to its MIDI port: the
 * status word at 00702808 says both FIFOs are empty (NAOMI 2 games spin in
 * their timer's interrupt until the input one is) */
class AicaBus : public hl::Device {
public:
	AicaBus (hl::Machine *mach, hl::Device *aica) : hl::Device (mach), aica (aica) {}
	int get (unsigned size, uint32_t addr, void *val) override
	{
		uint64_t data = 0;
		int ret;

		/* The clock takes 32-bit accesses alone; games also read it by halves */
		if (rtc (addr) && size < 4) {
			uint32_t word = 0;

			ret = aica->get (4, addr & ~3u, &word);
			set_ptr (val, size, word >> ((addr & 3) * 8));
			return ret;
		}
		ret = aica->get (size, addr, &data);
		/* Nothing on the MIDI port: both FIFOs empty */
		if (!((hikaru::Aica *) aica)->midi_out && (addr & 0x00FFFFFF) == 0x00702808 && size >= 2)
			data |= 0x0900;
		else if (!((hikaru::Aica *) aica)->midi_out && (addr & 0x00FFFFFF) == 0x00702809)
			data |= 0x09;
		set_ptr (val, size, data);
		return ret;
	}
	int put (unsigned size, uint32_t addr, uint64_t val) override
	{
		return aica->put (rtc (addr) ? 4 : size, rtc (addr) ? addr & ~3u : addr, val);
	}
private:
	static bool rtc (uint32_t addr)
	{
		return (addr & 0x00FFFFFF) >= 0x00710000 && (addr & 0x00FFFFFF) < 0x0071000C;
	}

	hl::Device *aica;
};

/*
 * The SH-4's Port A
 * =================
 *
 * The main board's X76F100, the board's serial number: bit 2 SCL, 3 SDA, 4 RST,
 * 5 CS written; bit 3 SDA read, bit 0 set tells the BIOS it is an X76F100 (not a
 * 93C46). A board comes to its handlers through the CPU's user data.
 */

static Board *
board_of (Sh4 *ctx)
{
	return (Board *) ctx->user;
}

static int
porta_get (Sh4 *ctx, uint16_t *val)
{
	*val = (board_of (ctx)->main_serial.sda_read () ? 8 : 0) | 1;
	return 0;
}

static int
porta_put (Sh4 *ctx, uint16_t val)
{
	Board *board = board_of (ctx);

	board->porta = val;
	board->main_serial.set_pins ((val & 0x20) != 0, (val & 0x10) != 0, (val & 0x04) != 0, (val & 0x08) != 0);
	return 0;
}

/*
 * The Board
 */

Board::Board (Naomi *machine_, unsigned index_, hl::Game *game_) :
	machine (machine_),
	game (game_),
	index (index_)
{
	is_naomi2 = game->naomi2;
	is_atomiswave = !strcmp (game->board, "aw");
	sh = NULL;
	mmap = NULL;
	holly = NULL;
	elan = NULL;
	mie = NULL;
	link = NULL;
	has_cart = true;
	aica = NULL;
	jvs = NULL;
	io_board = NULL;
	bios = sram = ram = vram = aica_ram = elan_ram = NULL;
	porta = 0;
	cycles_per_line = 0;
	mie_t_states = 0;
	frame_lines = 532;
	memset (nvram_old, 0, sizeof (nvram_old));
}

Board::~Board ()
{
}

void
Board::add_buffer (hl::Buffer *&buf, unsigned size, bool battery)
{
	buf = hl::Buffer::create (size, 0, hl::Buffer::LE32);
	if (buf)
		machine->register_buffer (buf, battery);
}

/* Each board's AICA, its own SH-4's line */
static void
aica_irq (void *user, bool raised)
{
	((Board *) user)->holly->set_external (2, raised);
}

hl::Mmap *
Board::setup_mmap ()
{
	hl::Mmap *map = new hl::Mmap (machine);
	Area4 *area4 = new Area4 (machine, holly);
	Vram32 *vram32 = new Vram32 (machine, vram);
	Nothing *zero = new Nothing (machine, 0);
	AicaBus *aica_bus = new AicaBus (machine, aica);
	hl::Device *g2 = link ? (hl::Device *) link : new Nothing (machine, 0xFFFFFFFF);
	if (!link && !strcmp (game->g2_board, "837-14381")) {
		g2_board = new G2Board (machine);
		g2_board->name = "g2_board";
		g2 = g2_board;
	}
	int ret = 0;
	uint32_t mirror;

	if (is_atomiswave) {
		/* The flash: its writes first, its reads the ROM's */
		ret |= map->add_dev (0x00000000, 0x0001FFFF, 0x0001FFFF, HL_REGION_W | HL_REGION_SIZE_ALL, flash, "FLASH");
		ret |= map->add_rom (0x00000000, 0x0001FFFF, 0x0001FFFF, 0, bios, "BIOS");
		ret |= map->add_ram (0x00200000, 0x0021FFFF, 0x0001FFFF, 0, sram, "SRAM");
	} else {
		ret |= map->add_rom (0x00000000, 0x001FFFFF, 0x001FFFFF, 0, bios, "BIOS");
		ret |= map->add_ram (0x00200000, 0x00207FFF, 0x00007FFF, 0, sram, "SRAM");
	}
	for (mirror = 0; mirror <= 0x02000000; mirror += 0x02000000) {
		ret |= map->add_dev (mirror + 0x005F6800, mirror + 0x005F9FFF, 0x00FFFFFF, DEV_FLAGS, holly, "HOLLY");
		ret |= map->add_dev (mirror + 0x00600000, mirror + 0x006007FF, 0x00FFFFFF, DEV_FLAGS, is_atomiswave ? (hl::Device *) modem : zero, "MODEM");
		ret |= map->add_dev (mirror + 0x00700000, mirror + 0x0071000F, 0x00FFFFFF, DEV_FLAGS, aica_bus, "AICA");
		ret |= map->add_ram (mirror + 0x00800000, mirror + 0x00FFFFFF, 0x007FFFFF, 0, aica_ram, "AICA-RAM");
		ret |= map->add_dev (mirror + 0x01000000, mirror + 0x01FFFFFF, 0x00FFFFFF, DEV_FLAGS, g2, "G2");
	}
	/* The Atomiswave has half of it, as a Dreamcast */
	const uint32_t vram_mask = is_atomiswave ? 0x007FFFFF : 0x00FFFFFF;
	ret |= map->add_ram (0x04000000, 0x04FFFFFF, vram_mask, 0, vram, "VRAM64");
	ret |= map->add_dev (0x05000000, 0x05FFFFFF, 0x00FFFFFF, DEV_FLAGS, vram32, "VRAM32");
	ret |= map->add_ram (0x06000000, 0x06FFFFFF, vram_mask, 0, vram, "VRAM64B");
	ret |= map->add_dev (0x07000000, 0x07FFFFFF, 0x00FFFFFF, DEV_FLAGS, vram32, "VRAM32B");
	if (elan) {
		/* What is written here reaches both CLX2, one Holly for us */
		ret |= map->add_dev (0x085F6800, 0x085F69FF, 0x00FFFFFF, DEV_FLAGS, holly, "HOLLY-BOTH");
		ret |= map->add_dev (0x085F8000, 0x085F9FFF, 0x00FFFFFF, DEV_FLAGS, holly, "PVR-BOTH");
		ret |= map->add_dev (0x08800000, 0x088000FF, 0x0FFFFFFF, DEV_FLAGS, elan, "ELAN");
		ret |= map->add_dev (0x09000000, 0x09FFFFFF, 0x0FFFFFFF, DEV_FLAGS, elan, "ELAN-CMD");
		ret |= map->add_ram (0x0A000000, 0x0BFFFFFF, 0x01FFFFFF, 0, elan_ram, "ELAN-RAM");
	}
	ret |= map->add_ram (0x0C000000, 0x0FFFFFFF, ram->size () - 1, 0, ram, "RAM");
	ret |= map->add_dev (0x10000000, 0x13FFFFFF, 0x03FFFFFF, DEV_FLAGS, area4, "TA");
	if (ret) {
		delete map;
		return NULL;
	}
	return map;
}

bool
Board::init ()
{
	hl::Buffer *bios_rom = game->section_data ("bios");
	hl::Buffer *rom_board = game->section_data ("rom_board");
	GdCart::File gd_file {};
	hl::Buffer *gd_pic = game->section_data ("pic");
	hl::Buffer *mie_rom = game->section_data ("mie");
	hl::Buffer *main_rom = game->section_data ("main_x76f100");
	hl::Buffer *mie_default = game->section_data ("main_eeprom");
	hl::Buffer *x76f100 = game->section_data ("x76f100");
	hl::Buffer *pic = game->section_data ("pic_readout");
	unsigned i;

	/* The boards of a machine of several whole systems have each its own */
	has_cart = index == 0 || machine->networked;
	has_comm = !is_atomiswave && (index == 0 || machine->networked);
	if (game->gdrom[0] && !rom_board && has_cart)
		rom_board = GdCart::load (machine, game, gd_pic ? (const uint8_t *) gd_pic->ptr (0) : NULL,
		                          gd_pic ? gd_pic->size () : 0, gd_file);
	rom_image = rom_board;
	/* The BIOS refuses a game not for its region: one the game's header
	 * lists (byte 428; M1 and M4 headers are encrypted) */
	if (rom_board && !is_atomiswave && rom_board->size () > 0x428 &&
	    strcmp (game->board, "m1") && strcmp (game->board, "m4") &&
	    game->use_region ("bios", *(uint8_t *) rom_board->ptr (0x428) & 0x1F)) {
		bios_rom = game->section_data ("bios");
		HL_PRINT ("naomi: the BIOS of the region of the game (%02X)", *(uint8_t *) rom_board->ptr (0x428));
	}
	if (!bios_rom || !rom_board || (!mie_rom && !is_atomiswave)) {
		HL_ERROR ("naomi: %s: its BIOS, its cartridge or the MIE's ROM (mie.zip) is missing", game->name);
		return false;
	}

	add_buffer (bios, 2 * MB, true);
	add_buffer (sram, is_atomiswave ? 128 * KB : 32 * KB, true);
	add_buffer (ram, is_atomiswave ? 16 * MB : 32 * MB);
	add_buffer (vram, 16 * MB);
	add_buffer (aica_ram, 8 * MB);
	if (!bios || !sram || !ram || !vram || !aica_ram)
		return false;
	memcpy (bios->ptr (0), bios_rom->ptr (0), std::min (bios_rom->size (), 2u * MB));

	if (main_rom)
		main_serial.load ((const uint8_t *) main_rom->ptr (0), main_rom->size ());

	Cart::Type type = Cart::TYPE_ROM;
	if (!strcmp (game->board, "m1"))
		type = Cart::TYPE_M1;
	else if (!strcmp (game->board, "m2"))
		type = Cart::TYPE_M2;
	else if (!strcmp (game->board, "m4"))
		type = Cart::TYPE_M4;
	{
		uint32_t key = game->key;
		hl::Buffer *key_rom = game->section_data ("rom_key");

		if (key_rom)
			key = (uint32_t) key_rom->get (4, 0);
		if (is_atomiswave)
			cart.reset (new AwCart (rom_board, (uint8_t) key));
		else if (game->gdrom[0])
			cart.reset (gd = new GdCart (this, rom_board, gd_file));
		else if (has_cart)
			cart.reset (new Cart (rom_board, type, key, game->id, pic ? (const uint8_t *) pic->ptr (0) : NULL));
		else {
			/* A slave's slot is empty: the bus reads all ones */
			hl::Buffer *none;

			add_buffer (none, 16, true);
			if (!none)
				return false;
			memset (none->ptr (0), 0xFF, 16);
			cart.reset (new Cart (none, Cart::TYPE_ROM, 0, 0, NULL));
		}
	}
	/* An M4's header is encrypted on most boards: its regions, as above */
	if (type == Cart::TYPE_M4 && has_cart &&
	    game->use_region ("bios", (memcmp (rom_board->ptr (0), "NAOMI", 5) ? cart->decrypted_byte (0x428) :
	                               *(uint8_t *) rom_board->ptr (0x428)) & 0x1F)) {
		bios_rom = game->section_data ("bios");
		memcpy (bios->ptr (0), bios_rom->ptr (0), std::min (bios_rom->size (), 2u * MB));
		HL_PRINT ("naomi: the BIOS of the region of the game (M4)");
	}
	if (x76f100)
		cart->eeprom.load ((const uint8_t *) x76f100->ptr (0), x76f100->size ());
	/* The BIOS's password, whatever a dump holds (its own passwords not known) */
	memcpy (cart->eeprom.image + hl::X76F100::IMAGE_WRITE_PASSWORD, "iyhktmhm", 8);
	memcpy (cart->eeprom.image + hl::X76F100::IMAGE_READ_PASSWORD, "iyhktmhm", 8);

	if (machine->link)
		link = new MultiboardPort (this, machine->link.get (), index);
	holly = new Holly (this, vram);
	if (is_naomi2) {
		add_buffer (elan_ram, 32 * MB);
		if (!elan_ram)
			return false;
		elan = new Elan (this, elan_ram);
	}
	if (is_atomiswave) {
		flash = new AwFlash (machine, bios);
		modem = new AwModem (this);
		aw_wheel = !strcmp (game->name, "maxspeed") || !strcmp (game->name, "ftspeed");
		if (!strcmp (game->name, "ggisuka"))
			controllers.count = 4;
	} else {
		mie = new Mie (this, mie_rom);
		if (index && !machine->networked)
			memset (mie->eeprom.mem, 0, sizeof (mie->eeprom.mem));
		else if (mie_default && mie_default->size () >= 128)
			for (i = 0; i < 128; i++)
				mie->eeprom.mem[i] = *(uint8_t *) mie_default->ptr (i ^ 1);
		/* The cabinet's place of a whole system, on its switches: the
		 * games read them at the start */
		if (machine->networked)
			mie->switches = (uint8_t) (0x09 | (index == 1 ? 2 : index == 2 ? 4 : 0));
	}
	/* The board's switch SW1-1 as the cabinet's monitor, from the game's
	 * header. The BIOS's table: 1 = 31 kHz, 2 = 15 kHz, others are not checked */
	if (mie)
		mie->monitor_15khz = rom_board->size () > 0x42A && *(uint8_t *) rom_board->ptr (0x42A) == 2;
	jvs = new hl::JvsBus (machine);
	/* F355 Challenge's cabinets have the drive board's I/O board, the 837-13844 */
	if (!strncmp (game->name, "f355", 4) || !strncmp (game->name, "18wheel", 7)) {
		if (!index)
			drive_board = new hl::DriveBoard (machine, new hl::DriveBoardF355 (machine, game->name[0] == '1'));
		hl::SegaIoBoard13844 *board13844 = new hl::SegaIoBoard13844 (machine, jvs, drive_board);

		board13844->three_position_lever = game->name[0] == '1';
		io_board = board13844;
	}
	/* The other boards of a machine of whole systems have no I/O board */
	else if (!index || !machine->networked) {
		io_board = new hl::SegaIoBoard (machine, jvs);
		((hl::SegaIoBoard *) io_board)->analog_inputs = game->analogs;
		/* The cabinet's other boards, each further along the chain (the
		 * far end gets the first address); the controls stay on the first */
		for (i = 1; i < game->io_boards.size (); i++) {
			if (game->io_boards[i] == "837-13938")
				new hl::SegaIoBoard13938 (machine, jvs);
			else if (game->io_boards[i] == "837-13551")
				(new hl::SegaIoBoard (machine, jvs))->cabinet = false;
			else
				HL_ERROR ("naomi: unknown I/O board %s", game->io_boards[i].c_str ());
		}
	}

	hikaru::Aica *sound = new hikaru::Aica (machine, aica_ram, true);
	sound->main_irq_hook = aica_irq;
	sound->main_irq_user = this;
	aica = sound;

	mmap = setup_mmap ();
	if (!mmap)
		return false;

	Sh4 *cpu = new Sh4 (machine, mmap, true, true);
	sh = cpu;
	cpu->user = this;
	cpu->sq_buffered = true;
	cpu->rfcr_counts = true;
	cpu->mmu_trusted = false;
	cpu->blocks.own (0x0C000000, 0x0DFFFFFF);
	cpu->blocks.own (0x00000000, 0x001FFFFF);
	sh4_set_porta_handlers (cpu, porta_get, porta_put);

	/* The Lua shell's names */
	sh->name = index == 0 ? "master" : index == 1 ? "slave1" : "slave2";
	comm.name = index == 0 ? "comm" : index == 1 ? "comm1" : "comm2";
	if (index == 0) {
		holly->name = "holly";
		if (mie)
			mie->name = "mie";
		aica->name = "aica";
		jvs->name = "jvs";
		machine->add_memory ("ram", ram);
		machine->add_memory ("vram", vram);
		machine->add_memory ("sram", sram);
		machine->add_memory ("aica_ram", aica_ram);
		machine->add_memory ("iregs", cpu->iregs);
	}
	return true;
}

/*
 * The MIE's EEPROM
 * ================
 *
 *  0	 A CRC of 2-17, then 16 bytes of the BIOS's settings
 *  18	 The same again
 *  36	 The games' record: a CRC, its size, its padded size, then that again, then
 *	 its bytes, twice over
 *
 * The settings: 2 bit 0 a vertical screen, bit 4 the attract's sound; 3-6 the
 * game's id (the header's); 8 bit 0 the coin chutes, bits 4-5 the players - 1;
 * 9-17 the coins. The BIOS stops on "GAME ASSIGNMENTS ARE INCORRECT" when they
 * do not agree with the header, as a cabinet's owner would have set them.
 */

static uint16_t
eeprom_crc (const uint8_t *buf, int size)
{
	uint32_t n = 0xDEBDEB00;
	int i, c;

	for (i = 0; i <= size; i++) {
		if (i < size) {
			n &= 0xFFFFFF00;
			n += buf[i];
		}
		for (c = 0; c < 8; c++)
			n = (n & 0x80000000) ? (n << 1) + 0x10210000 : n << 1;
	}
	return (uint16_t) (n >> 16);
}

/* A byte of the first record, in both copies, their CRCs again */
static void
eeprom_put (uint8_t *mem, unsigned offs, uint8_t val)
{
	uint16_t crc;

	mem[offs] = val;
	mem[offs + 18] = val;
	crc = eeprom_crc (mem + 2, 16);
	memcpy (mem, &crc, 2);
	memcpy (mem + 18, &crc, 2);
}

/*
 * The record of the games' own settings, in the EEPROM: at 36 its CRC and its
 * size, at 40 the same, at 44 its bytes, twice. In the backup memory: at 200 its
 * size, at 218 its bytes, then the same again, its CRC at 1F8 and 208.
 */
uint8_t
Board::eeprom_read (unsigned offs) const
{
	return mie ? mie->eeprom.mem[offs & 127] : 0;
}

bool
Board::eeprom_write (unsigned offs, uint8_t val)
{
	uint8_t *mem;
	unsigned padded;
	uint16_t crc;

	if (!mie)
		return false;
	mem = mie->eeprom.mem;
	if (offs >= 2 && offs < 18) {
		eeprom_put (mem, offs, val);
		return true;
	}
	padded = mem[39];
	/* No record: 0, or the bytes of a blank EEPROM */
	if (offs < 44 || offs - 44 >= padded || padded >= 0x7F || offs + padded >= sizeof (mie->eeprom.mem))
		return false;
	mem[offs] = val;
	mem[offs + padded] = val;
	crc = eeprom_crc (mem + 44, (int) padded);
	memcpy (mem + 36, &crc, 2);
	memcpy (mem + 40, &crc, 2);
	return true;
}

bool
Board::flash_write (unsigned offs, uint8_t val)
{
	uint8_t *mem = (uint8_t *) sram->ptr (0);
	uint32_t block = 0;
	uint16_t crc;

	memcpy (&block, mem + 0x200, 4);
	if (offs < 0x218 || block == 0xFFFFFFFF || offs >= 0x218 + block || 0x218 + block * 2 > sram->size ())
		return false;
	mem[offs] = val;
	mem[offs + block] = val;
	crc = eeprom_crc (mem + 0x218, (int) block);
	memcpy (mem + 0x1F8, &crc, 2);
	memcpy (mem + 0x208, &crc, 2);
	return true;
}

void
Board::configure_eeprom ()
{
	if (!mie)
		return;
	uint8_t *mem = mie->eeprom.mem;
	const uint8_t *h = (const uint8_t *) rom_image->ptr (0);
	uint8_t decrypted[0x430];
	unsigned i;

	/* An M4's header, encrypted on most of the boards */
	if (!strcmp (game->board, "m4") && cart && memcmp (h, "NAOMI", 5) && memcmp (h, "Naomi2", 6)) {
		for (i = 0; i < sizeof (decrypted); i++)
			decrypted[i] = cart->decrypted_byte (i);
		h = decrypted;
	}
	const uint8_t *coin = h + 0x1E0;
	uint8_t cabinet = h[0x429], vertical = h[0x42B];

	if (memcmp (h, "NAOMI", 5) && memcmp (h, "Naomi2", 6))
		return;
	if (memcmp (h + 0x134, mem + 3, 4)) {
		for (i = 0; i < 4; i++)
			eeprom_put (mem, 3 + i, h[0x134 + i]);
		eeprom_put (mem, 7, 9);
		eeprom_put (mem, 8, (cabinet & 8) ? 0x30 : (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0);
		if (coin[0] == 1) {
			eeprom_put (mem, 2, (coin[1] & 1) | 0x10);
			if (coin[2] == 1)
				eeprom_put (mem, 8, mem[8] | 1);
			eeprom_put (mem, 9, coin[3] - 1);
			eeprom_put (mem, 10, std::max (coin[6], (uint8_t) 1));
			eeprom_put (mem, 11, std::max (coin[4], (uint8_t) 1));
			eeprom_put (mem, 12, std::max (coin[5], (uint8_t) 1));
			eeprom_put (mem, 13, coin[7]);
			for (i = 0; i < 4; i++)
				eeprom_put (mem, 14 + i, coin[8 + i * 2] | (coin[9 + i * 2] << 4));
		} else {
			eeprom_put (mem, 2, (vertical & 2) ? 0x11 : 0x10);
			eeprom_put (mem, 9, 0);
			for (i = 10; i < 13; i++)
				eeprom_put (mem, i, 1);
			eeprom_put (mem, 13, 0);
			for (i = 14; i < 18; i++)
				eeprom_put (mem, i, 0x11);
		}
	}
	if (vertical == 2)
		eeprom_put (mem, 2, mem[2] | 1);
	else if (vertical == 1)
		eeprom_put (mem, 2, mem[2] & ~1);
	if (cabinet != 0 && cabinet < 0x10 && !(cabinet & (1 << (mem[8] >> 4)))) {
		uint8_t chute = mem[8] & 1;

		eeprom_put (mem, 8, ((cabinet & 8) ? 0x30 : (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0) | chute);
	}
}

void
Board::software_reset ()
{
	holly->reset_requested = false;
	if (servo)
		servo->reset ();
	if (hopper)
		hopper->reset ();
		/* As at power-on: the BIOS would restart again on a manual reset's EXPEVT */
	sh->reset (hl::RESET_HARD);
	holly->reset (hl::RESET_SOFT);
	aica->reset (hl::RESET_SOFT);
	if (mie)
		mie->reset (hl::RESET_SOFT);
	((Sh4 *) sh)->blocks.new_epoch ();
}

void
Board::reset (hl::ResetType type)
{
	(void) type;
	cart->reset ();
	comm.reset ();
	porta = 0;
	main_serial.reset ();
}

/* The wheels with the servo board 838-14174 on the MIDI port: by the title in
 * the cartridge's header; Driving Simulator's is encrypted */
void
Board::setup_servo ()
{
	if (is_atomiswave)
		return;
	const char *title = (const char *) rom_image->ptr (0x30);

	if (machine->networked) {
		/* Its dashboard, on the SCIF (nothing answers) */
		meters.reset (new DriveSimMeters ());
		((Sh4 *) sh)->scif_pipe = meters.get ();
	}
	if (strncmp (title, "CLUB KART IN JAPAN", 18) && strncmp (title, "THE KING OF ROUTE66", 19) &&
	    strncmp (title, "INITIAL D", 9) &&
	    strcmp (game->name, "sgdrvsim"))
		return;
	servo.reset (new ServoBoard (this));
	servo->attach ();
	HL_PRINT ("naomi: a servo board 838-14174 on the MIDI port");
	/* Initial D's card reader, on the SH-4's SCIF */
	if (!strncmp (title, "INITIAL D", 9)) {
		std::string file = std::string ("nvram/") + game->name + ".card";

		sanwa.reset (new SanwaCard (machine, hl::Paths::game_write (game->name, file)));
		((Sh4 *) sh)->scif_pipe = sanwa.get ();
	}
	/* The first Club Kart's card reader */
	if (!strncmp (game->name, "clubkrt", 7) || !strcmp (game->name, "clubk2k3") ||
	    !strncmp (game->name, "clubkcyc", 8)) {
		card.reset (new ClubKartCard (game->name));
		((Sh4 *) sh)->scif_pipe = card.get ();
	}
}

/* The medal games: their hopper board on the SH-4's SCIF */
void
Board::setup_hopper ()
{
	if (strncmp (game->name, "clubkp", 6))
		return;
	hopper.reset (new SwpHopper ());
	((Sh4 *) sh)->scif_pipe = hopper.get ();
	HL_PRINT ("naomi: a hopper board on the SCIF");
}

unsigned
Board::begin_frame ()
{
	frame_lines = holly->lines ();
	cycles_per_line = (unsigned) ((uint64_t) hl::Config::naomi_sh4_clock_mhz () * MHZ / (60 * frame_lines));
	mie_t_states = (int) (16000000 / (60 * frame_lines));
	uint32_t hz = (uint32_t) ((uint64_t) cycles_per_line * frame_lines * 60);
	((hikaru::Aica *) aica)->cpu_hz = hz;
	((Sh4 *) sh)->cpu_hz = hz;
	return frame_lines;
}

void
Board::run_lines (unsigned line, unsigned count)
{
	unsigned n;

	for (n = line; n < line + count && n < frame_lines; n++) {
		/* About a millisecond */
		if (servo && ++servo_lines >= frame_lines * 60 / 1000) {
			servo_lines = 0;
			servo->tick ();
		}
		if (card || sanwa || hopper)
			((Sh4 *) sh)->scif_update (cycles_per_line);
		sh->run (cycles_per_line);
		aica->exec (cycles_per_line);
		if (mie)
			mie->exec (mie_t_states);
		holly->line (n);
		/* The sockets of a Net DIMM, every few lines */
		if (gd && !(n & 15))
			gd->poll (16);
		if (holly->reset_requested)
			software_reset ();
		if (elan)
			elan->line ();
	}
}

void
Board::end_frame (std::vector<int32_t> *mix)
{
	std::vector<int32_t> &in = ((hikaru::Aica *) aica)->out;

	if (card)
		card->frame ();
	if (hopper)
		hopper->frame ();
	if (gd)
		gd->frame ();
	if (sanwa)
		sanwa->reader->frame ();
	if (link_settings_due && !(machine->frame % 60))
		link_settings ();
	if (link_watch && !(machine->frame % 60)) {
		link_watch--;
		watch_link_settings ();
	}
	/* Each board's amplifier output reaches the cabinet's speakers: the
	 * sounds of all the boards heard together */
	if (mix) {
		if (mix->size () < in.size ())
			mix->resize (in.size (), 0);
		for (size_t i = 0; i < in.size (); i++)
			(*mix)[i] += in[i];
	}
	in.clear ();
}

const char *
Board::debug_string ()
{
	snprintf (debug_text, sizeof (debug_text), "[%s] [%s]", sh->debug_string (), holly->debug_string ());
	return debug_text;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Board::save_state (hl::State *state)
{
	int ret = 0;

	ret |= main_serial.save_state (state);
	if (modem) {
		ret |= modem->save_state (state);
		ret |= flash->save_state (state);
	}
	if (servo)
		ret |= servo->save_state (state);
	if (hopper)
		ret |= hopper->save_state (state);
	SAVE (porta);
	ret |= cart->save_state (state);
	ret |= comm.save_state (state);
	return ret;
}

int
Board::load_state (hl::State *state)
{
	int ret = 0;

	ret |= main_serial.load_state (state);
	if (modem) {
		ret |= modem->load_state (state);
		ret |= flash->load_state (state);
	}
	if (servo)
		ret |= servo->load_state (state);
	if (hopper)
		ret |= hopper->load_state (state);
	LOAD (porta);
	ret |= cart->load_state (state);
	ret |= comm.load_state (state);
	return ret;
}

#undef SAVE
#undef LOAD

/*
 * The Non-volatile Memories
 */

static const char *const NV_TYPES[4] = { "EEPR", "MIEE", "BRAM", "X76F" };

static void
block_name (char out[5], const char *base, unsigned board)
{
	memcpy (out, base, 4);
	out[4] = 0;
	/* The others': the last letter is their number */
	if (board)
		out[3] = (char) ('0' + board);
}

void
Board::nvram_read (const hl::Nvram &nvram)
{
	char name[5];
	const std::vector<uint8_t> *d;

	block_name (name, NV_TYPES[0], index);
	if ((d = nvram.get (name)))
		main_serial.load (d->data (), d->size ());
	block_name (name, NV_TYPES[1], index);
	if (mie && (d = nvram.get (name)) && d->size () == sizeof (mie->eeprom.mem))
		memcpy (mie->eeprom.mem, d->data (), d->size ());
	/* The Atomiswave's BIOS: its settings in its flash */
	if (flash && (d = nvram.get ("FLSH")) && d->size () == 0x20000)
		memcpy (bios->ptr (0), d->data (), d->size ());
	block_name (name, NV_TYPES[2], index);
	if ((d = nvram.get (name)) && d->size () == sram->size ())
		memcpy (sram->ptr (0), d->data (), d->size ());
	block_name (name, NV_TYPES[3], index);
	if ((d = nvram.get (name)))
		cart->eeprom.load (d->data (), d->size ());
	if (hopper && (d = nvram.get ("HOPR")))
		hopper->load (d->data (), d->size ());
	if (g2_board && (d = nvram.get ("G2RA")))
		memcpy (g2_board->ram, d->data (), std::min (d->size (), sizeof (g2_board->ram)));
}

/* A new cabinet's backup memory where the game needs one: F355 Twin's says it
 * is not linked to another (data/naomi, from Flycast's resources/flash); a game
 * without its record starts looking for the cabinet it would be linked to.
 * The same for the MIE's EEPROM, the settings a game's operator would have made */
void
Board::default_backup ()
{
	const struct {
		const char *ext;
		void *dst;
		size_t size;
	} defaults[2] = {
		{ ".sram", sram->ptr (0), sram->size () },
		{ ".eeprom", mie ? mie->eeprom.mem : NULL, mie ? sizeof (mie->eeprom.mem) : 0 }
	};

	for (auto &d : defaults) {
		const std::string name = std::string ("data/naomi/") + game->name + d.ext;

		if (!d.dst)
			continue;
		for (const std::string &path : { hl::Paths::config_read (name.c_str ()), "./" + name }) {
			FILE *fp = fopen (path.c_str (), "rb");

			if (!fp)
				continue;
			size_t got = fread (d.dst, 1, d.size, fp);

			fclose (fp);
			HL_PRINT ("naomi: the default '%s' from '%s' (%u bytes)", d.ext, path.c_str (), (unsigned) got);
			break;
		}
	}
}

/* A new EEPROM's settings where the game needs them: the record of a machine of
 * whole systems (data/naomi, from Flycast), each board's */
void
Board::default_eeprom ()
{
	const std::string name = std::string ("data/naomi/") + game->name + ".eeprom";

	if (!mie)
		return;
	for (const std::string &path : { hl::Paths::config_read (name.c_str ()), "./" + name }) {
		FILE *fp = fopen (path.c_str (), "rb");

		if (!fp)
			continue;
		size_t got = fread (mie->eeprom.mem, 1, sizeof (mie->eeprom.mem), fp);

		fclose (fp);
		HL_PRINT ("naomi: board %u's EEPROM from '%s' (%u bytes)", index, path.c_str (), (unsigned) got);
		return;
	}
}

void
Board::nvram_write (hl::Nvram &nvram)
{
	char name[5];

	block_name (name, NV_TYPES[0], index);
	nvram.set (name, main_serial.image, hl::X76F100::IMAGE_SIZE);
	block_name (name, NV_TYPES[1], index);
	if (mie)
		nvram.set (name, mie->eeprom.mem, sizeof (mie->eeprom.mem));
	if (flash)
		nvram.set ("FLSH", bios->ptr (0), 0x20000);
	block_name (name, NV_TYPES[2], index);
	nvram.set (name, sram->ptr (0), sram->size ());
	block_name (name, NV_TYPES[3], index);
	nvram.set (name, cart->eeprom.image, hl::X76F100::IMAGE_SIZE);
	if (hopper)
		nvram.set ("HOPR", hopper->settings (), hopper->settings_size ());
	if (g2_board)
		nvram.set ("G2RA", g2_board->ram, sizeof (g2_board->ram));
}

bool
Board::nvram_changed ()
{
	uint8_t now[sizeof (nvram_old)];

	memcpy (now, main_serial.image, 128);
	if (mie)
		memcpy (now + 128, mie->eeprom.mem, 128);
	else
		memset (now + 128, 0, 128);
	memcpy (now + 256, cart->eeprom.image, 132);
	if (flash && flash->changed) {
		flash->changed = false;
		return true;
	}
	if (hopper && hopper->changed ())
		return true;
	if (g2_board && g2_board->changed) {
		g2_board->changed = false;
		return true;
	}
	if (!memcmp (now, nvram_old, sizeof (now)))
		return false;
	memcpy (nvram_old, now, sizeof (now));
	return true;
}

/*
 * The Machine
 */

Naomi::Naomi (hl::Game *game) :
	hl::Machine (game)
{
	strcpy (name, "NAOMI");
}

Naomi::~Naomi ()
{
	stop_threads ();
	if (nvram_due)
		save_nvram ();
}

void
Naomi::load_nvram ()
{
	std::string file = std::string ("nvram/") + game->name + ".nvram";
	std::string read;

	nvram_path = hl::Paths::game_write (game->name, file);
	if (hl::Paths::has_write_dir ())
		hl::Paths::seed (file, nvram_path);
	read = hl::Paths::game_read (game->name, file);
	switch (nvram.load (read.c_str ())) {
	case hl::Nvram::LOADED:
		master ()->nvram_read (nvram);
		HL_PRINT ("naomi: the game's settings from '%s'", read.c_str ());
		break;
	case hl::Nvram::MISSING:
		master ()->link_watch = 120;
		master ()->default_backup ();
		master ()->default_eeprom ();
		break;
	default:
		HL_ERROR ("naomi: '%s' is no nvram file of this version: kept, and nothing saved", read.c_str ());
		nvram_path.clear ();
		break;
	}
	/* The other boards of a machine of whole systems keep nothing */
	for (size_t i = 1; networked && i < boards.size (); i++)
		boards[i]->default_eeprom ();
	for (auto &b : boards)
		b->nvram_changed ();
}

void
Naomi::save_nvram ()
{
	if (nvram_path.empty ())
		return;
	/* The slaves keep nothing */
	master ()->nvram_write (nvram);
	if (!nvram.save (nvram_path.c_str ()))
		HL_ERROR ("naomi: cannot save '%s'", nvram_path.c_str ());
}

void
Naomi::keep_nvram ()
{
	bool changed = false;

	for (auto &b : boards)
		changed |= b->nvram_changed ();
	if (changed)
		nvram_due = 120;
	else if (nvram_due && !--nvram_due)
		save_nvram ();
}

/* About a millisecond of waiting, with the pauses' length on today's CPUs */
static const unsigned WORKER_SPINS = 20000, MASTER_SPINS = 20000;

static inline void
spin_pause ()
{
	_mm_pause ();
}

void
Naomi::run_slice (Board *board)
{
	board->run_lines (slice_line, slice_count);
}

void
Naomi::worker (unsigned index)
{
	uint64_t seen = 0;

	for (;;) {
		unsigned spins = 0;

		while (!quitting && slice_id == seen) {
			if (++spins < WORKER_SPINS)
				spin_pause ();
			else {
				std::unique_lock<std::mutex> hold (lock);

				sleeping++;
				go.wait (hold, [&] { return quitting || slice_id != seen; });
				sleeping--;
				spins = 0;
			}
		}
		if (quitting)
			return;
		seen = slice_id;
		run_slice (boards[index].get ());
		running--;
	}
}

void
Naomi::start_threads ()
{
	unsigned i;

	for (i = 1; i < boards.size (); i++)
		threads.emplace_back (&Naomi::worker, this, i);
}

void
Naomi::stop_threads ()
{
	{
		std::lock_guard<std::mutex> hold (lock);
		quitting = true;
	}
	go.notify_all ();
	for (std::thread &t : threads)
		t.join ();
	threads.clear ();
}

/* The boards with a monitor, left to right: SCREEN_ORDER, else the game's
 * ("screens" in games.json), else the master between its two slaves */
void
Naomi::setup_screens ()
{
	hl::Config *cfg = hl::Config::get ();
	unsigned count = (unsigned) boards.size (), i;
	std::vector<unsigned> wanted;
	const char *p = cfg->screen_order;

	while (*p) {
		if (*p >= '0' && *p <= '9') {
			wanted.push_back ((unsigned) strtoul (p, (char **) &p, 10));
			continue;
		}
		p++;
	}
	if (wanted.empty ())
		for (i = 0; i < game->num_screens; i++)
			wanted.push_back (game->screens[i]);
	for (unsigned b : wanted)
		if (b >= 1 && b <= count &&
		    std::find (screen_order.begin (), screen_order.end (), b - 1) == screen_order.end ())
			screen_order.push_back (b - 1);
	if (screen_order.empty ()) {
		if (count == 3)
			screen_order = { 1, 0, 2 };
		else
			for (i = 0; i < count; i++)
				screen_order.push_back (i);
	}
	multi_screen = count > 1 && screen_order.size () > 1 ? cfg->multi_screen : (int) hl::MULTI_ONE;
	/* The master, or the middle screen when it has none */
	screen = std::find (screen_order.begin (), screen_order.end (), 0u) != screen_order.end () ?
	         0 : screen_order[screen_order.size () / 2];
	if (count > 1)
		HL_PRINT ("naomi: %u boards, %u with a monitor, MULTI_SCREEN %d", count,
		          (unsigned) screen_order.size (), multi_screen);
}

std::vector<unsigned>
Naomi::shown_boards () const
{
	std::vector<unsigned> shown, unseen = boards_unseen ();

	if (multi_screen == hl::MULTI_ONE)
		shown.push_back (screen);
	else if (unseen.empty ())
		for (unsigned b : screen_order)
			shown.push_back ((b + screen_shift) % (unsigned) boards.size ());
	else {
		shown = screen_order;
		if (screen_shift)
			shown[shown.size () / 2] = unseen[screen_shift - 1];
	}
	return shown;
}

std::vector<unsigned>
Naomi::boards_unseen () const
{
	std::vector<unsigned> unseen;
	unsigned i;

	for (i = 0; i < boards.size (); i++)
		if (std::find (screen_order.begin (), screen_order.end (), i) == screen_order.end ())
			unseen.push_back (i);
	return unseen;
}

/* The key. One board at a time: the next one, whether it has a monitor or not.
 * Several screens: the middle one and the boards without a monitor in turn */
void
Naomi::next_screen ()
{
	if (boards.size () < 2)
		return;
	if (multi_screen == hl::MULTI_ONE) {
		screen = (screen + 1) % (unsigned) boards.size ();
		screen_chosen = true;
		screen_idle = 0;
		HL_PRINT ("naomi: the picture of board %u", screen + 1);
	} else {
		std::vector<unsigned> unseen = boards_unseen ();

		screen_shift = (screen_shift + 1) % (unsigned) (unseen.empty () ? boards.size () : unseen.size () + 1);
		HL_PRINT ("naomi: the screens' boards: F8 step %u", screen_shift);
	}
	((Renderer *) renderer)->update_titles ();
}

/* One board at a time: the first with a monitor, unless it draws nothing while
 * another does; the drawings of the boards not shown are dropped */
void
Naomi::choose_screen ()
{
	std::vector<unsigned> shown;
	unsigned i;

	if (boards.size () < 2)
		return;
	if (multi_screen == hl::MULTI_ONE && !screen_chosen && screen_order.size () > 1) {
		if (boards[screen]->holly->pending_draws ())
			screen_idle = 0;
		else if (++screen_idle > 30) {
			/* Those with a monitor only: another may well draw (F355
			 * Challenge Deluxe's master, black frames); F8 still shows it */
			for (unsigned b : screen_order) {
				if (boards[b]->holly->pending_draws ()) {
					HL_PRINT ("naomi: the picture from board %u", b);
					screen = b;
					screen_idle = 0;
					((Renderer *) renderer)->update_titles ();
					break;
				}
			}
		}
	}
	shown = shown_boards ();
	for (i = 0; i < boards.size (); i++)
		if (std::find (shown.begin (), shown.end (), i) == shown.end ())
			boards[i]->holly->take_draws ();
}

int
Naomi::run_frame ()
{
	unsigned lines = master ()->begin_frame (), line, i;

	for (i = 1; i < boards.size (); i++)
		boards[i]->begin_frame ();

	for (line = 0; line < lines; line += SLICE_LINES) {
		slice_line = line;
		slice_count = SLICE_LINES;
		if (boards.size () > 1) {
			unsigned spins = 0;

			running = (unsigned) boards.size () - 1;
			slice_id++;
			if (sleeping) {
				std::lock_guard<std::mutex> hold (lock);

				go.notify_all ();
			}
			run_slice (master ());
			/* A PC with few cores: the others need this one */
			while (running)
				if (++spins < MASTER_SPINS)
					spin_pause ();
				else
					std::this_thread::yield ();
			for (auto &b : boards)
				if (b->link)
					b->link->sync ();
		} else
			run_slice (master ());
		/* All stopped: the rings' work of the boards past their blank */
		for (auto &b : boards) {
			if (b->vblank_due) {
				b->vblank_due = false;
				if (b->has_comm)
					b->comm.vblank ();
			}
		}
	}
	choose_screen ();
	mix.clear ();
	for (auto &b : boards)
		b->end_frame (&mix);
	for (size_t q = 0; q + 1 < mix.size (); q += 2) {
		audio.push_back ((int16_t) std::max (-32768, std::min (32767, mix[q])));
		audio.push_back ((int16_t) std::max (-32768, std::min (32767, mix[q + 1])));
	}
	keep_nvram ();
	if (reset_due) {
		reset_due = false;
		reset (hl::RESET_HARD);
	}
	return 0;
}

void
Naomi::on_reset (hl::ResetType type)
{
	for (auto &b : boards)
		b->reset (type);
}

const char *
Naomi::debug_string ()
{
	return master ()->debug_string ();
}

int
Naomi::on_load_state (hl::State *state)
{
	int ret = 0;

	for (auto &b : boards)
		ret |= b->load_state (state);
	return ret;
}

int
Naomi::on_save_state (hl::State *state)
{
	int ret = 0;

	for (auto &b : boards)
		ret |= b->save_state (state);
	return ret;
}

Naomi *
Naomi::create (hl::Game *game)
{
	Naomi *naomi = new Naomi (game);
	unsigned count = game->boards ? game->boards : 1, i;
	const char *forced = getenv ("HR_NAOMI_BOARDS");

	if (game->network_boards > 1) {
		count = game->network_boards;
		naomi->networked = true;
		naomi->comm_boxes.reset (new CommBoxes (count));
		HL_PRINT ("naomi: %u whole systems on a ring", count);
	} else if (forced)
		count = (unsigned) MAX2 (1, MIN2 (4, atoi (forced)));
	if (count > 1 && !naomi->networked) {
		naomi->link.reset (new Multiboard (count));
		HL_PRINT ("naomi: %u boards linked", count);
	}
	for (i = 0; i < count; i++) {
		naomi->boards.emplace_back (new Board (naomi, i, game));
		if (!naomi->boards.back ()->init ()) {
			delete naomi;
			return NULL;
		}
		if (naomi->networked)
			naomi->boards.back ()->comm.attach (comm_local_ring (naomi->comm_boxes.get (), i));
	}
	/* The ring over TCP: not a cabinet alone (no socket then, nor the
	 * firewall's question); opened when the game starts its board */
	{
		const hl::Config *cfg = hl::Config::get ();
		bool loop = cfg->port_in == cfg->port_out &&
		            (!strcmp (cfg->address_out, "127.0.0.1") || !strcmp (cfg->address_out, "localhost"));

		if (!naomi->networked && naomi->master ()->has_comm && cfg->link != 0 && !loop)
			naomi->master ()->comm.attach (comm_network_ring (naomi));
	}
	naomi->audio_channels = 2;
	/* The slaves' I/O boards were declared after it */
	if (naomi->master ()->drive_board)
		hl::Outputs::get ()->board (14, true);
	naomi->master ()->setup_servo ();
	naomi->master ()->setup_hopper ();
	naomi->setup_screens ();
	naomi->renderer = new Renderer (naomi);
	naomi->load_nvram ();
	if (naomi->networked)
		for (auto &b : naomi->boards)
			b->configure_eeprom ();
	else
		naomi->master ()->configure_eeprom ();
	naomi->master ()->link_settings ();
	naomi->start_threads ();
	return naomi;
}

} /* namespace naomi */
