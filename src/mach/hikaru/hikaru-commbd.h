/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Odin's src/odin/machine/cabinet/netbd_837_13404.hpp,
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

#ifndef __HL_HIKARU_COMMBD_H__
#define __HL_HIKARU_COMMBD_H__

#include <deque>
#include <vector>

#include "hl/device.h"
#include "hl/machine.h"
#include "hl/ringlink.h"
#include "cpu/m68k/m68000.h"

namespace hikaru {

/* The communication board 837-13404 with its 68000 running the program the
 * game uploads, on the external bus's bank 0E ([Network] COMM_BOARD =
 * PROGRAM); its frames go round the cabinets' ring in step with their
 * time. See hikaru-commbd.cpp; hikaru-comm.cpp simulates it instead. */
class CommBoard : public hl::Device, public M68000::Bus {
public:
	explicit CommBoard (hl::Machine *mach);
	~CommBoard ();

	/* addr: the offset in bank 0E */
	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;

	/* A line of the Hikaru's frame: the board runs as long, once the
	 * previous cabinet is far enough */
	void line ();
	/* A frame's end: the ring's connections, its log */
	void frame ();

	/* M68000::Bus */
	uint16_t read16 (uint32_t addr) override;
	uint8_t	 read8 (uint32_t addr) override;
	void	 write16 (uint32_t addr, uint16_t val) override;
	void	 write8 (uint32_t addr, uint8_t val) override;

private:
	/* A frame of the fibre on its way into the board: its bytes, and the
	 * board's clock it comes at */
	struct Arrival {
		std::vector<uint8_t> data;
		uint64_t at;
	};
	/* A frame that came while the 68000 did not receive, and when */
	struct Held {
		std::vector<uint8_t> data;
		uint64_t came;
	};

	/* The board (Odin's NetBoard837_13404) */
	uint32_t host_read (uint32_t offset, unsigned size);
	void	 host_write (uint32_t offset, unsigned size, uint32_t val);
	uint8_t	 io_read (uint32_t reg);
	void	 io_write (uint32_t reg, uint8_t val);
	void	 ctrl_write (uint32_t reg, uint16_t val);
	void	 raise (unsigned mask) { irq |= mask; update_ipl (); }
	void	 lower (unsigned mask) { irq &= ~mask; update_ipl (); }
	void	 update_ipl ();
	void	 schedule_timer ();
	void	 start_transmit ();
	void	 deliver (std::vector<uint8_t> &data);
	void	 receive (std::vector<uint8_t> &data, uint64_t at);
	void	 run_until (uint64_t until);
	uint64_t next_event () const;
	/* The board's clock, also in the middle of a run of the 68000 (a
	 * register it writes, a frame it sends) */
	uint64_t now () const { return in_run ? cpu->regs.cycles - cpu_base : clock; }
	/* An event at a clock while the 68000 may be running: its run stops
	 * there, so that the event is not taken late */
	void	 event_at (uint64_t at) { if (in_run) cpu->end_run_at (at + cpu_base); }
	uint8_t	&comm_byte (unsigned b, uint32_t offset) { return comm[b & 1][offset & 0xFFFF]; }

	/* The ring */
	void	 open_ring ();
	void	 keep_step (uint64_t target);
	void	 take_messages ();
	void	 took_time (int64_t time);
	void	 took_frame (int64_t end, std::vector<uint8_t> &data);
	void	 send_time ();
	void	 out_of_step (const char *why);
	void	 previous_left ();

	M68000	*cpu;
	uint8_t	 ram[0x10000];		/* the 68000's */
	uint8_t	 comm[2][0x10000];	/* the comm RAM's two banks */
	unsigned bank;			/* the one the 68000 sees at 80000 */

	uint8_t	 io[0x100];		/* the 315-5917s' registers */
	uint16_t rx_addr, rx_size, tx_addr, tx_size;
	uint16_t status0, status1;	/* 88 and 8A: what the host reads */
	bool	 rx_armed, tx_busy;
	uint64_t tx_end;

	uint16_t timer;			/* 40000+C0: the count that gives IRQ5 */
	bool	 ticked;		/* the count gave an IRQ5 since 40000+E0 */
	uint64_t timer_zero;		/* the clock the count was 0 at */
	uint64_t timer_next;
	uint16_t leds;
	unsigned irq;			/* the levels raised, a bit each */

	uint16_t host_regs[0x80];
	bool	 running;		/* host register C0: the 68000 let go */
	bool	 in_run;		/* in the middle of a run of the 68000 */

	uint64_t clock;			/* the board's, 10 MHz */
	uint64_t cpu_base;		/* the 68000's cycles at clock 0 */
	uint64_t due;			/* the clock the host's lines reached */
	uint64_t line_frac;		/* their fraction of a clock, in lines */
	std::deque<Arrival> arrivals;
	std::deque<Held> held;

	/* What went out and came in (the log): held, came while the 68000
	 * did not receive; lost, came while it was held in reset; stale, held
	 * too long */
	struct Counts {
		uint64_t sent, received, held, lost, stale;
	} counts;

	/* The ring, to the other cabinets; loop: closed on this one, the
	 * frames back at once */
	hl::RingLink link;
	bool	 ring_up;
	bool	 loop;
	unsigned in_seen, out_seen;	/* the link's connections so far */
	bool	 peer_ok;		/* the previous cabinet said hello */
	/* The ring's time is the board's clock plus offset, the same in all
	 * the cabinets once in step. promise: the ring's time the previous
	 * cabinet ran to, none of its frames to come starting before it */
	int64_t	 offset;
	int64_t	 promise;
	bool	 has_promise;
	bool	 in_step;		/* this board waits for the previous one */
	uint64_t sent_at;		/* the clock of the last time sent */
	uint64_t latency;		/* the fibre's, in clocks */
	uint64_t slice;			/* the time sent every slice clocks */
	uint64_t spin_us;		/* a wait looks this long before sleeping */
	/* The host's microseconds spent waiting for the previous cabinet,
	 * and the frames, for the log */
	uint64_t waited_us;
	unsigned log_frames;
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_COMMBD_H__ */
