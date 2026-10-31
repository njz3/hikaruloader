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
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#define closesocket close
#define ioctlsocket ioctl
#endif

#include <chrono>

#include "hl/core.h"
#include "hl/ringlink.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace hl {

static uint64_t
now_ms ()
{
	return (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds> (
		std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

static void
set_nonblocking (uintptr_t sock, bool on)
{
#ifdef _WIN32
	u_long mode = on;
#else
	int mode = on;
#endif
	ioctlsocket (sock, FIONBIO, &mode);
}

RingLink::RingLink () :
	listener (NO_SOCKET),
	in (NO_SOCKET),
	out (NO_SOCKET),
	connecting (false),
	next_try (0),
	lost (false),
	port_out (0)
{
}

RingLink::~RingLink ()
{
	close ();
}

void
RingLink::drop (uintptr_t &sock)
{
	if (sock != NO_SOCKET)
		closesocket (sock);
	sock = NO_SOCKET;
}

bool
RingLink::open (unsigned port_in, const char *addr_out, unsigned port_out)
{
	struct sockaddr_in addr;
	int yes = 1;

	close ();
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa)) {
		HL_ERROR ("ring: cannot start Winsock");
		return false;
	}
#endif
	listener = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == NO_SOCKET) {
		HL_ERROR ("ring: cannot create a socket");
		return false;
	}
	setsockopt (listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof (yes));
	/* Every interface, the previous cabinet another PC; a ring of one,
	 * this PC alone (no firewall's question) */
	memset (&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (port_in == port_out &&
	                              (!strcmp (addr_out, "127.0.0.1") ||
	                               !strcmp (addr_out, "localhost")) ?
	                              INADDR_LOOPBACK : INADDR_ANY);
	addr.sin_port = htons ((uint16_t) port_in);
	if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) ||
	    listen (listener, 2)) {
		HL_ERROR ("ring: cannot listen on port %u", port_in);
		close ();
		return false;
	}
	set_nonblocking (listener, true);
	this->addr_out = addr_out;
	this->port_out = port_out;
	lost = false;
	next_try = 0;
	HL_PRINT ("ring: listening on port %u, the next node at %s:%u",
	          port_in, addr_out, port_out);
	return true;
}

void
RingLink::close ()
{
	if (listener == NO_SOCKET)
		return;
	drop (in);
	drop (out);
	drop (listener);
	connecting = false;
	rx.clear ();
#ifdef _WIN32
	WSACleanup ();
#endif
}

bool
RingLink::poll ()
{
	struct timeval none = { 0, 0 };
	fd_set set;
	int yes = 1;

	if (listener == NO_SOCKET)
		return false;

	/* The previous node; a new one replaces the old */
	for (;;) {
		uintptr_t sock = (uintptr_t) accept (listener, NULL, NULL);
		if (sock == NO_SOCKET)
			break;
		drop (in);
		in = sock;
		set_nonblocking (in, false);
		setsockopt (in, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
		rx.clear ();
		lost = false;
		HL_PRINT ("ring: the previous node is connected");
	}

	/* The next node */
	if (out == NO_SOCKET && now_ms () >= next_try) {
		struct addrinfo hints, *res = NULL;
		char port[8];

		memset (&hints, 0, sizeof (hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		snprintf (port, sizeof (port), "%u", port_out);
		next_try = now_ms () + 1000;
		if (getaddrinfo (addr_out.c_str (), port, &hints, &res) || !res) {
			HL_ERROR ("ring: unknown address '%s'", addr_out.c_str ());
			return false;
		}
		out = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (out != NO_SOCKET) {
			set_nonblocking (out, true);
			connect (out, res->ai_addr, (int) res->ai_addrlen);
			connecting = true;
		}
		freeaddrinfo (res);
	}
	if (out != NO_SOCKET && connecting) {
		fd_set failed;
		FD_ZERO (&set);
		FD_SET (out, &set);
		FD_ZERO (&failed);
		FD_SET (out, &failed);
		if (select ((int) out + 1, NULL, &set, &failed, &none) > 0) {
			int err = 0;
			socklen_t len = sizeof (err);
			getsockopt (out, SOL_SOCKET, SO_ERROR, (char *) &err, &len);
			if (FD_ISSET (out, &failed) || err) {
				/* Not there yet: again later */
				drop (out);
				connecting = false;
			} else {
				connecting = false;
				set_nonblocking (out, false);
				setsockopt (out, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
				lost = false;
				HL_PRINT ("ring: connected to the next node");
			}
		}
	}
	return in != NO_SOCKET && out != NO_SOCKET && !connecting && !lost;
}

bool
RingLink::send (uint32_t type, const void *data, uint32_t size)
{
	std::vector<uint8_t> msg (8 + size);
	size_t done = 0;

	if (out == NO_SOCKET || connecting)
		return false;
	for (int i = 0; i < 4; i++) {
		msg[i] = (uint8_t) (size >> (i * 8));
		msg[4 + i] = (uint8_t) (type >> (i * 8));
	}
	if (size)
		memcpy (&msg[8], data, size);
	while (done < msg.size ()) {
		int sent = ::send (out, (const char *) &msg[done], (int) (msg.size () - done), MSG_NOSIGNAL);
		if (sent <= 0) {
			HL_ERROR ("ring: the link to the next node broke");
			drop (out);
			lost = true;
			return false;
		}
		done += sent;
	}
	return true;
}

/* A whole message from rx, if there is one */
bool
RingLink::take (uint32_t *type, std::vector<uint8_t> &data)
{
	uint32_t size;

	if (rx.size () < 8)
		return false;
	size = rx[0] | (rx[1] << 8) | (rx[2] << 16) | ((uint32_t) rx[3] << 24);
	if (rx.size () < 8 + (size_t) size)
		return false;
	*type = rx[4] | (rx[5] << 8) | (rx[6] << 16) | ((uint32_t) rx[7] << 24);
	data.assign (rx.begin () + 8, rx.begin () + 8 + size);
	rx.erase (rx.begin (), rx.begin () + 8 + size);
	return true;
}

bool
RingLink::receive (uint32_t *type, std::vector<uint8_t> &data, unsigned timeout_ms)
{
	uint64_t end = now_ms () + timeout_ms;

	for (;;) {
		struct timeval wait;
		uint64_t now;
		fd_set set;
		char buf[16384];
		int got;

		if (take (type, data))
			return true;
		if (in == NO_SOCKET)
			return false;
		now = now_ms ();
		if (now > end)
			return false;
		wait.tv_sec = (long) ((end - now) / 1000);
		wait.tv_usec = (long) ((end - now) % 1000) * 1000;
		FD_ZERO (&set);
		FD_SET (in, &set);
		if (select ((int) in + 1, &set, NULL, NULL, &wait) <= 0)
			continue;
		got = recv (in, buf, sizeof (buf), 0);
		if (got <= 0) {
			HL_ERROR ("ring: the link from the previous node broke");
			drop (in);
			lost = true;
			return false;
		}
		rx.insert (rx.end (), buf, buf + got);
	}
}

} /* namespace hl */
