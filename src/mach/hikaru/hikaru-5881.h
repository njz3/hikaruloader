/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/315-5881_crypt.cpp and .h, Copyright
 * (C) Andreas Naive, Olivier Galibert, David Haywood (BSD-3-Clause, see
 * hikaru-5881.cpp).
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

#ifndef __HL_HIKARU_5881_H__
#define __HL_HIKARU_5881_H__

#include <stdint.h>

namespace hikaru {

/* Sega's 315-5881, the ROM boards' decryption chip (NAOMI's "M2" and "M3"
 * cartridges, Model 3's boards, some ST-V carts): a stream cipher in
 * counter mode on a 16-bit block cipher, and a decompression. It reads its
 * 16-bit words through read (addr: the word's address, the counter); see
 * hikaru-5881.cpp. Its state is plain data, for the save states. */
class Crypt5881 {
public:
	typedef uint16_t (*Read) (void *opaque, uint32_t addr);

	void	 reset ();
	void	 set_addr_low (uint16_t data);
	void	 set_addr_high (uint16_t data);
	void	 set_subkey (uint16_t data);
	/* The next word, as a little endian CPU reads it (its bytes the other
	 * way round: AIRTRIX swaps them back, @0C0440C0); the first after an
	 * address is a dummy one (0) */
	uint16_t read_le ();

	/* The game's key */
	uint32_t key;
	/* The NAOMI's cartridges: their addresses have a high half (the
	 * Hikaru's boards take it as 0), and no dummy word starts the stream */
	bool	 high_address = false;
	bool	 dummy_first = true;
	Read	 read_cb;
	void	*opaque;

	enum { LINE_SIZE = 512 };
	struct State {
		uint32_t cur_address;
		uint16_t subkey, dec_hist;
		uint32_t dec_header;
		uint8_t	 enc_ready, first_read;
		int32_t	 buffer_pos, line_buffer_pos, line_buffer_size;
		int32_t	 buffer_bit, buffer_bit2;
		uint8_t	 buffer[2], buffer2[2];
		uint16_t buffer2a;
		int32_t	 block_size, block_pos, block_numlines, done_compression;
		/* The lines, the current one at line_cur */
		uint8_t	 lines[2][LINE_SIZE];
		uint8_t	 line_cur;
	} s;

private:
	uint16_t do_decrypt ();
	uint16_t get_decrypted_16 ();
	int	 get_compressed_bit ();
	void	 enc_start ();
	void	 enc_fill ();
	void	 line_fill ();
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_5881_H__ */
