/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Flycast's core/hw/naomi/netdimm.h (GPL-2.0-or-later,
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

#ifndef HL_NAOMI_NETDIMM_H
#define HL_NAOMI_NETDIMM_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mach/naomi/naomi-smcp.h"

namespace naomi {

/*
 * The socket library of a Net DIMM's firmware: see naomi-netdimm.cpp. One for
 * each DIMM board, nothing shared with the others: the boards of one process
 * may run on threads of their own.
 */
class DimmNet {
public:
	/* What the board answers in its mailbox */
	struct Reply {
		uint16_t command;
		uint16_t offset;
		uint32_t parameter;
		bool	 failed;
	};

	/* The NAOMI, as the board's TCP service reads it */
	class Host {
	public:
		virtual ~Host () {}
		/* A read of 1, 2 or 4 bytes on the NAOMI's bus */
		virtual uint32_t bus_read (uint32_t addr, unsigned bytes) = 0;
	};

	/* memory: the DIMM's, size bytes (a power of 2); buffers: where its
	 * 16 MB of command buffers begin; file_size and file_crc: the game's
	 * file, as the TCP service reports it */
	DimmNet (uint8_t *memory, uint32_t size, uint32_t buffers, Host *host,
	         uint32_t file_size, uint32_t file_crc, bool motion_pc);
	~DimmNet ();

	/* The board's address, as [Network] gives it */
	uint32_t address () const { return own_ip; }

	/* A command of the network group: true, the reply is there; false,
	 * it comes later, from poll () */
	bool	 command (uint16_t ret, uint8_t buffer_index, Reply &reply);
	/* The time gone by; true: a command ended, the reply is there */
	bool	 poll (uint64_t elapsed_us, Reply &reply);
	void	 close_all ();

	/* The board's own TCP service (port 10703), [Network] DIMM_LISTEN on:
	 * the time gone by */
	void	 service (uint64_t elapsed_us);
	/* A client waits for the NAOMI's control word: the mailbox's command
	 * is to be sent, then the NAOMI answers it */
	bool	 control_wanted () const { return control_state == CONTROL_WANTED; }
	void	 control_sent () { control_state = CONTROL_SENT; }
	void	 control_answer (uint32_t address);

private:
	struct Sock;
	struct Client;
	enum { CONTROL_NONE, CONTROL_WANTED, CONTROL_SENT };
	struct Peer {
		std::string name;	/* a host name, "": an address */
		uint32_t ip;		/* the address the game uses, 0 any */
		unsigned port;		/* the game's port, 0 any */
		uint32_t host;		/* where it is on this PC's network */
		unsigned host_port;	/* a port there, 0: the game's */
		int	 offset;	/* added to the game's port */
	};
	struct Target {
		uint32_t ip;
		unsigned port;
	};
	struct Select {
		bool	 active;
		uint16_t ret;
		uint32_t count, in, out, except;
		uint64_t deadline;	/* 0: none */
	};

	uint32_t rd32 (uint32_t at) const;
	void	 wr32 (uint32_t at, uint32_t val);
	uint8_t *ptr (uint32_t at, uint32_t &len);
	Sock	*get (uint32_t index);
	void	 fail (Sock *s, int error);
	void	 done (Reply &r, uint16_t ret, bool failed, uint16_t offset, uint32_t parameter) const;

	void	 parse_map (const char *text);
	/* Where the game's address and port are; none: not reachable */
	void	 targets (uint32_t ip, unsigned port, std::vector<Target> &out) const;
	void	 write_address (uint32_t addr_at, uint32_t len_at, uint32_t ip, unsigned port);
	void	 game_address (uint32_t at, uint32_t &ip, unsigned &port) const;
	void	 seen_as (uint32_t ip, unsigned port, uint32_t &game_ip, unsigned &game_port) const;

	bool	 start_connect (Sock *s, uint32_t index, uint16_t ret, const uint32_t *args, Reply &r);
	bool	 do_send_to (Sock *s, uint32_t index, uint16_t ret, const uint32_t *args, Reply &r);
	bool	 do_select (uint16_t ret, const uint32_t *args, Reply &r);
	bool	 check_select (Reply &r, bool timed_out);
	bool	 try_recv (Sock *s, uint32_t index, Reply &r);
	bool	 try_send (Sock *s, uint32_t index, Reply &r);
	bool	 try_accept (Sock *s, uint32_t index, Reply &r);
	bool	 socket_option (uint16_t ret, const uint32_t *args, bool set, Reply &r);
	bool	 poll_sockets (Reply &r);
	uint32_t add_socket (uintptr_t fd, bool stream);

	void	 open_service (const char *address);
	void	 close_service ();
	void	 serve_packet (Client *c, const uint8_t *packet, uint32_t size);
	void	 send_packet (Client *c, uint8_t opcode, uint8_t flags, const std::vector<uint8_t> &payload);
	void	 pump_download (Client *c);
	bool	 flush_client (Client *c);

	uint32_t mask, buffers;
	uint8_t	*mem;
	uint32_t own_ip;
	int	 port_offset;
	bool	 listening;
	std::vector<Peer> map;
	std::vector<Sock *> socks;
	Select	 sel;
	int	 global_error;
	uint64_t now_us;
	unsigned next_socket;
	unsigned notes;		/* what the log has been told so far */

	Host	*host;
	uint32_t info_size, info_crc;	/* the file, or what set_information said */
	uintptr_t service_fd;
	std::vector<Client *> clients;
	uint64_t service_us;
	int	 control_state;

	/* A Cycraft cabinet's PC, for the games that have one */
	std::unique_ptr<SmcpPeer> smcp;
};

} /* namespace naomi */

#endif
