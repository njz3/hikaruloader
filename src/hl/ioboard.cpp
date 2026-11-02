/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cabinet/io_837_13551.cpp, io_837_13741.cpp,
 * src/odin/machine/core/io_line.hpp, src/odin/machine/chips/d71054.cpp and
 * src/odin/machine/chips/mie.cpp, Copyright (C) 2026, Réda Chérif-Touil.
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

#include "hl/ioboard.h"
#include "hl/state.h"

namespace hl {

/* The inputs read again every millisecond of the board's time: the programs
 * look at their switches every few (315-6215's tick is 4.44 ms) */
static const uint64_t SAMPLE_TICKS = 9216000;
/* A coin closes its switch 50 ms, then leaves it open 50 ms: 315-6215 counts
 * a closing of 13 ms to 4.4 s (06F9) */
static const uint64_t COIN_TICKS = 460800000;

/* Coins */

bool
IoBoardCoins::closed (unsigned slot, uint64_t at, uint64_t length)
{
	if (phase[slot] && at >= until[slot]) {
		phase[slot] = (phase[slot] == 1) ? 2 : 0;
		until[slot] = at + length;
	}
	if (!phase[slot] && waiting[slot]) {
		waiting[slot]--;
		phase[slot] = 1;
		until[slot] = at + length;
	}
	return phase[slot] == 1;
}

/* 837-13551 */

/* A pin of the TMP90PH44 and what drives it: bit bit of the system byte
 * (word 0) or of player 1's or 2's switches (word 1, 2), closed at 1 there */
static const struct {
	uint8_t	 port, mask;
	uint8_t	 word, bit;
} pins_13551[] = {
	/* The tables 307C-3177; the bits of the reply from 12F8 (the system
	 * byte's bit 7 FF3C, bit 6 FF3D) */
	{ 2, 0x01, 1, 15 }, { 2, 0x02, 1, 10 }, { 2, 0x04, 1, 11 }, { 2, 0x08, 1, 13 },
	{ 2, 0x10, 1, 12 }, { 2, 0x20, 1, 9 }, { 2, 0x40, 1, 8 }, { 2, 0x80, 1, 7 },
	{ 1, 0x01, 1, 6 }, { 1, 0x02, 1, 5 }, { 1, 0x04, 1, 4 }, { 1, 0x08, 1, 3 },
	{ 1, 0x10, 1, 14 }, { 1, 0x20, 0, 7 },
	{ 0, 0x01, 2, 15 }, { 0, 0x02, 2, 10 }, { 0, 0x04, 2, 11 }, { 0, 0x08, 2, 13 },
	{ 0, 0x10, 2, 12 }, { 0, 0x20, 2, 9 }, { 0, 0x40, 2, 8 }, { 0, 0x80, 2, 7 },
	{ 4, 0x01, 2, 6 }, { 4, 0x02, 2, 5 }, { 4, 0x04, 2, 4 }, { 4, 0x08, 2, 3 },
	{ 4, 0x10, 2, 14 }, { 4, 0x20, 0, 6 },
};
/* 3178 and 318B: the coin switches of slots 1 and 2 */
static const uint8_t coin_port_13551[2] = { 1, 4 };

IoBoard13551::IoBoard13551 (const uint8_t *rom, size_t size, JvsBus *bus, SegaIoBoard *cabinet) :
	bus (bus),
	cabinet (cabinet),
	chip (rom, size, this)
{
	memset (&s, 0, sizeof (s));
	reset ();
}

void
IoBoard13551::reset ()
{
	memset (&s, 0, sizeof (s));
	chip.reset ();
	s.sampled = now ();
	read_pins ();
}

/* The switches as the cabinet has them, the coins as they go through, the
 * analog channels */
void
IoBoard13551::read_pins ()
{
	uint16_t words[3];
	unsigned presses[2], slot, i;

	words[0] = cabinet->system_bits ();
	words[1] = cabinet->player_bits (0);
	words[2] = cabinet->player_bits (1);
	cabinet->take_coins (presses);

	memset (s.pins, 0xFF, sizeof (s.pins));
	s.pins[6] = s.pins[7] = 0x0F;
	for (i = 0; i < NUMELEM (pins_13551); i++)
		if (words[pins_13551[i].word] & (1u << pins_13551[i].bit))
			s.pins[pins_13551[i].port] &= (uint8_t) ~pins_13551[i].mask;
	for (slot = 0; slot < 2; slot++) {
		s.coins.waiting[slot] += presses[slot];
		if (s.coins.closed (slot, now (), COIN_TICKS))
			s.pins[coin_port_13551[slot]] &= (uint8_t) ~0x40;
	}
	for (i = 0; i < 8; i++)
		s.analog[i] = (uint8_t) (cabinet->analog_bits (i) >> 8);
}

void
IoBoard13551::run_until (uint64_t tick)
{
	if (now () >= s.sampled + SAMPLE_TICKS) {
		s.sampled = now ();
		read_pins ();
	}
	while (now () < tick) {
		uint64_t start = bus->line_next_byte ();
		uint64_t due = (start == UINT64_MAX) ? UINT64_MAX :
		               start + Tmp90ph44::RX_STATES * TICKS_PER_STATE;
		uint64_t to;

		if (due <= now ()) {
			chip.serial_received (bus->line_take_byte ());
			continue;
		}
		to = MIN2 (tick, due);
		chip.run ((unsigned) ((to - now () + TICKS_PER_STATE - 1) / TICKS_PER_STATE));
	}
}

void
IoBoard13551::port_out (unsigned port, uint8_t latch, uint8_t driven)
{
	s.latch[port] = latch;
	s.driven[port] = driven;
	if (port == 3) {
		bool addressed = (driven & 0x04) && !(latch & 0x04);

		if (addressed != (s.addressed != 0)) {
			s.addressed = addressed;
			bus->line_event (now (), true, addressed);
		}
	} else if (port == 6 || port == 7) {
		uint8_t on = 0;
		unsigned n;

		for (n = 0; n < 6; n++) {
			unsigned p = (n < 3) ? 6 : 7;
			uint8_t bit = (uint8_t) (2u << (n % 3));

			if ((s.driven[p] & bit) && !(s.latch[p] & bit))
				on |= (uint8_t) (0x80 >> n);
		}
		if (on != s.outputs) {
			s.outputs = on;
			cabinet->drive_outputs (&on, 1);
		}
	}
}

uint8_t
IoBoard13551::analog_pin (unsigned n)
{
	return s.analog[(s.latch[5] & 0x40) ? n : n + 4];
}

/* A byte reaches the MIE while the board drives the line */
void
IoBoard13551::serial_sent (uint8_t byte)
{
	if ((s.driven[3] & 0x10) && !(s.latch[3] & 0x10))
		bus->line_event (now (), false, byte);
}

int
IoBoard13551::save_state (State *state)
{
	int ret = chip.save_state (state);
	ret |= state->put (&s, sizeof (s));
	return ret;
}

int
IoBoard13551::load_state (State *state)
{
	int ret = chip.load_state (state);
	ret |= state->get (&s, sizeof (s));
	return ret;
}

const char *
IoBoard13551::debug_string ()
{
	static char text[96];

	snprintf (text, sizeof (text), "TMP90PH44 PC %04X, outputs %02X",
	          chip.cpu ().pc (), s.outputs);
	return text;
}

/* D71054 */

void
D71054::reset ()
{
	memset (this, 0, sizeof (*this));
	next_pulse = UINT64_MAX;
}

/* Counter 0 in mode 2 clocked by counter 2 in mode 3: a pulse every count 0
 * x count 2 clocks (a count of 0 is 65536); 0 when not so set */
uint64_t
D71054::period () const
{
	uint64_t n0, n2;

	if (!loaded[0] || !loaded[2] || mode[0] != 2 || mode[2] != 3)
		return 0;
	n0 = count[0] ? count[0] : 65536;
	n2 = count[2] ? count[2] : 65536;
	return n0 * n2;
}

void
D71054::write (uint8_t reg, uint8_t val, uint64_t now)
{
	unsigned counter;

	if (reg == 3) {
		uint8_t m;

		counter = val >> 6;
		/* The read-back (counter 3) and the latch (RW 00): not used */
		if (counter == 3 || !(val & 0x30)) {
			unknown = 1;
			return;
		}
		/* LSB then MSB, binary; modes 6 and 7 are 2 and 3 */
		m = (val >> 1) & 7;
		if (m > 5)
			m = (uint8_t) (m - 4);
		if ((val & 0x30) != 0x30 || (val & 1) || (counter == 0 && m != 2) || (counter == 2 && m != 3))
			unknown = 1;
		mode[counter] = m;
		high_next[counter] = 0;
		loaded[counter] = 0;
		if (counter == 0 || counter == 2)
			next_pulse = UINT64_MAX;
		return;
	}
	counter = reg & 3;
	if (!high_next[counter]) {
		count[counter] = (uint16_t) ((count[counter] & 0xFF00) | val);
		high_next[counter] = 1;
		return;
	}
	count[counter] = (uint16_t) ((count[counter] & 0x00FF) | (val << 8));
	high_next[counter] = 0;
	loaded[counter] = 1;
	if ((counter == 0 || counter == 2) && period ())
		next_pulse = now + period ();
}

void
D71054::pulse_taken ()
{
	if (next_pulse != UINT64_MAX)
		next_pulse += period ();
}

/* 837-13741 */

/* The ports of the 315-6146 (MAME's mie.cpp, mie_port) */
enum {
	PORT_GPIO_H		= 0x07,
	PORT_GPIO_DIR		= 0x08,
	PORT_SERIAL		= 0x09,
	PORT_SERIAL_END		= 0x0E,
	PORT_ADC		= 0x0F,
	PORT_UART1		= 0x10,
	PORT_UART1_END		= 0x17,
	PORT_UART2		= 0x20,
	PORT_UART2_END		= 0x27,
	PORT_IRQ_ENABLE		= 0x30,
	PORT_MAPLE_IRQLEVEL	= 0x50,
	PORT_IRQ_PENDING	= 0x70,
	PORT_JVS_CONTROL	= 0x90,
	PORT_JVS_SENSE		= 0x91,
	PORT_TIMER		= 0xA0,
	PORT_TIMER_END		= 0xA3
};

/* A 16550's registers, from the UART's base, and their bits */
enum {
	UART_RBR = 0, UART_IER = 1, UART_IIR = 2, UART_LCR = 3, UART_MCR = 4,
	UART_LSR = 5, UART_MSR = 6, UART_SCR = 7,

	LCR_DLAB = 0x80,
	FCR_ENABLE = 0x01, FCR_CLEAR_RX = 0x02, FCR_CLEAR_TX = 0x04,
	LSR_DR = 0x01, LSR_THRE = 0x20, LSR_TEMT = 0x40,
	IER_RX = 0x01, IER_THRE = 0x02,
	IIR_NONE = 0x01, IIR_THRE = 0x02, IIR_RX = 0x04, IIR_FIFO = 0xC0
};

/* Port 30's bits: the levels let through (UART 1 is level 3), the board's
 * input (bit 7); bit 6 grants the bus to a host, none here */
enum {
	IRQ_UART1	= 0x08,
	IRQ_EXTERNAL	= 0x80,
	IRQ_SOURCES	= 0xBF
};

IoBoard13741::IoBoard13741 (const uint8_t *rom, size_t size, JvsBus *bus, SegaIoBoard *cabinet) :
	bus (bus),
	cabinet (cabinet),
	z80 (this),
	eprom (rom, rom + size)
{
	memset (&s, 0, sizeof (s));
	memset (told, 0, sizeof (told));
	reset ();
}

/* As MAME's mie.cpp resets the chip; its RAM cleared too, as the program
 * tests it first (16F5). The time goes on */
void
IoBoard13741::reset ()
{
	memset (&s, 0, sizeof (s));
	s.gpio_dir = 0xFF;
	s.maple_irqlevel = 0xFF;
	s.timer.reset ();
	z80.reset ();
	memset (&idle, 0, sizeof (idle));
	s.sampled = now ();
	read_pins ();
}

void
IoBoard13741::read_pins ()
{
	uint8_t system = cabinet->system_bits ();
	uint16_t player[2] = { cabinet->player_bits (0), cabinet->player_bits (1) };
	unsigned presses[2], slot, k;

	cabinet->take_coins (presses);
	memset (s.pins, 0xFF, sizeof (s.pins));
	/* The DIP switches off, the meters wired, the coins, TEST, TILT */
	s.pins[0] = 0xF3;
	if (system & 0x80)
		s.pins[0] &= (uint8_t) ~0x40;
	if (system & 0x40)
		s.pins[0] &= (uint8_t) ~0x80;
	for (k = 0; k < 8; k++) {
		if ((player[0] >> (15 - k)) & 1)
			s.pins[1] &= (uint8_t) ~(1u << k);
		if ((player[1] >> (15 - k)) & 1)
			s.pins[2] &= (uint8_t) ~(1u << k);
	}
	for (k = 0; k < 4; k++) {
		if ((player[0] >> (7 - k)) & 1)
			s.pins[6] &= (uint8_t) ~(1u << k);
		if ((player[1] >> (7 - k)) & 1)
			s.pins[6] &= (uint8_t) ~(1u << (4 + k));
	}
	for (slot = 0; slot < 2; slot++) {
		s.coins.waiting[slot] += presses[slot];
		if (s.coins.closed (slot, now (), COIN_TICKS))
			s.pins[0] &= (uint8_t) ~(0x10u << slot);
	}
	for (k = 0; k < 8; k++)
		s.analog[k] = (uint8_t) (cabinet->analog_bits (k) >> 8);
}

/* Up to tick, the host's bytes a character after their start bit, the
 * timer's pulses at their time */
void
IoBoard13741::run_until (uint64_t tick)
{
	if (now () >= s.sampled + SAMPLE_TICKS) {
		s.sampled = now ();
		read_pins ();
	}
	while (now () < tick) {
		uint64_t start = bus->line_next_byte ();
		uint64_t due = (start == UINT64_MAX) ? UINT64_MAX : start + uart_byte_ticks ();
		uint64_t pulse = (s.timer.next_pulse == UINT64_MAX) ? UINT64_MAX :
		                 s.timer.next_pulse * TICKS_PER_Z80_CYCLE;
		uint64_t to;

		if (due <= now ()) {
			rx_push (bus->line_take_byte ());
			continue;
		}
		if (pulse <= now ()) {
			idle.touched = true;
			s.external_irq = 1;
			s.timer.pulse_taken ();
			continue;
		}
		to = MIN2 (tick, MIN2 (due, pulse));
		run_chip ((to - now () + TICKS_PER_Z80_CYCLE - 1) / TICKS_PER_Z80_CYCLE);
	}
}

/* All of the Z80 but R and the cycles */
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

/* The Z80 for cycles at least, its idle loop at once, as the MIE's
 * (Mie::run_z80): epr-21868 waits for the JVS line at 03A2-041A, reading
 * UART 1's LSR, between its timer's interrupts. Back at the loop's head as it
 * was there the last time, with no byte of the memory changed, no port but
 * the LSR read, the LSR the same, nothing on its way out of the UART and no
 * interrupt taken, the turns that fit before the next event (run_until's
 * end: a byte, a pulse of the timer) are counted at once, R and the cycles
 * as they would go */
void
IoBoard13741::run_chip (uint64_t cycles)
{
	uint64_t end = z80.regs.cycles + cycles;

	while (z80.regs.cycles < end) {
		uint16_t pc = z80.regs.pc;
		uint64_t from;
		int t;

		z80.set_irq ((s.irq_enable & IRQ_SOURCES) && irq_line ());
		if (idle.confirmed && pc == idle.head && !idle.touched && !z80.r_read &&
		    !s.tx_ticks && !s.tx_count && uart_lsr () == idle.lsr &&
		    end - z80.regs.cycles > (uint64_t) idle.t && same_state (z80.regs, idle.regs)) {
			uint64_t turns = (end - z80.regs.cycles - 1) / (uint64_t) idle.t;

			z80.regs.cycles += turns * (uint64_t) idle.t;
			z80.regs.r = (uint8_t) ((z80.regs.r & 0x80) |
			                        ((z80.regs.r + turns * idle.r) & 0x7F));
		}
		from = now ();
		t = z80.step ();
		uart_tick (from, (uint64_t) t * TICKS_PER_Z80_CYCLE);
		/* A jump back: a loop's head, perhaps */
		if (z80.regs.pc < pc && pc - z80.regs.pc <= 64)
			came_back ();
	}
}

void
IoBoard13741::came_back ()
{
	if (idle.valid && z80.regs.pc == idle.head && !idle.touched &&
	    !z80.r_read && uart_lsr () == idle.lsr && same_state (z80.regs, idle.regs)) {
		/* The first turn back: its length; the next ones go by it */
		if (!idle.confirmed) {
			idle.t = (int) (z80.regs.cycles - idle.regs.cycles);
			idle.r = (uint8_t) ((z80.regs.r - idle.regs.r) & 0x7F);
			idle.confirmed = idle.t > 0;
		}
		return;
	}
	idle.valid = true;
	idle.confirmed = false;
	idle.touched = false;
	idle.head = z80.regs.pc;
	idle.regs = z80.regs;
	idle.lsr = uart_lsr ();
	z80.r_read = false;
}

/* The memory: the EPROM below 8000, the RAM above */
uint8_t
IoBoard13741::read (uint16_t addr)
{
	if (addr >= 0x8000)
		return s.ram[addr - 0x8000];
	return (addr < eprom.size ()) ? eprom[addr] : 0xFF;
}

void
IoBoard13741::write (uint16_t addr, uint8_t val)
{
	/* The stack's CALLs write the same return addresses at each turn */
	if (addr >= 0x8000 && s.ram[addr - 0x8000] != val) {
		s.ram[addr - 0x8000] = val;
		idle.touched = true;
	}
}

/* The interrupts */

bool
IoBoard13741::uart_irq () const
{
	return ((s.ier & IER_RX) && s.rx_count) || ((s.ier & IER_THRE) && s.thre_irq);
}

/* The levels 0 to 6 that ask and are let through */
uint8_t
IoBoard13741::levels () const
{
	return (uint8_t) (s.irq_enable & (s.irq_pending | (uart_irq () ? IRQ_UART1 : 0)) & 0x7F);
}

bool
IoBoard13741::irq_line () const
{
	return levels () || ((s.irq_enable & IRQ_EXTERNAL) && s.external_irq);
}

/* The lowest level first (MAME's mie.cpp irq_callback); UART 1 asks as long
 * as its cause holds; then the board's input */
uint8_t
IoBoard13741::acknowledge ()
{
	uint8_t asking = levels ();
	unsigned level;

	idle.touched = true;
	for (level = 0; level < 7; level++)
		if (asking & (1u << level)) {
			s.irq_pending &= (uint8_t) ~(1u << level);
			return (uint8_t) (0xF2 + 2 * level);
		}
	if ((s.irq_enable & IRQ_EXTERNAL) && s.external_irq) {
		s.external_irq = 0;
		return 0xFE;
	}
	return 0xFF;
}

/* The ports */

void
IoBoard13741::unknown_port (char rw, uint8_t port, uint8_t val)
{
	bool &said = told[(rw == 'o' ? 256 : 0) + port];

	if (!said)
		HL_PRINT ("837-13741: port %02X %s %02X (unknown), PC %04X", port,
		          rw == 'o' ? "written" : "read", val, z80.regs.pc);
	said = true;
}

uint8_t
IoBoard13741::in (uint16_t port16)
{
	uint8_t port = (uint8_t) port16;

	/* The LSR changes at the events only, which end an idle loop's turns */
	if (port != PORT_UART1 + UART_LSR)
		idle.touched = true;
	if (port <= PORT_GPIO_H)
		return (s.gpio_dir & (1u << port)) ? s.pins[port] : s.gpio[port];
	if (port >= PORT_UART1 && port <= PORT_UART1_END)
		return uart_read ((uint8_t) (port - PORT_UART1));
	if (port >= PORT_UART2 && port <= PORT_UART2_END)
		/* The second UART, nothing on it: idle, no byte */
		return (port - PORT_UART2 == UART_LSR) ? (LSR_THRE | LSR_TEMT) : 0;
	switch (port) {
	case PORT_GPIO_DIR:
		return s.gpio_dir;
	case PORT_ADC:
		/* The channels one after the other from 0 (139B) */
		return s.analog[s.adc_channel++ & 7];
	case PORT_IRQ_ENABLE:
		return s.irq_enable;
	case PORT_MAPLE_IRQLEVEL:
		return s.maple_irqlevel;
	case PORT_IRQ_PENDING:
		return s.irq_pending;
	case PORT_JVS_SENSE:
		/* No board after it, its DIP switches off */
		return 0xFF;
	default:
		unknown_port ('i', port, 0xFF);
		return 0xFF;
	}
}

void
IoBoard13741::out (uint16_t port16, uint8_t val)
{
	uint8_t port = (uint8_t) port16;

	idle.touched = true;
	if (port <= PORT_GPIO_H) {
		gpio_write (port, val);
		return;
	}
	if (port >= PORT_UART1 && port <= PORT_UART1_END) {
		uart_write ((uint8_t) (port - PORT_UART1), val);
		return;
	}
	if (port >= PORT_UART2 && port <= PORT_UART2_END)
		return;
	if (port >= PORT_TIMER && port <= PORT_TIMER_END) {
		bool known = !s.timer.unknown;

		s.timer.write ((uint8_t) (port & 3), val, z80.regs.cycles);
		if (known && s.timer.unknown)
			HL_ERROR ("837-13741: the D71054 set in a way not run (%02X to port %02X)", val, port);
		return;
	}
	switch (port) {
	case PORT_GPIO_DIR:
		gpio_set_dir (val);
		break;
	case PORT_ADC:
		s.adc_channel = 0;
		break;
	case PORT_IRQ_ENABLE:
		s.irq_enable = val;
		break;
	case PORT_MAPLE_IRQLEVEL:
		s.maple_irqlevel = val;
		break;
	case PORT_IRQ_PENDING:
		s.irq_pending = val;
		break;
	case PORT_JVS_CONTROL: {
		/* Bit 1: the line that tells the MIE the board has its
		 * address */
		bool addressed = (val & 0x02) != 0;

		s.jvs_control = val;
		if (addressed != (s.addressed != 0)) {
			s.addressed = addressed;
			bus->line_event (now (), true, addressed);
		}
		break;
	}
	default:
		unknown_port ('o', port, val);
		break;
	}
}

/* An output reads back its latch; the ports turned outputs drive it */
void
IoBoard13741::gpio_write (uint8_t port, uint8_t val)
{
	s.gpio[port] = val;
	if (port >= 3 && port <= 5)
		drive_outputs ();
}

void
IoBoard13741::gpio_set_dir (uint8_t dir)
{
	s.gpio_dir = dir;
	drive_outputs ();
}

/* Output n of JVS from its pin, on at 0 while the chip drives the port:
 * outputs 1-6 port 3 bits 2-7, 7-14 port 4, 15-22 port 5 */
void
IoBoard13741::drive_outputs ()
{
	uint8_t on[3] = { 0, 0, 0 };
	unsigned n;

	for (n = 1; n <= 22; n++) {
		unsigned port = (n <= 6) ? 3 : (n <= 14) ? 4 : 5;
		unsigned bit = (n <= 6) ? n + 1 : (n <= 14) ? n - 7 : n - 15;

		if (!(s.gpio_dir & (1u << port)) && !(s.gpio[port] & (1u << bit)))
			on[(n - 1) / 8] |= (uint8_t) (0x80 >> ((n - 1) % 8));
	}
	if (memcmp (on, s.outputs, sizeof (on))) {
		memcpy (s.outputs, on, sizeof (on));
		cabinet->drive_outputs (on, 3);
	}
}

/* UART 1: characters take their time on the line, its divisor's (the
 * program sets 8 at 01A1: 115200 bauds from 14.7456 MHz) */
uint64_t
IoBoard13741::uart_byte_ticks () const
{
	uint64_t divisor = (uint64_t) ((s.dlm << 8) | s.dll);

	if (!divisor)
		divisor = 1;
	return 10 * 16 * TICKS_PER_UART_CLOCK * divisor;
}

void
IoBoard13741::rx_push (uint8_t byte)
{
	idle.touched = true;
	/* Lost when the FIFO is full */
	if (s.rx_count < sizeof (s.rx_fifo)) {
		s.rx_fifo[(s.rx_head + s.rx_count) % sizeof (s.rx_fifo)] = byte;
		s.rx_count++;
	}
}

uint8_t
IoBoard13741::uart_lsr () const
{
	uint8_t v = 0;

	if (s.rx_count)
		v |= LSR_DR;
	if (!s.tx_count) {
		v |= LSR_THRE;
		if (!s.tx_ticks)
			v |= LSR_TEMT;
	}
	return v;
}

uint8_t
IoBoard13741::uart_read (uint8_t reg)
{
	switch (reg) {
	case UART_RBR:
		if (s.lcr & LCR_DLAB)
			return s.dll;
		if (s.rx_count) {
			uint8_t v = s.rx_fifo[s.rx_head];
			s.rx_head = (s.rx_head + 1) % sizeof (s.rx_fifo);
			s.rx_count--;
			return v;
		}
		return 0;
	case UART_IER:
		return (s.lcr & LCR_DLAB) ? s.dlm : s.ier;
	case UART_IIR: {
		/* Received data first, then the transmitter, whose request this
		 * read takes (a 16550) */
		uint8_t fifo = (s.fcr & FCR_ENABLE) ? IIR_FIFO : 0;

		if ((s.ier & IER_RX) && s.rx_count)
			return (uint8_t) (IIR_RX | fifo);
		if ((s.ier & IER_THRE) && s.thre_irq) {
			s.thre_irq = 0;
			return (uint8_t) (IIR_THRE | fifo);
		}
		return (uint8_t) (IIR_NONE | fifo);
	}
	case UART_LCR:
		return s.lcr;
	case UART_MCR:
		return s.mcr;
	case UART_LSR:
		return uart_lsr ();
	case UART_MSR:
		return 0;
	default:
		return s.scr;
	}
}

void
IoBoard13741::uart_write (uint8_t reg, uint8_t val)
{
	switch (reg) {
	case UART_RBR:
		if (s.lcr & LCR_DLAB)
			s.dll = val;
		else {
			s.thre_irq = 0;
			if (s.tx_count < sizeof (s.tx_fifo))
				s.tx_fifo[s.tx_count++] = val;
		}
		break;
	case UART_IER:
		if (s.lcr & LCR_DLAB)
			s.dlm = val;
		else {
			/* Enabled while it is empty, the transmitter asks at once */
			if ((val & IER_THRE) && !(s.ier & IER_THRE) && !s.tx_count)
				s.thre_irq = 1;
			s.ier = val;
		}
		break;
	case UART_IIR:
		s.fcr = val;
		if (val & FCR_CLEAR_RX)
			s.rx_head = s.rx_count = 0;
		if (val & FCR_CLEAR_TX)
			s.tx_count = 0;
		break;
	case UART_LCR:
		s.lcr = val;
		break;
	case UART_MCR:
		s.mcr = val;
		break;
	case UART_LSR:
	case UART_MSR:
		break;
	default:
		s.scr = val;
		break;
	}
}

/* The transmitter over ticks from from: the shift register, then the FIFO.
 * A byte reaches the MIE a character after it starts */
void
IoBoard13741::uart_tick (uint64_t from, uint64_t ticks)
{
	uint64_t byte_ticks = uart_byte_ticks ();
	uint64_t left = ticks;

	while (left) {
		if (s.tx_ticks) {
			uint64_t step = MIN2 (s.tx_ticks, left);
			s.tx_ticks -= step;
			left -= step;
		} else if (s.tx_count) {
			s.tx_byte = s.tx_fifo[0];
			memmove (s.tx_fifo, s.tx_fifo + 1, (size_t) (s.tx_count - 1));
			s.tx_count--;
			s.tx_ticks = byte_ticks;
			if (!s.tx_count)
				s.thre_irq = 1;
			bus->line_event (from + (ticks - left) + byte_ticks, false, s.tx_byte);
		} else
			break;
	}
}

int
IoBoard13741::save_state (State *state)
{
	int ret = state->put (&z80.regs, sizeof (z80.regs));
	ret |= state->put (&s, sizeof (s));
	return ret;
}

int
IoBoard13741::load_state (State *state)
{
	int ret = state->get (&z80.regs, sizeof (z80.regs));
	ret |= state->get (&s, sizeof (s));
	idle.valid = idle.confirmed = false;
	return ret;
}

const char *
IoBoard13741::debug_string ()
{
	static char text[96];

	snprintf (text, sizeof (text), "Z80 PC %04X, outputs %02X %02X %02X",
	          z80.regs.pc, s.outputs[0], s.outputs[1], s.outputs[2]);
	return text;
}

} /* namespace hl */
