/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * The board's memory map, ports and CTC as Supermodel emulates them
 * (Src/Model3/DriveBoard/DriveBoard.cpp, WheelBoard.cpp, Z80CTC.cpp;
 * Copyright 2011-2021 Bart Trzynadlowski, Nik Henson, Ian Curtis, Harry
 * Tuttle and Spindizzi, GPL v3 or later).
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

#ifndef __HL_DRIVEBOARD_Z80_H__
#define __HL_DRIVEBOARD_Z80_H__

#include <functional>
#include <vector>

#include "hl/driveboard.h"
#include "cpu/z80/z80.h"

namespace hl {

/* A Model 3 drive board's Z80 running its EPROM (EPR-20512, Sega Rally 2's,
 * on NASCAR Arcade's cabinets), instead of the simulation (DriveBoardSr2).
 * A Z80 at 8 MHz:
 *
 *   0000-8FFF  The EPROM
 *   E000-FFFF  RAM (8 KB)
 *
 *   Ports, in:  20, 21 the DIP switches (ON reads 0); 24-27 the ADC's
 *               channels, a bit at a time, MSB first, after a write to the
 *               port (25 the wheel: 00 left, 80 the centre, FF right; 26
 *               the cockpit, 80); 28 the main board's command; 2C the
 *               encoder's errors (0, none)
 *          out: 10-13 the CTC (channel 1's timer: the NMI, 500 Hz); 20-23
 *               the two 7-segment displays; 29 the reply to the main board;
 *               2E a servo board register (F8-FF), then 2A its value; 2D
 *               the lamps; F0, F1 the watchdog
 *
 * Each nmi () runs 2 ms of it (the NMIs where its CTC puts them). */
class DriveBoardZ80 : public DriveBoardProgram, private Z80::Bus {
public:
	static const unsigned CLOCK = 8000000;
	static const unsigned ROM_SIZE = 0x9000;
	static const unsigned RAM_SIZE = 0x2000;

	/* rom: the EPROM (64 KB, the first 36 used); strength: the operator's
	 * row, DIP switches 1-3 to 1-5 (ON reads 0), 0 to 7, as DriveBoardSr2's;
	 * 4 gives Supermodel's CF */
	DriveBoardZ80 (const uint8_t *rom, size_t size, unsigned strength = 4);

	void	 reset () override;
	void	 nmi (uint8_t command, uint8_t wheel, Servo &servo) override;
	uint8_t	 reply () const override { return out_reply; }

	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;

	/* As the program reads them: ON reads 0 */
	uint8_t	 dip1, dip2;

	/* The ports read and written (28 and 25 read, 29, 2A, 2D, 2E written;
	 * 25 as the byte the 8 reads make), for the comparisons with
	 * Supermodel's journal (tests/driveboard_z80_test.cpp) */
	std::function<void (uint64_t nmi, uint8_t port, uint8_t val)> observer;
	uint64_t nmis () const { return s.nmis; }

	/* What nmi () does in two: the main board's command and the wheel,
	 * then cycles of the Z80 (the servo board's writes go nowhere unless
	 * from nmi ()); for a test that runs it by frames, as Supermodel */
	void	 set_inputs (uint8_t command, uint8_t wheel);
	void	 run (int cycles);
	/* The NMIs as Supermodel times them (each from the end of the
	 * instruction it falls in, a few cycles late), for the comparisons
	 * with its journals; off, every 16000 cycles as the CTC counts */
	bool	 supermodel_timing = false;

private:
	uint8_t	 read (uint16_t addr) override;
	void	 write (uint16_t addr, uint8_t val) override;
	uint8_t	 in (uint16_t port) override;
	void	 out (uint16_t port, uint8_t val) override;

	/* The CTC channel 1's period, in Z80 cycles; 0 when stopped */
	unsigned ctc_period () const;

	std::vector<uint8_t> rom;
	Z80	 z80;
	/* During nmi (): where the servo board's writes go */
	Servo	*servo;

	/* Plain data, for the states */
	struct {
		uint8_t	 ram[RAM_SIZE];
		/* The CTC: control words and time constants */
		uint8_t	 ctc_control[4];
		uint16_t ctc_constant[4];
		bool	 ctc_running[4];
		bool	 ctc_waiting[4];
		/* Cycles to the next NMI */
		int32_t	 nmi_timer;
		uint8_t	 command, wheel;
		uint8_t	 adc_port, adc_bit;
		uint8_t	 servo_reg, servo_data;
		uint8_t	 out_reply;
		uint8_t	 seg[4];
		uint8_t	 lamps;
		/* Replied CC: ready, the wheel read as it is; until then (and
		 * after a CB) at its centre, 80 or what CD's 70-8F set: no motor
		 * brings it there for the calibration */
		bool	 ready;
		uint8_t	 centre;
		/* The command of the last nmi (): a CB only once */
		uint8_t	 last_command;
		/* The 8 bits of the ADC read so far */
		uint8_t	 adc_value;
		uint64_t nmis;
	} s;
	uint8_t	&out_reply = s.out_reply;
};

} /* namespace hl */

#endif /* __HL_DRIVEBOARD_Z80_H__ */
