/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cabinet/netbd_837_13404.cpp and
 * src/odin/machine/boards/hikaru/hikaru_netbd.cpp,
 * Copyright (C) 2026, Réda Chérif-Touil.
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

#include <chrono>

#include "hl/core.h"
#include "hl/config.h"
#include "hl/state.h"

#include "mach/hikaru/hikaru-commbd.h"

namespace hikaru {

/*
 * Communication Board, its program run
 * ====================================
 *
 * [Network] COMM_BOARD = PROGRAM: the SAMURAI COMMUNICATION BD (837-13404;
 * see hikaru-comm.cpp for the host's side, the games and the simulation)
 * with its 68000 running the program the game uploads ("m3_comm.asm ver.
 * 1.3", the same in VON4, SGNASCAR and SWRACER), as Odin runs it: a 68000
 * with 64 KB of RAM, the two banks of comm RAM it shares with the host, and
 * two 315-5917 that send and receive the frames of the optical ring. What
 * the 315-5917s do is deduced from what the program needs (Odin's
 * guesses, marked so; its TODO, "Network board").
 *
 * The 68000's map, by its address bits 19-18:
 *
 *  00000	its RAM
 *  40000	control: 00 the bank of comm RAM it sees, 40 IRQ5 acknowledged,
 *		80 the LEDs, A0 IRQ2 acknowledged, C0 IRQ5's count, E0 the
 *		count back to 0
 *  80000	the comm RAM, its bank
 *  C0000	the 315-5917s, an odd byte each (below)
 *
 * The host's bank 0E: the comm RAM bank the 68000 does not use (a host byte
 * the 68000's at the offset XOR 3: an SH-4 32-bit word is a 68000 long),
 * the registers (one 16-bit register per 32-bit word: 88 and 8A the status
 * words both read and write, C0 the 68000's reset), its RAM (a 16-bit word
 * per 32-bit word). A host write of status 0, the mailbox, raises IRQ2.
 *
 * Interrupts: IRQ2 the host's mailbox, IRQ4 a frame sent, IRQ5 the
 * counter, IRQ6 a frame received.
 *
 * The ring
 * ========
 *
 * Each board's frames go to the next cabinet's (hl::RingLink over TCP,
 * [Network] PORT_IN, ADDRESS_OUT, PORT_OUT). In each turn of its counter
 * (16.4 ms) a board sends two frames: a small one, which goes round the
 * ring first, then the slots of all the cabinets (VON4's 4 x 0x1200 bytes,
 * 14.7 ms at 10 Mbit/s). The small one must be round in what is left: with
 * four VON4s, a hop of 8 of the Hikaru's lines (0.25 ms) keeps the two
 * frames a turn, one of 16 lines (0.5 ms) halves them, the program then
 * skipping every other turn.
 *
 * The Hikaruloaders of a ring therefore run in step, at the grain of a few
 * lines (a line is 30.6 us): each board tells the next one the time it ran
 * to (TIME, every 2 lines), and a board whose 68000 runs goes no further
 * than the previous one's time plus the fibre's latency, which no frame of
 * the previous board can beat; it waits for the next TIME else. Frames
 * carry the time their last byte left (FRAM) and come in the latency after
 * it, at the same time of the boards in every run whatever the PCs do.
 * Running free in real time instead (each board on a thread of its own,
 * its frames delivered as they come) needs every cabinet at full speed,
 * and loses frames to the wakes of the threads; in step, a ring runs as
 * fast as its slowest cabinet, at any speed (the agent's steps too).
 *
 * The cost is the messages and the waits: a TIME every 2 lines is 16000
 * sends a second, some 15 us each on Windows, sent by RingLink's thread,
 * which looks for the next one 300 us before sleeping; a board that waits
 * looks for the next TIME as long, as a thread asleep wakes late. With four
 * SWRACERs on one 8-core PC (November 2026), the board took 3.4 to 4.9 ms
 * of a frame (its 68000 0.7), the waits 0.5 to 2 s in 10; they raced each
 * other at 25 frames a second unthrottled, four racing alone at 37.
 *
 * The cabinets start at different times: the ring's time is each board's
 * clock plus an offset, which only grows, by whole 3 frames so that all
 * the frames start at the same time of the ring (each cabinet's work at
 * the end of its frame, the picture, done while the others do theirs: the
 * waits of four cabinets fell from 10 s to 0.7 s in 10). A board the
 * previous one is far ahead of (0.1 s) takes its time at once; one far
 * behind is not waited for, until it took the time of its own previous
 * board; so the latest time goes round the ring and all are in step after
 * a turn of messages. A board waits 2 s at most for the previous one
 * (paused, stopped), then runs on alone, its frames late, until it is
 * back. A board held in reset (the game booting, its test menu) receives
 * nothing and waits for nobody.
 *
 * A cabinet alone (PORT_OUT = PORT_IN on this PC) has its ring closed on
 * itself: its frames come back 10 us after leaving (Odin's).
 */

/* Odin's guesses: the 68000 at 40 MHz / 4 (a 12 MHz part); a tick of IRQ5's
 * 15-bit counter 0.5 us, so that a turn of it (16.4 ms) is about a frame;
 * 10 Mbit/s on the fibre, 2 bytes around each frame; a frame waits 1 ms at
 * most in the chip for the 68000 to receive */
#define CLOCK_HZ	10000000
#define TICK		5
#define BYTE_CLOCKS	8
#define FRAME_OVERHEAD	2
#define HELD_MOST	10000
/* Around a ring of one, 10 us */
#define LOOP_LATENCY	100

/* The Hikaru's lines a second (544 a frame); a line, 306.4 clocks */
#define LINES_HZ	(60 * (480 + 64))

/* The fibre's latency between two Hikaruloaders, 8 lines (0.25 ms); the
 * slice their times go at, 2 lines; how long a wait looks before sleeping,
 * in us. A board in step waits when it is within a line of the previous
 * one's time plus the latency, after the longest a 68000 instruction runs
 * past its target (OVERSHOOT). For a test (a ring of PCs over a network,
 * slower): HR_COMM_LATENCY (clocks), HR_COMM_SLICE (lines), HR_COMM_SPIN,
 * HR_COMM_SEND_SPIN (the sender thread's look, us) */
#define DEFAULT_LATENCY	2450
#define DEFAULT_SLICE	2
#define DEFAULT_SPIN	300
#define DEFAULT_SEND_SPIN 300
#define OVERSHOOT	200
/* The previous cabinet this far ahead of the ring's time is followed at
 * once (0.1 s); waited for this long at most (ms) */
#define JUMP		1000000
#define WAIT_MOST	2000
/* The offset grows by whole 3 frames (exactly 500000 clocks), so that the
 * cabinets' frames start at the same time of the ring: each one's work at
 * its frame's end (the picture, the inputs) is then done while the others
 * do theirs, not while they wait for it */
#define ALIGN		500000

/* The ring's messages (hl::RingLink), little endian */
#define MSG_HELLO	0x4F4C4548	/* "HELO": the game, PROTOCOL */
#define MSG_TIME	0x454D4954	/* "TIME": the ring's time run to */
#define MSG_FRAME	0x4D415246	/* "FRAM": its last byte's time, its bytes */
#define PROTOCOL	1

/* The registers the program uses (C0000 + an odd byte) */
enum {
	IO_RX_STATUS	= 0x11,	/* read when a frame is in */
	IO_RX_CTRL	= 0x15,	/* 80 then 17 = 8C: receive; other: stop, IRQ6 off */
	IO_RX_GO	= 0x17,
	IO_TX_STATUS	= 0x19,
	IO_TX_CTRL	= 0x1B,	/* 80 then 1D = 8C: send; 0: IRQ4 off */
	IO_TX_GO	= 0x1D,
	IO_RX_ADDR	= 0x41,	/* two byte writes each, the low byte first */
	IO_RX_SIZE	= 0x43,
	IO_TX_ADDR	= 0x45,
	IO_TX_SIZE	= 0x47,
	IO_IRQ		= 0x83,	/* read, then written 0, by each handler */
	IO_STATUS0	= 0x88,	/* the two words the host reads */
	IO_STATUS1	= 0x8A
};

enum {
	IRQ2 = 1 << 2,
	IRQ4 = 1 << 4,
	IRQ5 = 1 << 5,
	IRQ6 = 1 << 6
};

/* The 68000's map, by address bits 19-18 */
enum {
	MAP_RAM,
	MAP_CTRL,
	MAP_COMM,
	MAP_IO
};

/* With the frame, for the cabinets' logs side by side */
#define COMM_PRINT(fmt_, ...) \
	HL_PRINT ("comm: frame %llu: " fmt_, (unsigned long long) mach->frame, ##__VA_ARGS__)

static uint64_t
now_us ()
{
	return (uint64_t) std::chrono::duration_cast<std::chrono::microseconds> (
		std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

static void
put64 (uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t) (v >> (8 * i));
}

static uint64_t
get64 (const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t) p[i] << (8 * i);
	return v;
}

CommBoard::CommBoard (hl::Machine *mach) :
	hl::Device (mach),
	link ()
{
	int lat = hl_util_get_int_option ("HR_COMM_LATENCY", DEFAULT_LATENCY);
	int sl = hl_util_get_int_option ("HR_COMM_SLICE", DEFAULT_SLICE);

	cpu = new M68000 (this);
	spin_us = (uint64_t) MAX2 (0, hl_util_get_int_option ("HR_COMM_SPIN", DEFAULT_SPIN));
	slice = (uint64_t) MAX2 (1, MIN2 (64, sl)) * CLOCK_HZ / LINES_HZ;
	/* Below this, a board in step could wait for a time the previous one
	 * only sends once it ran past it */
	latency = MAX2 ((uint64_t) MAX2 (lat, 0), slice + 2 * (CLOCK_HZ / LINES_HZ + 1) + OVERSHOOT);
	if (lat != DEFAULT_LATENCY || sl != DEFAULT_SLICE)
		HL_PRINT ("comm: the fibre's latency %llu clocks, a time sent every %llu clocks",
		          (unsigned long long) latency, (unsigned long long) slice);
	ring_up = loop = false;
	in_seen = out_seen = 0;
	peer_ok = has_promise = in_step = false;
	offset = promise = 0;
	sent_at = 0;
	waited_us = 0;
	log_frames = 0;
	clock = cpu_base = due = line_frac = 0;
	in_run = false;
	memset (&counts, 0, sizeof (counts));
	reset (hl::RESET_HARD);
}

CommBoard::~CommBoard ()
{
	link.close ();
	delete cpu;
}

/*
 * The board
 */

/* Power-on: the 68000 held in reset until the host lets it go */
void
CommBoard::reset (hl::ResetType type)
{
	(void) type;
	memset (ram, 0, sizeof (ram));
	memset (comm, 0, sizeof (comm));
	bank = 0;
	memset (io, 0, sizeof (io));
	rx_addr = rx_size = tx_addr = tx_size = 0;
	status0 = status1 = 0;
	rx_armed = tx_busy = false;
	tx_end = 0;
	timer = 0;
	timer_zero = 0;
	timer_next = UINT64_MAX;
	ticked = false;
	leds = 0;
	irq = 0;
	memset (host_regs, 0, sizeof (host_regs));
	running = false;
	arrivals.clear ();
	held.clear ();
	update_ipl ();
}

void
CommBoard::update_ipl ()
{
	unsigned level = 0, l;

	for (l = 7; l > 0; l--)
		if (irq & (1u << l)) {
			level = l;
			break;
		}
	cpu->set_ipl (level);
}

/* Odin's guess: a 15-bit counter turns all the time; IRQ5 comes each time
 * it reaches the value of 40000+C0; a write of 40000+E0 sets it back to 0,
 * with an IRQ5 at once when the count gave none since the write before. So
 * a slave that keeps in step with the master's frames gets one IRQ5 a
 * frame: when the frame comes before its count (the slave of m3_comm 1.3
 * wants an IRQ5 there), at its count otherwise (the program 1.2 switches
 * its bank in IRQ5) */
void
CommBoard::schedule_timer ()
{
	uint64_t turn = 0x8000 * TICK;
	uint64_t at = (uint64_t) (timer & 0x7FFF) * TICK;
	uint64_t t = now ();
	uint64_t since = (t - timer_zero) % turn;
	uint64_t next = t - since + at;

	if (next <= t)
		next += turn;
	timer_next = next;
	event_at (next);
}

/* The clock of its next event: its counter, the end of a frame it sends,
 * a frame that comes */
uint64_t
CommBoard::next_event () const
{
	uint64_t t = timer_next;

	if (tx_busy)
		t = MIN2 (t, tx_end);
	if (!arrivals.empty ())
		t = MIN2 (t, arrivals.front ().at);
	return t;
}

void
CommBoard::run_until (uint64_t until)
{
	while (clock < until) {
		uint64_t stop = MIN2 (until, next_event ());

		if (stop > clock) {
			if (running) {
				in_run = true;
				clock += cpu->run ((uint32_t) MIN2 (stop - clock, (uint64_t) 1 << 30));
				in_run = false;
			} else
				clock = stop;
		}
		while (!arrivals.empty () && arrivals.front ().at <= clock) {
			std::vector<uint8_t> data = std::move (arrivals.front ().data);
			arrivals.pop_front ();
			deliver (data);
		}
		if (timer_next <= clock) {
			raise (IRQ5);
			ticked = true;
			schedule_timer ();
		}
		if (tx_busy && tx_end <= clock) {
			tx_busy = false;
			raise (IRQ4);
		}
	}
}

/* A frame from the previous board, coming at clock at; not before one that
 * came before it */
void
CommBoard::receive (std::vector<uint8_t> &data, uint64_t at)
{
	if (!arrivals.empty ())
		at = MAX2 (at, arrivals.back ().at);
	arrivals.push_back (Arrival { std::move (data), at });
	event_at (at);
}

/* A frame in while receiving: into the 68000's bank at the receive
 * address, as much as the receive size allows. Odin's guess: one that comes
 * while the 68000 is held in reset is lost; one that comes while it runs
 * but does not receive waits in the chip, and comes in when it receives, if
 * it waited HELD_MOST at most */
void
CommBoard::deliver (std::vector<uint8_t> &data)
{
	size_t i, n;

	if (!rx_armed) {
		if (running) {
			held.push_back (Held { std::move (data), now () });
			counts.held++;
		} else
			counts.lost++;
		return;
	}
	counts.received++;
	n = MIN2 (data.size (), (size_t) rx_size);
	for (i = 0; i < n; i++)
		comm_byte (bank, (uint32_t) (rx_addr + i)) = data[i];
	rx_armed = false;
	raise (IRQ6);
}

/* A frame out: the bytes at the send address as they are now (Odin's
 * guess: the whole frame read at its start), IRQ4 once its last byte left */
void
CommBoard::start_transmit ()
{
	std::vector<uint8_t> data (tx_size);
	uint64_t start = now ();
	uint32_t i;

	for (i = 0; i < tx_size; i++)
		data[i] = comm_byte (bank, tx_addr + i);
	tx_end = start + (uint64_t) (tx_size + FRAME_OVERHEAD) * BYTE_CLOCKS;
	tx_busy = true;
	counts.sent++;
	event_at (tx_end);
	if (loop)
		receive (data, tx_end + LOOP_LATENCY);
	else if (ring_up && link.out_up ()) {
		std::vector<uint8_t> msg (8 + data.size ());
		put64 (&msg[0], (uint64_t) ((int64_t) tx_end + offset));
		if (!data.empty ())
			memcpy (&msg[8], &data[0], data.size ());
		link.send (MSG_FRAME, &msg[0], (uint32_t) msg.size ());
	}
}

uint8_t
CommBoard::io_read (uint32_t reg)
{
	switch (reg) {
	case IO_RX_STATUS:
	case IO_TX_STATUS:
		return 0x05;	/* Odin's guess: "done", as MAME and Supermodel answer */
	case IO_IRQ:
		return 0;
	case IO_STATUS0:
		return (uint8_t) (status0 >> 8);
	case IO_STATUS0 + 1:
		return (uint8_t) status0;
	case IO_STATUS1:
		return (uint8_t) (status1 >> 8);
	case IO_STATUS1 + 1:
		return (uint8_t) status1;
	default:
		return io[reg & 0xFF];
	}
}

void
CommBoard::io_write (uint32_t reg, uint8_t val)
{
	io[reg & 0xFF] = val;
	switch (reg) {
	case IO_RX_ADDR:
		rx_addr = (uint16_t) ((rx_addr >> 8) | (val << 8));
		break;
	case IO_RX_SIZE:
		rx_size = (uint16_t) ((rx_size >> 8) | (val << 8));
		break;
	case IO_TX_ADDR:
		tx_addr = (uint16_t) ((tx_addr >> 8) | (val << 8));
		break;
	case IO_TX_SIZE:
		tx_size = (uint16_t) ((tx_size >> 8) | (val << 8));
		break;
	case IO_RX_CTRL:
		if (val != 0x80) {
			rx_armed = false;
			lower (IRQ6);
		}
		break;
	case IO_RX_GO:
		if (val == 0x8C && io[IO_RX_CTRL] == 0x80) {
			rx_armed = true;
			while (!held.empty () && now () - held.front ().came > HELD_MOST) {
				held.pop_front ();
				counts.stale++;
			}
			if (!held.empty ()) {
				std::vector<uint8_t> data = std::move (held.front ().data);
				held.pop_front ();
				deliver (data);
			}
		} else if (val == 0) {
			rx_armed = false;
			lower (IRQ6);
		}
		break;
	case IO_TX_GO:
		if (val == 0x8C && io[IO_TX_CTRL] == 0x80)
			start_transmit ();
		else if (val == 0)
			lower (IRQ4);
		break;
	case IO_STATUS0:
		status0 = (uint16_t) ((status0 & 0x00FF) | (val << 8));
		break;
	case IO_STATUS0 + 1:
		status0 = (uint16_t) ((status0 & 0xFF00) | val);
		break;
	case IO_STATUS1:
		status1 = (uint16_t) ((status1 & 0x00FF) | (val << 8));
		break;
	case IO_STATUS1 + 1:
		status1 = (uint16_t) ((status1 & 0xFF00) | val);
		break;
	default:
		break;
	}
}

void
CommBoard::ctrl_write (uint32_t reg, uint16_t val)
{
	switch (reg) {
	case 0x00:	/* the bank of comm RAM at 80000 */
		bank = val & 1;
		break;
	case 0x40:
		lower (IRQ5);
		break;
	case 0x80:
		leds = val;
		break;
	case 0xA0:
		lower (IRQ2);
		break;
	case 0xC0:
		timer = val;
		schedule_timer ();
		break;
	case 0xE0:
		if (!ticked)
			raise (IRQ5);
		ticked = false;
		timer_zero = now ();
		schedule_timer ();
		break;
	default:
		break;
	}
}

/* M68000::Bus */

uint16_t
CommBoard::read16 (uint32_t addr)
{
	uint32_t off = addr & 0xFFFF;

	switch ((addr >> 18) & 3) {
	case MAP_RAM:
		return (uint16_t) ((ram[off] << 8) | ram[(off + 1) & 0xFFFF]);
	case MAP_CTRL:
		return (addr & 0xFF) == 0 ? (uint16_t) bank : 0;
	case MAP_COMM:
		return (uint16_t) ((comm_byte (bank, off) << 8) | comm_byte (bank, off + 1));
	default:
		return (uint16_t) ((io_read (addr & 0xFE) << 8) | io_read ((addr & 0xFE) + 1));
	}
}

uint8_t
CommBoard::read8 (uint32_t addr)
{
	uint32_t off = addr & 0xFFFF;

	switch ((addr >> 18) & 3) {
	case MAP_RAM:
		return ram[off];
	case MAP_CTRL:
		return (addr & 0xFF) == 1 ? (uint8_t) bank : 0;
	case MAP_COMM:
		return comm_byte (bank, off);
	default:
		return io_read (addr & 0xFF);
	}
}

void
CommBoard::write16 (uint32_t addr, uint16_t val)
{
	uint32_t off = addr & 0xFFFF;

	switch ((addr >> 18) & 3) {
	case MAP_RAM:
		ram[off] = (uint8_t) (val >> 8);
		ram[(off + 1) & 0xFFFF] = (uint8_t) val;
		break;
	case MAP_CTRL:
		ctrl_write (addr & 0xFF, val);
		break;
	case MAP_COMM:
		comm_byte (bank, off) = (uint8_t) (val >> 8);
		comm_byte (bank, off + 1) = (uint8_t) val;
		break;
	default:
		io_write (addr & 0xFE, (uint8_t) (val >> 8));
		io_write ((addr & 0xFE) + 1, (uint8_t) val);
		break;
	}
}

void
CommBoard::write8 (uint32_t addr, uint8_t val)
{
	uint32_t off = addr & 0xFFFF;

	switch ((addr >> 18) & 3) {
	case MAP_RAM:
		ram[off] = val;
		break;
	case MAP_CTRL:
		ctrl_write (addr & 0xFF, val);
		break;
	case MAP_COMM:
		comm_byte (bank, off) = val;
		break;
	default:
		io_write (addr & 0xFF, val);
		break;
	}
}

/*
 * The host's bank 0E
 *
 * In the comm RAM a host byte is the 68000's byte at the same offset XOR 3:
 * a 32-bit word of the SH-4 is a long of the 68000 (Odin found it from the
 * frame counter at 80006 that the host reads as the word at +4, VON4
 * 0C1B8CF4).
 */

uint32_t
CommBoard::host_read (uint32_t offset, unsigned size)
{
	uint32_t v = 0, reg, a;
	unsigned i;

	offset &= 0x3FFFF;
	if (offset < 0x10000) {
		for (i = 0; i < size; i++)
			v |= (uint32_t) comm_byte (bank ^ 1, (offset + i) ^ 3) << (8 * i);
		return v;
	}
	if (offset < 0x20000) {
		reg = ((offset - 0x10000) >> 1) & 0xFE;
		if (reg == IO_STATUS0)
			return status0;
		if (reg == IO_STATUS1)
			return status1;
		return host_regs[(reg >> 1) & 0x7F];
	}
	if (offset & 2)
		return 0;
	a = ((offset - 0x20000) >> 1) & 0xFFFE;
	return (uint32_t) ((ram[a] << 8) | ram[a + 1]);
}

void
CommBoard::host_write (uint32_t offset, unsigned size, uint32_t val)
{
	uint32_t reg, a;
	uint16_t v;
	unsigned i;

	offset &= 0x3FFFF;
	if (offset < 0x10000) {
		for (i = 0; i < size; i++)
			comm_byte (bank ^ 1, (offset + i) ^ 3) = (uint8_t) (val >> (8 * i));
		return;
	}
	if (offset < 0x20000) {
		reg = ((offset - 0x10000) >> 1) & 0xFE;
		v = (uint16_t) val;
		if (reg == IO_STATUS0) {
			/* Odin's guess, as MAME: the mailbox raises IRQ2 */
			status0 = v;
			raise (IRQ2);
			return;
		}
		if (reg == IO_STATUS1) {
			status1 = v;
			return;
		}
		host_regs[(reg >> 1) & 0x7F] = v;
		if (reg == 0xC0) {
			/* The 68000 runs while it is not 0 */
			bool run = v != 0;
			if (run && !running) {
				held.clear ();
				irq = 0;
				cpu->reset ();
				cpu_base = cpu->regs.cycles - clock;
				update_ipl ();
				timer = 0;
				timer_zero = clock;
				ticked = false;
				raise (IRQ5);
				schedule_timer ();
				COMM_PRINT ("the board's 68000 runs the game's program");
				open_ring ();
			} else if (!run && running)
				COMM_PRINT ("the game holds the board");
			running = run;
		}
		return;
	}
	if (offset & 2)
		return;
	a = ((offset - 0x20000) >> 1) & 0xFFFE;
	ram[a] = (uint8_t) (val >> 8);
	ram[a + 1] = (uint8_t) val;
}

int
CommBoard::get (unsigned size, uint32_t addr, void *val)
{
	uint64_t v;

	addr &= 0x3FFFF;
	if (addr < 0x10000 && size <= 8) {
		unsigned i;
		v = 0;
		for (i = 0; i < size; i++)
			v |= (uint64_t) comm_byte (bank ^ 1, (addr + i) ^ 3) << (8 * i);
	} else if (size == 8)
		v = host_read (addr, 4) | ((uint64_t) host_read (addr + 4, 4) << 32);
	else
		v = host_read (addr, size);
	if (addr >= 0x10000 && addr < 0x20000)
		HL_LOG ("COMM R%u %05X = %llX", size * 8, addr, (unsigned long long) v);
	set_ptr (val, size, v);
	return 0;
}

int
CommBoard::put (unsigned size, uint32_t addr, uint64_t val)
{
	addr &= 0x3FFFF;
	if (addr >= 0x10000 && addr < 0x20000)
		HL_LOG ("COMM W%u %05X = %llX", size * 8, addr, (unsigned long long) val);
	if (addr < 0x10000 && size <= 8) {
		unsigned i;
		for (i = 0; i < size; i++)
			comm_byte (bank ^ 1, (addr + i) ^ 3) = (uint8_t) (val >> (8 * i));
	} else if (size == 8) {
		host_write (addr, 4, (uint32_t) val);
		host_write (addr + 4, 4, (uint32_t) (val >> 32));
	} else
		host_write (addr, size, (uint32_t) val);
	return 0;
}

/*
 * The ring
 */

static bool
is_this_pc (const char *addr)
{
	return !strcmp (addr, "127.0.0.1") || !strcasecmp (addr, "localhost");
}

/* At the first start of the 68000: the ring the settings give */
void
CommBoard::open_ring ()
{
	const hl::Config *cfg = hl::Config::get ();
	const char *game = mach->game ? mach->game->name : "";
	uint8_t hello[36];

	if (ring_up || loop)
		return;
	if (!cfg->link) {
		COMM_PRINT ("LINK = NO_LINK: no ring, the frames lost");
		return;
	}
	if (cfg->port_in == cfg->port_out && is_this_pc (cfg->address_out)) {
		/* No socket, nor the firewall's question */
		COMM_PRINT ("the ring closed on this cabinet (PORT_OUT = PORT_IN): its frames come back to it");
		loop = true;
		return;
	}
	/* What this cabinet is, first on each link to the next one: its
	 * game, and this ring's protocol */
	memset (hello, 0, sizeof (hello));
	strncpy ((char *) hello, game, 31);
	hello[32] = PROTOCOL;
	link.set_queued (true);
	link.set_hello (MSG_HELLO, hello, sizeof (hello));
	link.set_spin_us ((unsigned) MAX2 (0, hl_util_get_int_option ("HR_COMM_SEND_SPIN", DEFAULT_SEND_SPIN)));
	if (!link.open (cfg->port_in, cfg->address_out, cfg->port_out)) {
		HL_ERROR ("comm: no ring: the frames lost");
		return;
	}
	ring_up = true;
	in_seen = link.links_in ();
	out_seen = link.links_out ();
	peer_ok = has_promise = in_step = false;
	sent_at = clock;
	link.poll ();
}

void
CommBoard::out_of_step (const char *why)
{
	if (in_step)
		COMM_PRINT ("%s: this cabinet runs on alone", why);
	in_step = false;
}

/* Its link broke: what is left of its messages is not taken, until it
 * links again and says hello */
void
CommBoard::previous_left ()
{
	if (peer_ok || in_step)
		COMM_PRINT ("the previous cabinet left%s", in_step ? ": this cabinet runs on alone" : "");
	peer_ok = has_promise = in_step = false;
}

/* The previous board's time: the ring's time, and the waits, as it says */
void
CommBoard::took_time (int64_t time)
{
	int64_t ahead = time - ((int64_t) clock + offset);

	promise = time;
	has_promise = true;
	if (ahead > JUMP) {
		/* Its time taken at once, to the nearest 3 frames; the frames
		 * on their way come now */
		ahead = (ahead + ALIGN / 2) / ALIGN * ALIGN;
		offset += ahead;
		for (Arrival &a : arrivals)
			a.at = clock;
		if (!arrivals.empty ())
			event_at (clock);
		COMM_PRINT ("the previous cabinet %.2f s ahead: this one takes its time",
		            (double) ahead / CLOCK_HZ);
		if (!in_step)
			COMM_PRINT ("in step with the previous cabinet");
		in_step = true;
	} else if (ahead < -JUMP) {
		/* It takes its own previous cabinet's time first */
		if (in_step)
			COMM_PRINT ("the previous cabinet %.2f s behind: not waited for",
			            (double) -ahead / CLOCK_HZ);
		in_step = false;
	} else if (!in_step) {
		COMM_PRINT ("in step with the previous cabinet");
		in_step = true;
	}
}

/* A frame of the previous board: in, the latency after its last byte
 * left, at once when this board is past that (out of step) */
void
CommBoard::took_frame (int64_t end, std::vector<uint8_t> &data)
{
	int64_t at = end + (int64_t) latency - offset;

	receive (data, at < (int64_t) clock ? clock : (uint64_t) at);
}

/* All the messages that came, without waiting */
void
CommBoard::take_messages ()
{
	std::vector<uint8_t> msg;
	uint32_t type;

	while (link.receive (&type, msg, 0)) {
		switch (type) {
		case MSG_HELLO: {
			char game[33];
			uint32_t protocol;
			const char *mine = mach->game ? mach->game->name : "";

			if (msg.size () < 36) {
				peer_ok = false;
				break;
			}
			memcpy (game, &msg[0], 32);
			game[32] = '\0';
			protocol = msg[32] | (msg[33] << 8) | (msg[34] << 16) | ((uint32_t) msg[35] << 24);
			peer_ok = protocol == PROTOCOL && !strcmp (game, mine);
			if (peer_ok)
				COMM_PRINT ("the previous cabinet runs %s too", game);
			else
				HL_ERROR ("comm: the previous cabinet runs %s (protocol %u), this one %s (%u): not linked",
				          game, protocol, mine, PROTOCOL);
			break;
		}
		case MSG_TIME:
			if (peer_ok && msg.size () >= 8)
				took_time ((int64_t) get64 (&msg[0]));
			break;
		case MSG_FRAME:
			if (peer_ok && msg.size () >= 8) {
				int64_t end = (int64_t) get64 (&msg[0]);
				msg.erase (msg.begin (), msg.begin () + 8);
				took_frame (end, msg);
			}
			break;
		default:
			/* A simulated board's (COMM_BOARD = SIMULATED) */
			if (peer_ok || !has_promise)
				HL_ERROR_FEW ("comm: the previous cabinet's board is not PROGRAM: not linked");
			peer_ok = false;
			break;
		}
	}
}

void
CommBoard::send_time ()
{
	uint8_t buf[8];

	sent_at = clock;
	if (!link.out_up ())
		return;
	put64 (buf, (uint64_t) ((int64_t) clock + offset));
	link.send (MSG_TIME, buf, 8);
}

/* Before a line: in step, as far as the previous board's time plus the
 * fibre's latency allows; reads what came once it is used up (all its
 * frames before that time are in), and waits for its next time if it must.
 * A read and a send are what the ring costs, each some microseconds */
void
CommBoard::keep_step (uint64_t target)
{
	int64_t need = (int64_t) target + OVERSHOOT;
	uint64_t start = 0;

	if (!running || !in_step)
		return;
	if (has_promise && promise + (int64_t) latency - offset >= need)
		return;
	take_messages ();
	while (in_step && (!has_promise || promise + (int64_t) latency - offset < need)) {
		uint64_t t = now_us ();

		if (!start)
			start = t;
		else if (t - start > WAIT_MOST * 1000ull) {
			waited_us += t - start;
			out_of_step ("no time from the previous cabinet for 2 s");
			return;
		}
		/* A look at a time first: the time comes within some tens of
		 * microseconds, sooner than a thread asleep wakes */
		if (t - start < spin_us) {
			take_messages ();
			if (link.broken () || !link.in_up ()) {
				previous_left ();
				break;
			}
		} else if (link.wait_us (1000))
			take_messages ();
		else if (link.broken () || !link.in_up ()) {
			previous_left ();
			break;
		}
	}
	if (start)
		waited_us += now_us () - start;
}

void
CommBoard::line ()
{
	/* 10 MHz over 60 x 544 lines a second; the host's time, not the
	 * board's, which ends its runs a little after it */
	line_frac += CLOCK_HZ;
	due += line_frac / LINES_HZ;
	line_frac %= LINES_HZ;
	if (ring_up)
		keep_step (due);
	run_until (due);
	if (ring_up && clock >= sent_at + slice) {
		send_time ();
		/* Out of step, the frames taken as they come */
		if (running && !in_step)
			take_messages ();
	}
}

void
CommBoard::frame ()
{
	if (!ring_up)
		return;
	link.poll ();
	if (link.links_out () != out_seen) {
		/* A new link to the next cabinet (its hello sent): its first
		 * time at once */
		out_seen = link.links_out ();
		send_time ();
	}
	if (link.links_in () != in_seen) {
		/* A new previous cabinet: in step once it said hello and its
		 * time */
		in_seen = link.links_in ();
		peer_ok = has_promise = false;
		out_of_step ("a new previous cabinet");
	}
	if (peer_ok && (link.broken () || !link.in_up ()))
		previous_left ();
	take_messages ();

	/* Every 10 s, what went round */
	if (++log_frames >= 600) {
		COMM_PRINT ("10 s: %llu frames out, %llu in, %llu held, %llu lost, %llu late; %s, waited %llu ms",
		            (unsigned long long) counts.sent, (unsigned long long) counts.received,
		            (unsigned long long) counts.held, (unsigned long long) counts.lost,
		            (unsigned long long) counts.stale, in_step ? "in step" : "alone",
		            (unsigned long long) (waited_us / 1000));
		memset (&counts, 0, sizeof (counts));
		waited_us = 0;
		log_frames = 0;
	}
}

/*
 * State
 *
 * The board as it is between two lines; not the frames on their way, nor
 * the ring's offset: a cabinet loaded goes out of step, then takes the
 * ring's time again.
 */

int
CommBoard::save_state (hl::State *state)
{
	int ret = 0;

	ret |= state->put (&cpu->regs, sizeof (cpu->regs));
	ret |= state->put (ram, sizeof (ram));
	ret |= state->put (comm, sizeof (comm));
	ret |= state->put (&bank, sizeof (bank));
	ret |= state->put (io, sizeof (io));
	ret |= state->put (&rx_addr, sizeof (rx_addr));
	ret |= state->put (&rx_size, sizeof (rx_size));
	ret |= state->put (&tx_addr, sizeof (tx_addr));
	ret |= state->put (&tx_size, sizeof (tx_size));
	ret |= state->put (&status0, sizeof (status0));
	ret |= state->put (&status1, sizeof (status1));
	ret |= state->put (&rx_armed, sizeof (rx_armed));
	ret |= state->put (&tx_busy, sizeof (tx_busy));
	ret |= state->put (&tx_end, sizeof (tx_end));
	ret |= state->put (&timer, sizeof (timer));
	ret |= state->put (&ticked, sizeof (ticked));
	ret |= state->put (&timer_zero, sizeof (timer_zero));
	ret |= state->put (&timer_next, sizeof (timer_next));
	ret |= state->put (&leds, sizeof (leds));
	ret |= state->put (&irq, sizeof (irq));
	ret |= state->put (host_regs, sizeof (host_regs));
	ret |= state->put (&running, sizeof (running));
	ret |= state->put (&clock, sizeof (clock));
	ret |= state->put (&due, sizeof (due));
	ret |= state->put (&line_frac, sizeof (line_frac));
	return ret;
}

int
CommBoard::load_state (hl::State *state)
{
	int ret = 0;
	bool was_running = running;

	ret |= state->get (&cpu->regs, sizeof (cpu->regs));
	ret |= state->get (ram, sizeof (ram));
	ret |= state->get (comm, sizeof (comm));
	ret |= state->get (&bank, sizeof (bank));
	ret |= state->get (io, sizeof (io));
	ret |= state->get (&rx_addr, sizeof (rx_addr));
	ret |= state->get (&rx_size, sizeof (rx_size));
	ret |= state->get (&tx_addr, sizeof (tx_addr));
	ret |= state->get (&tx_size, sizeof (tx_size));
	ret |= state->get (&status0, sizeof (status0));
	ret |= state->get (&status1, sizeof (status1));
	ret |= state->get (&rx_armed, sizeof (rx_armed));
	ret |= state->get (&tx_busy, sizeof (tx_busy));
	ret |= state->get (&tx_end, sizeof (tx_end));
	ret |= state->get (&timer, sizeof (timer));
	ret |= state->get (&ticked, sizeof (ticked));
	ret |= state->get (&timer_zero, sizeof (timer_zero));
	ret |= state->get (&timer_next, sizeof (timer_next));
	ret |= state->get (&leds, sizeof (leds));
	ret |= state->get (&irq, sizeof (irq));
	ret |= state->get (host_regs, sizeof (host_regs));
	ret |= state->get (&running, sizeof (running));
	ret |= state->get (&clock, sizeof (clock));
	ret |= state->get (&due, sizeof (due));
	ret |= state->get (&line_frac, sizeof (line_frac));
	bank &= 1;
	cpu->loaded ();
	cpu_base = cpu->regs.cycles - clock;
	arrivals.clear ();
	held.clear ();
	update_ipl ();
	/* A state of a running board: its ring, if not open yet */
	if (running && !was_running)
		open_ring ();
	out_of_step ("a state loaded");
	return ret;
}

const char *
CommBoard::debug_string ()
{
	static char out[128];

	snprintf (out, sizeof (out), "%s, bank %u, status %04X %04X, LEDs %04X, %s",
	          running ? "running" : "held", bank, status0, status1, leds,
	          loop ? "ring of one" : !ring_up ? "no ring" : in_step ? "in step" : "alone");
	return out;
}

} /* namespace hikaru */
