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

/* An I/O board that runs its own program at the far end of the line (the
 * [Emulation] IO_BOARD_PROGRAM setting, hl/ioboard.h), instead of the
 * simulated JvsDevices: the host's bytes reach its UART one at a time, at the
 * line's speed, and its replies come back the same way, through the bus's
 * line (JvsBus::attach_line). Times are ticks of 1/9.216 GHz from the reset,
 * in which the MIE's Z80 clock (16 MHz, 576 ticks) and the UARTs' crystal
 * (14.7456 MHz, 625 ticks) are both whole */
class JvsLine {
public:
	static const uint64_t TICKS_PER_Z80_CYCLE = 576;
	static const uint64_t TICKS_PER_UART_CLOCK = 625;

	virtual ~JvsLine () {}

	/* The board's name in the log: "837-13551" */
	virtual const char *name () const = 0;
	virtual void	 reset () = 0;
	/* Runs the board up to tick, taking the host's bytes at their time
	 * (JvsBus::line_next_byte) and giving it its own (JvsBus::line_event) */
	virtual void	 run_until (uint64_t tick) = 0;
	/* Where the board is */
	virtual uint64_t now () const = 0;
	virtual int	 save_state (State *state) = 0;
	virtual int	 load_state (State *state) = 0;
	virtual const char *debug_string () { return NULL; }
};

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
 * 0: a byte to read, bit 1: a device, bit 2: all addressed).
 *
 * With a line (attach_line: an I/O board running its program), the packets
 * are no longer handled here: each byte the host sends goes to the board as
 * the line carries it (line_send), and the board's bytes and its sense line
 * come back as events, given to the host at their time (line_deliver); the
 * chain's simulated board stays, for its inputs and outputs only. */
class JvsBus : public Device {
public:
	explicit JvsBus (Machine *mach);
	~JvsBus () override;

	/* At the end of the chain */
	void	 attach (JvsDevice *dev);
	/* The board that runs its program: the bus keeps it, and deletes it */
	void	 attach_line (JvsLine *board);
	bool	 line_mode () const { return line != NULL; }
	JvsLine	*line_board () { return line; }

	void	 send (uint8_t byte);
	void	 end_packet ();
	/* The answer's next byte; -1 when there is none */
	int	 receive ();
	bool	 can_receive () const { return in_pos < in.size (); }
	/* The host UART's FIFO resets: what was sent of a packet, what is
	 * left of an answer */
	void	 flush_send () { out.clear (); }
	void	 flush_receive () { in.clear (); in_pos = 0; }
	bool	 present () const { return !chain.empty () || line; }
	bool	 all_addressed () const;

	/* The line, the host's side. A byte its UART starts at tick (or once
	 * the one before has gone), a character of char_ticks */
	void	 line_send (uint8_t byte, uint64_t tick, uint64_t char_ticks);
	/* The transmitter's FIFO empty (every byte started), and the whole of
	 * it (the last stop bit gone), at tick */
	bool	 line_fifo_empty (uint64_t tick) const;
	bool	 line_idle (uint64_t tick) const { return tick >= line_free; }
	/* The FIFO reset: the bytes not started yet dropped */
	void	 line_flush_send (uint64_t tick);
	/* The board run to at least tick, if it is not there yet */
	void	 line_run (uint64_t tick);
	uint64_t line_board_now () const { return line ? line->now () : 0; }
	/* The board's next event for the host, UINT64_MAX for none; the
	 * events up to tick given to the host's UART and sense line */
	uint64_t line_next_event () const;
	void	 line_deliver (uint64_t tick);

	/* The line, the board's side: the start of the host's next byte,
	 * UINT64_MAX for none, and that byte taken */
	uint64_t line_next_byte () const;
	uint8_t	 line_take_byte ();
	/* A byte for the host, or its sense line (all addressed or not), from
	 * tick on */
	void	 line_event (uint64_t tick, bool sense, uint8_t value);

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

	/* The line: what goes either way, in the order of their ticks */
	struct LineEvent {
		uint64_t tick;
		uint8_t	 sense;		/* the sense line, not a byte */
		uint8_t	 value;
	};
	JvsLine	*line;
	std::vector<LineEvent> to_board, to_host;
	/* The host's transmitter free from then on; the board's sense line as
	 * the host has it */
	uint64_t line_free;
	bool	 line_addressed;
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
	/* Channels 0 and 1 Star Wars: Racer Arcade's levers, from the
	 * controls as TWIN_LEVERS says (twin_lever_thrusts ()) */
	bool	 twin_levers = false;
	/* Set by the shell (or the agent): no longer the controllers' */
	bool	 analog_set[8];
	/* The coins counted, and the outputs (lamps) the game drives */
	uint16_t coin[2];
	uint8_t	 output;

	/* The cabinet's side, for a board that runs its own program
	 * (hl::JvsLine) on these same inputs and outputs: the system byte
	 * (TEST in bit 7), a player's switches as SWITCH_INPUTS gives them
	 * (START in bit 15, closed at 1), a channel (its middle 8000), the
	 * coin switches pressed since the last call, and the outputs the
	 * program drives, as the outputs command writes them (output 1 in bit
	 * 7 of the first byte) */
	uint8_t	 system_bits ();
	virtual uint16_t player_bits (unsigned player);
	uint16_t analog_bits (unsigned channel);
	void	 take_coins (unsigned presses[2]);
	void	 drive_outputs (const uint8_t *data, unsigned bytes) { outputs (bytes, data); }

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
	~SegaIoBoard13844 ();

	int	 get (unsigned size, uint32_t addr, void *val) override;
	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;

	DriveBoard *drive;
	/* Outputs 9 to 14, in bits 7 to 2 */
	uint8_t	 output2;

	/* With the drive board's reply in bits 7-4, as on the cabinet; the
	 * view change button on player 1's (view_bits ()), the gear lever on
	 * player 2's (shifter_bits ()) */
	uint16_t player_bits (unsigned player) override;

protected:
	const char *identity () override;
	void	 functions (std::vector<uint8_t> &out) override;
	bool	 switches (unsigned players, unsigned bytes, std::vector<uint8_t> &out) override;
	bool	 outputs (unsigned bytes, const uint8_t *data) override;

private:
	/* The drive board's reply player_bits (0) read, for player 2's */
	uint8_t	 line_reply;

	/* The gear lever's (hl::Gearbox), on player 2's switches */
	uint16_t shifter_bits ();
	/* The view change button's, on player 1's */
	uint16_t view_bits ();
};

/* Star Wars: Racer Arcade's levers from the controls, 0 (at rest) to 1
 * (pushed forward, the engine's whole thrust): an axis each, or the
 * accelerator and the wheel (Config::twin_levers) */
void	 twin_lever_thrusts (double *left, double *right);

} /* namespace hl */

#endif /* __HL_JVS_H__ */
