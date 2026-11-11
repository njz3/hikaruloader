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

#include <algorithm>

#include "hl/jvs.h"
#include "hl/driveboard.h"
#include "hl/buffer.h"
#include "hl/input.h"
#include "hl/pointers.h"
#include "hl/controls.h"
#include "hl/state.h"
#include "hl/outputs.h"
#include "hl/gearbox.h"
#include "hl/gamecontrols.h"
#include "hl/machine.h"
#include "hl/config.h"

namespace hl {

enum {
	JVS_SYNC	= 0xE0,
	JVS_ESCAPE	= 0xD0,
	JVS_BROADCAST	= 0xFF,
	JVS_HOST	= 0x00,

	/* The packet's status */
	STATUS_OK	= 0x01,
	STATUS_UNKNOWN	= 0x02,
	STATUS_SUM	= 0x03,

	/* Each command's report */
	REPORT_OK	= 0x01,
	REPORT_PARAMS	= 0x02,
};

static std::string
hex_string (const std::vector<uint8_t> &bytes)
{
	std::string text;
	char byte[4];

	for (uint8_t b : bytes) {
		snprintf (byte, sizeof (byte), "%02X ", b);
		text += byte;
	}
	if (!text.empty ())
		text.pop_back ();
	return text;
}

static int
save_bytes (State *state, const std::vector<uint8_t> &bytes)
{
	uint32_t size = (uint32_t) bytes.size ();
	int ret = state->put (&size, sizeof (size));
	if (!ret && size)
		ret = state->put (bytes.data (), size);
	return ret;
}

static int
load_bytes (State *state, std::vector<uint8_t> &bytes)
{
	uint32_t size;
	int ret = state->get (&size, sizeof (size));
	if (ret || size > 4096)
		return -1;
	bytes.resize (size);
	return size ? state->get (bytes.data (), size) : 0;
}

/* Bus */

JvsBus::JvsBus (Machine *mach) :
	Device (mach),
	packets (0),
	in_pos (0),
	log (false),
	line (NULL),
	line_free (0),
	line_addressed (false)
{
}

JvsBus::~JvsBus ()
{
	delete line;
}

void
JvsBus::attach (JvsDevice *dev)
{
	chain.push_back (dev);
}

void
JvsBus::attach_line (JvsLine *board)
{
	delete line;
	line = board;
}

bool
JvsBus::all_addressed () const
{
	if (line)
		return line_addressed;
	for (JvsDevice *dev : chain)
		if (dev->address == 0xFF)
			return false;
	return !chain.empty ();
}

void
JvsBus::send (uint8_t byte)
{
	/* A new packet drops what was left of the last answer */
	if (out.empty ()) {
		in.clear ();
		in_pos = 0;
	}
	out.push_back (byte);
}

int
JvsBus::receive ()
{
	return can_receive () ? in[in_pos++] : -1;
}

void
JvsBus::end_packet ()
{
	std::vector<uint8_t> packet;
	size_t i = 0;

	/* From the sync on, unescaped */
	while (i < out.size () && out[i] != JVS_SYNC)
		i++;
	for (i++; i < out.size (); i++) {
		if (out[i] == JVS_ESCAPE && i + 1 < out.size ())
			packet.push_back (out[++i] + 1);
		else
			packet.push_back (out[i]);
	}
	out.clear ();
	in.clear ();
	in_pos = 0;
	if (!packet.empty ())
		handle (packet);
}

void
JvsBus::handle (const std::vector<uint8_t> &packet)
{
	std::vector<uint8_t> reports, answer;
	uint8_t node, status = STATUS_OK, sum = 0;
	const uint8_t *data;
	JvsDevice *target = NULL;
	size_t i, size;

	packets++;
	/* Node, length, at least a command, and the sum */
	if (packet.size () < 4 || packet[1] != packet.size () - 2) {
		if (log)
			HL_PRINT ("jvs: > %s (malformed, dropped)", hex_string (packet).c_str ());
		return;
	}
	node = packet[0];
	data = &packet[2];
	size = packet.size () - 3;
	for (i = 0; i + 1 < packet.size (); i++)
		sum += packet[i];

	if (sum != packet.back ())
		status = STATUS_SUM;
	else if (size == 2 && data[0] == 0xF1) {
		/* Set the address: the last device of the chain that has none,
		 * the one whose sense line the host sees */
		for (i = chain.size (); i-- > 0;)
			if (chain[i]->address == 0xFF) {
				target = chain[i];
				break;
			}
		if (!target && !chain.empty ())
			target = chain.front ();
		if (target) {
			target->address = data[1];
			reports.push_back (REPORT_OK);
		}
	} else if (node == JVS_BROADCAST) {
		/* The reset, mostly: nobody answers */
		for (JvsDevice *dev : chain)
			dev->answer (data, size, reports);
		reports.clear ();
		if (log)
			HL_PRINT ("jvs: > %s (to all)", hex_string (packet).c_str ());
		return;
	} else {
		for (JvsDevice *dev : chain)
			if (dev->address == node) {
				target = dev;
				break;
			}
		if (target && !target->answer (data, size, reports)) {
			status = STATUS_UNKNOWN;
			reports.clear ();
		}
	}
	if (!target && status == STATUS_OK) {
		if (log)
			HL_PRINT ("jvs: > %s (nobody there)", hex_string (packet).c_str ());
		return;
	}

	/* To the host: node, length (status, reports, sum), status, reports,
	 * sum; escaped after the sync */
	answer.push_back (JVS_HOST);
	answer.push_back ((uint8_t) (reports.size () + 2));
	answer.push_back (status);
	answer.insert (answer.end (), reports.begin (), reports.end ());
	sum = 0;
	for (uint8_t b : answer)
		sum += b;
	answer.push_back (sum);

	in.push_back (JVS_SYNC);
	for (uint8_t b : answer) {
		if (b == JVS_SYNC || b == JVS_ESCAPE) {
			in.push_back (JVS_ESCAPE);
			in.push_back (b - 1);
		} else
			in.push_back (b);
	}
	if (log)
		HL_PRINT ("jvs: > %s\njvs: < %s", hex_string (packet).c_str (), hex_string (answer).c_str ());
}

/* The line */

void
JvsBus::line_send (uint8_t byte, uint64_t tick, uint64_t char_ticks)
{
	uint64_t start = MAX2 (tick, line_free);
	LineEvent ev = { start, 0, byte };

	line_free = start + char_ticks;
	to_board.push_back (ev);
	packets += (byte == JVS_SYNC);
	if (log)
		HL_PRINT ("jvs: > %02X (line, at %llu)", byte, (unsigned long long) start);
}

/* A 16550's FIFO keeps the bytes not started: empty once the last one has */
bool
JvsBus::line_fifo_empty (uint64_t tick) const
{
	return to_board.empty () || to_board.back ().tick <= tick;
}

void
JvsBus::line_flush_send (uint64_t tick)
{
	while (!to_board.empty () && to_board.back ().tick > tick) {
		line_free = to_board.back ().tick;
		to_board.pop_back ();
	}
}

void
JvsBus::line_run (uint64_t tick)
{
	if (line && line->now () < tick)
		line->run_until (tick);
}

uint64_t
JvsBus::line_next_event () const
{
	return to_host.empty () ? UINT64_MAX : to_host.front ().tick;
}

void
JvsBus::line_deliver (uint64_t tick)
{
	size_t n = 0;

	for (; n < to_host.size () && to_host[n].tick <= tick; n++) {
		if (to_host[n].sense) {
			line_addressed = to_host[n].value != 0;
			if (log)
				HL_PRINT ("jvs: %s (line)", line_addressed ? "addressed" : "not addressed");
		} else {
			/* What was read of the last answer goes */
			if (in_pos == in.size ()) {
				in.clear ();
				in_pos = 0;
			}
			in.push_back (to_host[n].value);
			if (log)
				HL_PRINT ("jvs: < %02X (line)", to_host[n].value);
		}
	}
	to_host.erase (to_host.begin (), to_host.begin () + n);
}

uint64_t
JvsBus::line_next_byte () const
{
	return to_board.empty () ? UINT64_MAX : to_board.front ().tick;
}

uint8_t
JvsBus::line_take_byte ()
{
	uint8_t byte = to_board.front ().value;

	to_board.erase (to_board.begin ());
	return byte;
}

void
JvsBus::line_event (uint64_t tick, bool sense, uint8_t value)
{
	LineEvent ev = { tick, (uint8_t) sense, value };
	size_t n = to_host.size ();

	/* In the order of their ticks; the same tick, in the order given */
	while (n > 0 && to_host[n - 1].tick > tick)
		n--;
	to_host.insert (to_host.begin () + n, ev);
}

int
JvsBus::get (unsigned size, uint32_t addr, void *val)
{
	if (size != 1)
		return -1;
	if (addr == 0) {
		int byte = receive ();
		set_ptr (val, 1, byte < 0 ? 0xFF : byte);
	} else if (addr == 1)
		set_ptr (val, 1, (can_receive () ? 1 : 0) | (present () ? 2 : 0) |
		                 (all_addressed () ? 4 : 0));
	else
		return -1;
	return 0;
}

int
JvsBus::put (unsigned size, uint32_t addr, uint64_t val)
{
	if (size != 1)
		return -1;
	if (addr == 0)
		send ((uint8_t) val);
	else if (addr == 1)
		end_packet ();
	else
		return -1;
	return 0;
}

/* The board on the line resets with the machine; the line's time goes on, as
 * the MIE's (its Z80's cycles) does */
void
JvsBus::reset (ResetType type)
{
	(void) type;
	out.clear ();
	in.clear ();
	in_pos = 0;
	packets = 0;
	if (line) {
		to_board.clear ();
		to_host.clear ();
		line_addressed = false;
		line->reset ();
	}
}

static int
save_events (State *state, const void *events, uint32_t count, size_t size)
{
	int ret = state->put (&count, sizeof (count));
	if (!ret && count)
		ret = state->put (events, (uint32_t) (count * size));
	return ret;
}

int
JvsBus::save_state (State *state)
{
	uint32_t pos = (uint32_t) in_pos;
	int ret = save_bytes (state, out);
	ret |= save_bytes (state, in);
	ret |= state->put (&pos, sizeof (pos));
	ret |= state->put (&packets, sizeof (packets));
	/* The line's, after the rest: none without it, so that the states
	 * saved without IO_BOARD_PROGRAM keep loading */
	if (line) {
		uint8_t addressed = line_addressed;
		ret |= save_events (state, to_board.data (), (uint32_t) to_board.size (), sizeof (LineEvent));
		ret |= save_events (state, to_host.data (), (uint32_t) to_host.size (), sizeof (LineEvent));
		ret |= state->put (&line_free, sizeof (line_free));
		ret |= state->put (&addressed, sizeof (addressed));
		ret |= line->save_state (state);
	}
	return ret;
}

static int
load_events (State *state, std::vector<uint8_t> &raw, uint32_t *count, size_t size)
{
	int ret = state->get (count, sizeof (*count));
	if (ret || *count > 4096)
		return -1;
	raw.resize (*count * size);
	return *count ? state->get (raw.data (), (uint32_t) raw.size ()) : 0;
}

int
JvsBus::load_state (State *state)
{
	uint32_t pos;
	int ret = load_bytes (state, out);
	ret |= load_bytes (state, in);
	ret |= state->get (&pos, sizeof (pos));
	ret |= state->get (&packets, sizeof (packets));
	in_pos = pos;
	if (line) {
		std::vector<uint8_t> raw;
		uint32_t count;
		uint8_t addressed;

		ret |= load_events (state, raw, &count, sizeof (LineEvent));
		to_board.resize (count);
		if (count)
			memcpy (to_board.data (), raw.data (), raw.size ());
		ret |= load_events (state, raw, &count, sizeof (LineEvent));
		to_host.resize (count);
		if (count)
			memcpy (to_host.data (), raw.data (), raw.size ());
		ret |= state->get (&line_free, sizeof (line_free));
		ret |= state->get (&addressed, sizeof (addressed));
		line_addressed = addressed != 0;
		ret |= line->load_state (state);
	}
	return ret;
}

const char *
JvsBus::debug_string ()
{
	static char text[160];

	if (line) {
		const char *board = line->debug_string ();
		snprintf (text, sizeof (text), "%s running its program%s, %u packets%s%s",
		          line->name (), line_addressed ? " addressed" : "", packets,
		          board ? ", " : "", board ? board : "");
		return text;
	}
	snprintf (text, sizeof (text), "%u devices%s, %u packets",
	          (unsigned) chain.size (), all_addressed () ? " addressed" : "", packets);
	return text;
}

bool
JvsBus::set_option (const char *name, int value)
{
	if (strcmp (name, "log"))
		return false;
	log = value != 0;
	return true;
}

/* Device */

JvsDevice::JvsDevice (Machine *mach, JvsBus *bus) :
	Device (mach),
	address (0xFF)
{
	bus->attach (this);
}

bool
JvsDevice::answer (const uint8_t *data, size_t size, std::vector<uint8_t> &reports)
{
	size_t i = 0;

	while (i < size) {
		const uint8_t *cmd = data + i;
		size_t left = size - i, used;
		std::vector<uint8_t> out;
		bool ok = true;

		switch (cmd[0]) {
		case 0xF0: /* Reset: D9 */
			used = 2;
			if (left >= 2 && cmd[1] == 0xD9)
				bus_reset ();
			break;
		case 0x10: { /* Identity */
			const char *id = identity ();
			used = 1;
			out.assign (id, id + strlen (id) + 1);
			break;
		}
		case 0x11:
			used = 1;
			out.push_back (command_revision ());
			break;
		case 0x12:
			used = 1;
			out.push_back (jvs_revision ());
			break;
		case 0x13:
			used = 1;
			out.push_back (comm_revision ());
			break;
		case 0x14: /* The functions */
			used = 1;
			functions (out);
			out.push_back (0x00);
			break;
		case 0x15: /* The main board's identity: a string */
			used = 1;
			while (used < left && cmd[used])
				used++;
			used++;
			break;
		case 0x20: /* Switches: players, bytes each */
			used = 3;
			ok = left >= used && switches (cmd[1], cmd[2], out);
			break;
		case 0x21: /* Coins: slots */
			used = 2;
			ok = left >= used && coins (cmd[1], out);
			break;
		case 0x22: /* Analog: channels */
			used = 2;
			ok = left >= used && analogs (cmd[1], out);
			break;
		case 0x23: /* Encoders: channels */
			used = 2;
			ok = left >= used && rotary (cmd[1], out);
			break;
		case 0x25: /* Screen position: the channel */
			used = 2;
			ok = left >= used && screen (cmd[1], out);
			break;
		case 0x30: /* Coins taken: slot, count */
		case 0x31:
			used = 4;
			ok = left >= used && coin_change (cmd[1], ((cmd[0] == 0x30) ? -1 : 1) *
			                                           ((cmd[2] << 8) | cmd[3]));
			break;
		case 0x32: /* Outputs: bytes, then them */
			used = (left >= 2) ? 2 + cmd[1] : 2;
			ok = left >= used && outputs (cmd[1], cmd + 2);
			break;
		default:
			used = maker_command (cmd, left, out);
			if (!used) {
				HL_ERROR ("jvs: device %02X: unknown command %02X", address, cmd[0]);
				return false;
			}
			break;
		}
		if (left < used) {
			reports.push_back (REPORT_PARAMS);
			break;
		}
		if (cmd[0] != 0xF0) {
			reports.push_back (ok ? REPORT_OK : REPORT_PARAMS);
			if (ok)
				reports.insert (reports.end (), out.begin (), out.end ());
		}
		i += used;
	}
	return true;
}

void
JvsDevice::reset (ResetType type)
{
	(void) type;
	address = 0xFF;
}

int
JvsDevice::save_state (State *state)
{
	return state->put (&address, sizeof (address));
}

int
JvsDevice::load_state (State *state)
{
	return state->get (&address, sizeof (address));
}

/* Sega 837-13551 */

SegaIoBoard::SegaIoBoard (Machine *mach, JvsBus *bus) :
	JvsDevice (mach, bus)
{
	reset (RESET_HARD);
	Outputs::get ()->board (6, false);
}

const char *
SegaIoBoard::identity ()
{
	return "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";
}

void
SegaIoBoard::functions (std::vector<uint8_t> &out)
{
	const uint8_t list[] = {
		0x01, (uint8_t) player_count, 13, 0,	/* Switches: 2 players, 13 each */
		0x02, 2, 0, 0,		/* Coins: 2 slots */
		0x03, 8, 16, 0,		/* Analog: 8 channels, 16 bits */
		0x12, 6, 0, 0,		/* Outputs: 6 */
	};
	out.insert (out.end (), list, list + sizeof (list));
}

/* Its port among the game's controls: the first board's "p1", "p2", "sys",
 * "an"; the second's (players 3 and 4) "p3", "p4", "sys2", "an2" */
std::string
SegaIoBoard::port (const char *kind, unsigned player) const
{
	if (!strcmp (kind, "p"))
		return "p" + std::to_string (first_player + player + 1);
	return unit ? kind + std::to_string (unit + 1) : std::string (kind);
}

void
SegaIoBoard::count_coins ()
{
	uint32_t slots;
	unsigned i;

	if (!cabinet)
		return;
	/* The game's coin chutes: "coin:1" and "coin:2" (the second board's
	 * "coin:3" and "coin:4") */
	slots = GameControls::get ().switches ("coin", mach->frame);
	for (i = 0; i < 2; i++) {
		bool held = (slots >> (2 * unit + i + 1)) & 1;
		if (held && !coin_held[i])
			coin[i] = (coin[i] + 1) & 0x3FFF;
		coin_held[i] = held;
	}
}

bool
SegaIoBoard::switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out)
{
	unsigned p, i;

	if (players > player_count || bytes > 2)
		return false;
	/* Polled each frame: the coins too */
	count_coins ();
	/* HR_LOG_INPUTS=1: what the game reads, at each change */
	static const bool log_inputs = getenv ("HR_LOG_INPUTS") != NULL;
	if (log_inputs) {
		static uint64_t last = ~0ull;
		uint64_t now = ((uint64_t) SegaIoBoard::player_bits (1) << 17) |
		               ((uint64_t) SegaIoBoard::player_bits (0) << 1) | (system_bits () >> 7);
		if (now != last && cabinet && !unit)
			HL_PRINT ("inputs: frame %llu, test %u, player 1 %04X, player 2 %04X",
			          (unsigned long long) mach->frame, (unsigned) (now & 1),
			          (unsigned) ((now >> 1) & 0xFFFF), (unsigned) (now >> 17));
		if (cabinet && !unit)
			last = now;
	}
	out.push_back (system_bits ());
	for (p = 0; p < players; p++) {
		uint16_t bits = SegaIoBoard::player_bits (p);
		for (i = 0; i < bytes; i++)
			out.push_back ((uint8_t) (bits >> (8 - 8 * i)));
	}
	return true;
}

uint8_t
SegaIoBoard::system_bits ()
{
	/* TEST in bit 7: "sys:7" */
	return cabinet ? (uint8_t) GameControls::get ().switches (port ("sys").c_str (), mach->frame) : 0x00;
}

uint16_t
SegaIoBoard::player_bits (unsigned player)
{
	if (!cabinet || player >= player_count)
		return 0;
	return (uint16_t) GameControls::get ().switches (port ("p", player).c_str (), mach->frame);
}

/* The wheel and the pedals read from the controllers once a call for
 * channel 0, as the analog command reads them all */
uint16_t
SegaIoBoard::analog_bits (unsigned channel)
{
	if (channel == 0)
		read_controls ();
	return (channel < 8) ? analog[channel] : 0x8000;
}

void
SegaIoBoard::take_coins (unsigned presses[2])
{
	uint16_t before[2] = { coin[0], coin[1] };
	unsigned i;

	count_coins ();
	for (i = 0; i < 2; i++)
		presses[i] = (unsigned) ((coin[i] - before[i]) & 0x3FFF);
}

bool
SegaIoBoard::coins (unsigned slots, std::vector<uint8_t> &out)
{
	unsigned i;

	if (slots > 2)
		return false;
	count_coins ();
	/* The top two bits: the slot's condition, 0 when fine */
	for (i = 0; i < slots; i++) {
		out.push_back ((uint8_t) (coin[i] >> 8));
		out.push_back ((uint8_t) coin[i]);
	}
	return true;
}

bool
SegaIoBoard::coin_change (unsigned slot, int count)
{
	int value;

	if (slot < 1 || slot > 2)
		return false;
	value = coin[slot - 1] + count;
	coin[slot - 1] = (uint16_t) ((value < 0) ? 0 : (value & 0x3FFF));
	return true;
}

/* The game's channels from its controls (GameControls), those the shell or
 * the agent set kept */
void
SegaIoBoard::read_controls ()
{
	GameControls &g = GameControls::get ();

	if (!cabinet)
		return;
	for (unsigned i = 0; i < 8; i++)
		if (!analog_set[i])
			g.analog (port ("an").c_str (), i, &analog[i]);
}

bool
SegaIoBoard::analogs (unsigned channels, std::vector<uint8_t> &out)
{
	unsigned i;

	if (channels > 8)
		return false;
	read_controls ();
	for (i = 0; i < channels; i++) {
		out.push_back ((uint8_t) (analog[i] >> 8));
		out.push_back ((uint8_t) analog[i]);
	}
	/* HR_LOG_INPUTS=1: the channels' top bytes too, at each change */
	static const bool log_inputs = getenv ("HR_LOG_INPUTS") != NULL;
	if (log_inputs && cabinet && !unit) {
		static std::string last;
		char text[48] = "";

		for (i = 0; i < channels && i < 8; i++)
			snprintf (text + 3 * i, sizeof (text) - 3 * i, " %02X", analog[i] >> 8);
		if (last != text)
			HL_PRINT ("inputs: frame %llu, analogs%s", (unsigned long long) mach->frame, text);
		last = text;
	}
	return true;
}

bool
SegaIoBoard::outputs (unsigned bytes, const uint8_t *data)
{
	/* Six outputs: the top bits of one byte */
	if (bytes != 1)
		return false;
	output = data[0] & 0xFC;
	if (cabinet && lamps)
		Outputs::get ()->jvs (&output, 1);
	return true;
}

void
SegaIoBoard::reset (ResetType type)
{
	JvsDevice::reset (type);
	/* The wheel in the middle, the pedals up */
	memset (analog, 0, sizeof (analog));
	memset (analog_set, 0, sizeof (analog_set));
	analog[0] = 0x8000;
	coin[0] = coin[1] = 0;
	coin_held[0] = coin_held[1] = false;
	output = 0;
}

int
SegaIoBoard::save_state (State *state)
{
	int ret = JvsDevice::save_state (state);
	ret |= state->put (analog, sizeof (analog));
	ret |= state->put (coin, sizeof (coin));
	ret |= state->put (coin_held, sizeof (coin_held));
	ret |= state->put (&output, sizeof (output));
	return ret;
}

int
SegaIoBoard::load_state (State *state)
{
	int ret = JvsDevice::load_state (state);
	ret |= state->get (analog, sizeof (analog));
	ret |= state->get (coin, sizeof (coin));
	ret |= state->get (coin_held, sizeof (coin_held));
	ret |= state->get (&output, sizeof (output));
	return ret;
}

int
SegaIoBoard::get (unsigned size, uint32_t addr, void *val)
{
	if (size == 2 && addr < 0x10 && !(addr & 1))
		set_ptr (val, 2, analog[addr >> 1]);
	else if (size == 2 && (addr == 0x10 || addr == 0x12))
		set_ptr (val, 2, coin[(addr - 0x10) >> 1]);
	else if (size == 1 && addr == 0x14)
		set_ptr (val, 1, output);
	else
		return -1;
	return 0;
}

int
SegaIoBoard::put (unsigned size, uint32_t addr, uint64_t val)
{
	if (size == 2 && addr < 0x10 && !(addr & 1)) {
		analog[addr >> 1] = (uint16_t) val;
		analog_set[addr >> 1] = true;
	}
	else if (size == 2 && (addr == 0x10 || addr == 0x12))
		coin[(addr - 0x10) >> 1] = (uint16_t) (val & 0x3FFF);
	else
		return -1;
	return 0;
}

const char *
SegaIoBoard::debug_string ()
{
	static char text[128];

	snprintf (text, sizeof (text), "address %02X, coins %u %u, outputs %02X, wheel %04X",
	          address, coin[0], coin[1], output, analog[0]);
	return text;
}

/* SegaIoBoard13938 */

/* A window's width, -1 to 1, makes this many counts */
static const double ENCODER_COUNTS = 960.0;

SegaIoBoard13938::SegaIoBoard13938 (Machine *mach, JvsBus *bus) :
	SegaIoBoard (mach, bus)
{
	cabinet = false;
	reset (RESET_HARD);
}

const char *
SegaIoBoard13938::identity ()
{
	return "SEGA ENTERPRISES,LTD.;837-13938 ENCORDER BD  ;Ver0.01;99/08";
}

void
SegaIoBoard13938::functions (std::vector<uint8_t> &out)
{
	static const uint8_t list[] = {
		0x01, 1, 9, 0,		/* Switches: 1 player, 9 */
		0x04, 4, 0, 0,		/* Encoders: 4 */
		0x12, 8, 0, 0,		/* Outputs: 8 */
	};
	out.insert (out.end (), list, list + sizeof (list));
}

bool
SegaIoBoard13938::rotary (unsigned channels, std::vector<uint8_t> &out)
{
	unsigned i;

	if (channels > 4)
		return false;
	Pointers::State mouse = Pointers::get (0);
	/* X grows to the right; Y grows upwards, as a trackball's */
	rolled_x += (mouse.x - last_x) * ENCODER_COUNTS;
	rolled_y -= (mouse.y - last_y) * ENCODER_COUNTS;
	last_x = mouse.x;
	last_y = mouse.y;
	encoder[0] = (uint16_t) (int32_t) lround (rolled_x);
	encoder[1] = (uint16_t) (int32_t) lround (rolled_y);
	for (i = 0; i < channels; i++) {
		out.push_back ((uint8_t) (encoder[i] >> 8));
		out.push_back ((uint8_t) encoder[i]);
	}
	return true;
}

bool
SegaIoBoard13938::outputs (unsigned bytes, const uint8_t *data)
{
	if (bytes != 1)
		return false;
	output = data[0];
	return true;
}

void
SegaIoBoard13938::reset (ResetType type)
{
	SegaIoBoard::reset (type);
	memset (encoder, 0, sizeof (encoder));
	last_x = last_y = rolled_x = rolled_y = 0.0;
}

int
SegaIoBoard13938::save_state (State *state)
{
	int ret = SegaIoBoard::save_state (state);
	ret |= state->put (encoder, sizeof (encoder));
	ret |= state->put (&rolled_x, sizeof (rolled_x));
	ret |= state->put (&rolled_y, sizeof (rolled_y));
	return ret;
}

int
SegaIoBoard13938::load_state (State *state)
{
	int ret = SegaIoBoard::load_state (state);
	ret |= state->get (encoder, sizeof (encoder));
	ret |= state->get (&rolled_x, sizeof (rolled_x));
	ret |= state->get (&rolled_y, sizeof (rolled_y));
	return ret;
}

int
SegaIoBoard13938::get (unsigned size, uint32_t addr, void *val)
{
	if (size == 2 && addr >= 0x20 && addr < 0x28 && !(addr & 1)) {
		set_ptr (val, 2, encoder[(addr - 0x20) >> 1]);
		return 0;
	}
	return SegaIoBoard::get (size, addr, val);
}

/* NamcoIoBoard */

/* Their identities and functions as Flycast has them (maple_jvs.cpp): the
 * FCA-1's one coin slot, the BIOS stopping with two. The screen positions'
 * counts as the games' factory settings take them, the shots where they aim
 * before any adjustment: the JYU's X 56 to 414 across the picture, Y 68 to
 * 543 down it (Ninja Assault's GUN INITIALIZE, the shots within a pixel); the
 * FCB's signed, X -9840 to 13060, Y 28370 to 8030 (Mazan's SWORD TEST: -320
 * to 319 across, 240 to -240 down) */
static const NamcoIoBoard::Model namco_models[] = {
	{ "namco-jyu", "namco ltd.;JYU-PCB;Ver1.00;JPN,2Coins 2Guns", 2, 12, 2, 0, 0, 2, 16, false, 56, 358, 68, 475 },
	{ "namco-fcb", "namco ltd.;FCB;Ver1.0;JPN,Touch Panel & Multipurpose", 1, 16, 2, 7, 2, 1, 6, true, -9840, 22900, 28370, -20340 },
	{ "namco-fca", "namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder", 1, 16, 1, 7, 2, 0, 6, false, 0, 0, 0, 0 },
};

const NamcoIoBoard::Model *
NamcoIoBoard::find (const std::string &name)
{
	for (const Model &m : namco_models)
		if (name == m.name)
			return &m;
	return NULL;
}

NamcoIoBoard::NamcoIoBoard (Machine *mach, JvsBus *bus, const Model *model) :
	SegaIoBoard (mach, bus),
	model (model)
{
	player_count = model->players;
	Outputs::get ()->board (model->outputs, false);
	reset (RESET_HARD);
}

void
NamcoIoBoard::functions (std::vector<uint8_t> &out)
{
	out.insert (out.end (), { 0x01, model->players, model->switches, 0 });
	if (model->coins)
		out.insert (out.end (), { 0x02, model->coins, 0, 0 });
	if (model->analogs)
		out.insert (out.end (), { 0x03, model->analogs, 16, 0 });
	if (model->encoders)
		out.insert (out.end (), { 0x04, model->encoders, 0, 0 });
	/* Screen positions: 16 bits of X, 16 of Y, the channels */
	if (model->positions)
		out.insert (out.end (), { 0x06, 16, 16, model->positions });
	out.insert (out.end (), { 0x12, model->outputs, 0, 0 });
}

bool
NamcoIoBoard::position (unsigned player, unsigned *x, unsigned *y)
{
	GameControls &g = GameControls::get ();
	std::string port = "pos" + std::to_string (player + 1);
	uint16_t vx, vy;

	if (!cabinet || ((g.switches (port.c_str (), mach->frame) >> 2) & 1))
		return false;
	if (!g.analog (port.c_str (), 0, &vx) || !g.analog (port.c_str (), 1, &vy))
		return false;
	/* Signed counts, 16 bits */
	*x = (unsigned) (model->left + (int64_t) vx * model->width / 65536) & 0xFFFF;
	*y = (unsigned) (model->top + (int64_t) vy * model->height / 65536) & 0xFFFF;
	return true;
}

bool
NamcoIoBoard::screen (unsigned channel, std::vector<uint8_t> &out)
{
	unsigned x = 0, y = 0;

	if (channel < 1 || channel > model->positions)
		return false;
	if (!position (first_player + channel - 1, &x, &y))
		x = y = 0;
	/* HR_LOG_GUN=1: the positions read, at each change */
	static const bool log_gun = getenv ("HR_LOG_GUN") != NULL;
	if (log_gun) {
		static uint32_t last[4] = { ~0u, ~0u, ~0u, ~0u };
		unsigned player = MIN2 (first_player + channel - 1, 3u);

		if (last[player] != ((x << 16) | y))
			HL_PRINT ("jvs: device %02X: screen position %u: %u, %u, frame %llu", address, channel,
			          x, y, (unsigned long long) mach->frame);
		last[player] = (x << 16) | y;
	}
	out.insert (out.end (), { (uint8_t) (x >> 8), (uint8_t) x, (uint8_t) (y >> 8), (uint8_t) y });
	return true;
}

bool
NamcoIoBoard::analogs (unsigned channels, std::vector<uint8_t> &out)
{
	unsigned x, y, i;
	uint16_t v;

	if (!model->presence)
		return SegaIoBoard::analogs (channels, out);
	if (channels > 8)
		return false;
	v = (!starting && position (first_player, &x, &y)) ? 0x8000 : 0;
	for (i = 0; i < channels; i++)
		out.insert (out.end (), { (uint8_t) (v >> 8), (uint8_t) v });
	return true;
}

bool
NamcoIoBoard::rotary (unsigned channels, std::vector<uint8_t> &out)
{
	/* Nothing on them in these cabinets */
	if (channels > 4)
		return false;
	out.insert (out.end (), 2 * channels, 0);
	return true;
}

bool
NamcoIoBoard::outputs (unsigned bytes, const uint8_t *data)
{
	if (bytes < 1 || bytes > 2)
		return false;
	memcpy (lamps_out, data, bytes);
	output = data[0];
	if (cabinet && lamps)
		Outputs::get ()->jvs (data, bytes);
	return true;
}

/* Namco's start (70), its replies as Flycast gives them (the board's own are
 * not known). Mazan's, other commands after them in their packets: 70 18
 * and four bytes, 70 60 and one, 70 62 03 and two, the reads of 16 bytes
 * (pages 00 to F0 of 01 twice, 00 of 02), the last 70 62 03 00 10 the end
 * of the start; any other the rest of the packet */
size_t
NamcoIoBoard::maker_command (const uint8_t *cmd, size_t left, std::vector<uint8_t> &out)
{
	size_t size;

	if (cmd[0] != 0x70)
		return 0;
	size = (left < 2) ? left : (cmd[1] == 0x18) ? 6 : (cmd[1] == 0x60) ? 3 : (cmd[1] == 0x62) ? 5 : left;
	starting = true;
	if (left >= 3 && cmd[2] == 3) {
		out.push_back (0x10);
		out.insert (out.end (), 16, 0x7F);
		if (left >= 5 && (cmd[4] == 0x10 || cmd[4] == 0x11))
			starting = false;
	} else
		out.insert (out.end (), { 2, 3, 1 });
	return size;
}

void
NamcoIoBoard::reset (ResetType type)
{
	SegaIoBoard::reset (type);
	starting = false;
	lamps_out[0] = lamps_out[1] = 0;
}

int
NamcoIoBoard::save_state (State *state)
{
	int ret = SegaIoBoard::save_state (state);
	ret |= state->put (&starting, sizeof (starting));
	ret |= state->put (lamps_out, sizeof (lamps_out));
	return ret;
}

int
NamcoIoBoard::load_state (State *state)
{
	int ret = SegaIoBoard::load_state (state);
	ret |= state->get (&starting, sizeof (starting));
	ret |= state->get (lamps_out, sizeof (lamps_out));
	return ret;
}

/* SegaIoBoard13844 */

SegaIoBoard13844::SegaIoBoard13844 (Machine *mach, JvsBus *bus, DriveBoard *drive) :
	SegaIoBoard (mach, bus),
	drive (drive)
{
	output2 = 0;
	/* Outputs 15 to 22 are the drive board's */
	Outputs::get ()->board (14, drive != NULL);
}

SegaIoBoard13844::~SegaIoBoard13844 ()
{
}

const char *
SegaIoBoard13844::identity ()
{
	return "SEGA ENTERPRISES,LTD.;837-13844-01 I/O CNTL BD2 ;Ver1.00;99/07";
}

void
SegaIoBoard13844::functions (std::vector<uint8_t> &out)
{
	static const uint8_t list[] = {
		0x01, 2, 12, 0,		/* Switches: 2 players, 12 each */
		0x02, 2, 0, 0,		/* Coins: 2 slots */
		0x03, 8, 16, 0,		/* Analog: 8 channels, 16 bits */
		0x12, 22, 0, 0,		/* Outputs: 22 */
	};
	out.insert (out.end (), list, list + sizeof (list));
}

static uint8_t
reverse8 (uint8_t v)
{
	v = (uint8_t) ((v & 0xF0) >> 4 | (v & 0x0F) << 4);
	v = (uint8_t) ((v & 0xCC) >> 2 | (v & 0x33) << 2);
	return (uint8_t) ((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

bool
SegaIoBoard13844::switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out)
{
	size_t first = out.size ();
	uint8_t reply, p1 = 0, p2 = 0;
	unsigned k;

	if (!SegaIoBoard::switches (players, bytes, out))
		return false;
	if (!drive || bytes < 2)
		return true;
	drive->set_wheel ((uint8_t) (analog[0] >> 8));
	reply = drive->read ();
	/* A real drive board's, from the outputs' client (m3ffb_rx) */
	Outputs::get ()->drive_reply (&reply);
	for (k = 0; k < 4; k++) {
		if (!(reply & (1 << k)))
			p1 |= 0x80 >> k;
		if (!(reply & (0x10 << k)))
			p2 |= 0x80 >> k;
	}
	/* After the system byte: player 1's second byte, then player 2's */
	if (players >= 1)
		out[first + 2] = (uint8_t) ((out[first + 2] & 0x0F) | p1);
	if (players >= 2)
		out[first + 2 + bytes] = (uint8_t) ((out[first + 2 + bytes] & 0x0F) | p2);
	return true;
}

/* The drive board's reply read with player 1's switches, kept for player
 * 2's: each bit's inverse on a switch (see the class) */
uint16_t
SegaIoBoard13844::player_bits (unsigned player)
{
	uint16_t bits = SegaIoBoard::player_bits (player);
	unsigned k;

	if (!drive || player > 1)
		return bits;
	if (player == 0) {
		drive->set_wheel ((uint8_t) (analog[0] >> 8));
		line_reply = drive->read ();
		Outputs::get ()->drive_reply (&line_reply);
	}
	for (k = 0; k < 4; k++)
		if (!(line_reply & ((player ? 0x10 : 0x01) << k)))
			bits |= 0x80 >> k;
	return bits;
}

bool
SegaIoBoard13844::outputs (unsigned bytes, const uint8_t *data)
{
	if (bytes < 1 || bytes > 3)
		return false;
	output = data[0];
	output2 = (bytes >= 2) ? (uint8_t) (data[1] & 0xFC) : 0;
	if (bytes == 3 && drive) {
		uint8_t command = (uint8_t) ~reverse8 ((uint8_t) ((data[1] << 6) | (data[2] >> 2)));
		drive->set_wheel ((uint8_t) (analog[0] >> 8));
		drive->write (command);
		Outputs::get ()->drive_command (command);
	}
	uint8_t bytes_out[2] = { output, output2 };
	if (lamps)
		Outputs::get ()->jvs (bytes_out, 2);
	return true;
}

int
SegaIoBoard13844::get (unsigned size, uint32_t addr, void *val)
{
	if (size == 1 && addr == 0x15) {
		set_ptr (val, 1, output2);
		return 0;
	}
	return SegaIoBoard::get (size, addr, val);
}

void
SegaIoBoard13844::reset (ResetType type)
{
	SegaIoBoard::reset (type);
	output2 = 0;
	line_reply = 0xFF;
	/* The cabinet's sound volume control on the fifth analog input, turned
	 * all the way up: F355 Challenge takes its master volume from it (16
	 * times the square root of its top byte over 256, silent under 8) */
	analog[4] = 0xFFFF;
	Gearbox::get ().reset ();
}

int
SegaIoBoard13844::save_state (State *state)
{
	int ret = SegaIoBoard::save_state (state);
	ret |= state->put (&output2, 1);
	return ret;
}

int
SegaIoBoard13844::load_state (State *state)
{
	int ret = SegaIoBoard::load_state (state);
	ret |= state->get (&output2, 1);
	return ret;
}

} /* namespace hl */
