/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/mach/hikaru/hikaru-aica.h, Copyright (C) 2011, 2012, Stefano Teso.
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

#ifndef __HL_HIKARU_AICA_H__
#define __HL_HIKARU_AICA_H__

#include <vector>

#include "hl/device.h"
#include "hl/buffer.h"
#include "hl/machine.h"
#include "cpu/arm/arm7.h"

namespace hikaru {

/* An AICA sound chip, as the master or the slave CPU sees it: its
 * registers, its RTC and its RAM, and its own ARM7, which runs the sound
 * driver from that RAM; see hikaru-aica.cpp. */
class Aica : public hl::Device {
public:
	Aica (hl::Machine *mach, hl::Buffer *ram, bool master);
	~Aica ();

	int get (unsigned size, uint32_t addr, void *val) override;
	int put (unsigned size, uint32_t addr, uint64_t val) override;
	void reset (hl::ResetType type) override;
	/* cycles of the SH-4s, at cpu_hz: the samples they make, the ARM's
	 * cycles between them */
	int exec (int cycles) override;
	int save_state (hl::State *state) override;
	int load_state (hl::State *state) override;
	const char *debug_string () override;
	/* The SH-4's interrupt line: MCIPD & MCIEB */
	bool	 main_irq_level () const;

	hl::Buffer *ram;
	hl::Buffer *regs;
	uint32_t rtc[4];
	bool master;
	/* The SH-4 cycles exec () counts in a second */
	uint32_t cpu_hz;
	/* The sound of the samples run since the machine took it: left and
	 * right, 16 bits and some headroom */
	std::vector<int32_t> out;
	/* HR_ARM_PROFILE=1: the ARM's instructions counted by address (a word
	 * each); empty without. The busiest printed, with their instruction,
	 * by report_arm_profile (), before the RAM goes */
	std::vector<uint32_t> arm_pcs;
	void	 report_arm_profile ();

private:
	/* The ARM's view of the chip: the RAM from 0, the registers at
	 * 800000 */
	class ArmBus : public Arm7::Bus {
	public:
		explicit ArmBus (Aica *aica) : aica (aica) {}
		uint32_t read32 (uint32_t addr) override;
		uint16_t read16 (uint32_t addr) override;
		uint8_t	 read8 (uint32_t addr) override;
		void	 write32 (uint32_t addr, uint32_t val) override;
		void	 write16 (uint32_t addr, uint16_t val) override;
		void	 write8 (uint32_t addr, uint8_t val) override;
	private:
		Aica *aica;
	};

	/* The registers, as both CPUs see them: offs within the 64 KB of
	 * registers (the SH-4's 700000, the ARM's 800000) */
	uint32_t reg_read (unsigned size, uint32_t offs);
	void	 reg_write (unsigned size, uint32_t offs, uint32_t val);

	/* One sample's time (44.1 kHz): the timers, the interrupts, the
	 * channels' sound */
	void	 sample ();
	/* The ARM's FIQ, the SH-4's interrupt, from what is pending */
	void	 update_arm_irq ();
	void	 update_main_irq ();

	/* A channel's playback, beside its registers */
	struct Voice {
		bool	 keyed;		/* KYONB, as the last KYONEX took it */
		bool	 on;		/* sounding: keyed on, not ended */
		bool	 lp;		/* past its loop end, until 2810 is read */
		bool	 loop_saved;	/* the ADPCM decoder's state at LSA */
		uint8_t	 eg_state;	/* EG_ATTACK... */
		int32_t	 eg;		/* the attenuation, 0 to 3FF, << 16 */
		uint32_t pos;		/* the sample played, from SA */
		uint32_t frac;		/* towards the next, of 1 << 16 */
		int32_t	 prev, cur;	/* the samples at pos - 1 and pos */
		int32_t	 adpcm, adpcm_step;
		int32_t	 loop_adpcm, loop_step;
		uint32_t lfo_phase;	/* of 1 << 32 */
		uint32_t lfo_noise;
		int32_t	 feg;		/* the filter's cutoff, 13 bits << 16 */
		int32_t	 flt_y1, flt_y2;	/* the filter's last outputs */
	};
	/* A channel's registers, as the playback reads them */
	struct Slot;

	uint32_t slot_reg (unsigned ch, unsigned reg) const;
	void	 read_slot (unsigned ch, Slot &s) const;
	/* The channel's Slot: the one kept when optimized (decoded again once
	 * its registers are written), else decoded into tmp */
	const Slot &slot (unsigned ch, Slot &tmp);
	/* KYONEX written: each channel keyed on or off, as its KYONB says */
	void	 key_on_off ();
	void	 key_on (unsigned ch);
	/* The channel's sample at pos, through the ADPCM decoder */
	int32_t	 fetch (Voice &v, const Slot &s);
	/* To the next sample, past the loop end to LSA, or the end */
	void	 advance (Voice &v, const Slot &s);
	void	 envelope (Voice &v, const Slot &s);
	/* A rate as the envelopes run it, the key's scaling added */
	unsigned rate (unsigned r, const Slot &s) const;
	void	 filter_envelope (Voice &v, const Slot &s);
	int32_t	 filter (Voice &v, const Slot &s, int32_t x);
	unsigned lfo_wave (Voice &v, unsigned ws) const;
	/* The 64 channels mixed into out, through the DSP's effects */
	void	 mix ();
	void	 dsp_count_steps ();
	/* A step of the DSP's program, its fields apart */
	struct DspStep;
	void	 dsp_decode (unsigned step, DspStep &p) const;
	void	 dsp_run ();
	/* The sound RAM's 16 bits at byte (even), through the RAM's pointer
	 * when optimized */
	uint16_t ram_read16 (uint32_t byte) const;
	uint8_t	 ram_read8 (uint32_t byte) const;
	void	 ram_write16 (uint32_t byte, uint16_t val);

	ArmBus	 arm_bus;
	Arm7	 arm;

	/* [Sound] SOUND_CORE, at each exec (): OPTIMIZED keeps the channels'
	 * registers decoded, and the DSP's program, until they are written,
	 * and reads the RAM through its pointer; INTERPRETED decodes them at
	 * each sample, as the reference. The same sound either way */
	bool	 optimized;
	uint8_t	*ram_ptr;
	Slot	*slot_cache;		/* 64 */
	uint64_t slot_dirty;		/* a bit a channel to decode again */
	DspStep	*dsp_cache;		/* 128 */
	uint16_t madrs_cache[64];
	bool	 dsp_dirty;

	/* The gain of an attenuation (in 3/32 dB, 64 to halve), 1 << 16
	 * for none; the envelopes' steps per sample, at each rate (the
	 * attack's a ratio, of 1 << 24); the LFOs' phase steps, their pitch
	 * factors (of 1 << 16) and attenuations at each depth and wave
	 * value */
	static const unsigned ATT_STEPS = 1024;
	int32_t	 att_gain[ATT_STEPS];
	int32_t	 attack_k[64];
	int32_t	 decay_step[64];
	int32_t	 feg_step[64];
	uint32_t lfo_step[32];
	int32_t	 plfo_mul[8][256];
	uint16_t alfo_att[8][256];

	/* The chip's state beside its registers: plain data, for the save
	 * states */
	struct State {
		uint32_t scieb, scipd;		/* the ARM's interrupts */
		uint32_t mcieb, mcipd;		/* the SH-4's */
		uint32_t scilv[3];		/* each interrupt's level, 3 bits */
		uint32_t timer[3];		/* A, B, C: 8-bit counters */
		uint32_t prescale[3];		/* log2 of their sample counts */
		uint32_t timer_div[3];
		bool	 arm_running;		/* ARMRST bit 0 clear */
		bool	 fiq;			/* until the ARM acknowledges it */
		bool	 main_irq;		/* raised to the SH-4 */
		uint32_t level;			/* the FIQ's, read at 2D00 */
		int64_t	 units;			/* towards the next sample */
		int32_t	 arm_owed;		/* the ARM's cycles */
		uint32_t starts;		/* the ARM's releases from reset */
		uint32_t key_ons;		/* the channels started */
		uint32_t main_irqs;		/* the SH-4's interrupts */
		Voice	 voice[64];
		/* The DSP's registers */
		struct Dsp {
			int32_t	 temp[128];	/* 24 bits */
			int32_t	 mems[32];	/* 24 bits */
			int32_t	 mixs[16];	/* 20 bits: the channels' sends */
			int32_t	 efreg[16];	/* 16 bits: the outputs */
			int32_t	 acc, frc_reg, y_reg, adrs_reg, memval;
			uint32_t dec;
			uint32_t steps;		/* to the program's last non-zero */
		} dsp;
	} st;
};

} /* namespace hikaru */

#endif /* __HL_HIKARU_AICA_H__ */
