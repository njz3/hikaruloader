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
#include <string>
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

	bool open (unsigned port_in, const char *addr_out, unsigned port_out);
	void close ();
	bool is_open () const { return listener != NO_SOCKET; }

	/* Accepts the previous node and connects to the next one, without
	 * waiting (a refused connection is tried again a second later);
	 * true when both links are up */
	bool poll ();
	bool send (uint32_t type, const void *data, uint32_t size);
	/* The previous node's next message, waiting up to timeout_ms; false
	 * when none came, or the link broke (broken () then) */
	bool receive (uint32_t *type, std::vector<uint8_t> &data, unsigned timeout_ms);
	bool broken () const { return lost; }

	static const uintptr_t NO_SOCKET = (uintptr_t) -1;

private:
	bool take (uint32_t *type, std::vector<uint8_t> &data);
	void drop (uintptr_t &sock);

	uintptr_t listener, in, out;
	bool connecting;	/* out's connect () under way */
	uint64_t next_try;	/* when to connect again, in ms */
	bool lost;
	std::string addr_out;
	unsigned port_out;
	std::vector<uint8_t> rx;	/* in's bytes, not taken yet */
};

} /* namespace hl */

#endif /* __HL_RINGLINK_H__ */
