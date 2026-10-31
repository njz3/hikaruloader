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

#ifndef __HL_JVS_H__
#define __HL_JVS_H__

#include <string>
#include <vector>

#include "hl/core.h"
#include "hl/device.h"

namespace hl {

class JvsDevice;

/* The JVS bus (JAMMA Video Standard): the RS-485 line from the main board to
 * the cabinet's I/O boards, chained one after the other. Seen from its host
 * (the Hikaru's MIE): a packet goes out a byte at a time, then its answer
 * comes back a byte at a time; and two lines tell whether a device is there,
 * and whether every device has its address.
 *
 * A packet: E0 (sync), the destination node (FF: all), the length of what
 * follows, the commands, the sum of the bytes from the node on. D0 escapes an
 * E0 or a D0 in the rest: D0 then the byte minus one. The answer goes to node
 * 00, and starts with a status: 01 fine, 02 unknown command, 03 sum error.
 *
 * As a device of the machine, its host side is also a set of registers, for
 * the Lua shell: put (1, 0, byte) sends, put (1, 1, any) ends the packet,
 * get (1, 0) reads the answer (FF when it is over), get (1, 1) the lines (bit
 * 0: a byte to read, bit 1: a device, bit 2: all addressed). */
class JvsBus : public Device {
public:
	explicit JvsBus (Machine *mach);

	/* At the end of the chain */
	void	 attach (JvsDevice *dev);

	void	 send (uint8_t byte);
	void	 end_packet ();
	/* The answer's next byte; -1 when there is none */
	int	 receive ();
	bool	 can_receive () const { return in_pos < in.size (); }
	/* The host UART's FIFO resets: what was sent of a packet, what is
	 * left of an answer */
	void	 flush_send () { out.clear (); }
	void	 flush_receive () { in.clear (); in_pos = 0; }
	bool	 present () const { return !chain.empty (); }
	bool	 all_addressed () const;

	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;
	/* "log": every packet and its answer, to the log */
	bool	 set_option (const char *name, int value) override;

	std::vector<JvsDevice *> chain;
	unsigned packets;

private:
	void	 handle (const std::vector<uint8_t> &packet);

	/* As they travel, escaped */
	std::vector<uint8_t> out, in;
	size_t	 in_pos;
	bool	 log;
};

/* A device on the JVS bus: an I/O board, a card reader... It answers the
 * common commands here; the subclasses supply what they have. */
class JvsDevice : public Device {
public:
	/* Attaches itself at the end of bus's chain */
	JvsDevice (Machine *mach, JvsBus *bus);

	/* The commands of a packet for this device: their reports go to
	 * reports. False on an unknown command */
	bool	 answer (const uint8_t *data, size_t size, std::vector<uint8_t> &reports);
	/* The broadcast reset (F0 D9): no address any more */
	void	 bus_reset () { address = 0xFF; }

	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;

	/* 0xFF until the host gives it one */
	uint8_t	 address;

protected:
	/* "MAKER;NAME;VERSION;DATE" */
	virtual const char *identity () = 0;
	virtual uint8_t	 command_revision () { return 0x13; }
	virtual uint8_t	 jvs_revision () { return 0x30; }
	virtual uint8_t	 comm_revision () { return 0x10; }
	/* Its functions, 4 bytes each, without the closing 00 */
	virtual void	 functions (std::vector<uint8_t> &out) { (void) out; }
	/* False when it has no such function, or not that many */
	virtual bool	 switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out)
	{ (void) players; (void) bytes; (void) out; return false; }
	virtual bool	 coins (unsigned slots, std::vector<uint8_t> &out)
	{ (void) slots; (void) out; return false; }
	virtual bool	 coin_change (unsigned slot, int count)
	{ (void) slot; (void) count; return false; }
	virtual bool	 analogs (unsigned channels, std::vector<uint8_t> &out)
	{ (void) channels; (void) out; return false; }
	virtual bool	 outputs (unsigned bytes, const uint8_t *data)
	{ (void) bytes; (void) data; return false; }
};

/* The Sega 837-13551 "I/O BD JVS", the cabinets' usual I/O board: two players
 * of 13 switches, two coin slots, eight analog channels, six outputs. The
 * switches come from the controls (hl::Input); the wheel and the pedals
 * (channels 0, 1, 2) from controls.ini's [Driving] (hl::Controls), but a
 * channel the shell set: as registers, channel n at 2n (16 bits), the coins
 * at 10 and 12, the outputs at 14 (read only). */
class SegaIoBoard : public JvsDevice {
public:
	SegaIoBoard (Machine *mach, JvsBus *bus);

	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;

	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;

	/* 16 bits each, the top ones meaningful: a wheel's middle is 8000 */
	uint16_t analog[8];
	/* Set by the shell (or the agent): no longer the controllers' */
	bool	 analog_set[8];
	/* The coins counted, and the outputs (lamps) the game drives */
	uint16_t coin[2];
	uint8_t	 output;

protected:
	const char *identity () override;
	uint8_t	 command_revision () override { return 0x11; }
	uint8_t	 jvs_revision () override { return 0x20; }
	uint8_t	 comm_revision () override { return 0x10; }
	void	 functions (std::vector<uint8_t> &out) override;
	bool	 switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out) override;
	bool	 coins (unsigned slots, std::vector<uint8_t> &out) override;
	bool	 coin_change (unsigned slot, int count) override;
	bool	 analogs (unsigned channels, std::vector<uint8_t> &out) override;
	/* The wheel and the pedals from the controllers */
	void	 read_controls ();
	bool	 outputs (unsigned bytes, const uint8_t *data) override;

private:
	/* A coin counts when its switch goes down */
	void	 count_coins ();
	bool	 coin_held[2];
};

class DriveBoard;

/* The Sega 837-13844-01 "I/O CNTL BD2": the 837-13551's inputs (12 switches
 * a player), 22 outputs. On the racing cabinets a Model 3 drive board hangs
 * on it (F355 on the NAOMI, NASCAR Arcade on the Hikaru): its command on the
 * outputs 15 to 22 (the second byte's bits 1-0, the third's 7-2), bits
 * reversed and active low; its reply on the players' second switch bytes,
 * bits 7 to 4, each the inverse of a reply bit: player 1 bits 0-3, player
 * 2 bits 4-7. Outputs 9 to 14 at 15 (read only). */
class SegaIoBoard13844 : public SegaIoBoard {
public:
	/* drive: none, or the drive board on it (the machine keeps it) */
	SegaIoBoard13844 (Machine *mach, JvsBus *bus, DriveBoard *drive);

	int	 get (unsigned size, uint32_t addr, void *val) override;
	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;

	DriveBoard *drive;
	/* Outputs 9 to 14, in bits 7 to 2 */
	uint8_t	 output2;

protected:
	const char *identity () override;
	void	 functions (std::vector<uint8_t> &out) override;
	bool	 switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out) override;
	bool	 outputs (unsigned bytes, const uint8_t *data) override;
};

} /* namespace hl */

#endif /* __HL_JVS_H__ */
