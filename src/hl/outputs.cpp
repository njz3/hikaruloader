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

/* Before windows.h, which the other headers may bring */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#define closesocket close
#define ioctlsocket ioctl
#endif

#include "hl/outputs.h"
#include "hl/config.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* A text the settings translate (data/lang) */
#define N_(text) text

namespace hl {

static const uintptr_t NO_SOCKET = (uintptr_t) -1;
/* A client this far behind reads nothing: dropped */
static const size_t MAX_PENDING = 64 * 1024;

/* The outputs of each game's OUTPUT TEST, by JVS output (from 1), under
 * Supermodel's kind of names: each item turned on in the test, the output
 * that changed. NASCAR Arcade's test lights its two in turn: its start lamp
 * taken on output 1, as in the other games. Brave Firefighters' menu has no
 * OUTPUT TEST: its start lamps blink with credits, its nozzle shakes as it
 * sprays (player 1's; player 2's nozzle and the revolving lights unseen) */
static const struct {
	const char	*family;
	unsigned	 output;
	const char	*name;
	const char	*meaning;
} game_outputs[] = {
	{ "airtrix",	1, "LampStart",		N_("Start button lamp") },
	{ "airtrix",	2, "LampLeftUpper",	N_("Left upper lamp") },
	{ "airtrix",	3, "LampRightLower",	N_("Right lower lamp") },
	{ "airtrix",	4, "LockMecha",		N_("The board's lock (LOCK MECHA.), released at 1") },
	{ "airtrix",	5, "LampLeftLower",	N_("Left lower lamp") },
	{ "airtrix",	6, "LampRightUpper",	N_("Right upper lamp") },
	{ "braveff",	1, "LampStart1",	N_("Player 1's start button lamp") },
	{ "braveff",	2, "NozzleVibration1",	N_("Player 1's nozzle vibration") },
	{ "braveff",	4, "LampStart2",	N_("Player 2's start button lamp") },
	{ "pharrier",	1, "LampStart",		N_("Start button lamp") },
	{ "pharrier",	2, "LampVrR",		N_("VR R button lamp") },
	{ "pharrier",	3, "LampVrL",		N_("VR L button lamp") },
	{ "pharrier",	5, "LampBomb",		N_("Bomb button lamp") },
	{ "pharrier",	6, "LampBillboard",	N_("Billboard lamp") },
	{ "sgnascar",	1, "LampStart",		N_("Start button lamp") },
	{ "sgnascar",	4, "LampView",		N_("View change button lamp") },
	{ "swracer",	1, "LampStart",		N_("Start button lamp") },
	{ "swracer",	4, "LampBoost",		N_("Boost button lamp") },
	{ "swracer",	5, "BoostLock",		N_("Boost lever lock") },
	{ "von4",	1, "LampStart",		N_("Start button lamp") },
	{ "von4",	6, "LampWinner",	N_("Winner lamp") },
};

/* Flycast's numbering: output 1 on bit 7 of the first byte, 8 on bit 0, 9 on
 * bit 7 of the second (bit 15) */
static int
lamp_bit (unsigned output)
{
	return (int) ((output - 1) / 8 * 8 + 7 - (output - 1) % 8);
}

static unsigned
lamp_output (int bit)
{
	return (unsigned) (bit / 8 * 8 + 7 - bit % 8 + 1);
}

static void
set_nonblocking (uintptr_t sock)
{
#ifdef _WIN32
	u_long mode = 1;
#else
	int mode = 1;
#endif
	ioctlsocket (sock, FIONBIO, &mode);
}

static bool
would_block ()
{
#ifdef _WIN32
	return WSAGetLastError () == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static bool
address_in_use ()
{
#ifdef _WIN32
	return WSAGetLastError () == WSAEADDRINUSE;
#else
	return errno == EADDRINUSE;
#endif
}

Outputs *
Outputs::get ()
{
	static Outputs outputs;
	return &outputs;
}

Outputs::Outputs () :
	jvs_count (0),
	has_drive (false),
	started (false),
	paused (false),
	enabled (false),
	port (0),
	announce_port (0),
	winsock (false),
	listener (NO_SOCKET),
	reply_set (false),
	reply (0),
	reply_from (NO_SOCKET)
{
}

Outputs::~Outputs ()
{
	close_server ();
#ifdef _WIN32
	if (winsock)
		WSACleanup ();
#endif
}

/* The game and its machine */

void
Outputs::board (unsigned count, bool drive_board)
{
	std::lock_guard<std::mutex> hold (lock);
	unsigned n;

	jvs_count = MIN2 (count, 16u);
	has_drive = drive_board;
	items.clear ();
	/* The lamps known at 0: a client learns their names at once */
	for (n = 1; n <= jvs_count; n++)
		items.push_back ({ "lamp" + std::to_string (lamp_bit (n)), lamp_bit (n), NULL, false, 0, true });
	if (has_drive)
		items.push_back ({ "m3ffb", -1, NULL, false, 0, false });
}

void
Outputs::start (const char *name, const char *parent)
{
	std::lock_guard<std::mutex> hold (lock);
	const char *family = (parent && parent[0]) ? parent : name;

	game = name;
	for (const auto &g : game_outputs) {
		if (strcmp (g.family, family) || g.output > jvs_count)
			continue;
		for (Item &item : items)
			if (item.bit == lamp_bit (g.output) && !item.alias)
				item.meaning = g.meaning;
		items.push_back ({ g.name, lamp_bit (g.output), g.meaning, true, 0, true });
	}
	started = true;
	paused = false;
	for (Client &client : clients)
		greet (client);
	announce ();
}

void
Outputs::stop ()
{
	std::lock_guard<std::mutex> hold (lock);

	if (started)
		send_all ("mame_stop = 1\r");
	started = false;
	paused = false;
	game.clear ();
	items.clear ();
	jvs_count = 0;
	has_drive = false;
	forget_reply ();
}

void
Outputs::set_paused (bool now)
{
	std::lock_guard<std::mutex> hold (lock);

	if (now == paused)
		return;
	paused = now;
	if (started)
		send_all (paused ? "pause = 1\r" : "pause = 0\r");
}

/* The values */

std::string
Outputs::line (const Item &item) const
{
	char text[96];

	if (item.bit < 0)
		snprintf (text, sizeof (text), "%s = %02X\r", item.name.c_str (), item.value & 0xFF);
	else
		snprintf (text, sizeof (text), "%s = %d\r", item.name.c_str (), item.value);
	return text;
}

void
Outputs::set (Item &item, int value)
{
	if (item.known && item.value == value)
		return;
	item.value = value;
	item.known = true;
	if (started)
		send_all (line (item));
}

void
Outputs::jvs (const uint8_t *data, unsigned bytes)
{
	std::lock_guard<std::mutex> hold (lock);
	uint32_t bits = 0;
	unsigned i;

	for (i = 0; i < bytes && i < 4; i++)
		bits |= (uint32_t) data[i] << (8 * i);
	for (Item &item : items)
		if (item.bit >= 0)
			set (item, (bits >> item.bit) & 1);
}

void
Outputs::drive_command (uint8_t command)
{
	std::lock_guard<std::mutex> hold (lock);

	for (Item &item : items)
		if (item.bit < 0)
			set (item, command);
}

bool
Outputs::drive_reply (uint8_t *value)
{
	std::lock_guard<std::mutex> hold (lock);

	if (!reply_set)
		return false;
	*value = reply;
	return true;
}

void
Outputs::forget_reply ()
{
	reply_set = false;
	reply_from = NO_SOCKET;
}

/* The server */

void
Outputs::open_server ()
{
	struct sockaddr_in addr;

	error.clear ();
#ifdef _WIN32
	if (!winsock) {
		WSADATA wsa;
		if (WSAStartup (MAKEWORD (2, 2), &wsa)) {
			error = "Winsock cannot start";
			HL_ERROR ("outputs: %s", error.c_str ());
			return;
		}
		winsock = true;
	}
#endif
	listener = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == NO_SOCKET) {
		error = "cannot create a socket";
		HL_ERROR ("outputs: %s", error.c_str ());
		return;
	}
#ifndef _WIN32
	/* At once again after a restart; not on Windows, where it would let
	 * another program's server share the port */
	int yes = 1;
	setsockopt (listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof (yes));
#endif
	/* All of the PC's addresses: the tools may run on another one */
	memset (&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (INADDR_ANY);
	addr.sin_port = htons ((uint16_t) port);
	if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) || listen (listener, 4)) {
		char text[96];
		snprintf (text, sizeof (text), address_in_use () ? "port %u is in use" : "cannot listen on port %u", port);
		error = text;
		HL_ERROR ("outputs: %s (another emulator, or another instance?): "
		          "[Outputs] PORT to change it", error.c_str ());
		closesocket (listener);
		listener = NO_SOCKET;
		return;
	}
	HL_PRINT ("outputs: on TCP port %u", port);
}

void
Outputs::close_server ()
{
	if (started)
		send_all ("mame_stop = 1\r");
	for (Client &client : clients)
		if (client.sock != NO_SOCKET)
			closesocket (client.sock);
	clients.clear ();
	if (listener != NO_SOCKET)
		closesocket (listener);
	listener = NO_SOCKET;
	error.clear ();
	forget_reply ();
}

/* Supermodel's datagram, once a game starts (or the server opens on a game
 * running), to this PC, where the tools run */
void
Outputs::announce ()
{
	struct sockaddr_in addr;
	uintptr_t sock;
	char text[160];
	int length;

	if (!started || !announce_port || listener == NO_SOCKET)
		return;
	sock = (uintptr_t) socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == NO_SOCKET)
		return;
	memset (&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
	addr.sin_port = htons ((uint16_t) announce_port);
	length = snprintf (text, sizeof (text), "mame_start = %s\rtcp = %u\r", game.c_str (), port);
	sendto (sock, text, length, 0, (struct sockaddr *) &addr, sizeof (addr));
	closesocket (sock);
}

void
Outputs::flush (Client &client)
{
	while (client.sock != NO_SOCKET && !client.pending.empty ()) {
		int sent = ::send (client.sock, client.pending.data (), (int) client.pending.size (), MSG_NOSIGNAL);
		if (sent <= 0) {
			if (sent < 0 && would_block ())
				break;
			closesocket (client.sock);
			client.sock = NO_SOCKET;
			HL_PRINT ("outputs: a client left");
			return;
		}
		client.pending.erase (0, sent);
	}
	if (client.sock != NO_SOCKET && client.pending.size () > MAX_PENDING) {
		closesocket (client.sock);
		client.sock = NO_SOCKET;
		HL_ERROR ("outputs: a client that read nothing was dropped");
	}
}

void
Outputs::send_all (const std::string &text)
{
	for (Client &client : clients) {
		if (client.sock == NO_SOCKET)
			continue;
		client.pending += text;
		flush (client);
	}
}

/* The game, then all that is known */
void
Outputs::greet (Client &client)
{
	if (!started || client.sock == NO_SOCKET)
		return;
	client.pending += "mame_start = " + game + "\r";
	for (const Item &item : items)
		if (item.known)
			client.pending += line (item);
	if (paused)
		client.pending += "pause = 1\r";
	flush (client);
}

/* A line a client sent: "m3ffb_rx = XX", the reply of its real drive board,
 * in hex; spaces and case free */
void
Outputs::received (Client &client, const std::string &text)
{
	size_t equal = text.find ('=');
	std::string name, value;
	unsigned long byte;
	char *end;

	if (equal == std::string::npos || !has_drive)
		return;
	auto trim = [] (const std::string &s) {
		size_t first = s.find_first_not_of (" \t");
		return first == std::string::npos ? std::string ()
		       : s.substr (first, s.find_last_not_of (" \t") - first + 1);
	};
	name = trim (text.substr (0, equal));
	value = trim (text.substr (equal + 1));
	if (strcasecmp (name.c_str (), "m3ffb_rx") || value.empty () || value.size () > 2)
		return;
	byte = strtoul (value.c_str (), &end, 16);
	if (*end)
		return;
	if (!reply_set || reply_from != client.sock)
		HL_PRINT ("outputs: the drive board's replies now come from a client's real board (m3ffb_rx)");
	reply = (uint8_t) byte;
	reply_set = true;
	reply_from = client.sock;
}

void
Outputs::poll ()
{
	Config *cfg = Config::get ();
	std::lock_guard<std::mutex> hold (lock);
	struct timeval none = { 0, 0 };
	fd_set readable;
	uintptr_t top;
	size_t i;

	/* The settings, at once: off, or another port */
	if (enabled && (!cfg->outputs_network || cfg->outputs_port != port)) {
		close_server ();
		enabled = false;
	}
	announce_port = cfg->outputs_announce_port;
	if (cfg->outputs_network && !enabled) {
		enabled = true;
		port = cfg->outputs_port;
		open_server ();
		/* A game already running: told to the tools that wait */
		announce ();
	}
	if (listener == NO_SOCKET)
		return;

	FD_ZERO (&readable);
	FD_SET (listener, &readable);
	top = listener;
	for (Client &client : clients) {
		if (client.sock == NO_SOCKET)
			continue;
		FD_SET (client.sock, &readable);
		top = MAX2 (top, client.sock);
	}
	if (select ((int) top + 1, &readable, NULL, NULL, &none) > 0) {
		if (FD_ISSET (listener, &readable)) {
			uintptr_t sock = (uintptr_t) accept (listener, NULL, NULL);
			if (sock != NO_SOCKET) {
				int yes = 1;
				set_nonblocking (sock);
				/* Each line at once: a drive board's command */
				setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
				clients.push_back ({ sock, std::string (), std::string () });
				HL_PRINT ("outputs: a client connected");
				greet (clients.back ());
			}
		}
		/* What they sent, without waiting; by index, greet may add */
		for (i = 0; i < clients.size (); i++) {
			Client &client = clients[i];
			char data[512];
			size_t start, k;
			int got;

			if (client.sock == NO_SOCKET || !FD_ISSET (client.sock, &readable))
				continue;
			got = recv (client.sock, data, sizeof (data), 0);
			if (got <= 0) {
				if (got < 0 && would_block ())
					continue;
				closesocket (client.sock);
				client.sock = NO_SOCKET;
				HL_PRINT ("outputs: a client left");
				continue;
			}
			client.input.append (data, got);
			for (start = k = 0; k < client.input.size (); k++) {
				if (client.input[k] != '\r' && client.input[k] != '\n')
					continue;
				if (k > start)
					received (client, client.input.substr (start, k - start));
				start = k + 1;
			}
			client.input.erase (0, start);
			/* No line's end in sight: not one of ours */
			if (client.input.size () > 1024)
				client.input.clear ();
		}
	}

	for (Client &client : clients)
		flush (client);
	for (i = clients.size (); i-- > 0;)
		if (clients[i].sock == NO_SOCKET)
			clients.erase (clients.begin () + i);
	if (reply_set) {
		bool there = false;
		for (const Client &client : clients)
			there |= client.sock == reply_from;
		if (!there) {
			HL_PRINT ("outputs: the real drive board's client left: the emulated replies again");
			forget_reply ();
		}
	}
}

/* The settings */

std::vector<Outputs::Row>
Outputs::rows (const char *(*tr) (const char *))
{
	std::lock_guard<std::mutex> hold (lock);
	std::vector<Row> rows;
	char text[512];

	if (!started)
		return rows;
	auto value = [] (const Item &item) -> std::string {
		char hex[8];
		if (!item.known)
			return "-";
		if (item.bit >= 0)
			return std::to_string (item.value);
		snprintf (hex, sizeof (hex), "%02X", item.value & 0xFF);
		return hex;
	};

	rows.push_back ({ "mame_start", tr ("text"), tr ("The game's name, the first line a client gets"), game });
	rows.push_back ({ "pause", tr ("0 or 1"), tr ("1 while the game is paused"), paused ? "1" : "0" });
	/* The I/O board's outputs, then the game's names for them */
	for (const Item &item : items) {
		if (item.bit < 0 || item.alias)
			continue;
		if (item.meaning)
			snprintf (text, sizeof (text), tr ("%s (JVS output %u)"), tr (item.meaning), lamp_output (item.bit));
		else
			snprintf (text, sizeof (text), tr ("JVS output %u"), lamp_output (item.bit));
		rows.push_back ({ item.name, tr ("0 or 1"), text, value (item) });
	}
	for (const Item &item : items) {
		if (!item.alias)
			continue;
		snprintf (text, sizeof (text), tr ("%s: lamp%d under the game's name"), tr (item.meaning), item.bit);
		rows.push_back ({ item.name, tr ("0 or 1"), text, value (item) });
	}
	/* The drive board's commands, and its replies sent back */
	for (const Item &item : items) {
		if (item.bit >= 0)
			continue;
		rows.push_back ({ item.name, tr ("byte in hex, 00 to FF"),
		                  tr ("Each command the game writes to its drive board, as the board reads it"),
		                  value (item) });
		snprintf (text, sizeof (text), "%02X", reply);
		rows.push_back ({ "m3ffb_rx", tr ("byte in hex, 00 to FF"),
		                  tr ("Received from a client (BackForceFeeder): a real drive board's reply, "
		                      "which the game reads instead of the emulated one"),
		                  reply_set ? text : "-" });
	}
	rows.push_back ({ "mame_stop", "1", tr ("When the game ends"), "-" });
	return rows;
}

Outputs::Status
Outputs::status ()
{
	std::lock_guard<std::mutex> hold (lock);
	Status s = { enabled, listener != NO_SOCKET, 0, error };

	for (const Client &client : clients)
		if (client.sock != NO_SOCKET)
			s.clients++;
	return s;
}

} /* namespace hl */
