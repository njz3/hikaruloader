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

/* The card reader's tests: its frames (ACK, NAK, the answer on ENQ), the
 * card at the slot taken in by a read, the tracks written to the card's file
 * and read back, the stocker's blank card (the file before kept), the eject.
 * The frames are VON4's. Run by ctest, or alone: cardreadertest. */

#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <vector>

#include "hl/cardreader.h"
#include "hl/machine.h"

static int failures, checks;

#define CHECK(name_, got_, want_) \
	do { \
		unsigned g_ = (unsigned) (got_), w_ = (unsigned) (want_); \
		checks++; \
		if (g_ != w_) { \
			failures++; \
			fprintf (stderr, "FAIL %s: %s = %X, expected %X\n", name_, #got_, g_, w_); \
		} \
	} while (0)

class TestMachine : public hl::Machine {
public:
	TestMachine () : hl::Machine (NULL) {}
	int run_frame () override { return 0; }
	const char *debug_string () override { return "cardreadertest"; }
};

static hl::CardReader *reader;

/* A command's frame: STX LEN CMD 00 00 00 parameters ETX BCC */
static std::vector<uint8_t>
frame (uint8_t cmd, const std::vector<uint8_t> &params)
{
	std::vector<uint8_t> f = { 0x02, 0, cmd, 0, 0, 0 };
	f.insert (f.end (), params.begin (), params.end ());
	f.push_back (0x03);
	f[1] = (uint8_t) (f.size () - 1);
	uint8_t bcc = 0;
	for (size_t i = 1; i < f.size (); i++)
		bcc ^= f[i];
	f.push_back (bcc);
	return f;
}

static std::vector<uint8_t>
take_bytes ()
{
	std::vector<uint8_t> out;
	while (reader->has_byte ())
		out.push_back (reader->send ());
	return out;
}

/* A command sent, acknowledged, then its answer asked for (ENQ) */
static std::vector<uint8_t>
command (const char *name, uint8_t cmd, const std::vector<uint8_t> &params)
{
	for (uint8_t b : frame (cmd, params))
		reader->receive (b);
	std::vector<uint8_t> ack = take_bytes ();
	CHECK (name, ack.size (), 1);
	CHECK (name, ack.empty () ? 0 : ack[0], 0x06);
	reader->receive (0x05);
	std::vector<uint8_t> answer = take_bytes ();
	if (answer.size () < 8) {
		failures++;
		fprintf (stderr, "FAIL %s: an answer of %u bytes\n", name, (unsigned) answer.size ());
		return answer;
	}
	/* STX LEN CMD S1 S2 S3 ... ETX BCC */
	uint8_t bcc = 0;
	for (size_t i = 1; i + 1 < answer.size (); i++)
		bcc ^= answer[i];
	CHECK (name, answer[0], 0x02);
	CHECK (name, answer[1], answer.size () - 2);
	CHECK (name, answer[2], cmd);
	CHECK (name, answer[answer.size () - 2], 0x03);
	CHECK (name, answer.back (), bcc);
	return answer;
}

/* SDL's main, through hl/core.h */
int
main (int argc, char **argv)
{
	(void) argc;
	(void) argv;
	std::error_code error;
	std::filesystem::path dir = std::filesystem::temp_directory_path () / "hl_cardreadertest";
	std::filesystem::remove_all (dir, error);
	std::filesystem::create_directories (dir, error);
	std::string path = (dir / "von4.card").string ();

	TestMachine *mach = new TestMachine ();
	reader = new hl::CardReader (mach, path);
	reader->reset (hl::RESET_HARD);

	/* A wrong BCC: NAK */
	std::vector<uint8_t> f = frame (0x20, {});
	f.back () ^= 0xFF;
	for (uint8_t b : f)
		reader->receive (b);
	std::vector<uint8_t> nak = take_bytes ();
	CHECK ("nak", nak.size (), 1);
	CHECK ("nak", nak.empty () ? 0 : nak[0], 0x15);

	/* The state: shutter closed, no card */
	std::vector<uint8_t> a = command ("state", 0x20, {});
	CHECK ("state", a.size () > 5 ? a[3] : 0, 0x60);
	CHECK ("state", a.size () > 5 ? a[5] : 0, '0');

	/* The shutter opened */
	a = command ("shutter", 0xD0, { '1' });
	CHECK ("shutter", a.size () > 5 ? a[3] : 0, 0xA0);

	/* A read with no card: S3 '4', no data */
	a = command ("read none", 0x33, { '0', '1', '6' });
	CHECK ("read none", a.size (), 8);
	CHECK ("read none", a.size () > 5 ? a[5] : 0, '4');

	/* A card at the slot (no file: blank), taken in by the read */
	reader->insert_card ();
	a = command ("read blank", 0x33, { '0', '1', '6' });
	CHECK ("read blank", a.size (), 8 + 3 * hl::CardReader::TRACK);
	CHECK ("read blank", a.size () > 5 ? a[3] & 0x1F : 0, 0x18);

	/* The three tracks written, then read back, track 2 alone too */
	std::vector<uint8_t> data = { '0', '1', '6' };
	for (unsigned i = 0; i < 3 * hl::CardReader::TRACK; i++)
		data.push_back ((uint8_t) (i * 7 + 1));
	a = command ("write", 0x53, data);
	CHECK ("write", a.size () > 5 ? a[5] : 0, '0');
	CHECK ("write file", std::filesystem::file_size (path, error), 3 * hl::CardReader::TRACK);
	a = command ("read 2", 0x33, { '0', '1', '1' });
	CHECK ("read 2", a.size (), 8 + hl::CardReader::TRACK);
	CHECK ("read 2", a.size () > 6 ? a[6] : 0, data[3 + hl::CardReader::TRACK]);

	/* Ejected: at the slot, then taken */
	a = command ("eject", 0x80, {});
	CHECK ("eject", a.size () > 5 ? a[3] & 0x1F : 0, 0x01);
	reader->take_card ();
	a = command ("state 2", 0x20, {});
	CHECK ("state 2", a.size () > 5 ? a[3] & 0x1F : 0, 0x00);

	/* The card comes back from its file */
	reader->insert_card ();
	a = command ("read file", 0x33, { '0', '1', '0' });
	CHECK ("read file", a.size () > 6 ? a[6] : 0, data[3]);
	command ("eject 2", 0x80, {});
	reader->take_card ();

	/* The stocker: '2' checks only; '1' takes a blank card in, the file
	 * before kept as von4.1.card */
	a = command ("stocker check", 0xB0, { '2' });
	CHECK ("stocker check", a.size () > 5 ? a[3] & 0x1F : 0, 0x00);
	a = command ("stocker", 0xB0, { '1' });
	CHECK ("stocker", a.size () > 5 ? a[3] & 0x1F : 0, 0x18);
	CHECK ("stocker kept", std::filesystem::exists (dir / "von4.1.card", error), 1);
	CHECK ("stocker moved", std::filesystem::exists (path, error), 0);
	a = command ("read new", 0x33, { '0', '1', '0' });
	CHECK ("read new", a.size () > 6 ? a[6] : 1, 0);

	std::filesystem::remove_all (dir, error);
	printf ("cardreadertest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
