/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru.c, Copyright (C) 2011, 2012, Stefano Teso.
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

/* 
 * SEGA Hikaru
 *
 * Specs
 * =====
 *
 * Taken from system16.com
 *
 * CPU			2 x Hitachi SH-4 @ 200 MHz
 * Graphic Engine	Sega Custom 3D 
 * Sound Engine		2 x ARM7 Yamaha AICA @ 45 MHz + ARM7, 64 channel ADPCM
 * Main Memory		64 MB
 * Graphic Memory	28 MB
 * Sound Memory		8 MB [per AICA board]
 * Media		ROM Board (max 352 MB)
 * Colors		24bit
 * Resolution		24 KHz 496x384, 31 KHz 640x480 
 * Shading		Flat, Linear, Phong
 * Lighting		Horizontal, Spot
 *			1024 lights per scene
 *			4 lights per polygon
 *			8 window surfaces
 * Effects		Fog, Depth Queueing, Stencil, Shadow, Motion blur
 * Others Capabilties	Bitmap Layer x 2
 *			Calendar [Note: AICA RTC]
 *			Dual Monitor (24 kHz)
 * Extensions		communication, 4 channel audio, PCI, MIDI, RS-232C 
 * Connection		Jamma Video compliant
 *
 *
 * ICs
 * ===
 *
 * According to the RAM test:
 *
 * 15,16,17S,18S = @0C000000 RAM (32 MB), Master RAM
 * 22,23,24S,25S = @16000000 RAM (32 MB), Slave RAM
 * 38 39S        = @14000000 RAM (4 MB), CMDRAM
 * 41            = @16000000 RAM (4 MB), TEXRAM Bank 0
 * 42            = @16000000 RAM (4 MB), TEXRAM Bank 1
 * 44,45S,46,47S = @1B000000 RAM (8 MB), FB
 * 33,34S,35,36S = @........ RAM (8 MB), Unknown (not tested by the RAM test)
 * 91S,92S       = @00C00000 CMOS SRAM (64 KB), Backup RAM
 * 98            = @02800000 8 MB SDRAM (main AICA board)
 *
 *
 * BIOSes
 * ======
 *
 * Currently three different bios revisions are known: 0.84, 0.92, and 0.96
 * (see the MAME driver.)
 *
 * Any IRQ controller in there pumping stuff from the various devices to
 * the master SH-4 IRL pins and Port A?
 *
 * The master SH-4 is configured for external-request DMAC (DTD). Requests
 * are sent either to the master SH-4 or the slave SH-4.
 *
 * TMU Channel 2 is configured for input capture; the DMAC is automatically
 * activeted whenever the interrupt fires. Timed DMA, way to go!
 *
 *
 * EEPROMs
 * =======
 *
 * The Hikaru hosts a few EEPROMs:
 *  - One on the MAINBD, connected to the master SH4 Port A.
 *  - One on the ROMBD, which likely holds game-specific data and protection.
 *  - One on the INPTBD, at 0800000[AC], unknown usage.
 *
 *
 * MAINBD EEPROM
 * =============
 *
 * The MAINBD EEPROM is a serial 128 x 8-bit EEPROM. It is interfaced to bits
 * 2-5 of PDTRA. The port is configured as follows:
 *
 *  PCTRA = (PCTRA & ~FF0) | 950
 *  PDTRA = PDTRA & ~3C
 *
 * See @0C00BF5C.
 *
 *  bit 2	Output		[configured as: pulled up, output]
 *  bit 3	Clock		[configured as: pulled up, output]
 *  bit 4	Chip Select	[configured as: pulled up, output]
 *  bit 5	Input		[configured as: not pulled up, input]
 *
 * See @0C0067AC, @0C0067DC.
 *
 *
 * ROMBD EEPROM, Type 1
 * ====================
 *
 * Located at 03000000, in bank_base + 0x14. It's a serial EEPROM of size
 * 128 x 8-bit entries. An 9C346. Bits (active low):
 *
 *  bit 0	Output
 *  bit 1	Clock
 *  bit 2	Chip Select
 *  bit 3	Input
 *
 * See @0C00C27E, @0C00C2AE
 *
 *
 * ROMBD EEPROM, Type 2
 * ====================
 *
 * An 76X100 secure EEPROM.
 *
 * MIE EEPROM
 * ==========
 *
 * See hikaru-mie.c
 */

#include "hl/core.h"
#include "hl/config.h"
#include "hl/games.h"
#include "hl/driveboard.h"
#include "hl/driveboard-z80.h"
#include "hl/paths.h"
#include "hl/profile.h"

#include "cpu/sh/sh4.h"

#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-memctl.h"
#include "mach/hikaru/hikaru-mscomm.h"
#include "mach/hikaru/hikaru-comm.h"
#include "mach/hikaru/hikaru-mie.h"
#include "mach/hikaru/hikaru-aica.h"
#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-renderer.h"

namespace hikaru {

/*
 * Port A
 * ======
 *
 * The master SH-4 port A is used for a variety of things. The bitmask is
 * as follows:
 *
 * [M] xxxx xxii iiee eexM
 *
 * x = Unused
 * i = IRQ causes
 * e = MAINBD EEPROM
 * M = master-to-slave communication
 *
 * Note: apparently bit M is connected to the slave NMI pin; writing a
 * specific bit pattern there requestes an NMI to the slave SH-4.
 *
 * [S] xxxx xxxx xxxx xxSx
 *
 * S = slave-to-master communication?
 *
 * Note: GPIOIC is never access by the bootrom.
 */

/* These functions get called by the SH-4 core */

static int
porta_get_m (Sh4 *ctx, uint16_t *val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Hikaru *hikaru = (Hikaru *) dev->mach;
	*val = hikaru->porta_m;
	return 0;
}

static int
porta_get_s (Sh4 *ctx, uint16_t *val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Hikaru *hikaru = (Hikaru *) dev->mach;
	*val = hikaru->porta_s;
	return 0;
}

/* XXX check port A in the core; wrong values reported (0 instead of 1)? */

static int
porta_put_m (Sh4 *ctx, uint16_t val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Hikaru *hikaru = (Hikaru *) dev->mach;
	hikaru->porta_m = val;

	/* This is a simple hack to detect three consecutive cycles of
	 * low-high-low pin; the pattern 000-111-000[0000] is there because
	 * in the SH-4:
	 *
	 * "A noise-cancellation feature is built in, and the IRL interrupt
	 *  is not detected unless the levels sampled at every bus clock
	 *  cycle remain unchanged for three consecutive cycles, so that no
	 *  transient level on the IRL pin change is detected."
	 *
	 * The additional [0000] bits can be explained with:
	 *
	 *  "[...] the NMI interrupt is not detected for a maximum of 6 bus
	 *   clock cycles after the modification."
	 *
	 * Basically it's flipping the NMI pin 0->1->0.
	 *
	 * See sections 19.2.1 and 19.2.2 of the SH-4 manual.
	 */
	/* XXX ideally this should be done automatically by the SH-4 core;
	 * but it requires a rewrite of the IRQ code based on lines */
	hikaru->porta_m_bit0_buffer = (hikaru->porta_m_bit0_buffer << 1) |
	                              (val & 1);
	if ((hikaru->porta_m_bit0_buffer & 0x1FFF) == 0x1C7F) {
		/* Send an IRQ to the slave */
		HL_CPU_LOG (ctx, " ### PORTA: sending NMI to SLAVE!");
		hikaru->sh_s->set_irq_state (SH4_IESOURCE_NMI, hl::IRQ_STATE_RAISED, hikaru->sh_m);
		hikaru->porta_m_bit0_buffer = 0;
	}
	return 0;
}

static int
porta_put_s (Sh4 *ctx, uint16_t val)
{
	hl::Device *dev = (hl::Device *) ctx;
	Hikaru *hikaru = (Hikaru *) dev->mach;
	hikaru->porta_s = val;
	hikaru->porta_s_bit1_buffer = (hikaru->porta_s_bit1_buffer << 1) |
	                              ((val >> 1) & 1);
	/* TODO */
	return 0;
}

/*
 * The Bridges' Local Bus
 * ======================
 *
 * The MEMCTLs are SEGA's 315-6154 PCI bridges: their apertures put 16 MB
 * pages of their local bus (or of PCI, from C0000000) in the SH-4s' areas;
 * see hikaru-memctl.cpp. Most of it has its devices; what is left:
 *
 *  Master	Slave
 *  01xxxxxx	1Bxxxxxx  Local bus 02xxxxxx: the MIE's Z80 bus request,
 *			  the whole 16 MB alike. Bit 8 written requests the bus,
 *			  bit 8 read is clear once granted; the Z80 stops in
 *			  between (Mie::bus_get). The games hold it to reach the
 *			  MIE's RAM (formerly semaphores answering their writes
 *			  inverted; the games' 0C010B2C takes it, 0C010B54 lets
 *			  it go). PHARRIER writes FFFD to 01000006 while holding
 *			  it (PH:@0C012756)
 *  -		1A80xxxx  Local bus 0080xxxx: the MIE's ports and RAM; the
 *			  slave's BIOS writes FFFF to its 08 at the start
 *  -		14000800  The Antarctic's PCI configuration (the 3D GPU's
 *			  command processor, 17C3:11DB, on the slave bridge's
 *			  port B): command 6, latency F8, BARs 0, 04000000,
 *			  00800000, 00080000 (AIRTRIX writes them each scene)
 *  00400000	-	  A line to a PLD (OKB_CS), purpose unknown; the GPU
 *			  keeps what is written there
 */

/* The master's: the MIE's bus request (formerly UnknownMaster) */
class LocalBusMaster : public hl::Device {
public:
	explicit LocalBusMaster (hl::Machine *mach) : hl::Device (mach) {}
	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
};

/* The slave's: the bus request, the MIE, the Antarctic's PCI configuration
 * (formerly UnknownSlave) */
class LocalBusSlave : public hl::Device {
public:
	explicit LocalBusSlave (hl::Machine *mach) : hl::Device (mach) {}
	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;

	/* The PCI configuration's 64 bytes */
	uint8_t pci[0x40];
};

int
LocalBusMaster::get (unsigned size, uint32_t addr, void *val)
{
	(void) addr;
	set_ptr (val, size, ((Mie *) ((Hikaru *) mach)->mie)->bus_get ());
	return 0;
}

int
LocalBusMaster::put (unsigned size, uint32_t addr, uint64_t val)
{
	(void) size;
	(void) addr;
	((Mie *) ((Hikaru *) mach)->mie)->bus_put (val);
	return 0;
}

void
LocalBusSlave::reset (hl::ResetType type)
{
	(void) type;
	memset (pci, 0, sizeof (pci));
	/* The vendor (SEGA) and the device */
	pci[0] = 0xDB;
	pci[1] = 0x11;
	pci[2] = 0xC3;
	pci[3] = 0x17;
}

int
LocalBusSlave::get (unsigned size, uint32_t addr, void *val)
{
	Mie *mie = (Mie *) ((Hikaru *) mach)->mie;

	if (addr >= 0x14000800 && addr <= 0x1400083F) {
		uint64_t v = 0;
		memcpy (&v, pci + (addr & 0x3F), MIN2 (size, 0x40 - (addr & 0x3F)));
		set_ptr (val, size, v);
	} else if (addr >= 0x1A800000 && addr <= 0x1A83FFFF)
		return mie->get (size, addr - 0x1A000000, val);
	else if (addr >= 0x1B000000 && addr <= 0x1BFFFFFF)
		set_ptr (val, size, mie->bus_get ());
	else
		return -1;
	return 0;
}

int
LocalBusSlave::put (unsigned size, uint32_t addr, uint64_t val)
{
	Mie *mie = (Mie *) ((Hikaru *) mach)->mie;

	if (addr >= 0x14000800 && addr <= 0x1400083F) {
		/* The IDs stay */
		if ((addr & 0x3F) >= 4)
			memcpy (pci + (addr & 0x3F), &val, MIN2 (size, 0x40 - (addr & 0x3F)));
	} else if (addr >= 0x1A800000 && addr <= 0x1A83FFFF)
		return mie->put (size, addr - 0x1A000000, val);
	else if (addr >= 0x1B000000 && addr <= 0x1BFFFFFF)
		mie->bus_put (val);
	else
		return -1;
	return 0;
}

int
LocalBusSlave::save_state (hl::State *state)
{
	return state->put (pci, sizeof (pci));
}

int
LocalBusSlave::load_state (hl::State *state)
{
	return state->get (pci, sizeof (pci));
}


/*
 * IRQs
 * ====
 *
 * The master SH-4 is configured for independent per-pin external IRQs. It
 * looks like that the IRQ sources are as follows:
 *
 *  IRL0 unused/unhandled
 *  IRL1 GPU hardware, 
 *  IRL2 memory controller DMA termination
 *  IRL3 unused/unhandled
 *
 * IRQ-related Registers
 * =====================
 *
 * The following registers are used in the IRL1 handling routine to check
 * what happened, see @0C00174C.
 *
 * PDTRA (active low):
 *
 *  0040	IRQ source is GPU
 *  0080	IRQ source is AICA/DMA
 *  0100	IRQ source is UNKNOWN [slave?], calls @0C000A30
 *  0200	Error
 *
 * For more informations on the GPU IRQs, see hikaru-gpu.c
 */

static void
hikaru_raise_irq (hl::Machine *mach, unsigned num, uint16_t porta, hl::Device *from)
{
	Hikaru *hikaru = (Hikaru *) mach;
	hikaru->sh_m->set_irq_state (num, hl::IRQ_STATE_RAISED, from);
	hikaru->porta_m &= ~porta;
}

void
Hikaru::raise_gpu_irq ()
{
	hl::Machine *mach = this;
	hikaru_raise_irq (mach, SH4_IESOURCE_IRL2, 0x40, gpu);
}

void
Hikaru::raise_aica_irq ()
{
	hl::Machine *mach = this;
	hikaru_raise_irq (mach, SH4_IESOURCE_IRL2, 0x80, aica_m);
}

void
Hikaru::raise_memctl_irq ()
{
	hl::Machine *mach = this;
	hikaru_raise_irq (mach, SH4_IESOURCE_IRL1, 0, memctl_m);
}

/* The SH-4s' cycles a line (Hikaru::cycles_per_line), of a frame's 544: the
 * [Emulation] SH4_CLOCK, 200 MHz as on the board, 57 by default (Valkyrie's
 * 50 MHz over 480 lines), as fast as the dynamic recompiler keeps up with
 * the waits the games poll that are not run forward (sh4.cpp): at 200 MHz,
 * their boots take 12 to 23 ms of the master a frame. Their timers, the
 * MIE's Z80 and the AICAs keep real time whatever it is. The GPU's command
 * processor and the MEMCTL's DMA keep the work a line they had: their own
 * clocks are unknown */
static const unsigned bus_cycles_per_line = (50 * MHZ) / (60 * 480);

static void
hikaru_run_cycles (hl::Machine *mach, int cycles)
{
	Hikaru *hikaru = (Hikaru *) mach;

	/* Each in its section of HR_PROFILE (hl/profile.h) */
	/* Run the master */
	{
		HL_PROFILE (SH4_MASTER);
		hikaru->sh_current = hikaru->sh_m;
		hikaru->sh_m->run (cycles);
	}

	/* Run the slave */
	{
		HL_PROFILE (SH4_SLAVE);
		hikaru->sh_current = hikaru->sh_s;
		hikaru->sh_s->run (cycles);
	}

	/* Run the MEMCTL and GPU */
	{
		HL_PROFILE (MEMCTL);
		hikaru->memctl_m->exec (bus_cycles_per_line);
	}
	/* XXX run the slave MEMCTL? I've never seen it used, and would
	 * like to debug it a bit before enabling it. */
	{
		HL_PROFILE (GPU_CP);
		hikaru->gpu->exec (bus_cycles_per_line);
	}

	/* The MIE's Z80, and through it the JVS bus */
	{
		HL_PROFILE (MIE);
		hikaru->mie->exec (cycles);
	}

	/* The AICAs' ARM7s and timers (their own sections) */
	hikaru->aica_m->exec (cycles);
	hikaru->aica_s->exec (cycles);
}

/* The GPU's events, in their section of HR_PROFILE */
static void
hikaru_hblank_in (Hikaru *hikaru, unsigned line)
{
	HL_PROFILE (GPU_EVENTS);
	((Gpu *) hikaru->gpu)->hblank_in (line);
}

static inline int16_t
clamp16 (int32_t x)
{
	return (int16_t) ((x > 32767) ? 32767 : (x < -32768) ? -32768 : x);
}

/* The frame's sound as the cabinet's speakers have it, into 16 bits: from
 * the main sound board, and the option board (both run the same
 * samples) */
static void
hikaru_mix_audio (Hikaru *hikaru)
{
	std::vector<int32_t> &m = ((hikaru::Aica *) hikaru->aica_m)->out;
	std::vector<int32_t> &s = ((hikaru::Aica *) hikaru->aica_s)->out;
	size_t i, n = MIN2 (m.size (), s.size ());

	for (i = 0; i + 1 < n; i += 2) {
		switch (hikaru->audio_setup) {
		case Hikaru::AUDIO_STEREO:
			hikaru->audio.push_back (clamp16 (m[i]));
			hikaru->audio.push_back (clamp16 (m[i + 1]));
			break;
		case Hikaru::AUDIO_STEREO_MIX:
			hikaru->audio.push_back (clamp16 (m[i] + s[i]));
			hikaru->audio.push_back (clamp16 (m[i + 1] + s[i + 1]));
			break;
		default:
			hikaru->audio.push_back (clamp16 (m[i]));
			hikaru->audio.push_back (clamp16 (m[i + 1]));
			hikaru->audio.push_back (clamp16 (s[i]));
			hikaru->audio.push_back (clamp16 (s[i + 1]));
			break;
		}
	}
	m.clear ();
	s.clear ();
}

/*
 * NVRAM
 * =====
 *
 * The MAINBD's 93C46 EEPROM (the BIOS's settings, then the game's) and its
 * backup RAM, a battery's (the games' records and bookkeeping), kept together
 * in nvram/<game>.nvram: blocks EEPR and BRAM (see hl/nvram.h); with a write
 * directory, in its <game>/nvram/, copied from the usual place when it has
 * none (hl::Paths). The earlier versions kept the
 * EEPROM alone, in nvram/<game>.eeprom: still read when the game has no
 * .nvram yet.
 */

#define NVRAM_EEPROM	"EEPR"
#define NVRAM_BRAM	"BRAM"
/* The EEPROM saved half a second after its last write (the BIOS and the
 * games write it a byte at a time), the backup RAM looked at every ten
 * seconds; both on the way out */
#define NVRAM_EEPROM_DELAY	30
#define NVRAM_BRAM_PERIOD	600

static void
hikaru_load_nvram (Hikaru *hikaru)
{
	hl::Machine *mach = hikaru;
	Mie *mie = (Mie *) hikaru->mie;
	const std::vector<uint8_t> *block;
	const char *name, *path;
	std::string file, read, old;

	if (!mach->game || !mach->game->name)
		return;
	/* With a write directory, its copy, made from the usual one when it
	 * has none; never replaced by it (hl::Paths). The usual one read when
	 * the copy cannot be made */
	name = mach->game->name;
	file = std::string ("nvram/") + name + ".nvram";
	hikaru->nvram_path = hl::Paths::game_write (name, file);
	if (hl::Paths::has_write_dir () && hl::Paths::seed (file, hikaru->nvram_path))
		HL_PRINT ("hikaru: '%s' copied from '%s'", hikaru->nvram_path.c_str (), file.c_str ());
	read = hl::Paths::game_read (name, file);
	path = read.c_str ();
	switch (hikaru->nvram.load (path)) {
	case hl::Nvram::LOADED:
		HL_PRINT ("hikaru: the EEPROM and the backup RAM from '%s'", path);
		break;
	case hl::Nvram::MISSING:
		/* The EEPROM alone, of the earlier versions; into the write
		 * directory's .nvram at once */
		old = hl::Paths::game_read (name, std::string ("nvram/") + name + ".eeprom");
		if (mie->eeprom.load (old.c_str ())) {
			HL_PRINT ("hikaru: the EEPROM from '%s'", old.c_str ());
			if (hl::Paths::has_write_dir () && hl::Paths::unusable_write_dir ().empty ()) {
				HL_PRINT ("hikaru: written to '%s'", hikaru->nvram_path.c_str ());
				hikaru->save_nvram ();
			}
		}
		return;
	default:
		/* Left as it is, for a later version to read */
		HL_ERROR ("hikaru: '%s' is no nvram file of this version: kept, and nothing saved", path);
		hikaru->nvram_path.clear ();
		return;
	}

	block = hikaru->nvram.get (NVRAM_EEPROM);
	if (block && block->size () == sizeof (mie->eeprom.mem))
		memcpy (mie->eeprom.mem, block->data (), block->size ());
	else if (block)
		HL_ERROR ("hikaru: '%s': an EEPROM of %u bytes, not %u: ignored",
		          path, (unsigned) block->size (), (unsigned) sizeof (mie->eeprom.mem));
	block = hikaru->nvram.get (NVRAM_BRAM);
	if (block) {
		if (block->size () != hikaru->bram->size ())
			HL_ERROR ("hikaru: '%s': a backup RAM of %u bytes, not %u",
			          path, (unsigned) block->size (), hikaru->bram->size ());
		memcpy (hikaru->bram->ptr (0), block->data (),
		        MIN2 (block->size (), (size_t) hikaru->bram->size ()));
	}
}

void
Hikaru::save_nvram ()
{
	Mie *m = (Mie *) mie;

	nvram_due = 0;
	if (nvram_path.empty () || !m || !bram)
		return;
	nvram.set (NVRAM_EEPROM, m->eeprom.mem, sizeof (m->eeprom.mem));
	nvram.set (NVRAM_BRAM, bram->ptr (0), bram->size ());
	/* The first failure of a row told (a rejected write directory fails
	 * them all) */
	static bool failing = false;
	if (nvram.save (nvram_path.c_str ()))
		failing = false;
	else if (!failing) {
		HL_ERROR ("hikaru: cannot save '%s'", nvram_path.c_str ());
		failing = true;
	}
}

static void
hikaru_keep_nvram (Hikaru *hikaru)
{
	Mie *mie = (Mie *) hikaru->mie;
	const std::vector<uint8_t> *saved;

	if (mie->eeprom.changed) {
		mie->eeprom.changed = false;
		hikaru->nvram_due = NVRAM_EEPROM_DELAY;
	} else if (hikaru->nvram_due && !--hikaru->nvram_due)
		hikaru->save_nvram ();

	if (++hikaru->nvram_frames < NVRAM_BRAM_PERIOD)
		return;
	hikaru->nvram_frames = 0;
	saved = hikaru->nvram.get (NVRAM_BRAM);
	if (!saved || saved->size () != hikaru->bram->size () ||
	    memcmp (saved->data (), hikaru->bram->ptr (0), saved->size ()))
		hikaru->save_nvram ();
}

int
Hikaru::run_frame ()
{
	hl::Machine *mach = this;
	Hikaru *hikaru = (Hikaru *) mach;
	unsigned line;

	/* XXX move the main loop into vk_machine */

	HL_LOG (" *** VBLANK-OUT %s ***", mach->debug_string ());

	for (line = 0; line < 480; line++) {
		hikaru_run_cycles (mach, hikaru->cycles_per_line);
		hikaru_hblank_in (hikaru, line);
	}

	HL_LOG (" *** VBLANK-IN  %s ***", mach->debug_string ());
	{
		HL_PROFILE (GPU_EVENTS);
		((Gpu *) hikaru->gpu)->vblank_in ();
	}

	for (line = 480; line < (480+64); line++) {
		hikaru_run_cycles (mach, hikaru->cycles_per_line);
		hikaru_hblank_in (hikaru, line);
	}

	/* this may actually be an hblank-out IRQ */
	{
		HL_PROFILE (GPU_EVENTS);
		((Gpu *) hikaru->gpu)->vblank_out ();
	}

	/* XXX An IRL2 each frame, at its end: the games' handler (BRAVEFF
	 * @0C0102BC) serves it as the GPU's, PDTRA bit 6 staying low, and so
	 * runs the callbacks of the GPU events it masks (15000084 = 72:
	 * CmdsAnalyzeEnd, the GPU 1A's) at the frame's end, not at the next
	 * vblank-in with PolygonOutEnd. PolygonOutEnd can't move to the
	 * command list's end: it's the games' only tick (the GPU 1A's vblank
	 * masked), and PHARRIER boots no further without it. Without this
	 * IRL2, BRAVEFF's frames overflow the renderer's modelviews in its
	 * demo, and PHARRIER's attract runs 3000 frames late */
	hikaru_raise_irq (mach, SH4_IESOURCE_IRL2, 0x80, hikaru->aica_m);

	{
		HL_PROFILE (COMM);
		((Comm *) hikaru->comm)->vblank ();
	}
	hikaru_mix_audio (hikaru);
	hikaru_keep_nvram (hikaru);
	return 0;
}

void
Hikaru::on_reset (hl::ResetType type)
{
	hl::Machine *mach = this;
	Hikaru *hikaru = (Hikaru *) mach;

	/* The backup RAM, a battery's, stays (hikaru_load_nvram) */
	hikaru->sh_s->set_state (hl::CPU_STATE_RUN);
	/* For the accesses the Lua shell makes before the first frame */
	hikaru->sh_current = hikaru->sh_m;

	/* Port A's are active low */
	hikaru->porta_m = 0xFFFF;
	hikaru->porta_s = 0xFFFF;

}

const char *
Hikaru::debug_string ()
{
	hl::Machine *mach = this;
	Hikaru *hikaru = (Hikaru *) mach;
	static char out[256];
	char *mstr, *sstr;
	const char *gpustr;

	mstr = strdup (hikaru->sh_m->debug_string ());
	sstr = strdup (hikaru->sh_s->debug_string ());
	gpustr = ((Gpu *) hikaru->gpu)->get_debug_str ();

	sprintf (out, "[%s %04X] [%s %04X] [%s]",
	         mstr, hikaru->porta_m,
	         sstr, hikaru->porta_s,
	         gpustr);

	free (mstr);
	free (sstr);
	return out;
}

#define SAVE(thing_) \
		ret |= state->put ((void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		ret |= state->get ((void *) &(thing_), sizeof (thing_))

int
Hikaru::on_load_state (hl::State *state)
{
	hl::Machine *mach = this;
	Hikaru *hikaru = (Hikaru *) mach;
	int ret = 0;

	LOAD (hikaru->porta_m);
	LOAD (hikaru->porta_m_bit0_buffer);
	LOAD (hikaru->porta_s);
	LOAD (hikaru->porta_s_bit1_buffer);
	LOAD (hikaru->rombd_config);

	/* Another TEXRAM: the textures uploaded again, the drawing thread's
	 * copy taken again */
	if (hikaru->renderer)
		((Renderer *) hikaru->renderer)->invalidate_texram ();

	return ret;
}

int
Hikaru::on_save_state (hl::State *state)
{
	hl::Machine *mach = this;
	Hikaru *hikaru = (Hikaru *) mach;
	int ret = 0;

	SAVE (hikaru->porta_m);
	SAVE (hikaru->porta_m_bit0_buffer);
	SAVE (hikaru->porta_s);
	SAVE (hikaru->porta_s_bit1_buffer);
	SAVE (hikaru->rombd_config);

	return ret;
}

#undef SAVE
#undef LOAD

static void
hikaru_dump (hl::Machine *mach)
{
	Hikaru *hikaru = (Hikaru *) mach;
	char *name = mach->game->name;
	const std::pair<hl::Buffer *, const char *> dumps[] = {
		{ hikaru->ram_m, "ram-m" },
		{ hikaru->ram_s, "ram-s" },
		{ hikaru->cmdram, "cmdram" },
		{ hikaru->texram[0], "texram-0" },
		{ hikaru->texram[1], "texram-1" },
		{ hikaru->fb, "fb" },
		{ hikaru->aica_ram_m, "aica-m" },
		{ hikaru->aica_ram_s, "aica-s" },
		{ hikaru->bram, "bram" },
		{ hikaru->mie_ram, "mie" }
	};

	if (!name)
		return;

	/* <game>-<memory>.bin, in the write directory's <game>/ when there */
	for (const auto &dump : dumps)
		dump.first->dump (hl::Paths::game_write (name, std::string (name) + "-" +
		                                         dump.second + ".bin").c_str ());
}

/*
 * Master SH-4 Memory Map
 * ======================
 *
 * Area 0	00000000-00200000 Boot ROM 
 *		00400000-00400003 A PLD's line (OKB_CS), unknown
 *		00800000-0083FFFF On-board Switches + MIE
 *		00C00000-00C0FFFF Backup RAM
 *		01000000-01FFFFFF The MIE's bus request (local bus 02xxxxxx)
 *		02000000-02FFFFFF Aperture-02
 *		03000000-03FFFFFF Aperture-03
 * Area 1	04000000-0400003F Memory Controller [Master]
 * Area 3	0C000000-0DFFFFFF RAM
 * Area 5	14000000-140000FF Master/Slave Communication Box
 *		14000100-143FFFFF Command RAM
 *		15000000-150FFFFF Geometry Processor
 *		16000000-16FFFFFF Aperture-16
 *		17000000-17FFFFFF Aperture-17
 * Area 6	18001000-180010FF GPU Regs
 *		1A000000-1A0FFFFF Image Generator
 *		1B000000-1B7FFFFF Frame Buffer
 */

static hl::Mmap *
setup_master_mmap (Hikaru *hikaru)
{
	hl::Mmap *mmap = new hl::Mmap (hikaru);
	int ret = 0;

	if (!mmap)
		return NULL;

	ret |= mmap->add_ram (0x0C000000, 0x0DFFFFFF, 0x01FFFFFF, 0, hikaru->ram_m, "RAM/M");
	ret |= mmap->add_rom (0x00000000, 0x001FFFFF, 0x001FFFFF, 0, hikaru->bootrom, "BOOTROM/M");
	ret |= mmap->add_dev (0x00400000, 0x00400001, 0x00000001, HL_REGION_RW | HL_REGION_SIZE_16 | HL_REGION_SIZE_32 | HL_REGION_LOG_RW, hikaru->gpu, "UNK/M");
	/* SGNASCAR reads the MIE's JVS data 32 bits at a time */
	ret |= mmap->add_dev (0x00800000, 0x0083FFFF, 0x0003FFFF, HL_REGION_RW | HL_REGION_SIZE_8 | HL_REGION_SIZE_16 | HL_REGION_SIZE_32 | HL_REGION_LOG_RW, hikaru->mie, "MIE/M");
	ret |= mmap->add_ram (0x00C00000, 0x00C0FFFF, 0x0000FFFF, 0, hikaru->bram, "BRAM/M");
	ret |= mmap->add_dev (0x01000000, 0x01FFFFFF, 0x00FFFFFF, HL_REGION_RW | HL_REGION_SIZE_16 | HL_REGION_SIZE_32 | HL_REGION_LOG_RW, hikaru->local_m, "MIEBUS/M");
	ret |= mmap->add_dev (0x02000000, 0x03FFFFFF, 0x01FFFFFF, HL_REGION_RW | HL_REGION_SIZE_ALL, hikaru->memctl_m, "APERTURE02/M");
	ret |= mmap->add_dev (0x04000000, 0x0400003F, 0x0000003F, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->memctl_m, "MEMCTL/M");
	ret |= mmap->add_dev (0x14000000, 0x1400002F, 0x0000003F, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->mscomm, "MSCOMM/M");
	ret |= mmap->add_ram (0x14000030, 0x143FFFFF, 0x003FFFFF, 0, hikaru->cmdram, "CMDRAM/M");
	ret |= mmap->add_dev (0x15000000, 0x150FFFFF, 0x000FFFFF, HL_REGION_RW | HL_REGION_SIZE_16 | HL_REGION_SIZE_32 | HL_REGION_LOG_RW, hikaru->gpu, "GPU/M");
	ret |= mmap->add_dev (0x16000000, 0x17FFFFFF, 0x01FFFFFF, HL_REGION_RW | HL_REGION_SIZE_ALL, hikaru->memctl_m, "APERTURE16/M");
	ret |= mmap->add_dev (0x18001000, 0x1800101F, 0x0000001F, HL_REGION_RW | HL_REGION_SIZE_32 | HL_REGION_LOG_RW, hikaru->gpu, "GPU/M");
	ret |= mmap->add_dev (0x1A000000, 0x1A0FFFFF, 0x000FFFFF, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->gpu, "GPU/M");
	ret |= mmap->add_ram (0x1B000000, 0x1B7FFFFF, 0x007FFFFF, 0, hikaru->fb, "FB/M");

	return ret ? NULL : mmap;
}

/*
 * Slave SH-4 Memory Map
 * =====================
 *
 * Area 0	00000000-001FFFFF Boot ROM
 * Area 1	04000000-0400003F Memory Controller [Slave]
 * Area 3	0C000000-0DFFFFFF RAM
 * Area 4	10000000-100000FF Master/Slave Communication Box
 *		10000100-103FFFFF Command RAM [Slave]
 * Area 5	14000800-1400083F The Antarctic's PCI configuration
 * Area 6	1A800000-1A83FFFF The MIE (local bus 0080xxxx)
 *		1B000000-1BFFFFFF The MIE's bus request (local bus 02xxxxxx)
 */

static hl::Mmap *
setup_slave_mmap (Hikaru *hikaru)
{
	hl::Mmap *mmap = new hl::Mmap (hikaru);
	int ret = 0;

	if (!mmap)
		return NULL;

	ret |= mmap->add_ram (0x0C000000, 0x0DFFFFFF, 0x01FFFFFF, 0, hikaru->ram_s, "RAM/S");
	ret |= mmap->add_rom (0x00000000, 0x001FFFFF, 0x001FFFFF, 0, hikaru->bootrom, "BOOTROM/S");
	ret |= mmap->add_dev (0x04000000, 0x0400003F, 0x0000003F, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->memctl_s, "MEMCTL/S");
	ret |= mmap->add_dev (0x10000000, 0x1000002F, 0x0000003F, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->mscomm, "MSCOMM/S");
	ret |= mmap->add_ram (0x10000100, 0x103FFFFF, 0x003FFFFF, HL_REGION_LOG_W, hikaru->cmdram, "CMDRAM/S");
	ret |= mmap->add_dev (0x14000800, 0x1400083F, 0x0000003F, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->local_s, "PCICFG/S");
	ret |= mmap->add_dev (0x1A800000, 0x1A83FFFF, 0x0003FFFF, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->local_s, "MIE/S");
	ret |= mmap->add_dev (0x1B000000, 0x1BFFFFFF, 0x00FFFFFF, HL_REGION_RW | HL_REGION_SIZE_ALL | HL_REGION_LOG_RW, hikaru->local_s, "MIEBUS/S");

	return ret ? NULL : mmap;
}

static int
hikaru_set_rombd_config (Hikaru *hikaru)
{
	hl::Game *game = hikaru->game;
	hikaru_rombd_config_t *config = &hikaru->rombd_config;
	uint32_t rombd_offs = 0, eprom_bank_size = 0, maskrom_bank_size = 0;
	bool has_rom = true, maskrom_is_stretched = false;
	/* A clone has its parent's ROM board */
	const char *name = game->parent[0] ? game->parent : game->name;

	/* The fields computed here are used in hikaru-memctl.c::rombd_get () */

	/* We require at least the bootrom to be loaded */
	/* XXX add a "required" field to the json file */
	if (!hikaru->bootrom)
		return -1;

	/* Determine the layout of ROMBD data within the external bus; see
	 * hikaru-memctl.c for more details. */
	if (!strcmp (name, "hikaru")) {
		has_rom = false;
		hikaru->eprom = NULL;
		hikaru->maskrom = NULL;
	} else if (!strcmp (name, "airtrix")) {
		eprom_bank_size = 4;
		maskrom_bank_size = 16;
	} else if (!strcmp (name, "braveff")) {
		eprom_bank_size = 2;
		maskrom_bank_size = 8;
	} else if (!strcmp (name, "pharrier")) {
		eprom_bank_size = 4;
		maskrom_bank_size = 16;
	} else if (!strcmp (name, "swracer")) {
		eprom_bank_size = 4;
		maskrom_bank_size = 8;
		maskrom_is_stretched = true;
	} else if (!strcmp (name, "sgnascar")) {
		//rombd_offs = 8;
		eprom_bank_size = 4;
		maskrom_bank_size = 16;
		maskrom_is_stretched = true;
	} else if (!strcmp (name, "von4")) {
		/* Same layout as pharrier, with only two EPROM pairs */
		eprom_bank_size = 4;
		maskrom_bank_size = 16;
	} else
		return -1;

	config->has_rom = has_rom;

	/* Set the ROMBD data layout within the external bus */
	if (has_rom) {
		/* There are four EPROM banks */
		config->eprom_bank[0] = rombd_offs + 0x10;
		config->eprom_bank[1] = rombd_offs + 0x13;
		/* There are sixteen MASKROM banks */
		/* XXX how does MASKROM stretching affect this? */
		config->maskrom_bank[0] = (rombd_offs == 0) ? 0x20 : 0x30;
		config->maskrom_bank[1] = (rombd_offs == 0) ? 0x2F : 0x4F;
		/* Set the remaining configuration btis */
		config->eprom_bank_size = eprom_bank_size;
		config->maskrom_bank_size = maskrom_bank_size;
		config->maskrom_is_stretched = maskrom_is_stretched;
	}
	/* The EEPROM bank is constant */
	config->eeprom_bank = rombd_offs + 0x14;

	return 0;
}

/* XXX actually make Sh4 opaque and add accessors for the registers; that's
 * going to be needed for any possible future debugger anyway. */
#define R(n_)	ctx->regs.r[n_]
#define PR	ctx->regs.pr
#define T	ctx->regs.sr.bit.t

static uint32_t
patch_braveff (hl::Cpu *cpu, uint32_t pc, uint32_t inst)
{
	Sh4 *ctx = (Sh4 *) cpu;

	switch (pc) {
	case 0x0C0407D0:
		/* XXX One case of a state dispatcher (@0C040762) calls
		 * 0C06A620 instead of 0C0693D8. Not reached in the attract,
		 * nor in the first 30 s of play: its reason is unknown */
		R(3) = 0x0C06A620;
		break;
	}
	return inst;
}

static uint32_t
patch_pharrier (hl::Cpu *cpu, uint32_t pc, uint32_t inst)
{
	if (0) {
		/* Debug switches
		 *
		 * 0C21D4A8 00000003 Enable axis on top right
		 * 0C21D4A8 00000007 CPU bar
		 * 0C21D4A8 0000000F Date
		 * 0C21D4A8 00000041 Char ball
		 * 0C21D4A8 000000F0 World bounding boxes?
		 * 0C21D4A8 00000F00 Light list + fog debug?
		 * 0C21D4A8 00020000 Disable char + world
		 * 0C21D4A8 00F00000 Switch between world and bounding boxes
		 * 0C21D4A8 F0000000 Mod light?
		 */
		cpu->write (4, 0x0C21D4A8, 0xFFFFFFFF);
		cpu->write (4, 0x0C21D4AC, 0xFFFFFFFF);
	}

	switch (pc) {
	case 0x0C01C322:
		/* The while (1) after a failed self-test (@0C103460) of the
		 * device at 8A000000: the game writes a ROM block to it, and
		 * compares what it reads back with another. Not the AICA: the
		 * banks from 80 (see hikaru-memctl.cpp), not emulated. A NOP */
		return 0x0009;
	}
	return inst;
}

static uint32_t
patch_sgnascar (hl::Cpu *cpu, uint32_t pc, uint32_t inst)
{
	Sh4 *ctx = (Sh4 *) cpu;

	switch (pc) {
	case 0x0C0130CE:
		/* Skip the "BAD IO BOARD" infinite loop: the game waits up to
		 * 600 frames for the MIE to report the JVS board, which the
		 * faked MIE never does. The Z80 does: no skip, or the game
		 * moves on before the MIE is ready, and goes another way */
		if (((hikaru::Mie *) ((Hikaru *) cpu->mach)->mie)->hack)
			R(4) = 1;
		break;
	}
	return inst;
}

/* Install game-specific patches into the master SH-4 */
static void
hikaru_install_game_patches (Hikaru *hikaru)
{
	hl::Cpu *cpu = hikaru->sh_m;
	hl::Game *game = hikaru->game;
	bool patched = true;

	if (!game)
		return;

	/* With the addresses each patch looks at (the SH-4's blocks call
	 * it there only) */
	/* AIRTRIX's warning screen was shortened by 656 frames here
	 * (0C010F9A): no longer, the screen is the game's. Gone too, now
	 * that the hardware is: BRAVEFF's sound hangs (0C05B53E,
	 * 0C05B4F6: the AICA's ARM) and 0C0D522A (T was 1 already);
	 * SGNASCAR's 0C00BC9A (the EEPROM's data, now emulated, pass) */
	if (!strcmp (game->name, "braveff"))
		cpu->install_patch (patch_braveff, { 0x0C0407D0 });
	else if (!strcmp (game->name, "pharrier"))
		cpu->install_patch (patch_pharrier, { 0x0C01C322 });
	else if (!strcmp (game->name, "sgnascar"))
		cpu->install_patch (patch_sgnascar, { 0x0C0130CE });
	else
		patched = false;

	if (patched)
		HL_PRINT ("Installed patches for '%s'", game->name);
}

Hikaru::~Hikaru ()
{
	save_nvram ();
	/* Valkyrie's dumps of all the memories on the way out (some 150 MB):
	 * with HR_EXIT_DUMPS=1 only, for the debugging */
	if (hl_util_get_bool_option ("HR_EXIT_DUMPS", false))
		hikaru_dump (this);
	/* HR_ARM_PROFILE's, while the sound RAMs are there */
	if (aica_m)
		((Aica *) aica_m)->report_arm_profile ();
	if (aica_s)
		((Aica *) aica_s)->report_arm_profile ();
	if (mie)
		((Mie *) mie)->report_z80_profile ();
}

static int
hikaru_init (Hikaru *hikaru)
{
	hl::Machine *mach = hikaru;
	hl::Game *game = mach->game;

	hikaru->local_m = new LocalBusMaster (mach);
	hikaru->local_s = new LocalBusSlave (mach);

	hikaru->ram_m		= hl::Buffer::create (32*MB, 0, hl::Buffer::LE32);
	hikaru->ram_s		= hl::Buffer::create (32*MB, 0, hl::Buffer::LE32);
	hikaru->cmdram		= hl::Buffer::create (4*MB, 0, hl::Buffer::LE32);
	hikaru->fb		= hl::Buffer::create (8*MB, 0, hl::Buffer::LE32);
	hikaru->texram[0]	= hl::Buffer::create (4*MB, 0, hl::Buffer::LE32);
	hikaru->texram[1]	= hl::Buffer::create (4*MB, 0, hl::Buffer::LE32);
	hikaru->aica_ram_m	= hl::Buffer::create (8*MB, 0, hl::Buffer::LE32);
	hikaru->aica_ram_s	= hl::Buffer::create (8*MB, 0, hl::Buffer::LE32);
	hikaru->mie_ram		= hl::Buffer::create (32*KB, 0, hl::Buffer::LE32);
	hikaru->bram		= hl::Buffer::create (64*KB, 0, hl::Buffer::LE32);

	if (!hikaru->ram_m || !hikaru->ram_s ||
	    !hikaru->cmdram || !hikaru->fb ||
	    !hikaru->texram[0] || !hikaru->texram[1] ||
	    !hikaru->aica_ram_m || !hikaru->aica_ram_s ||
	    !hikaru->bram || !hikaru->mie_ram)
		return -1;

	mach->register_buffer (hikaru->ram_m);
	mach->register_buffer (hikaru->ram_s);
	mach->register_buffer (hikaru->cmdram);
	mach->register_buffer (hikaru->fb);
	mach->register_buffer (hikaru->texram[0]);
	mach->register_buffer (hikaru->texram[1]);
	mach->register_buffer (hikaru->aica_ram_m);
	mach->register_buffer (hikaru->aica_ram_s);
	mach->register_buffer (hikaru->mie_ram);
	mach->register_buffer (hikaru->bram, true);

	if (game) {
		char *version;

		hikaru->bootrom 	= game->section_data ("bootrom");
		hikaru->eprom   	= game->section_data ("eprom");
		hikaru->maskrom 	= game->section_data ("maskrom");

		/* Patch theBOOTROM  EEPROM check. Allows games to load. */
		HL_PRINT ("patching BOOTROM");

		version = (char *) hikaru->bootrom->ptr (0xD4);
		if (!strcmp (version, "SAMURAI BootROM Version 0.84") ||
		    !strcmp (version, "SAMURAI BootROM Version 0.92"))
			hikaru->bootrom->put (2, 0x8AE, 0xE00F); /* MOV R0, 0xFFFFFFFF */
		else if (!strcmp (version, "SAMURAI BootROM Version 0.96"))
			hikaru->bootrom->put (2, 0x8E6, 0xE00F); /* MOV R0, 0xFFFFFFFF */
		else {
			HL_ERROR ("unknown BOOTROM version!");
			return -1;
		}

		if (hikaru_set_rombd_config (hikaru))
			return -1;
	} else {
		/* Create a mock bootrom */                                     
		hikaru->bootrom = hl::Buffer::create (2*MB, 0, hl::Buffer::LE32);
	}

	hikaru->memctl_m = new hikaru::Memctl (mach, true);
	hikaru->memctl_s = new hikaru::Memctl (mach, false);

	if (!hikaru->memctl_m || !hikaru->memctl_s)
		return -1;

	hikaru->mscomm = new hikaru::Mscomm (mach);
	if (!hikaru->mscomm)
		return -1;

	hikaru->comm = new hikaru::Comm (mach);

	/* The cabinet's JVS bus, its I/O board first, then the MIE driving
	 * it. NASCAR's has the drive board of a Model 3 (Sega Rally 2's) on
	 * an 837-13844 */
	hikaru->jvs = new hl::JvsBus (mach);
	if (!strncmp (game->name, "sgnascar", 8)) {
		/* Its EPROM (EPR-20512) on its Z80 when the srally2 set has it,
		 * else simulated; DRIVEBOARD_SIMULATED=1 simulates it anyway */
		hl::Buffer *drive_rom = game->section_data ("drivebd");
		hl::DriveBoardProgram *program;
		if (drive_rom && !hl_util_get_bool_option ("DRIVEBOARD_SIMULATED", false)) {
			program = new hl::DriveBoardZ80 ((const uint8_t *) drive_rom->ptr (0), drive_rom->size ());
			HL_PRINT ("drive board: EPR-20512 on its Z80");
		} else {
			program = new hl::DriveBoardSr2 ();
			HL_PRINT ("drive board: simulated%s", drive_rom ? "" : " (no EPR-20512: the srally2 set)");
		}
		hikaru->drive_board = new hl::DriveBoard (mach, program);
		hikaru->io_board = new hl::SegaIoBoard13844 (mach, hikaru->jvs, hikaru->drive_board);
	} else
		hikaru->io_board = new hl::SegaIoBoard (mach, hikaru->jvs);
	hikaru->mie = new hikaru::Mie (mach, hikaru->mie_ram, hikaru->jvs);
	if (!hikaru->mie)
		return -1;

	hikaru->renderer = new Renderer (hikaru->fb, hikaru->texram);

	hikaru->gpu = new Gpu (mach, hikaru->cmdram, hikaru->fb, hikaru->texram, (Renderer *) hikaru->renderer);

	if (!hikaru->gpu || !hikaru->renderer)
		return -1;

	((Renderer *) hikaru->renderer)->set_gpu (hikaru->gpu);

	/* The SH-4s' clock, [Emulation] SH4_CLOCK in MHz */
	hikaru->cycles_per_line = (unsigned) ((uint64_t) hl::Config::sh4_clock_mhz () * MHZ /
	                                      (60 * (480 + 64)));

	/* Each AICA its own RAM (the second one used to share the first's) */
	hikaru->aica_m = new hikaru::Aica (mach, hikaru->aica_ram_m, true);
	hikaru->aica_s = new hikaru::Aica (mach, hikaru->aica_ram_s, false);

	if (!hikaru->aica_m || !hikaru->aica_s)
		return -1;
	/* The SH-4 cycles hikaru_run_cycles () gives them in a second */
	((hikaru::Aica *) hikaru->aica_m)->cpu_hz = hikaru->cycles_per_line * (480 + 64) * 60;
	((hikaru::Aica *) hikaru->aica_s)->cpu_hz = hikaru->cycles_per_line * (480 + 64) * 60;
	/* The cabinet's speakers; without a word of them, all the sound the
	 * boards make, which a stereo sound card gets mixed */
	if (!strcmp (mach->game->audio, "stereo"))
		hikaru->audio_setup = Hikaru::AUDIO_STEREO;
	else if (!strcmp (mach->game->audio, "stereo_mix"))
		hikaru->audio_setup = Hikaru::AUDIO_STEREO_MIX;
	else {
		if (mach->game->audio[0] && strcmp (mach->game->audio, "quad"))
			HL_ERROR ("games.json: %s: unknown audio '%s', quad instead",
			          mach->game->name, mach->game->audio);
		hikaru->audio_setup = Hikaru::AUDIO_QUAD;
	}
	mach->audio_channels = (hikaru->audio_setup == Hikaru::AUDIO_QUAD) ? 4 : 2;

	hikaru->mmap_m = setup_master_mmap (hikaru);
	hikaru->mmap_s = setup_slave_mmap (hikaru);

	if (!hikaru->mmap_m || !hikaru->mmap_s)
		return -1;

	hikaru->sh_m = new Sh4 (mach, hikaru->mmap_m, true, true);
	hikaru->sh_s = new Sh4 (mach, hikaru->mmap_s, false, true);

	if (!hikaru->sh_m || !hikaru->sh_s)
		return -1;
	/* Their timers count against their clock */
	((Sh4 *) hikaru->sh_m)->cpu_hz = hikaru->cycles_per_line * (480 + 64) * 60;
	((Sh4 *) hikaru->sh_s)->cpu_hz = hikaru->cycles_per_line * (480 + 64) * 60;

	sh4_set_porta_handlers (hikaru->sh_m, porta_get_m, porta_put_m);
	sh4_set_porta_handlers (hikaru->sh_s, porta_get_s, porta_put_s);

	hikaru_install_game_patches (hikaru);

	/* The names the Lua shell knows them by */
	hikaru->sh_m->name	= "master";
	hikaru->sh_s->name	= "slave";
	hikaru->memctl_m->name	= "memctl_m";
	hikaru->memctl_s->name	= "memctl_s";
	hikaru->mscomm->name	= "mscomm";
	hikaru->comm->name	= "comm";
	hikaru->mie->name	= "mie";
	hikaru->jvs->name	= "jvs";
	hikaru->io_board->name	= "io_board";
	if (hikaru->drive_board)
		hikaru->drive_board->name = "drive_board";
	hikaru->gpu->name	= "gpu";
	hikaru->aica_m->name	= "aica_m";
	hikaru->aica_s->name	= "aica_s";
	/* Formerly unk_m and unk_s */
	hikaru->local_m->name	= "local_m";
	hikaru->local_s->name	= "local_s";

	mach->add_memory ("ram_m", hikaru->ram_m);
	mach->add_memory ("ram_s", hikaru->ram_s);
	mach->add_memory ("cmdram", hikaru->cmdram);
	mach->add_memory ("fb", hikaru->fb);
	mach->add_memory ("texram0", hikaru->texram[0]);
	mach->add_memory ("texram1", hikaru->texram[1]);
	mach->add_memory ("aica_ram_m", hikaru->aica_ram_m);
	mach->add_memory ("aica_ram_s", hikaru->aica_ram_s);
	mach->add_memory ("mie_ram", hikaru->mie_ram);
	mach->add_memory ("bram", hikaru->bram);
	mach->add_memory ("bootrom", hikaru->bootrom);
	mach->add_memory ("eprom", hikaru->eprom);
	mach->add_memory ("maskrom", hikaru->maskrom);
	mach->add_memory ("iregs_m", ((Sh4 *) hikaru->sh_m)->iregs);
	mach->add_memory ("iregs_s", ((Sh4 *) hikaru->sh_s)->iregs);

	hikaru_load_nvram (hikaru);
	return 0;
}

Hikaru::Hikaru (hl::Game *game) :
	hl::Machine (game)
{
	strcpy (name, "SEGA Hikaru");
}

Hikaru *
Hikaru::create (hl::Game *game)
{
	Hikaru *hikaru = new Hikaru (game);

	if (hikaru_init (hikaru)) {
		delete hikaru;
		return NULL;
	}
	return hikaru;
}

} /* namespace hikaru */
