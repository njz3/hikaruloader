/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
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

#ifndef __HL_RINGLINK_H__
#define __HL_RINGLINK_H__

#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hl {

/* The cabinets' ring over TCP, as their optical fibres: the previous
 * node's link comes in on port_in, ours goes out to the next node's
 * address and port. A node can be its own next (the same port, on
 * 127.0.0.1). Messages: a 32-bit length, a 32-bit type, the data; little
 * endian */
class RingLink {
public:
	RingLink ();
	~RingLink ();

	/* Before open (): queued, send () never waits. A thread of the link's
	 * own connects to the next node and sends what waits in its queue
	 * (QUEUE_MOST bytes at most, more is dropped), the hello first on each
	 * new link; this node runs on when the next one stops, and a send's
	 * cost (some 15 us on Windows) is not the emulation's. For the board's
	 * program, which sends at each slice of a frame (hikaru-commbd.cpp).
	 * Else send () waits, as the simulated board's exchange of a frame
	 * wants */
	void set_queued (bool on) { queued = on; }
	void set_hello (uint32_t type, const void *data, uint32_t size);
	/* Queued: after sending, the thread looks for more this long before
	 * sleeping (a thread asleep takes long to wake) */
	void set_spin_us (unsigned us) { spin_us = us; }

	bool open (unsigned port_in, const char *addr_out, unsigned port_out);
	void close ();
	bool is_open () const { return listener != NO_SOCKET; }

	/* Accepts the previous node and (not queued) connects to the next
	 * one, without waiting (a refused connection is tried again a second
	 * later); true when both links are up */
	bool poll ();
	/* Queued: false when the message was dropped (no next node, or too
	 * much waiting) */
	bool send (uint32_t type, const void *data, uint32_t size);
	/* The previous node's next message, waiting up to timeout_ms (0: not
	 * at all); false when none came, or the link broke (broken () then) */
	bool receive (uint32_t *type, std::vector<uint8_t> &data, unsigned timeout_ms);
	/* Waits up to timeout_us for the previous node's bytes and keeps those
	 * that came, for receive (); true when some came */
	bool wait_us (unsigned timeout_us);
	bool broken () const { return lost; }

	/* The connections made so far, from the previous node and to the next
	 * one: a new one starts its messages again (a hello first) */
	unsigned links_in () const { return in_count; }
	unsigned links_out () const { return queued ? out_count.load () : out_count_plain; }
	bool in_up () const { return in != NO_SOCKET; }
	bool out_up () const { return queued ? out_ok.load () : out != NO_SOCKET && !connecting; }

	static const uintptr_t NO_SOCKET = (uintptr_t) -1;
	static const size_t QUEUE_MOST = 1 << 20;

private:
	bool take (uint32_t *type, std::vector<uint8_t> &data);
	void drop (uintptr_t &sock);
	/* Reads what in has: false when nothing came (queued) or it broke
	 * (broken () then) */
	bool read_in ();
	/* Queued: the thread's work, and its connection to the next node */
	void send_loop ();
	bool connect_out ();
	static void put_header (std::vector<uint8_t> &msg, uint32_t type, uint32_t size);

	uintptr_t listener, in, out;
	bool connecting;	/* out's connect () under way */
	uint64_t next_try;	/* when to connect again, in ms */
	bool lost;
	std::string addr_out;
	unsigned port_out;
	std::vector<uint8_t> rx;	/* in's bytes, not taken yet from rx_at */
	size_t rx_at;
	unsigned in_count, out_count_plain;

	/* Queued: the messages for the thread, the hello, the links made */
	bool queued;
	std::thread sender;
	std::mutex out_lock;
	std::condition_variable out_wake;
	std::vector<uint8_t> tx;
	std::vector<uint8_t> hello;
	bool sender_quit, sender_idle;
	unsigned spin_us;
	std::atomic<bool> tx_pending;
	std::atomic<bool> out_ok;
	std::atomic<unsigned> out_count;
};

} /* namespace hl */

#endif /* __HL_RINGLINK_H__ */
