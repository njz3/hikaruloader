/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/hopper.cpp (GPL-2.0-or-later,
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

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>

#include "hl/core.h"
#include "hl/input.h"
#include "hl/state.h"

#include "mach/naomi/naomi.h"

namespace naomi {

/*
 * The SWP Hopper Board
 * ====================
 *
 * The medal games' coin hopper is run by a NAOMI of its own (cartridge 840-0130,
 * expansion board 837-14381), which talks to the game's board through the SH-4's
 * SCIF. It holds the credits, the bets, the payouts and its settings, battery
 * backed; here they go in the game's nvram file. The coins go into it and not
 * into the JVS: Coin 1 is a coin in its chute.
 *
 * Messages, both ways, little endian:
 *
 *  'H'  command  size (u16, of the whole message)  payload...  checksum
 *
 * A checksum is the sum of the bytes before it. What the board answers is made
 * of 32-bit words, the last one lacking its top byte.
 */

enum {
	/* From the game */
	CMD_POWER_ON	= 0x20,
	CMD_GET_STATUS	= 0x21,
	CMD_GAME_START	= 0x22,
	CMD_GAME_END	= 0x23,
	CMD_TEST_START	= 0x24,
	CMD_BACKUP_CLEAR = 0x25,
	CMD_BET		= 0x26,
	CMD_SWITCH	= 0x29,
	CMD_CONFIG	= 0x2A,
	CMD_TEST_DATA	= 0x2F,
	CMD_COIN_IN_ACK	= 0x31,
	CMD_COIN_OUT_ACK = 0x32,
	/* From the board, unasked */
	MSG_COIN_IN	= 0x01,
	MSG_PAY_WIN	= 0x03,
	/* What it answers */
	ANS_POWER_ON	= 0x10,
	ANS_GET_STATUS	= 0x11,
	ANS_GAME_START	= 0x12,
	ANS_GAME_END	= 0x13,
	ANS_TEST_START	= 0x14,
	ANS_BACKUP_CLEAR = 0x15,
	ANS_BET		= 0x16,
	ANS_CONFIG	= 0x1A
};

enum {
	CURRENCY_MEDAL,
	CURRENCY_POUND,
	CURRENCY_DOLLAR,
	CURRENCY_EURO,
	CURRENCY_TOKEN
};

/* A coin is worth 100 of the credits' units */
static const uint32_t COIN_VALUE = 100;

/* No error, no door open */
static const uint32_t STATUS_OK = 0;

/* The texts the game shows for the board's errors, in the answer to POWER ON */
static const char ERROR_TEXTS[][32] = {
	"UNKNOWN ERROR",
	"ROM IS BAD",
	"LOW BATTERY",
	"ROM HAS CHANGED",
	"RAM DATA IS BAD",
	"I/O ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"COIN IN JAM(HOPPER)",
	"COIN IN JAM(GAME)",
	"HOPPER OVER PAID",
	"HOPPER RUNAWAY",
	"HOPPER EMPTY/JAM",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"COM. TIME OUT",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"I/O BOARD IS BAD",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"UNKNOWN ERROR",
	"ATP NONE\0\0\0\0\0\0\0\0MAX. PAY",
	"HOPPER SIZE\0\0\0\0\0MAX. CREDIT",
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0DOOR IS OPEN"
};

SwpHopper::SwpHopper ()
{
	saved = s;
}

void
SwpHopper::reset ()
{
	in.clear ();
	out.clear ();
	expected = 0;
	started = false;
}

bool
SwpHopper::load (const uint8_t *data, size_t size)
{
	if (size != sizeof (s))
		return false;
	memcpy (&s, data, size);
	saved = s;
	return true;
}

bool
SwpHopper::changed ()
{
	if (!memcmp (&s, &saved, sizeof (s)))
		return false;
	saved = s;
	return true;
}

int
SwpHopper::available ()
{
	return (int) out.size ();
}

uint8_t
SwpHopper::read ()
{
	uint8_t b;

	if (out.empty ())
		return 0;
	b = out.front ();
	out.pop_front ();
	return b;
}

void
SwpHopper::write (uint8_t value)
{
	if (in.empty () && value != 'H') {
		HL_LOG ("hopper: ignored byte %02X", value);
		return;
	}
	in.push_back (value);
	if (in.size () == 3) {
		expected = value;
	} else if (in.size () == 4) {
		expected |= value << 8;
		if (expected < 5) {
			HL_ERROR_FEW ("hopper: a message of %u bytes", expected);
			in.clear ();
			expected = 0;
		}
	} else if (in.size () > 4 && in.size () == expected) {
		command (in[1]);
		in.clear ();
		expected = 0;
	}
}

uint32_t
SwpHopper::arg (size_t offset) const
{
	uint32_t v = 0;

	if (offset + 4 <= in.size ())
		memcpy (&v, in.data () + offset, 4);
	return v;
}

void
SwpHopper::send (uint8_t cmd, const std::vector<uint32_t> &payload)
{
	size_t len = payload.size () * 4 - 1;
	size_t size = len + 5;
	const uint8_t *p = (const uint8_t *) payload.data ();
	const uint8_t head[4] = { 'H', cmd, (uint8_t) size, (uint8_t) (size >> 8) };
	uint8_t sum = 0;

	HL_LOG ("hopper: sending %02X, %u bytes", cmd, (unsigned) size);
	for (uint8_t b : head) {
		out.push_back (b);
		sum += b;
	}
	for (size_t i = 0; i < len; i++) {
		out.push_back (p[i]);
		sum += p[i];
	}
	out.push_back (sum);
}

void
SwpHopper::frame ()
{
	bool held = hl::Input::action (hl::ACTION_COIN1);

	if (held && !coin_held && started)
		coin_in ();
	coin_held = held;
}

void
SwpHopper::coin_in ()
{
	std::vector<uint32_t> p (7);

	s.credit0 += COIN_VALUE;
	p[1] = STATUS_OK;
	p[2] = s.credit0;
	p[3] = s.credit1;
	p[4] = s.total_credit;
	p[5] = s.premium;
	send (MSG_COIN_IN, p);
}

void
SwpHopper::send_pay_win ()
{
	std::vector<uint32_t> p (9);

	p[1] = STATUS_OK;
	p[2] = s.credit0;
	p[3] = s.credit1;
	p[4] = s.total_credit;
	p[5] = s.premium;
	p[6] = s.won;
	p[7] = s.paid;
	send (MSG_PAY_WIN, p);
}

/* The bets of the first two meters, each taken on the other when it has too
 * little, and of the premium */
void
SwpHopper::bet ()
{
	const uint32_t bets[2] = { arg (4), arg (8) };

	for (int i = 0; i < 2; i++) {
		uint32_t &first = i == 0 ? s.credit0 : s.credit1;
		uint32_t &second = i == 0 ? s.credit1 : s.credit0;

		if (first >= bets[i]) {
			first -= bets[i];
		} else {
			uint32_t rest = bets[i] - first;

			first = 0;
			second -= std::min (second, rest);
		}
	}
	s.premium -= std::min (s.premium, arg (12));
}

void
SwpHopper::pay_out (uint32_t value)
{
	if (value == 0)
		return;
	s.won += value;
	if (s.auto_pay_out) {
		s.paid += value;
		send_pay_win ();
	} else {
		s.credit0 += value;
	}
}

void
SwpHopper::set_defaults ()
{
	switch (s.currency) {
	case CURRENCY_MEDAL:
		s.medal_exchange = 5;
		s.two_way = 1;
		s.auto_pay_out = 0;
		s.coin_discrimination = 1;
		s.cur_base = 100;
		s.bet_button = 1;
		s.max_pay = 1999900;
		s.max_credit = 1999900;
		s.hopper_size = 39900;
		s.max_bet = 10000;
		s.min_bet = 1000;
		s.add_bet = 100;
		break;
	case CURRENCY_POUND:
		s.medal_exchange = 0;
		s.two_way = 0;
		s.auto_pay_out = 1;
		s.coin_discrimination = 0;
		s.cur_base = 5;
		s.bet_button = 0;
		s.max_pay = 4000;
		s.max_credit = 1999900;
		s.hopper_size = 1999900;
		s.max_bet = 100;
		s.min_bet = 100;
		s.add_bet = 50;
		break;
	case CURRENCY_DOLLAR:
	case CURRENCY_EURO:
		s.medal_exchange = 0;
		s.two_way = 0;
		s.auto_pay_out = 0;
		s.coin_discrimination = 0;
		s.cur_base = 5;
		s.bet_button = 1;
		s.max_pay = 1999900;
		s.max_credit = 1999900;
		s.hopper_size = 39900;
		s.max_bet = 10000;
		s.min_bet = 100;
		s.add_bet = 100;
		break;
	case CURRENCY_TOKEN:
		s.medal_exchange = 0;
		s.two_way = 0;
		s.auto_pay_out = 0;
		s.coin_discrimination = 0;
		s.cur_base = 100;
		s.bet_button = 1;
		s.max_pay = 1999900;
		s.max_credit = 1999900;
		s.hopper_size = 39900;
		s.max_bet = 10000;
		s.min_bet = 100;
		s.add_bet = 100;
		break;
	default:
		HL_ERROR_FEW ("hopper: unknown currency %u", s.currency);
		break;
	}
	s.total_credit = s.min_bet;
}

uint32_t
SwpHopper::play_options () const
{
	return s.free_play | (s.auto_pay_out << 8) | (s.two_way << 16) | (s.medal_exchange << 24);
}

uint32_t
SwpHopper::exchange_options () const
{
	return s.auto_exchange | (s.coin_discrimination << 8) | (s.cur_base << 16) | (s.bet_button << 24);
}

/* The board's own clock, in two words: year, month, day; day of the week, hour,
 * minute, second */
void
SwpHopper::put_clock (uint32_t *words) const
{
	time_t now = time (NULL);
	const struct tm *t = localtime (&now);

	if (!t)
		return;
	words[0] = (uint32_t) (t->tm_year + 1900) | ((uint32_t) (t->tm_mon + 1) << 16) | ((uint32_t) t->tm_mday << 24);
	words[1] = (uint32_t) t->tm_wday | ((uint32_t) t->tm_hour << 8) | ((uint32_t) t->tm_min << 16) | ((uint32_t) t->tm_sec << 24);
}

void
SwpHopper::power_on ()
{
	std::vector<uint32_t> p (0x13A);
	size_t offset = 0x84;
	uint32_t currency = in.size () > 8 ? in[8] : 0;

	if (s.currency != currency) {
		s.currency = currency;
		set_defaults ();
	}
	p[0] = s.game_number;
	p[2] = STATUS_OK;
	p[3] = play_options ();
	p[4] = exchange_options ();
	p[5] = s.max_pay;
	p[6] = s.max_credit;
	p[7] = s.hopper_size;
	p[8] = s.max_bet;
	p[9] = s.min_bet;
	p[10] = s.add_bet;
	p[11] = s.currency;
	p[12] = s.credit0;
	p[13] = s.credit1;
	p[14] = s.total_credit;
	p[15] = s.premium;
	p[16] = s.won;
	p[17] = s.paid;
	/* 18-32: the coin counters, the history: none */
	for (size_t i = 0; i < std::size (ERROR_TEXTS); i++, offset += sizeof (ERROR_TEXTS[0]))
		memcpy ((uint8_t *) p.data () + offset, ERROR_TEXTS[i], sizeof (ERROR_TEXTS[0]));
	/* Ready */
	p[0x139] = 1;
	send (ANS_POWER_ON, p);
	started = true;
}

void
SwpHopper::config ()
{
	std::vector<uint32_t> p (9);

	/* Without its settings, a message that only asks for them */
	if (in[2] == 0x28 && in[3] == 0) {
		s.free_play = in[4] & 1;
		s.auto_pay_out = in[5] & 1;
		s.two_way = in[6] & 1;
		s.medal_exchange = in[7];
		s.auto_exchange = in[8] & 1;
		s.coin_discrimination = in[9] & 1;
		s.cur_base = in[0xA];
		s.bet_button = in[0xB] & 1;
		s.max_pay = arg (0xC);
		s.max_credit = arg (0x10);
		s.hopper_size = arg (0x14);
		s.max_bet = arg (0x18);
		s.min_bet = arg (0x1C);
		s.add_bet = arg (0x20);
	}
	p[0] = play_options ();
	p[1] = exchange_options ();
	p[2] = s.max_pay;
	p[3] = s.max_credit;
	p[4] = s.hopper_size;
	p[5] = s.max_bet;
	p[6] = s.min_bet;
	p[7] = s.add_bet;
	send (ANS_CONFIG, p);
}

void
SwpHopper::command (uint8_t cmd)
{
	HL_LOG ("hopper: received %02X, %u bytes", cmd, (unsigned) in.size ());
	switch (cmd) {
	case CMD_POWER_ON:
		HL_PRINT ("hopper: power on, currency %u", in.size () > 8 ? in[8] : 0);
		power_on ();
		break;
	case CMD_GET_STATUS: {
		std::vector<uint32_t> p (3);

		p[1] = STATUS_OK;
		send (ANS_GET_STATUS, p);
		break;
	}
	case CMD_GAME_START: {
		std::vector<uint32_t> p (0x17);

		bet ();
		s.won = 0;
		s.paid = 0;
		p[0] = ++s.game_number;
		p[2] = STATUS_OK;
		p[3] = s.credit0;
		p[4] = s.credit1;
		p[5] = s.total_credit;
		p[6] = s.premium;
		send (ANS_GAME_START, p);
		break;
	}
	case CMD_GAME_END: {
		std::vector<uint32_t> p (0x15);

		pay_out (arg (8));
		p[0] = s.game_number;
		p[1] = s.credit0;
		p[2] = s.credit1;
		p[3] = s.total_credit;
		p[4] = s.premium;
		send (ANS_GAME_END, p);
		break;
	}
	case CMD_TEST_START: {
		std::vector<uint32_t> p (0x33);

		p[1] = STATUS_OK;
		put_clock (&p[0x12]);
		p[0x14] = s.game_number;
		send (ANS_TEST_START, p);
		break;
	}
	case CMD_BACKUP_CLEAR: {
		std::vector<uint32_t> p (0x44);

		s.credit0 = 0;
		s.credit1 = 0;
		s.premium = 0;
		s.paid = 0;
		s.won = 0;
		p[0] = s.credit0;
		p[1] = s.credit1;
		p[2] = s.total_credit;
		p[3] = s.premium;
		put_clock (&p[0x23]);
		p[0x25] = s.game_number;
		send (ANS_BACKUP_CLEAR, p);
		break;
	}
	case CMD_BET: {
		std::vector<uint32_t> p (2);

		p[0] = arg (4);
		send (ANS_BET, p);
		break;
	}
	case CMD_CONFIG:
		config ();
		break;
	case CMD_SWITCH:
	case CMD_TEST_DATA:
	case CMD_COIN_IN_ACK:
	case CMD_COIN_OUT_ACK:
		/* No answer */
		break;
	default:
		HL_ERROR_FEW ("hopper: unknown command %02X", cmd);
		break;
	}
}

int
SwpHopper::save_state (hl::State *state)
{
	uint32_t in_size = (uint32_t) in.size ();
	uint32_t out_size = (uint32_t) out.size ();
	std::vector<uint8_t> pending (out.begin (), out.end ());
	int ret = 0;

	ret |= state->put (&s, sizeof (s));
	ret |= state->put (&expected, sizeof (expected));
	ret |= state->put (&started, sizeof (started));
	ret |= state->put (&coin_held, sizeof (coin_held));
	ret |= state->put (&in_size, sizeof (in_size));
	ret |= state->put (in.data (), in_size);
	ret |= state->put (&out_size, sizeof (out_size));
	ret |= state->put (pending.data (), out_size);
	return ret;
}

int
SwpHopper::load_state (hl::State *state)
{
	uint32_t in_size = 0, out_size = 0;
	std::vector<uint8_t> pending;
	int ret = 0;

	ret |= state->get (&s, sizeof (s));
	ret |= state->get (&expected, sizeof (expected));
	ret |= state->get (&started, sizeof (started));
	ret |= state->get (&coin_held, sizeof (coin_held));
	ret |= state->get (&in_size, sizeof (in_size));
	if (ret || in_size > 0x10000)
		return -1;
	in.resize (in_size);
	ret |= state->get (in.data (), in_size);
	ret |= state->get (&out_size, sizeof (out_size));
	if (ret || out_size > 0x10000)
		return -1;
	pending.resize (out_size);
	ret |= state->get (pending.data (), out_size);
	out.assign (pending.begin (), pending.end ());
	saved = s;
	return ret;
}

} /* namespace naomi */
