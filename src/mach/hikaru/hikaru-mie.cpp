/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-mie.c, Copyright (C) 2011, Stefano Teso (released under the GNU GPL version 2 or later).
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

#include <algorithm>
#include <functional>

#include "hl/input.h"
#include "hl/config.h"
#include "hl/state.h"

#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-aica.h"
#include "mach/hikaru/hikaru-mie.h"

namespace hikaru {

/*
 * MIE
 * ===
 *
 * Includes a Z80 processor clocked at ? MHz.
 *
 * MME Ports
 * =========
 *
 * 00800000  16-bit RW	Unknown \ Related
 * 00800002  16-bit  W	Unknown /
 * 00800004  16-bit  W	Unknown; = 0000
 * 00800006  16-bit RW	MIE Control
 *			bit 1 is accessed in @0C00BDFC; turn ON/OFF [active low]
 *			bit 5 is accessed in @0C007880, @0C002AB8; lock/unlock BRAM? see @0C002C4A
 *			bit 8 is accessed in @0C0078F8, related to hi/lo-res video mode
 * 00800008  16-bit RW	Unknown
 *			bits 0-3 are read (and looped if == 0) in @0C00BDFC
 *			Some kind of ready bits
 * 0080000A  16-bit RW	Serial Port; = 007F
 * 0080000C  16-bit RW	Serial Port; = FFFF; plus MAINBD Switches
 * 00800010  16-bit  W	Unknown; = 0043
 *
 * Basic setup is done in @0C001956. Most bits seem active-low, with the
 * exception of 00800004. Access to register is guarded either by 01000000 or
 * 01000100.
 *
 * 0082F000   8-bit RW	MIE Z80 Control; 0x80 is the Z80 reset/start bit
 *
 * 00830000-0083FFFF  8,16-bit RW	MIE RAM: even bytes only [2]
 *
 * Note: airtrix polls 0083800[02]
 *
 * [1] 7800 in Z80 space
 * [2] 8000-FFFF in Z80 space
 *
 * MIE Serial Bus
 * ==============
 *
 * Perhaps queries devices attached to the (MAPLE?) bus?
 *
 * The Z80 (Hikaruloader)
 * ======================
 *
 * 00820000-0083FFFF is the whole Z80 space, on the even bytes: 0082F000 is
 * the Z80's 7800, 00830000 its 8000. The game puts its Z80 program at 8000
 * (a JR to 8010 first), then sets 7800 bit 7: the MIE's internal ROM (2 KB,
 * 315-6146, not dumped with the Hikaru) waits for that bit, then jumps to
 * 8010. A stub of ours does the same. The program talks with the SH-4
 * through a mailbox at C000, and with the JVS bus through the Z80's ports:
 *
 * 00-07, 08	GPIO, and their direction (1: input): an input reads the
 *		outside, an output what was written
 * 0F		ADC
 * 10-17	A 16550 UART, the JVS line: 10 data, 12 FIFO control (bit 1
 *		empties the answer, bit 2 the packet), 13 LCR, 15 LSR
 * 30, 50, 70	The interrupts: enabled, level, pending; levels 0 to 6, in
 *		mode 2 at the vectors F2 + 2 * level. An answer from the
 *		JVS bus raises level 3
 * 90		Bit 0 drives the RS-485 line: a packet ends when it falls
 * 91		The JVS lines: bit 0 no device, bit 1 all of them addressed
 *
 * With an I/O board running its own program on the line (IO_BOARD_PROGRAM,
 * hl::JvsLine), the UART's bytes go out one at a time, LSR's THRE and TEMT
 * telling when they have, the board's replies come in as the line brings
 * them, and port 91's bit 1 is the board's sense line (see run_line ())
 * 40, 80, 81, 84	Unknown: kept as written, see log_ports
 *
 * As the NAOMI's MIE, which MAME emulates (src/mame/sega/mie.cpp). The
 * Hikaru's GPIO (MAME's hikaru.cpp; the BIOS makes A, B and G inputs):
 *
 * A	The DIP switches, not fitted: FF
 * B	The interrupts for the SH-4's IRL2 (active high); C, their mask. None
 *	is routed through them here: B reads 00
 * D	Resets, active low (system, GPU, slave SH-4...), the video clock:
 *	bit 5 resets the sound boards (see drive ()), the rest kept as written
 * E	LEDs, the GPU's JTAG
 * F	The 93C46 EEPROM: bit 0 SK, 1 DI, 3 CS; bits 4-6, interrupts for B
 * G	Bit 0 the EEPROM's DO, 2 /Service, 3 /Test (MAME swaps them)
 *
 * The SH-4 sees the ports at 00800000-0081FFFF, on the even bytes: port n
 * at 00800000 + 2n (above, Valkyrie's names for those it uses: 00800010 is
 * the GPIO's direction). The odd bytes read FF: the games use the 16 bits.
 */

/* The internal ROM's part: wait for 7800 bit 7, then go */
static const uint8_t boot_stub[] = {
	0x3A, 0x00, 0x78,	/* 0000  LD A,(7800h) */
	0xE6, 0x80,		/* 0003  AND 80h */
	0x28, 0xF9,		/* 0005  JR Z,0000h */
	0xC3, 0x10, 0x80,	/* 0007  JP 8010h */
};

/* The Z80 at 16 MHz, as MAME runs it (measured at about 13.5 on a NAOMI):
 * its T-states per 100 cycles of the SH-4s, 1600 / their clock in MHz (8 at
 * 200, 28 at 57). At such speeds SGNASCAR's MIE program waits some 4
 * seconds before it addresses the JVS board, and the game waits for it, as
 * the real one does. 200 MHz shortens that, but takes a sixth of the
 * emulator's time. See set_option ("speed") */
#define Z80_MHZ		16

/* The JVS bus's answers */
#define IRQ_JVS		3

/* The Z80's side */

uint8_t
Mie::read (uint16_t addr)
{
	if (addr < sizeof (boot_stub))
		return boot_stub[addr];
	if (addr < 0x7000)
		return 0xFF;
	if (addr < 0x7800)
		return 0x00;
	if (addr < 0x8000)
		return (uint8_t) ram_lo->get (1, addr & 0x7FF);
	return (uint8_t) ram->get (1, addr & 0x7FFF);
}

void
Mie::write (uint16_t addr, uint8_t val)
{
	idle.touched = true;
	if (addr >= 0x8000)
		ram->put (1, addr & 0x7FFF, val);
	else if (addr >= 0x7800)
		ram_lo->put (1, addr & 0x7FF, val);
}

void
Mie::update_irq ()
{
	z80->set_irq ((io.irq_enable & io.irq_pending & 0x7F) != 0);
}

void
Mie::raise_irq (unsigned level)
{
	io.irq_pending |= 1 << level;
	update_irq ();
}

uint8_t
Mie::acknowledge ()
{
	idle.touched = true;
	uint8_t active = io.irq_enable & io.irq_pending & 0x7F;
	unsigned level;

	for (level = 0; level < 7; level++)
		if (active & (1 << level))
			break;
	if (level == 7)
		return 0xFF;
	io.irq_pending &= ~(1 << level);
	update_irq ();
	return (uint8_t) (0xF2 + 2 * level);
}

/* The ports with a meaning here; the others keep what was written */
static bool
known_port (uint8_t port)
{
	return port <= 0x0D || port == 0x0F || (port >= 0x10 && port <= 0x17) ||
	       port == 0x30 || port == 0x50 || port == 0x70 || port == 0x90 || port == 0x91;
}

/* The serial line to a card reader (VON4's), ports 09-0D, as VON4's program
 * for it (at 9000) uses them: 0A the byte, then 09 the operation; 0D bit 3
 * once its answer is there (bit 2 for the loopback), bits 0-1 busy; the
 * answer in 0B (the operation) and 0C. The operations:
 *
 *   01  Send 0A to the reader; 0C, the byte sent
 *   55  The loopback; 0C, 0A
 *   80  Poll; 0C bit 6, a byte from the reader to read
 *   81  Read it; 0C, the byte
 *
 * The answer comes at once. The program also sets up a 16550 at 20-27
 * (divisor 8, 8N1, FIFOs), kept as written */
void
Mie::serial_write (uint8_t op)
{
	serial.answer[0] = op;
	serial.status = 0x08;
	switch (op) {
	case 0x01:
		serial.answer[1] = serial.data;
		card_reader->receive (serial.data);
		break;
	case 0x55:
		serial.answer[1] = serial.data;
		serial.status = 0x04;
		break;
	case 0x80:
		serial.answer[1] = card_reader->has_byte () ? 0x40 : 0;
		break;
	case 0x81:
		serial.answer[1] = card_reader->send ();
		break;
	default:
		HL_PRINT ("mie: serial operation %02X (%02X)", op, serial.data);
		serial.answer[1] = serial.data;
		break;
	}
}

uint8_t
Mie::in (uint16_t port)
{
	uint8_t val = port_read ((uint8_t) port);

	/* The inputs, the JVS bus: they change */
	idle.touched = true;
	if (log_ports && (port & 0xFF) != 0x15)
		HL_PRINT ("mie: Z80 %04X: in %02X = %02X%s", z80->regs.pc, port & 0xFF, val,
		          known_port ((uint8_t) port) ? "" : " (unknown)");
	return val;
}

void
Mie::out (uint16_t port, uint8_t val)
{
	idle.touched = true;
	if (log_ports)
		HL_PRINT ("mie: Z80 %04X: out %02X = %02X", z80->regs.pc, port & 0xFF, val);
	port_write ((uint8_t) port, val);
}

uint8_t
Mie::port_read (uint8_t port)
{
	uint8_t val;
	int byte;

	switch (port) {
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
		val = gpio_read (port);
		break;
	case 0x08:
		val = io.gpio_dir;
		break;
	case 0x09: case 0x0A:
		val = io.other[port];
		break;
	case 0x0B:
		val = card_reader ? serial.answer[0] : 0;
		break;
	case 0x0C:
		val = card_reader ? serial.answer[1] : 0;
		serial.status = 0;
		break;
	case 0x0D:
		val = card_reader ? serial.status : 0;
		break;
	case 0x0F:
		val = 0;
		break;
	case 0x10:
		if (io.lcr & 0x80)
			val = io.dll;
		else {
			byte = jvs->receive ();
			val = (byte < 0) ? 0 : (uint8_t) byte;
		}
		break;
	case 0x11:
		val = (io.lcr & 0x80) ? io.dlm : io.ier;
		break;
	case 0x12:
		/* No interrupt; the FIFOs, when on */
		val = 0x01 | ((io.fcr & 1) ? 0xC0 : 0);
		break;
	case 0x13:
		val = io.lcr;
		break;
	case 0x14:
		val = io.mcr;
		break;
	case 0x15:
		/* Sent and empty; a byte to read. With a board on the line, the
		 * bytes take their time to go */
		if (jvs->line_mode ())
			val = (jvs->can_receive () ? 1 : 0) |
			      (jvs->line_fifo_empty (line_ticks ()) ? 0x20 : 0) |
			      (jvs->line_idle (line_ticks ()) ? 0x40 : 0);
		else
			val = 0x60 | (jvs->can_receive () ? 1 : 0);
		break;
	case 0x16:
		val = 0;
		break;
	case 0x17:
		val = io.scr;
		break;
	case 0x30:
		val = io.irq_enable;
		break;
	case 0x50:
		val = io.irq_level;
		break;
	case 0x70:
		val = io.irq_pending;
		break;
	case 0x91:
		val = 0x8C | (jvs->all_addressed () ? 2 : 0) | (jvs->present () ? 0 : 1);
		break;
	default:
		val = io.other[port];
		break;
	}
	return val;
}

void
Mie::port_write (uint8_t port, uint8_t val)
{
	switch (port) {
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x04: case 0x05: case 0x06: case 0x07:
		gpio_write (port, val);
		break;
	case 0x08:
		gpio_set_dir (val);
		break;
	case 0x09:
		io.other[port] = val;
		if (card_reader)
			serial_write (val);
		break;
	case 0x0A:
		io.other[port] = val;
		serial.data = val;
		break;
	case 0x10:
		if (io.lcr & 0x80)
			io.dll = val;
		else if (jvs->line_mode ())
			jvs->line_send (val, line_ticks (), uart_char_ticks ());
		else
			jvs->send (val);
		break;
	case 0x11:
		if (io.lcr & 0x80)
			io.dlm = val;
		else
			io.ier = val;
		break;
	case 0x12:
		/* Bits 1 and 2 empty the FIFOs, and clear themselves */
		if (val & 0x02)
			jvs->flush_receive ();
		if ((val & 0x04) && jvs->line_mode ())
			jvs->line_flush_send (line_ticks ());
		else if (val & 0x04)
			jvs->flush_send ();
		io.fcr = val & ~0x06;
		break;
	case 0x13:
		io.lcr = val;
		break;
	case 0x14:
		io.mcr = val;
		break;
	case 0x17:
		io.scr = val;
		break;
	case 0x30:
		io.irq_enable = val;
		update_irq ();
		break;
	case 0x50:
		io.irq_level = val;
		break;
	case 0x70:
		io.irq_pending = val;
		update_irq ();
		break;
	case 0x90:
		/* The line released: the packet is out, its answer comes (a
		 * board on the line answers by itself) */
		if ((io.jvs_control & 1) && !(val & 1) && !jvs->line_mode ()) {
			jvs->end_packet ();
			if (jvs->can_receive ())
				raise_irq (IRQ_JVS);
		}
		io.jvs_control = val;
		break;
	default:
		io.other[port] = val;
		break;
	}
}

/* The GPIO, as MAME has it: an input reads the outside, an output what was
 * written */
uint8_t
Mie::gpio_read (unsigned port)
{
	if (!(io.gpio_dir & (1 << port)))
		return io.gpio[port];
	switch (port) {
	case 1:
		/* B: no interrupt for the SH-4 */
		return 0x00;
	case 6: {
		/* G: the EEPROM's DO, /Service, /Test; the fans' sensors and
		 * the JTAG's TDO high. MAME has Test at bit 2, but SGNASCAR
		 * enters its test menu on bit 3, as Valkyrie had it. With the
		 * I/O board running its program, Test and Service are the
		 * cabinet's, on that board, as on a cabinet: on both, the
		 * board's reaching the game a few frames after these, the
		 * BIOS took a press for two */
		bool mainbd = !jvs->line_mode ();
		return 0xF2 | (eeprom.data_out () ? 0x01 : 0) |
		       ((mainbd && hl::Input::action (hl::ACTION_SERVICE)) ? 0 : 0x04) |
		       ((mainbd && hl::Input::action (hl::ACTION_TEST)) ? 0 : 0x08);
	}
	default:
		/* A: no DIP switches */
		return 0xFF;
	}
}

void
Mie::gpio_write (unsigned port, uint8_t val)
{
	io.gpio[port] = val;
	if (!(io.gpio_dir & (1 << port)))
		drive (port);
}

void
Mie::gpio_set_dir (uint8_t dir)
{
	/* The ports turned to outputs drive what was written to them */
	uint8_t outputs = (uint8_t) ((dir ^ io.gpio_dir) & ~dir);

	io.gpio_dir = dir;
	for (unsigned port = 0; port < 8; port++)
		if (outputs & (1 << port))
			drive (port);
}

/* An output's lines to the outside */
void
Mie::drive (unsigned port)
{
	uint8_t val = io.gpio[port];

	/* D: the resets, active low. Bit 5, the sound boards' (SND_RES in
	 * MAME's hikaru.cpp): SGNASCAR pulls it as it starts, and again as it
	 * leaves its SYSTEM MENU, then loads the sound drivers again, which the
	 * ARMs left running the old ones never answered. The others are kept as
	 * written */
	if (port == 3) {
		uint8_t fell = (uint8_t) (resets & ~val);

		resets = val;
		if (fell & 0x20) {
			Hikaru *hikaru = (Hikaru *) mach;
			((Aica *) hikaru->aica_m)->reset_pin ();
			((Aica *) hikaru->aica_s)->reset_pin ();
		}
		return;
	}
	/* F: the EEPROM; its bits 4-6, the interrupts for B, are not routed */
	if (port != 5)
		return;
	/* Its writes, saved once they're over (hikaru_keep_nvram) */
	eeprom.write_lines ((val & 0x08) != 0, (val & 0x01) != 0, (val & 0x02) != 0);
}

/* With the backup RAM, in the game's nvram file */
void
Mie::save_eeprom ()
{
	eeprom.changed = false;
	((Hikaru *) mach)->save_nvram ();
}

/* The SH-4 writes 7800: bit 7 cleared sends the Z80 back to the stub */
void
Mie::write_lo (uint16_t addr, uint8_t val)
{
	uint8_t old = (uint8_t) ram_lo->get (1, addr & 0x7FF);

	ram_lo->put (1, addr & 0x7FF, val);
	if (addr == 0x7800 && (old & 0x80) && !(val & 0x80))
		z80->reset ();
}

/* The SH-4's side */

uint8_t
Mie::sh4_read (uint8_t port)
{
	uint8_t val;

	/* XXX hack (Valkyrie's): the BIOS (@0C00B860) waits for GPIO E's
	 * bits 0-3, which it has just cleared, to read other than 0; what sets
	 * them is not known. They toggle at each read */
	if (port == 0x04)
		io.gpio[4] ^= 0x0F;
	val = port_read (port);
	if (log_ports)
		HL_PRINT ("mie: SH-4: in %02X = %02X", port, val);
	return val;
}

void
Mie::sh4_write (uint8_t port, uint8_t val)
{
	if (log_ports)
		HL_PRINT ("mie: SH-4: out %02X = %02X", port, val);
	port_write (port, val);
}

int
Mie::get (unsigned size, uint32_t addr, void *val)
{
	hl::Device *dev = this;
	Mie *mie = (Mie *) dev;

	set_ptr (val, size, 0);
	if (addr >= 0x00800000 && addr <= 0x0081FFFF) {
		uint8_t port = (uint8_t) (addr >> 1);

		/* The ports on the even bytes; the odd ones read FF */
		switch (size) {
		case 1:
			set_ptr (val, 1, (addr & 1) ? 0xFF : sh4_read (port));
			break;
		case 2:
			set_ptr (val, 2, 0xFF00 | sh4_read (port));
			break;
		case 4:
			set_ptr (val, 4, 0xFF00FF00u | sh4_read (port) |
			         ((uint32_t) sh4_read ((uint8_t) (port + 1)) << 16));
			break;
		default:
			return -1;
		}
	} else if (addr >= 0x00820000 && addr <= 0x0083FFFF) {
		uint16_t z = (uint16_t) ((addr - 0x00820000) >> 1);

		/* The Z80's bytes on the even addresses */
		if (size == 4)
			set_ptr (val, 4, read (z) | (read ((uint16_t) (z + 1)) << 16));
		else if (!(addr & 1))
			set_ptr (val, size, read (z));
	} else
		return -1;
	return 0;
}

int
Mie::put (unsigned size, uint32_t addr, uint64_t val)
{
	idle.touched = true;
	if (addr >= 0x00800000 && addr <= 0x0081FFFF) {
		uint8_t port = (uint8_t) (addr >> 1);

		switch (size) {
		case 1:
			if (!(addr & 1))
				sh4_write (port, (uint8_t) val);
			break;
		case 2:
			sh4_write (port, (uint8_t) val);
			break;
		case 4:
			sh4_write (port, (uint8_t) val);
			sh4_write ((uint8_t) (port + 1), (uint8_t) (val >> 16));
			break;
		default:
			return -1;
		}
	} else if (addr >= 0x00820000 && addr <= 0x0083FFFF) {
		uint16_t z = (uint16_t) ((addr - 0x00820000) >> 1);
		unsigned i, num = (size == 4) ? 2 : 1;

		if ((addr & 1) && size == 1)
			return 0;
		for (i = 0; i < num; i++, z++, val >>= 16) {
			if (z >= 0x8000)
				ram->put (1, z & 0x7FFF, val & 0xFF);
			else if (z >= 0x7800)
				write_lo (z, (uint8_t) val);
		}
	} else
		return -1;
	return 0;
}

/* All of the Z80 but R and the cycles count */
static bool
same_state (const Z80::Registers &a, const Z80::Registers &b)
{
	return a.af.w == b.af.w && a.bc.w == b.bc.w && a.de.w == b.de.w &&
	       a.hl.w == b.hl.w && a.af2.w == b.af2.w && a.bc2.w == b.bc2.w &&
	       a.de2.w == b.de2.w && a.hl2.w == b.hl2.w && a.ix.w == b.ix.w &&
	       a.iy.w == b.iy.w && a.pc == b.pc && a.sp == b.sp && a.wz == b.wz &&
	       a.i == b.i && a.im == b.im && a.iff1 == b.iff1 && a.iff2 == b.iff2 &&
	       a.halted == b.halted && a.ei_delay == b.ei_delay &&
	       a.irq_line == b.irq_line && a.nmi_pending == b.nmi_pending;
}

int
Mie::exec (int cycles)
{
	if (jvs->line_mode ()) {
		run_line (cycles);
		return cycles;
	}
	if (busrq)
		return cycles;
	owed += cycles * speed / 100;
	run_for (owed);
	return cycles;
}

/* The Z80 for left T-states: one instruction at a time with HR_Z80_PROFILE
 * or MIE_IDLE_SKIP=0, else its idle loops at once */
void
Mie::run_for (int &left)
{
	if (!z80_pcs.empty ()) {
		while (left > 0) {
			uint16_t pc = z80->regs.pc;
			int t = z80->step ();
			z80_pcs[pc] += t;
			left -= t;
		}
	} else if (idle_skip)
		run_z80 (left);
	else
		while (left > 0)
			left -= z80->step ();
}

/* An I/O board running its program on the JVS line (IO_BOARD_PROGRAM): the
 * board a piece ahead of the Z80, then the Z80 up to where the board is,
 * stopped where the board has something for it (a byte, its sense line),
 * which the UART then shows. The board is never farther ahead than a byte of
 * the Z80 takes to reach it (73.8 us on the 837-13551). Held by the SH-4s
 * (BUSRQ), the Z80 waits while the line and the board go on */
#define LINE_PIECE	1024

void
Mie::run_line (int cycles)
{
	const uint64_t T = hl::JvsLine::TICKS_PER_Z80_CYCLE;
	int share = cycles * speed / 100;

	if (busrq) {
		line_stall += share;
		jvs->line_run (line_ticks ());
		jvs->line_deliver (line_ticks ());
		return;
	}
	owed += share;
	while (owed > 0) {
		uint64_t now, end, to, event;

		now = line_cycles ();
		jvs->line_deliver (now * T);
		end = now + owed;
		if (jvs->line_board_now () < end * T)
			jvs->line_run ((now + LINE_PIECE) * T);
		to = MIN2 (end, jvs->line_board_now () / T);
		event = jvs->line_next_event ();
		if (event != UINT64_MAX)
			to = MIN2 (to, (event + T - 1) / T);
		if (to > now) {
			int chunk = (int) (to - now), left = chunk;

			run_for (left);
			owed -= chunk - left;
		}
	}
	jvs->line_deliver (line_ticks ());
}

uint64_t
Mie::uart_char_ticks () const
{
	uint64_t divisor = (uint64_t) ((io.dlm << 8) | io.dll);

	/* 10 bits, 16 clocks of the 14.7456 MHz crystal each by the divisor
	 * (8 from the game's program: 115200 bauds) */
	return 10 * 16 * hl::JvsLine::TICKS_PER_UART_CLOCK * (divisor ? divisor : 1);
}

/* The Z80's T-states owed, its idle loops at once. Back at a loop's head
 * as it was there the last time, with nothing written, no port read or
 * written, no interrupt taken, R not read, and nothing written by the SH-4
 * in between, the Z80 runs the same turn again, and again, until one of
 * them happens: the whole turns that fit are counted at once, R and the
 * cycles as they would go, the rest run as usual. The games' MIE program
 * (the same in SGNASCAR, AIRTRIX, SWRACER) spends 79% of its time at
 * 8046-804F, polling the SH-4's command at C000. */
void
Mie::run_z80 (int &left)
{
	while (left > 0) {
		uint16_t pc = z80->regs.pc;

		if (idle.confirmed && pc == idle.head && !idle.touched &&
		    !z80->r_read && left > idle.t && same_state (z80->regs, idle.regs)) {
			int turns = (left - 1) / idle.t;

			left -= turns * idle.t;
			z80->regs.cycles += (uint64_t) turns * idle.t;
			z80->regs.r = (uint8_t) ((z80->regs.r & 0x80) |
			                         ((z80->regs.r + turns * idle.r) & 0x7F));
		}
		left -= z80->step ();
		/* A jump back: a loop's head, perhaps */
		if (z80->regs.pc < pc && pc - z80->regs.pc <= 64)
			came_back ();
	}
}

void
Mie::came_back ()
{
	if (idle.valid && z80->regs.pc == idle.head && !idle.touched &&
	    !z80->r_read && same_state (z80->regs, idle.regs)) {
		/* The first turn back: its length; the next ones go by it */
		if (!idle.confirmed) {
			idle.t = (int) (z80->regs.cycles - idle.regs.cycles);
			idle.r = (uint8_t) ((z80->regs.r - idle.regs.r) & 0x7F);
			idle.confirmed = idle.t > 0;
		}
		return;
	}
	idle.valid = true;
	idle.confirmed = false;
	idle.touched = false;
	idle.head = z80->regs.pc;
	idle.regs = z80->regs;
	z80->r_read = false;
}

/* The BIOS keeps the board's system settings in the EEPROM: two copies of 64
 * bytes, each the game's ID first, the cabinet type in byte 4 (the players,
 * 1 to 4), the country in byte 5 (0 Japan, 1 USA, 2 Export, 3 Australia,
 * 4 Korea: its COUNTRY SETTING), the advertise sound in byte 6 (0 off, 1 on:
 * the attract's sound; BIOS 0.92 @00003D58), the communication mode in
 * byte 9 (0 no link, 1 master, 2 slave, 3 relay; see hikaru-comm.cpp), its
 * other settings up to byte 29 (the coin ones in 16-19, from the game's header), the game's own from
 * byte 30, and in bytes 62-63 a CRC-16 of the 62 others (0x1021, its own
 * start; BIOS 0.92 @000074E4). On another game's ID the BIOS takes the new
 * one, keeps its settings but the coin ones, and clears the game's
 * (@0C00BA3C). The games read their language from the country. */
static uint16_t
get_settings_crc (const uint8_t *data, unsigned len)
{
	uint32_t crc = 0x01A18104;
	unsigned i, bit;

	for (i = 0; i <= len; i++) {
		if (i < len)
			crc |= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000) ? (crc << 1) ^ 0x10210000 : crc << 1;
	}
	return crc >> 16;
}

/* One of the settings' bytes, in each copy the BIOS has written (a blank
 * EEPROM keeps its own until then); true when it changed */
static bool
set_setting (uint8_t *mem, unsigned offs, int value)
{
	bool changed = false;
	unsigned copy;

	for (copy = 0; copy < 128; copy += 64) {
		uint8_t *s = mem + copy;
		uint16_t crc;

		if (get_settings_crc (s, 62) != (s[62] | (s[63] << 8)) ||
		    s[offs] == value)
			continue;
		s[offs] = (uint8_t) value;
		crc = get_settings_crc (s, 62);
		s[62] = crc & 0xFF;
		s[63] = crc >> 8;
		changed = true;
	}
	return changed;
}

/* The BIOS's settings when no copy is valid (BIOS 0.92 @0C00BC1C): no game's
 * ID yet, 2 players, Japan, no advertise sound... */
static const uint8_t default_settings[64] = {
	0x17, 0x02, 0x62, 0x19, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
	0x01, 0x01, 0x01, 0x00, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06
};

/* Its bytes 12-15 by the coin setting the game's header gives (@0C00BBA8;
 * the first, never given, the bytes of a pointer the table follows) */
static const uint8_t coin_settings[28][4] = {
	{ 0xE4, 0x74, 0x00, 0x8C }, { 1, 1, 1, 0 }, { 2, 1, 1, 0 }, { 3, 1, 1, 0 },
	{ 4, 1, 1, 0 }, { 5, 1, 1, 0 }, { 2, 2, 1, 0 }, { 5, 2, 1, 0 },
	{ 3, 3, 1, 0 }, { 4, 4, 1, 0 }, { 5, 5, 1, 0 }, { 6, 6, 1, 0 },
	{ 1, 1, 2, 0 }, { 2, 1, 2, 0 }, { 4, 1, 2, 0 }, { 1, 1, 1, 2 },
	{ 2, 1, 1, 2 }, { 1, 1, 3, 0 }, { 1, 1, 4, 0 }, { 1, 1, 1, 4 },
	{ 4, 1, 1, 4 }, { 1, 1, 5, 0 }, { 5, 1, 3, 5 }, { 1, 1, 2, 5 },
	{ 5, 1, 2, 5 }, { 1, 1, 1, 5 }, { 5, 1, 1, 5 }, { 0, 0, 0, 0 }
};

/* A blank EEPROM (no copy valid): the BIOS takes its defaults, then makes
 * them the game's as it does on another game's ID (@0C00BA5A): the game's
 * ID, byte 9 and the game's own settings cleared, bytes 10-20 from the
 * header's settings for the country (17 bytes at 206 + 17 country: the coin
 * chutes'). It checks the cabinet type against the header (the EPROM's byte
 * 3F3: bit n for n + 1 players; @0C00BC96) and stops on ERROR 7 when the
 * game does not take it: SGNASCAR takes 1 player only. It writes none of it
 * until an operator changes a setting: written here at once, so that the
 * settings of hikaruloader.ini apply from the first start, the cabinet type
 * the first the game takes (its operator's in SYSTEM ASSIGNMENTS). Checked
 * against the BIOS's own writes. True when written */
static bool
init_settings (uint8_t *mem, hl::Buffer *eprom, int country)
{
	uint8_t s[64], h[17], types;
	unsigned copy, i;

	if (!eprom || eprom->size () < 0x400)
		return false;
	for (copy = 0; copy < 128; copy += 64)
		if (get_settings_crc (mem + copy, 62) == (mem[copy + 62] | (mem[copy + 63] << 8)))
			return false;

	memcpy (s, default_settings, sizeof (s));
	for (i = 0; i < 4; i++)
		s[i] = (uint8_t) eprom->get (1, 0x136 + i);
	s[5] = (uint8_t) country;
	s[9] = 0;
	memset (s + 30, 0, 32);
	for (i = 0; i < sizeof (h); i++)
		h[i] = (uint8_t) eprom->get (1, 0x206 + 17 * country + i);
	if (h[0]) {
		/* The header's byte 3, the coin setting: by the table; past
		 * 27, its bytes 4-7 */
		if (h[3] > 27) {
			s[12] = h[6];
			s[13] = h[4];
			s[14] = h[5];
			s[15] = h[7];
		} else
			memcpy (s + 12, coin_settings[h[3]], 4);
		s[10] = (uint8_t) (((h[2] & 1) << 1) | (h[3] == 27) |
		                   ((s[13] == s[14]) << 2) | ((h[3] > 27) << 3));
		s[11] = h[3];
		/* Its bytes 8-15, the nibbles of 16-19 */
		for (i = 0; i < 4; i++)
			s[16 + i] = (uint8_t) ((h[8 + 2 * i] << 4) | h[9 + 2 * i]);
		s[20] = (h[2] >> 3) & 1;
	}
	types = (uint8_t) eprom->get (1, 0x3F3);
	if (types && !(types & (1 << (s[4] - 1)))) {
		for (i = 0; !(types & (1 << i)); i++)
			;
		s[4] = (uint8_t) (i + 1);
	}
	i = get_settings_crc (s, 62);
	s[62] = i & 0xFF;
	s[63] = (i >> 8) & 0xFF;
	memcpy (mem, s, 64);
	memcpy (mem + 64, s, 64);
	return true;
}

void
Mie::reset (hl::ResetType type)
{
	hl::Device *dev = this;
	Mie *mie = (Mie *) dev;

	mie->regs->clear ();
	mie->regs->put (2, 0x00, 0xFFFF);

	memset (&io, 0, sizeof (io));
	memset (&serial, 0, sizeof (serial));
	io.gpio_dir = 0xFF;
	owed = 0;
	busrq = false;
	resets = 0xFF;
	idle.valid = idle.confirmed = false;
	z80->reset ();
	if (type == hl::RESET_HARD) {
		eeprom.power_on ();
		/* A blank EEPROM: the BIOS's settings for the game, in the
		 * country of the settings */
		if (init_settings (eeprom.mem, ((Hikaru *) dev->mach)->eprom,
		                   MAX2 (hl::Config::get ()->region, 0))) {
			HL_PRINT ("mie: a blank EEPROM: the BIOS's settings for the game, a %u-player cabinet, country %u",
			          eeprom.mem[4], eeprom.mem[5]);
			save_eeprom ();
		}
		/* The settings' country, advertise sound and communication
		 * mode, for the BIOS to read at the start */
		if (hl::Config::get ()->region >= 0 &&
		    set_setting (eeprom.mem, 5, hl::Config::get ()->region)) {
			HL_PRINT ("mie: the EEPROM's country set to %d",
			          hl::Config::get ()->region);
			save_eeprom ();
		}
		if (hl::Config::get ()->attract_sound >= 0 &&
		    set_setting (eeprom.mem, 6, hl::Config::get ()->attract_sound)) {
			HL_PRINT ("mie: the EEPROM's advertise sound set to %d",
			          hl::Config::get ()->attract_sound);
			save_eeprom ();
		}
		if (hl::Config::get ()->link >= 0 &&
		    set_setting (eeprom.mem, 9, hl::Config::get ()->link)) {
			HL_PRINT ("mie: the EEPROM's communication mode set to %d",
			          hl::Config::get ()->link);
			save_eeprom ();
		}
	}
}

int
Mie::save_state (hl::State *state)
{
	int ret = state->put (&z80->regs, sizeof (z80->regs));
	ret |= state->put (&io, sizeof (io));
	ret |= state->put (&owed, sizeof (owed));
	ret |= state->put (&busrq, sizeof (busrq));
	ret |= eeprom.save_state (state);
	/* The card reader's line, for VON4 alone */
	if (card_reader)
		ret |= state->put (&serial, sizeof (serial));
	/* The line's time, with a board running its program only: the states
	 * saved without it keep loading */
	if (jvs->line_mode ())
		ret |= state->put (&line_stall, sizeof (line_stall));
	return ret;
}

int
Mie::load_state (hl::State *state)
{
	int ret = state->get (&z80->regs, sizeof (z80->regs));
	ret |= state->get (&io, sizeof (io));
	ret |= state->get (&owed, sizeof (owed));
	ret |= state->get (&busrq, sizeof (busrq));
	ret |= eeprom.load_state (state);
	if (card_reader)
		ret |= state->get (&serial, sizeof (serial));
	if (jvs->line_mode ())
		ret |= state->get (&line_stall, sizeof (line_stall));
	idle.valid = idle.confirmed = false;
	return ret;
}

const char *
Mie::debug_string ()
{
	static char text[160];

	snprintf (text, sizeof (text), "Z80 PC %04X SP %04X%s, irq %02X/%02X, 7800=%02X, JVS %s",
	          z80->regs.pc, z80->regs.sp, z80->regs.halted ? " halted" : "",
	          io.irq_enable, io.irq_pending, read (0x7800), jvs->debug_string ());
	return text;
}

bool
Mie::set_option (const char *name, int value)
{
	if (!strcmp (name, "log_ports"))
		log_ports = value != 0;
	else if (!strcmp (name, "speed") && value > 0)
		speed = value;
	else
		return false;
	return true;
}

Mie::Mie (hl::Machine *mach, hl::Buffer *ram, hl::JvsBus *jvs) :
	hl::Device (mach),
	ram (ram),
	jvs (jvs)
{
	regs = hl::Buffer::create (0x20, 0, hl::Buffer::LE32);
	HL_ASSERT (regs);
	mach->register_buffer (regs);
	ram_lo = hl::Buffer::create (0x800, 0, hl::Buffer::LE32);
	HL_ASSERT (ram_lo);
	mach->register_buffer (ram_lo);
	z80 = new Z80 (this);
	/* MIE_SPEED in the environment, for the experiments */
	speed = hl_util_get_int_option ("MIE_SPEED",
	                                (int) ((Z80_MHZ * 100 + hl::Config::sh4_clock_mhz () / 2) /
	                                       hl::Config::sh4_clock_mhz ()));
	/* The EEPROM's contents: the machine's (hikaru_load_nvram) */
	idle_skip = hl_util_get_bool_option ("MIE_IDLE_SKIP", true);
	memset (&idle, 0, sizeof (idle));
	if (hl_util_get_bool_option ("HR_Z80_PROFILE", false))
		z80_pcs.assign (0x10000, 0);
}

void
Mie::report_z80_profile ()
{
	std::vector<std::pair<uint32_t, uint32_t>> busy;
	uint64_t all = 0;
	size_t i;

	if (z80_pcs.empty ())
		return;
	for (i = 0; i < z80_pcs.size (); i++) {
		all += z80_pcs[i];
		if (z80_pcs[i])
			busy.push_back (std::make_pair (z80_pcs[i], (uint32_t) i));
	}
	std::sort (busy.begin (), busy.end (), std::greater<> ());
	HL_PRINT ("mie: the Z80's %llu T-states, the busiest addresses:",
	          (unsigned long long) all);
	for (i = 0; i < busy.size () && i < 48; i++) {
		uint16_t a = (uint16_t) busy[i].second;
		HL_PRINT ("  %04X %02X %02X %02X %02X %10u %5.2f%%", a, read (a),
		          read ((uint16_t) (a + 1)), read ((uint16_t) (a + 2)),
		          read ((uint16_t) (a + 3)), busy[i].first,
		          100.0 * busy[i].first / (double) all);
	}
	z80_pcs.clear ();
}

Mie::~Mie ()
{
	delete z80;
}

} /* namespace hikaru */
