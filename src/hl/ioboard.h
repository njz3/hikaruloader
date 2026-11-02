/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cabinet/io_837_13551.hpp, io_837_13741.hpp,
 * src/odin/machine/core/io_line.hpp, src/odin/machine/chips/d71054.hpp and
 * src/odin/machine/chips/mie.hpp, Copyright (C) 2026, Réda Chérif-Touil.
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

#ifndef __HL_IOBOARD_H__
#define __HL_IOBOARD_H__

#include <vector>

#include "hl/jvs.h"
#include "hl/tmp90ph44.h"
#include "cpu/z80/z80.h"

namespace hl {

/* The JVS I/O boards running their own programs (the [Emulation]
 * IO_BOARD_PROGRAM setting), at the far end of the MIE's line (JvsLine),
 * instead of the simulated SegaIoBoard: what the game reads in its JVS and
 * INPUT TESTs (the board's name, its functions, the switches) is then the
 * board's own answer. The cabinet's inputs and outputs still go through the
 * simulated board (SegaIoBoard::player_bits, drive_outputs...), read again
 * every millisecond of the board's time; a coin closes its switch 50 ms,
 * then leaves it open 50 ms before the next. Their programs come from MAME's
 * sets: 315-6215.bin (jvs13551), epr-21868.ic7 (18wheelr). */

/* A coin slot's switch as the board sees it: a coin closes it for a while,
 * then leaves it open as long before the next */
struct IoBoardCoins {
	uint8_t	 phase[2];	/* 0 idle, 1 closed, 2 open */
	uint64_t until[2];
	uint32_t waiting[2];

	/* Whether the slot's switch is closed at tick at */
	bool	 closed (unsigned slot, uint64_t at, uint64_t length);
};

/* The Sega 837-13551 "I/O BD JVS": its TMP90PH44 running 315-6215. Its
 * switches are those the program's tables name (3080-31C0), closed at 0:
 * player 1 on P2 and P1, player 2 on P0 and P4, TEST on P1 bit 5; a coin
 * closes P1 bit 6 (slot 1) or P4 bit 6 (slot 2); the coin meters' return
 * pins read 1, wired. AN0-AN3 are the channels 1-4 while P56 is 1, 5-8 while
 * it is 0 (127E, 0A46). P6 bits 1-3 and P7 bits 1-3 drive the outputs 1-6,
 * on at 0 (12BA). P3 bit 4 at 0 drives the line; P3 bits 0-1 read 3 (no board
 * after it); P3 bit 2 at 0 tells the MIE the board has its address (1450,
 * 1471). Its 14.7456 MHz crystal is taken as the MIE's UART's: a state of the
 * chip is 1250 ticks */
class IoBoard13551 : public JvsLine, private Tmp90ph44::Board {
public:
	static const uint64_t TICKS_PER_STATE = 1250;

	IoBoard13551 (const uint8_t *rom, size_t size, JvsBus *bus, SegaIoBoard *cabinet);

	const char *name () const override { return "837-13551"; }
	void	 reset () override;
	void	 run_until (uint64_t tick) override;
	uint64_t now () const override { return chip.states () * TICKS_PER_STATE; }
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;

private:
	uint8_t	 port_pins (unsigned port) override { return s.pins[port & 7]; }
	void	 port_out (unsigned port, uint8_t latch, uint8_t driven) override;
	uint8_t	 analog_pin (unsigned n) override;
	void	 serial_sent (uint8_t byte) override;
	void	 read_pins ();

	JvsBus	*bus;
	SegaIoBoard *cabinet;
	Tmp90ph44 chip;
	/* Plain data, for the states */
	struct {
		/* The inputs as read last, and when */
		uint8_t	 pins[8], analog[8];
		uint64_t sampled;
		uint8_t	 latch[8], driven[8];
		uint8_t	 addressed;	/* as P3 bit 2 drives it */
		uint8_t	 outputs;	/* outputs 1-6, in bits 7-2, on at 1 */
		IoBoardCoins coins;
	} s;
};

/* The NEC D71054, an 8254's three counters, as the 837-13741 uses them
 * (epr-21868 0120-0145): counter 2 in mode 3 divides the clock, counter 0 in
 * mode 2 divides its output, which is the 315-6146's interrupt input. That
 * wiring and its clock (the Z80's 16 MHz: 16 MHz / 1000 / 16 is the 1 ms the
 * program's numbers aim at), and the first pulse a whole period after counter
 * 0 is loaded, are guesses. In the board's clocks */
struct D71054 {
	uint16_t count[3];
	uint8_t	 mode[3];
	uint8_t	 high_next[3];	/* the next write is the MSB */
	uint8_t	 loaded[3];
	uint64_t next_pulse;	/* UINT64_MAX: none */
	/* A setting it does not run */
	uint8_t	 unknown;

	void	 reset ();
	/* Registers 0-2 the counters, 3 the control word, at clock now */
	void	 write (uint8_t reg, uint8_t val, uint64_t now);
	/* The next pulse of counter 0 given: the one after */
	void	 pulse_taken ();

private:
	uint64_t period () const;
};

/* The Sega 837-13741 "I/O CONTROL BD2", NASCAR Arcade's: a 315-6146 (a Z80
 * at 16 MHz, as the main board's MIE) running epr-21868 from 0000-7FFF, 32 KB
 * of RAM at 8000-FFFF, the D71054 at ports A0-A3, its UART 1 on the JVS line.
 * As the program reads and drives them (found by running it): port 0 bits 6
 * and 7 TEST and TILT, 4 and 5 the coin switches, 2 and 3 the coin meters'
 * return pins (wired: 0), 0 and 1 DIP switches (off: 1); port 1 player 1's
 * first eight switches in JVS order, port 6 bits 0-3 its next four; port 2
 * and port 6 bits 4-7 player 2's; closed at 0. The outputs 1-6 on port 3
 * bits 2-7, 7-14 on port 4, 15-22 on port 5 (the drive board), on at 0. The
 * converter (port 0F) gives the eight analog channels, one after the other.
 * Port 91 reads FF: no board after it, the DIP switches off (configuration 0,
 * NASCAR's manual). Port 90 bit 1 tells the MIE the board has its address.
 * Its interrupts: levels 0-6 at the vectors F2 + 2n (UART 1 is level 3), the
 * timer's input at FE, enabled by port 30 bit 7 (its routine at 0828) */
class IoBoard13741 : public JvsLine, private Z80::Bus {
public:
	static const size_t EPROM_SIZE = 0x10000;

	IoBoard13741 (const uint8_t *rom, size_t size, JvsBus *bus, SegaIoBoard *cabinet);

	const char *name () const override { return "837-13741"; }
	void	 reset () override;
	void	 run_until (uint64_t tick) override;
	uint64_t now () const override { return z80.regs.cycles * TICKS_PER_Z80_CYCLE; }
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;

private:
	uint8_t	 read (uint16_t addr) override;
	void	 write (uint16_t addr, uint8_t val) override;
	uint8_t	 in (uint16_t port) override;
	void	 out (uint16_t port, uint8_t val) override;
	uint8_t	 acknowledge () override;

	void	 read_pins ();
	void	 run_chip (uint64_t cycles);
	void	 came_back ();
	bool	 uart_irq () const;
	uint8_t	 levels () const;
	bool	 irq_line () const;
	uint8_t	 uart_read (uint8_t reg);
	void	 uart_write (uint8_t reg, uint8_t val);
	uint8_t	 uart_lsr () const;
	uint64_t uart_byte_ticks () const;
	void	 uart_tick (uint64_t from, uint64_t ticks);
	void	 rx_push (uint8_t byte);
	void	 gpio_write (uint8_t port, uint8_t val);
	void	 gpio_set_dir (uint8_t dir);
	void	 drive_outputs ();
	void	 unknown_port (char rw, uint8_t port, uint8_t val);

	JvsBus	*bus;
	SegaIoBoard *cabinet;
	Z80	 z80;
	std::vector<uint8_t> eprom;
	bool	 told[512];
	/* Its idle loop (see run_chip): a head the Z80 came back to, as it
	 * was there; a turn found the same; something since that may change
	 * the next turn; a turn's T-states and R's count; the LSR at the head */
	struct {
		uint16_t head;
		bool	 valid, confirmed, touched;
		Z80::Registers regs;
		int	 t;
		uint8_t	 r, lsr;
	} idle;
	/* Plain data, for the states */
	struct {
		/* The inputs as read last, and when */
		uint8_t	 pins[8], analog[8];
		uint64_t sampled;
		uint8_t	 ram[0x8000];
		uint8_t	 gpio[8], gpio_dir;
		uint8_t	 irq_enable, irq_pending, maple_irqlevel, jvs_control;
		uint8_t	 adc_channel, external_irq;
		/* UART 1, a 16550 */
		uint8_t	 dll, dlm, ier, fcr, lcr, mcr, scr;
		uint8_t	 tx_fifo[16], tx_count, tx_byte, thre_irq;
		uint64_t tx_ticks;	/* the byte in the shift register's left */
		uint8_t	 rx_fifo[64];
		uint32_t rx_head, rx_count;
		D71054	 timer;
		uint8_t	 addressed;
		uint8_t	 outputs[3];
		IoBoardCoins coins;
	} s;
};

} /* namespace hl */

#endif /* __HL_IOBOARD_H__ */
