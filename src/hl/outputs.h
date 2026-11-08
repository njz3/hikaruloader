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

#ifndef __HL_OUTPUTS_H__
#define __HL_OUTPUTS_H__

#include <mutex>
#include <string>
#include <vector>

#include "hl/core.h"

namespace hl {

/* The cabinet's outputs: the lamps and motors its I/O board drives, and the
 * commands of its drive board, for the programs that drive real ones
 * (MAMEHooker, BackForceFeeder, DOFLinx...), in MAME's network protocol as
 * Supermodel and sm2-emu send it ([Outputs] in hikaruloader.ini):
 *
 * - A TCP server (PORT, 8000) on the PC's addresses, sending "name = value\r"
 *   lines at each change. A client that connects gets "mame_start = <set>"
 *   first, then every value known; "pause = 1" while the game is paused,
 *   "mame_stop = 1" when it ends.
 * - When a game starts, "mame_start = <set>\rtcp = <port>\r" in a UDP
 *   datagram to 127.0.0.1 (ANNOUNCE_PORT, 8001), as Supermodel sends it:
 *   BackForceFeeder waits for it before it connects.
 *
 * The I/O board's general outputs are lamp0 to lamp15 as Flycast names them,
 * the bits of the outputs command's bytes (JVS output 1 on lamp7, 8 on
 * lamp0, 9 on lamp15); each game's own names follow for those its OUTPUT
 * TEST shows (LampStart...), the same values again. NASCAR Arcade's drive
 * board gets its commands as "m3ffb = XX" (hex), Supermodel's and Flycast's
 * name; a client that answers "m3ffb_rx = XX", a real drive board's reply,
 * is what the game reads from then on, until it leaves. */
class Outputs {
public:
	static Outputs	*get ();
	~Outputs ();

	/* The machine's I/O board has count general outputs (6 on the
	 * 837-13551, 14 on the 837-13844), and a drive board on it or not;
	 * from the machine being built */
	void	 board (unsigned count, bool drive_board);
	/* The game runs: its set, and its parent's ("" for a parent) for its
	 * names. Its values go to the clients from then on */
	void	 start (const char *game, const char *parent);
	/* It ended: mame_stop, and what its machine declared is forgotten */
	void	 stop ();
	void	 set_paused (bool paused);

	/* What the I/O board's outputs command wrote, its general outputs'
	 * bytes (outputs 1 to 8 in the first, from bit 7) */
	void	 jvs (const uint8_t *data, unsigned bytes);
	/* JVS output n (1 to 8), as the game last wrote it, sent to the
	 * clients or not */
	bool	 jvs_output (unsigned n);
	/* What the player does, which no output of the cabinet tells
	 * (hl::GameRumble): a name the game declares (shot1, nozzle1...) */
	void	 signal (const char *name, int value);
	/* A command written to the drive board, as the board reads it */
	void	 drive_command (uint8_t command);
	/* The reply of a client's real drive board (m3ffb_rx): false when no
	 * client gives one */
	bool	 drive_reply (uint8_t *reply);

	/* Between two frames: the settings taken (at once), the clients
	 * accepted, what they sent read, what waits sent */
	void	 poll ();

	/* For the settings: what the running game sends (empty with none),
	 * each name with its type, its meaning (through tr) and its last
	 * value ("-" before the first) */
	struct Row {
		std::string	 name, type, meaning, value;
	};
	std::vector<Row> rows (const char *(*tr) (const char *));

	struct Status {
		bool		 enabled;	/* in the settings */
		bool		 listening;
		unsigned	 clients;
		std::string	 error;		/* why not listening */
	};
	Status	 status ();

private:
	Outputs ();

	/* One name sent */
	struct Item {
		std::string	 name;
		int		 bit;		/* lamp<bit>'s; -1 the drive board's */
		const char	*meaning;	/* the game's use, N_ (); NULL */
		bool		 alias;		/* the game's name for lamp<bit> */
		int		 value;
		bool		 known;		/* false before the first value */
	};
	struct Client {
		uintptr_t	 sock;
		std::string	 input;		/* a line begun */
		std::string	 pending;	/* what the socket did not take yet */
	};

	/* The first byte of the outputs command */
	uint8_t	 jvs_first = 0;

	void	 open_server ();
	void	 close_server ();
	void	 set (Item &item, int value);
	std::string line (const Item &item) const;
	void	 send_all (const std::string &text);
	void	 greet (Client &client);
	void	 flush (Client &client);
	void	 received (Client &client, const std::string &text);
	void	 announce ();
	void	 forget_reply ();

	std::mutex	 lock;

	/* The machine's, and the game's */
	unsigned	 jvs_count;
	bool		 has_drive;
	std::string	 game;
	bool		 started, paused;
	std::vector<Item> items;

	/* The server, as the settings have it */
	bool		 enabled;
	unsigned	 port, announce_port;
	bool		 winsock;
	uintptr_t	 listener;
	std::vector<Client> clients;
	std::string	 error;

	/* The real drive board's last reply, and its client's socket */
	bool		 reply_set;
	uint8_t		 reply;
	uintptr_t	 reply_from;
};

} /* namespace hl */

#endif /* __HL_OUTPUTS_H__ */
