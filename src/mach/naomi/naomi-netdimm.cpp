/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/netdimm.cpp (GPL-2.0-or-later,
 * flyinghead).
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

/*
 * The Net DIMM's socket library
 * =============================
 *
 * The firmware of a Net DIMM board (VxWorks) has a network stack the game
 * reaches through the mailbox, as commands of group 1: the arguments of each,
 * words of 32 bits, in the 4 KB buffer of the board's memory named by the low
 * byte of the command. The first word of a buffer is the function's number:
 *
 *   1 accept       2 bind        3 close       4 connect    5 getIpByDns
 *   6 inet_addr    8 listen      9 recv       10 send      11 socket
 *  12 select      14 setsockopt 15 getsockopt 16 settimeout
 *  17 errno       22 recvfrom   23 sendto      (route, DHCP: answered failed)
 *
 * Sockets are numbers from 1. The game's fd_set is a bitmap of 32 bytes of
 * those numbers; its sockaddr has BSD's layout: sin_len, sin_family, the port
 * and the address, both big endian. Errors are VxWorks's numbers.
 *
 * Nothing waits here: every socket is non-blocking and a command that cannot
 * end at once ends later, when poll () finds it done, or when its timeout
 * runs out in the emulated time. Its reply leaves through the mailbox, one
 * per poll, as the NAOMI's interrupt of the last one has been dealt with.
 *
 * The network as the game sees it is not the PC's: [Network] DIMM_MAP says
 * where an address the game uses is, and DIMM_PORT_OFFSET moves the ports
 * this board binds. An address the map does not list is not reachable, no
 * host name is looked up: nothing leaves the PC unless the user asked for it.
 *
 * The board has a TCP service of its own, on port 10703, for the machines that
 * put a game on it or watch it: [Network] DIMM_LISTEN opens it, on the address
 * DIMM_SERVICE_IP says (127.0.0.1 by default). A packet is a 32-bit header
 * (little endian) and what it announces: opcode, flags, 16 bits of length.
 *
 *   01 nothing       04 write the DIMM      05 read the DIMM
 *   07 host mode     09 end                 0A restart (not done)
 *   10 peek          16 host control read   17 time limit
 *   18 information   19 set information     7F key code (ignored)
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hl/config.h"
#include "hl/core.h"
#include "mach/naomi/naomi-netdimm.h"

#ifdef _WIN32
#define NE(x) WSAE##x
static int last_error () { return WSAGetLastError (); }
#else
#define NE(x) E##x
static int last_error () { return errno; }
#endif

namespace naomi {

static const uintptr_t NO_SOCKET = ~(uintptr_t) 0;

enum {
	VX_EBADF	= 9,
	VX_EINVAL	= 22,
	VX_EMFILE	= 24,
	VX_ENOPROTOOPT	= 42,
	VX_ENETUNREACH	= 51,
	VX_ETIMEDOUT	= 60,
};

/* The host's error as VxWorks numbers it */
static int
vx_error (int e)
{
	switch (e) {
	case 0: return 0;
	case NE(BADF): return 9;
	case NE(ACCES): return 13;
	case NE(FAULT): return 14;
	case NE(INVAL): return 22;
	case NE(MFILE): return 24;
	case NE(NAMETOOLONG): return 26;
	case NE(MSGSIZE): return 36;
	case NE(DESTADDRREQ): return 40;
	case NE(PROTOTYPE): return 41;
	case NE(NOPROTOOPT): return 42;
	case NE(PROTONOSUPPORT): return 43;
	case NE(SOCKTNOSUPPORT): return 44;
	case NE(OPNOTSUPP): return 45;
	case NE(PFNOSUPPORT): return 46;
	case NE(AFNOSUPPORT): return 47;
	case NE(ADDRINUSE): return 48;
	case NE(ADDRNOTAVAIL): return 49;
	case NE(NOTSOCK): return 50;
	case NE(NETUNREACH): return 51;
	case NE(NETRESET): return 52;
	case NE(CONNABORTED): return 53;
	case NE(CONNRESET): return 54;
	case NE(NOBUFS): return 55;
	case NE(ISCONN): return 56;
	case NE(NOTCONN): return 57;
	case NE(SHUTDOWN): return 58;
	case NE(TOOMANYREFS): return 59;
	case NE(TIMEDOUT): return 60;
	case NE(CONNREFUSED): return 61;
	case NE(NETDOWN): return 62;
	case NE(LOOP): return 64;
	case NE(HOSTUNREACH): return 65;
	case NE(HOSTDOWN): return 67;
	case NE(INPROGRESS): return 68;
	case NE(ALREADY): return 69;
	case NE(WOULDBLOCK): return 70;
	default: return 5;	/* EIO */
	}
}

static bool
pending_error (int e)
{
	return e == NE(WOULDBLOCK) || e == NE(INPROGRESS);
}

/* A dotted address into a host-order number */
static bool
parse_ip (const std::string &text, uint32_t &ip)
{
	unsigned a, b, c, d;
	char extra;

	if (sscanf (text.c_str (), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
	    a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	ip = (a << 24) | (b << 16) | (c << 8) | d;
	return true;
}

static uint32_t
swap32 (uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static std::string
trim (const std::string &text)
{
	size_t a = text.find_first_not_of (" \t"), b = text.find_last_not_of (" \t");

	return a == std::string::npos ? "" : text.substr (a, b - a + 1);
}

/* What one socket of the game is doing */
struct DimmNet::Sock {
	uintptr_t fd = NO_SOCKET;
	bool	 stream = false;
	int	 error = 0;
	uint32_t connect_ms = 0, send_ms = 0, recv_ms = 0;
	bool	 smcp = false;	/* connected to the cabinet's motion PC, in here */

	bool	 connecting = false, receiving = false, sending = false, accepting = false;
	uint16_t connect_ret = 0, recv_ret = 0, send_ret = 0, accept_ret = 0;
	uint64_t connect_end = 0, recv_end = 0, send_end = 0, accept_end = 0;
	uint32_t recv_at = 0, recv_len = 0, recv_flags = 0, recv_addr = 0, recv_addr_len = 0;
	bool	 recv_from = false;
	uint32_t send_at = 0, send_len = 0;
	uint32_t accept_addr = 0, accept_addr_len = 0;

	bool busy () const { return connecting || receiving || sending || accepting; }

	void close ()
	{
		if (fd != NO_SOCKET) {
			shutdown (fd, 2);
			closesocket (fd);
		}
		*this = Sock ();
	}
};

DimmNet::DimmNet (uint8_t *memory, uint32_t size, uint32_t buffers_, Host *host_,
                  uint32_t file_size, uint32_t file_crc, bool motion_pc) :
	mask (size - 1),
	buffers (buffers_),
	mem (memory),
	own_ip (0),
	port_offset (0),
	listening (false),
	sel (),
	global_error (0),
	now_us (0),
	next_socket (0),
	notes (0),
	host (host_),
	info_size (file_size),
	info_crc (file_crc),
	service_fd (NO_SOCKET),
	service_us (0),
	control_state (CONTROL_NONE)
{
	const hl::Config *cfg = hl::Config::get ();
	uint32_t ip = 0;

#ifdef _WIN32
	WSADATA wsa;

	WSAStartup (MAKEWORD (2, 2), &wsa);
#endif
	if (!parse_ip (cfg->dimm_ip, ip)) {
		HL_ERROR ("naomi: [Network] DIMM_IP '%s' is not an address", cfg->dimm_ip);
		ip = (192u << 24) | (168 << 16) | (1 << 8) | 30;
	}
	own_ip = ip;
	port_offset = cfg->dimm_port_offset;
	listening = cfg->dimm_listen;
	parse_map (cfg->dimm_map);
	if (cfg->dimm_listen)
		open_service (cfg->dimm_service_ip);
	if (motion_pc)
		smcp.reset (new SmcpPeer ());
}

DimmNet::~DimmNet ()
{
	close_all ();
	close_service ();
	for (Sock *s : socks)
		delete s;
#ifdef _WIN32
	WSACleanup ();
#endif
}

void
DimmNet::close_all ()
{
	for (Sock *s : socks)
		s->close ();
	if (smcp)
		smcp->reset ();
	sel = Select ();
	control_state = CONTROL_NONE;
}

/*
 * "game=place" entries, separated by ';', spaces around them ignored:
 *   game:  a.b.c.d, a.b.c.d:port, a name a DNS query gives, or * (any address)
 *   place: host, host:port or host+offset (the game's port moved by offset);
 *          the host's address only, a name is never looked up
 */
void
DimmNet::parse_map (const char *text)
{
	std::string all (text), entry;
	size_t at = 0;

	while (at <= all.size ()) {
		size_t end = all.find (';', at);
		entry = trim (all.substr (at, end == std::string::npos ? end : end - at));
		at = end == std::string::npos ? all.size () + 1 : end + 1;
		if (entry.empty ())
			continue;

		size_t eq = entry.find ('=');
		if (eq == std::string::npos) {
			HL_ERROR ("naomi: [Network] DIMM_MAP: '%s' has no '='", entry.c_str ());
			continue;
		}
		std::string left = trim (entry.substr (0, eq)), right = trim (entry.substr (eq + 1));
		Peer p {};

		if (left == "*") {
			p.ip = 0;
		} else {
			size_t colon = left.find (':');

			if (colon != std::string::npos) {
				p.port = (unsigned) atoi (left.c_str () + colon + 1);
				left.resize (colon);
			}
			if (!parse_ip (left, p.ip))
				p.name = left;
		}

		size_t plus = right.find ('+'), colon = right.find (':');
		if (plus != std::string::npos) {
			p.offset = atoi (right.c_str () + plus + 1);
			right.resize (plus);
		} else if (colon != std::string::npos) {
			p.host_port = (unsigned) atoi (right.c_str () + colon + 1);
			right.resize (colon);
		}
		if (!parse_ip (right, p.host)) {
			HL_ERROR ("naomi: [Network] DIMM_MAP: '%s' is not an address", right.c_str ());
			continue;
		}
		map.push_back (p);
	}
}

void
DimmNet::targets (uint32_t ip, unsigned port, std::vector<Target> &out) const
{
	const Peer *any = NULL;

	out.clear ();
	if (ip == own_ip) {
		out.push_back ({ INADDR_LOOPBACK, (unsigned) ((int) port + port_offset) });
		return;
	}
	for (const Peer &p : map) {
		if (!p.name.empty ()) {
			/* What a DNS query gave: the game connects to it */
			if (p.host == ip)
				out.push_back ({ p.host, p.host_port ? p.host_port : (unsigned) ((int) port + p.offset) });
			continue;
		}
		if (!p.ip) {
			any = &p;
			continue;
		}
		if (p.ip != ip || (p.port && p.port != port))
			continue;
		out.push_back ({ p.host, p.host_port ? p.host_port : (unsigned) ((int) port + p.offset) });
	}
	if (out.empty () && any)
		out.push_back ({ any->host, any->host_port ? any->host_port : (unsigned) ((int) port + any->offset) });
}

uint32_t
DimmNet::rd32 (uint32_t at) const
{
	uint32_t val = 0;

	at &= mask;
	if (at <= mask - 3)
		memcpy (&val, mem + at, 4);
	return val;
}

void
DimmNet::wr32 (uint32_t at, uint32_t val)
{
	at &= mask;
	if (at <= mask - 3)
		memcpy (mem + at, &val, 4);
}

/* Bytes at a game's address: len no more than what the memory has there */
uint8_t *
DimmNet::ptr (uint32_t at, uint32_t &len)
{
	at &= mask;
	len = std::min (len, mask + 1 - at);
	return mem + at;
}

DimmNet::Sock *
DimmNet::get (uint32_t index)
{
	if (index < 1 || index > socks.size () || socks[index - 1]->fd == NO_SOCKET) {
		global_error = VX_EBADF;
		return NULL;
	}
	return socks[index - 1];
}

void
DimmNet::fail (Sock *s, int error)
{
	if (s)
		s->error = error;
	else
		global_error = error;
}

void
DimmNet::done (Reply &r, uint16_t ret, bool failed, uint16_t offset, uint32_t parameter) const
{
	r.command = ret;
	r.failed = failed;
	r.offset = offset;
	r.parameter = parameter;
}

/* The sockaddr the game reads, BSD's layout */
void
DimmNet::write_address (uint32_t addr_at, uint32_t len_at, uint32_t ip, unsigned port)
{
	if (addr_at) {
		uint8_t a[16] = { 0 };
		uint32_t n = 16;

		a[0] = 16;
		a[1] = AF_INET;
		a[2] = (uint8_t) (port >> 8);
		a[3] = (uint8_t) port;
		a[4] = (uint8_t) (ip >> 24);
		a[5] = (uint8_t) (ip >> 16);
		a[6] = (uint8_t) (ip >> 8);
		a[7] = (uint8_t) ip;
		memcpy (ptr (addr_at, n), a, n);
	}
	if (len_at)
		wr32 (len_at, 16);
}

/* The sockaddr the game gave */
void
DimmNet::game_address (uint32_t at, uint32_t &ip, unsigned &port) const
{
	at &= mask;
	port = ((unsigned) mem[(at + 2) & mask] << 8) | mem[(at + 3) & mask];
	ip = ((uint32_t) mem[(at + 4) & mask] << 24) | ((uint32_t) mem[(at + 5) & mask] << 16) |
	     ((uint32_t) mem[(at + 6) & mask] << 8) | mem[(at + 7) & mask];
}

/* A datagram's or a connection's origin, as the game knows it */
void
DimmNet::seen_as (uint32_t ip, unsigned port, uint32_t &game_ip, unsigned &game_port) const
{
	for (const Peer &p : map) {
		if (p.host != ip || (!p.ip && p.name.empty ()))
			continue;
		game_ip = p.ip ? p.ip : p.host;
		game_port = p.host_port ? (p.port ? p.port : port) : (unsigned) ((int) port - p.offset);
		return;
	}
	game_ip = ip;
	game_port = port;
	if (ip == INADDR_LOOPBACK) {
		game_ip = own_ip;
		game_port = (unsigned) ((int) port - port_offset);
	}
}

#define NOTE(fmt_, ...) \
	do { \
		if (notes < 300) { \
			notes++; \
			HL_PRINT ("naomi: Net DIMM: " fmt_, ##__VA_ARGS__); \
		} \
	} while (0)

static uint64_t
deadline_of (uint64_t now, uint32_t ms)
{
	return ms ? now + (uint64_t) ms * 1000 : 0;
}

uint32_t
DimmNet::add_socket (uintptr_t fd, bool stream)
{
#ifdef _WIN32
	u_long on = 1;
#else
	int on = 1;
#endif
	size_t i;

	ioctlsocket (fd, FIONBIO, &on);
	for (i = 0; i < socks.size (); i++)
		if (socks[i]->fd == NO_SOCKET)
			break;
	if (i == socks.size ())
		socks.push_back (new Sock ());
	socks[i]->fd = fd;
	socks[i]->stream = stream;
	socks[i]->error = 0;
	return (uint32_t) i + 1;
}

bool
DimmNet::start_connect (Sock *s, uint32_t index, uint16_t ret, const uint32_t *a, Reply &r)
{
	uint32_t ip;
	unsigned port;
	std::vector<Target> where;

	game_address (a[2], ip, port);
	targets (ip, port, where);
	if (where.empty () && smcp && port == 2000) {
		NOTE ("connect socket %u to the motion PC", index);
		smcp->reset ();
		s->smcp = true;
		done (r, ret, false, (uint16_t) index, 0);
		return true;
	}
	if (where.empty ()) {
		NOTE ("connect to %u.%u.%u.%u:%u: not in DIMM_MAP", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
		fail (s, VX_ENETUNREACH);
		done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
		return true;
	}

	sockaddr_in to {};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl (where[0].ip);
	to.sin_port = htons ((uint16_t) where[0].port);
	NOTE ("connect socket %u to %u.%u.%u.%u:%u (host %08X:%u)", index, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port,
	      where[0].ip, where[0].port);
	if (connect (s->fd, (const sockaddr *) &to, sizeof (to)) == 0) {
		done (r, ret, false, (uint16_t) index, 0);
		return true;
	}

	int e = last_error ();

	if (pending_error (e)) {
		s->connecting = true;
		s->connect_ret = ret;
		s->connect_end = deadline_of (now_us, s->connect_ms);
		return false;
	}
	fail (s, vx_error (e));
	done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
	return true;
}

bool
DimmNet::try_accept (Sock *s, uint32_t index, Reply &r)
{
	sockaddr_in from {};
	socklen_t size = sizeof (from);
	uintptr_t fd = (uintptr_t) accept (s->fd, (sockaddr *) &from, &size);

	if (fd == NO_SOCKET) {
		int e = last_error ();

		if (pending_error (e))
			return false;
		s->accepting = false;
		fail (s, vx_error (e));
		done (r, s->accept_ret, true, (uint16_t) index, 0xFFFFFFFF);
		return true;
	}

	uint32_t game_ip;
	unsigned game_port;
	uint32_t added = add_socket (fd, true);

	seen_as (ntohl (from.sin_addr.s_addr), ntohs (from.sin_port), game_ip, game_port);
	write_address (s->accept_addr, s->accept_addr_len, game_ip, game_port);
	s->accepting = false;
	NOTE ("accepted socket %u on %u", added, index);
	done (r, s->accept_ret, false, (uint16_t) index, added);
	return true;
}

bool
DimmNet::try_recv (Sock *s, uint32_t index, Reply &r)
{
	uint32_t len = s->recv_len;
	uint8_t *p = ptr (s->recv_at, len);
	int n;

	if (s->smcp) {
		n = (int) smcp->output (p, len);
		if (!n)
			return false;
		s->receiving = false;
		done (r, s->recv_ret, false, (uint16_t) index, (uint32_t) n);
		return true;
	}
	if (s->recv_from) {
		sockaddr_in from {};
		socklen_t size = sizeof (from);

		n = recvfrom (s->fd, (char *) p, (int) len, (int) s->recv_flags, (sockaddr *) &from, &size);
		if (n >= 0 || last_error () == NE(MSGSIZE)) {
			uint32_t game_ip;
			unsigned game_port;

			seen_as (ntohl (from.sin_addr.s_addr), ntohs (from.sin_port), game_ip, game_port);
			write_address (s->recv_addr, s->recv_addr_len, game_ip, game_port);
		}
	} else
		n = recv (s->fd, (char *) p, (int) len, (int) s->recv_flags);

	if (n >= 0 || last_error () == NE(MSGSIZE)) {
		s->receiving = false;
		if (n < 0)
			n = (int) len;	/* cut to the game's buffer, as a BSD stack does */
		NOTE ("received %d bytes on socket %u", n, index);
		done (r, s->recv_ret, false, (uint16_t) index, (uint32_t) n);
		return true;
	}

	int e = last_error ();

	/* A datagram of ours refused: no matter for the next one */
	if (pending_error (e) || (e == NE(CONNRESET) && !s->stream))
		return false;
	s->receiving = false;
	fail (s, vx_error (e));
	done (r, s->recv_ret, true, (uint16_t) index, 0xFFFFFFFF);
	return true;
}

bool
DimmNet::try_send (Sock *s, uint32_t index, Reply &r)
{
	uint32_t len = s->send_len;
	const uint8_t *p = ptr (s->send_at, len);

	if (s->smcp) {
		smcp->input (p, len);
		s->sending = false;
		done (r, s->send_ret, false, (uint16_t) index, len);
		return true;
	}

	int n = send (s->fd, (const char *) p, (int) len, 0);

	if (n >= 0) {
		s->sending = false;
		NOTE ("sent %d bytes on socket %u", n, index);
		done (r, s->send_ret, false, (uint16_t) index, (uint32_t) n);
		return true;
	}

	int e = last_error ();

	if (pending_error (e))
		return false;
	s->sending = false;
	fail (s, vx_error (e));
	done (r, s->send_ret, true, (uint16_t) index, 0xFFFFFFFF);
	return true;
}

/* Datagrams to every place the map gives; none: lost on the wire, no error */
bool
DimmNet::do_send_to (Sock *s, uint32_t index, uint16_t ret, const uint32_t *a, Reply &r)
{
	uint32_t len = a[3], ip;
	const uint8_t *p = ptr (a[2], len);
	unsigned port;
	std::vector<Target> where;

	game_address (a[5], ip, port);
	targets (ip, port, where);
	if (where.empty ())
		NOTE ("datagram to %u.%u.%u.%u:%u dropped (not in DIMM_MAP)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);

	int rc = (int) len;

	for (const Target &t : where) {
		sockaddr_in to {};

		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl (t.ip);
		to.sin_port = htons ((uint16_t) t.port);
		if (sendto (s->fd, (const char *) p, (int) len, 0, (const sockaddr *) &to, sizeof (to)) < 0) {
			int e = last_error ();

			if (!pending_error (e)) {
				fail (s, vx_error (e));
				rc = -1;
			}
		} else
			NOTE ("datagram of %u bytes to %08X:%u", len, t.ip, t.port);
	}
	done (r, ret, rc < 0, (uint16_t) index, (uint32_t) rc);
	return true;
}

bool
DimmNet::do_select (uint16_t ret, const uint32_t *a, Reply &r)
{
	sel.active = true;
	sel.ret = ret;
	sel.count = std::min (a[1], (uint32_t) 256);
	sel.in = a[2];
	sel.out = a[3];
	sel.except = a[4];
	sel.deadline = 0;
	if (a[5])
		sel.deadline = now_us + (uint64_t) rd32 (a[5]) * 1000000 + rd32 (a[5] + 4);

	bool immediate = a[5] && !rd32 (a[5]) && !rd32 (a[5] + 4);

	return check_select (r, immediate);
}

bool
DimmNet::check_select (Reply &r, bool timed_out)
{
	const uint32_t sets[3] = { sel.in, sel.out, sel.except };
	fd_set host[3];
	bool any = false, peer_ready = false, peer_asked = false;
	uintptr_t top = 0;
	uint32_t i, k, total = 0;

	for (k = 0; k < 3; k++)
		FD_ZERO (&host[k]);
	for (i = 1; i < sel.count; i++) {
		if (i > socks.size () || socks[i - 1]->fd == NO_SOCKET)
			continue;
		for (k = 0; k < 3; k++) {
			if (sets[k] && (mem[(sets[k] + i / 8) & mask] & (1 << (i & 7)))) {
				if (socks[i - 1]->smcp) {
					peer_asked = true;
					peer_ready |= (k == 0 && smcp->pending ()) || k == 1;
					continue;
				}
				FD_SET (socks[i - 1]->fd, &host[k]);
				top = std::max (top, socks[i - 1]->fd);
				any = true;
			}
		}
	}

	int rc = 0;

	if (any) {
		timeval tv {};

		rc = select ((int) top + 1, &host[0], &host[1], &host[2], &tv);
		if (rc < 0) {
			global_error = vx_error (last_error ());
			sel.active = false;
			done (r, sel.ret, true, 0, 0xFFFFFFFF);
			return true;
		}
	}
	if (rc == 0 && !peer_ready && !timed_out && (any || peer_asked || sel.deadline))
		return false;

	uint8_t out[3][32];

	memset (out, 0, sizeof (out));
	for (i = 1; i < sel.count; i++) {
		if (i > socks.size () || socks[i - 1]->fd == NO_SOCKET)
			continue;
		for (k = 0; k < 3; k++) {
			if (sets[k] && (mem[(sets[k] + i / 8) & mask] & (1 << (i & 7))) &&
			    (socks[i - 1]->smcp ? (k == 0 && smcp->pending ()) || k == 1
			                        : FD_ISSET (socks[i - 1]->fd, &host[k]) != 0)) {
				out[k][i / 8] |= (uint8_t) (1 << (i & 7));
				total++;
			}
		}
	}
	for (k = 0; k < 3; k++) {
		if (sets[k]) {
			uint32_t len = 32;

			memcpy (ptr (sets[k], len), out[k], len);
		}
	}
	sel.active = false;
	global_error = 0;
	done (r, sel.ret, false, 0, total);
	return true;
}

bool
DimmNet::socket_option (uint16_t ret, const uint32_t *a, bool set, Reply &r)
{
	Sock *s = get (a[1]);
	int level = 0, opt = 0;

	if (!s || s->busy ()) {
		done (r, ret, true, (uint16_t) a[1], 0xFFFFFFFF);
		return true;
	}
	if (a[2] == 0xFFFF) {
		level = SOL_SOCKET;
		switch (a[3]) {
		case 0x0004: opt = SO_REUSEADDR; break;
		case 0x0008: opt = SO_KEEPALIVE; break;
		case 0x0020: opt = SO_BROADCAST; break;
		case 0x0080: opt = SO_LINGER; break;
		case 0x1001: opt = SO_SNDBUF; break;
		case 0x1002: opt = SO_RCVBUF; break;
		default: break;
		}
	} else if (a[2] == 6 && a[3] == 1) {
		level = IPPROTO_TCP;
		opt = TCP_NODELAY;
	}
	if (!opt) {
		NOTE ("option %X of level %X not known", a[3], a[2]);
		fail (s, VX_ENOPROTOOPT);
		done (r, ret, true, (uint16_t) a[1], 0xFFFFFFFF);
		return true;
	}

	int rc;

	if (set) {
		uint32_t len = a[5];

		rc = setsockopt (s->fd, level, opt, (const char *) ptr (a[4], len), (int) len);
	} else {
		uint32_t len = 4;
		socklen_t size = (socklen_t) rd32 (a[5]);
		uint8_t *to = ptr (a[4], len);

		if (size > len)
			size = (socklen_t) len;
		rc = getsockopt (s->fd, level, opt, (char *) to, &size);
		if (rc == 0)
			wr32 (a[5], (uint32_t) size);
	}
	if (rc < 0)
		fail (s, vx_error (last_error ()));
	done (r, ret, rc < 0, (uint16_t) a[1], (uint32_t) rc);
	return true;
}

bool
DimmNet::command (uint16_t ret, uint8_t buffer_index, Reply &r)
{
	const uint32_t base = buffers + 0x800000 + 0x1000 * buffer_index;
	uint32_t a[8];

	for (unsigned i = 0; i < 8; i++)
		a[i] = rd32 (base + i * 4);

	const uint32_t index = a[1];
	Sock *s = NULL;
	int rc;

	switch (a[0]) {
	case 2:		/* bind */
	case 3:		/* close */
	case 4:		/* connect */
	case 8:		/* listen */
	case 9:		/* recv */
	case 10:	/* send */
	case 16:	/* settimeout */
	case 22:	/* recvfrom */
	case 23:	/* sendto */
		s = get (index);
		if (!s) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		break;
	default:
		break;
	}

	switch (a[0]) {
	case 1: {	/* accept */
		s = get (index);
		if (!s || s->accepting) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		s->accept_ret = ret;
		s->accept_addr = a[2];
		s->accept_addr_len = a[3];
		s->accept_end = 0;
		if (try_accept (s, index, r))
			return true;
		s->accepting = true;
		return false;
	}
	case 2: {	/* bind */
		uint32_t ip;
		unsigned port;
		sockaddr_in at {};

		if (s->busy ()) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		game_address (a[2], ip, port);
		at.sin_family = AF_INET;
		at.sin_addr.s_addr = htonl (listening ? INADDR_ANY : INADDR_LOOPBACK);
		at.sin_port = htons ((uint16_t) (port ? (int) port + port_offset : 0));
		rc = bind (s->fd, (const sockaddr *) &at, sizeof (at));
		if (rc < 0)
			fail (s, vx_error (last_error ()));
		NOTE ("bind socket %u to port %u -> %d", index, port, rc);
		done (r, ret, rc < 0, (uint16_t) index, (uint32_t) rc);
		return true;
	}
	case 3:		/* close */
		if (s->smcp)
			smcp->reset ();
		s->close ();
		done (r, ret, false, (uint16_t) index, 0);
		return true;
	case 4:		/* connect */
		if (s->busy ()) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		return start_connect (s, index, ret, a, r);
	case 5: {	/* getIpByDns: names the map has, none else */
		uint32_t len = 256;
		const char *name = (const char *) ptr (a[1], len);
		std::string wanted (name, strnlen (name, len));

		for (const Peer &p : map) {
			if (p.name == wanted) {
				NOTE ("DNS '%s' -> %08X", wanted.c_str (), p.host);
				done (r, ret, false, 0, swap32 (p.host));
				return true;
			}
		}
		NOTE ("DNS '%s': not in DIMM_MAP", wanted.c_str ());
		done (r, ret, false, 0, 0);
		return true;
	}
	case 6: {	/* inet_addr */
		uint32_t len = 64, ip;
		const char *text = (const char *) ptr (a[1], len);

		if (!parse_ip (std::string (text, strnlen (text, len)), ip)) {
			done (r, ret, true, 0, 0xFFFFFFFF);
			return true;
		}
		done (r, ret, false, 0, swap32 (ip));
		return true;
	}
	case 8:		/* listen */
		rc = listen (s->fd, (int) a[2]);
		if (rc < 0)
			fail (s, vx_error (last_error ()));
		done (r, ret, rc < 0, (uint16_t) index, (uint32_t) rc);
		return true;
	case 9:		/* recv */
	case 22:	/* recvfrom */
		if (s->receiving) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		s->recv_at = a[2];
		s->recv_len = a[3];
		s->recv_from = a[0] == 22;
		s->recv_flags = a[0] == 22 ? a[4] & 3 : 0;	/* OOB, PEEK */
		s->recv_addr = a[0] == 22 ? a[5] : 0;
		s->recv_addr_len = a[0] == 22 ? a[6] : 0;
		s->recv_ret = ret;
		if (try_recv (s, index, r))
			return true;
		s->receiving = true;
		s->recv_end = deadline_of (now_us, s->recv_ms);
		return false;
	case 10:	/* send */
		if (s->sending) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		s->send_at = a[2];
		s->send_len = a[3];
		s->send_ret = ret;
		if (try_send (s, index, r))
			return true;
		s->sending = true;
		s->send_end = deadline_of (now_us, s->send_ms);
		return false;
	case 11: {	/* socket */
		const bool stream = a[2] == 1;

		if (a[1] != AF_INET || (a[2] != 1 && a[2] != 2)) {
			global_error = VX_EINVAL;
			done (r, ret, true, 0, 0xFFFFFFFF);
			return true;
		}
		if (socks.size () >= 60) {
			bool free_slot = false;

			for (const Sock *t : socks)
				free_slot |= t->fd == NO_SOCKET;
			if (!free_slot) {
				global_error = VX_EMFILE;
				done (r, ret, true, 0, 0xFFFFFFFF);
				return true;
			}
		}

		uintptr_t fd = (uintptr_t) socket (AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, (int) a[3]);

		if (fd == NO_SOCKET) {
			global_error = vx_error (last_error ());
			done (r, ret, true, 0, 0xFFFFFFFF);
			return true;
		}
		global_error = 0;

		uint32_t added = add_socket (fd, stream);

		NOTE ("socket type %u -> %u", a[2], added);
		done (r, ret, false, 0, added);
		return true;
	}
	case 12:	/* select */
		if (sel.active) {
			global_error = VX_EINVAL;
			done (r, ret, true, 0, 0xFFFFFFFF);
			return true;
		}
		return do_select (ret, a, r);
	case 14:	/* setsockopt */
		return socket_option (ret, a, true, r);
	case 15:	/* getsockopt */
		return socket_option (ret, a, false, r);
	case 16:	/* settimeout, in milliseconds */
		if (s->busy ()) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		s->connect_ms = a[2];
		s->send_ms = a[3];
		s->recv_ms = a[4];
		done (r, ret, false, (uint16_t) index, 0);
		return true;
	case 17:	/* errno */
		if (index == 0) {
			done (r, ret, false, 0, (uint32_t) global_error);
			return true;
		}
		s = get (index);
		done (r, ret, s == NULL, (uint16_t) index, s ? (uint32_t) s->error : 0xFFFFFFFF);
		return true;
	case 20:	/* the parameters by DHCP: nothing to do */
		done (r, ret, false, 0, 0);
		return true;
	case 23:	/* sendto */
		if (s->sending) {
			done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
			return true;
		}
		return do_send_to (s, index, ret, a, r);
	default:
		HL_ERROR_FEW ("naomi: Net DIMM: network function %u not known", a[0]);
		done (r, ret, true, (uint16_t) index, 0xFFFFFFFF);
		return true;
	}
}

bool
DimmNet::poll_sockets (Reply &r)
{
	const size_t n = socks.size ();
	fd_set in, out, except;
	bool any = false;
	uintptr_t top = 0;

	FD_ZERO (&in);
	FD_ZERO (&out);
	FD_ZERO (&except);
	for (const Sock *s : socks) {
		if (s->fd == NO_SOCKET || !s->busy () || s->smcp)
			continue;
		if (s->connecting || s->sending)
			FD_SET (s->fd, &out);
		if (s->receiving || s->accepting)
			FD_SET (s->fd, &in);
		FD_SET (s->fd, &except);
		top = std::max (top, s->fd);
		any = true;
	}
	if (any) {
		timeval tv {};

		if (select ((int) top + 1, &in, &out, &except, &tv) < 0) {
			FD_ZERO (&in);
			FD_ZERO (&out);
			FD_ZERO (&except);
		}
	}
	for (size_t k = 0; k < n; k++) {
		const size_t j = (next_socket + k) % n;
		Sock *s = socks[j];
		const uint32_t index = (uint32_t) j + 1;

		if (s->fd == NO_SOCKET || !s->busy ())
			continue;
		next_socket = (unsigned) (j + 1);

		const bool ready_in = s->smcp ? smcp->pending () : FD_ISSET (s->fd, &in) || FD_ISSET (s->fd, &except);
		const bool ready_out = s->smcp || FD_ISSET (s->fd, &out) || FD_ISSET (s->fd, &except);

		if (s->connecting) {
			bool ended = false;
			int error = 0;

			if (ready_out) {
				socklen_t size = sizeof (error);

				getsockopt (s->fd, SOL_SOCKET, SO_ERROR, (char *) &error, &size);
				ended = true;
			} else if (s->connect_end && now_us >= s->connect_end) {
				error = NE(TIMEDOUT);
				ended = true;
			}
			if (ended) {
				s->connecting = false;
				s->error = vx_error (error);
				NOTE ("connect of socket %u ended: %d", index, s->error);
				done (r, s->connect_ret, error != 0, (uint16_t) index, error ? 0xFFFFFFFF : 0);
				return true;
			}
			continue;
		}
		if (s->accepting) {
			if (ready_in && try_accept (s, index, r))
				return true;
			if (s->accepting && s->accept_end && now_us >= s->accept_end) {
				s->accepting = false;
				fail (s, VX_ETIMEDOUT);
				done (r, s->accept_ret, true, (uint16_t) index, 0xFFFFFFFF);
				return true;
			}
		}
		if (s->receiving) {
			if (ready_in && try_recv (s, index, r))
				return true;
			if (s->receiving && s->recv_end && now_us >= s->recv_end) {
				s->receiving = false;
				fail (s, VX_ETIMEDOUT);
				done (r, s->recv_ret, true, (uint16_t) index, 0xFFFFFFFF);
				return true;
			}
		}
		if (s->sending) {
			if (ready_out && try_send (s, index, r))
				return true;
			if (s->sending && s->send_end && now_us >= s->send_end) {
				s->sending = false;
				fail (s, VX_ETIMEDOUT);
				done (r, s->send_ret, true, (uint16_t) index, 0xFFFFFFFF);
				return true;
			}
		}
	}
	if (sel.active) {
		if (check_select (r, false))
			return true;
		if (sel.deadline && now_us >= sel.deadline)
			return check_select (r, true);
	}
	return false;
}

bool
DimmNet::poll (uint64_t elapsed_us, Reply &r)
{
	now_us += elapsed_us;
	if (smcp)
		smcp->tick (elapsed_us);
	return poll_sockets (r);
}

/* What one machine connected to the service has going */
struct DimmNet::Client {
	uintptr_t fd = NO_SOCKET;
	std::vector<uint8_t> in, out;
	size_t	 sent = 0;
	uint32_t dl_addr = 0, dl_left = 0, dl_seq = 1;
	bool	 control = false;	/* waits for the NAOMI's control word */
	bool	 ended = false;
};

static uint16_t
get16 (const uint8_t *p)
{
	return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t
get32 (const uint8_t *p)
{
	return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void
push16 (std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back ((uint8_t) v);
	out.push_back ((uint8_t) (v >> 8));
}

static void
push32 (std::vector<uint8_t> &out, uint32_t v)
{
	push16 (out, v);
	push16 (out, v >> 16);
}

void
DimmNet::open_service (const char *address)
{
	const unsigned port = (unsigned) (10703 + port_offset);
	uint32_t ip;

	if (!parse_ip (address, ip)) {
		HL_ERROR ("naomi: [Network] DIMM_SERVICE_IP '%s' is not an address", address);
		return;
	}

	uintptr_t fd = (uintptr_t) socket (AF_INET, SOCK_STREAM, 0);

	if (fd == NO_SOCKET)
		return;
#ifdef _WIN32
	u_long on = 1;
#else
	int on = 1;

	setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof (on));
#endif
	ioctlsocket (fd, FIONBIO, &on);

	sockaddr_in at {};

	at.sin_family = AF_INET;
	at.sin_addr.s_addr = htonl (ip);
	at.sin_port = htons ((uint16_t) port);
	if (bind (fd, (const sockaddr *) &at, sizeof (at)) < 0 || listen (fd, 4) < 0) {
		HL_ERROR ("naomi: Net DIMM: the service cannot listen on %s:%u", address, port);
		closesocket (fd);
		return;
	}
	service_fd = fd;
	HL_PRINT ("naomi: Net DIMM: service on %s:%u", address, port);
}

void
DimmNet::close_service ()
{
	for (Client *c : clients) {
		closesocket (c->fd);
		delete c;
	}
	clients.clear ();
	if (service_fd != NO_SOCKET)
		closesocket (service_fd);
	service_fd = NO_SOCKET;
}

void
DimmNet::send_packet (Client *c, uint8_t opcode, uint8_t flags, const std::vector<uint8_t> &payload)
{
	push16 (c->out, (uint32_t) payload.size ());
	c->out.push_back (flags);
	c->out.push_back (opcode);
	c->out.insert (c->out.end (), payload.begin (), payload.end ());
}

/* As much of a read of the DIMM as the connection takes now */
void
DimmNet::pump_download (Client *c)
{
	while (c->dl_left && c->out.size () - c->sent < 64 * 1024) {
		uint32_t chunk = std::min (c->dl_left, (uint32_t) 8192);
		const uint8_t *from = ptr (c->dl_addr, chunk);
		std::vector<uint8_t> p;

		push32 (p, c->dl_seq++);
		push32 (p, c->dl_addr);
		push16 (p, 0);
		p.insert (p.end (), from, from + chunk);
		send_packet (c, 4, chunk < c->dl_left ? 0x80 : 0x81, p);
		c->dl_addr += chunk;
		c->dl_left -= chunk;
	}
}

void
DimmNet::serve_packet (Client *c, const uint8_t *packet, uint32_t size)
{
	const uint8_t opcode = packet[3];
	const uint8_t *data = packet + 4;
	const uint32_t len = size - 4;
	std::vector<uint8_t> p;

	switch (opcode) {
	case 0x01:	/* nothing */
		break;
	case 0x04:	/* write the DIMM: sequence, address, 2 bytes, the data */
		if (len > 10) {
			uint32_t n = len - 10;
			uint8_t *to = ptr (get32 (data + 4), n);

			memcpy (to, data + 10, n);
		}
		break;
	case 0x05: {	/* read the DIMM */
		if (len < 8)
			break;

		uint32_t addr = get32 (data), count = get32 (data + 4), value = 0;
		const uint32_t low = addr & 0x3FFFFFFF;

		/* The state of the check of the loaded data, and the game's size */
		if (low == 0x3FFEFFE0 || low == 0x3FFF0004) {
			value = low == 0x3FFEFFE0 ? 2 : info_size;
			push32 (p, 1);
			push32 (p, addr);
			push16 (p, 0);
			push32 (p, value);
			send_packet (c, 4, 0x81, p);
			break;
		}
		c->dl_addr = addr;
		c->dl_left = count;
		c->dl_seq = 1;
		pump_download (c);
		break;
	}
	case 0x07:	/* host mode: the bits set, the bits cleared */
		if (len < 2)
			break;
		push32 (p, data[0] & (uint32_t) ~data[1]);
		send_packet (c, 7, 0, p);
		break;
	case 0x09:
		c->ended = true;
		break;
	case 0x0A:
		NOTE ("service: restart asked, not done");
		break;
	case 0x10: {	/* peek */
		if (len < 8)
			break;

		const uint32_t kind = get32 (data + 4);

		push32 (p, 0);
		push32 (p, host->bus_read (get32 (data), kind == 1 ? 1 : kind == 2 ? 2 : 4));
		send_packet (c, 0x10, 0, p);
		break;
	}
	case 0x16:	/* the host control word: the NAOMI's answer later */
		c->control = true;
		if (control_state == CONTROL_NONE)
			control_state = CONTROL_WANTED;
		break;
	case 0x17:	/* the time limit */
		NOTE ("service: time limit %u", len >= 4 ? get32 (data) : 0);
		break;
	case 0x18: {	/* the board and the game */
		const uint32_t mb = (mask + 1) >> 20;

		push16 (p, 0xC);
		push16 (p, 0x317);
		push16 (p, mb - 16);
		push16 (p, mb);
		push32 (p, info_crc);
		send_packet (c, 0x18, 0, p);
		break;
	}
	case 0x19:	/* what the machine says it has loaded: crc, size */
		if (len >= 8) {
			info_crc = get32 (data);
			info_size = get32 (data + 4);
			NOTE ("service: set_information %08X, %u bytes", info_crc, info_size);
		}
		break;
	case 0x7F:	/* the key of the data: already decrypted here */
		break;
	default:
		NOTE ("service: opcode %02X not known", opcode);
		break;
	}
}

void
DimmNet::control_answer (uint32_t address)
{
	std::vector<uint8_t> p;

	push32 (p, 0);
	push32 (p, address);
	for (Client *c : clients) {
		if (c->control) {
			c->control = false;
			send_packet (c, 0x10, 0x81, p);
		}
	}
	control_state = CONTROL_NONE;
}

/* What is waiting to leave; false: the connection is gone */
bool
DimmNet::flush_client (Client *c)
{
	while (c->sent < c->out.size ()) {
		int n = send (c->fd, (const char *) c->out.data () + c->sent, (int) (c->out.size () - c->sent), 0);

		if (n <= 0)
			return n < 0 && pending_error (last_error ());
		c->sent += (size_t) n;
	}
	c->out.clear ();
	c->sent = 0;
	return true;
}

void
DimmNet::service (uint64_t elapsed_us)
{
	if (service_fd == NO_SOCKET)
		return;
	service_us += elapsed_us;
	if (service_us < 1000)
		return;
	service_us = 0;

	uintptr_t fd = (uintptr_t) accept (service_fd, NULL, NULL);

	if (fd != NO_SOCKET) {
		if (clients.size () >= 4) {
			closesocket (fd);
		} else {
#ifdef _WIN32
			u_long on = 1;
#else
			int on = 1;
#endif
			const int nodelay = 1;
			Client *c = new Client ();

			ioctlsocket (fd, FIONBIO, &on);
			setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof (nodelay));
			c->fd = fd;
			clients.push_back (c);
			NOTE ("service: a client connected");
		}
	}

	for (size_t i = 0; i < clients.size (); ) {
		Client *c = clients[i];
		bool alive = true;

		while (alive && !c->ended) {
			uint8_t buffer[4096];
			int n = recv (c->fd, (char *) buffer, sizeof (buffer), 0);

			if (n == 0) {
				alive = false;
			} else if (n < 0) {
				alive = pending_error (last_error ());
				break;
			} else {
				c->in.insert (c->in.end (), buffer, buffer + n);
			}
			/* Whole packets out of what came in */
			size_t at = 0;

			while (!c->ended && c->in.size () - at >= 4) {
				const uint32_t size = 4 + (uint32_t) get16 (c->in.data () + at);

				if (c->in.size () - at < size)
					break;
				serve_packet (c, c->in.data () + at, size);
				at += size;
			}
			c->in.erase (c->in.begin (), c->in.begin () + (std::ptrdiff_t) at);
		}
		pump_download (c);
		alive = flush_client (c) && alive;
		if (!alive || (c->ended && c->out.empty ())) {
			closesocket (c->fd);
			delete c;
			clients.erase (clients.begin () + (std::ptrdiff_t) i);
			continue;
		}
		i++;
	}
	if (control_state != CONTROL_NONE) {
		bool waiting = false;

		for (const Client *c : clients)
			waiting |= c->control;
		if (!waiting)
			control_state = CONTROL_NONE;
	}
}

} /* namespace naomi */
