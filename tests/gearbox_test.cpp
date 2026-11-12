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

/* The gear lever's rules (SPECS.md, "The gear lever"), by a harness: each
 * scenario a gearbox (the gears a game has), then a script of the
 * machine's frames, each the buttons pressed and let go before it and the
 * gear the game gets in it (hl::Gearbox). Then the games' own levers from
 * data/games.json: their gears and the switches each closes
 * (hl::GameControls). Run by ctest, or alone: gearboxtest
 * <data/games.json>.
 *
 * A frame's token: its events, then "=" and the gear expected (left out:
 * not checked); "." a frame with no event. An event: "+" pressed or "-"
 * let go, then the button: R (GearReverse), N (GearNeutral), 1 to 6
 * (Gear1 to Gear6), U (ShiftUp), D (ShiftDown). A gear: R, N, 1 to 6.
 * "+2+3=3": Gear2 and Gear3 pressed in the same frame, gear 3. The first
 * frame is the game's start: a shift pressed in it is held from the
 * start, no event. */

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "hl/config.h"
#include "hl/controls.h"
#include "hl/gamecontrols.h"
#include "hl/games.h"
#include "hl/gearbox.h"

static int failures, checks;

static void
check (const std::string &name, bool ok)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf (stderr, "FAIL %s\n", name.c_str ());
	}
}

/* The button's row of [Driving] */
static const char *
button_key (char b)
{
	switch (b) {
	case 'R': return "GearReverse";
	case 'N': return "GearNeutral";
	case 'U': return "ShiftUp";
	case 'D': return "ShiftDown";
	case '1': return "Gear1";
	case '2': return "Gear2";
	case '3': return "Gear3";
	case '4': return "Gear4";
	case '5': return "Gear5";
	case '6': return "Gear6";
	}
	return NULL;
}

static std::string
gear_name (int gear)
{
	return gear == -1 ? "R" : gear == 0 ? "N" : std::to_string (gear);
}

/* NASCAR Arcade's four gears and neutral (H4), F355's six (H6, its paddles
 * its own), 18 Wheeler's reverse, neutral, low and high, Crazy Taxi's and
 * The King of Route 66's drive and reverse, no neutral */
static const std::vector<int> H4 = { 0, 1, 2, 3, 4 }, H6 = { 0, 1, 2, 3, 4, 5, 6 },
                              TRUCK = { -1, 0, 1, 2 }, DRIVE = { -1, 1 };

struct Scenario {
	const char	*name;
	const std::vector<int> *gears;
	bool		 paddles;
	int		 neutral_ms;	/* SHIFT_NEUTRAL_MS */
	const char	*script;
};

static const Scenario scenarios[] = {
	/* The game's start */
	{ "start: none held, neutral", &H4, true, 0, ".=N .=N" },
	{ "start: a button held, its gear", &H4, true, 0, "+3=3 .=3" },
	{ "start: all held, the highest", &H4, true, 0, "+1+2+3+4=4 .=4" },
	{ "start: H6 all held, the highest", &H6, false, 0, "+1+2+3+4+5+6=6" },
	{ "start: neutral held, neutral", &H4, true, 0, "+N=N" },
	{ "start: neutral and a gear held, the gear", &H4, true, 0, "+N+2=2" },
	{ "start: the held gives no event after", &H4, true, 0, "+2=2 +U=3 -U=3 .=3 -2=3" },
	{ "start: let go after, the gear kept", &H4, true, 0, "+1+2+3+4=4 -4=4 -3-2-1=4 .=4" },
	{ "start: no neutral, the first forward gear", &DRIVE, true, 0, ".=1" },
	{ "start: reverse held", &TRUCK, true, 0, "+R=R .=R" },
	{ "start: a shift held, no event", &H4, true, 0, "+U=N -U=N +U=1" },
	{ "start: reverse and high held, high", &TRUCK, true, 0, "+R+2=2" },

	/* A button pressed: its gear, kept once let go */
	{ "event: pressed, its gear", &H4, true, 0, "=N +3=3 -3=3 .=3 .=3" },
	{ "event: each button pressed", &H4, true, 0, "+1=1 -1=1 +4=4 -4=4 +2=2 -2=2" },
	{ "event: H6's fifth and sixth", &H6, false, 0, "+5=5 -5=5 +6=6 -6=6 .=6" },
	{ "event: the lever moved through the middle", &H4, true, 0, "+2=2 -2=2 .=2 +3=3 -3=3" },

	/* Held: no new event, the gear stays */
	{ "held: a shift over it stays", &H4, true, 0, "+2=2 +U=3 -U=3 .=3 .=3 -2=3" },
	{ "held: down over it stays", &H4, true, 0, "+3=3 +D=2 -D=2 .=2" },

	/* Several held: the last pressed */
	{ "several: the last pressed", &H4, true, 0, "+1=1 +3=3 +2=2 .=2" },
	{ "several: letting go changes nothing", &H4, true, 0, "+1=1 +3=3 -3=3 .=3 -1=3" },
	{ "several: pressed in a frame, the highest", &H4, true, 0, "=N +1+4=4" },

	/* Neutral: by its button, kept until a gear's event */
	{ "neutral: its button", &H4, true, 0, "+3=3 -3+N=N -N=N .=N +2=2" },
	{ "neutral: held, a shift up", &H4, true, 0, "+N=N +U=1 -U=1 .=1" },
	{ "neutral: a game without, nothing", &DRIVE, true, 0, ".=1 +N=1 -N=1" },

	/* Shift up and down: the next gear, neutral not one */
	{ "paddles: up from neutral", &H4, true, 0, "=N +U=1 -U=1 +U=2 -U=2" },
	{ "paddles: up to the top", &H4, true, 0, "=N +U=1 -U +U=2 -U +U=3 -U +U=4 -U +U=4 -U=4" },
	{ "paddles: down stops at the first", &H4, true, 0, "+3=3 -3=3 +D=2 -D +D=1 -D=1 +D=1 -D=1" },
	{ "paddles: down at neutral, nothing below", &H4, true, 0, "=N +D=N -D=N" },
	{ "paddles: held, a gear a press", &H4, true, 0, "=N +U=1 .=1 .=1 -U=1" },
	{ "paddles: after a button", &H4, true, 0, "+3=3 -3=3 +U=4 -U=4 +D=3" },
	{ "paddles: F355's own, the lever's alone", &H6, false, 0, "+3=3 -3 +U=3 -U +D=3" },
	{ "paddles: 18 Wheeler down to reverse", &TRUCK, true, 0, "=N +U=1 -U +D=R -D=R +U=1 -U +U=2 -U=2" },
	{ "paddles: drive and reverse", &DRIVE, true, 0, "=1 +D=R -D=R +U=1 -U=1" },
	{ "paddles: down from neutral, reverse", &TRUCK, true, 0, "=N +D=R -D=R +U=1" },
	{ "paddles: up past the top, nothing", &TRUCK, true, 0, "+2=2 -2 +U=2" },

	/* Through neutral SHIFT_NEUTRAL_MS from a gear to another: 50 ms,
	 * three frames */
	{ "crossing: a button, three frames", &H4, true, 50, "+2=2 -2=2 +3=N .=N .=N .=3" },
	{ "crossing: the paddles", &H4, true, 50, "=N +U=1 -U=1 +U=N -U=N .=N .=2" },
	{ "crossing: none from neutral", &H4, true, 50, "+N=N -N=N +1=1" },
	{ "crossing: none to neutral", &H4, true, 50, "+2=2 +N=N" },
	{ "crossing: to reverse", &TRUCK, true, 50, "+1=1 +R=N .=N .=N .=R" },
	{ "crossing: none in a game without neutral", &DRIVE, true, 50, "=1 +R=R -R=R +1=1" },
	{ "crossing: off, at once", &H4, true, 0, "+2=2 +3=3" },
};

static void
run (const Scenario &s)
{
	hl::Gearbox &g = hl::Gearbox::get ();
	hl::Controls &c = hl::Controls::get ();
	static uint64_t frame = 1;
	std::string script = s.script;
	size_t at = 0;
	int n = 0;

	c.release_held (NULL, NULL);
	g.hold (hl::Gearbox::NO_GEAR);
	g.reset ();
	hl::Config::get ()->shift_neutral_ms = s.neutral_ms;
	while (at < script.size ()) {
		size_t end = script.find (' ', at);
		std::string token = script.substr (at, end == std::string::npos ? std::string::npos : end - at);
		size_t i = 0;
		int got;

		at = end == std::string::npos ? script.size () : end + 1;
		if (token.empty ())
			continue;
		n++;
		for (; i + 1 < token.size () && (token[i] == '+' || token[i] == '-'); i += 2) {
			const char *key = button_key (token[i + 1]);

			if (!key) {
				check (std::string (s.name) + ": bad token " + token, false);
				break;
			}
			if (token[i] == '+')
				c.hold ("Driving", key, 1.0);
			else
				c.release_held ("Driving", key);
		}
		got = g.gear (frame++, *s.gears, s.paddles);
		if (i < token.size () && token[i] == '.')
			i++;
		if (i < token.size () && token[i] == '=') {
			std::string want = token.substr (i + 1);

			check (std::string (s.name) + ": frame " + std::to_string (n) + " (" + token + "), got " +
			       gear_name (got), gear_name (got) == want);
		}
	}
	c.release_held (NULL, NULL);
}

/* The agent's gear over the controls ("set Gear"), and a frame looked at
 * once */
static void
test_agent (void)
{
	hl::Gearbox &g = hl::Gearbox::get ();
	hl::Controls &c = hl::Controls::get ();
	uint64_t frame = 1000000;

	g.reset ();
	c.release_held (NULL, NULL);
	check ("agent: neutral", g.gear (frame++, H4) == 0);
	g.hold (4);
	check ("agent: its gear", g.gear (frame++, H4) == 4);
	c.hold ("Driving", "Gear2", 1.0);
	check ("agent: over a button", g.gear (frame++, H4) == 4);
	g.hold (hl::Gearbox::NO_GEAR);
	check ("agent: let go, the button's", g.gear (frame++, H4) == 2);
	c.release_held (NULL, NULL);
	check ("agent: a frame looked at once", g.gear (frame - 1, H4) == 2);
	g.reset ();
	check ("reset: neutral", g.gear (frame++, H4) == 0);
}

/* The games' levers from data/games.json: their gears, and the switches a
 * gear closes on their I/O board */
static void
test_games (hl::GameList *list)
{
	hl::GameControls &gc = hl::GameControls::get ();
	hl::Controls &c = hl::Controls::get ();
	uint64_t frame = 2000000;
	struct {
		const char *game;
		std::vector<int> gears;
		const char *button;	/* held from the start */
		const char *port;
		uint32_t bits;		/* the gear's switches */
	} games[] = {
		{ "sgnascar", H4, "Gear3", "p2", 1u << 13 },
		{ "sgnascar", H4, "Gear1", "p2", (1u << 13) | (1u << 11) },
		{ "f355", H6, "Gear6", "p2", (1u << 12) | (1u << 10) },
		{ "18wheelr", TRUCK, "GearReverse", "p2", (1u << 12) | (1u << 11) },
		{ "18wheelr", TRUCK, "Gear2", "p2", 1u << 13 },
		{ "kingrt66", DRIVE, "GearReverse", "p2", 1u << 12 },
		{ "kingrt66", DRIVE, "Gear1", "p2", 1u << 13 },
		{ "kingrt66p", DRIVE, "Gear1", "p2", 1u << 13 },
		{ "crzytaxi", DRIVE, "GearReverse", "p1", 1u << 12 },
	};

	if (!list) {
		check ("games: the game list", false);
		return;
	}
	for (const auto &t : games) {
		std::string name = std::string (t.game) + " " + t.button;
		uint32_t gear_bits = 0, bits;

		gc.set_game (list, t.game);
		check (name + ": its gears", gc.gears () == t.gears);
		c.release_held (NULL, NULL);
		c.hold ("Driving", t.button, 1.0);
		bits = gc.switches (t.port, frame++);
		/* The bits a gear may close, of any of the game's gears */
		for (uint32_t b : { 10u, 11u, 12u, 13u })
			gear_bits |= 1u << b;
		check (name + ": its switches", (bits & gear_bits) == t.bits);
		c.release_held (NULL, NULL);
		bits = gc.switches (t.port, frame++);
		check (name + ": let go, kept", (bits & gear_bits) == t.bits);
	}
	/* The King of Route 66 with nothing held drives forward: no neutral */
	gc.set_game (list, "kingrt66");
	c.release_held (NULL, NULL);
	check ("kingrt66: forward at the start", (gc.switches ("p2", frame++) & (3u << 12)) == 1u << 13);
	gc.set_game (NULL, NULL);
}

int
main (int argc, char **argv)
{
	hl::GameList *list = argc > 1 ? hl::GameList::load (argv[1]) : NULL;

	for (const Scenario &s : scenarios)
		run (s);
	test_agent ();
	test_games (list);
	hl::Config::get ()->shift_neutral_ms = 0;
	printf ("gearboxtest: %d scenarios, %d checks, %d failures\n",
	        (int) (sizeof (scenarios) / sizeof (scenarios[0])), checks, failures);
	return failures ? 1 : 0;
}
