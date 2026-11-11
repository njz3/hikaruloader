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

#include <cstring>

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

Mie::Mie (Board *board_, hl::Buffer *rom_) :
	hl::Device (board_->machine),
	board (board_),
	z80 (new Z80 (this))
{
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
	if (level == 7)
		return 0xFF;
	s.irq_pending &= ~(1 << level);
	update_irq ();
	return (uint8_t) (0xF2 + 2 * level);
}

void
Mie::reply_timer ()
{
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
	if (s.timer > 0 && !--s.timer)
		reply_timer ();
	s.owed += t_states;
	if (s.owed > 0)
		s.owed -= z80->run (s.owed);
	return t_states;
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

uint8_t
Mie::in (uint16_t port)
{
	unsigned p = port & 0xFF;

	if (p < 8) {
		if (!(s.gpio_dir & (1 << p)))
			return s.gpio[p];
		if (p == 5)
			return (switches & (monitor_15khz ? 0x0E : 0x0F)) |
			       (hl::GameControls::get ().pressed ("Test") ? 0 : 0x10) |
			       (hl::GameControls::get ().pressed ("Service") ? 0 : 0x20) | 0x40 |
			       (eeprom.data_out () ? 0x80 : 0);
		return 0xFF;
	}
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
	s.control &= ~(CTRL_RXB | CTRL_TXB | CTRL_TFB | CTRL_RFB | CTRL_BFOV);
	s.control |= CTRL_HRES;
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
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
