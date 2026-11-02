/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/chips/tmp90ph44.hpp, Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __HL_TMP90PH44_H__
#define __HL_TMP90PH44_H__

#include "hl/core.h"
#include "cpu/tlcs90/tlcs90.h"

namespace hl {

class State;

/* The Toshiba TMP90PH44: a TMP90C844 with 16 KB of PROM at 0000 and 512
 * bytes of RAM at FDC0, around a TLCS-90 CPU; the CPU of the JVS I/O board
 * 837-13551, its PROM the program 315-6215. No data sheet of the TMP90C844
 * was found: its registers (FFC0-FFFF) are where MAME has them; each block
 * works as the TMP90C840's of the 1988 data book where the register is the
 * same, and as 315-6215 uses it otherwise:
 *
 *  - Prescaler: fc/4 into 9 stages; the 8-bit timers count phiT1 (fc/8),
 *    phiT16 (fc/128) or phiT256 (fc/2048). Timer n counts while TRUN bit n
 *    and the prescaler's bit 5 are set, is cleared when it matches TREGn,
 *    and then asks for its interrupt (timers 1 and 3: 0 and 2 are not used).
 *  - Watchdog: INTWD every 2^(14 + 2 WDTP) periods of fc; 4E in WDCR
 *    clears it, B1 with WDTE clear stops it.
 *  - Interrupts: a flag a vector, cleared when taken or when IRFL is
 *    written with the vector / 8.
 *  - Serial channel: an 8-bit UART at the JVS line's rate; a byte received
 *    while SCBUF holds one not read is lost (OERR).
 *  - A/D converter: AN0 to AN3 (P50-P53) in FFF0-FFF3.
 *
 * A setting nothing tells the meaning of is told once in the log. Times are
 * in states, two periods of fc. */
class Tmp90ph44 : private Tlcs90::Bus {
public:
	static const unsigned ROM_SIZE = 0x4000;
	/* The states a byte takes on the line: 10 bits at fc / 128 a bit */
	static const unsigned BYTE_STATES = 640;
	/* From the start of a byte on the line to its INTRX: the middle of its
	 * last data bit, where the UART samples it */
	static const unsigned RX_STATES = 544;

	/* What the board around the chip gives it */
	class Board {
	public:
		virtual ~Board () {}
		/* The pins of port 0 to 7 */
		virtual uint8_t	 port_pins (unsigned port) = 0;
		/* The program wrote port 0 to 7 or its control register: its
		 * latch, and the bits that drive their pins. Until then, after
		 * the reset, no pin is driven */
		virtual void	 port_out (unsigned port, uint8_t latch, uint8_t driven) = 0;
		/* Analog input AN0 to AN3: 00 at AGND, FF at VREF */
		virtual uint8_t	 analog_pin (unsigned n) = 0;
		/* A byte has left on the serial line, its stop bit ended */
		virtual void	 serial_sent (uint8_t byte) = 0;
	};

	/* rom: the PROM's 16 KB, copied */
	Tmp90ph44 (const uint8_t *rom, size_t size, Board *board);

	/* The reset pin */
	void	 reset ();
	/* Runs instructions and the devices for states at least; the states
	 * it ran */
	unsigned run (unsigned states);
	/* The data bits of a byte on the serial line sampled: RX_STATES after
	 * its start */
	void	 serial_received (uint8_t byte);

	uint64_t states () const { return s.states; }
	/* For the tests: a byte as the CPU reads it, the registers as written */
	uint8_t	 peek (uint16_t addr) const;
	Tlcs90	&cpu () { return tlcs90; }

	int	 save_state (State *state);
	int	 load_state (State *state);

private:
	enum {
		VEC_WD = 0x20, VEC_T1 = 0x28, VEC_T3 = 0x38, VEC_RX = 0x60, VEC_TX = 0x68
	};

	uint8_t	 read (uint16_t addr) override;
	void	 write (uint16_t addr, uint8_t val) override;
	uint16_t interrupt (bool maskable) override;

	uint8_t	 sfr_read (uint8_t reg);
	void	 sfr_write (uint8_t reg, uint8_t val);
	void	 tell_port (unsigned port);
	uint8_t	 p67_driven (unsigned port) const;
	void	 sync ();
	uint64_t next_event () const;
	unsigned timer_period (unsigned n) const;
	unsigned ticks_to_match (unsigned n) const;
	unsigned watchdog_period () const;
	void	 request (unsigned vector) { s.requests |= 1u << (vector >> 3); }
	bool	 requested (unsigned vector) const { return (s.requests & (1u << (vector >> 3))) != 0; }
	void	 unknown (const char *what, unsigned value) const;

	Tlcs90	 tlcs90;
	Board	*board;
	uint8_t	 rom[ROM_SIZE];

	/* Plain data, for the states */
	struct {
		Tlcs90::Registers cpu;
		uint8_t	 ram[0x200];
		uint8_t	 sfr[0x40];	/* FFC0-FFFF as written */
		uint8_t	 timer[4];
		uint64_t states;
		uint64_t prescaler;	/* the states since the prescaler runs */
		uint32_t watchdog;	/* the states since it was cleared */
		uint32_t requests;	/* a bit a vector / 8 */
		int32_t	 tx_left;	/* the states until the byte is sent, -1 none */
		uint8_t	 tx, rx;
		uint8_t	 rx_full;	/* SCBUF not read since its byte came */
		uint8_t	 rx_errors;	/* SCCR bits 4-2 */
	} s;
	/* The devices are up to date at this time; the first time one asks
	 * for an interrupt */
	uint64_t synced, next;
	/* What unknown () told already */
	mutable bool told;
};

} /* namespace hl */

#endif /* __HL_TMP90PH44_H__ */
