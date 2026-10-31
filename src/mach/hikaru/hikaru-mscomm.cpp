/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-mscomm.c, Copyright (C) 2011, Stefano Teso (released under the GNU GPL version 2 or later).
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

#include "hl/buffer.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-mscomm.h"

namespace hikaru {

/*
 * Master/Slave Communication Box
 * ==============================
 *
 * Not sure how the comm box works exactly; does it imply any kind of IRQ?
 * Does it allow to start/reset the slave (kind of like the Saturn SMPC)?
 *
 * MMIOs
 * =====
 *
 * Master   Slave
 * -------- --------
 * 14000000 10000000	Unknown; Control? Possibly 0 turns off/resets the slave (NMI?); anything else turns it on
 * /        /
 * 14000008 10000008	Box Port 0
 * 1400000C 1000000C	Box Port 1
 * 14000010 10000010	Box Port 2
 * 14000014 10000014	Box Port 3
 * /        /
 * 1400002E 1000002E	Unknown; 3 is written in __slave_init and read from the master
 */

/* Note: access from master will have addr = 140000xx, from slave = 100000xx */

int
Mscomm::get (unsigned size, uint32_t addr, void *val)
{
	hl::Device *dev = this;
	Mscomm *comm = (Mscomm *) dev;

	set_ptr (val, size, comm->regs->get (size, addr & 0x3F));
	switch (addr & 0xFF) {
	case 0x00:
	case 0x08:
	case 0x0C:
	case 0x10:
	case 0x14:
		HL_ASSERT (size == 4);
		break;
	case 0x2E:
		HL_ASSERT (size == 2);
		break;
	default:
		return -1;
	}
	return 0;
}

int
Mscomm::put (unsigned size, uint32_t addr, uint64_t val)
{
	hl::Device *dev = this;
	Mscomm *comm = (Mscomm *) dev;
	switch (addr & 0xFF) {
	case 0x00:
	case 0x08:
	case 0x0C:
	case 0x10:
	case 0x14:
	case 0x20:
	case 0x24:
	case 0x28:
		HL_ASSERT (size == 4);
		break;
	case 0x2E:
		HL_ASSERT (size == 2);
		break;
	default:
		return -1;
	}
	comm->regs->put (size, addr & 0x3F, val);
	return 0;
}

void
Mscomm::reset (hl::ResetType type)
{
	hl::Device *dev = this;
	Mscomm *comm = (Mscomm *) dev;
	comm->regs->clear ();
}

Mscomm::Mscomm (hl::Machine *mach) :
	hl::Device (mach)
{
	regs = hl::Buffer::create (0x40, 0, hl::Buffer::LE32);
	HL_ASSERT (regs);
	mach->register_buffer (regs);
}

} /* namespace hikaru */
