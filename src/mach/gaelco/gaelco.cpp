/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/gaelco/atvtrack.cpp (license BSD-3-Clause,
 * copyright-holders Samuele Zannoli).
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
 * Gaelco's PowerVR Boards
 * =======================
 *
 * ATV Track and Gaelco Football (2002, REF. 020419 and 020201), Smashing
 * Drive (2000, REF. 010131):
 *
 *  - two Hitachi SH-4s (HD6417750S), 33 MHz x 6 = 200 MHz, little endian;
 *  - the slave's NEC PowerVR Neon 250 and its 32 MB (gaelco-gpu.cpp);
 *  - an Altera FLEX EPF10K50 FPGA, configured at power on by an EPC1PC8:
 *    the 1 KB both CPUs share, the control registers, the 32 kHz DAC,
 *    the inputs behind the master's port A;
 *  - ATV Track's program and data in four 128 Mbit NAND chips (K9F2808U0B,
 *    a byte each of the 32-bit bus); Smashing Drive's in ROMs mapped at 0
 *    and 14000000.
 *
 * Master SH-4 Memory Map
 * ======================
 *
 *	ATV Track		Smashing Drive
 *	00000000-000003FF	10000000-100003FF  Shared RAM (ATV Track boots
 *						   there: the FPGA's configuration
 *						   holds its boot loader)
 *	00020000-0002003F	10000400-1000043F  Control registers
 *	00020040-0002007F	10000440-1000047F  DAC buffer
 *	-			00000000-03FFFFFF  Program ROM
 *	0C000000-0C7FFFFF	0C000000-0C7FFFFF  RAM (8 MB)
 *	14000000		-		   NAND data (a byte a chip)
 *	14100000		-		   NAND command
 *	14200000		-		   NAND address
 *	-			14000000-143FFFFF  Data ROM
 *
 * Slave SH-4 Memory Map
 * =====================
 *
 *	00000000-000003FF	Shared RAM, and at 10000000 (ATV Track's
 *				boot code clears its 1D0 there)
 *	0C000000-0CFFFFFF	RAM (16 MB)
 *	14000000-14004FFF	The Neon 250's registers and PCI configuration
 *	18000000-19FFFFFF	The Neon 250's RAM (32 MB)
 *
 * The slave is held in its reset until port A's 7xxx sets bit 8 and the
 * control register then sets bit 2.
 *
 * Control Registers
 * =================
 *
 *  00	Read: statuses, all set when well. Written: bit 2 lets the slave run;
 *	the others (the GPU, ...) unknown
 *  04	The RS-422/485 port, linking the cabinets
 *
 * The DAC
 * =======
 *
 * 2 x 32 bytes, played at 32 kHz: a frame of four signed 16-bit samples
 * (bass 0, bass 1, left, right). Each half played raises an interrupt on
 * the master, which fills it again. The IRL pins it drives are not known
 * for sure (said "IRL5"); it starts with the first sample written.
 *
 * Port A (Master)
 * ===============
 *
 * Bits 15-11 select a port of the FPGA: F000 and E800 read, F800 and 7800
 * written. Switches are active low.
 *
 *  Bit	F000 (JP10)	E800 (JP8)	F800 (ADC, DAC)		7800 (system)
 *  10	-		game's		DAC out (JP3)		motion lamp?
 *  9	ADC data	game's		DAC out (JP3)		amplifier on?
 *  8	coin		game's		DAC out (JP3)		slave start
 *  7	-		game's		1			slave (30, then 10)
 *  6	test		-		ADC CS, DAC /CS		-
 *  5	down		-		ADC CLK, DAC /CLK	-
 *  4	service		-		DAC data (JP6)		lamp
 *  3	up		-		ADC data (JP5)		coin counter
 *  2	Smashing Drive: the FPGA configured (1)
 *  1	the data bits' direction (Smashing Drive: the FPGA's configuration)
 *  0	Smashing Drive: the FPGA configuration's clock
 *
 * The ADC and the DAC are 4-channel serial chips. System's bit 1 set: no outputs
 * meant, the data bits being inputs.
 *
 * What each game has there, from their own input tests:
 *
 *  ATV Track, JP8: 10 start, 9 the emergency stop (low when not pressed), 8 and
 *  7 the right and left brake levers; JP10's up and down the volume. ADC: 0
 *  the handlebar (high to the left), 1 the throttle.
 *  Smashing Drive, JP8: 9 the horn, which starts, 8 the brake, 7 the gas; JP10
 *  as ATV Track. ADC: 0 the wheel.
 *  Gaelco Football, 6800 player 1 and 5800 player 2: 10 start, 9 to 7 buttons
 *  C, B, A, 6 left, 5 right, 4 down, 3 up; 7000: 8 the coin, 6 test, 4 service.
 */

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "hl/gamecontrols.h"
#include "hl/core.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/games.h"
#include "hl/ffb.h"
#include "hl/input.h"
#include "hl/outputs.h"
#include "hl/paths.h"
#include "hl/profile.h"
#include "hl/ringlink.h"
#include "hl/state.h"

#include "cpu/sh/sh4.h"
#include "cpu/sh/sh4-wide-view.h"

#include "mach/gaelco/gaelco.h"
#include "mach/gaelco/gaelco-gpu.h"

namespace gaelco {

/* VGA's lines: 480 shown of 525 */
static const unsigned LINES = 525;

/* The IRL pins the DAC drives on the master: "IRL5" (unverified) */
static const unsigned DAC_IRL_PINS = 5;

static inline int16_t
clamp16 (int32_t x)
{
	return (int16_t) ((x < -32768) ? -32768 : (x > 32767) ? 32767 : x);
}

/*
 * The FPGA's Registers and DAC
 */

class Fpga : public hl::Device {
public:
	explicit Fpga (hl::Machine *mach) : hl::Device (mach) {}

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int exec (int cycles) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;

	/* The link: its words sent and received, the master's DMA channels 1 and
	 * 0 taking them */
	void link_start ();
	void link_send (uint32_t word);
	void link_run ();
	hl::RingLink ring;
	std::vector<uint32_t> link_out;
	std::deque<uint32_t> link_in;
	bool link_open;

	/* The SH-4 cycles a second */
	uint64_t cpu_hz;
	/* The frame's samples at 32 kHz, four a sample (as the DAC's) */
	std::vector<int16_t> out;

	uint32_t control;
	uint32_t link;
	uint8_t dac[64];
	unsigned play;		/* the DAC frame played next, 0 to 7 */
	uint64_t acc;		/* cycles x 32000 not yet played */
	bool started;
};

void
Fpga::reset (hl::ResetType type)
{
	(void) type;
	control = 0;
	link = 0;
	memset (dac, 0, sizeof (dac));
	play = 0;
	acc = 0;
	started = false;
	out.clear ();
	link_out.clear ();
	link_in.clear ();
}

int
Fpga::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t offs = addr & 0x7F;

	if (offs >= 0x40) {
		uint64_t data = 0;
		memcpy (&data, &dac[(offs - 0x40) & ~(size - 1)], size);
		set_ptr (val, size, data);
		return 0;
	}
	/* The statuses: all set, as the games want them */
	set_ptr (val, size, ~0ull);
	return 0;
}

int
Fpga::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0x7F;

	if (offs >= 0x40) {
		memcpy (&dac[(offs - 0x40) & ~(size - 1)], &val, size);
		if (!started)
			HL_PRINT ("gaelco: frame %llu: the DAC starts", (unsigned long long) mach->frame);
		started = true;
		return 0;
	}
	switch (offs & ~3) {
	case 0x00:
		control = (uint32_t) val;
		((Gaelco *) mach)->control_put (control);
		break;
	case 0x04:
		link = (uint32_t) val;
		HL_LOG ("fpga: link port = %08X", link);
		link_send ((uint32_t) val);
		break;
	default:
		HL_LOG ("fpga: unhandled write %08X = %08X", addr, (uint32_t) val);
		break;
	}
	return 0;
}

int
Fpga::exec (int cycles)
{
	Gaelco *gaelco = (Gaelco *) mach;

	if (!started)
		return cycles;
	acc += (uint64_t) cycles * 32000;
	while (acc >= cpu_hz) {
		const int16_t *s = (const int16_t *) &dac[play * 8];
		acc -= cpu_hz;
		out.push_back (s[0]);
		out.push_back (s[1]);
		out.push_back (s[2]);
		out.push_back (s[3]);
		play = (play + 1) & 7;
		/* A half played: the master fills it again */
		if (!(play & 3))
			gaelco->set_irl (gaelco->sh_m, DAC_IRL_PINS, true, this);
	}
	return cycles;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Fpga::save_state (hl::State *state)
{
	int ret = 0;
	SAVE (control);
	SAVE (link);
	SAVE (dac);
	SAVE (play);
	SAVE (acc);
	SAVE (started);
	return ret;
}

int
Fpga::load_state (hl::State *state)
{
	int ret = 0;
	LOAD (control);
	LOAD (link);
	LOAD (dac);
	LOAD (play);
	LOAD (acc);
	LOAD (started);
	out.clear ();
	return ret;
}

/*
 * The Link
 * ========
 *
 * The DB9's RS-422, Smashing Drive's Link setting (master, slave): each word
 * written at the control register 04 goes out, bit 31 of 00 telling it can; the
 * DMA channel 1 can send them, and the channel 0 takes those that come into the
 * RAM. Over [Network]'s ring (hl::RingLink), a word's 32 bits at a time, opened
 * when the game waits for its first word or sends one.
 */

enum { LINK_WORDS = 1 };

void
Fpga::link_start ()
{
	const hl::Config *cfg = hl::Config::get ();

	link_open = true;
	if (!cfg->link)
		return;
	ring.set_queued (true);
	if (!ring.open (cfg->port_in, cfg->address_out, cfg->port_out))
		HL_ERROR ("gaelco: the link cannot open port %u", cfg->port_in);
	else
		HL_PRINT ("gaelco: the link: in on port %u, out to %s:%u", cfg->port_in,
		          cfg->address_out, cfg->port_out);
}

void
Fpga::link_send (uint32_t word)
{
	if (!link_open)
		link_start ();
	link_out.push_back (word);
}

void
Fpga::link_run ()
{
	Sh4 *sh = (Sh4 *) ((Gaelco *) mach)->sh_m;

	while (sh->dma_ready (1))
		link_send (sh->dma_from_memory (1));
	if (!ring.is_open ()) {
		link_out.clear ();
		return;
	}
	ring.poll ();
	if (!link_out.empty ()) {
		ring.send (LINK_WORDS, link_out.data (), (uint32_t) (link_out.size () * 4));
		link_out.clear ();
	}
	{
		std::vector<uint8_t> data;
		uint32_t type;
		size_t i;

		while (ring.receive (&type, data, 0)) {
			for (i = 0; type == LINK_WORDS && i + 4 <= data.size (); i += 4) {
				uint32_t word;
				memcpy (&word, &data[i], 4);
				link_in.push_back (word);
			}
		}
	}
	while (!link_in.empty () && sh->dma_ready (0)) {
		sh->dma_to_memory (0, link_in.front ());
		link_in.pop_front ();
	}
}

/*
 * The NAND Chips (ATV Track, Gaelco Football)
 * ===========================================
 *
 * Four Samsung K9F2808U0B, 16 MB + 512 KB each: 32768 pages of 528 bytes
 * (512 + 16 spare), erased 32 pages at a time. Each sees a byte of the
 * 32-bit accesses, chip c the byte c; data holds them as the ROM set does
 * (chip c's byte k at 4k + c). The commands:
 *
 *  00, 01, 50	read from byte 0, 256, 512 of the page; three address
 *		bytes (the column, the page's low and high)
 *  80 ... 10	program: the data ANDed in
 *  60 ... D0	erase: two address bytes (the page)
 *  70		status: C0, ready and not protected
 *  90		ID: EC 73
 *  FF		reset
 */

class Nand : public hl::Device {
public:
	Nand (hl::Machine *mach, hl::Buffer *data) : hl::Device (mach), data (data) {}

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;

	static const unsigned PAGE = 528;
	static const unsigned BLOCK = 32;

	hl::Buffer *data;
	struct Chip {
		uint8_t command;
		uint8_t step;	/* the address byte written next */
		uint16_t area;	/* 0, 256 or 512: where 00, 01, 50 point */
		uint32_t page;
		uint32_t column;
		uint32_t offset;/* read or programmed since the address */
	} chips[4];
};

void
Nand::reset (hl::ResetType type)
{
	(void) type;
	memset (chips, 0, sizeof (chips));
	for (Chip &chip : chips)
		chip.command = 0xFF;
}

int
Nand::get (unsigned size, uint32_t addr, void *val)
{
	uint32_t out = 0;
	int c;

	if (addr & 0x00300000) {
		HL_LOG ("nand: unhandled read %08X", addr);
		set_ptr (val, size, 0);
		return 0;
	}
	for (c = 3; c >= 0; c--) {
		Chip &chip = chips[c];
		uint8_t byte;

		if (chip.command == 0x00 || chip.command == 0x01 || chip.command == 0x50) {
			uint32_t pos = chip.page * PAGE + chip.column + chip.offset;
			byte = (pos * 4 + c < data->size ()) ? (uint8_t) data->get (1, pos * 4 + c) : 0xFF;
			chip.offset++;
		} else if (chip.command == 0x90)
			byte = (chip.offset++ & 1) ? 0x73 : 0xEC;
		else
			byte = 0xC0;
		out = (out << 8) | byte;
	}
	set_ptr (val, size, out);
	return 0;
}

int
Nand::put (unsigned size, uint32_t addr, uint64_t val)
{
	unsigned c;

	(void) size;
	switch (addr & 0x00300000) {
	case 0x00000000:
		/* Data, programmed */
		for (c = 0; c < 4; c++, val >>= 8) {
			Chip &chip = chips[c];
			if (chip.command == 0x80) {
				uint32_t pos = (chip.page * PAGE + chip.column + chip.offset) * 4 + c;
				if (pos < data->size ())
					data->put (1, pos, data->get (1, pos) & (val & 0xFF));
				chip.offset++;
				((Gaelco *) mach)->nvram_changed = true;
			}
		}
		break;
	case 0x00100000:
		/* Command */
		for (c = 0; c < 4; c++, val >>= 8) {
			Chip &chip = chips[c];
			chip.command = val & 0xFF;
			chip.step = 0;
			chip.offset = 0;
			switch (chip.command) {
			case 0x00:
				chip.area = 0;
				break;
			case 0x01:
				chip.area = 256;
				break;
			case 0x50:
				chip.area = 512;
				break;
			case 0x60:
				/* The page alone */
				chip.step = 1;
				chip.page = 0;
				break;
			case 0xD0: {
				uint32_t first = chip.page & ~(BLOCK - 1), i;
				for (i = 0; i < BLOCK * PAGE; i++) {
					uint32_t pos = (first * PAGE + i) * 4 + c;
					if (pos < data->size ())
						data->put (1, pos, 0xFF);
				}
				((Gaelco *) mach)->nvram_changed = true;
				break;
			}
			case 0x10: case 0x70: case 0x80: case 0x90: case 0xFF:
				break;
			default:
				HL_LOG ("nand: chip %u: unknown command %02X", c, chip.command);
				chip.command = 0xFF;
				break;
			}
		}
		break;
	case 0x00200000:
		/* Address */
		for (c = 0; c < 4; c++, val >>= 8) {
			Chip &chip = chips[c];
			switch (chip.step) {
			case 0:
				/* The column, in the area the pointer chose */
				chip.column = chip.area + (val & 0xFF);
				chip.page = 0;
				break;
			case 1:
				chip.page = val & 0xFF;
				break;
			case 2:
				chip.page |= (val & 0xFF) << 8;
				break;
			}
			chip.step++;
			chip.offset = 0;
		}
		break;
	default:
		HL_LOG ("nand: unhandled write %08X = %08X", addr, (uint32_t) val);
		break;
	}
	return 0;
}

int
Nand::save_state (hl::State *state)
{
	int ret = 0;
	SAVE (chips);
	return ret;
}

int
Nand::load_state (hl::State *state)
{
	int ret = 0;
	LOAD (chips);
	return ret;
}

/*
 * The Data Flash (Smashing Drive)
 * ===============================
 *
 * PRG.IC23, at 14000000: its data, and the settings the game programs in
 * its last sectors (from 143E0000). The commands as AMD's, by their values
 * (AA, 55, then A0 a byte programmed, 80 AA 55 30 a 64 KB sector erased,
 * 80 AA 55 10 all of it, F0 back to reading); done at once, so the status
 * polls read the data. The reads go to the ROM region (setup_master_mmap ())
 */

class Flash : public hl::Device {
public:
	Flash (hl::Machine *mach, hl::Buffer *data) : hl::Device (mach), data (data) {}

	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override { (void) type; cycle = 0; mode = 0; }
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;

	hl::Buffer *data;
	unsigned cycle;		/* the unlock's cycles seen: AA, 55 */
	unsigned mode;		/* 0 commands, 1 a byte to program, 2 an erase's
				 * unlock to come */
};

int
Flash::put (unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t offs = addr & 0x003FFFFF;
	uint8_t byte = val & 0xFF;

	(void) size;
	if (mode == 1) {
		data->put (1, offs, data->get (1, offs) & byte);
		mode = 0;
		cycle = 0;
		((Gaelco *) mach)->nvram_changed = true;
		return 0;
	}
	if (byte == 0xF0) {
		mode = 0;
		cycle = 0;
		return 0;
	}
	switch (cycle) {
	case 0:
		cycle = (byte == 0xAA) ? 1 : 0;
		break;
	case 1:
		cycle = (byte == 0x55) ? 2 : 0;
		break;
	case 2:
		cycle = 0;
		if (mode == 2) {
			if (byte == 0x30)
				memset (data->ptr (offs & ~0xFFFFu), 0xFF, 0x10000);
			else if (byte == 0x10)
				memset (data->ptr (0), 0xFF, data->size ());
			HL_LOG ("flash: frame %llu: erased %s at %06X",
			        (unsigned long long) mach->frame,
			        byte == 0x30 ? "a sector" : "all", offs & ~0xFFFFu);
			((Gaelco *) mach)->nvram_changed = true;
			mode = 0;
		} else if (byte == 0xA0)
			mode = 1;
		else if (byte == 0x80)
			mode = 2;
		else
			HL_LOG ("flash: command %02X", byte);
		break;
	}
	return 0;
}

int
Flash::save_state (hl::State *state)
{
	int ret = 0;
	SAVE (cycle);
	SAVE (mode);
	return ret;
}

int
Flash::load_state (hl::State *state)
{
	int ret = 0;
	LOAD (cycle);
	LOAD (mode);
	return ret;
}

/*
 * The Interrupts
 */

void
Gaelco::set_irl (hl::Cpu *cpu, unsigned pins, bool on, hl::Device *from)
{
	HL_ASSERT (pins < 15);
	cpu->set_irq_state (SH4_IESOURCE_IRQ0 + pins,
	                    on ? hl::IRQ_STATE_RAISED : hl::IRQ_STATE_CLEAR, from);
}

void
Gaelco::control_put (uint32_t val)
{
	bool run = (val & 4) && slave_enabled;

	if (run && slave_held) {
		HL_PRINT ("gaelco: frame %llu: the slave starts", (unsigned long long) frame);
		sh_s->reset (hl::RESET_HARD);
		sh_s->set_state (hl::CPU_STATE_RUN);
		slave_held = false;
	} else if (!run && !slave_held) {
		HL_PRINT ("gaelco: frame %llu: the slave is held", (unsigned long long) frame);
		sh_s->set_state (hl::CPU_STATE_STOP);
		slave_held = true;
	}
}

/*
 * Port A and the ADC
 */

/* The ADC's channels: the game's controls on "an" (the wheel or the
 * handlebar, high to the left; ATV Track's throttle), the middle for the
 * others */
static uint8_t
adc_sample (unsigned channel)
{
	uint16_t v;

	if (hl::GameControls::get ().analog ("an", channel, &v))
		return (uint8_t) (v >> 8);
	return (channel == 1) ? 0x00 : 0x80;
}

/* An ADC0834's serial protocol: CS low; on CLK's rising edges, DI's start
 * bit, then SGL/DIF, ODD/SIGN, SELECT1; after a clock for the multiplexer,
 * the sample on DO on the falling edges, MSB first */
static void
adc_clock (Gaelco *gaelco, bool cs, bool clk, bool di)
{
	auto &adc = gaelco->adc;
	bool rise = clk && !adc.clk;
	bool fall = !clk && adc.clk;

	adc.clk = clk;
	if (cs) {
		adc.cs = true;
		adc.state = 0;
		adc.out = true;
		return;
	}
	adc.cs = false;
	if (rise) {
		if (adc.state == 0) {
			if (di) {
				adc.state = 1;
				adc.mux = 0;
			}
		} else if (adc.state < 4) {
			adc.mux = (adc.mux << 1) | (di ? 1 : 0);
			if (++adc.state == 4)
				adc.value = adc_sample (((adc.mux & 1) << 1) | ((adc.mux >> 1) & 1));
		}
	} else if (fall && adc.state >= 4) {
		/* 4: the multiplexer settles, DO low; 5 to 12: bits 7 to 0 */
		unsigned n = adc.state - 4;
		adc.out = (n == 0) ? false : (n <= 8) ? ((adc.value >> (8 - n)) & 1) : true;
		adc.state++;
	}
}

static void lamps_tell (Gaelco *gaelco);

/* The DAC, seen selected on the port's bit 6: 11-bit words, MSB first, two or
 * three at each selection. A word with its bits 10 and 9 clear is the steering
 * motor's, one side a word (bit 8) and its strength (bits 7-0): ATV Track's
 * test writes 7F on side 1 with the handlebar held right, whose spring pushes
 * left; on Smashing Drive, the same on side 0. Bit 9 alone is Smashing Drive's
 * seat motor (its OUTPUT TEST), told on the outputs */
static void
dac_clock (Gaelco *gaelco, bool cs, bool clk, bool di)
{
	auto &dac = gaelco->dac;

	if (cs && clk && !dac.clk) {
		dac.word = (dac.word << 1) | (di ? 1 : 0);
		dac.bits++;
	}
	if (!cs && dac.cs) {
		unsigned bits;

		for (bits = dac.bits; bits >= 11; bits -= 11) {
			unsigned word = (unsigned) (dac.word >> (bits - 11)) & 0x7FF;
			if (!(word >> 9))
				dac.side[word >> 8] = word & 0xFF;
			else if ((word >> 9) == 1)
				dac.seat = word & 0xFF;
		}
		dac.bits = 0;
		dac.word = 0;
		lamps_tell (gaelco);
		if (gaelco->inputs != Gaelco::INPUTS_FOOTBALL) {
			unsigned left = gaelco->inputs == Gaelco::INPUTS_ATV ? 1 : 0;
			dac.force = (int) dac.side[left] - (int) dac.side[left ^ 1];
		}
	}
	dac.cs = cs;
	dac.clk = clk;
}

/* A frame's force: over two, as the games pulse the motor a frame in two, on
 * and off, which its inertia smooths */
static void
motor_frame (Gaelco *gaelco)
{
	auto &dac = gaelco->dac;
	int force = dac.force + dac.last;

	dac.last = dac.force;
	if (gaelco->inputs != Gaelco::INPUTS_FOOTBALL && force != dac.told) {
		dac.told = force;
		hl::ForceFeedback::set (hl::ForceFeedback::CONSTANT,
		                        std::clamp (force / 254.0f, -1.0f, 1.0f), gaelco->frame);
	}
}

/* The lamps and the coin counter, on the outputs; Gaelco Football's two start
 * lamps at bits 4 and 5, one for the others at bit 4 */
static void
lamps_tell (Gaelco *gaelco)
{
	uint16_t val = gaelco->system_port;
	uint8_t out = 0;

	if (val & 0x10)
		out |= 0x80;
	if (gaelco->inputs == Gaelco::INPUTS_FOOTBALL) {
		if (val & 0x20)
			out |= 0x40;
		if (val & 0x08)
			out |= 0x20;
	} else {
		if (val & 0x08)
			out |= 0x40;
		if (gaelco->dac.seat)
			out |= 0x20;
	}
	if (out != gaelco->lamps) {
		gaelco->lamps = out;
		hl::Outputs::get ()->jvs (&out, 1);
	}
}

/* A switch, active low */
static inline uint16_t
low_if (bool on, unsigned bit)
{
	return on ? (uint16_t) ~(1u << bit) : (uint16_t) 0xFFFF;
}

/* What each game has on the FPGA's ports, from its own input test */
static uint16_t
port_inputs (Gaelco *gaelco, unsigned select)
{
	/* The game's controls on the port its select reads ("f000",
	 * "e800"...), active low */
	char port[8];
	uint16_t in;
	/* Smashing Drive starts silent: its volume is a float in its RAM (World
	 * set: 8C0733FC), cleared with the rest at power on, that the cabinet's
	 * buttons change; nothing on the board keeps it. Held up for it at the
	 * start, the button speeds up: 111 frames make 0.38 */
	bool volume_up = gaelco->inputs == Gaelco::INPUTS_SMASHDRV &&
	                 gaelco->frame >= 300 && gaelco->frame < 411;

	snprintf (port, sizeof (port), "%04x", select);
	in = (uint16_t) ~hl::GameControls::get ().switches (port, gaelco->frame);
	if (select == 0xF000) {
		/* JP10: the ADC's DO on bit 9 */
		if (!gaelco->adc.out)
			in &= ~(1 << 9);
		in &= low_if (volume_up, 3);
	}
	return in;
}

static int
porta_get_m (Sh4 *ctx, uint16_t *val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Gaelco *gaelco = (Gaelco *) dev->mach;
	uint16_t in = 0xFFFF;

	in = port_inputs (gaelco, gaelco->porta_m & 0xF800);
	*val = in;
	return 0;
}

static int
porta_put_m (Sh4 *ctx, uint16_t val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Gaelco *gaelco = (Gaelco *) dev->mach;

	gaelco->porta_m = val;
	if ((val & 0xF000) == 0x7000 && (val & 0x0100)) {
		if (!gaelco->slave_enabled)
			HL_PRINT ("gaelco: frame %llu: port A lets the slave run (%04X)",
			          (unsigned long long) gaelco->frame, val);
		gaelco->slave_enabled = true;
	}
	if ((val & 0xF800) == 0xF800)
		adc_clock (gaelco, (val >> 6) & 1, (val >> 5) & 1, (val >> 3) & 1);
	if ((val & 0xF800) == 0xF800)
		dac_clock (gaelco, (val >> 6) & 1, (val >> 5) & 1, (val >> 4) & 1);
	/* Bit 1 set: the data bits are inputs */
	if ((val & 0xF800) == 0x7800 && !(val & 2)) {
		gaelco->system_port = val;
		lamps_tell (gaelco);
	}
	return 0;
}

static int
porta_get_s (Sh4 *ctx, uint16_t *val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Gaelco *gaelco = (Gaelco *) dev->mach;
	*val = gaelco->porta_s;
	return 0;
}

static int
porta_put_s (Sh4 *ctx, uint16_t val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Gaelco *gaelco = (Gaelco *) dev->mach;
	gaelco->porta_s = val;
	return 0;
}

/*
 * The Boot Loader (ATV Track, Gaelco Football)
 * ============================================
 *
 * The FPGA's first two Embedded Array Blocks are the shared RAM; the
 * EPC1PC8 configures them with the boot loader. Their 4096 bits each are
 * scattered over the configuration's bits, and their data lines over the
 * words' bits.
 */

/* bit 15 of the result is v's bit b[0], ..., bit 0 v's bit b[15] */
static uint16_t
bitswap16 (uint16_t v, const uint8_t (&b)[16])
{
	uint16_t out = 0;
	unsigned i;
	for (i = 0; i < 16; i++)
		out |= ((v >> b[i]) & 1) << (15 - i);
	return out;
}

/* Embedded Array Block eab of an Altera FLEX 10KE, 512 bytes; the offsets
 * fit this dump, whose header the dumper dropped */
static void
fpga_eab (uint8_t *dst, const hl::Buffer *pof, unsigned eab)
{
	static const uint8_t order[16] = { 15, 14, 13, 12, 9, 8, 7, 6, 5, 4, 3, 11, 10, 2, 1, 0 };
	uint32_t start = 0x45B * 8 + 1 + (0x25E6 * 8) * eab;
	unsigned bit;

	for (bit = 0; bit < 4096; bit++) {
		uint32_t t = bitswap16 ((uint16_t) bit, order) ^ 0x0FC0;
		uint32_t pos = start + (t & 0x1F) + ((t >> 5) & 1) * 63 + ((t >> 6) & 0x3F) * 0x4D * 8;
		unsigned value = (pos / 8 < pof->size ()) ? (pof->get (1, pos / 8) >> (pos & 7)) & 1 : 0;
		dst[bit / 8] = (uint8_t) ((dst[bit / 8] & ~(1 << (bit & 7))) | (value << (bit & 7)));
	}
}

static void
load_boot_loader (Gaelco *gaelco)
{
	static const uint8_t low[16] = { 7, 9, 0, 10, 3, 11, 4, 12, 2, 15, 1, 13, 6, 8, 5, 14 };
	static const uint8_t high[16] = { 5, 10, 7, 9, 6, 13, 3, 15, 2, 11, 1, 8, 0, 12, 4, 14 };
	uint8_t eabs[1024];
	unsigned i;

	fpga_eab (&eabs[0], gaelco->fpga_conf, 0);
	fpga_eab (&eabs[512], gaelco->fpga_conf, 1);
	for (i = 0; i < 256; i++) {
		uint16_t lo = eabs[i * 2 + 512] | (eabs[i * 2 + 513] << 8);
		uint16_t hi = eabs[i * 2] | (eabs[i * 2 + 1] << 8);
		gaelco->shared->put (2, i * 4, bitswap16 (lo, low));
		gaelco->shared->put (2, i * 4 + 2, bitswap16 (hi, high));
	}
}

/*
 * The Game's Memories
 * ===================
 *
 * What the games program in the NAND or the flash (settings, bookkeeping,
 * scores), kept in nvram/<game>.nvram: of 68 MB and 4 MB, the erase blocks
 * that differ from the ROM's, each a u32 offset then its bytes, in a block
 * NAND or FLSH.
 */

#define NVRAM_NAND	"NAND"
#define NVRAM_FLASH	"FLSH"

/* The frames after a change, the game's writing done, before saving */
static const unsigned NVRAM_DELAY = 180;

void
Gaelco::nvram_buffers (const char **type, hl::Buffer **now, const uint8_t **rom, uint32_t *block)
{
	if (board == BOARD_ATVTRACK) {
		*type = NVRAM_NAND;
		*now = nand_data;
		*rom = (const uint8_t *) game->section_data ("nand")->ptr (0);
		*block = Nand::BLOCK * Nand::PAGE * 4;
	} else {
		*type = NVRAM_FLASH;
		*now = data_rom;
		*rom = (const uint8_t *) game->section_data ("data")->ptr (0);
		*block = 0x10000;
	}
}

void
Gaelco::load_nvram ()
{
	const std::vector<uint8_t> *saved;
	const char *type;
	hl::Buffer *now;
	const uint8_t *rom;
	std::string file, read;
	uint32_t block;
	size_t pos = 0;

	if (!game || !game->name)
		return;
	file = std::string ("nvram/") + game->name + ".nvram";
	nvram_path = hl::Paths::game_write (game->name, file);
	if (hl::Paths::has_write_dir ())
		hl::Paths::seed (file, nvram_path);
	read = hl::Paths::game_read (game->name, file);
	switch (nvram.load (read.c_str ())) {
	case hl::Nvram::LOADED:
		break;
	case hl::Nvram::MISSING:
		return;
	default:
		HL_ERROR ("gaelco: '%s' is no nvram file of this version: kept, and nothing saved", read.c_str ());
		nvram_path.clear ();
		return;
	}
	nvram_buffers (&type, &now, &rom, &block);
	saved = nvram.get (type);
	if (!saved)
		return;
	while (pos + 4 + block <= saved->size ()) {
		uint32_t offs;
		memcpy (&offs, saved->data () + pos, 4);
		if (offs + block <= now->size ())
			memcpy (now->ptr (offs), saved->data () + pos + 4, block);
		pos += 4 + block;
	}
	HL_PRINT ("gaelco: the game's settings from '%s'", read.c_str ());
}

void
Gaelco::save_nvram ()
{
	std::vector<uint8_t> out;
	const char *type;
	hl::Buffer *now;
	const uint8_t *rom;
	uint32_t block, offs;
	static bool failing = false;

	if (nvram_path.empty ())
		return;
	nvram_buffers (&type, &now, &rom, &block);
	for (offs = 0; offs + block <= now->size (); offs += block) {
		const uint8_t *p = (const uint8_t *) now->ptr (offs);
		if (!memcmp (p, rom + offs, block))
			continue;
		out.insert (out.end (), (const uint8_t *) &offs, (const uint8_t *) &offs + 4);
		out.insert (out.end (), p, p + block);
	}
	nvram.set (type, out.data (), out.size ());
	if (nvram.save (nvram_path.c_str ()))
		failing = false;
	else if (!failing) {
		HL_ERROR ("gaelco: cannot save '%s'", nvram_path.c_str ());
		failing = true;
	}
}

void
Gaelco::keep_nvram ()
{
	if (nvram_changed) {
		nvram_changed = false;
		nvram_due = NVRAM_DELAY;
	} else if (nvram_due && !--nvram_due)
		save_nvram ();
}

/*
 * The Machine
 */

static void
mix_audio (Gaelco *gaelco)
{
	std::vector<int16_t> &in = gaelco->fpga->out;
	const unsigned n_out = 44100 / 60;
	unsigned n_in = (unsigned) (in.size () / 4), k;

	/* The basses into the sides, 32 kHz to 44.1 */
	for (k = 0; k < n_out; k++) {
		int32_t l = 0, r = 0;
		if (n_in) {
			const int16_t *s = &in[(k * n_in / n_out) * 4];
			l = s[2] + s[0] / 2;
			r = s[3] + s[1] / 2;
		}
		gaelco->audio.push_back (clamp16 (l));
		gaelco->audio.push_back (clamp16 (r));
	}
	in.clear ();
}

int
Gaelco::run_frame ()
{
	unsigned line;

	for (line = 0; line < LINES; line++) {
		{
			HL_PROFILE (SH4_MASTER);
			sh_current = sh_m;
			sh_m->run (cycles_per_line);
		}
		{
			HL_PROFILE (SH4_SLAVE);
			sh_current = sh_s;
			sh_s->run (cycles_per_line);
		}
		fpga->exec (cycles_per_line);
		if (line == 480)
			gpu->vblank ();
		if (fpga->link_open)
			fpga->link_run ();
		else if (((Sh4 *) sh_m)->dma_ready (0))
			fpga->link_start ();
	}
	mix_audio (this);
	motor_frame (this);
	keep_nvram ();
	return 0;
}

void
Gaelco::on_reset (hl::ResetType type)
{
	(void) type;

	porta_m = 0xFFFF;
	porta_s = 0xFFFF;
	memset (&adc, 0, sizeof (adc));
	adc.cs = true;
	adc.out = true;
	memset (&dac, 0, sizeof (dac));
	lamps = 0;
	system_port = 0;

	/* The slave held until the master lets it go */
	slave_enabled = false;
	slave_held = true;
	sh_s->set_state (hl::CPU_STATE_STOP);
	sh_current = sh_m;

	/* The shared RAM was cleared: the boot loader again */
	if (board == BOARD_ATVTRACK)
		load_boot_loader (this);
}

const char *
Gaelco::debug_string ()
{
	static char out[256];
	char *mstr, *sstr;

	mstr = strdup (sh_m->debug_string ());
	sstr = strdup (sh_s->debug_string ());
	snprintf (out, sizeof (out), "[%s %04X] [%s%s] [%s]",
	          mstr, porta_m, sstr, slave_held ? " held" : "",
	          gpu->debug_string ());
	free (mstr);
	free (sstr);
	return out;
}

int
Gaelco::on_load_state (hl::State *state)
{
	int ret = 0;
	LOAD (porta_m);
	LOAD (porta_s);
	LOAD (slave_enabled);
	LOAD (slave_held);
	LOAD (adc);
	LOAD (dac);
	LOAD (lamps);
	LOAD (system_port);
	return ret;
}

int
Gaelco::on_save_state (hl::State *state)
{
	int ret = 0;
	SAVE (porta_m);
	SAVE (porta_s);
	SAVE (slave_enabled);
	SAVE (slave_held);
	SAVE (adc);
	SAVE (dac);
	SAVE (lamps);
	SAVE (system_port);
	return ret;
}

#undef SAVE
#undef LOAD

#define DEV_FLAGS (HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW)

static hl::Mmap *
setup_master_mmap (Gaelco *gaelco)
{
	hl::Mmap *mmap = new hl::Mmap (gaelco);
	int ret = 0;

	ret |= mmap->add_ram (0x0C000000, 0x0C7FFFFF, 0x007FFFFF, 0, gaelco->ram_m, "RAM/M");
	if (gaelco->board == Gaelco::BOARD_ATVTRACK) {
		ret |= mmap->add_ram (0x00000000, 0x000003FF, 0x000003FF, 0, gaelco->shared, "SHARED/M");
		/* Also in area 4, as on Smashing Drive (ATV Track's code
		 * clears 100001D0 there) */
		ret |= mmap->add_ram (0x10000000, 0x100003FF, 0x000003FF, 0, gaelco->shared, "SHARED4/M");
		ret |= mmap->add_dev (0x00020000, 0x0002007F, 0x0000007F, DEV_FLAGS, gaelco->fpga, "FPGA/M");
		/* Not logged: the games read it all */
		ret |= mmap->add_dev (0x14000000, 0x142FFFFF, 0x003FFFFF, HL_REGION_RW | HL_REGION_SIZE_ALL, gaelco->nand, "NAND/M");
	} else {
		ret |= mmap->add_rom (0x00000000, 0x03FFFFFF, 0x03FFFFFF, 0, gaelco->rom, "ROM/M");
		ret |= mmap->add_ram (0x10000000, 0x100003FF, 0x000003FF, 0, gaelco->shared, "SHARED/M");
		ret |= mmap->add_dev (0x10000400, 0x1000047F, 0x0000007F, DEV_FLAGS, gaelco->fpga, "FPGA/M");
		/* Read as a ROM, written as a flash */
		ret |= mmap->add_rom (0x14000000, 0x143FFFFF, 0x003FFFFF, 0, gaelco->data_rom, "DATA/M");
		ret |= mmap->add_dev (0x14000000, 0x143FFFFF, 0x003FFFFF, HL_REGION_W | HL_REGION_SIZE_ALL,
		                      gaelco->flash, "FLASH/M");
	}
	if (ret) {
		delete mmap;
		return NULL;
	}
	return mmap;
}

static hl::Mmap *
setup_slave_mmap (Gaelco *gaelco)
{
	hl::Mmap *mmap = new hl::Mmap (gaelco);
	int ret = 0;

	ret |= mmap->add_ram (0x0C000000, 0x0CFFFFFF, 0x00FFFFFF, 0, gaelco->ram_s, "RAM/S");
	ret |= mmap->add_ram (0x00000000, 0x000003FF, 0x000003FF, 0, gaelco->shared, "SHARED/S");
	ret |= mmap->add_ram (0x10000000, 0x100003FF, 0x000003FF, 0, gaelco->shared, "SHARED4/S");
	ret |= mmap->add_dev (0x14000000, 0x14004FFF, 0x00007FFF, DEV_FLAGS, gaelco->gpu, "NEON/S");
	ret |= mmap->add_ram (0x18000000, 0x19FFFFFF, 0x01FFFFFF, 0, gaelco->vram, "VRAM/S");
	if (ret) {
		delete mmap;
		return NULL;
	}
	return mmap;
}

/* Smashing Drive's program: sdra and sdrb a 16-bit half each of the first
 * 32 MB, sdrc the low halves of the next 32 (ic21 unpopulated) */
static hl::Buffer *
build_smashdrv_rom (hl::Buffer *ab, hl::Buffer *c)
{
	hl::Buffer *rom = hl::Buffer::create (64 * MB, 0, hl::Buffer::LE32);
	unsigned i;

	if (!rom)
		return NULL;
	memset (rom->ptr (0), 0xFF, 64 * MB);
	memcpy (rom->ptr (0), ab->ptr (0), std::min (ab->size (), 32u * MB));
	for (i = 0; i < c->size () / 2 && i < 8 * MB; i++)
		rom->put (2, 32 * MB + i * 4, c->get (2, i * 2));
	return rom;
}

static int
gaelco_init (Gaelco *gaelco)
{
	hl::Machine *mach = gaelco;
	hl::Game *game = mach->game;
	hl::Buffer *nand, *rom_ab, *rom_c;

	nand = game->section_data ("nand");
	rom_ab = game->section_data ("rom_ab");
	rom_c = game->section_data ("rom_c");
	gaelco->fpga_conf = game->section_data ("fpga");
	gaelco->data_rom = game->section_data ("data");

	if (!strncmp (game->name, "gfootbal", 8))
		gaelco->inputs = Gaelco::INPUTS_FOOTBALL;
	else if (!strncmp (game->name, "atvtrack", 8))
		gaelco->inputs = Gaelco::INPUTS_ATV;
	else
		gaelco->inputs = Gaelco::INPUTS_SMASHDRV;

	if (nand && gaelco->fpga_conf)
		gaelco->board = Gaelco::BOARD_ATVTRACK;
	else if (rom_ab && rom_c && gaelco->data_rom)
		gaelco->board = Gaelco::BOARD_SMASHDRV;
	else {
		HL_ERROR ("gaelco: %s: neither the NAND and the FPGA, nor the ROMs", game->name);
		return -1;
	}

	gaelco->shared	= hl::Buffer::create (1 * KB, 0, hl::Buffer::LE32);
	gaelco->ram_m	= hl::Buffer::create (8 * MB, 0, hl::Buffer::LE32);
	gaelco->ram_s	= hl::Buffer::create (16 * MB, 0, hl::Buffer::LE32);
	gaelco->vram	= hl::Buffer::create (32 * MB, 0, hl::Buffer::LE32);
	if (!gaelco->shared || !gaelco->ram_m || !gaelco->ram_s || !gaelco->vram)
		return -1;
	mach->register_buffer (gaelco->shared);
	mach->register_buffer (gaelco->ram_m);
	mach->register_buffer (gaelco->ram_s);
	mach->register_buffer (gaelco->vram);

	if (gaelco->board == Gaelco::BOARD_ATVTRACK) {
		/* The machine's copy, which the games program: a battery's,
		 * kept through the resets and in the states. TODO keep it in
		 * the game's nvram */
		gaelco->nand_data = hl::Buffer::create (nand->size (), 0, hl::Buffer::LE32);
		if (!gaelco->nand_data)
			return -1;
		memcpy (gaelco->nand_data->ptr (0), nand->ptr (0), nand->size ());
		mach->register_buffer (gaelco->nand_data, true);
		gaelco->nand = new Nand (mach, gaelco->nand_data);
		gaelco->nand->name = "nand";
	} else {
		gaelco->rom = build_smashdrv_rom (rom_ab, rom_c);
		if (!gaelco->rom)
			return -1;
		/* Not cleared on reset: a battery's, though a ROM */
		mach->register_buffer (gaelco->rom, true);
		/* The data flash's copy, which the game programs. TODO keep it
		 * in the game's nvram */
		hl::Buffer *data = hl::Buffer::create (gaelco->data_rom->size (), 0, hl::Buffer::LE32);
		if (!data)
			return -1;
		memcpy (data->ptr (0), gaelco->data_rom->ptr (0), data->size ());
		gaelco->data_rom = data;
		mach->register_buffer (data, true);
		gaelco->flash = new Flash (mach, data);
		gaelco->flash->name = "flash";
	}

	hl::Outputs::get ()->board (8, false);
	gaelco->fpga = new Fpga (mach);
	gaelco->gpu = new Neon250 (mach, gaelco->vram);
	gaelco->renderer = new Renderer (gaelco->gpu);
	if (!gaelco->fpga || !gaelco->gpu || !gaelco->renderer)
		return -1;

	/* The SH-4s' clock over VGA's lines: [Emulation] SH4_CLOCK, as for the
	 * Hikaru's games (the board's own is 200 MHz) */
	gaelco->cycles_per_line = (unsigned) ((uint64_t) hl::Config::sh4_clock_mhz () * MHZ / (60 * LINES));
	gaelco->fpga->cpu_hz = (uint64_t) gaelco->cycles_per_line * LINES * 60;
	mach->audio_channels = 2;

	gaelco->mmap_m = setup_master_mmap (gaelco);
	gaelco->mmap_s = setup_slave_mmap (gaelco);
	if (!gaelco->mmap_m || !gaelco->mmap_s)
		return -1;

	/* Both bus masters (MD7) */
	gaelco->sh_m = new Sh4 (mach, gaelco->mmap_m, true, true);
	gaelco->sh_s = new Sh4 (mach, gaelco->mmap_s, true, true);
	if (!gaelco->sh_m || !gaelco->sh_s)
		return -1;
	/* Smashing Drive's master: its pages, all their own addresses, set once.
	 * Not its slave, whose TLB misses at the start page its files in */
	((Sh4 *) gaelco->sh_m)->mmu_trusted = true;
	/* Their RAMs, which they alone write, and Smashing Drive's ROM: their code
	 * compared once a code epoch (sh4-blocks.h), the translations linked */
	for (hl::Cpu *cpu : { gaelco->sh_m, gaelco->sh_s }) {
		((Sh4 *) cpu)->blocks.own (0x0C000000, 0x0CFFFFFF);
		((Sh4 *) cpu)->blocks.own (0x00000000, 0x03FFFFFF);
	}
	((Sh4 *) gaelco->sh_m)->cpu_hz = (uint32_t) gaelco->fpga->cpu_hz;
	((Sh4 *) gaelco->sh_s)->cpu_hz = (uint32_t) gaelco->fpga->cpu_hz;
	/* The 33 MHz crystal: the games' timers count it */
	((Sh4 *) gaelco->sh_m)->pclk_hz = 33000000;
	((Sh4 *) gaelco->sh_s)->pclk_hz = 33000000;
	sh4_set_porta_handlers (gaelco->sh_m, porta_get_m, porta_put_m);
	sh4_set_porta_handlers (gaelco->sh_s, porta_get_s, porta_put_s);
	/* The view's sides, for the widescreen: the code decides, not the game */
	sh4_wide_view_install (gaelco->sh_m);
	sh4_wide_view_install (gaelco->sh_s);

	/* The names the Lua shell knows them by */
	gaelco->sh_m->name	= "master";
	gaelco->sh_s->name	= "slave";
	gaelco->fpga->name	= "fpga";
	gaelco->gpu->name	= "gpu";

	mach->add_memory ("shared", gaelco->shared);
	mach->add_memory ("ram_m", gaelco->ram_m);
	mach->add_memory ("ram_s", gaelco->ram_s);
	mach->add_memory ("vram", gaelco->vram);
	if (gaelco->nand_data)
		mach->add_memory ("nand", gaelco->nand_data);
	if (gaelco->rom) {
		mach->add_memory ("rom", gaelco->rom);
		mach->add_memory ("data", gaelco->data_rom);
	}
	mach->add_memory ("iregs_m", ((Sh4 *) gaelco->sh_m)->iregs);
	mach->add_memory ("iregs_s", ((Sh4 *) gaelco->sh_s)->iregs);

	return 0;
}

Gaelco::Gaelco (hl::Game *game) :
	hl::Machine (game)
{
	strcpy (name, "Gaelco PowerVR");
}

Gaelco::~Gaelco ()
{
	if (nvram_changed || nvram_due)
		save_nvram ();
}

Gaelco *
Gaelco::create (hl::Game *game)
{
	Gaelco *gaelco = new Gaelco (game);

	if (gaelco_init (gaelco)) {
		delete gaelco;
		return NULL;
	}
	gaelco->load_nvram ();
	return gaelco;
}

} /* namespace gaelco */
