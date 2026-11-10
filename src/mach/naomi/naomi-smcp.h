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

#ifndef HL_NAOMI_SMCP_H
#define HL_NAOMI_SMCP_H

#include <cstdint>
#include <vector>

namespace naomi {

/*
 * The motion PC of a Cycraft cabinet, as its TCP connection shows it: see
 * naomi-smcp.cpp. One connection at a time, driven from the machine's thread.
 */
class SmcpPeer {
public:
	SmcpPeer () { reset (); }

	/* A connection opened or closed: the motion base back at its initial
	 * position, the game's version not yet known */
	void	 reset ();
	/* What the game sent */
	void	 input (const uint8_t *data, uint32_t size);
	/* What it may read; the bytes given */
	uint32_t output (uint8_t *to, uint32_t size);
	bool	 pending () const { return sent < out.size (); }
	/* The time gone by: a move of the motion base ends */
	void	 tick (uint64_t elapsed_us);

private:
	void	 command (const uint8_t *packet);

	std::vector<uint8_t> in, out;
	size_t	 sent;
	uint32_t status;	/* the status word of the motion PC */
	unsigned target;	/* the position the base is moving to */
	uint64_t move_us;	/* time left of its move */
};

} /* namespace naomi */

#endif
