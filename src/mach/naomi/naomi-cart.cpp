/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/naomibd.cpp, naomim1.cpp, naomim2.cpp and
 * naomim4.cpp (license BSD-3-Clause, copyright-holders ElSemi, Olivier
 * Galibert, Andreas Naive).
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

#include "hl/core.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The Cartridge Boards
 * ====================
 *
 * Their registers, 16 bits each, at 005F7000 (words of 32 bits apart):
 *
 *  00, 04	ROM_OFFSET's high and low halves
 *  08		The data at ROM_OFFSET: 2 bytes a read (or written)
 *  0C, 10	DMA_OFFSET's halves, where the G1's DMA reads
 *  14		DMA_COUNT, in 32 bytes
 *  14		M1, read: the Actel's id
 *  34		M4: its id
 *  78, 7C	The board's X76F100 security EEPROM: bit 0 SDA, 1 SCL, 2 CS, 3 RST
 *		written; bit 15 SDA read
 *
 * The offsets' top bits are modes. Bit 31: the address moves on with each
 * read. Bit 29 on an M2 board: 8 MB ROM mapping (else 4 MB); on an M1 board's
 * DMA, clear: through its decryption. Bit 30 on an M4 board: decrypted; on an
 * M2 board, 4001FFF8-E are the 315-5881's registers, its output at 4001FFFE,
 * and 40020000 up its RAM.
 */

enum {
	REG_OFFSETH	= 0x00,
	REG_OFFSETL	= 0x04,
	REG_DATA	= 0x08,
	REG_DMAH	= 0x0C,
	REG_DMAL	= 0x10,
	REG_DMA_COUNT	= 0x14,
	REG_M4_ID	= 0x34,
	REG_BOARDID_W	= 0x78,
	REG_BOARDID_R	= 0x7C,
};

/* The M2's RAM */
static const uint32_t M2_RAM_SIZE = 32768;

static uint16_t
crypt_read_cb (void *opaque, uint32_t addr)
{
	return ((Cart *) opaque)->crypt_read (addr);
}

/* M4's substitution boxes: the 4 of a round */
static const uint8_t m4_sboxes[4][16] = {
	{ 9, 8, 2, 11, 1, 14, 5, 15, 12, 6, 0, 3, 7, 13, 10, 4 },
	{ 2, 10, 0, 15, 14, 1, 11, 3, 7, 12, 13, 8, 4, 9, 5, 6 },
	{ 4, 11, 3, 8, 7, 2, 15, 13, 1, 5, 14, 9, 6, 12, 0, 10 },
	{ 1, 13, 8, 2, 0, 5, 6, 14, 4, 11, 15, 10, 12, 3, 7, 9 }
};

Cart::Cart (hl::Buffer *rom_, Type type_, uint32_t key, uint32_t id, const uint8_t *m4_key) :
	rom_buffer (rom_),
	rom ((const uint8_t *) rom_->ptr (0)),
	rom_size (rom_->size ()),
	type (type_),
	m1_key ((((key & 0xFF) << 24) | ((key & 0xFF00) << 8) | ((key >> 8) & 0xFF00) | (key >> 24))),
	m_id (id),
	buffer (BUFFER_SIZE)
{
	subkey1 = subkey2 = 0;
	memset (&s, 0, sizeof (s));
	if (type == TYPE_M2) {
		m2_ram.resize (M2_RAM_SIZE);
		crypt.key = key;
		crypt.read_cb = crypt_read_cb;
		crypt.opaque = this;
		crypt.high_address = true;
		crypt.dummy_first = false;
	}
	if (type == TYPE_M4) {
		if (m4_key) {
			subkey1 = (m4_key[0x5E2] << 8) | m4_key[0x5E0];
			subkey2 = (m4_key[0x5E6] << 8) | m4_key[0x5E4];
		}
		if (!m_id)
			m_id = 0x5504;
		m4_table.resize (0x10000);
		for (unsigned input = 0; input < 0x10000; input++) {
			uint8_t in[4], out[4] = { 0, 0, 0, 0 };
			uint16_t result = 0;
			int n, i;

			for (n = 0; n < 4; n++)
				in[n] = (input >> (n * 4)) & 15;
			uint8_t aux = in[3];
			for (n = 0; n < 4; n++) {
				aux ^= m4_sboxes[n][in[n]];
				for (i = 0; i < 4; i++)
					out[(n - i) & 3] |= aux & (1 << i);
			}
			for (n = 0; n < 4; n++)
				result |= out[n] << (4 * n);
			m4_table[input] = result;
		}
	}
}

void
Cart::reset ()
{
	memset (&s, 0, sizeof (s));
	if (type == TYPE_M2) {
		std::fill (m2_ram.begin (), m2_ram.end (), 0);
		crypt.reset ();
	}
}

uint16_t
Cart::rom_u16 (uint32_t addr) const
{
	return addr + 1 < rom_size ? (uint16_t) (rom[addr] | (rom[addr + 1] << 8)) : 0xFFFF;
}

uint16_t
Cart::crypt_read (uint32_t addr)
{
	if ((addr & 0xFFFF0000) == 0x01000000) {
		unsigned base = 2 * (addr & 0x7FFF);
		return (uint16_t) (m2_ram[base + 1] | (m2_ram[base] << 8));
	}
	addr *= 2;
	if (addr + 1 >= rom_size)
		return 0xFFFF;
	return (uint16_t) (rom[addr + 1] | (rom[addr] << 8));
}

void
Cart::setup (uint32_t address, bool is_dma)
{
	s.buffer_size = 0;
	switch (type) {
	case TYPE_ROM:
		s.cur = address & 0x1FFFFFFF;
		break;
	case TYPE_M1:
		s.cur = address & 0x1FFFFFFF;
		s.encryption = !(address & 0x20000000) && is_dma;
		if (s.encryption) {
			m1_reset ();
			m1_fill ();
		}
		break;
	case TYPE_M2:
		s.cur = address;
		break;
	case TYPE_M4:
		s.cur = address & 0x1FFFFFFE;
		s.encryption = (s.rom_offset & 0x40000000) != 0;
		if (s.encryption) {
			s.iv = 0;
			s.counter = 0;
			m4_fill ();
		}
		break;
	}
}

void
Cart::read (uint8_t *out, uint32_t size)
{
	while (size) {
		uint32_t n = size;

		if (s.encryption && (type == TYPE_M1 || type == TYPE_M4)) {
			n = std::min (n, s.buffer_size);
			if (!n)
				return;
			memcpy (out, buffer.data (), n);
			if (type == TYPE_M1)
				m1_advance (n);
			else
				m4_advance (n);
		} else if (type == TYPE_M2 && (s.cur & 0x40000000)) {
			uint16_t word = (s.cur == 0x4001FFFE) ? crypt.read_le () : 0xFFFF;

			if (s.cur != 0x4001FFFE)
				s.cur += 2;
			out[0] = (uint8_t) word;
			if (n > 1)
				out[1] = (uint8_t) (word >> 8);
			n = std::min (n, 2u);
		} else if (type == TYPE_M4 && s.cfi && ((s.cur >> 26) & 7) < (m_id & 0x7F)) {
			/* The flash's identification: nothing the games need */
			memset (out, 0, n = std::min (n, 2u));
			s.cur += n;
		} else {
			uint32_t at = s.cur;

			if (type == TYPE_M2) {
				if (s.rom_offset & 0x20000000)
					at = s.cur & 0x1FFFFFFF;
				else {
					at = (s.cur & 0x103FFFFF) | ((s.cur & 0x07C00000) << 1);
					n = std::min (n, 0x00400000 - (at & 0x003FFFFF));
				}
			}
			if (at >= rom_size) {
				memset (out, type == TYPE_M4 ? 0 : 0xFF, n);
			} else {
				n = std::min (n, rom_size - at);
				memcpy (out, rom + at, n);
			}
			s.cur += n;
		}
		out += n;
		size -= n;
	}
}

uint32_t
Cart::get (unsigned reg)
{
	switch (reg) {
	case REG_OFFSETH:
		/* Bit 13 on an M4 board: its security PIC is there, its header
		 * read through the decryption */
		return (s.rom_offset >> 16) | (type == TYPE_M4 ? 0x2000 : 0);
	case REG_OFFSETL:
		return s.rom_offset & 0xFFFF;
	case REG_DATA: {
		uint8_t b[2];
		uint32_t cur = s.cur;

		if (!s.pio_ready || s.last_is_dma) {
			setup (s.rom_offset, false);
			s.pio_ready = 1;
			s.last_is_dma = 0;
			s.dma_ready = 0;
			cur = s.cur;
		}
		if (!(s.rom_offset & 0x80000000) && !(type == TYPE_M2 && (s.cur & 0x40000000))) {
			read (b, 2);
			s.cur = cur;
			if (s.encryption) {
				setup (s.rom_offset, false);
			}
			return b[0] | (b[1] << 8);
		}
		read (b, 2);
		return b[0] | (b[1] << 8);
	}
	case REG_DMA_COUNT:
		return type == TYPE_M1 ? (m_id & 0xFFFF) : 0xFFFF;
	case REG_M4_ID:
		return type == TYPE_M4 ? (m_id & 0xFF80) : 0xFFFF;
	case REG_BOARDID_R:
		return eeprom.sda_read () ? 0x8000 : 0;
	default:
		return 0xFFFF;
	}
}

void
Cart::put (unsigned reg, uint32_t val)
{
	val &= 0xFFFF;
	switch (reg) {
	case REG_OFFSETH:
		s.rom_offset = (s.rom_offset & 0x0000FFFF) | (val << 16);
		s.pio_ready = 0;
		break;
	case REG_OFFSETL:
		s.rom_offset = (s.rom_offset & 0xFFFF0000) | val;
		s.pio_ready = 0;
		break;
	case REG_DATA:
		if (type == TYPE_M2 && (s.rom_offset & 0x40000000)) {
			uint32_t offs = s.rom_offset;

			if (offs & 0x00020000) {
				offs &= M2_RAM_SIZE - 1;
				m2_ram[offs] = (uint8_t) val;
				m2_ram[offs + 1] = (uint8_t) (val >> 8);
			} else {
				switch (offs & 0x1FFFF) {
				case 0x1FFF8: crypt.set_addr_low ((uint16_t) val); break;
				case 0x1FFFA: crypt.set_addr_high ((uint16_t) val); break;
				case 0x1FFFC: crypt.set_subkey ((uint16_t) val); break;
				default: break;
				}
			}
		} else if (type == TYPE_M4) {
			if ((s.rom_offset & 0xFFFF) == 0x00AA && val == 0x0098)
				s.cfi = 1;
			if ((s.rom_offset & 0xFFFF) == 0x0000 && val == 0x00F0)
				s.cfi = 0;
		}
		if (s.rom_offset & 0x80000000)
			s.rom_offset += 2;
		s.pio_ready = 0;
		break;
	case REG_DMAH:
		s.dma_offset = (s.dma_offset & 0x0000FFFF) | (val << 16);
		s.dma_ready = 0;
		break;
	case REG_DMAL:
		s.dma_offset = (s.dma_offset & 0xFFFF0000) | val;
		s.dma_ready = 0;
		break;
	case REG_DMA_COUNT:
		s.dma_count = val;
		break;
	case REG_BOARDID_W:
		eeprom.set_pins ((val >> 2) & 1, (val >> 3) & 1, (val >> 1) & 1, val & 1);
		break;
	default:
		break;
	}
}

void
Cart::dma (uint8_t *out, uint32_t size)
{
	if (!s.dma_ready || !s.last_is_dma) {
		setup (s.dma_offset, true);
		s.dma_ready = 1;
		s.last_is_dma = 1;
		s.pio_ready = 0;
	}
	read (out, size);
}

/* M1: a dictionary compression over 32-bit words */

uint32_t
Cart::m1_word ()
{
	uint8_t a = 0, b = 0, c = 0, d = 0;

	if (s.cur + 4 <= rom_size) {
		a = rom[s.cur];
		b = rom[s.cur + 1];
		c = rom[s.cur + 2];
		d = rom[s.cur + 3];
	}
	s.cur += 4;
	return m1_key ^ (uint32_t) (((b ^ d) << 24) | ((a ^ c) << 16) | (b << 8) | a);
}

uint32_t
Cart::m1_look (int bits)
{
	if (bits > s.avail_bits) {
		s.avail_val = (s.avail_val << 32) | m1_word ();
		s.avail_bits += 32;
	}
	return (uint32_t) (s.avail_val >> (s.avail_bits - bits)) & ((1u << bits) - 1);
}

uint32_t
Cart::m1_get (int bits)
{
	uint32_t res = m1_look (bits);

	s.avail_bits -= bits;
	return res;
}

void
Cart::m1_reset ()
{
	s.avail_val = 0;
	s.avail_bits = 0;
	s.stream_ended = 0;
	s.has_history = 0;
	s.buffer_size = 0;
	for (uint8_t &e : s.dict)
		e = (uint8_t) m1_get (8);
}

void
Cart::m1_put (uint8_t byte)
{
	uint32_t &n = s.buffer_size;

	if (s.dict[0] & 64) {
		if (n < 2)
			buffer[n] = s.has_history ? (uint8_t) (s.hist[n] - byte) : byte;
		else
			buffer[n] = (uint8_t) (buffer[n - 2] - byte);
	} else
		buffer[n] = byte;
	n++;
}

void
Cart::m1_fill ()
{
	while (s.buffer_size < BUFFER_SIZE && !s.stream_ended) {
		switch (m1_look (3)) {
		case 0:
		case 1: {
			s.avail_bits -= 2;
			unsigned addr = m1_get (2);
			m1_put (addr ? s.dict[addr] : (uint8_t) m1_get (8));
			break;
		}
		case 2:
			s.avail_bits -= 3;
			m1_put (s.dict[m1_get (2) + 4]);
			break;
		case 3:
			s.avail_bits -= 3;
			m1_put (s.dict[m1_get (3) + 8]);
			break;
		case 4:
		case 5:
			s.avail_bits -= 2;
			m1_put (s.dict[m1_get (5) + 16]);
			break;
		default: {
			s.avail_bits -= 2;
			unsigned addr = m1_get (6) + 48;
			if (addr == 111)
				s.stream_ended = 1;
			else
				m1_put (s.dict[addr]);
			break;
		}
		}
	}
	while (s.buffer_size < BUFFER_SIZE)
		buffer[s.buffer_size++] = 0;
}

void
Cart::m1_advance (uint32_t size)
{
	if (size < s.buffer_size) {
		memmove (buffer.data (), buffer.data () + size, s.buffer_size - size);
		s.buffer_size -= size;
	} else {
		s.hist[0] = buffer[s.buffer_size - 2];
		s.hist[1] = buffer[s.buffer_size - 1];
		s.has_history = 1;
		s.buffer_size = 0;
	}
	m1_fill ();
}

/* M4: a block cipher, restarted each 16 words */

uint16_t
Cart::m4_round (uint16_t word, uint16_t subkey) const
{
	return m4_table[word ^ subkey] ^ subkey;
}

void
Cart::m4_fill ()
{
	while (s.buffer_size < BUFFER_SIZE) {
		uint16_t enc = rom_u16 (s.cur);
		uint16_t dec = s.iv;

		s.iv = m4_round (enc ^ s.iv, subkey1);
		dec ^= m4_round (s.iv, subkey2);
		buffer[s.buffer_size++] = (uint8_t) dec;
		buffer[s.buffer_size++] = (uint8_t) (dec >> 8);
		s.cur += 2;
		if (++s.counter == 16) {
			s.counter = 0;
			s.iv = 0;
		}
	}
}

void
Cart::m4_advance (uint32_t size)
{
	if (size < s.buffer_size) {
		memmove (buffer.data (), buffer.data () + size, s.buffer_size - size);
		s.buffer_size -= size;
	} else
		s.buffer_size = 0;
	m4_fill ();
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Cart::save_state (hl::State *state)
{
	int ret = 0;

	SAVE (s);
	ret |= state->put (buffer.data (), BUFFER_SIZE);
	if (type == TYPE_M2) {
		SAVE (crypt.s);
		ret |= state->put (m2_ram.data (), M2_RAM_SIZE);
	}
	ret |= eeprom.save_state (state);
	return ret;
}

int
Cart::load_state (hl::State *state)
{
	int ret = 0;

	LOAD (s);
	ret |= state->get (buffer.data (), BUFFER_SIZE);
	if (type == TYPE_M2) {
		LOAD (crypt.s);
		ret |= state->get (m2_ram.data (), M2_RAM_SIZE);
	}
	ret |= eeprom.load_state (state);
	return ret;
}

#undef SAVE
#undef LOAD

} /* namespace naomi */
