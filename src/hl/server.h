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

#ifndef __HL_SERVER_H__
#define __HL_SERVER_H__

#include <functional>
#include <string>
#include <vector>

#include "hl/core.h"

namespace hl {

/* A line-based TCP server on 127.0.0.1, for the Lua shell. No threads: the
 * main loop polls it between two frames, so that the commands it receives
 * find the machine still. */
class Server {
public:
	Server ();
	~Server ();

	Server (const Server &) = delete;
	Server &operator= (const Server &) = delete;

	/* False when the port cannot be listened on */
	bool	 open (unsigned port);

	/* Takes the clients that connected, and the lines they sent: greet
	 * (client) for each new client, line (client, text) for each complete
	 * line, without its end */
	void	 poll (const std::function<void (int)> &greet,
		       const std::function<void (int, const char *)> &line);

	void	 send (int client, const char *text, size_t length);
	void	 broadcast (const char *text);

	unsigned port;

private:
	struct Client {
		int		 id;
		uintptr_t	 sock;
		std::string	 input;
		bool		 closed;
	};

	void	 close (Client &client);

	uintptr_t		 listener;
	std::vector<Client>	 clients;
	int			 next_id;
};

} /* namespace hl */

#endif /* __HL_SERVER_H__ */
