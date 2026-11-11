/*
 * Hikaruloader (formerly Valkyrie)
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

#include <stdio.h>
#include <string.h>

#include <filesystem>

#include "hl/cardreader.h"
#include "hl/gamecontrols.h"
#include "hl/input.h"
#include "hl/state.h"

namespace hl {

/*
 * Sanwa CRP-1231 card reader/writer
 * =================================
 *
 * A magnetic card reader/writer with a shutter at its slot, a thermal
 * printer and, on some models, a dispenser of blank cards (its stocker).
 * The host and the reader talk in frames:
 *
 *   host    STX LEN CMD 00 00 00 {parameters} ETX BCC
 *   reader  ACK, or NAK when the frame is wrong
 *   host    ENQ, for the result
 *   reader  STX LEN CMD S1 S2 S3 {data} ETX BCC
 *
 * LEN counts the bytes from LEN to ETX, BCC is their XOR. The results:
 *
 *   S1  The sensors: bits 7-6 the shutter (1 closed, 2 open), bit 5 the
 *       stocker (1 has cards), bits 4-0 the card (00 none, 01 at the
 *       slot, 18 inside)
 *   S2  The error, a character: '0' none, '1' read, '2' write
 *   S3  The command's state, a character: '0' done, '2' disabled, '3'
 *       busy, '4' waiting for a card, '5' no card in the stocker
 *
 * The commands (CMD), their parameters in ASCII:
 *
 *   10  Initialize
 *   20  The state
 *   33  Read: mode, parity, track ('0'-'2' one, '3'-'5' two, '6' all)
 *   40  Cancel
 *   53  Write: mode, parity, track, then the tracks' bytes
 *   78  The printer's settings
 *   7A  Register a font
 *   7C  Print; 7D erase the printing
 *   80  Eject the card to the slot
 *   A0  Clean the heads
 *   B0  '1': a blank card from the stocker, taken in; '2': only checked
 *   C1  (VON4's RETRY SET, at its start)
 *   D0  The shutter: '1' open, '0' closed
 *
 * The answers come at once. A read with no card inside is answered
 * "waiting for a card" (S3 '4'): the game asks again (ENQ) until the
 * player's card comes, and takes it in.
 *
 * The card: three tracks of 69 bytes, in a file of 207 bytes, written at
 * each write. A card from the stocker replaces it: the file before is kept
 * as von4.1.card (the first number free).
 */

#define STX	0x02
#define ETX	0x03
#define ENQ	0x05
#define ACK	0x06
#define NAK	0x15

/* The frames before the player takes the card given back: 3 s */
#define TAKEN_FRAMES	180

CardReader::CardReader (Machine *mach, const std::string &card_path) :
	Device (mach), path (card_path)
{
	where = NO_CARD;
	shutter_open = false;
}

static void
print_frame (const char *what, const uint8_t *data, size_t size)
{
	char line[3 * 64 + 1];
	size_t i, n = 0;

	for (i = 0; i < size && i < 64; i++)
		n += snprintf (line + n, sizeof (line) - n, " %02X", data[i]);
	HL_PRINT ("card reader: %s%s%s", what, line, size > 64 ? " ..." : "");
}

void
CardReader::receive (uint8_t b)
{
	/* The host asks for the result of the command acknowledged */
	if (in.empty () && b == ENQ) {
		answer ();
		return;
	}
	if (in.empty () && b != STX) {
		if (log)
			HL_PRINT ("card reader: %02X ignored", b);
		return;
	}
	in.push_back (b);
	if (in.size () < 2 || in.size () < (size_t) in[1] + 2)
		return;

	uint8_t bcc = 0;
	for (size_t i = 1; i + 1 < in.size (); i++)
		bcc ^= in[i];
	if (log)
		print_frame ("<", in.data (), in.size ());
	if (bcc != in.back () || in[in.size () - 2] != ETX || in.size () < 7) {
		HL_PRINT ("card reader: a wrong frame (BCC %02X, %02X expected): NAK",
		          in.back (), bcc);
		out.push_back (NAK);
	} else {
		pending.assign (in.begin () + 2, in.end () - 2);
		out.push_back (ACK);
	}
	in.clear ();
}

uint8_t
CardReader::send ()
{
	if (out.empty ())
		return 0;
	uint8_t b = out.front ();
	out.pop_front ();
	return b;
}

uint8_t
CardReader::sensors () const
{
	static const uint8_t card_sensors[] = { 0x00, 0x01, 0x18, 0x01 };

	return ((shutter_open ? 2 : 1) << 6) | 0x20 | card_sensors[where];
}

/* The result of the command acknowledged: pending is CMD, its three bytes,
 * its parameters */
void
CardReader::answer ()
{
	std::vector<uint8_t> data;
	uint8_t s2 = '0', s3 = '0';

	if (pending.empty ())
		return;
	const uint8_t cmd = pending[0];
	const uint8_t *p = pending.data () + 4;
	const size_t np = pending.size () - 4;
	unsigned first = 0, count = 0;

	switch (cmd) {
	case 0x10:	/* Initialize: a card inside is given back */
		if (where == INSIDE)
			where = EJECTED;
		break;
	case 0x33:	/* Read */
	case 0x53:	/* Write */
		if (np >= 3) {
			static const uint8_t firsts[] = { 0, 1, 2, 0, 0, 1, 0 };
			static const uint8_t counts[] = { 1, 1, 1, 2, 2, 2, 3 };
			unsigned t = (unsigned) (p[2] - '0');
			if (t < 7) {
				first = firsts[t];
				count = counts[t];
			}
		}
		/* A card at the slot is taken in */
		if (where == AT_SLOT)
			where = INSIDE;
		if (where != INSIDE) {
			s3 = '4';
			break;
		}
		if (cmd == 0x33) {
			/* Tracks 1 and 3 */
			if (np >= 3 && p[2] == '4') {
				data.insert (data.end (), card, card + TRACK);
				data.insert (data.end (), card + 2 * TRACK, card + 3 * TRACK);
			} else
				data.insert (data.end (), card + first * TRACK,
				             card + (first + count) * TRACK);
		} else if (np >= 3 + TRACK * count) {
			if (p[2] == '4') {
				memcpy (card, p + 3, TRACK);
				memcpy (card + 2 * TRACK, p + 3 + TRACK, TRACK);
			} else
				memcpy (card + first * TRACK, p + 3, TRACK * count);
			save_card ();
		} else
			s2 = '2';
		break;
	case 0x80:	/* Eject */
		if (where == INSIDE)
			where = EJECTED;
		break;
	case 0xB0:	/* A blank card from the stocker; '2', only checked */
		if (np >= 1 && p[0] == '2')
			break;
		if (where != NO_CARD) {
			s3 = '3';
			break;
		}
		keep_old_card ();
		memset (card, 0, sizeof (card));
		where = INSIDE;
		HL_PRINT ("card reader: a blank card from the stocker");
		break;
	case 0xD0:	/* The shutter */
		if (np >= 1)
			shutter_open = p[0] == '1';
		break;
	case 0x40:	/* Cancel; with a card inside, S3 '4': VON4 then
			 * reads and writes it at the end of a game, where '0'
			 * makes it report the card stuck in the reader */
		if (where == INSIDE)
			s3 = '4';
		break;
	case 0x20:	/* The state */
	case 0xC1:	/* RETRY SET */
	case 0x78:	/* The printer's settings */
	case 0x7A:	/* A font */
	case 0x7C:	/* Print */
	case 0x7D:	/* Erase the printing */
	case 0xA0:	/* Clean */
		break;
	default:
		HL_PRINT ("card reader: unknown command %02X", cmd);
		break;
	}

	std::vector<uint8_t> frame = { STX, 0, cmd, sensors (), s2, s3 };
	frame.insert (frame.end (), data.begin (), data.end ());
	frame.push_back (ETX);
	frame[1] = (uint8_t) (frame.size () - 1);
	uint8_t bcc = 0;
	for (size_t i = 1; i < frame.size (); i++)
		bcc ^= frame[i];
	frame.push_back (bcc);
	if (log)
		print_frame (">", frame.data (), frame.size ());
	out.insert (out.end (), frame.begin (), frame.end ());
}

void
CardReader::insert_card ()
{
	if (where != NO_CARD)
		return;
	memset (card, 0, sizeof (card));
	FILE *f = path.empty () ? NULL : fopen (path.c_str (), "rb");
	if (f) {
		size_t n = fread (card, 1, sizeof (card), f);
		fclose (f);
		HL_PRINT ("card reader: card '%s' inserted (%u bytes)", path.c_str (), (unsigned) n);
	} else
		HL_PRINT ("card reader: a blank card inserted");
	where = AT_SLOT;
}

void
CardReader::take_card ()
{
	if (where == EJECTED || where == AT_SLOT) {
		where = NO_CARD;
		HL_PRINT ("card reader: card taken");
	}
}

void
CardReader::toggle_card ()
{
	if (where == NO_CARD)
		insert_card ();
	else if (where == INSIDE)
		HL_PRINT ("card reader: the card is inside, until the game gives it back");
	else
		take_card ();
}

void
CardReader::frame ()
{
	bool now = GameControls::get ().pressed ("Card");

	if (now && !held)
		toggle_card ();
	held = now;
	/* The player takes the card the reader gave back */
	if (where == EJECTED) {
		if (++ejected_frames >= TAKEN_FRAMES)
			take_card ();
	} else
		ejected_frames = 0;
}

/* Before a new card from the stocker: the player's card file, if any, kept
 * as von4.1.card (the first number free) */
void
CardReader::keep_old_card ()
{
	std::error_code error;
	std::filesystem::path old (path);

	if (path.empty () || !std::filesystem::exists (old, error))
		return;
	for (unsigned n = 1; n < 1000; n++) {
		std::filesystem::path kept = old;
		kept.replace_extension (std::to_string (n) + old.extension ().string ());
		if (std::filesystem::exists (kept, error))
			continue;
		std::filesystem::rename (old, kept, error);
		if (!error)
			HL_PRINT ("card reader: the card before kept as '%s'", kept.string ().c_str ());
		return;
	}
}

void
CardReader::save_card ()
{
	if (path.empty ())
		return;
	std::filesystem::path target (path);
	std::error_code error;
	if (target.has_parent_path ())
		std::filesystem::create_directories (target.parent_path (), error);
	FILE *f = fopen (path.c_str (), "wb");
	if (!f) {
		HL_ERROR ("card reader: cannot write '%s'", path.c_str ());
		return;
	}
	fwrite (card, 1, sizeof (card), f);
	fclose (f);
	HL_PRINT ("card reader: card written to '%s'", path.c_str ());
}

void
CardReader::reset (ResetType type)
{
	in.clear ();
	out.clear ();
	pending.clear ();
	if (type == RESET_HARD)
		shutter_open = false;
}

int
CardReader::save_state (State *state)
{
	uint32_t n_in = (uint32_t) in.size (), n_out = (uint32_t) out.size ();
	uint32_t n_pending = (uint32_t) pending.size ();
	std::vector<uint8_t> o (out.begin (), out.end ());
	int ret = state->put (&n_in, 4);

	ret |= state->put (in.data (), n_in);
	ret |= state->put (&n_out, 4);
	ret |= state->put (o.data (), n_out);
	ret |= state->put (&n_pending, 4);
	ret |= state->put (pending.data (), n_pending);
	ret |= state->put (&where, 1);
	ret |= state->put (&shutter_open, 1);
	ret |= state->put (&ejected_frames, 4);
	ret |= state->put (card, sizeof (card));
	return ret;
}

int
CardReader::load_state (State *state)
{
	uint32_t n_in = 0, n_out = 0, n_pending = 0;
	int ret = state->get (&n_in, 4);

	if (ret || n_in > 4096)
		return -1;
	in.resize (n_in);
	ret |= state->get (in.data (), n_in);
	ret |= state->get (&n_out, 4);
	if (ret || n_out > 4096)
		return -1;
	std::vector<uint8_t> o (n_out);
	ret |= state->get (o.data (), n_out);
	out.assign (o.begin (), o.end ());
	ret |= state->get (&n_pending, 4);
	if (ret || n_pending > 4096)
		return -1;
	pending.resize (n_pending);
	ret |= state->get (pending.data (), n_pending);
	ret |= state->get (&where, 1);
	ret |= state->get (&shutter_open, 1);
	ret |= state->get (&ejected_frames, 4);
	ret |= state->get (card, sizeof (card));
	return ret;
}

const char *
CardReader::debug_string ()
{
	static const char *const wheres[] = { "no card", "a card at the slot", "a card inside", "a card given back" };

	snprintf (text, sizeof (text), "%s, shutter %s, %u bytes to send",
	          wheres[where], shutter_open ? "open" : "closed", (unsigned) out.size ());
	return text;
}

bool
CardReader::set_option (const char *name, int value)
{
	if (!strcmp (name, "log"))
		log = value != 0;
	else if (!strcmp (name, "insert"))
		insert_card ();
	else if (!strcmp (name, "take"))
		take_card ();
	else
		return false;
	return true;
}

} /* namespace hl */
