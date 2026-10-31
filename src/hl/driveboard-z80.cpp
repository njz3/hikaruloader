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

#include "hl/driveboard-z80.h"
#include "hl/state.h"

namespace hl {

/*
 * The CTC
 * =======
 *
 * A control word has bit 0 set: bit 7 its interrupt, 6 counter mode, 5 the
 * prescaler (256, or 16), 3 a trigger starts the timer (or the time
 * constant does), 2 a time constant follows, 1 a reset. EPR-20512 runs
 * channel 0 as a timer (16 x 250 cycles: 2 kHz) and channel 1 counting
 * channel 0 down from 4: the NMI at 500 Hz, every 16000 cycles.
 */

unsigned
DriveBoardZ80::ctc_period () const
{
	unsigned period0;

	if (!s.ctc_running[1])
		return 0;
	if (!(s.ctc_control[1] & 0x40))
		return s.ctc_constant[1] * ((s.ctc_control[1] & 0x20) ? 256 : 16);
	/* Counting channel 0, a timer */
	if (!s.ctc_running[0] || (s.ctc_control[0] & 0x40))
		return 0;
	period0 = s.ctc_constant[0] * ((s.ctc_control[0] & 0x20) ? 256 : 16);
	return period0 * s.ctc_constant[1];
}

DriveBoardZ80::DriveBoardZ80 (const uint8_t *data, size_t size, unsigned strength) :
	rom (data, data + size),
	z80 (this),
	servo (NULL)
{
	/* As Supermodel's CF: 1-3 to 1-5 the strength's row (ON reads 0),
	 * the others as the cabinets have them */
	dip1 = (uint8_t) (0xC3 | (~((strength & 7) << 2) & 0x1C));
	dip2 = 0xFF;
	rom.resize (ROM_SIZE > rom.size () ? ROM_SIZE : rom.size (), 0xFF);
	reset ();
}

void
DriveBoardZ80::reset ()
{
	memset (&s, 0, sizeof (s));
	s.wheel = 0x80;
	s.centre = 0x80;
	z80.reset ();
}

uint8_t
DriveBoardZ80::read (uint16_t addr)
{
	if (addr < ROM_SIZE)
		return rom[addr];
	if (addr >= 0xE000)
		return s.ram[addr - 0xE000];
	return 0xFF;
}

void
DriveBoardZ80::write (uint16_t addr, uint8_t val)
{
	if (addr >= 0xE000)
		s.ram[addr - 0xE000] = val;
}

uint8_t
DriveBoardZ80::in (uint16_t port)
{
	uint8_t p = port & 0xFF, val;

	switch (p) {
	case 0x20:
		return dip1;
	case 0x21:
		return dip2;
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
		if (p != s.adc_port || !s.adc_bit)
			return 0xFF;
		switch (p) {
		case 0x25:
			/* The wheel; centred until the board is ready: no motor
			 * moves it to its centre for the calibration */
			val = s.ready ? s.wheel : s.centre;
			break;
		case 0x26:
			/* The cockpit: none */
			val = 0x80;
			break;
		default:
			/* The servo board's speed (the displays alone), and
			 * nothing */
			val = 0x00;
			break;
		}
		s.adc_bit--;
		if (p == 0x25) {
			s.adc_value = (uint8_t) ((s.adc_value << 1) | ((val >> s.adc_bit) & 1));
			if (!s.adc_bit && observer)
				observer (s.nmis, 0x25, s.adc_value);
		}
		return (val >> s.adc_bit) & 1;
	case 0x28:
		if (observer)
			observer (s.nmis, 0x28, s.command);
		return s.command;
	case 0x2C:
		/* The servo board's alarms: none */
		return 0x00;
	}
	return 0xFF;
}

void
DriveBoardZ80::out (uint16_t port, uint8_t val)
{
	uint8_t p = port & 0xFF;
	unsigned ch;

	if (observer && (p == 0x29 || p == 0x2A || p == 0x2D || p == 0x2E))
		observer (s.nmis, p, val);

	switch (p) {
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
		ch = p & 3;
		if (val & 1) {
			s.ctc_control[ch] = val;
			s.ctc_waiting[ch] = (val & 0x04) != 0;
			if (val & 0x02)
				s.ctc_running[ch] = false;
		} else if (s.ctc_waiting[ch]) {
			s.ctc_constant[ch] = val ? val : 256;
			s.ctc_waiting[ch] = false;
			/* Started by the time constant, unless by a trigger */
			s.ctc_running[ch] = !(s.ctc_control[ch] & 0x08);
		}
		if (ch == 1)
			s.nmi_timer = (int32_t) ctc_period ();
		break;
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
		s.seg[p & 3] = val;
		break;
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
		/* A conversion: the next 8 reads */
		s.adc_port = p;
		s.adc_bit = 8;
		break;
	case 0x29:
		s.out_reply = val;
		if (val == 0xCC)
			s.ready = true;
		break;
	case 0x2A:
		s.servo_data = val;
		if (servo)
			servo->servo_write (s.servo_reg, val);
		break;
	case 0x2D:
		s.lamps = val;
		break;
	case 0x2E:
		s.servo_reg = val;
		break;
	default:
		/* 1C-1F the PIO, F0 and F1 the watchdog */
		break;
	}
}

void
DriveBoardZ80::set_inputs (uint8_t command, uint8_t wheel)
{
	s.command = command;
	s.wheel = wheel;
	/* CB starts again from CE: the wheel at its centre until CC; CD's
	 * 70-8F, the centre */
	if (command == 0xCB && s.last_command != 0xCB)
		s.ready = false;
	else if (!s.ready && s.out_reply == 0xCD && command >= 0x70 && command <= 0x8F)
		s.centre = command;
	s.last_command = command;
}

void
DriveBoardZ80::nmi (uint8_t command, uint8_t wheel, Servo &to)
{
	servo = &to;
	set_inputs (command, wheel);
	run (CLOCK / DriveBoard::NMI_HZ);
	servo = NULL;
}

void
DriveBoardZ80::run (int left)
{
	while (left > 0) {
		unsigned period = ctc_period ();
		int slice = left;

		if (period && s.nmi_timer > 0 && s.nmi_timer < slice)
			slice = s.nmi_timer;
		slice = z80.run (slice);
		left -= slice;
		if (period) {
			s.nmi_timer -= slice;
			/* Every period exactly, as the CTC counts: the
			 * instruction's overshoot kept (Supermodel loses it,
			 * which supermodel_timing does, for the comparisons) */
			if (s.nmi_timer <= 0) {
				z80.nmi ();
				s.nmis++;
				s.nmi_timer = (supermodel_timing ? 0 : s.nmi_timer) + (int32_t) period;
			}
		}
	}
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
DriveBoardZ80::save_state (State *state)
{
	int ret = 0;
	SAVE (s);
	SAVE (z80.regs);
	return ret;
}

int
DriveBoardZ80::load_state (State *state)
{
	int ret = 0;
	LOAD (s);
	LOAD (z80.regs);
	return ret;
}

#undef SAVE
#undef LOAD

const char *
DriveBoardZ80::debug_string ()
{
	static char out[96];
	snprintf (out, sizeof (out), "Z80 @%04X, NMI %llu, command %02X, reply %02X%s",
	          z80.regs.pc, (unsigned long long) s.nmis, s.command, s.out_reply,
	          s.ready ? "" : " (starting)");
	return out;
}

} /* namespace hl */
