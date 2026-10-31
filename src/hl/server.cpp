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

/* Before windows.h, which the other headers may bring */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif

#include "hl/server.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace hl {

static const uintptr_t NO_SOCKET = (uintptr_t) -1;

Server::Server () :
	port (0),
	listener (NO_SOCKET),
	next_id (1)
{
}

Server::~Server ()
{
	for (Client &client : clients)
		close (client);
	if (listener != NO_SOCKET) {
		closesocket (listener);
#ifdef _WIN32
		WSACleanup ();
#endif
	}
}

bool
Server::open (unsigned port)
{
	struct sockaddr_in addr;
	int yes = 1;

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa)) {
		HL_ERROR ("server: cannot start Winsock");
		return false;
	}
#endif
	listener = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == NO_SOCKET) {
		HL_ERROR ("server: cannot create a socket");
		goto fail;
	}
	setsockopt (listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof (yes));

	/* This machine only: the shell runs anything it is given */
	memset (&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
	addr.sin_port = htons ((uint16_t) port);
	if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) ||
	    listen (listener, 4)) {
		HL_ERROR ("server: cannot listen on 127.0.0.1:%u", port);
		goto fail;
	}
	this->port = port;
	return true;
fail:
	if (listener != NO_SOCKET)
		closesocket (listener);
	listener = NO_SOCKET;
#ifdef _WIN32
	WSACleanup ();
#endif
	return false;
}

void
Server::close (Client &client)
{
	if (!client.closed) {
		closesocket (client.sock);
		client.closed = true;
		HL_PRINT ("server: client %d left", client.id);
	}
}

void
Server::poll (const std::function<void (int)> &greet,
              const std::function<void (int, const char *)> &line)
{
	struct timeval none = { 0, 0 };
	fd_set readable;
	uintptr_t top;
	size_t i;

	if (listener == NO_SOCKET)
		return;

	FD_ZERO (&readable);
	FD_SET (listener, &readable);
	top = listener;
	for (Client &client : clients) {
		FD_SET (client.sock, &readable);
		if (client.sock > top)
			top = client.sock;
	}
	if (select ((int) top + 1, &readable, NULL, NULL, &none) <= 0)
		return;

	if (FD_ISSET (listener, &readable)) {
		uintptr_t sock = (uintptr_t) accept (listener, NULL, NULL);
		if (sock != NO_SOCKET) {
			int yes = 1;
			/* Answers go at once, in one piece */
			setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
			clients.push_back ({ next_id++, sock, std::string (), false });
			HL_PRINT ("server: client %d connected", clients.back ().id);
			greet (clients.back ().id);
		}
	}

	/* By index: the handlers may send, which closes the clients gone */
	for (i = 0; i < clients.size (); i++) {
		char data[4096];
		size_t end;
		int got;

		if (clients[i].closed || !FD_ISSET (clients[i].sock, &readable))
			continue;
		got = recv (clients[i].sock, data, sizeof (data), 0);
		if (got <= 0) {
			close (clients[i]);
			continue;
		}
		clients[i].input.append (data, got);
		while (!clients[i].closed &&
		       (end = clients[i].input.find ('\n')) != std::string::npos) {
			std::string text = clients[i].input.substr (0, end);
			clients[i].input.erase (0, end + 1);
			if (!text.empty () && text.back () == '\r')
				text.pop_back ();
			line (clients[i].id, text.c_str ());
		}
	}

	for (i = clients.size (); i-- > 0;)
		if (clients[i].closed)
			clients.erase (clients.begin () + i);
}

void
Server::send (int id, const char *text, size_t length)
{
	for (Client &client : clients) {
		size_t sent = 0;

		if (client.id != id || client.closed)
			continue;
		while (sent < length) {
			int num = ::send (client.sock, text + sent, (int) (length - sent), MSG_NOSIGNAL);
			if (num <= 0) {
				close (client);
				break;
			}
			sent += num;
		}
	}
}

void
Server::broadcast (const char *text)
{
	for (Client &client : clients)
		send (client.id, text, strlen (text));
}

} /* namespace hl */
