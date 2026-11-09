/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-aica.c, Copyright (C) 2011, 2012, Stefano Teso.
 * The RTC's origin from Odin, Copyright (C) 2026, Réda Chérif-Touil.
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
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>

#include "hl/config.h"
#include "hl/profile.h"
#include "hl/state.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-aica.h"
#include "mach/hikaru/hikaru-aica-math.h"

namespace hikaru {

/*
 * Sound Boards
 * ============
 *
 * Apparently SNDBD and SNDBD2 are identical, except that the latter is
 * optional. Each has an AICA, with its 8 MB of RAM and its own ARM7, which
 * runs the sound driver the game loads into that RAM.
 *
 * As the SH-4s see one (bus 0C for the first, 0D for the second):
 *
 * 700000-701FFF AICA Channels 0-63. Each slot is 128 bytes long.
 * 702000-7027FF AICA ESF (?)
 * 702800-7028BD AICA Global (?)
 * 702C00        ARM Reset
 * 702D00        AICA IRQ L
 * 702D04        AICA IRQ R
 * 703000-7031FF COEF
 * 703200-7033FF MADRS
 * 703400-703BFF MPRO; 3BFE --> AICA DSP start
 *
 * 710000 RTC count, its high half (seconds since 1964-01-01)
 * 710004 RTC count, its low half
 * 710008 RTC write enable (bit 0)
 *
 * 800000-FFFFFF RAM
 *
 * As its ARM sees it: the RAM at 000000-7FFFFF, the registers at 800000
 * (the SH-4s' 700000), on 24 address bits: all of it again every 16 MB.
 * The drivers' loop over their 64 channels at ARM 964 starts from channel
 * FF after their init (ARM 1318), and looks for a byte with bit 7 every
 * 32 bytes, through 1000000 and back to its own code; it took a 32-bit
 * address space (and thousands of frames) without the wrap.
 *
 * The registers are 16 bits wide, one in each 32-bit word. Among the global
 * ones:
 *
 * 2890, 2894, 2898  Timers A, B, C: an 8-bit count (bits 0-7) going up once
 *                   in 2^n samples (n, bits 8-10); its overflow interrupts
 * 289C SCIEB        The ARM's interrupts enabled
 * 28A0 SCIPD        ... pending; writing bit 5 interrupts the ARM
 * 28A4 SCIRE        ... cleared by writing 1s
 * 28A8-28B0 SCILV0-2  Each interrupt's level (bits 0-7; 7 for 8 and up),
 *                   one bit of it in each register
 * 28B4 MCIEB        The SH-4's interrupts enabled
 * 28B8 MCIPD        ... pending; writing bit 5 interrupts the SH-4
 * 28BC MCIRE        ... cleared by writing 1s
 * 2C00 ARMRST       Bit 0: the ARM held in reset (from the power-on, until
 *                   the SH-4 has loaded its driver)
 * 2D00 L            The level of the FIQ the ARM takes
 * 2D04 M            Bit 0: the ARM is done with it
 *
 * The interrupt bits: 5 one CPU interrupting the other, 6-8 the timers,
 * 10 each sample. The ARM's come as its FIQ, the SH-4's as IRL2 with bit 7
 * of the master's PDTRA (see hikaru.cpp, "IRQs").
 *
 * 2800 MVOL         The master volume (bits 0-3, 3 dB a step, 0 silent);
 *                   bit 15 mono
 * 280C MSLC         The channel 2810 and 2814 show (bits 8-13)
 * 2810              Its loop end passed (bit 15, until read), its envelope's
 *                   state (13-14) and attenuation (0-12)
 * 2814 CA           Its sample played
 *
 * Each channel's registers, at 80h * its number:
 *
 * 00  KYONEX (15: keys each channel on or off, as its KYONB says), KYONB
 *     (14), LPCTL (9: loops), PCMS (7-8: 16-bit PCM, 8-bit PCM, ADPCM,
 *     ADPCM that keeps its state on looping), SA's bits 16-22
 * 04  SA's bits 0-15: the samples' address, in bytes
 * 08  LSA, 0C LEA: the loop's start and end, in samples
 * 10  The envelope's rates: D2R (11-15), D1R (6-10), AR (0-4)
 * 14  LPSLNK (14: the attack lasts until LSA), KRS (10-13), DL (5-9: the
 *     decay's level for D2R), RR (0-4)
 * 18  OCT (11-14, signed), FNS (0-9): the pitch, 2^OCT * (1 + FNS/1024)
 * 24  DISDL (8-11: the direct output's level, 3 dB a step, 0 off), DIPAN
 *     (0-4: the left attenuated 3 dB a step up to 0F, off; the right from
 *     11 to 1F)
 * 28  TL (8-15): the attenuation, 0.375 dB a step
 *
 * The ADPCM is Yamaha's, four bits a sample, the low nibble first.
 *
 * Note: this documentation comes from the following sources: MAME,
 * lxdream, nullDC. Kudos to the original authors.
 */

enum {
	REG_MVOL	= 0x2800,
	REG_MSLC	= 0x280C,
	REG_EG		= 0x2810,
	REG_CA		= 0x2814,
	REG_TIMA	= 0x2890,
	REG_TIMB	= 0x2894,
	REG_TIMC	= 0x2898,
	REG_SCIEB	= 0x289C,
	REG_SCIPD	= 0x28A0,
	REG_SCIRE	= 0x28A4,
	REG_SCILV0	= 0x28A8,
	REG_SCILV1	= 0x28AC,
	REG_SCILV2	= 0x28B0,
	REG_MCIEB	= 0x28B4,
	REG_MCIPD	= 0x28B8,
	REG_MCIRE	= 0x28BC,
	REG_ARMRST	= 0x2C00,
	REG_L		= 0x2D00,
	REG_M		= 0x2D04
};

enum {
	INT_CPU		= 1 << 5,
	INT_TIMA	= 1 << 6,
	INT_MIDI_OUT	= 1 << 9,
	INT_MIDI_IN	= 1 << 3,
	INT_SAMPLE	= 1 << 10,
	INT_ALL		= 0x7FF
};

static const uint32_t SAMPLE_HZ = 44100;
/* The ARM's cycles in a sample. Its clock is 22.5792 MHz (512 a sample), but
 * it gets one of the chip's memory cycles in eight, the channels the rest:
 * some 2.82 MHz of instructions */
static const int32_t ARM_CYCLES_PER_SAMPLE = 512 / 8;

/* The envelope: its states (as 2810 shows them), its attenuation */
enum {
	EG_ATTACK,
	EG_DECAY1,
	EG_DECAY2,
	EG_RELEASE
};
static const int32_t EG_MAX = 0x3FF << 16;

struct Aica::Slot {
	uint32_t sa, lsa, lea;
	unsigned pcms;
	bool	 lpctl, lpslnk;
	unsigned ar, d1r, d2r, rr, dl, krs;
	int	 oct;
	unsigned fns;
	/* Samples a sample, of 1 << 16 */
	uint32_t step;
	bool	 lfore;
	unsigned lfof, plfows, plfos, alfows, alfos;
	unsigned isel, imxl;
	unsigned tl, disdl, dipan;
	bool	 lpoff;
	unsigned q, flv[5], far, fd1r, fd2r, frr;
	/* The envelopes' rates as they run them (rate ()), the key's scaling
	 * in */
	unsigned ar_rate, d1r_rate, d2r_rate, rr_rate;
	unsigned far_rate, fd1r_rate, fd2r_rate, frr_rate;
};

/* A DSP step's four MPRO words apart, its COEF sign-extended */
struct Aica::DspStep {
	unsigned tra, twa, ysel, ira, iwa, ewa, shift, masa;
	bool	 twt, xsel, iwt, table, mwt, mrd, ewt, adrl, frcl, yrl;
	bool	 negb, zero, bsel, nofl, adreb, nxadr;
	int32_t	 coef;
};

/****************************************************************************
 Registers
****************************************************************************/

void
Aica::midi_send (uint8_t data)
{
	midi_in.push_back (data);
	st.scipd |= INT_MIDI_IN;
	st.mcipd |= INT_MIDI_IN;
	update_arm_irq ();
	update_main_irq ();
}

uint16_t
Aica::midi_read (bool pop)
{
	uint16_t val = 0x0800;	/* MOEMP: what is written goes at once */

	if (midi_in.empty ())
		return val | 0x0100;	/* MIEMP */
	val |= 0x0200 | midi_in.front ();	/* MIFUL */
	if (pop)
		midi_in.pop_front ();
	return val;
}

uint32_t
Aica::reg_read (unsigned size, uint32_t offs)
{
	uint32_t reg = offs & ~3u, val;

	/* The MIDI port wired (the NAOMI's servo board): the byte taken by a
	 * read of its low byte, ARM or SH-4 */
	if (reg == 0x2808 && midi_out)
		return size == 4 ? midi_read (true) :
		       ((uint32_t) midi_read ((offs & 3) == 0) >> ((offs & 3) * 8)) & ((1u << (size * 8)) - 1);
	switch (reg) {
	case REG_TIMA:
	case REG_TIMB:
	case REG_TIMC: {
		unsigned t = (reg - REG_TIMA) / 4;
		val = (st.prescale[t] << 8) | st.timer[t];
		break;
	}
	case REG_SCIEB:	val = st.scieb; break;
	case REG_SCIPD:	val = st.scipd; break;
	case REG_SCILV0:
	case REG_SCILV1:
	case REG_SCILV2: val = st.scilv[(reg - REG_SCILV0) / 4]; break;
	case REG_MCIEB:	val = st.mcieb; break;
	case REG_MCIPD:	val = st.mcipd; break;
	case REG_L:	val = st.level; break;
	case REG_EG: {
		uint32_t mslc = (uint32_t) regs->get (2, REG_MSLC);
		Voice &v = st.voice[(mslc >> 8) & 0x3F];
		uint32_t eg = v.on ? (uint32_t) (v.eg >> 16) : 0x3FF;

		val = (v.lp ? 0x8000 : 0) | ((v.on ? v.eg_state : EG_RELEASE) << 13);
		if (mslc & 0x4000)
			/* AFSET: the filter's envelope */
			val |= (uint32_t) (v.feg >> 16) & 0x1FFF;
		else
			/* 13 bits of attenuation, all set when silent */
			val |= (eg << 3) | (eg == 0x3FF ? 7 : 0);
		v.lp = false;
		break;
	}
	case REG_CA:
		val = st.voice[(regs->get (2, REG_MSLC) >> 8) & 0x3F].pos & 0xFFFF;
		break;
	default:
		if (offs + size > regs->size ())
			return 0;
		return (uint32_t) regs->get (size, offs);
	}
	/* A byte or a halfword of the register's word */
	if (size == 4)
		return val;
	return (val >> ((offs & 3) * 8)) & ((1u << (size * 8)) - 1);
}

void
Aica::reg_write (unsigned size, uint32_t offs, uint32_t val)
{
	uint32_t reg = offs & ~3u, v = val;

	/* Read only; reading 2810 would clear its flag */
	if (reg == REG_EG || reg == REG_CA)
		return;
	if (reg == REG_MSLC && (offs & 3) == 0 && midi_out)
		midi_out (midi_user, (uint8_t) val);
	if (size != 4) {
		/* Into the register's word */
		unsigned shift = (offs & 3) * 8;
		uint32_t mask = ((1u << (size * 8)) - 1) << shift;
		v = (reg_read (4, reg) & ~mask) | ((val << shift) & mask);
	}

	switch (reg) {
	case REG_TIMA:
	case REG_TIMB:
	case REG_TIMC: {
		unsigned t = (reg - REG_TIMA) / 4;
		st.timer[t] = v & 0xFF;
		st.prescale[t] = (v >> 8) & 7;
		st.timer_div[t] = 0;
		return;
	}
	case REG_SCIEB:
		st.scieb = v & INT_ALL;
		update_arm_irq ();
		return;
	case REG_SCIPD:
		if (v & INT_CPU) {
			st.scipd |= INT_CPU;
			update_arm_irq ();
		}
		return;
	case REG_SCIRE:
		st.scipd &= ~v;
		return;
	case REG_SCILV0:
	case REG_SCILV1:
	case REG_SCILV2:
		st.scilv[(reg - REG_SCILV0) / 4] = v & 0xFF;
		return;
	case REG_MCIEB:
		st.mcieb = v & INT_ALL;
		update_main_irq ();
		return;
	case REG_MCIPD:
		if (v & INT_CPU) {
			st.mcipd |= INT_CPU;
			update_main_irq ();
		}
		return;
	case REG_MCIRE:
		st.mcipd &= ~v;
		update_main_irq ();
		return;
	case REG_ARMRST:
		regs->put (4, reg, v);
		if (v & 1) {
			if (st.arm_running)
				HL_MACH_LOG (mach, "AICA/%c: ARM held in reset", master ? 'M' : 'S');
			st.arm_running = false;
		} else if (!st.arm_running) {
			HL_MACH_LOG (mach, "AICA/%c: ARM started", master ? 'M' : 'S');
			arm.reset ();
			st.arm_owed = 0;
			st.fiq = false;
			st.arm_running = true;
			st.starts++;
		}
		return;
	case REG_M:
		if (v & 1) {
			st.fiq = false;
			arm.set_fiq (false);
			update_arm_irq ();
		}
		return;
	default:
		if (offs + size <= regs->size ())
			regs->put (size, offs, val);
		/* A channel's registers, decoded again when optimized */
		if (reg < 0x2000)
			slot_dirty |= 1ull << (reg >> 7);
		/* A channel's KYONEX, not kept */
		if (reg < 0x2000 && !(reg & 0x7F) && (v & 0x8000)) {
			regs->put (2, reg, v & 0x7FFF);
			key_on_off ();
		}
		/* The DSP's COEF, MADRS and program */
		if (reg >= 0x3000 && reg < 0x3C00)
			dsp_dirty = true;
		if (reg >= 0x3400 && reg < 0x3C00)
			dsp_count_steps ();
	}
}

/****************************************************************************
 Interrupts, timers
****************************************************************************/

void
Aica::update_arm_irq ()
{
	uint32_t pending = st.scipd & st.scieb & INT_ALL;
	unsigned bit, lv;

	/* One at a time: the ARM acknowledges it at 2D04 */
	if (st.fiq || !pending)
		return;
	for (bit = 0; !(pending & (1u << bit)); bit++)
		;
	lv = (bit < 7) ? bit : 7;
	st.level = ((st.scilv[0] >> lv) & 1) |
	           (((st.scilv[1] >> lv) & 1) << 1) |
	           (((st.scilv[2] >> lv) & 1) << 2);
	st.fiq = true;
	arm.set_fiq (true);
}

bool
Aica::main_irq_level () const
{
	return (st.mcipd & st.mcieb & INT_ALL) != 0;
}

void
Aica::update_main_irq ()
{
	bool raised = (st.mcipd & st.mcieb & INT_ALL) != 0;

	if (main_irq_hook) {
		if (raised != st.main_irq)
			main_irq_hook (mach, raised);
		st.main_irq = raised;
		return;
	}
	if (raised && !st.main_irq) {
		st.main_irqs++;
		if (master)
			((Hikaru *) mach)->raise_aica_irq ();
		else
			HL_MACH_LOG (mach, "AICA/S: interrupt to the SH-4s, not wired");
	}
	st.main_irq = raised;
}

void
Aica::sample ()
{
	unsigned t;

	st.scipd |= INT_SAMPLE;
	st.mcipd |= INT_SAMPLE;
	/* The MIDI output's FIFO, always empty: what the drivers write to
	 * MOBUF (280C bits 0-7) goes nowhere */
	st.scipd |= INT_MIDI_OUT;
	st.mcipd |= INT_MIDI_OUT;
	for (t = 0; t < 3; t++) {
		if (++st.timer_div[t] < (1u << st.prescale[t]))
			continue;
		st.timer_div[t] = 0;
		st.timer[t] = (st.timer[t] + 1) & 0xFF;
		if (st.timer[t] == 0) {
			st.scipd |= INT_TIMA << t;
			st.mcipd |= INT_TIMA << t;
		}
	}
	update_arm_irq ();
	update_main_irq ();
	mix ();
	if (++rtc_samples == SAMPLE_HZ) {
		rtc_samples = 0;
		rtc_seconds++;
	}
}

/****************************************************************************
 Channels
****************************************************************************/

static inline int32_t
clamp32 (int64_t v, int32_t lo, int32_t hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : (int32_t) v;
}

uint32_t
Aica::slot_reg (unsigned ch, unsigned reg) const
{
	return (uint32_t) regs->get (2, ch * 0x80 + reg);
}

void
Aica::read_slot (unsigned ch, Slot &s) const
{
	uint32_t r00 = slot_reg (ch, 0x00), r10 = slot_reg (ch, 0x10);
	uint32_t r14 = slot_reg (ch, 0x14), r18 = slot_reg (ch, 0x18);
	uint32_t r1c = slot_reg (ch, 0x1C), r20 = slot_reg (ch, 0x20);
	uint32_t r24 = slot_reg (ch, 0x24), r28 = slot_reg (ch, 0x28);
	uint32_t r40 = slot_reg (ch, 0x40), r44 = slot_reg (ch, 0x44), base;
	unsigned i;

	s.pcms = (r00 >> 7) & 3;
	s.lpctl = (r00 >> 9) & 1;
	s.sa = ((r00 & 0x7F) << 16) | slot_reg (ch, 0x04);
	s.lsa = slot_reg (ch, 0x08);
	s.lea = slot_reg (ch, 0x0C);
	s.ar = r10 & 0x1F;
	s.d1r = (r10 >> 6) & 0x1F;
	s.d2r = (r10 >> 11) & 0x1F;
	s.rr = r14 & 0x1F;
	s.dl = (r14 >> 5) & 0x1F;
	s.krs = (r14 >> 10) & 0xF;
	s.lpslnk = (r14 >> 14) & 1;
	/* 1 + FNS/1024, in 16.16, times 2^OCT */
	s.oct = (int) ((r18 >> 11) & 0xF);
	if (s.oct & 8)
		s.oct -= 16;
	s.fns = r18 & 0x3FF;
	base = (0x400 + s.fns) << 6;
	s.step = (s.oct >= 0) ? base << s.oct : base >> -s.oct;
	s.lfore = (r1c >> 15) & 1;
	s.lfof = (r1c >> 10) & 0x1F;
	s.plfows = (r1c >> 8) & 3;
	s.plfos = (r1c >> 5) & 7;
	s.alfows = (r1c >> 3) & 3;
	s.alfos = r1c & 7;
	s.isel = r20 & 0xF;
	s.imxl = (r20 >> 4) & 0xF;
	s.disdl = (r24 >> 8) & 0xF;
	s.dipan = r24 & 0x1F;
	s.tl = (r28 >> 8) & 0xFF;
	s.lpoff = (r28 >> 5) & 1;
	s.q = r28 & 0x1F;
	for (i = 0; i < 5; i++)
		s.flv[i] = slot_reg (ch, 0x2C + 4 * i) & 0x1FFF;
	s.far = (r40 >> 8) & 0x1F;
	s.fd1r = r40 & 0x1F;
	s.fd2r = (r44 >> 8) & 0x1F;
	s.frr = r44 & 0x1F;
	s.ar_rate = rate (s.ar, s);
	s.d1r_rate = rate (s.d1r, s);
	s.d2r_rate = rate (s.d2r, s);
	s.rr_rate = rate (s.rr, s);
	s.far_rate = rate (s.far, s);
	s.fd1r_rate = rate (s.fd1r, s);
	s.fd2r_rate = rate (s.fd2r, s);
	s.frr_rate = rate (s.frr, s);
}

const Aica::Slot &
Aica::slot (unsigned ch, Slot &tmp)
{
	if (!optimized) {
		read_slot (ch, tmp);
		return tmp;
	}
	if (slot_dirty & (1ull << ch)) {
		read_slot (ch, slot_cache[ch]);
		slot_dirty &= ~(1ull << ch);
	}
	return slot_cache[ch];
}

/* A rate (0-31) as the envelopes run it (0-63): twice, plus the key's
 * scaling (KRS 0-E: the octave, FNS's high bit, twice KRS; F: none). 0 stops
 * the envelope */
unsigned
Aica::rate (unsigned r, const Slot &s) const
{
	int k;

	if (!r)
		return 0;
	k = 2 * (int) r;
	if (s.krs != 0xF)
		k += s.oct + 2 * (int) s.krs + (int) ((s.fns >> 9) & 1);
	return (unsigned) clamp32 (k, 1, 63);
}

void
Aica::key_on_off ()
{
	unsigned ch;

	/* Those whose KYONB changed: one that ended (its sample, or its
	 * release) stays keyed on, silent, until keyed off */
	for (ch = 0; ch < 64; ch++) {
		Voice &v = st.voice[ch];
		bool kyonb = (slot_reg (ch, 0x00) & 0x4000) != 0;

		if (kyonb && !v.keyed)
			key_on (ch);
		else if (!kyonb && v.keyed) {
			v.keyed = false;
			v.eg_state = EG_RELEASE;
		}
	}
}

void
Aica::key_on (unsigned ch)
{
	Voice &v = st.voice[ch];
	Slot s;

	read_slot (ch, s);
	memset (&v, 0, sizeof (v));
	st.key_ons++;
	v.keyed = true;
	v.on = true;
	v.eg_state = EG_ATTACK;
	v.eg = EG_MAX;
	v.feg = (int32_t) (s.flv[0] << 16);
	v.lfo_noise = 0x1234567u + ch;
	v.adpcm_step = 0x7F;
	v.cur = v.prev = fetch (v, s);
	if (s.lsa == 0 && s.pcms >= 2) {
		v.loop_adpcm = v.adpcm;
		v.loop_step = v.adpcm_step;
		v.loop_saved = true;
	}
}

/* The RAM is little-endian, as the hosts are: its pointer reads it as get ()
 * does */
inline uint16_t
Aica::ram_read16 (uint32_t byte) const
{
	if (optimized)
		return *(const uint16_t *) (ram_ptr + byte);
	return (uint16_t) ram->get (2, byte);
}

inline uint8_t
Aica::ram_read8 (uint32_t byte) const
{
	if (optimized)
		return ram_ptr[byte];
	return (uint8_t) ram->get (1, byte);
}

inline void
Aica::ram_write16 (uint32_t byte, uint16_t val)
{
	if (optimized)
		*(uint16_t *) (ram_ptr + byte) = val;
	else
		ram->put (2, byte, val);
}

int32_t
Aica::fetch (Voice &v, const Slot &s)
{
	unsigned byte, nib;

	switch (s.pcms) {
	case 0:
		return (int16_t) ram_read16 ((s.sa + 2 * v.pos) & 0x7FFFFE);
	case 1:
		return (int8_t) ram_read8 ((s.sa + v.pos) & 0x7FFFFF) * 256;
	default:
		byte = ram_read8 ((s.sa + v.pos / 2) & 0x7FFFFF);
		nib = (v.pos & 1) ? byte >> 4 : byte & 0xF;
		/* Yamaha's (hikaru-aica-math.h) */
		return aica_adpcm_step (v.adpcm, v.adpcm_step, nib);
	}
}

void
Aica::advance (Voice &v, const Slot &s)
{
	v.pos++;
	if (v.pos >= s.lea) {
		v.lp = true;
		if (!s.lpctl) {
			/* The end */
			v.on = false;
			v.eg = EG_MAX;
			v.eg_state = EG_RELEASE;
			v.prev = v.cur = 0;
			return;
		}
		v.pos = s.lsa;
		/* The ADPCM as it was at LSA, but for the long streams */
		if (s.pcms == 2 && v.loop_saved) {
			v.adpcm = v.loop_adpcm;
			v.adpcm_step = v.loop_step;
			v.prev = v.cur;
			v.cur = v.adpcm;
			return;
		}
	}
	v.prev = v.cur;
	v.cur = fetch (v, s);
	if (v.pos == s.lsa) {
		if (s.pcms >= 2 && !v.loop_saved) {
			v.loop_adpcm = v.adpcm;
			v.loop_step = v.adpcm_step;
			v.loop_saved = true;
		}
		if (s.lpslnk && v.eg_state == EG_ATTACK)
			v.eg_state = EG_DECAY1;
	}
}

/* The amplitude's envelope. The attack's attenuation falls as a power: fast
 * at first, as Yamaha's do; the decays and the release rise in a line, in
 * decibels */
void
Aica::envelope (Voice &v, const Slot &s)
{
	unsigned r;

	switch (v.eg_state) {
	case EG_ATTACK:
		r = s.ar_rate;
		if (r >= 62)
			v.eg = 0;
		else
			v.eg -= (int32_t) (((int64_t) v.eg + (16 << 16)) * attack_k[r] >> 24);
		if (v.eg <= 0) {
			v.eg = 0;
			/* With LPSLNK, until LSA */
			if (!s.lpslnk)
				v.eg_state = EG_DECAY1;
		}
		return;
	case EG_DECAY1:
		v.eg += decay_step[s.d1r_rate];
		if ((v.eg >> 16) >= (int32_t) (s.dl << 5))
			v.eg_state = EG_DECAY2;
		break;
	case EG_DECAY2:
		v.eg += decay_step[s.d2r_rate];
		break;
	default:
		v.eg += decay_step[s.rr_rate];
		break;
	}
	if (v.eg >= EG_MAX) {
		v.eg = EG_MAX;
		if (v.eg_state == EG_RELEASE)
			v.on = false;
	}
}

/* The filter's envelope, which follows the amplitude's states: from FLV0 to
 * FLV1 (FAR), FLV2 (FD1R), FLV3 (FD2R), then FLV4 once keyed off (FRR) */
void
Aica::filter_envelope (Voice &v, const Slot &s)
{
	static const uint8_t level[4] = { 1, 2, 3, 4 };
	unsigned st_ = v.eg_state & 3;
	unsigned r = (st_ == EG_ATTACK) ? s.far_rate : (st_ == EG_DECAY1) ? s.fd1r_rate :
	             (st_ == EG_DECAY2) ? s.fd2r_rate : s.frr_rate;
	int32_t target = (int32_t) (s.flv[level[st_]] << 16), step = feg_step[r];

	if (v.feg < target)
		v.feg = (target - v.feg > step) ? v.feg + step : target;
	else if (v.feg > target)
		v.feg = (v.feg - target > step) ? v.feg - step : target;
}

/* A two-pole low-pass: the cutoff from the filter's envelope (13 bits: five
 * of exponent, eight of mantissa, 1FFF open), Q its resonance */
int32_t
Aica::filter (Voice &v, const Slot &s, int32_t x)
{
	uint32_t c = (uint32_t) (v.feg >> 16) & 0x1FFF;
	int64_t f = (int64_t) ((((c & 0xFF) | 0x100) << 4) >> ((c >> 8) ^ 0x1F));
	int64_t q = (int64_t) s.q * 237;	/* to 0.9, of 1 << 13 */
	int32_t y = clamp32 ((f * x + (0x2000 - f + q) * v.flt_y1 - q * v.flt_y2) >> 13,
	                     -0x40000, 0x3FFFF);

	v.flt_y2 = v.flt_y1;
	v.flt_y1 = y;
	return clamp32 (y, -32768, 32767);
}

/* The LFO's step, and its waves: saw, square, triangle, noise (0-255) */
unsigned
Aica::lfo_wave (Voice &v, unsigned ws) const
{
	unsigned p = v.lfo_phase >> 24;

	switch (ws) {
	case 0:		return p;
	case 1:		return (p < 128) ? 0 : 255;
	case 2:		return (p < 128) ? p * 2 : 255 - (p - 128) * 2;
	default:	return v.lfo_noise >> 24;
	}
}

void
Aica::mix ()
{
	uint32_t mvol = (uint32_t) regs->get (2, REG_MVOL);
	bool mono = (mvol & 0x8000) != 0;
	int64_t left = 0, right = 0;
	unsigned ch, i, active = 0;
	uint64_t voices_start = hl::Profile::enabled ? hl::Profile::now () : 0;

	for (ch = 0; ch < 64; ch++) {
		Voice &v = st.voice[ch];
		Slot tmp;
		int32_t smp, amp;
		uint32_t step;
		unsigned att;

		if (!v.on)
			continue;
		active++;
		const Slot &s = slot (ch, tmp);

		/* The LFO */
		if (s.lfore)
			v.lfo_phase = 0;
		else
			v.lfo_phase += lfo_step[s.lfof];
		v.lfo_noise = v.lfo_noise * 1103515245u + 12345u;

		smp = v.prev + (int32_t) (((int64_t) (v.cur - v.prev) * v.frac) >> 16);
		if (!s.lpoff)
			smp = filter (v, s, smp);
		att = (unsigned) (v.eg >> 16) + s.tl * 4;
		if (s.alfos)
			att += alfo_att[s.alfos][lfo_wave (v, s.alfows)];
		amp = (att < ATT_STEPS) ? (int32_t) (((int64_t) smp * att_gain[att]) >> 16) : 0;

		/* The direct output */
		if (s.disdl) {
			unsigned lvl = (15 - s.disdl) * 32;
			unsigned side = ((s.dipan & 0xF) == 0xF) ? ATT_STEPS : (s.dipan & 0xF) * 32;
			unsigned att_l = lvl, att_r = lvl;

			/* 1x: to the left, the right attenuated */
			if (!mono) {
				if (s.dipan & 0x10)
					att_r += side;
				else
					att_l += side;
			}
			if (att_l < ATT_STEPS)
				left += ((int64_t) amp * att_gain[att_l]) >> 16;
			if (att_r < ATT_STEPS)
				right += ((int64_t) amp * att_gain[att_r]) >> 16;
		}
		/* To the DSP's input ISEL (20 bits) */
		if (s.imxl)
			st.dsp.mixs[s.isel] += (int32_t) (((int64_t) amp * att_gain[(15 - s.imxl) * 32]) >> 12);

		envelope (v, s);
		filter_envelope (v, s);
		step = s.step;
		if (s.plfos)
			step = (uint32_t) (((uint64_t) step * plfo_mul[s.plfos][lfo_wave (v, s.plfows)]) >> 16);
		v.frac += step;
		while (v.frac >= 0x10000 && v.on) {
			v.frac -= 0x10000;
			advance (v, s);
		}
	}
	if (hl::Profile::enabled) {
		hl::Profile::add (hl::Profile::AICA_VOICES, hl::Profile::now () - voices_start);
		hl::Profile::count (hl::Profile::AICA_VOICES, active);
	}

	/* The effects: the DSP's outputs, panned like the channels */
	if (st.dsp.steps) {
		dsp_run ();
		for (i = 0; i < 16; i++) {
			uint32_t r = (uint32_t) regs->get (2, 0x2000 + 4 * i);
			unsigned efsdl = (r >> 8) & 0xF, efpan = r & 0x1F;
			unsigned lvl, side, att_l, att_r;

			if (!efsdl || !st.dsp.efreg[i])
				continue;
			lvl = (15 - efsdl) * 32;
			side = ((efpan & 0xF) == 0xF) ? ATT_STEPS : (efpan & 0xF) * 32;
			att_l = att_r = lvl;
			if (!mono) {
				if (efpan & 0x10)
					att_r += side;
				else
					att_l += side;
			}
			if (att_l < ATT_STEPS)
				left += ((int64_t) st.dsp.efreg[i] * att_gain[att_l]) >> 16;
			if (att_r < ATT_STEPS)
				right += ((int64_t) st.dsp.efreg[i] * att_gain[att_r]) >> 16;
		}
	}
	memset (st.dsp.mixs, 0, sizeof (st.dsp.mixs));

	if (mvol & 0xF) {
		unsigned att = (15 - (mvol & 0xF)) * 32;
		left = (left * att_gain[att]) >> 16;
		right = (right * att_gain[att]) >> 16;
	} else
		left = right = 0;
	out.push_back ((int32_t) left);
	out.push_back ((int32_t) right);
}

/****************************************************************************
 DSP
****************************************************************************/

/*
 * The effects' DSP runs its 128-step program once a sample. Each step is four
 * 16-bit words (MPRO, 3400 + 10h a step):
 *
 *  0  TRA (9-15), TWT (8), TWA (1-7): TEMP read and written, from DEC
 *  1  XSEL (15), YSEL (13-14), IRA (7-12), IWT (6), IWA (1-5)
 *  2  TABLE (15), MWT (14), MRD (13), EWT (12), EWA (8-11), ADRL (7),
 *     FRCL (6), SHIFT (4-5), YRL (3), NEGB (2), ZERO (1), BSEL (0)
 *  3  NOFL (15), MASA (9-14), ADREB (8), NXADR (7)
 *
 * ACC = X * Y >> 12 + B, with X an input or a TEMP, Y FRC_REG, the step's
 * COEF (3000, 13 bits), or part of Y_REG; B ACC or a TEMP. The inputs (IRA):
 * MEMS 00-1F, the channels' sends MIXS 20-2F, the external ones 30-31. The
 * ring buffer in the sound RAM (RBP at 2804 bits 0-11, in 1K words; RBL bits
 * 13-14, 8K to 64K words) holds 16-bit floats unless NOFL; its addresses
 * come from MADRS (3200) plus DEC, which counts down each sample. EFREG are
 * the 16 outputs, to EFSDL/EFPAN (2000 + 4n).
 */

/* The ring buffer's floats: aica_dsp_pack () and aica_dsp_unpack ()
 * (hikaru-aica-math.h) */
static inline int32_t
sext (int32_t v, unsigned bits)
{
	return aica_sext (v, bits);
}

static inline uint16_t
dsp_pack (int32_t val)
{
	return aica_dsp_pack (val);
}

static inline int32_t
dsp_unpack (uint16_t val)
{
	return aica_dsp_unpack (val);
}

void
Aica::dsp_count_steps ()
{
	unsigned i, k;

	st.dsp.steps = 0;
	for (i = 0; i < 128; i++)
		for (k = 0; k < 4; k++)
			if (regs->get (2, 0x3400 + i * 16 + k * 4))
				st.dsp.steps = i + 1;
}

/* The step's four MPRO words apart, and its COEF */
void
Aica::dsp_decode (unsigned step, DspStep &p) const
{
	uint32_t w0 = (uint32_t) regs->get (2, 0x3400 + step * 16);
	uint32_t w1 = (uint32_t) regs->get (2, 0x3404 + step * 16);
	uint32_t w2 = (uint32_t) regs->get (2, 0x3408 + step * 16);
	uint32_t w3 = (uint32_t) regs->get (2, 0x340C + step * 16);

	p.tra = (w0 >> 9) & 0x7F;
	p.twt = (w0 >> 8) & 1;
	p.twa = (w0 >> 1) & 0x7F;
	p.xsel = (w1 >> 15) & 1;
	p.ysel = (w1 >> 13) & 3;
	p.ira = (w1 >> 7) & 0x3F;
	p.iwt = (w1 >> 6) & 1;
	p.iwa = (w1 >> 1) & 0x1F;
	p.table = (w2 >> 15) & 1;
	p.mwt = (w2 >> 14) & 1;
	p.mrd = (w2 >> 13) & 1;
	p.ewt = (w2 >> 12) & 1;
	p.ewa = (w2 >> 8) & 0xF;
	p.adrl = (w2 >> 7) & 1;
	p.frcl = (w2 >> 6) & 1;
	p.shift = (w2 >> 4) & 3;
	p.yrl = (w2 >> 3) & 1;
	p.negb = (w2 >> 2) & 1;
	p.zero = (w2 >> 1) & 1;
	p.bsel = w2 & 1;
	p.nofl = (w3 >> 15) & 1;
	p.masa = (w3 >> 9) & 0x3F;
	p.adreb = (w3 >> 8) & 1;
	p.nxadr = (w3 >> 7) & 1;
	p.coef = sext ((int32_t) regs->get (2, 0x3000 + step * 4) >> 3, 13);
}

void
Aica::dsp_run ()
{
	Aica::State::Dsp &d = st.dsp;
	uint32_t rb = (uint32_t) regs->get (2, 0x2804);
	uint32_t rb_mask = (0x2000u << ((rb >> 13) & 3)) - 1, rb_base = (rb & 0xFFF) << 10;
	unsigned step, i;
	DspStep tmp;
	HL_PROFILE (AICA_DSP);

	hl::Profile::count (hl::Profile::AICA_DSP, d.steps);
	/* Optimized: the program decoded once written */
	if (optimized && dsp_dirty) {
		for (step = 0; step < 128; step++)
			dsp_decode (step, dsp_cache[step]);
		for (i = 0; i < 64; i++)
			madrs_cache[i] = (uint16_t) regs->get (2, 0x3200 + i * 4);
		dsp_dirty = false;
	}

	memset (d.efreg, 0, sizeof (d.efreg));
	for (step = 0; step < d.steps; step++) {
		const DspStep &p = optimized ? dsp_cache[step] : (dsp_decode (step, tmp), tmp);
		int32_t inputs, b, x, y, shifted;

		/* The input */
		if (p.ira < 0x20)
			inputs = d.mems[p.ira];
		else if (p.ira < 0x30)
			inputs = clamp32 (d.mixs[p.ira - 0x20], -0x80000, 0x7FFFF) << 4;
		else
			inputs = 0;	/* EXTS: no CD here */

		/* IWT: the memory read two steps before */
		if (p.iwt) {
			d.mems[p.iwa] = d.memval;
			if (p.ira == p.iwa)
				inputs = d.memval;
		}

		/* B */
		if (p.zero)
			b = 0;
		else {
			b = p.bsel ? d.acc : d.temp[(p.tra + d.dec) & 0x7F];
			if (p.negb)
				b = -b;
		}

		/* X, Y */
		x = p.xsel ? inputs : d.temp[(p.tra + d.dec) & 0x7F];
		switch (p.ysel) {
		case 0:	y = d.frc_reg; break;
		case 1:	y = p.coef; break;
		case 2:	y = sext ((d.y_reg >> 11) & 0x1FFF, 13); break;
		default: y = (d.y_reg >> 4) & 0xFFF; break;
		}
		if (p.yrl)
			d.y_reg = inputs;

		/* The shifter, on the last ACC */
		switch (p.shift) {
		case 0:	shifted = clamp32 (d.acc, -0x800000, 0x7FFFFF); break;
		case 1:	shifted = clamp32 ((int64_t) d.acc * 2, -0x800000, 0x7FFFFF); break;
		case 2:	shifted = sext (d.acc * 2, 24); break;
		default: shifted = sext (d.acc, 24); break;
		}

		d.acc = sext ((int32_t) (((int64_t) x * y) >> 12) + b, 26);

		if (p.twt)
			d.temp[(p.twa + d.dec) & 0x7F] = shifted;
		if (p.frcl)
			d.frc_reg = (p.shift == 3) ? (shifted & 0xFFF) : ((shifted >> 11) & 0x1FFF);

		/* The ring buffer, on the odd steps */
		if ((step & 1) && (p.mrd || p.mwt)) {
			uint32_t addr = optimized ? madrs_cache[p.masa] :
			                (uint32_t) regs->get (2, 0x3200 + p.masa * 4);
			uint32_t byte;

			if (!p.table)
				addr += d.dec;
			if (p.adreb)
				addr += d.adrs_reg & 0xFFF;
			if (p.nxadr)
				addr++;
			addr = p.table ? (addr & 0xFFFF) : (addr & rb_mask);
			byte = ((addr + rb_base) * 2) & 0x7FFFFE;
			if (p.mrd) {
				uint16_t m = ram_read16 (byte);
				d.memval = p.nofl ? sext ((int32_t) m << 8, 24) : dsp_unpack (m);
			}
			if (p.mwt)
				ram_write16 (byte, p.nofl ? (uint16_t) (shifted >> 8) : dsp_pack (shifted));
		}

		if (p.adrl)
			d.adrs_reg = (p.shift == 3) ? ((shifted >> 12) & 0xFFF) : (inputs >> 16);
		if (p.ewt)
			d.efreg[p.ewa] = clamp32 ((int64_t) d.efreg[p.ewa] + (shifted >> 8), -32768, 32767);
	}
	d.dec--;
}

int
Aica::exec (int cycles)
{
	HL_PROFILE (AICA);

	/* The settings', from this exec () on: the caches stay right either
	 * way, kept dirty by the writes */
	optimized = hl::Config::get ()->sound_optimized;
	/* Its drivers' scan loops at once too, as exact */
	arm.fast_loops = optimized;
	st.units += (int64_t) cycles * SAMPLE_HZ;
	while (st.units >= cpu_hz) {
		st.units -= cpu_hz;
		if (st.arm_running) {
			HL_PROFILE (AICA_ARM);
			hl::Profile::count (hl::Profile::AICA_ARM, ARM_CYCLES_PER_SAMPLE);
			st.arm_owed += ARM_CYCLES_PER_SAMPLE;
			if (st.arm_owed > 0 && arm_pcs.empty ())
				st.arm_owed -= arm.run (st.arm_owed);
			else if (st.arm_owed > 0) {
				/* run (), its instructions counted */
				int done = 0;

				while (done < st.arm_owed) {
					arm_pcs[(arm.pc () & 0x7FFFFF) >> 2]++;
					done += arm.step ();
				}
				st.arm_owed -= done;
			}
		}
		sample ();
	}
	return cycles;
}

/****************************************************************************
 The ARM's bus
****************************************************************************/

uint32_t
Aica::ArmBus::read32 (uint32_t addr)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		return (uint32_t) aica->ram->get (4, addr & 0x7FFFFC);
	if (addr < 0x810000)
		return aica->reg_read (4, addr & 0xFFFC);
	return 0;
}

uint16_t
Aica::ArmBus::read16 (uint32_t addr)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		return (uint16_t) aica->ram->get (2, addr & 0x7FFFFE);
	if (addr < 0x810000)
		return (uint16_t) aica->reg_read (2, addr & 0xFFFE);
	return 0;
}

uint8_t
Aica::ArmBus::read8 (uint32_t addr)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		return (uint8_t) aica->ram->get (1, addr & 0x7FFFFF);
	if (addr < 0x810000)
		return (uint8_t) aica->reg_read (1, addr & 0xFFFF);
	return 0;
}

void
Aica::ArmBus::write32 (uint32_t addr, uint32_t val)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		aica->ram->put (4, addr & 0x7FFFFC, val);
	else if (addr < 0x810000)
		aica->reg_write (4, addr & 0xFFFC, val);
}

void
Aica::ArmBus::write16 (uint32_t addr, uint16_t val)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		aica->ram->put (2, addr & 0x7FFFFE, val);
	else if (addr < 0x810000)
		aica->reg_write (2, addr & 0xFFFE, val);
}

void
Aica::ArmBus::write8 (uint32_t addr, uint8_t val)
{
	addr &= 0xFFFFFF;
	if (addr < 0x800000)
		aica->ram->put (1, addr & 0x7FFFFF, val);
	else if (addr < 0x810000)
		aica->reg_write (1, addr & 0xFFFF, val);
}

/****************************************************************************
 The SH-4s' side
****************************************************************************/

int
Aica::get (unsigned size, uint32_t addr, void *val)
{
	hl::Device *device = this;
	Aica *aica = (Aica *) device;
	uint32_t *val32 = (uint32_t *) val;
	uint32_t offs = addr & 0xFFFFFF;

	HL_MACH_LOG (device->mach, "AICA/%c R%u @%08X",
	             aica->master ? 'M' : 'S', 8*size, offs);

	switch (offs) {
	case 0x710000:
		/* RTC: the count's high half, then its low half */
		HL_ASSERT (size == 4);
		*val32 = aica->rtc_seconds >> 16;
		break;
	case 0x710004:
		HL_ASSERT (size == 4);
		*val32 = aica->rtc_seconds & 0xFFFF;
		break;
	case 0x710008:
		HL_ASSERT (size == 4);
		*val32 = 0;
		break;
	default:
		if (IN_RANGE (offs, 0x700000, 0x70FFFF)) {
			set_ptr (val, size, reg_read (size, offs & 0xFFFF));
			break;
		}
		if (IN_RANGE (offs, 0x800000, 0xFFFFFF)) {
			/* RAM */
			set_ptr (val, size, aica->ram->get (size, addr & 0x7FFFFF));
			break;
		}
		HL_MACH_ERROR (device->mach, "AICA unhandled R%u %08X", size*8, addr);
		return -1;
	}
	return 0;
}

int
Aica::put (unsigned size, uint32_t addr, uint64_t val)
{
	hl::Device *device = this;
	Aica *aica = (Aica *) device;
	uint32_t offs = addr & 0xFFFFFF;

	HL_MACH_LOG (device->mach, "AICA/%c W%u @%08X = %lX",
	             aica->master ? 'M' : 'S', 8*size, offs, val);

	switch (offs) {
	case 0x710000:
		/* RTC, as the BIOS's CLOCK SETTING sets it: the write enable
		 * (710008 bit 0), the high half, which takes the enable back,
		 * then the low half, which starts the second again */
		HL_ASSERT (size == 4);
		if (aica->rtc_we) {
			aica->rtc_seconds = (aica->rtc_seconds & 0xFFFF) | (((uint32_t) val & 0xFFFF) << 16);
			aica->rtc_we = false;
			aica->rtc_set = true;
		}
		break;
	case 0x710004:
		HL_ASSERT (size == 4);
		if (aica->rtc_we) {
			aica->rtc_seconds = (aica->rtc_seconds & 0xFFFF0000) | ((uint32_t) val & 0xFFFF);
			aica->rtc_samples = 0;
			aica->rtc_set = true;
		}
		break;
	case 0x710008:
		HL_ASSERT (size == 4);
		aica->rtc_we = (val & 1) != 0;
		break;
	default:
		if (IN_RANGE (offs, 0x700000, 0x70FFFF)) {
			reg_write (size, offs & 0xFFFF, (uint32_t) val);
			break;
		}
		if (IN_RANGE (offs, 0x800000, 0xFFFFFF)) {
			/* RAM */
			aica->ram->put (size, addr & 0x7FFFFF, val);
			break;
		}
		HL_MACH_ERROR (device->mach, "AICA unhandled W%u %08X = %lX", size*8, addr, val);
		return -1;
	}
	return 0;
}

/****************************************************************************
 The RTC
****************************************************************************/

/* The count of a board whose clock was set to the PC's local time: days
 * from 1964-01-01, four years of 1461 days from a leap one (the BOOTROM's
 * date routine, @0C008464, right until 2100), then the time of day; as
 * Odin's author, Réda Chérif-Touil, found it. The clock stood still at
 * 5BFC8900 before */
uint32_t
Aica::host_clock ()
{
	const char *fixed = getenv ("HR_CLOCK");
	time_t now;
	struct tm local;
	uint32_t years, days;

	if (fixed && *fixed)
		return (uint32_t) strtoul (fixed, NULL, 0);
	/* The agent's runs repeat, whatever the day */
	if (hl::Log::stdout_reserved)
		return 0x5BFC8900;
	now = time (NULL);
#ifdef _WIN32
	localtime_s (&local, &now);
#else
	localtime_r (&now, &local);
#endif
	years = local.tm_year > 64 ? (uint32_t) (local.tm_year - 64) : 0;
	days = years * 365 + (years + 3) / 4 + (uint32_t) local.tm_yday;
	return days * 86400 + (uint32_t) (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
}

void
Aica::set_clock (int32_t ahead)
{
	rtc_ahead = ahead;
	rtc_seconds = host_clock () + (uint32_t) ahead;
	rtc_samples = 0;
}

int32_t
Aica::clock_ahead () const
{
	return (int32_t) (rtc_seconds - host_clock ());
}

/****************************************************************************
 The device
****************************************************************************/

void
Aica::reset (hl::ResetType type)
{
	hl::Device *dev = this;
	Aica *aica = (Aica *) dev;
	uint32_t x, a;

	(void) type;
	aica->rtc_we = false;
	aica->set_clock (aica->rtc_ahead);
	reset_pin ();

	/* The SDRAM holds anything but zeros at power-on, and the drivers
	 * count on it: their loop over the channels (ARM 964) starts from
	 * channel FF after their init, and stops at the first byte with bit
	 * 7 every 32 bytes. Over zeros it went round the 16 MB for seconds,
	 * and missed the game's first request: BRAVEFF's first sound bank
	 * (frame 22) was never loaded, and nothing played. The same
	 * pseudo-random content each time (xorshift) */
	x = master ? 0x12345678u : 0x9ABCDEF0u;
	for (a = 0; a < ram->size (); a += 4) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		ram->put (4, a, x);
	}
}

void
Aica::reset_pin ()
{
	/* The ARM held in reset until the SH-4 lets it go */
	memset (&st, 0, sizeof (st));
	regs->clear ();
	regs->put (4, REG_ARMRST, 1);
	arm.reset ();
	out.clear ();
	slot_dirty = ~0ull;
	dsp_dirty = true;
}

int
Aica::save_state (hl::State *state)
{
	int ret = state->put (&st, sizeof (st));
	ret |= state->put (&arm.regs, sizeof (arm.regs));
	return ret;
}

int
Aica::load_state (hl::State *state)
{
	int ret = state->get (&st, sizeof (st));
	ret |= state->get (&arm.regs, sizeof (arm.regs));
	/* The registers came back with the machine's buffers */
	slot_dirty = ~0ull;
	dsp_dirty = true;
	return ret;
}

const char *
Aica::debug_string ()
{
	static char text[256];
	unsigned ch, on = 0;

	for (ch = 0; ch < 64; ch++)
		on += st.voice[ch].on;
	snprintf (text, sizeof (text), "ARM %s PC %08X (started %u times), SCIPD %03X/%03X, MCIPD %03X/%03X, %u channels on (%u keyed on), MVOL %X, %u interrupts to the SH-4",
	          st.arm_running ? "running" : "in reset", arm.pc (), st.starts,
	          st.scipd, st.scieb, st.mcipd, st.mcieb, on, st.key_ons,
	          (unsigned) regs->get (2, REG_MVOL) & 0xF, st.main_irqs);
	return text;
}

Aica::Aica (hl::Machine *mach, hl::Buffer *ram, bool master) :
	hl::Device (mach),
	ram (ram),
	master (master),
	cpu_hz (1736 * 544 * 60),
	arm_bus (this),
	arm (&arm_bus)
{
	unsigned i;

	/* All of it: the ARM's 000000-7FFFFF */
	arm.set_ram ((uint8_t *) ram->ptr (0), ram->size ());
	regs = hl::Buffer::create (0x3C00, 0, hl::Buffer::LE32);
	HL_ASSERT (regs);
	mach->register_buffer (regs);
	memset (&st, 0, sizeof (st));
	optimized = true;
	ram_ptr = (uint8_t *) ram->ptr (0);
	slot_cache = new Slot[64] ();
	slot_dirty = ~0ull;
	dsp_cache = new DspStep[128] ();
	memset (madrs_cache, 0, sizeof (madrs_cache));
	dsp_dirty = true;

	/* The LFO's frequencies (Hz), pitch depths (cents) and amplitude
	 * depths (dB) */
	static const double lfo_hz[32] = {
		0.17, 0.19, 0.23, 0.27, 0.34, 0.39, 0.45, 0.55, 0.68, 0.78, 0.92,
		1.10, 1.39, 1.60, 1.87, 2.27, 2.87, 3.31, 3.92, 4.79, 6.15, 7.18,
		8.60, 10.8, 14.4, 17.2, 21.5, 28.7, 43.1, 57.4, 86.1, 172.3
	};
	static const double plfo_cents[8] = { 0, 3.6, 7.2, 14.4, 28.8, 57.6, 115.2, 230.4 };
	static const double alfo_db[8] = { 0, 0.4, 0.8, 1.5, 3, 6, 12, 24 };
	unsigned w;

	for (i = 0; i < ATT_STEPS; i++)
		att_gain[i] = (int32_t) (pow (2.0, -(double) i / 64.0) * 65536.0 + 0.5);
	/* From the slowest rate (2): an attack of 8.1 s, a decay of 118 s
	 * to silence; twice faster each 4 rates. The attack's attenuation,
	 * plus 16, shrinks by a ratio: by 1039/16 over its time */
	for (i = 0; i < 64; i++) {
		double k = pow (2.0, ((double) i - 1.0) / 4.0);
		double attack_s = 8.1 / k, decay_s = 118.2 / k;

		attack_k[i] = (i < 2) ? 0 : (int32_t) (log (1039.0 / 16.0) / (attack_s * SAMPLE_HZ) * 16777216.0);
		decay_step[i] = (i < 2) ? 0 : (int32_t) (EG_MAX / (decay_s * SAMPLE_HZ));
		feg_step[i] = (i < 2) ? 0 : (int32_t) ((double) (0x1FFF << 16) / (decay_s * SAMPLE_HZ));
	}
	for (i = 0; i < 32; i++)
		lfo_step[i] = (uint32_t) (lfo_hz[i] / SAMPLE_HZ * 4294967296.0);
	for (i = 0; i < 8; i++)
		for (w = 0; w < 256; w++) {
			plfo_mul[i][w] = (int32_t) (pow (2.0, plfo_cents[i] * ((double) w - 128.0) / 128.0 / 1200.0) * 65536.0 + 0.5);
			alfo_att[i][w] = (uint16_t) (alfo_db[i] * w / 255.0 / 0.09375 + 0.5);
		}

	if (hl_util_get_bool_option ("HR_ARM_PROFILE", false))
		arm_pcs.assign (ram->size () / 4, 0);
}

Aica::~Aica ()
{
	delete[] slot_cache;
	delete[] dsp_cache;
}

/* HR_ARM_PROFILE: the busiest addresses, with their instruction, for the
 * idle loops; once */
void
Aica::report_arm_profile ()
{
	std::vector<std::pair<uint32_t, uint32_t>> busy;
	uint64_t all = 0;
	size_t i;

	if (arm_pcs.empty ())
		return;
	for (i = 0; i < arm_pcs.size (); i++) {
		all += arm_pcs[i];
		if (arm_pcs[i])
			busy.push_back (std::make_pair (arm_pcs[i], (uint32_t) i * 4));
	}
	std::sort (busy.begin (), busy.end (), std::greater<> ());
	HL_PRINT ("aica %s: the ARM's %llu instructions, the busiest addresses:",
	          master ? "m" : "s", (unsigned long long) all);
	for (i = 0; i < busy.size () && i < 48; i++)
		HL_PRINT ("  %06X %08X %10u %5.2f%%", busy[i].second,
		          (unsigned) ram->get (4, busy[i].second), busy[i].first,
		          100.0 * busy[i].first / (double) all);
	arm_pcs.clear ();
}

} /* namespace hikaru */
