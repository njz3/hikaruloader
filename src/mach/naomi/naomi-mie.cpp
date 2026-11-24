/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/mie.cpp (license BSD-3-Clause,
 * copyright-holders Olivier Galibert).
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
#include <vector>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/input.h"
#include "hl/gamecontrols.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The MIE on the Maple Bus
 * ========================
 *
 * The 315-6146: a Z80 with its 2 KB ROM (from MAME's mie.zip), which answers
 * the Maple bus, takes the firmware the BIOS sends it into its RAM, and drives
 * the JVS bus through a UART.
 *
 * Its memory: 0000-07FF the ROM, 7000-703F the Maple registers (mirrored each
 * 40h up to 77FF), 7800-FFFF the RAM. Those registers:
 *
 *  7000-7002	The control: bit 0 start sending, 1 go on sending, 2 add the end
 *		pattern, 8 empty, 11 overflow, 12 receiving, 13 received, 14
 *		sent, 15 a reset pattern received
 *  7003	The words - 1 of the buffer
 *  7004-7023	The buffer, 8 words
 *
 * Its ports: 00-07 the GPIO, 08 their direction (1: input), 10 the JVS data, 12
 * 13 15 the UART's FIFO, LCR and status, 30 50 70 the interrupts (enabled, the
 * Maple's level, pending; levels 0 to 6, mode 2 at the vectors F2 + 2 * level),
 * 90 bit 0 the line driven (its fall ends a packet), 91 the JVS's lines.
 *
 * GPIO 3 is the MIE's 93C46: bit 4 DI, 5 CS, 6 CLK. GPIO 5 the board's switches
 * (SW1 bits 0-3), TEST bit 4, SERVICE bit 5 (active low), the EEPROM's DO bit 7.
 */

enum {
	CTRL_TXB	= 0x000001,
	CTRL_CTXB	= 0x000002,
	CTRL_ENDP	= 0x000004,
	CTRL_EMP	= 0x000100,
	CTRL_BFOV	= 0x000800,
	CTRL_RXB	= 0x001000,
	CTRL_RFB	= 0x002000,
	CTRL_TFB	= 0x004000,
	CTRL_HRES	= 0x008000,
};

/* A reply, about 20 microseconds: within the next line */
static const int REPLY_LINES = 1;

/* The hash of a program's pieces as sent (FNV-1a over each piece's 1Ch
 * bytes, in order: sim_frame ()); Crazy Taxi's */
static const uint32_t FNV_START = 0x811C9DC5;
static const uint32_t CRAZY_TAXI_PROGRAM = 0x0A714E36;
/* The simulation's state in the Z80's RAM (save_state ()) */
static const uint32_t SIM_MARK = 0x4D49533F;

/* HR_LOG_MIE=1: each Maple frame the SH-4 sends the MIE and its reply, the
 * words in hex, with the frame they came at (to compare the MIE's program
 * with its simulation) */
static bool
log_mie ()
{
	static const bool on = getenv ("HR_LOG_MIE") != NULL;

	return on;
}

static void
log_words (const char *what, unsigned frame, const uint32_t *w, uint32_t n)
{
	char text[16 * 9 + 1];
	size_t at = 0;

	for (uint32_t i = 0; i < n && i < 16; i++)
		at += (size_t) snprintf (text + at, sizeof (text) - at, " %08X", w[i]);
	text[at] = 0;
	HL_PRINT ("mie %s %u:%s", what, frame, text);
}

Mie::Mie (Board *board_, hl::Buffer *rom_) :
	hl::Device (board_->machine),
	board (board_),
	z80 (new Z80 (this))
{
	simulated = !hl::Config::get ()->mie_program;
	idle_skip = hl_util_get_bool_option ("MIE_IDLE_SKIP", true);
	memcpy (rom, rom_->ptr (0), sizeof (rom));
	/* The ROM's test of its RAM, which the SH-4's code does not wait for
	 * (after a reset), left out, as MAME does: c3 d8 01 */
	rom[0x144] = 0xC3;
	rom[0x145] = 0xD8;
	rom[0x146] = 0x01;
	rom[0x147] = 0x00;
}

Mie::~Mie ()
{
}

void
Mie::reset (hl::ResetType type)
{
	(void) type;
	memset (&s, 0, sizeof (s));
	memset (ram, 0, sizeof (ram));
	s.gpio_dir = 0xFF;
	s.irq_level = 0xFF;
	z80->reset ();
	eeprom.power_on ();
	idle.valid = false;
	/* A state's mode kept until a reset: then the configured one */
	simulated = !hl::Config::get ()->mie_program;
	memset (&sim, 0, sizeof (sim));
	sim.hash = FNV_START;
}

void
Mie::update_irq ()
{
	z80->set_irq ((s.irq_enable & s.irq_pending & 0x7F) != 0);
}

void
Mie::raise_irq (int level)
{
	if (level >= 0 && level < 7) {
		s.irq_pending |= 1 << level;
		idle.touched = true;
		update_irq ();
	}
}

uint8_t
Mie::acknowledge ()
{
	int level;

	for (level = 0; level < 7; level++)
		if ((s.irq_enable & s.irq_pending) & (1 << level))
			break;
	idle.touched = true;
	if (level == 7)
		return 0xFF;
	s.irq_pending &= ~(1 << level);
	update_irq ();
	return (uint8_t) (0xF2 + 2 * level);
}

void
Mie::reply_timer ()
{
	idle.touched = true;
	if (s.control & CTRL_RXB) {
		s.control &= ~CTRL_RXB;
		s.control |= CTRL_RFB;
		raise_irq (s.irq_level);
	}
	if (s.control & (CTRL_TXB | CTRL_CTXB)) {
		board->holly->maple_end_of_reply ();
		s.lreg -= s.reply_size;
		if (s.reply_partial) {
			s.control &= ~CTRL_CTXB;
			s.control |= CTRL_EMP;
		} else {
			s.control &= ~(CTRL_TXB | CTRL_CTXB);
			s.control |= CTRL_TFB | CTRL_EMP;
		}
	}
	if (s.control & CTRL_HRES)
		raise_irq (s.irq_level);
}

int
Mie::exec (int t_states)
{
	if (simulated) {
		if (s.timer > 0 && !--s.timer)
			sim_deliver ();
		return t_states;
	}
	if (s.timer > 0 && !--s.timer)
		reply_timer ();
	s.owed += t_states;
	if (s.owed > 0) {
		if (idle_skip) {
			int left = s.owed;

			run_z80 (left);
			s.owed = left;
		} else
			s.owed -= z80->run (s.owed);
	}
	return t_states;
}

/* All of the Z80 but R and the cycles count */
static bool
same_state (const Z80::Registers &a, const Z80::Registers &b)
{
	return a.af.w == b.af.w && a.bc.w == b.bc.w && a.de.w == b.de.w &&
	       a.hl.w == b.hl.w && a.af2.w == b.af2.w && a.bc2.w == b.bc2.w &&
	       a.de2.w == b.de2.w && a.hl2.w == b.hl2.w && a.ix.w == b.ix.w &&
	       a.iy.w == b.iy.w && a.pc == b.pc && a.sp == b.sp && a.wz == b.wz &&
	       a.i == b.i && a.im == b.im && a.iff1 == b.iff1 && a.iff2 == b.iff2 &&
	       a.halted == b.halted && a.ei_delay == b.ei_delay &&
	       a.irq_line == b.irq_line && a.nmi_pending == b.nmi_pending;
}

/* The Z80's T-states owed, its idle loops at once. Back at a loop's head as
 * it was there the last time, with nothing written, no port read or written,
 * no interrupt raised or taken, R not read, and no Maple frame from the SH-4
 * in between, the Z80 runs the same turn again, and again, until one of them
 * happens: the whole turns that fit are counted at once, R and the cycles as
 * they would go, the rest run as usual. Exact: the same machine either way */
void
Mie::run_z80 (int &left)
{
	while (left > 0) {
		uint16_t pc = z80->regs.pc;

		if (idle.confirmed && pc == idle.head && !idle.touched &&
		    !z80->r_read && left > idle.t && same_state (z80->regs, idle.regs)) {
			int turns = (left - 1) / idle.t;

			left -= turns * idle.t;
			z80->regs.cycles += (uint64_t) turns * idle.t;
			z80->regs.r = (uint8_t) ((z80->regs.r & 0x80) |
			                         ((z80->regs.r + turns * idle.r) & 0x7F));
		}
		left -= z80->step ();
		/* A jump back: a loop's head, perhaps */
		if (z80->regs.pc < pc && pc - z80->regs.pc <= 64)
			came_back ();
	}
}

void
Mie::came_back ()
{
	if (idle.valid && z80->regs.pc == idle.head && !idle.touched &&
	    !z80->r_read && same_state (z80->regs, idle.regs)) {
		/* The first turn back: its length; the next ones go by it */
		if (!idle.confirmed) {
			idle.t = (int) (z80->regs.cycles - idle.regs.cycles);
			idle.r = (uint8_t) ((z80->regs.r - idle.regs.r) & 0x7F);
			idle.confirmed = idle.t > 0;
		}
		return;
	}
	idle.valid = true;
	idle.confirmed = false;
	idle.touched = false;
	idle.head = z80->regs.pc;
	idle.regs = z80->regs;
	z80->r_read = false;
}

uint8_t
Mie::read (uint16_t addr)
{
	if (addr < 0x0800)
		return rom[addr];
	if (addr < 0x7000)
		return 0xFF;
	if (addr < 0x7800) {
		unsigned offs = addr & 0x3F;

		if (offs < 3)
			return (uint8_t) (s.control >> (8 * offs));
		if (offs == 3)
			return (uint8_t) s.lreg;
		if (offs < 0x24) {
			offs -= 4;
			return (uint8_t) (s.tbuf[offs >> 2] >> (8 * (offs & 3)));
		}
		return 0;
	}
	return ram[addr - 0x7800];
}

void
Mie::write (uint16_t addr, uint8_t val)
{
	idle.touched = true;
	if (addr < 0x7000)
		return;
	if (addr < 0x7800) {
		unsigned offs = addr & 0x3F;

		if (offs < 3) {
			uint32_t prev = s.control;

			s.control = (s.control & ~(255u << (8 * offs))) | ((uint32_t) val << (8 * offs));
			if ((!(prev & CTRL_TXB) && (s.control & CTRL_TXB)) ||
			    (!(prev & CTRL_CTXB) && (s.control & CTRL_CTXB))) {
				s.control &= ~(CTRL_TFB | CTRL_RXB | CTRL_RFB | CTRL_BFOV | CTRL_EMP);
				s.reply_size = s.lreg + 1;
				if (s.reply_size > TBUF_SIZE)
					s.reply_size = TBUF_SIZE;
				memcpy (s.reply, s.tbuf, s.reply_size * 4);
				s.reply_partial = !(s.control & CTRL_ENDP);
				if (log_mie ())
					log_words (s.reply_partial ? "<+" : "<", board->holly->frames, s.reply, s.reply_size);
				s.timer = REPLY_LINES;
			}
		} else if (offs == 3) {
			s.lreg = val;
		} else if (offs < 0x24) {
			unsigned shift;

			offs -= 4;
			shift = (offs & 3) * 8;
			s.tbuf[offs >> 2] = (s.tbuf[offs >> 2] & ~(255u << shift)) | ((uint32_t) val << shift);
		}
		return;
	}
	ram[addr - 0x7800] = val;
}

/* A GPIO port's value: its latch where it drives, else its inputs */
uint8_t
Mie::in_port_value (unsigned p) const
{
	if (!(s.gpio_dir & (1 << p)))
		return s.gpio[p];
	if (p == 5)
		return (switches & (monitor_15khz ? 0x0E : 0x0F)) |
		       (hl::GameControls::get ().pressed ("Test") ? 0 : 0x10) |
		       (hl::GameControls::get ().pressed ("Service") ? 0 : 0x20) | 0x40 |
		       (eeprom.data_out () ? 0x80 : 0);
	return 0xFF;
}

uint8_t
Mie::in (uint16_t port)
{
	unsigned p = port & 0xFF;

	/* The inputs, the JVS bus: they change */
	idle.touched = true;
	if (p < 8)
		return in_port_value (p);
	switch (p) {
	case 0x08:
		return s.gpio_dir;
	case 0x10: {
		int byte;

		if (s.jvs_lcr & 0x80)
			return 0;
		byte = board->jvs->receive ();
		return byte < 0 ? 0 : (uint8_t) byte;
	}
	case 0x15:
		return 0x60 | (board->jvs->can_receive () ? 1 : 0);
	case 0x30:
		return s.irq_enable;
	case 0x50:
		return s.irq_level;
	case 0x70:
		return s.irq_pending;
	case 0x91:
		return 0x8C | (board->jvs->all_addressed () ? 2 : 0) | (board->jvs->present () ? 0 : 1);
	default:
		return 0;
	}
}

void
Mie::out (uint16_t port, uint8_t val)
{
	unsigned p = port & 0xFF;

	idle.touched = true;
	if (p < 8) {
		s.gpio[p] = val;
		if (p == 3 && !(s.gpio_dir & 8))
			eeprom.write_lines ((val & 0x20) != 0, (val & 0x40) != 0, (val & 0x10) != 0);
		return;
	}
	switch (p) {
	case 0x08:
		s.gpio_dir = val;
		if (!(val & 8))
			eeprom.write_lines ((s.gpio[3] & 0x20) != 0, (s.gpio[3] & 0x40) != 0, (s.gpio[3] & 0x10) != 0);
		break;
	case 0x10:
		if (!(s.jvs_lcr & 0x80))
			board->jvs->send (val);
		break;
	case 0x13:
		s.jvs_lcr = val;
		break;
	case 0x30:
		s.irq_enable = val;
		update_irq ();
		break;
	case 0x50:
		s.irq_level = val;
		break;
	case 0x70:
		s.irq_pending = val;
		update_irq ();
		break;
	case 0x90:
		if ((s.jvs_control & 1) && !(val & 1))
			board->jvs->end_packet ();
		s.jvs_control = val;
		break;
	default:
		break;
	}
}

void
Mie::maple_write (const uint32_t *data, uint32_t words)
{
	idle.touched = true;
	if (log_mie ())
		log_words (">", board->holly->frames, data, words);
	if (simulated) {
		sim_frame (data, words);
		return;
	}
	memcpy (s.tbuf, data, (words > TBUF_SIZE ? TBUF_SIZE : words) * 4);
	s.lreg = words - 1;
	s.control &= ~(CTRL_HRES | CTRL_TXB | CTRL_TFB | CTRL_RFB | CTRL_BFOV);
	s.control |= CTRL_RXB;
	s.timer = REPLY_LINES;
}

void
Mie::maple_read (uint32_t *data, uint32_t *words, bool *partial)
{
	memcpy (data, s.reply, s.reply_size * 4);
	*words = s.reply_size;
	*partial = s.reply_partial != 0;
}

void
Mie::maple_reset ()
{
	idle.touched = true;
	if (log_mie ())
		HL_PRINT ("mie reset %u", board->holly->frames);
	if (simulated) {
		/* The answer going on dropped */
		sim.out_n = sim.out_at = 0;
		s.timer = 0;
		return;
	}
	s.control &= ~(CTRL_RXB | CTRL_TXB | CTRL_TFB | CTRL_RFB | CTRL_BFOV);
	s.control |= CTRL_HRES;
	s.timer = REPLY_LINES;
}

/*
 * The MIE Simulated
 * =================
 *
 * Without its program run (MIE_PROGRAM false), the SH-4's Maple frames are
 * answered here as the MIE's ROM and the programs the BIOS and the games
 * send it answer them (seen through HR_LOG_MIE with the program run): a
 * frame's words, its first the command, the destination, the source and the
 * words that follow (low byte first); the answer the same, the destination
 * and the source swapped, given eight words at a time, a line each.
 *
 *  01		a device's status: FD, ready (05 with Crazy Taxi's program;
 *		the programs' FC, busy after 86 01 or an EEPROM's write, left
 *		out)
 *  03		reset: 07
 *  80		a piece of the program: its address (bytes 2-3, big endian),
 *		18h bytes from byte 4 (1Ch when byte 0 is FF), answered 81 with
 *		its first word, its low byte the sum of its 1Ch bytes; FFFFFFFF
 *		ends it: 07
 *  82		the ID: two frames 83, "315-6149    COPYRIGHT SEGA E", then
 *		"NTERPRISES CO,LTD.  " (and the 8-word buffer's rest, "1998")
 *  86		the JVS's commands, byte 4 their code, answered 87 (none:
 *		the program answers its buffer's 40h words, FD here):
 *    01	02 00 00 00 (its header counting two words, one sent)
 *    03	the EEPROM's 128 bytes, from address byte 5
 *    0B	write byte 6's count from address byte 5, the data from byte
 *		8: its first word back
 *    13	store node byte 5's request repeated (byte 6 its length, the
 *		bytes from 7): 14, the lines, length + 1, 0
 *    15	the channel byte 5's answers: 16 FF FF FF, the last request's
 *		byte 2 then FF FF FF, 0, 0,
 *		then 0, the channel, the lines, and each answer (its node, 0,
 *		its length, its bytes, E0 then unescaped), the bytes' count over
 *		4 and one words;
 *		none: 32 FF FF FF, 00 FF FF FF, 0, 0, 0
 *    17 19 21	a request to node byte 10 (length byte 11, bytes from 12) on
 *		channel byte 9: alone, after the node's repeated one, before
 *		it; 18, the channel, the lines, 0
 *    27	byte 10's count of requests (node, length, bytes): each before
 *		its node's repeated one; 26, the channel, the lines, 0
 *    31	the GPIO ports 0-2 and 4-6: 32 and ports 0-2, 0 and ports 4-6,
 *		0, 0, then 00 96 23 96 (the program's count over port 0F, the
 *		one most often seen)
 * Crazy Taxi's program (its hash) swaps 13 and 17, its answers without their
 * node, its lines always 8E. The lines: port 91's, a device there and all
 * addressed. The JVS's requests go on the bus as the UART would send them,
 * their answers taken at once (hl::JvsBus).
 */


static void
put_word (uint32_t *out, uint32_t &n, uint32_t w)
{
	if (n < 160)
		out[n++] = w;
}

uint8_t
Mie::sim_sense () const
{
	return 0x8C | (board->jvs->all_addressed () ? 2 : 0) | (board->jvs->present () ? 0 : 1);
}

/* A frame of the answer: the command, the source and destination swapped,
 * the bytes padded to words */
void
Mie::sim_answer (uint8_t cmd, uint32_t head, const uint8_t *bytes, uint32_t len)
{
	uint32_t words = (len + 3) / 4, i;

	put_word (sim.out, sim.out_n, cmd | (((head >> 16) & 0xFF) << 8) | (((head >> 8) & 0xFF) << 16) | (words << 24));
	for (i = 0; i < words; i++) {
		uint32_t w = 0;

		for (unsigned k = 0; k < 4; k++)
			if (i * 4 + k < len)
				w |= (uint32_t) bytes[i * 4 + k] << (8 * k);
		put_word (sim.out, sim.out_n, w);
	}
}

void
Mie::sim_jvs (unsigned node, unsigned channel, const uint8_t *req, unsigned len, int repeat)
{
	hl::JvsBus *bus = board->jvs;
	std::vector<uint8_t> body;
	uint8_t sum;
	unsigned i;

	if (repeat < 0 && node >= 1 && node <= 32 && sim.repeat[node - 1][0])
		body.insert (body.end (), &sim.repeat[node - 1][1], &sim.repeat[node - 1][1] + sim.repeat[node - 1][0]);
	body.insert (body.end (), req, req + len);
	if (repeat > 0 && node >= 1 && node <= 32 && sim.repeat[node - 1][0])
		body.insert (body.end (), &sim.repeat[node - 1][1], &sim.repeat[node - 1][1] + sim.repeat[node - 1][0]);
	if (body.empty () || body.size () > 250)
		return;

	/* E0, the node, the length, the bytes, the sum; E0 and D0 escaped */
	bus->flush_receive ();
	bus->send (0xE0);
	sum = (uint8_t) node + (uint8_t) (body.size () + 1);
	auto put = [&] (uint8_t b) {
		if (b == 0xE0 || b == 0xD0) {
			bus->send (0xD0);
			bus->send ((uint8_t) (b - 1));
		} else
			bus->send (b);
	};
	put ((uint8_t) node);
	put ((uint8_t) (body.size () + 1));
	for (uint8_t b : body) {
		put (b);
		sum += b;
	}
	put (sum);
	bus->end_packet ();

	/* Its answer as the program keeps it: E0, then the bytes unescaped
	 * (D0 and the byte less one: the byte) */
	std::vector<uint8_t> answer;
	int b;

	while ((b = bus->receive ()) >= 0) {
		if (b == 0xD0 && !answer.empty ()) {
			int next = bus->receive ();

			if (next < 0)
				break;
			b = (next + 1) & 0xFF;
		}
		answer.push_back ((uint8_t) b);
	}
	if (answer.empty ())
		return;
	channel &= 31;
	i = sim.rx_len[channel];
	if (i + 3 + answer.size () > sizeof (sim.rx[0]) || answer.size () > 255)
		return;
	if (sim.crazy) {
		/* Crazy Taxi's program: a status, the length, no node */
		sim.rx[channel][i] = 0;
		sim.rx[channel][i + 1] = (uint8_t) answer.size ();
		memcpy (&sim.rx[channel][i + 2], answer.data (), answer.size ());
		sim.rx_len[channel] += (uint32_t) (2 + answer.size ());
		return;
	}
	sim.rx[channel][i] = (uint8_t) node;
	sim.rx[channel][i + 1] = 0;
	sim.rx[channel][i + 2] = (uint8_t) answer.size ();
	memcpy (&sim.rx[channel][i + 3], answer.data (), answer.size ());
	sim.rx_len[channel] += (uint32_t) (3 + answer.size ());
}

void
Mie::sim_receive (unsigned channel, std::vector<uint8_t> &out)
{
	static const uint8_t head[16] = { 0, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF };

	channel &= 31;
	out.assign (head, head + 16);
	out[4] = sim.tag[channel];
	if (!sim.rx_len[channel]) {
		out[0] = 0x32;
		out.insert (out.end (), 4, 0);
		return;
	}
	out[0] = 0x16;
	out.push_back (0);
	out.push_back ((uint8_t) channel);
	out.push_back (sim.crazy ? 0x8E : sim_sense ());
	out.insert (out.end (), sim.rx[channel], sim.rx[channel] + sim.rx_len[channel]);
	/* Its words: the bytes' over 4, and one (a whole word of zeros when
	 * they fill the last) */
	out.resize ((out.size () / 4 + 1) * 4, 0);
	sim.rx_len[channel] = 0;
}

void
Mie::sim_command (uint32_t head, const uint8_t *in, uint32_t len)
{
	uint8_t sub = len ? in[0] : 0, out[8];
	std::vector<uint8_t> bytes;

	if (sim.crazy && (sub == 0x13 || sub == 0x17))
		sub ^= 0x13 ^ 0x17;
	/* The requests' header: the channel, the node, the length, the bytes */
	unsigned channel = len > 5 ? in[5] & 31 : 0, node = len > 6 ? in[6] : 0, n = len > 7 ? in[7] : 0;
	/* A request of no bytes: the repeated one alone */
	const uint8_t *req = len >= 8 ? in + 8 : NULL;

	if (req && 8 + n > len)
		n = len - 8;
	/* A request's byte 2, which the next 15 gives back (in its second
	 * word): the games' own count */
	if (len > 2 && sub != 0x15 && sub != 0x13 && sub != 0x01 && sub != 0x03 && sub != 0x0B && sub != 0x31)
		sim.tag[channel] = in[2];
	switch (sub) {
	case 0x01:
		if (sim.program)
			sim.program = 2;
		out[0] = 2;
		out[1] = out[2] = out[3] = 0;
		sim_answer (0x87, head, out, 4);
		/* The program's header counts two words, sending one */
		sim.out[sim.out_n - 2] += 1u << 24;
		return;
	case 0x03: {
		unsigned at = len > 1 ? in[1] & 127 : 0;

		bytes.assign (eeprom.mem + at, eeprom.mem + 128);
		bytes.resize (128, 0);
		sim_answer (0x87, head, bytes.data (), 128);
		return;
	}
	case 0x0B: {
		unsigned at = len > 1 ? in[1] & 127 : 0, size = len > 2 ? in[2] : 0;

		if (size > 128 - at)
			size = 128 - at;
		if (size > (len > 4 ? len - 4 : 0))
			size = len > 4 ? len - 4 : 0;
		if (size) {
			memcpy (eeprom.mem + at, in + 4, size);
			eeprom.changed = true;
		}
		sim_answer (0x87, head, eeprom.mem, 4);
		return;
	}
	case 0x13: {
		unsigned nd = len > 1 ? in[1] : 0, l = len > 2 ? in[2] : 0;

		if (l > len - 3)
			l = len > 3 ? len - 3 : 0;
		if (nd >= 1 && nd <= 32 && l) {
			sim.repeat[nd - 1][0] = (uint8_t) l;
			memcpy (&sim.repeat[nd - 1][1], in + 3, l);
		}
		out[0] = 0x14;
		out[1] = sim_sense ();
		out[2] = (uint8_t) (l + 1);
		out[3] = 0;
		sim_answer (0x87, head, out, 4);
		return;
	}
	case 0x15:
		sim_receive (len > 1 ? in[1] : 0, bytes);
		sim_answer (0x87, head, bytes.data (), (uint32_t) bytes.size ());
		return;
	case 0x17:
	case 0x19:
	case 0x21:
		sim.rx_len[channel] = 0;
		if (req)
			sim_jvs (node, channel, req, n, sub == 0x17 ? 0 : sub == 0x19 ? -1 : 1);
		out[0] = 0x18;
		out[1] = (uint8_t) channel;
		out[2] = sim_sense ();
		out[3] = 0;
		sim_answer (0x87, head, out, 4);
		return;
	case 0x35:
	case 0x27: {
		if (sub == 0x35) {
			sim_receive (channel, bytes);
			sim_answer (0x87, head, bytes.data (), (uint32_t) bytes.size ());
		}
		unsigned count = len > 6 ? in[6] : 0, at = 7, k;

		sim.rx_len[channel] = 0;
		for (k = 0; k < count && at + 2 <= len; k++) {
			unsigned nd = in[at], l = in[at + 1];

			if (at + 2 + l > len)
				break;
			sim_jvs (nd, channel, in + at + 2, l, 1);
			at += 2 + l;
		}
		out[0] = 0x26;
		out[1] = (uint8_t) channel;
		out[2] = sim_sense ();
		out[3] = 0;
		sim_answer (0x87, head, out, 4);
		return;
	}
	case 0x33:
		sim_receive (channel, bytes);
		sim_answer (0x87, head, bytes.data (), (uint32_t) bytes.size ());
		if (req)
			sim_jvs (node, channel, req, n, 1);
		out[0] = 0x18;
		out[1] = (uint8_t) channel;
		out[2] = sim_sense ();
		out[3] = 0;
		sim_answer (0x87, head, out, 4);
		return;
	case 0x31: {
		uint8_t b[20] = { 0x32 };
		static const unsigned ports[6] = { 0, 1, 2, 4, 5, 6 };

		for (unsigned k = 0; k < 6; k++)
			b[k < 3 ? 1 + k : 2 + k] = in_port_value (ports[k]);
		b[4] = 0;
		b[17] = 0x96;
		b[18] = 0x23;
		b[19] = 0x96;
		sim_answer (0x87, head, b, 20);
		return;
	}
	default:
		HL_ERROR_FEW ("mie: the simulation knows no JVS command %02X", sub);
		sim_answer (0xFD, head, NULL, 0);
		return;
	}
}

void
Mie::sim_frame (const uint32_t *data, uint32_t words)
{
	uint32_t head = data[0], len = std::min ((head >> 24) & 0xFF, words ? words - 1 : 0), i;
	uint8_t bytes[4 * 256 + 4];

	for (i = 0; i < len && i < 256; i++)
		memcpy (&bytes[i * 4], &data[1 + i], 4);
	sim.out_n = sim.out_at = 0;
	switch (head & 0xFF) {
	case 0x01:
		/* Ready: FD; Crazy Taxi's program, 05 (the programs' FC, busy
		 * after 86 01 or an EEPROM's write, left out) */
		sim_answer (sim.crazy ? 0x05 : 0xFD, head, NULL, 0);
		break;
	case 0x03:
		sim.hash = FNV_START;
		sim_answer (0x07, head, NULL, 0);
		break;
	case 0x80:
		if (len >= 1 && data[1] == 0xFFFFFFFF) {
			/* The program sent: which one, by its hash */
			sim.program = 1;
			sim.crazy = sim.hash == CRAZY_TAXI_PROGRAM;
			HL_PRINT ("mie: a program of %08X sent%s", sim.hash, sim.crazy ? " (Crazy Taxi's)" : "");
			sim.hash = FNV_START;
			sim_answer (0x07, head, NULL, 0);
		} else if (len >= 7) {
			uint8_t sum = 0, w[4];

			for (i = 0; i < 0x1C; i++) {
				sum += bytes[i];
				sim.hash = (sim.hash ^ bytes[i]) * 0x01000193;
			}
			memcpy (w, bytes, 4);
			w[0] = sum;
			sim_answer (0x81, head, w, 4);
		} else
			sim_answer (0xFD, head, NULL, 0);
		break;
	case 0x82: {
		static const char id[] = "315-6149    COPYRIGHT SEGA ENTERPRISES CO,LTD.  1998    ";

		sim_answer (0x83, head, (const uint8_t *) id, 28);
		sim_answer (0x83, head, (const uint8_t *) id + 28, 20);
		/* The program's eight-word buffer: its last two words too */
		put_word (sim.out, sim.out_n, 0x38393931);
		put_word (sim.out, sim.out_n, 0x20202020);
		break;
	}
	case 0x86:
		if (len)
			sim_command (head, bytes, len * 4);
		else
			sim_answer (0xFD, head, NULL, 0);
		break;
	default:
		sim_answer (0xFD, head, NULL, 0);
		break;
	}
	if (sim.out_n)
		s.timer = REPLY_LINES;
}

/* The next eight words of the answer, as the program sends them */
void
Mie::sim_deliver ()
{
	uint32_t n = std::min (sim.out_n - sim.out_at, (uint32_t) TBUF_SIZE);

	if (!n)
		return;
	memcpy (s.reply, sim.out + sim.out_at, n * 4);
	s.reply_size = n;
	sim.out_at += n;
	s.reply_partial = sim.out_at < sim.out_n;
	if (log_mie ())
		log_words (s.reply_partial ? "<+" : "<", board->holly->frames, s.reply, s.reply_size);
	board->holly->maple_end_of_reply ();
	if (s.reply_partial)
		s.timer = REPLY_LINES;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Mie::save_state (hl::State *state)
{
	int ret = 0;

	/* Simulated, the Z80's RAM holds the simulation's state, after a mark:
	 * the states' layout the same either way */
	static_assert (sizeof (sim) + 4 <= sizeof (ram), "the simulation's state in the Z80's RAM");
	if (simulated) {
		memcpy (ram, &SIM_MARK, 4);
		memcpy (ram + 4, &sim, sizeof (sim));
	}
	SAVE (s);
	SAVE (ram);
	SAVE (z80->regs);
	ret |= eeprom.save_state (state);
	return ret;
}

int
Mie::load_state (hl::State *state)
{
	int ret = 0;

	LOAD (s);
	LOAD (ram);
	LOAD (z80->regs);
	ret |= eeprom.load_state (state);
	/* The state's mode until the next reset: the simulation's state behind
	 * its mark, else the program in the Z80's RAM */
	uint32_t mark;
	bool was = simulated;

	memcpy (&mark, ram, 4);
	simulated = mark == SIM_MARK;
	if (simulated)
		memcpy (&sim, ram + 4, sizeof (sim));
	if (simulated != was)
		HL_PRINT ("mie: a state of the MIE %s, so until the game's reset",
		          simulated ? "simulated" : "running its program");
	idle.valid = false;
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
