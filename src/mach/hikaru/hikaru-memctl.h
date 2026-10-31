/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-memctl.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_HKMEMCTL_H__
#define __HL_HKMEMCTL_H__

#include "hl/device.h"
#include "hl/buffer.h"
#include "hl/machine.h"

namespace hikaru {

/* A memory controller (one per CPU): the apertures to the external bus,
 * the ROM board, and its DMA; see hikaru-memctl.cpp. */
class Memctl : public hl::Device {
public:
	Memctl (hl::Machine *mach, bool master);

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	int exec (int cycles) override;
	void reset (hl::ResetType type) override;

	hl::Buffer *regs;
	bool master;
};

} /* namespace hikaru */

#endif /* __HL_HKMEMCTL_H__ */
