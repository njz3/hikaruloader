/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/card_reader.cpp (GPL-2.0-or-later,
 * Copyright flyinghead).
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

#include <cstdio>
#include <cstring>

#include "hl/core.h"
#include "hl/input.h"
#include "hl/paths.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * Club Kart's Card Reader
 * =======================
 *
 * On the SH-4's SCIF. Commands of 5 bytes: a start, two letters, a parameter
 * (or, for WL, a track of 69 bytes), a CRC, the XOR of what follows the start;
 * RT and OT have one byte more. The reader answers ACK, then STX, its data,
 * ETX and a CRC. The card is a file, nvram/<game>.card; F7 inserts it, and a
 * new one comes from the dispenser when the game asks for it.
 */

enum {
	STX = 2,
	ETX = 3,
	ACK = 6
};

enum {
	CARD_INIT, CARD_CANCEL, CARD_EJECT, CARD_NEW, CARD_CLEAN, CARD_RT5, CARD_READ, CARD_WRITE, CARD_STATUS,
	CARD_MAX
};

static const char commands[CARD_MAX][3] = { "IN", "CA", "OT", "HI", "CL", "RT", "RL", "WL", "SS" };

ClubKartCard::ClubKartCard (const char *game)
{
	std::string file = std::string ("nvram/") + game + ".card";

	write_path = hl::Paths::game_write (game, file);
	read_path = hl::Paths::game_read (game, file);
	memset (data, 0, sizeof (data));
}

uint8_t
ClubKartCard::crc (const uint8_t *begin, const uint8_t *end)
{
	uint8_t c = 0;

	for (; begin != end; begin++)
		c ^= *begin;
	return c;
}

bool
ClubKartCard::load ()
{
	FILE *fp = fopen (read_path.c_str (), "rb");

	if (!fp)
		return false;
	size_t got = fread (data, 1, sizeof (data), fp);

	fclose (fp);
	HL_PRINT ("card: read from '%s' (%u bytes)", read_path.c_str (), (unsigned) got);
	return true;
}

void
ClubKartCard::save ()
{
	FILE *fp = fopen (write_path.c_str (), "wb");

	if (!fp) {
		HL_ERROR ("card: cannot write '%s'", write_path.c_str ());
		return;
	}
	fwrite (data, 1, sizeof (data), fp);
	fclose (fp);
	read_path = write_path;
}

void
ClubKartCard::frame ()
{
	bool held = hl::Input::action (hl::ACTION_CARD);

	if (held && !was_held) {
		inserted = load ();
		if (inserted && read_pending) {
			reply (CARD_READ);
			read_pending = false;
		}
	}
	was_held = held;
}

void
ClubKartCard::reply (int cmd)
{
	std::vector<uint8_t> body;
	unsigned i;

	if (cmd == CARD_STATUS) {
		body = { '0', '0', '0', (uint8_t) (inserted ? '1' : '0'), (uint8_t) (inserted ? '1' : '0'), '1' };
	} else {
		body = { 'O', 'K' };
		if (cmd == CARD_READ)
			body.insert (body.end (), data, data + sizeof (data));
	}
	body.push_back (ETX);
	out.push_back (STX);
	for (i = 0; i < body.size (); i++)
		out.push_back (body[i]);
	out.push_back (crc (body.data (), body.data () + body.size ()));
}

void
ClubKartCard::command ()
{
	int cmd;

	read_pending = false;
	for (cmd = 0; cmd < CARD_MAX; cmd++)
		if (in[1] == (uint8_t) commands[cmd][0] && in[2] == (uint8_t) commands[cmd][1])
			break;
	if (cmd == CARD_MAX) {
		HL_ERROR_FEW ("card: unknown command '%c%c'", in[1], in[2]);
		return;
	}
	if (crc (in.data () + 1, in.data () + in.size () - 1) != in.back ()) {
		HL_ERROR_FEW ("card: wrong CRC on '%c%c'", in[1], in[2]);
		return;
	}
	out.push_back (ACK);
	switch (cmd) {
	case CARD_WRITE:
		memcpy (data, in.data () + 3, sizeof (data));
		save ();
		break;
	case CARD_READ:
		if (!inserted) {
			read_pending = true;
			return;
		}
		break;
	case CARD_EJECT:
		inserted = false;
		break;
	case CARD_NEW:
		inserted = true;
		break;
	default:
		break;
	}
	reply (cmd);
}

void
ClubKartCard::write (uint8_t value)
{
	in.push_back (value);
	if ((in.size () == 5 && (in[1] != 'W' || in[2] != 'L') && in[2] != 'T') ||
	    (in.size () == 6 && in[2] == 'T') ||
	    in.size () == TRACK + 5) {
		command ();
		in.clear ();
	}
}

int
ClubKartCard::available ()
{
	return (int) out.size ();
}

uint8_t
ClubKartCard::read ()
{
	uint8_t b;

	if (out.empty ())
		return 0;
	b = out.front ();
	out.pop_front ();
	return b;
}

} /* namespace naomi */
