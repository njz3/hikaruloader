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

#ifndef __Z80_H__
#define __Z80_H__

#include "hl/core.h"

/* The Zilog Z80, interpreted: all its instructions, the undocumented ones
 * too (IXH, IXL, SLL, the DDCB copies, the X and Y flags), the interrupt
 * modes 0, 1 and 2, the NMI, HALT; counted in T-states. Its memory and ports
 * are its owner's, through a Z80::Bus. */
class Z80 {
public:
	/* What the Z80 is wired to */
	class Bus {
	public:
		virtual ~Bus () {}
		virtual uint8_t	 read (uint16_t addr) = 0;
		virtual void	 write (uint16_t addr, uint8_t val) = 0;
		virtual uint8_t	 in (uint16_t port) = 0;
		virtual void	 out (uint16_t port, uint8_t val) = 0;
		/* What the interrupting device puts on the data bus when the
		 * Z80 takes its interrupt: in mode 2, the vector's low byte; in
		 * mode 0, an RST */
		virtual uint8_t	 acknowledge () { return 0xFF; }
	};

	/* The flags, in F */
	enum {
		FLAG_C	= 0x01,
		FLAG_N	= 0x02,
		FLAG_PV	= 0x04,
		FLAG_X	= 0x08,	/* undocumented: bit 3 of a result */
		FLAG_H	= 0x10,
		FLAG_Y	= 0x20,	/* undocumented: bit 5 of a result */
		FLAG_Z	= 0x40,
		FLAG_S	= 0x80
	};

	/* A register pair, the high byte first on a big-endian host */
	union Pair {
		uint16_t w;
		struct {
#ifdef HL_BIG_ENDIAN
			uint8_t h, l;
#else
			uint8_t l, h;
#endif
		} b;
	};

	explicit Z80 (Bus *bus);

	/* What the RESET pin does: PC, I and R to 0, interrupts off, mode 0 */
	void	 reset ();
	/* Runs instructions until at least cycles T-states have gone; returns
	 * how many did */
	int	 run (int cycles);
	/* One instruction, or the taking of an interrupt: its T-states */
	int	 step ();

	/* The INT pin, level-triggered: the Z80 takes the interrupt while it
	 * is held and interrupts are enabled */
	void	 set_irq (bool held) { regs.irq_line = held; }
	/* The NMI pin's edge */
	void	 nmi () { regs.nmi_pending = true; }

	/* The registers: plain data, for the save states */
	struct Registers {
		Pair	 af, bc, de, hl;
		Pair	 af2, bc2, de2, hl2;
		Pair	 ix, iy;
		uint16_t pc, sp;
		/* MEMPTR, whose high byte BIT n,(HL) shows in X and Y */
		uint16_t wz;
		uint8_t	 i, r;
		uint8_t	 im;
		bool	 iff1, iff2;
		bool	 halted;
		/* EI enables the interrupts after the next instruction */
		bool	 ei_delay;
		bool	 irq_line;
		bool	 nmi_pending;
		uint64_t cycles;
	} regs;

	/* Set by LD A,R, cleared by the owner: whoever runs the idle loops at
	 * once (see Mie::run_z80 ()) must know when R mattered */
	bool	 r_read = false;

private:
	/* The index register a DD or FD prefix puts in HL's place */
	enum Index {
		INDEX_HL,
		INDEX_IX,
		INDEX_IY
	};

	uint8_t	 read8 (uint16_t addr) { return bus->read (addr); }
	void	 write8 (uint16_t addr, uint8_t val) { bus->write (addr, val); }
	uint16_t read16 (uint16_t addr);
	void	 write16 (uint16_t addr, uint16_t val);
	uint8_t	 fetch8 () { return read8 (regs.pc++); }
	uint16_t fetch16 ();
	/* An opcode fetch: also counts in R */
	uint8_t	 fetch_opcode ();
	void	 push16 (uint16_t val);
	uint16_t pop16 ();

	/* r[i]: B C D E H L (HL) A, with H and L the index's halves under a
	 * prefix; (HL) is not read here */
	uint8_t	 get_r (unsigned i, Index index);
	void	 set_r (unsigned i, Index index, uint8_t val);
	/* rp[i]: BC DE HL SP; rp2[i]: BC DE HL AF */
	uint16_t get_rp (unsigned i, Index index);
	void	 set_rp (unsigned i, Index index, uint16_t val);
	uint16_t get_rp2 (unsigned i, Index index);
	void	 set_rp2 (unsigned i, Index index, uint16_t val);
	Pair	&index_reg (Index index);
	/* The address of (HL), or of (IX+d), (IY+d): fetches d */
	uint16_t operand_addr (Index index);
	bool	 condition (unsigned cc);

	/* The ALU */
	void	 alu (unsigned op, uint8_t val);
	uint8_t	 inc8 (uint8_t val);
	uint8_t	 dec8 (uint8_t val);
	uint16_t add16 (uint16_t a, uint16_t b);
	void	 adc16 (uint16_t val);
	void	 sbc16 (uint16_t val);
	uint8_t	 rotate (unsigned op, uint8_t val);
	void	 bit (unsigned n, uint8_t val, uint8_t xy);
	void	 daa ();
	/* S, Z, Y, X and P/V from a result */
	void	 set_szp (uint8_t val);

	int	 execute (uint8_t op, Index index);
	int	 execute_cb (Index index);
	int	 execute_ed ();
	int	 interrupt ();

	Bus	*bus;
};

#endif /* __Z80_H__ */
