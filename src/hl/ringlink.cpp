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
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

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

/* A recv () that found nothing yet, not an error */
static bool
would_block ()
{
#ifdef _WIN32
	return WSAGetLastError () == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* A spinning thread's breath, kind to the other thread of its core */
static inline void
pause_cpu ()
{
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
	_mm_pause ();
#else
	std::this_thread::yield ();
#endif
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
	port_out (0),
	rx_at (0),
	in_count (0),
	out_count_plain (0),
	queued (false),
	sender_quit (false),
	sender_idle (false),
	spin_us (0),
	tx_pending (false),
	out_ok (false),
	out_count (0)
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

void
RingLink::put_header (std::vector<uint8_t> &msg, uint32_t type, uint32_t size)
{
	for (int i = 0; i < 4; i++)
		msg.push_back ((uint8_t) (size >> (i * 8)));
	for (int i = 0; i < 4; i++)
		msg.push_back ((uint8_t) (type >> (i * 8)));
}

void
RingLink::set_hello (uint32_t type, const void *data, uint32_t size)
{
	hello.clear ();
	put_header (hello, type, size);
	hello.insert (hello.end (), (const uint8_t *) data, (const uint8_t *) data + size);
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
	if (queued) {
		sender_quit = false;
		sender = std::thread (&RingLink::send_loop, this);
	}
	return true;
}

void
RingLink::close ()
{
	if (listener == NO_SOCKET)
		return;
	if (sender.joinable ()) {
		{
			std::lock_guard<std::mutex> hold (out_lock);
			sender_quit = true;
		}
		out_wake.notify_one ();
		sender.join ();
	}
	drop (in);
	drop (out);
	drop (listener);
	connecting = false;
	out_ok = false;
	rx.clear ();
	rx_at = 0;
	tx.clear ();
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
		/* Queued, its reads never wait either (receive () without a
		 * timeout, a call instead of two) */
		set_nonblocking (in, queued);
		setsockopt (in, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
		rx.clear ();
		rx_at = 0;
		lost = false;
		in_count++;
		HL_PRINT ("ring: the previous node is connected");
	}

	/* The next node: queued, the sender thread's */
	if (queued)
		return in != NO_SOCKET && out_ok && !lost;
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
				out_count_plain++;
				HL_PRINT ("ring: connected to the next node");
			}
		}
	}
	return in != NO_SOCKET && out != NO_SOCKET && !connecting && !lost;
}

bool
RingLink::send (uint32_t type, const void *data, uint32_t size)
{
	std::vector<uint8_t> msg;
	size_t done = 0;

	if (queued) {
		std::lock_guard<std::mutex> hold (out_lock);
		/* No next node, or it stopped taking them: lost, as on a
		 * fibre pulled out or jammed */
		if (!out_ok || tx.size () + 8 + size > QUEUE_MOST)
			return false;
		put_header (tx, type, size);
		if (size)
			tx.insert (tx.end (), (const uint8_t *) data, (const uint8_t *) data + size);
		tx_pending.store (true, std::memory_order_release);
		if (sender_idle)
			out_wake.notify_one ();
		return true;
	}
	if (out == NO_SOCKET || connecting)
		return false;
	msg.reserve (8 + size);
	put_header (msg, type, size);
	if (size)
		msg.insert (msg.end (), (const uint8_t *) data, (const uint8_t *) data + size);
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

/* Queued: the next node, from the sender thread, a second at most */
bool
RingLink::connect_out ()
{
	struct addrinfo hints, *res = NULL;
	struct timeval second = { 1, 0 };
	fd_set set, failed;
	char port[8];
	int yes = 1, err = 0;
	socklen_t len = sizeof (err);
	size_t done = 0;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf (port, sizeof (port), "%u", port_out);
	if (getaddrinfo (addr_out.c_str (), port, &hints, &res) || !res) {
		HL_ERROR ("ring: unknown address '%s'", addr_out.c_str ());
		return false;
	}
	out = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (out == NO_SOCKET) {
		freeaddrinfo (res);
		return false;
	}
	set_nonblocking (out, true);
	connect (out, res->ai_addr, (int) res->ai_addrlen);
	freeaddrinfo (res);
	FD_ZERO (&set);
	FD_SET (out, &set);
	FD_ZERO (&failed);
	FD_SET (out, &failed);
	if (select ((int) out + 1, NULL, &set, &failed, &second) <= 0 || FD_ISSET (out, &failed) ||
	    getsockopt (out, SOL_SOCKET, SO_ERROR, (char *) &err, &len) || err) {
		/* Not there yet: again later */
		drop (out);
		return false;
	}
	/* This thread's own: it may wait */
	set_nonblocking (out, false);
	setsockopt (out, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
	while (done < hello.size ()) {
		int sent = ::send (out, (const char *) &hello[done], (int) (hello.size () - done), MSG_NOSIGNAL);
		if (sent <= 0) {
			drop (out);
			return false;
		}
		done += sent;
	}
	{
		std::lock_guard<std::mutex> hold (out_lock);
		tx.clear ();
		out_ok = true;
	}
	out_count++;
	HL_PRINT ("ring: connected to the next node");
	return true;
}

/* Queued: the sender thread, which owns out */
void
RingLink::send_loop ()
{
	std::vector<uint8_t> batch;

	for (;;) {
		size_t done = 0;

		/* While the ring runs, a message comes every few tens of
		 * microseconds: looked for a while before sleeping */
		if (spin_us && out != NO_SOCKET) {
			auto until = std::chrono::steady_clock::now () + std::chrono::microseconds (spin_us);
			while (!tx_pending.load (std::memory_order_acquire) &&
			       std::chrono::steady_clock::now () < until)
				pause_cpu ();
		}
		{
			std::unique_lock<std::mutex> hold (out_lock);
			if (sender_quit)
				break;
			if (out != NO_SOCKET && tx.empty ()) {
				sender_idle = true;
				out_wake.wait_for (hold, std::chrono::milliseconds (100),
				                   [this] { return sender_quit || !tx.empty (); });
				sender_idle = false;
				if (sender_quit)
					break;
			}
			batch.swap (tx);
			tx_pending.store (false, std::memory_order_relaxed);
		}
		if (out == NO_SOCKET) {
			batch.clear ();
			if (!connect_out ()) {
				std::unique_lock<std::mutex> hold (out_lock);
				out_wake.wait_for (hold, std::chrono::seconds (1), [this] { return sender_quit; });
			}
			continue;
		}
		while (done < batch.size ()) {
			int sent = ::send (out, (const char *) &batch[done], (int) (batch.size () - done), MSG_NOSIGNAL);
			if (sent <= 0) {
				HL_ERROR ("ring: the link to the next node broke");
				out_ok = false;
				drop (out);
				break;
			}
			done += sent;
		}
		batch.clear ();
	}
}

bool
RingLink::read_in ()
{
	char buf[16384];
	int got = recv (in, buf, sizeof (buf), 0);

	/* Queued: nothing there yet */
	if (got < 0 && would_block ())
		return false;
	if (got <= 0) {
		HL_ERROR ("ring: the link from the previous node broke");
		drop (in);
		lost = true;
		return false;
	}
	rx.insert (rx.end (), buf, buf + got);
	return true;
}

bool
RingLink::wait_us (unsigned timeout_us)
{
	struct timeval wait;
	fd_set set;

	if (in == NO_SOCKET)
		return false;
	wait.tv_sec = (long) (timeout_us / 1000000);
	wait.tv_usec = (long) (timeout_us % 1000000);
	FD_ZERO (&set);
	FD_SET (in, &set);
	if (select ((int) in + 1, &set, NULL, NULL, &wait) <= 0)
		return false;
	return read_in ();
}

/* A whole message from rx, if there is one */
bool
RingLink::take (uint32_t *type, std::vector<uint8_t> &data)
{
	uint32_t size;
	const uint8_t *p = rx.data () + rx_at;

	if (rx.size () - rx_at < 8)
		return false;
	size = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
	if (size > QUEUE_MOST) {
		/* Not one of ours (a frame is 64 KB at most): the link is
		 * dropped rather than waiting for gigabytes */
		HL_ERROR ("ring: a message of %u bytes from the previous node: its link dropped", size);
		drop (in);
		rx.clear ();
		rx_at = 0;
		lost = true;
		return false;
	}
	if (rx.size () - rx_at < 8 + (size_t) size)
		return false;
	*type = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t) p[7] << 24);
	data.assign (p + 8, p + 8 + size);
	/* The messages taken dropped at once at the end of a read, else a
	 * read's worth of them at a time */
	rx_at += 8 + size;
	if (rx_at == rx.size () || rx_at >= 16384) {
		rx.erase (rx.begin (), rx.begin () + rx_at);
		rx_at = 0;
	}
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

		if (take (type, data))
			return true;
		if (in == NO_SOCKET)
			return false;
		if (queued && !timeout_ms) {
			/* A read that does not wait */
			if (!read_in ())
				return false;
			continue;
		}
		now = now_ms ();
		if (now > end)
			return false;
		wait.tv_sec = (long) ((end - now) / 1000);
		wait.tv_usec = (long) ((end - now) % 1000) * 1000;
		FD_ZERO (&set);
		FD_SET (in, &set);
		if (select ((int) in + 1, &set, NULL, NULL, &wait) <= 0) {
			/* Without a timeout, a look and no more (it spun for up
			 * to a millisecond) */
			if (!timeout_ms)
				return false;
			continue;
		}
		if (!read_in ())
			return false;
	}
}

} /* namespace hl */
