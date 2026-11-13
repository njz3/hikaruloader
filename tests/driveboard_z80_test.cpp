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

/* The Model 3 drive board's Z80 (src/hl/driveboard-z80.cpp) against a
 * journal of Supermodel's: the same scenario run as Supermodel's harness
 * runs it (frames of 139072 cycles, 8 MHz / 57.524; the main board's writes
 * queued, a byte equal to the last one dropped, one shown a frame; the
 * wheel), then what the program wrote compared, NMI by NMI: the reply (29),
 * the servo board's register and value (2E, 2A), the lamps (2D). Needs the
 * EPROM, EPR-20512 (64 KB, CRC32 CF64350D), which is not ours to ship: not
 * run by ctest.
 *
 * Passes when the outputs change the same way in the same order, whatever
 * NMIs the changes come in: the NMIs fall a little apart in the frames (a
 * Z80 core's cycles), so a command at a frame's start can be seen an NMI
 * sooner or later. With a journal of Supermodel's Z80 counting Zilog's
 * T-states (journal_z80full.txt: DJNZ, the conditional JR, RET and CALL,
 * LDIR and LDDR), the NMIs fall in the same frames but 66 of 21776, the
 * outputs differ at one NMI. With DJNZ and LDIR alone (journal_z80fix.txt),
 * the changes come at most 1 NMI apart. With its earlier core, which counted
 * fewer, its board starts 2 frames sooner: OFFSET 17 compares the NMIs so.
 * DUMP shows both sides around an NMI.
 *
 * EPR-19338A (Scud Race's, Jambo! Safari's here; CRC32 C9FAC464), DIP CF FF,
 * a scenario of all its commands (C1, C6, C7; the road effects 01 to 07, the
 * springs 10 to 17, the gains 20 to 2F, the pushes 5x and 6x, the idles 9x,
 * the vibrations 3x, the power slides 41 and 49; Jambo's sequences; C9, C8,
 * CA and its 80 to 87, D0 to D7, F5, FA, CB and the start-up again; 5462
 * frames, 47331 NMIs): the wheel at its centre, the 5461 changes the same,
 * at most 1 NMI apart. With the wheel swept for 240 frames (the torque by
 * its position), 47166 NMIs the same, 138 within an NMI, 25 a step or two of
 * the torque apart, all in the sweep: the wheel read in the frame before or
 * after, at the NMIs that fall in another frame (Supermodel's harness loses
 * each instruction's overshoot at the NMI; the two cores count the same
 * cycles from the reset to the CTC's start, 1959726, and the same for every
 * instruction the program uses).
 *
 * Usage: driveboardz80test epr-20512.bin script.txt journal.txt [OFFSET [DUMP]];
 * driveboardz80test ROM --boot, the cycles from the reset to the CTC's start,
 * by address (as Supermodel's z80_drive.exe --boot). DRIVEBOARD_LIST=1: our
 * outputs' changes, NMI by NMI; DRIVEBOARD_FRAMES=1: every NMI in another
 * frame. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "hl/driveboard-z80.h"

struct Line {
	int cmd, wheel, reply, mode, tq, out;
};

static const Line none = { -1, -1, -1, -1, -1, -1 };

/* "--": none */
static int
field (const char *s)
{
	unsigned v;
	return sscanf (s, "%x", &v) == 1 ? (int) v : -1;
}

/* --boot: the Z80 alone from its reset to its CTC's start (channel 1's time
 * constant written), the program's ROM checksum and RAM test, one
 * instruction at a time: the cycles of each address, as Supermodel's
 * z80_drive.exe --boot prints them, to compare the two cores' counts */
class BootBus : public Z80::Bus {
public:
	std::vector<uint8_t> &rom;
	uint8_t ram[0x2000];
	bool started = false, constant_next = false;

	explicit BootBus (std::vector<uint8_t> &rom) : rom (rom) { memset (ram, 0, sizeof (ram)); }
	uint8_t read (uint16_t a) override { return a < 0x9000 ? rom[a] : a >= 0xE000 ? ram[a - 0xE000] : 0xFF; }
	void write (uint16_t a, uint8_t v) override { if (a >= 0xE000) ram[a - 0xE000] = v; }
	uint8_t in (uint16_t port) override
	{
		port &= 0xFF;
		return port == 0x20 ? 0xCF : port == 0x21 ? 0xFF : port == 0x2C ? 0x00 : 0xFF;
	}
	void out (uint16_t port, uint8_t v) override
	{
		if ((port & 0xFF) != 0x11)
			return;
		if (constant_next)
			started = true;
		constant_next = !constant_next && (v & 0x05) == 0x05;
	}
};

static int
boot_cycles (std::vector<uint8_t> &rom)
{
	BootBus bus (rom);
	Z80 z80 (&bus);
	static long long count[0x10000], cycles[0x10000];
	long long total = 0;

	z80.reset ();
	while (!bus.started && total < 100000000) {
		uint16_t pc = z80.regs.pc;
		int c = z80.step ();
		count[pc]++;
		cycles[pc] += c;
		total += c;
	}
	printf ("cycles from reset to the CTC's start: %lld\n", total);
	for (int a = 0; a < 0x10000; a++)
		if (count[a])
			printf ("%04X %lld %lld\n", a, count[a], cycles[a]);
	return 0;
}

int
main (int argc, char **argv)
{
	struct Event { long frame; std::string what; int value; };
	std::vector<uint8_t> rom (0x10000);
	std::map<uint64_t, Line> want, got;
	/* The frame each NMI ran in */
	std::map<uint64_t, long> want_frame, got_frame;
	std::vector<Event> events;
	char buf[256];
	long end = -1;
	FILE *fp;

	if (argc == 3 && !strcmp (argv[2], "--boot"))
		;
	else if (argc < 4) {
		puts ("usage: driveboardz80test epr-20512.bin script.txt journal.txt [OFFSET [DUMP]]");
		return 0;
	}
	fp = fopen (argv[1], "rb");
	if (!fp || fread (rom.data (), 1, rom.size (), fp) != rom.size ()) {
		printf ("cannot read %s\n", argv[1]);
		return 1;
	}
	fclose (fp);
	if (!strcmp (argv[2], "--boot"))
		return boot_cycles (rom);

	/* The scenario: "FRAME cmd XX", "FRAME wheel XX", "FRAME end" */
	fp = fopen (argv[2], "r");
	if (!fp) {
		printf ("cannot read %s\n", argv[2]);
		return 1;
	}
	while (fgets (buf, sizeof (buf), fp)) {
		char *hash = strchr (buf, '#'), what[32];
		unsigned value = 0;
		long frame;
		if (hash)
			*hash = 0;
		if (sscanf (buf, "%ld %31s %x", &frame, what, &value) < 2)
			continue;
		if (!strcmp (what, "end")) {
			end = frame;
			break;
		}
		events.push_back (Event { frame, what, (int) value });
	}
	fclose (fp);

	/* The journal: "NMI FRAME CMD28 WHEEL25 REPLY29 MODE2E TQ2A OUT2D" */
	fp = fopen (argv[3], "r");
	if (!fp) {
		printf ("cannot read %s\n", argv[3]);
		return 1;
	}
	while (fgets (buf, sizeof (buf), fp)) {
		char c[8][16];
		unsigned long long nmi;
		if (buf[0] == '#' || sscanf (buf, "%llu %15s %15s %15s %15s %15s %15s %15s", &nmi,
		                             c[1], c[2], c[3], c[4], c[5], c[6], c[7]) != 8)
			continue;
		want[nmi] = Line { field (c[2]), field (c[3]), field (c[4]), field (c[5]), field (c[6]), field (c[7]) };
		want_frame[nmi] = atol (c[1]);
	}
	fclose (fp);

	hl::DriveBoardZ80 board (rom.data (), rom.size ());
	board.supermodel_timing = true;
	board.observer = [&] (uint64_t nmi, uint8_t port, uint8_t val) {
		Line &l = got.emplace (nmi, none).first->second;
		switch (port) {
		case 0x28: l.cmd = val; break;
		case 0x25: l.wheel = val; break;
		case 0x29: l.reply = val; break;
		case 0x2E: l.mode = val; break;
		case 0x2A: l.tq = val; break;
		case 0x2D: l.out = val; break;
		}
	};

	/* Frame by frame, as Supermodel's CDriveBoard::Write and RunFrame */
	std::deque<uint8_t> queue;
	int written = -1;
	uint8_t shown = 0, wheel = 0x80;
	size_t e = 0;
	long first_nmi_frame = -1;
	for (long f = 0; f < end; f++) {
		for (; e < events.size () && events[e].frame <= f; e++) {
			if (events[e].what == "cmd" && events[e].value != written) {
				if (queue.size () >= 16)
					queue.pop_front ();
				queue.push_back ((uint8_t) events[e].value);
				written = events[e].value;
			} else if (events[e].what == "wheel")
				wheel = (uint8_t) events[e].value;
		}
		if (!queue.empty ()) {
			shown = queue.front ();
			queue.pop_front ();
		}
		board.set_inputs (shown, wheel);
		uint64_t before = board.nmis ();
		board.run (139072);
		for (uint64_t n = before + 1; n <= board.nmis (); n++)
			got_frame[n] = f;
		if (first_nmi_frame < 0 && board.nmis ())
			first_nmi_frame = f;
	}
	printf ("first NMI in frame %ld\n", first_nmi_frame);

	/* Where the NMIs fall in the frames (the main board's commands come
	 * at the frames' starts, so an NMI in another frame can see one a
	 * NMI sooner or later) */
	unsigned other_frame = 0;
	for (const auto &w : want_frame) {
		auto it = got_frame.find (w.first);
		if (it == got_frame.end () || it->second == w.second)
			continue;
		/* DRIVEBOARD_FRAMES=1: all of them */
		if (other_frame++ < 5 || getenv ("DRIVEBOARD_FRAMES"))
			printf ("NMI %llu: in frame %ld, ours in %ld\n",
			        (unsigned long long) w.first, w.second, it->second);
	}
	printf ("%u NMIs in another frame\n", other_frame);

	/* DUMP: the NMIs around one, both sides */
	if (argc > 5) {
		long long at = atoll (argv[5]);
		for (long long n = at - 8; n <= at + 8; n++) {
			auto a = want.find ((uint64_t) n), b = got.find ((uint64_t) n);
			const Line wa = a != want.end () ? a->second : none, gb = b != got.end () ? b->second : none;
			printf ("NMI %lld: frame %ld 28=%02X 29=%02X 2E=%02X 2A=%02X | ours frame %ld 28=%02X 29=%02X 2E=%02X 2A=%02X\n",
			        n, want_frame.count ((uint64_t) n) ? want_frame[(uint64_t) n] : -1L,
			        wa.cmd & 0xFF, wa.reply & 0xFF, wa.mode & 0xFF, wa.tq & 0xFF,
			        got_frame.count ((uint64_t) n) ? got_frame[(uint64_t) n] : -1L,
			        gb.cmd & 0xFF, gb.reply & 0xFF, gb.mode & 0xFF, gb.tq & 0xFF);
		}
	}

	/* Our NMIs counted from where ours started: optionally shifted */
	long offset = argc > 4 ? atol (argv[4]) : 0;
	unsigned checked = 0, differ = 0;
	for (const auto &w : want) {
		const Line &a = w.second;
		auto it = got.find ((uint64_t) ((long long) w.first - offset));
		const Line b = it != got.end () ? it->second : none;
		const int wa[4] = { a.reply, a.mode, a.tq, a.out }, gb[4] = { b.reply, b.mode, b.tq, b.out };
		checked++;
		if (memcmp (wa, gb, sizeof (wa))) {
			if (differ++ < 20)
				printf ("NMI %llu: want 29=%02X 2E=%02X 2A=%02X 2D=%02X, got %02X %02X %02X %02X (28 %02X, 25 %02X)\n",
				        (unsigned long long) w.first, a.reply & 0xFF, a.mode & 0xFF, a.tq & 0xFF, a.out & 0xFF,
				        b.reply & 0xFF, b.mode & 0xFF, b.tq & 0xFF, b.out & 0xFF, b.cmd & 0xFF, b.wheel & 0xFF);
		}
	}
	printf ("driveboardz80test: %u NMIs, %u differ\n", checked, differ);

	/* The outputs' changes (29, 2E, 2A, 2D), whatever NMIs they come in:
	 * the same ones in the same order, and how many NMIs apart */
	auto changes = [] (const std::map<uint64_t, Line> &m) {
		std::vector<std::pair<uint64_t, Line>> v;
		Line last = none;
		for (const auto &x : m) {
			const Line &l = x.second;
			if (l.reply != last.reply || l.mode != last.mode || l.tq != last.tq || l.out != last.out)
				v.push_back (x);
			last = l;
		}
		return v;
	};
	const auto wc = changes (want), gc = changes (got);
	/* DRIVEBOARD_LIST=1: our changes, each with its NMI, its frame and the
	 * command then (another program than EPR-20512, a journal empty) */
	if (getenv ("DRIVEBOARD_LIST"))
		for (const auto &c : gc)
			printf ("NMI %llu, frame %ld: 28=%02X 29=%02X 2E=%02X 2A=%02X 2D=%02X\n",
			        (unsigned long long) c.first, got_frame[c.first], c.second.cmd & 0xFF,
			        c.second.reply & 0xFF, c.second.mode & 0xFF, c.second.tq & 0xFF, c.second.out & 0xFF);
	size_t same = 0;
	long long apart = 0;
	for (; same < wc.size () && same < gc.size (); same++) {
		const Line &a = wc[same].second, &b = gc[same].second;
		if (a.reply != b.reply || a.mode != b.mode || a.tq != b.tq || a.out != b.out) {
			printf ("change %zu (NMI %llu): want 29=%02X 2E=%02X 2A=%02X 2D=%02X, got %02X %02X %02X %02X\n", same,
			        (unsigned long long) wc[same].first, a.reply & 0xFF, a.mode & 0xFF, a.tq & 0xFF, a.out & 0xFF,
			        b.reply & 0xFF, b.mode & 0xFF, b.tq & 0xFF, b.out & 0xFF);
			break;
		}
		long long d = llabs ((long long) wc[same].first - offset - (long long) gc[same].first);
		if (d > apart)
			apart = d;
	}
	printf ("driveboardz80test: %zu changes, ours %zu, the first %zu the same, at most %lld NMIs apart\n",
	        wc.size (), gc.size (), same, apart);
	return same == wc.size () && same == gc.size () ? 0 : 1;
}
