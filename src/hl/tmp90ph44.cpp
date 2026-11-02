/*
 * Hikaruloader, Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/chips/tmp90ph44.cpp, Copyright (C) 2026, Réda Chérif-Touil.
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

#include "hl/tmp90ph44.h"
#include "hl/state.h"

namespace hl {

/* The registers, from FFC0 (MAME's tmp90844_regs) */
enum {
	P0 = 0x00, P1 = 0x02, P2 = 0x04, P3 = 0x06, P3CR = 0x07, P4 = 0x08, P5 = 0x0A,
	P6 = 0x0B, P7 = 0x0C, P67CR = 0x0D,
	WDMOD = 0x12, WDCR = 0x13, TREG0 = 0x14, T01MOD = 0x18, T23MOD = 0x19, TRUN = 0x1C,
	SCMOD = 0x26, SCCR = 0x27, SCBUF = 0x28, BRGCR = 0x29, IRFL = 0x2A, IRFH = 0x2B,
	ADMOD = 0x2F, ADREG0 = 0x30, INTEL = 0x34, INTEH = 0x35
};

static const uint16_t RAM_BASE = 0xFDC0;
static const uint16_t SFR_BASE = 0xFFC0;

Tmp90ph44::Tmp90ph44 (const uint8_t *rom, size_t size, Board *board) :
	tlcs90 (this),
	board (board),
	synced (0),
	next (0),
	told (false)
{
	memset (this->rom, 0xFF, sizeof (this->rom));
	memcpy (this->rom, rom, MIN2 (size, sizeof (this->rom)));
	memset (&s, 0, sizeof (s));
	tlcs90.set_fixed (ROM_SIZE);
	reset ();
}

void
Tmp90ph44::unknown (const char *what, unsigned value) const
{
	if (!told)
		HL_ERROR ("TMP90PH44: %s %02X, not known: run as told nowhere", what, value);
	told = true;
}

/* After the data book (TMP90C840, the reset): the timers and the prescaler
 * stop, the watchdog runs (WDTE set, WDTP 00), every port is an input, its
 * latch at 0. The time goes on */
void
Tmp90ph44::reset ()
{
	memset (s.sfr, 0, sizeof (s.sfr));
	s.sfr[WDMOD] = 0x80;
	/* The latch of P3 at 1, as the TMP90C840's (3.5.4), the only one the
	 * data book gives: the program sets P3CR before P3 (00A4), and at 0,
	 * P3 bit 2 would tell the MIE the board has its address for an
	 * instruction */
	s.sfr[P3] = 0xFF;
	s.prescaler = 0;
	memset (s.timer, 0, sizeof (s.timer));
	s.watchdog = 0;
	s.requests = 0;
	s.rx_full = 0;
	s.rx_errors = 0;
	s.tx_left = -1;
	synced = s.states;
	next = next_event ();
	tlcs90.reset ();
}

unsigned
Tmp90ph44::run (unsigned states)
{
	unsigned done = 0;

	while (done < states) {
		unsigned n = tlcs90.step ();

		s.states += n;
		done += n;
		if (s.states >= next)
			sync ();
	}
	sync ();
	return done;
}

/* After the data book (TMP90C840, 3.8.2): the byte goes from the shift
 * register into SCBUF with INTRX, unless SCBUF has not been read since the
 * last one: then the new byte is lost, and OERR set */
void
Tmp90ph44::serial_received (uint8_t byte)
{
	if (!(s.sfr[SCMOD] & 0x20))
		return;
	if (s.rx_full) {
		s.rx_errors |= 0x10;
		return;
	}
	s.rx = byte;
	s.rx_full = 1;
	request (VEC_RX);
}

uint8_t
Tmp90ph44::peek (uint16_t addr) const
{
	if (addr < ROM_SIZE)
		return rom[addr];
	if (addr >= SFR_BASE)
		return s.sfr[addr - SFR_BASE];
	if (addr >= RAM_BASE)
		return s.ram[addr - RAM_BASE];
	return 0xFF;
}

/* Nothing on the external bus of the 837-13551 */
uint8_t
Tmp90ph44::read (uint16_t addr)
{
	if (addr < ROM_SIZE)
		return rom[addr];
	if (addr >= SFR_BASE)
		return sfr_read ((uint8_t) (addr - SFR_BASE));
	if (addr >= RAM_BASE)
		return s.ram[addr - RAM_BASE];
	return 0xFF;
}

void
Tmp90ph44::write (uint16_t addr, uint8_t val)
{
	if (addr >= SFR_BASE)
		sfr_write ((uint8_t) (addr - SFR_BASE), val);
	else if (addr >= RAM_BASE)
		s.ram[addr - RAM_BASE] = val;
}

uint8_t
Tmp90ph44::sfr_read (uint8_t reg)
{
	switch (reg) {
	case P0:
	case P1:
	case P2:
	case P3:
	case P4: {
		/* A bit of the control register after the port drives the pin
		 * (P3CR 9C, P3 bits 0 and 1 read as inputs at 1450); the program
		 * leaves P0, P1, P2 and P4 inputs, the switches and coins its
		 * tables name (3080-31C0) */
		uint8_t out = s.sfr[reg + 1];
		return (uint8_t) ((s.sfr[reg] & out) | (board->port_pins (reg / 2u) & ~out));
	}
	case P6:
	case P7: {
		/* Four bits each */
		unsigned port = (reg == P6) ? 6 : 7;
		uint8_t out = p67_driven (port);
		return (uint8_t) ((s.sfr[reg] & out) | (board->port_pins (port) & ~out & 0x0F));
	}
	case P5:
		/* The latch: the program reads P5 only to change P56 (1296,
		 * 12B0); P50-P53 are its analog inputs */
		return s.sfr[P5];
	case ADREG0:
	case ADREG0 + 1:
	case ADREG0 + 2:
	case ADREG0 + 3:
		/* ADMOD F7 converts AN0 to AN3 over and over into FFF0-FFF3, a
		 * read giving the pin as it is then: the program never starts a
		 * conversion nor waits for one, and reads the four at each tick
		 * (127E), 4.44 ms apart; a conversion of the TMP90C840 takes 190
		 * states */
		if (s.sfr[ADMOD] != 0xF7)
			unknown ("A/D mode", s.sfr[ADMOD]);
		return board->analog_pin (reg - ADREG0);
	case SCCR: {
		/* OERR, bit 4, as serial_received sets it; no parity nor framing
		 * error on the line. The three cleared once read */
		uint8_t val = (uint8_t) ((s.sfr[SCCR] & 0xE3) | s.rx_errors);
		s.rx_errors = 0;
		return val;
	}
	case SCBUF:
		s.rx_full = 0;
		return s.rx;
	case IRFH:
		/* The request flags of the vectors 0018 to 0050, bit n for the
		 * vector (n + 3) * 8: bit 2, 0028, is polled with the interrupts
		 * shut while the delay before a reply runs (05DB, 14AB) */
		return (uint8_t) (s.requests >> 3);
	default:
		return s.sfr[reg];
	}
}

void
Tmp90ph44::sfr_write (uint8_t reg, uint8_t val)
{
	sync ();
	switch (reg) {
	case WDCR:
		if (val == 0x4E)
			s.watchdog = 0;
		else if (val == 0xB1 && !(s.sfr[WDMOD] & 0x80))
			s.sfr[WDMOD] &= 0x7F;
		break;
	case TRUN: {
		uint8_t stopped = s.sfr[TRUN] & (uint8_t) ~val;

		/* A timer stopped is cleared; so is the prescaler */
		for (unsigned n = 0; n < 4; n++)
			if (stopped & (1u << n))
				s.timer[n] = 0;
		if (stopped & 0x20)
			s.prescaler = 0;
		s.sfr[TRUN] = val;
		break;
	}
	case SCBUF:
		if (s.tx_left >= 0)
			unknown ("a byte written while one is sent:", val);
		s.tx = val;
		s.tx_left = BYTE_STATES;
		break;
	case IRFL:
		/* The flag of the vector val * 8 (1499, 14C1: 05 for 0028, 07
		 * for 0038) */
		s.requests &= ~(1u << (val & 31));
		break;
	default:
		s.sfr[reg] = val;
		if (reg < P5)
			tell_port (reg / 2u);
		else if (reg <= P7)
			tell_port (reg - P5 + 5u);
		else if (reg == P67CR) {
			tell_port (6);
			tell_port (7);
		}
		break;
	}
	next = next_event ();
}

/* P0 to P4 drive the bits their control register sets; P56 and P57 are
 * outputs (the TMP90PH44's data sheet, table 2.2); P6 and P7 four bits
 * each, as P67CR says */
void
Tmp90ph44::tell_port (unsigned port)
{
	if (port < 5)
		board->port_out (port, s.sfr[P0 + 2 * port], s.sfr[P0 + 2 * port + 1]);
	else if (port == 5)
		board->port_out (5, s.sfr[P5], 0xC0);
	else
		board->port_out (port, s.sfr[port == 6 ? P6 : P7] & 0x0F, p67_driven (port));
}

uint8_t
Tmp90ph44::p67_driven (unsigned port) const
{
	return (port == 6) ? (uint8_t) (s.sfr[P67CR] & 0x0F) : (uint8_t) (s.sfr[P67CR] >> 4);
}

/* The states of a tick of timer n: T01MOD and T23MOD as MAME reads them
 * (bits 1-0 timer 0 or 2, bits 3-2 timer 1 or 3: 1 phiT1, 2 phiT16, 3
 * phiT256; bits 7-4 the 8-bit mode, 0); the TMP90C844's layout of these
 * registers is a guess */
unsigned
Tmp90ph44::timer_period (unsigned n) const
{
	uint8_t mod = s.sfr[n < 2 ? T01MOD : T23MOD];
	unsigned clock = (mod >> ((n & 1) * 2)) & 3;

	if (mod & 0xF0)
		unknown ("timer mode", mod);
	switch (clock) {
	case 1: return 4;
	case 2: return 64;
	case 3: return 1024;
	default:
		unknown ("timer clock", mod);
		return 1024;
	}
}

/* The ticks timer n has left to its match */
unsigned
Tmp90ph44::ticks_to_match (unsigned n) const
{
	return ((s.sfr[TREG0 + n] - s.timer[n] - 1) & 0xFF) + 1u;
}

unsigned
Tmp90ph44::watchdog_period () const
{
	return 1u << (13 + 2 * ((s.sfr[WDMOD] >> 5) & 3));
}

/* The timers, the watchdog and the serial line brought to now, as they would
 * have gone after each instruction: the CPU sees nothing of theirs between
 * their events (the requests IRFH reads change only then), so they are
 * brought up to date only at an event, before a write to a register, and
 * at the end of run () */
void
Tmp90ph44::sync ()
{
	uint64_t states = s.states - synced;

	synced = s.states;
	if (!states)
		return;

	/* The timers: a tick each period of their clock, from the start of the
	 * prescaler; cleared at their match */
	if ((s.sfr[TRUN] & 0x20) && (s.sfr[TRUN] & 0x0F)) {
		for (unsigned n = 0; n < 4; n++) {
			unsigned period, left;
			uint64_t ticks;

			if (!(s.sfr[TRUN] & (1u << n)))
				continue;
			period = timer_period (n);
			ticks = (s.prescaler + states) / period - s.prescaler / period;
			left = ticks_to_match (n);
			if (ticks < left) {
				s.timer[n] = (uint8_t) (s.timer[n] + ticks);
				continue;
			}
			ticks -= left;
			s.timer[n] = (uint8_t) (ticks % (s.sfr[TREG0 + n] ? s.sfr[TREG0 + n] : 256u));
			/* Timers 0 and 2, whose vectors the program leaves empty,
			 * ask for nothing */
			if (n == 1)
				request (VEC_T1);
			else if (n == 3)
				request (VEC_T3);
		}
	}
	if (s.sfr[TRUN] & 0x20)
		s.prescaler += states;

	/* The watchdog: INTWD every 2^(14 + 2 WDTP) periods of fc */
	if (s.sfr[WDMOD] & 0x80) {
		s.watchdog += (uint32_t) states;
		if (s.watchdog >= watchdog_period ()) {
			s.watchdog -= watchdog_period ();
			request (VEC_WD);
		}
	}

	/* The byte being sent */
	if (s.tx_left >= 0) {
		s.tx_left -= (int32_t) states;
		if (s.tx_left < 0) {
			if (s.sfr[BRGCR] != 0x02 || (s.sfr[SCMOD] & 0x0F) != 0x09)
				unknown ("serial mode", (unsigned) (s.sfr[BRGCR] << 4 | (s.sfr[SCMOD] & 0x0F)));
			board->serial_sent (s.tx);
			request (VEC_TX);
		}
	}
	next = next_event ();
}

/* The first time a device asks for an interrupt: the watchdog's overflow,
 * the match of timer 1 or 3, the end of the byte sent */
uint64_t
Tmp90ph44::next_event () const
{
	uint64_t first = UINT64_MAX;

	if (s.sfr[WDMOD] & 0x80)
		first = synced + (s.watchdog < watchdog_period () ? watchdog_period () - s.watchdog : 0);
	if (s.tx_left >= 0)
		first = MIN2 (first, synced + (uint64_t) s.tx_left + 1);
	if (s.sfr[TRUN] & 0x20)
		for (unsigned n = 1; n < 4; n += 2)
			if (s.sfr[TRUN] & (1u << n)) {
				uint64_t period = timer_period (n);
				first = MIN2 (first, synced + (s.prescaler / period + ticks_to_match (n)) * period -
				                     s.prescaler);
			}
	return first;
}

/* In the order of their vectors, the watchdog's first. INTWD waits for the
 * interrupts to be open, as the others (the TMP90C840's does not: data book,
 * table 3.3 (1)): the program does all its work on a frame inside INTRX
 * (0276: the commands, the poll of the delay at 05DB, the start of the reply
 * at 13BB), shutting the interrupts at its start only (0079), while the
 * handler of INTWD opens them (019A); taken inside INTRX, INTWD lets the
 * delay be taken and its flag cleared, the poll then waiting for ever. INTEH
 * bit 2 enables 0028 and bit 0 0038, as the program sets and clears them
 * (147C, 1499, 14B9, 14E3); the serial ones are taken as enabled by the bits
 * the program sets at its start and never clears (INTEL 0C, INTEH 48), which
 * of them for which not being known */
uint16_t
Tmp90ph44::interrupt (bool maskable)
{
	uint8_t inteh = s.sfr[INTEH];
	bool serial;
	uint16_t vector = 0;

	if (!maskable || !s.requests)
		return 0;
	serial = (inteh & 0x48) == 0x48 && (s.sfr[INTEL] & 0x0C) == 0x0C;
	if (requested (VEC_WD))
		vector = VEC_WD;
	else if (requested (VEC_T1) && (inteh & 0x04))
		vector = VEC_T1;
	else if (requested (VEC_T3) && (inteh & 0x01))
		vector = VEC_T3;
	else if (requested (VEC_RX) && serial)
		vector = VEC_RX;
	else if (requested (VEC_TX) && serial)
		vector = VEC_TX;
	if (vector)
		s.requests &= ~(1u << (vector >> 3));
	return vector;
}

int
Tmp90ph44::save_state (State *state)
{
	tlcs90.get_registers (&s.cpu);
	return state->put (&s, sizeof (s));
}

/* run () leaves the devices up to date: they are at the time loaded */
int
Tmp90ph44::load_state (State *state)
{
	int ret = state->get (&s, sizeof (s));

	tlcs90.set_registers (&s.cpu);
	synced = s.states;
	next = next_event ();
	return ret;
}

} /* namespace hl */
